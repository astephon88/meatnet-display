// MeatNet field unit (ESP-IDF). See ../docs/field-unit-idf-plan.md.
//
// Joins WiFi with the settings saved in NVS, scans with NimBLE, holds the
// links the app unit asks for, and streams everything to the app unit over
// the split protocol. Settings are changed on the unit's web page; with no
// WiFi set up (or none reachable) it also opens a setup network for that.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "cJSON.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "captive.h"
#include "links.h"
#include "proto.h"
#include "radio.h"
#include "screen.h"
#include "settings.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "field_source.h"
#include "linktable.h"
#include "node.h"
#include "peripheral.h"
#include "split.h"
#include "web.h"

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

static std::string g_role;          // "field", "app" or "standalone" (fixed until a reboot)
static std::string g_serial;
static core::Settings g_cfg;            // the node's settings (app and standalone roles)

static void ble_start()
{
    // An app unit's devices are the field unit's: it never scans.
    if (g_role == "app") radio::Radio::get().set_scan_enabled(false);
    ESP_ERROR_CHECK(nimble_port_init());
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    // Phones connect to app and standalone units (GATT before the host starts).
    if (g_role != "field") periph::Peripheral::get().init(g_serial, g_cfg.dis_model, g_cfg.dis_firmware);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

// -- radio task: links + scan pacing (10 Hz) ----------------------------------------

static constexpr int kLinkedScanDuty = 20;     // % while any link is up
static constexpr int kBurstScanDuty = 5;       // % while a link streams (log reads)
static constexpr uint32_t kBurstRate = 20;     // notifications/s = "streaming"
static constexpr int64_t kBurstHoldMs = 5000;

static void radio_task(void *)
{
    radio::Radio &r = radio::Radio::get();
    radio::Links &links = radio::Links::get();
    int64_t rate_at = radio::Radio::now_ms(), burst_until = 0;
    uint32_t rate_count = links.total_rx();
    while (true) {
        links.tick();
        int64_t now = radio::Radio::now_ms();
        if (now - rate_at >= 1000) {
            uint32_t total = links.total_rx();
            if ((total - rate_count) * 1000 / (now - rate_at) > kBurstRate)
                burst_until = now + kBurstHoldMs;
            rate_at = now;
            rate_count = total;
        }
        int duty = now < burst_until ? kBurstScanDuty
                 : links.any_connected() ? kLinkedScanDuty : 50;
        r.set_duty(duty);
        if (!links.connecting()) r.ensure_scan();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// -- WiFi --------------------------------------------------------------------------

static constexpr int64_t kSetupApAfterMs = 30000;   // no IP this long: open the setup network
// While the setup network is up, WiFi is retried this rarely: every attempt
// scans all channels, and the setup network (same radio) can't be joined
// meanwhile. No retries at all while a phone is on the setup network.
static constexpr int64_t kSetupRetryMs = 60000;

static portMUX_TYPE g_net_mux = portMUX_INITIALIZER_UNLOCKED;
static char g_ip[16] = "";
static char g_setup_ap[33] = "";       // setup network's SSID while it is up
static esp_timer_handle_t g_ap_timer;
static esp_timer_handle_t g_retry_timer;
static std::atomic<int> g_ap_clients{0};
static esp_netif_ip_info_t g_ap_ip;    // the setup network's address (192.168.4.1)

static void set_net(char *dst, size_t n, const char *value)
{
    portENTER_CRITICAL(&g_net_mux);
    strlcpy(dst, value, n);
    portEXIT_CRITICAL(&g_net_mux);
}

static std::string get_net(const char *src)
{
    portENTER_CRITICAL(&g_net_mux);
    std::string v(src);
    portEXIT_CRITICAL(&g_net_mux);
    return v;
}

// Setup network "MeatNet-<serial>" next to the station, so the web page can
// be reached at 192.168.4.1 to enter (or fix) the WiFi settings. Open unless
// a setup password is saved; its DNS sends phones to the page (captive
// portal).
static void setup_ap_start(void * = nullptr)
{
    if (!get_net(g_setup_ap).empty()) return;
    std::string ssid = "MeatNet-" + g_serial;
    const std::string &pw = settings::get().ap_password;
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    strlcpy((char *)ap.ap.ssid, ssid.c_str(), sizeof(ap.ap.ssid));
    ap.ap.ssid_len = ssid.size();
    strlcpy((char *)ap.ap.password, pw.c_str(), sizeof(ap.ap.password));
    ap.ap.authmode = pw.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    // The station stays on (idle without a network) so the page can scan.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    set_net(g_setup_ap, sizeof(g_setup_ap), ssid.c_str());
    captive::start(g_ap_ip.ip.addr);
    esp_wifi_disconnect();          // stop the current attempt; retried slowly from now on
    ESP_LOGW(TAG, "setup network '%s' up (%s): settings at http://" IPSTR "/", ssid.c_str(),
             pw.empty() ? "open" : "WPA2", IP2STR(&g_ap_ip.ip));
}

static void setup_ap_stop()
{
    if (get_net(g_setup_ap).empty()) return;
    esp_timer_stop(g_retry_timer);
    captive::stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    set_net(g_setup_ap, sizeof(g_setup_ap), "");
    ESP_LOGI(TAG, "setup network closed");
}

static void sta_retry(void *)
{
    if (g_ap_clients > 0)
        esp_timer_start_once(g_retry_timer, kSetupRetryMs * 1000);   // a phone is setting us up
    else
        esp_wifi_connect();
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!settings::get().wifi_ssid.empty()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_events, kWifiUp);
        set_net(g_ip, sizeof(g_ip), "");
        if (get_net(g_setup_ap).empty()) {
            ESP_LOGW(TAG, "WiFi disconnected, retrying");
            esp_wifi_connect();
        } else if (!esp_timer_is_active(g_retry_timer)) {
            ESP_LOGW(TAG, "WiFi not reachable; retrying in %llds (setup network up)",
                     (long long)(kSetupRetryMs / 1000));
            esp_timer_start_once(g_retry_timer, kSetupRetryMs * 1000);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        g_ap_clients++;
        ESP_LOGI(TAG, "a device joined the setup network (%d on it)", g_ap_clients.load());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (g_ap_clients > 0) g_ap_clients--;
        ESP_LOGI(TAG, "a device left the setup network (%d on it)", g_ap_clients.load());
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        set_net(g_ip, sizeof(g_ip), ip);
        ESP_LOGI(TAG, "WiFi up, IP %s", ip);
        esp_timer_stop(g_ap_timer);
        setup_ap_stop();
        xEventGroupSetBits(g_events, kWifiUp);
    }
}

static void wifi_start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    // The setup network's DHCP hands out the unit itself as DNS server, and
    // the page as captive-portal address (RFC 8910) for phones that ask.
    static char portal_uri[32];
    esp_netif_get_ip_info(ap, &g_ap_ip);
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&g_ap_ip.ip));
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = g_ap_ip.ip;
    uint8_t offer_dns = 1;
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                           sizeof(offer_dns));
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portal_uri,
                           strlen(portal_uri));
    esp_netif_dhcps_start(ap);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    esp_timer_create_args_t ta = {};
    ta.callback = setup_ap_start;
    ta.name = "setup_ap";
    ESP_ERROR_CHECK(esp_timer_create(&ta, &g_ap_timer));
    ta.callback = sta_retry;
    ta.name = "sta_retry";
    ESP_ERROR_CHECK(esp_timer_create(&ta, &g_retry_timer));

    // Builds that set WiFi in Kconfig left it in the WiFi driver's own NVS
    // storage: take it over once, then clear that copy so the credentials
    // live in one place only.
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
        settings::adopt_wifi((const char *)wc.sta.ssid, (const char *)wc.sta.password);
        esp_wifi_restore();
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const settings::Settings &s = settings::get();
    if (s.wifi_ssid.empty()) {
        ESP_LOGW(TAG, "no WiFi network set");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        setup_ap_start();
    } else {
        memset(&wc, 0, sizeof(wc));
        strlcpy((char *)wc.sta.ssid, s.wifi_ssid.c_str(), sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s.wifi_password.c_str(), sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_timer_start_once(g_ap_timer, kSetupApAfterMs * 1000);
        ESP_LOGI(TAG, "joining WiFi '%s'", s.wifi_ssid.c_str());
    }
    // Who gets the shared radio when WiFi and Bluetooth both want it. The
    // API is deprecated in this IDF and may be a no-op on some chips: log
    // the result instead of assuming it helps.
    esp_coex_prefer_t pref = s.coex == "wifi" ? ESP_COEX_PREFER_WIFI
                           : s.coex == "balance" ? ESP_COEX_PREFER_BALANCE : ESP_COEX_PREFER_BT;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    esp_err_t cx = esp_coex_preference_set(pref);
#pragma GCC diagnostic pop
    ESP_LOGI(TAG, "coexistence preference %s: %s", s.coex.c_str(), esp_err_to_name(cx));
}

