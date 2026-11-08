// The field unit's web page: settings and a status card.
//   GET  /             the page
//   GET  /api/state    status (from the callback given to start())
//   GET  /api/config   settings, secrets as "<name>_set" flags
//   POST /api/config   change settings (applied after a reboot)
//   POST /api/reboot
// On the setup network, any other path redirects to the page (captive
// portal); elsewhere it is a 404.
#pragma once

#include <cstdint>
#include <functional>

struct cJSON;

namespace web {

// `state` builds the /api/state object; the server deletes it. `setup_ip`
// is the setup network's address (network byte order).
void start(std::function<cJSON *()> state, uint32_t setup_ip);

}  // namespace web
