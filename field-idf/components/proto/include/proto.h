// Pure helpers shared by the radio and the WiFi session: no ESP-IDF
// dependencies, so they can be unit-tested on the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace proto {

constexpr uint16_t kCombustionVendor = 0x09C7;
constexpr uint8_t kProductProbe = 1;
constexpr uint8_t kProductRepeated = 2;   // repeated probe data from a node
constexpr uint8_t kProductGauge = 3;      // >= 3: nodes (Gauge, Display, ...)

// Lowercase hex, as Python's bytes.hex() (the app unit parses either case).
std::string hex(const uint8_t *p, size_t n);
std::string hex(const std::string &bytes);
// Decode hex into raw bytes; false on bad input.
bool unhex(const std::string &text, std::string &out);

// MEATER: name starts with "MEATER" (any case), or a 128-bit service UUID
// (little-endian, as in advertising data) matching a known MEATER service.
bool meater_name(const uint8_t *name, size_t len);
bool meater_service_uuid(const uint8_t uuid_le[16]);

// "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" -> 16 bytes little-endian (as
// NimBLE's ble_uuid128_t stores them). False on bad input.
bool uuid128_le(const char *text, uint8_t out[16]);

// BLE addresses: NimBLE stores them least-significant byte first; the
// MeatNet protocol (and the Python units) use display order (as printed).
std::string addr_display(const uint8_t val_le[6]);
void addr_to_le(const std::string &display6, uint8_t val_le[6]);

}  // namespace proto