// -- main ---------------------------------------------------------------------------

// -- app and standalone roles: the MeatNet node ----------------------------------------

static core::LinkTable g_link_table;
static esp::EspTransport *g_local = nullptr;
static split::RemoteRadio *g_remote = nullptr;
static split::SplitServer *g_server = nullptr;

// What the web page and the screen show, published by the node task (they run
// on other tasks and mustn't touch the Node's state).
struct NodeStatus {
    int phones = 0;
    size_t probes = 0, devices = 0;
    bool field_connected = false;
    std::string field_peer;
    int field_rtt_ms = -1;
};
static std::mutex g_status_mu;
static NodeStatus g_status;

static NodeStatus node_status()
{
    std::lock_guard<std::mutex> g(g_status_mu);
    return g_status;
}

static void node_log(const std::string &m) { ESP_LOGI("node", "%s", m.c_str()); }

static void node_task(void *)
{
    protocol::set_random_source([] { return static_cast<uint32_t>(esp_random()); });
    bool standalone = g_role == "standalone";
    g_local = new esp::EspTransport(standalone);
    core::Transport *t = g_local;
    if (!standalone) t = g_remote = new split::RemoteRadio(g_local, node_log);
    // Standalone: phones and links share this radio's connections.
    g_local->max_conn = std::min(g_cfg.max_conn, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
    periph::Peripheral::get().set_limits(standalone ? g_local->max_conn : CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
                                         standalone ? [] { return radio::Links::get().active(); }
                                                    : static_cast<size_t (*)()>(nullptr));
    core::Node node(*t, g_cfg, g_serial, node_log, &g_link_table);
    ESP_LOGI(TAG, "MeatNet node %s running (%s)", g_serial.c_str(), g_role.c_str());
    for (int n = 0;; n++) {
        node.service();
        periph::Peripheral::get().service();
        if (n % 5 == 0) t->rotate_advertisement();
        if (n % 10 == 0) {
            node.tick();
            NodeStatus st;
            st.phones = static_cast<int>(t->inbound.size());
            st.probes = node.probes.size();
            st.devices = node.devices.size();
            if (g_remote) {
                st.field_connected = g_remote->connected;
                st.field_peer = g_remote->peer_serial;
                st.field_rtt_ms = g_remote->rtt_ms;
            }
            std::lock_guard<std::mutex> g(g_status_mu);
            g_status = st;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// App unit: listen for the field unit (once WiFi is up).
static void server_start()
{
    const settings::Settings &s = settings::get();
    if (s.split_key.empty())
        ESP_LOGW(TAG, "no shared key set: field units can't connect (set it on the web page)");
    split::ServerConfig cfg;
    cfg.key = s.split_key;
    cfg.serial = g_serial;
    cfg.mac = radio::Radio::get().address();
    cfg.port = s.split_port;
    g_server = new split::SplitServer(*g_remote, cfg, [](const std::string &m) {
        ESP_LOGI("split", "%s", m.compare(0, 8, "[split] ") == 0 ? m.c_str() + 8 : m.c_str());
    });
    xTaskCreate([](void *sv) { static_cast<split::SplitServer *>(sv)->run(); }, "split", 8192, g_server,
                5, nullptr);
    ESP_LOGI(TAG, "listening for the field unit on port %d", cfg.port);
}

// /api/state for the web page.
static split::SplitClient *g_client = nullptr;

static cJSON *state_json()
{
    radio::Radio &r = radio::Radio::get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "serial", g_serial.c_str());
    cJSON_AddStringToObject(o, "role", g_role.c_str());
    if (g_role != "field") {
        NodeStatus st = node_status();
        cJSON_AddNumberToObject(o, "phones", st.phones);
        cJSON_AddNumberToObject(o, "probes", st.probes);
        if (g_role == "app") {
            cJSON *f = cJSON_AddObjectToObject(o, "field_unit");
            cJSON_AddBoolToObject(f, "connected", st.field_connected);
            if (st.field_connected) {
                cJSON_AddStringToObject(f, "peer", st.field_peer.c_str());
                cJSON_AddNumberToObject(f, "rtt_ms", st.field_rtt_ms);
            }
        }
    }
    cJSON_AddStringToObject(o, "ip", get_net(g_ip).c_str());
    std::string ap = get_net(g_setup_ap);
    if (!ap.empty()) cJSON_AddStringToObject(o, "setup_ap", ap.c_str());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    if (g_role == "field") {
        cJSON *a = cJSON_AddObjectToObject(o, "app_unit");
        bool up = g_client && g_client->connected();
        cJSON_AddBoolToObject(a, "connected", up);
        if (up) {
            cJSON_AddStringToObject(a, "peer", g_client->peer().c_str());
            cJSON_AddNumberToObject(a, "rtt_ms", g_client->rtt_ms());
        }
    }
    size_t ready = 0, total = 0;
    for (auto &l : radio::Links::get().collect_states()) {
        total++;
        if (l == "ready") ready++;
    }
    cJSON_AddNumberToObject(o, "links", total);
    cJSON_AddNumberToObject(o, "links_ready", ready);
    cJSON *h = cJSON_AddObjectToObject(o, "heard");
    r.lock();
    cJSON_AddNumberToObject(h, "probes", r.direct.size());
    cJSON_AddNumberToObject(h, "repeated", r.repeated.size());
    cJSON_AddNumberToObject(h, "nodes", r.nodes.size());
    cJSON_AddNumberToObject(h, "meaters", r.meaters.size());
    r.unlock();
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    return o;
}

// A link's name for the screen: node serial, probe serial, or MEATER.
static std::string link_name(const radio::LinkBrief &l)
{
    if (l.kind == "node") return l.key;                         // 10 ASCII characters
    if (l.kind == "probe" && l.key.size() == 4) {               // little-endian serial
        char buf[9];
        snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", (uint8_t)l.key[3], (uint8_t)l.key[2],
                 (uint8_t)l.key[1], (uint8_t)l.key[0]);
        return buf;
    }
    return "MEATER";
}

static screen::Content screen_content()
{
    screen::Content c;
    if (g_role != "field") {
        NodeStatus st = node_status();
        std::string ap = get_net(g_setup_ap), ip = get_net(g_ip);
        c.title = g_role == "app" ? "App unit" : "Display";
        if (!ap.empty()) {
            c.lines = {"Join WiFi", ap};
        } else {
            if (g_role == "app")
                c.lines.push_back(st.field_connected ? "field " + st.field_peer : "field unit offline");
            c.lines.push_back(ip.empty() ? "WiFi connecting" : ip);
        }
        c.lines.push_back("phones " + std::to_string(st.phones));
        c.lines.push_back(std::to_string(st.probes) + (st.probes == 1 ? " probe, " : " probes, ") +
                          std::to_string(st.devices) + (st.devices == 1 ? " device" : " devices"));
        return c;
    }
    c.title = "Field unit";
    std::string ap = get_net(g_setup_ap), ip = get_net(g_ip);
    if (!ap.empty()) {
        c.lines.push_back("Join WiFi");
        c.lines.push_back(ap);
        char url[32];
        snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&g_ap_ip.ip));
        c.lines.push_back(url);
    } else {
        bool up = g_client && g_client->connected();
        c.lines.push_back(up ? "app " + g_client->peer() : "app unit offline");
        c.lines.push_back(ip.empty() ? "WiFi connecting" : ip);
    }
    auto links = radio::Links::get().collect_brief();
    radio::Radio &r = radio::Radio::get();
    r.lock();
    size_t probes = r.direct.size(), nodes = r.nodes.size(), meaters = r.meaters.size();
    r.unlock();
    std::string heard = "heard " + std::to_string(probes) + (probes == 1 ? " probe, " : " probes, ") +
                        std::to_string(nodes) + (nodes == 1 ? " node" : " nodes");
    c.lines.push_back(heard);
    if (meaters) c.lines.push_back("heard " + std::to_string(meaters) + (meaters == 1 ? " MEATER" : " MEATERs"));
    for (auto &l : links) {
        if (!ap.empty()) break;               // setup mode: no app unit, so no links
        const char *st = l.state == "ready" ? "up"
                       : l.state == "connecting" || l.state == "discovering" ? "conn" : "down";
        c.lines.push_back(link_name(l) + " " + st);
    }
    return c;
}

