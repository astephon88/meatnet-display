// MeatNet unit (ESP-IDF): the standalone, app or field role, as saved in NVS.
//
// Joins WiFi, runs the radio (scanning and links: standalone and field) and
// the MeatNet node (standalone and app); a field unit streams its radio to
// the app unit over the split protocol. Settings are changed on the unit's
// web page; with no WiFi set up (or none reachable) it also opens a setup
// network for that.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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

#include "battery.h"
#include "links.h"
#include "proto.h"
#include "radio.h"
#include "screen.h"
#include "esp_radio.h"
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
#include "wifi.h"

static const char *TAG = "main";

static EventGroupHandle_t g_events;
static constexpr EventBits_t kBleReady = BIT0;

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
static core::Settings g_cfg;            // the settings, as loaded at start (the node task updates its copy)

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

static constexpr int kBurstScanDuty = 5;       // % while a link streams (log reads)
static constexpr uint32_t kBurstRate = 20;     // notifications/s = "streaming"
static constexpr int64_t kBurstHoldMs = 5000;

static void radio_task(void *)
{
    radio::Radio &r = radio::Radio::get();
    radio::Links &links = radio::Links::get();
    int64_t rate_at = radio::Radio::now_ms(), burst_until = 0;
    uint32_t rate_count = links.total_rx();
    const int linked_duty = g_cfg.linked_scan_duty;    // % while any link is up (applied at start)
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
                 : links.any_connected() ? linked_duty : 50;
        r.set_duty(duty);
        if (!links.connecting()) r.ensure_scan();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// -- app and standalone roles: the MeatNet node ----------------------------------------

static core::LinkTable g_link_table;
static radio::EspRadio g_radio;
static core::LocalTransport *g_local = nullptr;
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
    g_local = new core::LocalTransport(periph::Peripheral::get(), standalone ? &g_radio : nullptr);
    core::Transport *t = g_local;
    if (!standalone) t = g_remote = new split::RemoteRadio(g_local, node_log);
    // Standalone: phones and links share this radio's connections.
    g_local->max_conn = std::min(g_cfg.max_conn, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
    periph::Peripheral::get().set_limits(standalone ? g_local->max_conn : CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
                                         standalone ? [] { return radio::Links::get().active(); }
                                                    : static_cast<size_t (*)()>(nullptr));
    core::Node node(*t, g_cfg, g_serial, node_log, &g_link_table);
    core::PowerManager::Hooks hooks;
    hooks.read_uv = [] { return battery_uv(g_cfg.battery_pin); };
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
        // Changes from the web page apply at once (the ones marked
        // "reboot" only matter at the next start).
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

static split::SplitClient *g_client = nullptr;

// A field unit has no Node: its part of /api/state comes from the radio.
static cJSON *field_json()
{
    core::RadioSource::Tables t = g_radio.tables();
    return core::field_status(t.direct, t.nodes, t.meaters, g_radio.link_summaries(),
                              CONFIG_BT_NIMBLE_MAX_CONNECTIONS, core::ticks_ms());
}

// /api/state for the web page: the Node's (or the field radio's) tables plus
// this unit's own state.
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
    std::string ap = wifi::setup_ssid(), ip = wifi::ip();
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "mode", !ip.empty() ? "sta" : !ap.empty() ? "ap" : "connecting");
    cJSON_AddStringToObject(w, "ip", !ip.empty() ? ip.c_str() : !ap.empty() ? wifi::setup_ip().c_str() : "");
    cJSON_AddStringToObject(w, "ssid", settings::get().wifi_ssid.c_str());
    cJSON_AddStringToObject(w, "ap_ssid", ap.c_str());
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "display", "GC9A01A 240x240");
    if (!cJSON_GetObjectItem(o, "power")) cJSON_AddNullToObject(o, "power");
    if (g_role == "field")
        cJSON_AddItemToObject(o, "split", g_client ? core::split_status(g_client->connected(), g_client->peer(),
                                                                         g_client->peer_ip(), g_client->rtt_ms())
                                                   : core::split_status(false, "", "", -1));
    else if (g_role == "app")
        cJSON_AddItemToObject(o, "split", core::split_status(st.field_connected, st.field_peer, st.field_ip,
                                                             st.field_rtt_ms, st.field_max_conn));
    else
        cJSON_AddNullToObject(o, "split");
    return o;
}

// The round screen: readings pages (app and standalone), or the field unit's
// status page. Runs on the screen task.
static screen_draw::View screen_view()
{
    static int page = 0;
    static int64_t page_at = 0;
    core::Settings cfg = settings::get();
    std::string ap = wifi::setup_ssid(), ip = wifi::ip();
    std::string mode = !ap.empty() ? "ap" : !ip.empty() ? "sta" : "connecting";
    std::string shown_ip = !ap.empty() ? wifi::setup_ip() : ip;
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
    static split::FieldRadio fradio(g_radio);
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

    // Serial: the last 10 hex digits of the WiFi MAC.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[11];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X", mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_serial = serial;
    g_cfg = settings::get();
    g_role = g_cfg.role;
    if (!g_cfg.serial.empty()) g_serial = g_cfg.serial;
    ESP_LOGI(TAG, "MeatNet %s unit (ESP-IDF), serial %s", g_role.c_str(), g_serial.c_str());

    wifi::start(g_serial, g_cfg);
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
    web::start(h, wifi::setup_addr());
    if (g_cfg.screen) screen::start(g_cfg.pins, screen_view);
    ble_start();
    xEventGroupWaitBits(g_events, kBleReady, pdFALSE, pdTRUE, portMAX_DELAY);
    radio::Links::get().max_links = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    if (g_role != "app") xTaskCreate(radio_task, "radio", 6144, nullptr, 6, nullptr);
    if (g_role != "field") xTaskCreate(node_task, "node", 12288, nullptr, 5, nullptr);

    radio::Radio &r = radio::Radio::get();
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool wifi = !wifi::ip().empty();
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
        for (auto &l : g_radio.link_summaries()) {
            total++;
            if (l.state == "ready") ready++;
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
