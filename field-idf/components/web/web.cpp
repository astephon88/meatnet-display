#include "web.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "settings.h"

extern const char page_start[] asm("_binary_settings_html_start");
extern const char page_end[] asm("_binary_settings_html_end");

namespace web {

static const char *TAG = "web";
static constexpr size_t kMaxBody = 2048;
static std::function<cJSON *()> g_state;
static uint32_t g_setup_ip;

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const std::string &error = "",
                             bool reboot = false)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok) cJSON_AddStringToObject(o, "error", error.c_str());
    if (reboot) cJSON_AddBoolToObject(o, "reboot", true);
    return send_json(req, o);
}

// Reads a JSON request body; nullptr (and an error reply sent) on failure.
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len > kMaxBody) {
        send_result(req, false, "request too large");
        return nullptr;
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int n = httpd_req_recv(req, &body[got], body.size() - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return nullptr;
        got += n;
    }
    cJSON *o = cJSON_Parse(body.c_str());
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        send_result(req, false, "expected a JSON object");
        return nullptr;
    }
    return o;
}

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);   // drop the NUL
}

static esp_err_t state(httpd_req_t *req)
{
    return send_json(req, g_state ? g_state() : cJSON_CreateObject());
}

static esp_err_t config_get(httpd_req_t *req)
{
    return send_json(req, settings::to_json());
}

static esp_err_t config_post(httpd_req_t *req)
{
    cJSON *o = read_json(req);
    if (!o) return ESP_OK;
    std::string err;
    bool ok = settings::update(o, err);
    cJSON_Delete(o);
    return send_result(req, ok, err, ok);
}

// Nearby networks, strongest first, one entry per name (hidden ones left
// out). It can't run while the station is in the middle of joining a
// network. Short channel visits with time back home in between, so the
// link to the app unit keeps flowing: with the default (120 ms per channel)
// the scan took ~5 s and the split session timed out, which closes every
// device link.
static esp_err_t wifi_scan(httpd_req_t *req)
{
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return send_result(req, false, "a scan is already running");
    wifi_scan_config_t sc = {};
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 20;
    sc.scan_time.active.max = 60;
    sc.home_chan_dwell_time = 100;
    sc.coex_background_scan = true;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        busy = false;
        return send_result(req, false, err == ESP_ERR_WIFI_STATE
                                           ? "busy joining a network; try again in a moment"
                                           : std::string("scan failed: ") + esp_err_to_name(err));
    }
    uint16_t n = 20;
    static wifi_ap_record_t recs[20];          // off the server's stack; `busy` guards it
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    std::map<std::string, const wifi_ap_record_t *> best;
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid((const char *)recs[i].ssid);
        if (ssid.empty()) continue;
        auto it = best.find(ssid);
        if (it == best.end() || recs[i].rssi > it->second->rssi) best[ssid] = &recs[i];
    }
    std::vector<const wifi_ap_record_t *> list;
    for (auto &kv : best) list.push_back(kv.second);
    std::sort(list.begin(), list.end(),
              [](const wifi_ap_record_t *a, const wifi_ap_record_t *b) { return a->rssi > b->rssi; });
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (auto *r : list) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", (const char *)r->ssid);
        cJSON_AddNumberToObject(e, "rssi", r->rssi);
        cJSON_AddBoolToObject(e, "secure", r->authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, e);
    }
    busy = false;
    return send_json(req, o);
}

// Unknown paths: on the setup network, send the phone to the settings page
// (its captive-portal check lands here, since the setup network's DNS
// answers every name with the unit); elsewhere a plain 404.
// The local IPv4 address a request arrived on (network byte order), or 0.
// With IPv6 on, the server's socket is dual-stack and reports IPv4 peers as
// IPv4-mapped IPv6 addresses (::ffff:a.b.c.d).
static uint32_t local_ipv4(httpd_req_t *req)
{
    sockaddr_storage local = {};
    socklen_t len = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (sockaddr *)&local, &len) != 0) return 0;
    uint32_t ip = 0;
    if (local.ss_family == AF_INET) {
        ip = ((sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        const uint8_t *a = (const uint8_t *)&((sockaddr_in6 *)&local)->sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (memcmp(a, mapped, sizeof(mapped)) == 0) memcpy(&ip, a + 12, 4);
    }
    return ip;
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t)
{
    uint32_t ip = local_ipv4(req);
    if (ip && ip == g_setup_ip) {
        char loc[32];
        snprintf(loc, sizeof(loc), "http://%u.%u.%u.%u/", (unsigned)(ip & 0xFF),
                 (unsigned)(ip >> 8 & 0xFF), (unsigned)(ip >> 16 & 0xFF), (unsigned)(ip >> 24));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, nullptr, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, nullptr);
}

static void restart(void *) { esp_restart(); }

static esp_err_t reboot(httpd_req_t *req)
{
    send_result(req, true);
    // Let the reply go out first.
    static esp_timer_handle_t t = nullptr;
    if (!t) {
        esp_timer_create_args_t a = {};
        a.callback = restart;
        a.name = "reboot";
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 500 * 1000);
    return ESP_OK;
}

void start(std::function<cJSON *()> state_fn, uint32_t setup_ip)
{
    g_state = state_fn;
    g_setup_ip = setup_ip;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;       // lwIP has 10 sockets; split link + captive DNS need 3
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, page, nullptr},
        {"/api/state", HTTP_GET, state, nullptr},
        {"/api/config", HTTP_GET, config_get, nullptr},
        {"/api/config", HTTP_POST, config_post, nullptr},
        {"/api/wifi/scan", HTTP_GET, wifi_scan, nullptr},
        {"/api/reboot", HTTP_POST, reboot, nullptr},
    };
    for (const auto &u : uris) httpd_register_uri_handler(server, &u);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web page on port %d", cfg.server_port);
}

}  // namespace web
