// Standby and battery monitoring (port of power.py). The platform supplies
// the hooks: reading the battery ADC, idling the hardware, saving the manual
// standby flag.
//
// Standby: stop advertising and drop connections (Transport::set_standby),
// let the Node idle, and whatever the platform adds (screen off, WiFi power
// save, ...). Auto standby: after auto_standby_min minutes with no probe
// heard, wake as soon as one advertises again. A manual standby only ends
// from the web page.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "node.h"

struct cJSON;

namespace core {

int battery_percent(double v);      // LiPo open-circuit voltage -> charge %

struct Battery {
    bool present = false;
    double v = 0;
    int pct = 0;
};

class PowerManager {
public:
    struct Hooks {
        std::function<std::optional<double>()> read_uv;   // the ADC pin in microvolts; nullopt = none
        std::function<void(bool standby)> apply;          // screen, WiFi power save, CPU, ...
        std::function<void(bool standby)> persist;        // remember a manual standby
    };
    static constexpr int64_t BATTERY_EVERY_MS = 10000;
    static constexpr int64_t WAKE_FRESH_MS = 10000;

    PowerManager(Settings &cfg, Transport &t, Node &node, Hooks hooks, std::function<void(const std::string &)> log,
                 int64_t now);

    Battery read_battery();
    void enter(const std::string &reason, int64_t now, bool persist = true);
    void exit(const std::string &why, int64_t now);
    void tick(int64_t now);
    cJSON *status(int64_t now) const;   // {standby, reason, since_s, auto_standby_min, battery}

    bool standby = false;
    std::string reason;                 // "manual" or "auto"
    int64_t since = 0, last_probe = 0;
    Battery battery;

private:
    void apply(bool on);
    void save(bool on);
    bool probe_heard(int64_t now) const;

    Settings &cfg_;
    Transport &t_;
    Node &node_;
    Hooks hooks_;
    std::function<void(const std::string &)> log_;
    std::optional<int64_t> bat_at_;
};

}  // namespace core
