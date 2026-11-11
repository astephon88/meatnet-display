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
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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
#include "esp_transport.h"
#include "field_source.h"
#include "linktable.h"
#include "log_ring.h"
#include "node.h"
#include "peripheral.h"
#include "power.h"
#include "readings.h"
#include "settings.h"
#include "split.h"
#include "status.h"
#include "util.h"
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

// The log, also kept for the web page (/api/log). The WiFi and Bluetooth
// stacks' chatter stays on the serial port only.
static core::LogRing g_log(60, 160);
static vprintf_like_t g_serial_log;

static int serial_out(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = g_serial_log(fmt, args);
    va_end(args);
    return n;
}

// Formats each line once (the caller may be a task with a small stack, such
// as the system event task) and passes the text on to the serial port.
static int log_hook(const char *fmt, va_list args)
{
    char line[256];
    vsnprintf(line, sizeof(line), fmt, args);
    static const char *const quiet[] = {"wifi", "wifi_init", "phy_init", "pp", "net80211", "BLE_INIT",
                                        "NimBLE", "esp_netif_handlers"};
    // Only whole log lines ("I (1234) tag: message"); the WiFi driver also
    // prints fragments of its own.
    const char *tag = strstr(line, ") ");
    const char *colon = tag ? strchr(tag, ':') : nullptr;
    bool skip = !colon || !strchr("EWIDV", line[0]) || line[1] != ' ' || line[2] != '(' ||
                strspn(colon + 1, " \r\n") == strlen(colon + 1);    // nothing after "tag:"
    if (!skip && line[0] == 'I') {
        for (const char *q : quiet) {
            size_t n = strlen(q);
            if (strncmp(tag + 2, q, n) == 0 && tag[2 + n] == ':') {
                skip = true;
                break;
            }
        }
    }
    if (!skip) g_log.push(line);
    return serial_out("%s", line);
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
    std::string pw = settings::get().ap_password;
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

    const core::Settings s = settings::get();
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
// on other tasks and mustn't touch the Node's state): a summary for the
// screen and the log, and the Node's part of /api/state as JSON text.
struct NodeStatus {
    int phones = 0;
    size_t probes = 0, devices = 0, links = 0, links_ready = 0;
    bool field_connected = false;
    std::string field_peer, field_ip;
    int field_rtt_ms = -1, field_max_conn = 0;
    std::string json;
};
static std::mutex g_status_mu;
static NodeStatus g_status;
static std::atomic<bool> g_cfg_changed{false};    // set by the web page

static NodeStatus node_status()
{
    std::lock_guard<std::mutex> g(g_status_mu);
    return g_status;
}

static void node_log(const std::string &m) { ESP_LOGI("node", "%s", m.c_str()); }

// The battery divider's pin in microvolts (ADC1 with curve-fitting
// calibration), or nothing when there is no battery pin.
static std::optional<double> battery_uv()
{
    static adc_oneshot_unit_handle_t unit;
    static adc_cali_handle_t cali;
    static adc_channel_t ch;
    static int state = 0;               // 0 = not set up yet, 1 = ready, -1 = unavailable
    if (state == 0) {
        state = -1;
        adc_unit_t id;
        if (g_cfg.battery_pin < 0) return std::nullopt;
        if (adc_oneshot_io_to_channel(g_cfg.battery_pin, &id, &ch) != ESP_OK || id != ADC_UNIT_1) {
            ESP_LOGW(TAG, "battery pin %d has no ADC1 channel", g_cfg.battery_pin);
            return std::nullopt;
        }
        adc_oneshot_unit_init_cfg_t uc = {};
        uc.unit_id = id;
        adc_oneshot_chan_cfg_t cc = {};
        cc.atten = ADC_ATTEN_DB_12;
        cc.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_cali_curve_fitting_config_t kc = {};
        kc.unit_id = id;
        kc.chan = ch;
        kc.atten = ADC_ATTEN_DB_12;
        kc.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_new_unit(&uc, &unit) != ESP_OK || adc_oneshot_config_channel(unit, ch, &cc) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&kc, &cali) != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC unavailable");
            return std::nullopt;
        }
        state = 1;
    }
    int raw, mv;
    if (state != 1 || adc_oneshot_read(unit, ch, &raw) != ESP_OK || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK)
        return std::nullopt;
    return mv * 1000.0;
}

// The web page's standby button, carried out by the node task.
static std::atomic<int> g_power_req{0};     // 1 = standby, 2 = wake
static std::atomic<uint32_t> g_published{0};

