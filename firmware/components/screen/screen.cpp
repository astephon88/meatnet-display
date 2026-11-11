#include "screen.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern const uint8_t font_big_start[] asm("_binary_big_bin_start");
extern const uint8_t font_mid_start[] asm("_binary_mid_bin_start");
extern const uint8_t font_med_start[] asm("_binary_med_bin_start");
extern const uint8_t font_small_start[] asm("_binary_small_bin_start");

namespace screen {

namespace sd = screen_draw;

static const char *TAG = "screen";
static constexpr int W = sd::W, H = sd::H;
static constexpr int kBand = 24;           // rows per SPI transfer (11.5 KB buffer)

const sd::Fonts &fonts()
{
    static sd::Font big(font_big_start), mid(font_mid_start), med(font_med_start), small(font_small_start);
    static sd::Fonts f{&big, &mid, &med, &small};
    return f;
}

static core::Pins g_pins;

// -- GC9A01A over SPI (init sequence from display.py / TFT_eSPI) ----------------------

static esp_lcd_panel_io_handle_t g_io;
static SemaphoreHandle_t g_done;

struct Cmd {
    uint8_t cmd;
    const char *data;
    uint8_t len;
    uint8_t delay_ms;
};

static const Cmd kInit[] = {
    {0xEF, "", 0, 0}, {0xEB, "\x14", 1, 0}, {0xFE, "", 0, 0}, {0xEF, "", 0, 0},
    {0xEB, "\x14", 1, 0}, {0x84, "\x40", 1, 0}, {0x85, "\xFF", 1, 0}, {0x86, "\xFF", 1, 0},
    {0x87, "\xFF", 1, 0}, {0x88, "\x0A", 1, 0}, {0x89, "\x21", 1, 0}, {0x8A, "\x00", 1, 0},
    {0x8B, "\x80", 1, 0}, {0x8C, "\x01", 1, 0}, {0x8D, "\x01", 1, 0}, {0x8E, "\xFF", 1, 0},
    {0x8F, "\xFF", 1, 0}, {0xB6, "\x00\x20", 2, 0}, {0x3A, "\x05", 1, 0},
    {0x90, "\x08\x08\x08\x08", 4, 0}, {0xBD, "\x06", 1, 0}, {0xBC, "\x00", 1, 0},
    {0xFF, "\x60\x01\x04", 3, 0}, {0xC3, "\x13", 1, 0}, {0xC4, "\x13", 1, 0},
    {0xC9, "\x22", 1, 0}, {0xBE, "\x11", 1, 0}, {0xE1, "\x10\x0E", 2, 0},
    {0xDF, "\x21\x0C\x02", 3, 0},
    {0xF0, "\x45\x09\x08\x08\x26\x2A", 6, 0}, {0xF1, "\x43\x70\x72\x36\x37\x6F", 6, 0},
    {0xF2, "\x45\x09\x08\x08\x26\x2A", 6, 0}, {0xF3, "\x43\x70\x72\x36\x37\x6F", 6, 0},
    {0xED, "\x1B\x0B", 2, 0}, {0xAE, "\x77", 1, 0}, {0xCD, "\x63", 1, 0},
    {0x70, "\x07\x07\x04\x0E\x0F\x09\x07\x08\x03", 9, 0}, {0xE8, "\x34", 1, 0},
    {0x62, "\x18\x0D\x71\xED\x70\x70\x18\x0F\x71\xEF\x70\x70", 12, 0},
    {0x63, "\x18\x11\x71\xF1\x70\x70\x18\x13\x71\xF3\x70\x70", 12, 0},
    {0x64, "\x28\x29\xF1\x01\xF1\x00\x07", 7, 0},
    {0x66, "\x3C\x00\xCD\x67\x45\x45\x10\x00\x00\x00", 10, 0},
    {0x67, "\x00\x3C\x00\x00\x00\x01\x54\x10\x32\x98", 10, 0},
    {0x74, "\x10\x85\x80\x00\x00\x4E\x00", 7, 0}, {0x98, "\x3E\x07", 2, 0},
    {0x35, "", 0, 0}, {0x21, "", 0, 0}, {0x11, "", 0, 120}, {0x29, "", 0, 20},
};

// MADCTL per rotation (BGR panel), from the TFT_eSPI GC9A01 driver.
static const uint8_t kMadctl[4] = {0x08, 0x68, 0xC8, 0xA8};

static bool on_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_done, &woken);
    return woken == pdTRUE;
}

