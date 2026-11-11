// The unit's settings (core::Settings), stored in NVS (namespace "meatnet")
// as one JSON blob, secrets included. Kconfig supplies defaults for the radio
// and the split port only; WiFi, the app unit address and the shared key only
// ever come from NVS. Units built before the blob had one NVS key per
// setting: load() moves those over once.
#pragma once

#include <string>

#include "unit_settings.h"

struct cJSON;

namespace settings {

void load();
core::Settings get();               // a copy: the web server changes them

// The settings as JSON for the web page: secrets replaced by
// "<name>_set": true/false.
cJSON *to_json();

// A change from the web page (core::apply_config), saved at once. Returns
// false with `err` set when a value is invalid; nothing changes then.
// `reboot`: some changed key only takes effect after a restart.
bool update(const cJSON *obj, std::string &err, bool &reboot);

// Remembers a manual standby across restarts (the power manager).
void set_standby(bool on);

// Adopts WiFi credentials stored by the WiFi driver (from builds that set
// them in Kconfig) when none are saved here yet.
void adopt_wifi(const std::string &ssid, const std::string &password);

}  // namespace settings