static void publish(core::Node &node, core::Transport &t, const core::PowerManager &pm)
{
    NodeStatus st;
    st.phones = static_cast<int>(t.inbound.size());
    st.probes = node.probes.size();
    st.devices = node.devices.size();
    for (auto &kv : t.links) {
        st.links++;
        if (kv.second->ready()) st.links_ready++;
    }
    if (g_remote) {
        st.field_connected = g_remote->connected;
        st.field_peer = g_remote->peer_serial;
        st.field_ip = g_remote->peer_ip;
        st.field_rtt_ms = g_remote->rtt_ms;
        st.field_max_conn = g_remote->max_conn;
    }
    cJSON *o = core::node_status(node, core::ticks_ms());
    cJSON_AddItemToObject(o, "power", pm.status(core::ticks_ms()));
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    st.json = text ? text : "{}";
    cJSON_free(text);
    {
        std::lock_guard<std::mutex> g(g_status_mu);
        g_status = std::move(st);
    }
    g_published++;
}

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
    core::PowerManager::Hooks hooks;
    hooks.read_uv = battery_uv;
    hooks.apply = [](bool on) {
        screen::set_sleep(on);
        esp_wifi_set_ps(on ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    };
    hooks.persist = settings::set_standby;
    core::PowerManager pm(g_cfg, *t, node, hooks, node_log, core::ticks_ms());
    ESP_LOGI(TAG, "MeatNet node %s running (%s)", g_serial.c_str(), g_role.c_str());
    for (int n = 0;; n++) {
        if (int req = g_power_req.exchange(0)) {
            if (req == 1) pm.enter("manual", core::ticks_ms());
            else pm.exit("manual", core::ticks_ms());
            publish(node, *t, pm);
        }
        // Changes from the web page apply at once, as in web.py (the ones
        // marked "reboot" only matter at the next start).
        if (g_cfg_changed.exchange(false)) {
            core::Settings fresh = settings::get();
            if (fresh.dis_firmware != g_cfg.dis_firmware) t->set_firmware(fresh.dis_firmware);
            g_cfg = fresh;
        }
        node.service();
        periph::Peripheral::get().service();
        if (n % 5 == 0) t->rotate_advertisement();
        if (n % 10 == 0) {
            node.tick();
            pm.tick(core::ticks_ms());
            publish(node, *t, pm);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// App unit: listen for the field unit (once WiFi is up).
static void server_start()
{
    const core::Settings s = settings::get();
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

// A field unit has no Node: its part of /api/state comes from the radio.
static cJSON *field_json()
{
    radio::Radio &r = radio::Radio::get();
    std::map<core::Bytes, core::Observation> direct, nodes, meaters;
    auto copy = [](const radio::Table &from, std::map<core::Bytes, core::Observation> &to) {
        for (auto &kv : from) {
            core::Observation o;
            o.body = kv.second.body;
            o.name = kv.second.name;
            o.ticks = kv.second.ms;
            o.rssi = kv.second.rssi;
            to[kv.first] = o;
        }
    };
    r.lock();
    copy(r.direct, direct);
    copy(r.nodes, nodes);
    copy(r.meaters, meaters);
    r.unlock();
    std::vector<core::LinkSummary> links;
    for (auto &l : radio::Links::get().collect_brief()) {
        core::LinkSummary ls;
        ls.kind = l.kind;
        ls.serial = link_name(l);
        ls.state = l.state;
        ls.count = l.count;
        ls.mtu = l.mtu;
        ls.stats = core::link_stats(l.connects, l.drops, l.params, l.asked);
        links.push_back(ls);
    }
    return core::field_status(direct, nodes, meaters, links, CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
                              radio::Radio::now_ms());
}

// /api/state for the web page: the Node's (or the field radio's) tables plus
// this unit's own state, as web.py builds it.
static cJSON *state_json()
{
    cJSON *o;
    NodeStatus st;
    if (g_role == "field") {
        o = field_json();
    } else {
        st = node_status();
        o = cJSON_Parse(st.json.c_str());
        if (!o) o = cJSON_CreateObject();
    }
    cJSON_AddStringToObject(o, "serial", g_serial.c_str());
    cJSON_DeleteItemFromObject(o, "role");
    cJSON_AddStringToObject(o, "role", g_role.c_str());
    std::string ap = get_net(g_setup_ap), ip = get_net(g_ip);
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "mode", !ip.empty() ? "sta" : !ap.empty() ? "ap" : "connecting");
    char ap_ip[16];
    snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&g_ap_ip.ip));
    cJSON_AddStringToObject(w, "ip", !ip.empty() ? ip.c_str() : !ap.empty() ? ap_ip : "");
    cJSON_AddStringToObject(w, "ssid", settings::get().wifi_ssid.c_str());
    cJSON_AddStringToObject(w, "ap_ssid", ap.c_str());
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "display", "GC9A01A 240x240");
    if (!cJSON_GetObjectItem(o, "power")) cJSON_AddNullToObject(o, "power");
    cJSON *sp = cJSON_AddObjectToObject(o, "split");
    if (g_role == "field") {
        bool up = g_client && g_client->connected();
        cJSON_AddBoolToObject(sp, "connected", up);
        if (up) {
            cJSON_AddStringToObject(sp, "peer", g_client->peer().c_str());
            cJSON_AddStringToObject(sp, "ip", g_client->peer_ip().c_str());
            cJSON_AddNumberToObject(sp, "rtt_ms", g_client->rtt_ms());
        }
    } else if (g_role == "app") {
        cJSON_AddBoolToObject(sp, "connected", st.field_connected);
        if (st.field_connected) {
            cJSON_AddStringToObject(sp, "peer", st.field_peer.c_str());
            cJSON_AddStringToObject(sp, "ip", st.field_ip.c_str());
            cJSON_AddNumberToObject(sp, "rtt_ms", st.field_rtt_ms);
            cJSON_AddNumberToObject(sp, "max_conn", st.field_max_conn);
        }
    } else {
        cJSON_DeleteItemFromObject(o, "split");
        cJSON_AddNullToObject(o, "split");
    }
    return o;
}