static bool panel_init()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = g_pins.mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = g_pins.sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = W * kBand * 2;
    spi_host_device_t host = g_pins.spi == 3 ? SPI3_HOST : SPI2_HOST;
    if (spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = g_pins.cs;
    io.dc_gpio_num = g_pins.dc;
    io.spi_mode = 0;
    io.pclk_hz = std::min(g_pins.baud, 40 * 1000 * 1000);    // these pins go through the GPIO matrix
    io.trans_queue_depth = 4;
    io.on_color_trans_done = on_done;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io, &g_io) != ESP_OK)
        return false;
    if (g_pins.rst >= 0) {
        gpio_num_t rst = static_cast<gpio_num_t>(g_pins.rst);
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        gpio_set_level(rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    for (const auto &c : kInit) {
        esp_lcd_panel_io_tx_param(g_io, c.cmd, c.len ? c.data : nullptr, c.len);
        if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
    }
    uint8_t madctl = kMadctl[g_pins.rotation & 3];
    esp_lcd_panel_io_tx_param(g_io, 0x36, &madctl, 1);
    return true;
}

static void backlight(int pct)
{
    if (g_pins.bl < 0) return;
    static bool ready = false;
    if (!ready) {
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.timer_num = LEDC_TIMER_0;
        t.freq_hz = 5000;
        t.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&t);
        ledc_channel_config_t ch = {};
        ch.gpio_num = g_pins.bl;
        ch.speed_mode = LEDC_LOW_SPEED_MODE;
        ch.channel = LEDC_CHANNEL_0;
        ch.timer_sel = LEDC_TIMER_0;
        ledc_channel_config(&ch);
        ready = true;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * std::max(0, std::min(100, pct)) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool on)
{
    if (on) {
        esp_lcd_panel_io_tx_param(g_io, 0x28, nullptr, 0);     // display off
        esp_lcd_panel_io_tx_param(g_io, 0x10, nullptr, 0);     // sleep in
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        esp_lcd_panel_io_tx_param(g_io, 0x11, nullptr, 0);     // sleep out
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_io_tx_param(g_io, 0x29, nullptr, 0);     // display on
    }
}

static void draw(const std::vector<sd::Prim> &prims, uint16_t *buf)
{
    for (int y0 = 0; y0 < H; y0 += kBand) {
        sd::raster(prims, y0, kBand, buf);
        uint8_t ca[4] = {0, 0, (W - 1) >> 8, (W - 1) & 0xFF};
        uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y0 + kBand - 1) >> 8),
                         (uint8_t)(y0 + kBand - 1)};
        esp_lcd_panel_io_tx_param(g_io, 0x2A, ca, 4);
        esp_lcd_panel_io_tx_param(g_io, 0x2B, ra, 4);
        esp_lcd_panel_io_tx_color(g_io, 0x2C, buf, W * kBand * 2);
        xSemaphoreTake(g_done, portMAX_DELAY);    // the buffer is reused for the next band
    }
}

// -- task -------------------------------------------------------------------------------

static std::function<sd::View()> g_view;
static std::atomic<int> g_brightness{80};
static std::atomic<bool> g_sleep{false};

void set_brightness(int pct) { g_brightness = pct; }
void set_sleep(bool on) { g_sleep = on; }

static void task(void *)
{
    uint16_t *buf = (uint16_t *)heap_caps_malloc(W * kBand * 2, MALLOC_CAP_DMA);
    if (!buf || !panel_init()) {
        ESP_LOGE(TAG, "screen init failed");
        vTaskDelete(nullptr);
        return;
    }
    sd::View shown;
    bool first = true, asleep = false;
    int lit = -1;
    while (true) {
        if (g_sleep) {
            if (!asleep) {
                asleep = true;
                backlight(0);
                panel_sleep(true);
                lit = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (asleep) {
            asleep = false;
            panel_sleep(false);
            first = true;                 // the panel's RAM may be lost: redraw
        }
        sd::View v = g_view();
        if (first || !(v == shown)) {
            draw(sd::draw(fonts(), v), buf);
            shown = std::move(v);
            first = false;
        }
        if (g_brightness != lit) {        // after the first frame, so no garbage shows
            lit = g_brightness;
            backlight(lit);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void start(const core::Pins &pins, std::function<sd::View()> view)
{
    const sd::Fonts &f = fonts();
    if (!f.big->ok() || !f.mid->ok() || !f.med->ok() || !f.small->ok()) {
        ESP_LOGE(TAG, "bad font data");
        return;
    }
    g_pins = pins;
    g_done = xSemaphoreCreateBinary();
    g_view = std::move(view);
    xTaskCreate(task, "screen", 4096, nullptr, 3, nullptr);
}

}  // namespace screen
