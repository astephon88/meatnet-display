// The field unit's web page: settings and a status card.
//   GET  /             the page
//   GET  /api/state    status (from the callback given to start())
//   GET  /api/config   settings, secrets as "<name>_set" flags
//   POST /api/config   change settings (applied after a reboot)
//   POST /api/reboot
#pragma once

#include <functional>

struct cJSON;

namespace web {

// `state` builds the /api/state object; the server deletes it.
void start(std::function<cJSON *()> state);

}  // namespace web
