#include "settings.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace settings {

static const char *TAG = "settings";
static const char *NS = "meatnet";

static Settings g = {
    "", "", "", CONFIG_MEATNET_SPLIT_PORT, "",
    CONFIG_MEATNET_SUPERVISION_PROBE_MS, CONFIG_MEATNET_SUPERVISION_NODE_MS,
    CONFIG_MEATNET_PEER_LATENCY_MAX,
#if CONFIG_MEATNET_COEX_WIFI
    "wifi",
#elif CONFIG_MEATNET_COEX_BALANCE
    "balance",
#else
    "bt",
#endif
};

// -- NVS helpers (NVS keys are at most 15 characters) ---------------------------

static void get_str(nvs_handle_t h, const char *key, std::string &out)
{
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return;
    std::string v(len, '\0');
    if (nvs_get_str(h, key, &v[0], &len) == ESP_OK) {
        v.resize(len - 1);
        out = v;
    }
}

static void get_int(nvs_handle_t h, const char *key, int &out)
{
    int32_t v;
    if (nvs_get_i32(h, key, &v) == ESP_OK) out = v;
}

void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   // nothing saved yet
    get_str(h, "wifi_ssid", g.wifi_ssid);
    get_str(h, "wifi_pw", g.wifi_password);
    get_str(h, "split_peer", g.split_peer);
    get_int(h, "split_port", g.split_port);
    get_str(h, "split_key", g.split_key);
    get_int(h, "sup_probe_ms", g.supervision_probe_ms);
    get_int(h, "sup_node_ms", g.supervision_node_ms);
    get_int(h, "lat_max", g.peer_latency_max);
    get_str(h, "coex", g.coex);
    nvs_close(h);
}

const Settings &get() { return g; }

static bool save(const Settings &s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "wifi_ssid", s.wifi_ssid.c_str()) == ESP_OK
           && nvs_set_str(h, "wifi_pw", s.wifi_password.c_str()) == ESP_OK
           && nvs_set_str(h, "split_peer", s.split_peer.c_str()) == ESP_OK
           && nvs_set_i32(h, "split_port", s.split_port) == ESP_OK
           && nvs_set_str(h, "split_key", s.split_key.c_str()) == ESP_OK
           && nvs_set_i32(h, "sup_probe_ms", s.supervision_probe_ms) == ESP_OK
           && nvs_set_i32(h, "sup_node_ms", s.supervision_node_ms) == ESP_OK
           && nvs_set_i32(h, "lat_max", s.peer_latency_max) == ESP_OK
           && nvs_set_str(h, "coex", s.coex.c_str()) == ESP_OK
           && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// -- JSON ---------------------------------------------------------------------------

cJSON *to_json()
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", g.wifi_ssid.c_str());
    cJSON_AddBoolToObject(o, "wifi_password_set", !g.wifi_password.empty());
    cJSON_AddStringToObject(o, "split_peer", g.split_peer.c_str());
    cJSON_AddNumberToObject(o, "split_port", g.split_port);
    cJSON_AddBoolToObject(o, "split_key_set", !g.split_key.empty());
    cJSON_AddNumberToObject(o, "supervision_probe_ms", g.supervision_probe_ms);
    cJSON_AddNumberToObject(o, "supervision_node_ms", g.supervision_node_ms);
    cJSON_AddNumberToObject(o, "peer_latency_max", g.peer_latency_max);
    cJSON_AddStringToObject(o, "coex", g.coex.c_str());
    return o;
}

static bool take_str(const cJSON *obj, const char *name, size_t max, std::string &out,
                     std::string &err)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!v) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) {
        err = std::string(name) + ": text of at most " + std::to_string(max) + " characters";
        return false;
    }
    out = v->valuestring;
    return true;
}

static bool take_int(const cJSON *obj, const char *name, int lo, int hi, int &out,
                     std::string &err)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!v) return true;
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi
        || v->valuedouble != (int)v->valuedouble) {
        err = std::string(name) + ": a whole number from " + std::to_string(lo) + " to "
            + std::to_string(hi);
        return false;
    }
    out = (int)v->valuedouble;
    return true;
}

bool update(const cJSON *obj, std::string &err)
{
    Settings s = g;
    std::string pw = s.wifi_password, key = s.split_key;
    if (!take_str(obj, "wifi_ssid", 32, s.wifi_ssid, err)
        || !take_str(obj, "wifi_password", 63, pw, err)
        || !take_str(obj, "split_peer", 64, s.split_peer, err)
        || !take_int(obj, "split_port", 1, 65535, s.split_port, err)
        || !take_str(obj, "split_key", 64, key, err)
        || !take_int(obj, "supervision_probe_ms", 1000, 32000, s.supervision_probe_ms, err)
        || !take_int(obj, "supervision_node_ms", 1000, 32000, s.supervision_node_ms, err)
        || !take_int(obj, "peer_latency_max", 0, 10, s.peer_latency_max, err)
        || !take_str(obj, "coex", 7, s.coex, err))
        return false;
    if (s.coex != "bt" && s.coex != "balance" && s.coex != "wifi") {
        err = "coex: bt, balance or wifi";
        return false;
    }
    if (!pw.empty() && pw.size() < 8) {
        err = "wifi_password: 8 to 63 characters (empty = open network)";
        return false;
    }
    // A blank split key in the form means "keep"; it can't be cleared to
    // nothing, since the app unit always checks it.
    if (!key.empty()) s.split_key = key;
    s.wifi_password = pw;
    if (!save(s)) {
        err = "could not save to flash";
        return false;
    }
    g = s;
    ESP_LOGI(TAG, "settings saved");
    return true;
}

void adopt_wifi(const std::string &ssid, const std::string &password)
{
    if (!g.wifi_ssid.empty() || ssid.empty()) return;
    Settings s = g;
    s.wifi_ssid = ssid;
    s.wifi_password = password;
    if (save(s)) {
        g = s;
        ESP_LOGI(TAG, "adopted WiFi network '%s' from the WiFi driver", ssid.c_str());
    }
}

}  // namespace settings
