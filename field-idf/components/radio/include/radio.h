// Field unit radio: NimBLE observer (scan tables) now; central links come in
// milestone 3. Mirrors the scan side of the MicroPython ble.BleTransport, so
// the WiFi session can report the same four tables to the app unit.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct ble_gap_event;          // NimBLE types (host/ble_gap.h), global namespace
struct ble_gap_disc_desc;

namespace radio {

struct Sighting {
    std::string body;     // manufacturer data after the company ID (Combustion)
    std::string name;     // MEATER: advertised name ("" if only the UUID matched)
    int64_t ms = 0;       // when last heard (esp_timer milliseconds)
    uint8_t addr_type = 0;
    std::string addr;     // 6 bytes, display order
    int8_t rssi = 0;
};

// Keys are raw bytes: probe serial (4), node serial (10), MEATER address (6).
using Table = std::map<std::string, Sighting>;

class Radio {
public:
    static Radio &get();

    // Called from the NimBLE sync callback.
    void on_ready();

    // Scan tables. Hold lock() while reading them from another task.
    Table direct;     // probes advertising themselves (product type 1)
    Table repeated;   // probe data repeated by a node (type 2)
    Table nodes;      // Gauges, Displays, Boosters, Engines (type >= 3)
    Table meaters;
    void lock() { xSemaphoreTake(mutex_, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mutex_); }

    // Drop entries not heard for this long.
    void prune(int64_t now_ms, int64_t max_age_ms = 60000);

    // Never report the app unit's own advertisements back to it.
    void set_ignore(const std::string &mac_display, const std::string &serial10);

    // Scan mode, as commanded by the app unit.
    void set_scan_active(bool on);
    void set_standby(bool on);

    std::string address() const { return address_; }   // our BLE address, display order
    uint32_t adverts() const { return adverts_; }

    static int64_t now_ms();

private:
    Radio();
    void restart_scan();
    static int gap_event(::ble_gap_event *event, void *arg);
    void ingest(const ::ble_gap_disc_desc &d);

    SemaphoreHandle_t mutex_;
    std::string ignore_mac_, ignore_serial_;
    std::string address_;
    bool ready_ = false;
    bool scan_active_ = false;
    bool standby_ = false;
    uint32_t adverts_ = 0;
};

}  // namespace radio
