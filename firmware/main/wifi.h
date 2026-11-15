// The unit's WiFi: the station and, while it has no network, the setup
// network "MeatNet-<serial>".
#pragma once

#include <cstdint>
#include <string>

#include "unit_settings.h"

namespace wifi {

// Starts WiFi with the unit's settings (network, coexistence preference; the
// hostname is used for DHCP and mDNS, <hostname>.local).
void start(const std::string &serial, const core::Settings &s);

std::string ip();               // the station's address, "" while not joined
std::string setup_ssid();       // the setup network's name while it is up, else ""
uint32_t setup_addr();          // the setup network's address (network byte order)
std::string setup_ip();         // ... as text

}  // namespace wifi
