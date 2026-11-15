// The unit's radio, scan side: a NimBLE observer filling the four scan
// tables (links.h holds the outbound links).
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "transport.h"

struct ble_gap_event;          // NimBLE types (host/ble_gap.h), global namespace
struct ble_gap_disc_desc;

namespace radio {

// Keys are raw bytes: probe serial (4), node serial (10), MEATER address (6).
// Sightings are timed in esp_timer milliseconds (core::ticks_ms()'s clock).
using Table = std::map<std::string, core::Observation>;

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

    // Scanning at all: off on an app unit, whose devices are the field unit's.
    void set_scan_enabled(bool on) { scan_enabled_ = on; }

    // Scan mode, as commanded by the app unit.
    void set_scan_active(bool on);
    void set_standby(bool on);

    // Scan pacing, driven by the 10 Hz radio task: duty in percent (50
    // unlinked, lower while links are up or streaming). Scanning is paused
    // around each outgoing connect (NimBLE can't connect while scanning)
    // and resumed by ensure_scan().
    void set_duty(int percent);
    void pause_scan();
    void ensure_scan();

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
    bool scanning_ = false;
    int duty_ = 50;
    bool scan_active_ = false;
    bool scan_enabled_ = true;
    bool standby_ = false;
    uint32_t adverts_ = 0;
};

}  // namespace radio
