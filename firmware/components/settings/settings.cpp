#include "settings.h"

#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

namespace settings {

static const char *TAG = "settings";
static const char *NS = "meatnet";
static const char *BLOB = "cfg";

static std::mutex g_mu;
static core::Settings g;

static bool save(const core::Settings &s)
{
    cJSON *o = core::settings_to_json(s, true);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return false;
    nvs_handle_t h;
    bool ok = nvs_open(NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, BLOB, text, strlen(text)) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    cJSON_free(text);
    return ok;
}

void load()
{
    core::Settings s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_blob(h, BLOB, nullptr, &len) == ESP_OK && len > 0) {
            std::string text(len, '\0');
            if (nvs_get_blob(h, BLOB, &text[0], &len) == ESP_OK) core::merge_settings(s, text);
        }
        nvs_close(h);
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

}  // namespace settings
