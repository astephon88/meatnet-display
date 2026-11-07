// MeatNet field unit (ESP-IDF). See ../docs/field-unit-idf-plan.md.
//
// Milestone 2: joins WiFi with bootstrap credentials, scans with NimBLE, and
// streams the scan tables to the app unit over the split protocol. Links
// (milestone 3) and the settings page (milestone 5) come later.

#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "proto.h"
#include "radio.h"
#include "split_client.h"

static const char *TAG = "field";

static EventGroupHandle_t g_events;
static constexpr EventBits_t kWifiUp = BIT0;
static constexpr EventBits_t kBleReady = BIT1;

// -- NimBLE host ----------------------------------------------------------------

static void on_sync()
{
    ble_hs_util_ensure_addr(0);
    radio::Radio::get().on_ready();
    ESP_LOGI(TAG, "NimBLE ready, address %s",
             proto::hex(radio::Radio::get().address()).c_str());
    xEventGroupSetBits(g_events, kBleReady);
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

// -- WiFi (bootstrap credentials) -------------------------------------------------

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_events, kWifiUp);
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "WiFi up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(g_events, kWifiUp);
    }
}

static bool wifi_start()
{
    if (strlen(CONFIG_MEATNET_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no bootstrap WiFi SSID set (idf.py menuconfig)");
        return false;
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
    return true;
}

// -- main ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    g_events = xEventGroupCreate();

    // Serial: the last 10 hex digits of the WiFi MAC, as the MicroPython unit.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[11];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X", mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "MeatNet field unit (ESP-IDF), serial %s", serial);

    bool wifi = wifi_start();
    ble_start();
    xEventGroupWaitBits(g_events, kBleReady, pdFALSE, pdTRUE, portMAX_DELAY);

    static split::Client *client = nullptr;
    if (wifi) {
        xEventGroupWaitBits(g_events, kWifiUp, pdFALSE, pdTRUE, portMAX_DELAY);
        split::Config cfg;
        cfg.peer = CONFIG_MEATNET_SPLIT_PEER;
        cfg.key = CONFIG_MEATNET_SPLIT_KEY;
        cfg.port = CONFIG_MEATNET_SPLIT_PORT;
        cfg.serial = serial;
        cfg.max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
        cfg.mac = radio::Radio::get().address();
        client = new split::Client(cfg);
        client->start();
        ESP_LOGI(TAG, "app unit: %s, port %d",
                 cfg.peer.empty() ? "find by beacon" : cfg.peer.c_str(), cfg.port);
    }

    radio::Radio &r = radio::Radio::get();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        r.lock();
        size_t d = r.direct.size(), rep = r.repeated.size(), n = r.nodes.size(), m = r.meaters.size();
        r.unlock();
        ESP_LOGI(TAG, "heard: %u probes, %u repeated, %u nodes, %u MEATERs (%u adverts); "
                      "app unit %s%s; free heap %u",
                 (unsigned)d, (unsigned)rep, (unsigned)n, (unsigned)m, (unsigned)r.adverts(),
                 client && client->connected() ? "connected " : "offline",
                 client && client->connected() ? client->peer().c_str() : "",
                 (unsigned)esp_get_free_heap_size());
    }
}
