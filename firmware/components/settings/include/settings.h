// The unit's settings (core::Settings), stored in NVS (namespace "meatnet")
// as one JSON blob, secrets included.
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

}  // namespace settings
