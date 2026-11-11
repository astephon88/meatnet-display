// Captive portal for the setup network: a DNS responder that answers every
// name with the unit's own address, so a phone that joins opens the
// settings page by itself. Runs only while the setup network is up.
#pragma once

#include <cstdint>

namespace captive {

// `ip` in network byte order (esp_ip4_addr_t::addr).
void start(uint32_t ip);
void stop();

}  // namespace captive
