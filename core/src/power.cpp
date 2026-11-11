#include "power.h"

#include <cmath>

#include "cJSON.h"

namespace core {

static const std::pair<double, int> kCurve[] = {
    {4.20, 100}, {4.10, 90}, {4.00, 80}, {3.90, 66}, {3.80, 52},
    {3.70, 36}, {3.60, 20}, {3.50, 10}, {3.40, 5}, {3.30, 0},
};

int battery_percent(double v)
{
    if (v >= kCurve[0].first) return 100;
    for (size_t i = 0; i + 1 < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
        double v1 = kCurve[i].first, v0 = kCurve[i + 1].first;
        int p1 = kCurve[i].second, p0 = kCurve[i + 1].second;
        if (v >= v0) return static_cast<int>(std::nearbyint(p0 + (p1 - p0) * (v - v0) / (v1 - v0)));
    }
    return 0;
}

PowerManager::PowerManager(Settings &cfg, Transport &t, Node &node, Hooks hooks,
                           std::function<void(const std::string &)> log, int64_t now)
    : since(now), last_probe(now), cfg_(cfg), t_(t), node_(node), hooks_(std::move(hooks)), log_(std::move(log))
{
    if (cfg_.standby) enter("manual", now, false);
}

Battery PowerManager::read_battery()
{
    if (!hooks_.read_uv) return battery;
    double total = 0;
    for (int i = 0; i < 8; i++) {
        auto uv = hooks_.read_uv();
        if (!uv) return battery;
        total += *uv;
    }
    double v = total / 8 / 1e6 * cfg_.battery_divider;
    Battery b;
    b.present = v > 2.5;                // a floating/absent cell reads near 0
    if (b.present) {
        b.v = std::round(v * 100) / 100;
        b.pct = battery_percent(v);
    }
    battery = b;
    return battery;
}

void PowerManager::apply(bool on)
{
    t_.set_standby(on);
    node_.standby = on;
    if (hooks_.apply) hooks_.apply(on);
}

void PowerManager::save(bool on)
{
    if (cfg_.standby == on) return;
    cfg_.standby = on;
    if (hooks_.persist) hooks_.persist(on);
}

void PowerManager::enter(const std::string &why, int64_t now, bool persist)
{
    if (standby) {
        if (why == "manual" && reason != "manual") {
            reason = "manual";
            if (persist) save(true);
        }
        return;
    }
    standby = true;
    reason = why;
    since = now;
    apply(true);
    if (log_) log_("[power] standby (" + why + ")");
    if (persist && why == "manual") save(true);
}

void PowerManager::exit(const std::string &why, int64_t now)
{
    if (!standby) return;
    standby = false;
    reason.clear();
    since = now;
    last_probe = now;                   // don't drop straight back to auto
    apply(false);
    if (log_) log_("[power] awake (" + why + ")");
    if (why == "manual") save(false);
}

bool PowerManager::probe_heard(int64_t now) const
{
    for (auto &kv : t_.direct)
        if (now - kv.second.ticks < WAKE_FRESH_MS) return true;
    return false;
}

void PowerManager::tick(int64_t now)
{
    if (!bat_at_ || now - *bat_at_ >= BATTERY_EVERY_MS) {
        bat_at_ = now;
        read_battery();
    }
    if (probe_heard(now) || !t_.repeated.empty()) last_probe = now;
    int mins = cfg_.auto_standby_min;
    if (standby) {
        if (reason == "auto" && probe_heard(now)) exit("probe heard", now);
    } else if (mins > 0 && now - last_probe >= static_cast<int64_t>(mins) * 60000) {
        enter("auto", now);
    }
}

cJSON *PowerManager::status(int64_t now) const
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "standby", standby);
    if (reason.empty()) cJSON_AddNullToObject(o, "reason");
    else cJSON_AddStringToObject(o, "reason", reason.c_str());
    cJSON_AddNumberToObject(o, "since_s", static_cast<double>((now - since) / 1000));
    cJSON_AddNumberToObject(o, "auto_standby_min", cfg_.auto_standby_min);
    cJSON *b = cJSON_AddObjectToObject(o, "battery");
    cJSON_AddBoolToObject(b, "present", battery.present);
    if (battery.present) {
        cJSON_AddNumberToObject(b, "v", battery.v);
        cJSON_AddNumberToObject(b, "pct", battery.pct);
    } else {
        cJSON_AddNullToObject(b, "v");
        cJSON_AddNullToObject(b, "pct");
    }
    return o;
}

}  // namespace core
