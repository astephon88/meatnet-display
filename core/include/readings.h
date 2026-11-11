// Configured readings + live MeatNet data -> display items (port of
// readings.py). Works on the /api/state tables (core::node_status), so the
// screen and the web page see the same data. A reading is a Settings
// Reading: dev "probe:<serial>" or "node:<serial>", a field, an optional label.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "unit_settings.h"

struct cJSON;

namespace readings {

constexpr int STALE_S = 15;

struct Item {
    std::string label, value, unit, short_value = "--";
    std::vector<std::string> subs;
    bool stale = true, alarm = false, status = false;
    std::array<uint8_t, 3> accent{200, 200, 200};
    bool operator==(const Item &o) const
    {
        return label == o.label && value == o.value && unit == o.unit && short_value == o.short_value &&
               subs == o.subs && stale == o.stale && alarm == o.alarm && status == o.status &&
               accent == o.accent;
    }
};

std::string fmt_temp(const cJSON *c, const std::string &unit);     // big value: "60.0", "140", "--"
std::string fmt_temp(double c, const std::string &unit);
std::string fmt_short(double c, const std::string &unit);
std::string fmt_eta(double seconds);                                // "1h05", "5m"
std::string field_label(const std::string &field);

std::vector<core::Reading> default_readings(const cJSON *status);   // core of every probe heard
Item resolve(const core::Reading &r, const cJSON *status, const std::string &unit);
// The status page; `lines` replaces the node summary (a field unit's own).
Item status_item(const cJSON *status, const std::string &wifi_mode, const std::string &ip,
                 const std::string &ap_ssid, const std::string &title = "MeatNet",
                 const std::vector<std::string> *lines = nullptr);
std::vector<Item> build_items(const core::Settings &cfg, const cJSON *status, const std::string &wifi_mode,
                              const std::string &ip, const std::string &ap_ssid);
std::vector<std::vector<Item>> pages(const core::Settings &cfg, const std::vector<Item> &items);

}  // namespace readings
