#include "screen.h"

#include <cmath>
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
#include "sdkconfig.h"

extern const uint8_t font_med_start[] asm("_binary_med_bin_start");
extern const uint8_t font_small_start[] asm("_binary_small_bin_start");

namespace screen {

static const char *TAG = "screen";
static constexpr int W = 240, H = 240;
static constexpr int kBand = 24;           // rows per SPI transfer (11.5 KB buffer)

// -- fonts (tools/make_font.py: "MNF1", height, -, count u16, then
//    count x (code u16, width u8, -, offset u32), MONO_HLSB bitmaps) -----------

struct Font {
    const uint8_t *data = nullptr;
    int height = 0, count = 0;

    explicit Font(const uint8_t *d) : data(d)
    {
        if (memcmp(d, "MNF1", 4) == 0) {
            height = d[4];
            count = d[6] | d[7] << 8;
        }
    }
    // Glyph width and bitmap, or width 0 when the font lacks the character.
    int glyph(char ch, const uint8_t **bits) const
    {
        const uint8_t *e = data + 8;
        for (int i = 0; i < count; i++, e += 8) {
            if ((e[0] | e[1] << 8) == (uint8_t)ch) {
                uint32_t off = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
                *bits = data + 8 + 8 * count + off;
                return e[2];
            }
        }
        return 0;
    }
    int width(const std::string &s) const
    {
        int w = 0;
        const uint8_t *b;
        for (char c : s) w += glyph(c, &b);
        return w;
    }
};

static Font *g_med, *g_small;

// -- layout ---------------------------------------------------------------------------

struct Item {
    const Font *font;
    std::string text;
    int x, y;
    uint16_t color;      // RGB565, byte-swapped for the panel (big-endian on the wire)
};

static constexpr uint16_t rgb(int r, int g, int b)
{
    uint16_t v = (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
    return v << 8 | v >> 8;
}

static constexpr uint16_t kWhite = rgb(255, 255, 255), kDim = rgb(160, 160, 160),
                          kAccent = rgb(120, 170, 255);

// Usable width of the round panel across rows y..y+h (as screen.py's chord).
static int chord(int y, int h)
{
    int d = std::max(std::abs(y - 120), std::abs(y + h - 120));
    if (d >= 120) return 0;
    return (int)(2 * std::sqrt(120.0 * 120 - d * d)) - 12;
}

static Item centred(const Font &f, std::string text, int y, uint16_t color)
{
    int lim = chord(y, f.height);
    while (!text.empty() && f.width(text) > lim) text.pop_back();
    return {&f, text, 120 - f.width(text) / 2, y, color};
}

// Title, then the lines top to bottom while they fit the circle; when some
// are left over, the last one shown becomes "+N more".
static std::vector<Item> layout(const Content &c)
{
    std::vector<Item> items;
    items.push_back(centred(*g_med, c.title, 34, kAccent));
    int y = 72;
    size_t i = 0;
    for (; i < c.lines.size() && y + g_small->height <= 216; i++) {
        const Font &f = i == 0 && g_med->width(c.lines[i]) <= chord(y, g_med->height) ? *g_med
                                                                                      : *g_small;
        items.push_back(centred(f, c.lines[i], y, i == 0 ? kWhite : kDim));
        y += f.height + (i == 0 ? 10 : 6);
    }
    if (i < c.lines.size() && items.size() > 2) {
        Item &last = items.back();
        last = centred(*last.font, "+" + std::to_string(c.lines.size() - i + 1) + " more", last.y,
                       kDim);
    }
    return items;
}

// Draws the parts of `items` that fall in rows y0..y0+kBand into `buf`.
static void raster(const std::vector<Item> &items, int y0, uint16_t *buf)
{
    memset(buf, 0, W * kBand * 2);
    for (const auto &it : items) {
        int h = it.font->height;
        if (it.y >= y0 + kBand || it.y + h <= y0) continue;
        int x = it.x;
        for (char ch : it.text) {
            const uint8_t *bits;
            int gw = it.font->glyph(ch, &bits);
            int stride = (gw + 7) / 8;
            for (int row = std::max(0, y0 - it.y); row < h && it.y + row < y0 + kBand; row++) {
                uint16_t *line = buf + (it.y + row - y0) * W;
                const uint8_t *src = bits + row * stride;
                for (int col = 0; col < gw; col++)
                    if (x + col >= 0 && x + col < W && (src[col >> 3] & (0x80 >> (col & 7))))
                        line[x + col] = it.color;
            }
            x += gw;
        }
    }
}

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
    {0x36, "\x08", 1, 0},                  // MADCTL: rotation 0, BGR panel
};

static bool on_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_done, &woken);
    return woken == pdTRUE;
}

static bool panel_init()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_MEATNET_SCREEN_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = CONFIG_MEATNET_SCREEN_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = W * kBand * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = CONFIG_MEATNET_SCREEN_CS;
    io.dc_gpio_num = CONFIG_MEATNET_SCREEN_DC;
    io.spi_mode = 0;
    io.pclk_hz = 40 * 1000 * 1000;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = on_done;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &g_io) != ESP_OK)
        return false;
#if CONFIG_MEATNET_SCREEN_RST >= 0
    gpio_set_direction((gpio_num_t)CONFIG_MEATNET_SCREEN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_MEATNET_SCREEN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)CONFIG_MEATNET_SCREEN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)CONFIG_MEATNET_SCREEN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif
    for (const auto &c : kInit) {
        esp_lcd_panel_io_tx_param(g_io, c.cmd, c.len ? c.data : nullptr, c.len);
        if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
    }
    return true;
}

static void backlight(int pct)
{
#if CONFIG_MEATNET_SCREEN_BL >= 0
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 5000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {};
    ch.gpio_num = CONFIG_MEATNET_SCREEN_BL;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 1023 * pct / 100;
    ledc_channel_config(&ch);
#endif
}

static void draw(const std::vector<Item> &items, uint16_t *buf)
{
    for (int y0 = 0; y0 < H; y0 += kBand) {
        raster(items, y0, buf);
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

static std::function<Content()> g_content;

static void task(void *)
{
    uint16_t *buf = (uint16_t *)heap_caps_malloc(W * kBand * 2, MALLOC_CAP_DMA);
    if (!buf || !panel_init()) {
        ESP_LOGE(TAG, "screen init failed");
        vTaskDelete(nullptr);
        return;
    }
    Content shown;
    bool first = true;
    while (true) {
        Content c = g_content();
        if (first || !(c == shown)) {
            draw(layout(c), buf);
            if (first) backlight(CONFIG_MEATNET_SCREEN_BRIGHTNESS);   // after the first frame
            shown = c;
            first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void start(std::function<Content()> content)
{
#if CONFIG_MEATNET_SCREEN
    static Font med(font_med_start), small(font_small_start);
    if (!med.height || !small.height) {
        ESP_LOGE(TAG, "bad font data");
        return;
    }
    g_med = &med;
    g_small = &small;
    g_done = xSemaphoreCreateBinary();
    g_content = content;
    xTaskCreate(task, "screen", 4096, nullptr, 3, nullptr);
#endif
}

}  // namespace screen
