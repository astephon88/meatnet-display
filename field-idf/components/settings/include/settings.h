// Field unit settings, stored in NVS (namespace "meatnet"). Loaded once at
// boot; changes are saved at once and applied by a reboot. Kconfig supplies
// defaults for the radio and the port only; WiFi, the app unit address and
// the split key only ever come from NVS.
#pragma once

#include <string>

struct cJSON;

namespace settings {

struct Settings {
    std::string wifi_ssid;
    std::string wifi_password;      // secret
    std::string split_peer;         // app unit host/IP; empty = UDP beacon
    int split_port;
    std::string split_key;          // secret
    int supervision_probe_ms;
    int supervision_node_ms;
    int peer_latency_max;
    std::string coex;               // "bt", "balance" or "wifi"
    std::string ap_password;        // secret; setup network, "" = open
};

void load();
const Settings &get();

// The settings as JSON for the web page: secrets replaced by
// "<name>_set": true/false.
cJSON *to_json();

// Applies the fields present in `obj`, validates them and saves them to NVS.
// Secrets: a missing wifi_password or ap_password keeps the saved one
// ("" = open network); a missing or empty split_key keeps the saved one. Returns false with `err`
// set when a value is invalid; nothing is saved then.
bool update(const cJSON *obj, std::string &err);

// Adopts WiFi credentials stored by the WiFi driver (from builds that set
// them in Kconfig) when none are saved here yet.
void adopt_wifi(const std::string &ssid, const std::string &password);

}  // namespace settings
