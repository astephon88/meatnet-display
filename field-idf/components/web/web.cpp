#include "web.h"

#include <string>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "settings.h"

extern const char page_start[] asm("_binary_settings_html_start");
extern const char page_end[] asm("_binary_settings_html_end");

namespace web {

static const char *TAG = "web";
static constexpr size_t kMaxBody = 2048;
static std::function<cJSON *()> g_state;

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

void start(std::function<cJSON *()> state_fn)
{
    g_state = state_fn;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;       // lwIP has 10 sockets; the split link needs 2
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
        {"/api/reboot", HTTP_POST, reboot, nullptr},
    };
    for (const auto &u : uris) httpd_register_uri_handler(server, &u);
    ESP_LOGI(TAG, "web page on port %d", cfg.server_port);
}

}  // namespace web
