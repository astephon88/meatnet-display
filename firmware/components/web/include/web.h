// The unit's web page (www/index.html, shared by every unit) and its API:
//   GET  /             the page
//   GET  /api/state    status (Handlers::state)
//   GET  /api/config   settings, secrets as "<name>_set" flags
//   POST /api/config   change settings: {ok, reboot?} (reboot: applied after a restart)
//   GET  /api/wifi/scan  nearby networks: {ok, networks: [{ssid, rssi, secure}]}
//   POST /api/wifi     {ssid, password?}: save and restart
//   POST /api/power    {standby: true|false}
//   GET  /api/log?since=N  {seq, lines}: log lines after N
//   POST /api/reboot
// On the setup network, any other path redirects to the page (captive
// portal); elsewhere it is a 404.
#pragma once

#include <cstdint>
#include <functional>

struct cJSON;

namespace core {
class LogRing;
}

namespace web {

struct Handlers {
    std::function<cJSON *()> state;             // the server deletes the object
    std::function<void()> config_changed;       // after a saved /api/config change
    std::function<cJSON *(bool standby)> power; // {ok, power}; nullptr = unavailable
    core::LogRing *log = nullptr;
};

// `setup_ip` is the setup network's address (network byte order).
void start(Handlers h, uint32_t setup_ip);

}  // namespace web
