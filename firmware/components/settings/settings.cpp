#include "settings.h"

#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace settings {

static const char *TAG = "settings";
static const char *NS = "meatnet";
static const char *BLOB = "cfg";

static std::mutex g_mu;
static core::Settings g;

static core::Settings defaults()
{
    core::Settings s;
    s.split_port = CONFIG_MEATNET_SPLIT_PORT;
    s.supervision_probe_ms = CONFIG_MEATNET_SUPERVISION_PROBE_MS;
    s.supervision_node_ms = CONFIG_MEATNET_SUPERVISION_NODE_MS;
    s.peer_latency_max = CONFIG_MEATNET_PEER_LATENCY_MAX;
#if CONFIG_MEATNET_COEX_WIFI
    s.coex = "wifi";
#elif CONFIG_MEATNET_COEX_BALANCE
    s.coex = "balance";
#else
    s.coex = "bt";
#endif
    return s;
}

// -- NVS helpers (NVS keys are at most 15 characters) ---------------------------

static bool get_str(nvs_handle_t h, const char *key, std::string &out)
{
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return false;
    std::string v(len, '\0');
    if (nvs_get_str(h, key, &v[0], &len) != ESP_OK) return false;
    v.resize(len - 1);
    out = v;
    return true;
}

static bool get_int(nvs_handle_t h, const char *key, int &out)
{
    int32_t v;
    if (nvs_get_i32(h, key, &v) != ESP_OK) return false;
    out = v;
    return true;
}

// Settings saved one key at a time by earlier builds (all field units, so
// the role defaults to field), plus their node settings blob.
static const char *const kOldKeys[] = {"wifi_ssid", "wifi_pw", "split_peer", "split_port", "split_key",
                                       "sup_probe_ms", "sup_node_ms", "lat_max", "coex", "ap_pw", "role",
                                       "node"};

static bool migrate(nvs_handle_t h, core::Settings &s)
{
    bool any = false;
    std::string node;
    if (get_str(h, "node", node)) {
        core::merge_settings(s, node);
        any = true;
    }
    s.role = "field";
    any |= get_str(h, "wifi_ssid", s.wifi_ssid);
    any |= get_str(h, "wifi_pw", s.wifi_password);
    any |= get_str(h, "split_peer", s.split_peer);
    any |= get_int(h, "split_port", s.split_port);
    any |= get_str(h, "split_key", s.split_key);
    any |= get_int(h, "sup_probe_ms", s.supervision_probe_ms);
    any |= get_int(h, "sup_node_ms", s.supervision_node_ms);
    any |= get_int(h, "lat_max", s.peer_latency_max);
    any |= get_str(h, "coex", s.coex);
    any |= get_str(h, "ap_pw", s.ap_password);
    any |= get_str(h, "role", s.role);
    return any;
}

static bool save(const core::Settings &s)
{
    cJSON *o = core::settings_to_json(s, true);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return false;
    nvs_handle_t h;
    bool ok = nvs_open(NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, BLOB, text, strlen(text)) == ESP_OK;
        // The per-key values of earlier builds are in the blob now.
        if (ok)
            for (const char *k : kOldKeys) nvs_erase_key(h, k);
        ok = ok && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    cJSON_free(text);
    return ok;
}

void load()
{
    core::Settings s = defaults();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_blob(h, BLOB, nullptr, &len) == ESP_OK && len > 0) {
            std::string text(len, '\0');
            if (nvs_get_blob(h, BLOB, &text[0], &len) == ESP_OK) core::merge_settings(s, text);
            nvs_close(h);
        } else {
            bool old = migrate(h, s);
            nvs_close(h);
            if (old) ESP_LOGI(TAG, "moved the settings to one NVS entry: %s", save(s) ? "ok" : "failed");
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g = s;
}

core::Settings get()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g;
}

cJSON *to_json()
{
    return core::settings_to_json(get(), false);
}

bool update(const cJSON *obj, std::string &err, bool &reboot)
{
    std::lock_guard<std::mutex> lk(g_mu);
    core::Settings s = g;
    if (!core::apply_config(s, obj, err, reboot)) return false;
    if (!save(s)) {
        err = "could not save to flash";
        return false;
    }
    g = s;
    ESP_LOGI(TAG, "settings saved");
    return true;
}

void set_standby(bool on)
{
    std::lock_guard<std::mutex> lk(g_mu);
    core::Settings s = g;
    s.standby = on;
    if (save(s)) g = s;
}

void adopt_wifi(const std::string &ssid, const std::string &password)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.wifi_ssid.empty() || ssid.empty()) return;
    core::Settings s = g;
    s.wifi_ssid = ssid;
    s.wifi_password = password;
    if (save(s)) {
        g = s;
        ESP_LOGI(TAG, "adopted WiFi network '%s' from the WiFi driver", ssid.c_str());
    }
}

}  // namespace settings