// The round screen: readings pages (app and standalone), or the field unit's
// status page. Runs on the screen task.
static screen_draw::View screen_view()
{
    static int page = 0;
    static int64_t page_at = 0;
    core::Settings cfg = settings::get();
    std::string ap = get_net(g_setup_ap), ip = get_net(g_ip);
    std::string mode = !ap.empty() ? "ap" : !ip.empty() ? "sta" : "connecting";
    char ap_ip[16];
    snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&g_ap_ip.ip));
    std::string shown_ip = !ap.empty() ? ap_ip : ip;
    std::vector<readings::Item> items;
    if (g_role == "field") {
        cJSON *st = field_json();
        int ready = 0;
        const cJSON *l;
        cJSON_ArrayForEach(l, cJSON_GetObjectItem(st, "links"))
            if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(l, "state")), "ready") == 0) ready++;
        bool up = g_client && g_client->connected();
        std::vector<std::string> lines = {
            up ? "app unit " + g_client->peer() : "app unit offline",
            "links " + std::to_string(ready) + "  probes " +
                std::to_string(cJSON_GetArraySize(cJSON_GetObjectItem(st, "probes"))),
            "devices " + std::to_string(cJSON_GetArraySize(cJSON_GetObjectItem(st, "devices"))) + "  MEATERs " +
                std::to_string(cJSON_GetArraySize(cJSON_GetObjectItem(st, "meaters")))};
        items.push_back(readings::status_item(st, mode, shown_ip, ap, "Field unit", &lines));
        cJSON_Delete(st);
    } else {
        NodeStatus ns = node_status();
        cJSON *st = cJSON_Parse(ns.json.c_str());
        if (!st) st = cJSON_CreateObject();
        items = readings::build_items(cfg, st, mode, shown_ip, ap);
        if (g_role == "app")
            for (auto &i : items)
                if (i.status) i.subs.insert(i.subs.begin() + 1, ns.field_connected ? "field " + ns.field_peer
                                                                                   : "field unit offline");
        cJSON_Delete(st);
    }
    screen::set_brightness(cfg.brightness);
    auto pages = readings::pages(cfg, items);
    screen_draw::View v;
    if (pages.empty()) {
        v.message = true;
        v.text = "Waiting for";
        v.sub = "MeatNet devices";
        return v;
    }
    int64_t now = core::ticks_ms();
    if (now - page_at >= cfg.rotate_s * 1000LL) {
        page_at = now;
        page++;
    }
    page %= static_cast<int>(pages.size());
    v.layout = cfg.layout;
    v.unit = cfg.unit;
    v.page = page;
    v.total = static_cast<int>(pages.size());
    v.items = pages[page];
    return v;
}

static void split_start()
{
    const core::Settings s = settings::get();
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
    g_serial_log = esp_log_set_vprintf(log_hook);
    settings::load();
    g_events = xEventGroupCreate();

    // Serial: the last 10 hex digits of the WiFi MAC, as the MicroPython unit.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[11];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X", mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_serial = serial;
    g_cfg = settings::get();
    g_role = g_cfg.role;
    if (!g_cfg.serial.empty()) g_serial = g_cfg.serial;
    ESP_LOGI(TAG, "MeatNet %s unit (ESP-IDF), serial %s", g_role.c_str(), g_serial.c_str());

    wifi_start();
    web::Handlers h;
    h.state = state_json;
    h.config_changed = [] { g_cfg_changed = true; };
    h.log = &g_log;
    h.power = [](bool standby) -> cJSON * {
        if (g_role == "field") return nullptr;
        uint32_t seen = g_published;
        g_power_req = standby ? 1 : 2;
        for (int i = 0; i < 20 && g_published == seen; i++) vTaskDelay(pdMS_TO_TICKS(50));
        cJSON *st = cJSON_Parse(node_status().json.c_str());
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON *pw = st ? cJSON_DetachItemFromObject(st, "power") : nullptr;
        cJSON_AddItemToObject(o, "power", pw ? pw : cJSON_CreateNull());
        cJSON_Delete(st);
        return o;
    };
    web::start(h, g_ap_ip.ip.addr);
    if (g_cfg.screen) screen::start(g_cfg.pins, screen_view);
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
        if (g_role != "field") {
            NodeStatus st = node_status();
            ESP_LOGI(TAG, "%d phones; %u probes, %u devices; links %u/%u ready%s%s; free heap %u", st.phones,
                     (unsigned)st.probes, (unsigned)st.devices, (unsigned)st.links_ready, (unsigned)st.links,
                     g_role == "app" ? (st.field_connected ? "; field unit " : "; field unit offline") : "",
                     g_role == "app" && st.field_connected ? st.field_peer.c_str() : "",
                     (unsigned)esp_get_free_heap_size());
            continue;
        }
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