static void split_start()
{
    const settings::Settings &s = settings::get();
    if (s.split_key.empty())
        ESP_LOGW(TAG, "no shared key set: the app unit will refuse us (set it on the web page)");
    split::ClientConfig cfg;
    cfg.peer = s.split_peer;
    cfg.key = s.split_key;
    cfg.port = s.split_port;
    cfg.serial = g_serial;
    cfg.max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    cfg.mac = radio::Radio::get().address();
    cfg.impl = "idf";
    static radio::FieldSource source;
    static split::FieldRadio fradio(source);
    // Messages come prefixed "[split] "; ESP-IDF's tag says that already.
    g_client = new split::SplitClient(fradio, cfg, [](const std::string &m) {
        ESP_LOGI("split", "%s", m.compare(0, 8, "[split] ") == 0 ? m.c_str() + 8 : m.c_str());
    });
    xTaskCreate([](void *c) { static_cast<split::SplitClient *>(c)->run(); }, "split", 8192, g_client,
                5, nullptr);
    ESP_LOGI(TAG, "app unit: %s, port %d",
             cfg.peer.empty() ? "find by beacon" : cfg.peer.c_str(), cfg.port);
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings::load();
    g_events = xEventGroupCreate();

    // Serial: the last 10 hex digits of the WiFi MAC, as the MicroPython unit.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[11];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X", mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_serial = serial;
    g_role = settings::get().role;
    core::merge_settings(g_cfg, settings::get().node_json);
    ESP_LOGI(TAG, "MeatNet %s unit (ESP-IDF), serial %s", g_role.c_str(), serial);

    wifi_start();
    web::start(state_json, g_ap_ip.ip.addr);
    screen::start(screen_content);
    ble_start();
    xEventGroupWaitBits(g_events, kBleReady, pdFALSE, pdTRUE, portMAX_DELAY);
    radio::Links::get().max_links = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    if (g_role != "app") xTaskCreate(radio_task, "radio", 6144, nullptr, 6, nullptr);
    if (g_role != "field") xTaskCreate(node_task, "node", 12288, nullptr, 5, nullptr);

    radio::Radio &r = radio::Radio::get();
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool wifi = xEventGroupGetBits(g_events) & kWifiUp;
        if (g_role == "field" && !g_client && wifi) split_start();
        if (g_role == "app" && !g_server && g_remote && wifi) server_start();
        if (tick % 30) continue;
        r.lock();
        size_t d = r.direct.size(), rep = r.repeated.size(), n = r.nodes.size(), m = r.meaters.size();
        r.unlock();
        size_t ready = 0, total = 0;
        for (auto &l : radio::Links::get().collect_states()) {
            total++;
            if (l == "ready") ready++;
        }
        ESP_LOGI(TAG, "heard: %u probes, %u repeated, %u nodes, %u MEATERs (%u adverts); "
                      "links %u/%u ready; app unit %s%s; free heap %u",
                 (unsigned)d, (unsigned)rep, (unsigned)n, (unsigned)m, (unsigned)r.adverts(),
                 (unsigned)ready, (unsigned)total,
                 g_client && g_client->connected() ? "connected " : "offline",
                 g_client && g_client->connected() ? g_client->peer().c_str() : "",
                 (unsigned)esp_get_free_heap_size());
    }
}
