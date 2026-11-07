// MeatNet field unit, ESP-IDF skeleton (milestone 1).
//
// Boots NVS, joins WiFi with bootstrap credentials from menuconfig, starts
// the NimBLE host as central/observer and runs a passive scan that reports
// the Combustion and MEATER devices it hears every 10 s. Later milestones
// replace this with the radio/split/web components described in
// ../docs/field-unit-idf-plan.md.

#include <cstring>
#include <map>
#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "field";

static constexpr uint16_t kCombustionVendor = 0x09C7;

// -- scan bookkeeping (written from the NimBLE host task) ---------------------

struct Heard {
    uint8_t ptype;
    int8_t rssi;
    uint32_t seen;
};

static SemaphoreHandle_t g_lock;
static std::map<std::string, Heard> g_heard;   // device id -> last sighting
static uint32_t g_adv_total;

static std::string hex(const uint8_t *p, size_t n, bool reverse = false)
{
    static const char *d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = reverse ? p[n - 1 - i] : p[i];
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

// Name of a Combustion device from its manufacturer data body (after the
// company ID): probes by their 4-byte serial, nodes by their 10-byte serial.
static std::string combustion_id(const uint8_t *body, size_t n, uint8_t *ptype)
{
    if (n < 1) return "";
    *ptype = body[0];
    if ((body[0] == 1 || body[0] == 2) && n >= 5)
        return (body[0] == 1 ? "probe " : "repeated ") + hex(body + 1, 4, true);
    if (body[0] >= 3 && n >= 11) {
        std::string serial(reinterpret_cast<const char *>(body + 1), 10);
        serial.erase(serial.find_last_not_of('\0') + 1);
        static const char *names[] = {"?", "?", "?", "gauge ", "display ",
                                      "booster ", "engine "};
        return std::string(body[0] <= 6 ? names[body[0]] : "node ") + serial;
    }
    return "";
}

static bool is_meater(const struct ble_hs_adv_fields &f)
{
    if (f.name && f.name_len >= 6 && strncasecmp((const char *)f.name, "MEATER", 6) == 0)
        return true;
    return false;
}

static int gap_event(struct ble_gap_event *event, void *)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0)
        return 0;
    std::string id;
    uint8_t ptype = 0;
    if (f.mfg_data && f.mfg_data_len >= 3 &&
        (f.mfg_data[0] | f.mfg_data[1] << 8) == kCombustionVendor) {
        id = combustion_id(f.mfg_data + 2, f.mfg_data_len - 2, &ptype);
    } else if (is_meater(f)) {
        id = "meater " + hex(event->disc.addr.val, 6, true);
        ptype = 0xFF;
    }
    if (id.empty()) return 0;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_heard[id] = Heard{ptype, event->disc.rssi, (uint32_t)(esp_log_timestamp() / 1000)};
    g_adv_total++;
    xSemaphoreGive(g_lock);
    return 0;
}

// -- NimBLE host --------------------------------------------------------------

static void start_scan()
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE identity address");
        return;
    }
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive = 1;
    p.itvl = 96;          // 60 ms (0.625 ms units)
    p.window = 48;        // 30 ms: 50% duty, as the MicroPython unit
    p.filter_duplicates = 0;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, nullptr);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else ESP_LOGI(TAG, "scanning (passive, 50%% duty)");
}

static void on_sync()
{
    ble_hs_util_ensure_addr(0);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, nullptr);
    ESP_LOGI(TAG, "NimBLE ready, address %s", hex(addr, 6, true).c_str());
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason %d", reason);
}

static void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_start()
{
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

// -- WiFi (bootstrap credentials) -----------------------------------------------

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "WiFi up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static void wifi_start()
{
    if (strlen(CONFIG_MEATNET_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no bootstrap WiFi SSID set (idf.py menuconfig)");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, CONFIG_MEATNET_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_MEATNET_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining WiFi '%s'", CONFIG_MEATNET_WIFI_SSID);
}

// -- main -------------------------------------------------------------------------

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    g_lock = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "MeatNet field unit (ESP-IDF skeleton), serial %s",
             hex(mac + 1, 5).c_str());

    wifi_start();
    ble_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t now = esp_log_timestamp() / 1000;
        xSemaphoreTake(g_lock, portMAX_DELAY);
        ESP_LOGI(TAG, "heard %u adverts; devices in the last 30 s:", (unsigned)g_adv_total);
        for (auto it = g_heard.begin(); it != g_heard.end();) {
            if (now - it->second.seen > 30) {
                it = g_heard.erase(it);
                continue;
            }
            ESP_LOGI(TAG, "  %-24s %4d dBm  %lus ago", it->first.c_str(),
                     it->second.rssi, (unsigned long)(now - it->second.seen));
            ++it;
        }
        xSemaphoreGive(g_lock);
        ESP_LOGI(TAG, "free heap %u", (unsigned)esp_get_free_heap_size());
    }
}
