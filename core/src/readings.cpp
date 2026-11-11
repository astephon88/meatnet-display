#include "readings.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "cJSON.h"

namespace readings {

static const std::pair<const char *, const char *> kProbeFields[] = {
    {"core", "Core"}, {"surface", "Surface"}, {"ambient", "Ambient"}, {"instant", "Instant"},
    {"eta", "Time left"}, {"t1", "T1"}, {"t2", "T2"}, {"t3", "T3"}, {"t4", "T4"},
    {"t5", "T5"}, {"t6", "T6"}, {"t7", "T7"}, {"t8", "T8"},
};
static const std::pair<const char *, const char *> kNodeFields[] = {{"temp", "Temp"}, {"setpoint", "Set point"}};

// Probe accent colours by probe ID 1-8.
static const std::array<uint8_t, 3> kAccents[] = {
    {255, 196, 0}, {64, 160, 255}, {80, 220, 100}, {255, 96, 96},
    {200, 120, 255}, {0, 210, 210}, {255, 150, 40}, {220, 220, 220},
};

static const char kDeg[] = "\xB0";      // the fonts' degree sign (Latin-1)

static double to_unit(double c, const std::string &unit) { return unit == "f" ? c * 9 / 5 + 32 : c; }

// Python's round(): halves go to the even neighbour.
static long py_round(double v)
{
    double r = std::nearbyint(v);
    return static_cast<long>(r);
}

static std::string printf_str(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string printf_str(const char *fmt, ...)
{
    char buf[64];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    return buf;
}

std::string fmt_temp(double c, const std::string &unit)
{
    double v = to_unit(c, unit);
    if (std::fabs(v) >= 100) return printf_str("%ld", py_round(v));
    return printf_str("%.1f", v);
}

std::string fmt_temp(const cJSON *c, const std::string &unit)
{
    return cJSON_IsNumber(c) ? fmt_temp(c->valuedouble, unit) : "--";
}

std::string fmt_short(double c, const std::string &unit) { return printf_str("%ld", py_round(to_unit(c, unit))); }

static std::string fmt_short(const cJSON *c, const std::string &unit)
{
    return cJSON_IsNumber(c) ? fmt_short(c->valuedouble, unit) : "--";
}

std::string fmt_eta(double seconds)
{
    long m = static_cast<long>(seconds) / 60;
    if (m >= 60) return printf_str("%ldh%02ld", m / 60, m % 60);
    return printf_str("%ldm", m);
}

std::string field_label(const std::string &field)
{
    for (auto &f : kProbeFields)
        if (field == f.first) return f.second;
    for (auto &f : kNodeFields)
        if (field == f.first) return f.second;
    return field;
}

static double num(const cJSON *o, const char *k, double dflt = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static std::string str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static const cJSON *arr(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

std::vector<core::Reading> default_readings(const cJSON *status)
{
    std::vector<std::pair<std::pair<int, std::string>, std::string>> probes;
    const cJSON *p;
    cJSON_ArrayForEach(p, arr(status, "probes")) {
        std::string serial = str(p, "serial");
        probes.push_back({{static_cast<int>(num(p, "id")), serial}, serial});
    }
    std::sort(probes.begin(), probes.end());
    std::vector<core::Reading> out;
    for (auto &x : probes) out.push_back({"probe:" + x.second, "core", ""});
    return out;
}

static const cJSON *find(const cJSON *status, const std::string &dev, std::string &kind)
{
    size_t colon = dev.find(':');
    kind = dev.substr(0, colon);
    std::string serial = colon == std::string::npos ? "" : dev.substr(colon + 1);
    const cJSON *e;
    cJSON_ArrayForEach(e, arr(status, kind == "probe" ? "probes" : "devices"))
        if (str(e, "serial") == serial) return e;
    return nullptr;
}

static const cJSON *predicting(const cJSON *p)
{
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(p, "prediction");
    if (!cJSON_IsObject(pr) || num(pr, "state") != 3) return nullptr;
    int type = static_cast<int>(num(pr, "type"));
    return type == 1 || type == 2 ? pr : nullptr;
}

Item resolve(const core::Reading &r, const cJSON *status, const std::string &unit)
{
    std::string kind;
    const cJSON *d = find(status, r.dev, kind);
    const std::string &field = r.field.empty() ? std::string("core") : r.field;
    std::string label = r.label;
    Item item;
    item.value = "--";
    std::string u = unit == "f" ? "F" : "C";
    if (!d) {
        item.label = label.empty() ? field_label(field) : label;
        item.subs = {"not found"};
        return item;
    }
    double age = num(d, "age_s");
    item.stale = age > STALE_S;
    if (kind == "probe") {
        int pid = static_cast<int>(num(d, "id", 1));
        item.accent = kAccents[((pid - 1) % 8 + 8) % 8];
        item.alarm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "alarming"));
        if (label.empty()) label = "P" + std::to_string(pid) + " " + field_label(field);
        if (field == "eta") {
            const cJSON *pr = predicting(d);
            item.value = pr ? fmt_eta(num(pr, "seconds")) : "--";
            item.short_value = item.value;
            if (pr) item.subs.push_back("to " + fmt_short(num(pr, "setpoint_c"), unit) + kDeg + u);
        } else {
            const cJSON *c = nullptr;
            if (field == "core" || field == "surface" || field == "ambient" || field == "instant") {
                c = cJSON_GetObjectItemCaseSensitive(d, (field + "_c").c_str());
                const cJSON *inst = cJSON_GetObjectItemCaseSensitive(d, "instant_c");
                if (field == "core" && !cJSON_IsNumber(c) && cJSON_IsNumber(inst)) {
                    c = inst;                   // Instant Read: show it on core
                    label = r.label.empty() ? "P" + std::to_string(pid) + " Instant" : r.label;
                }
            } else if (field.size() == 2 && field[0] == 't' && field[1] >= '1' && field[1] <= '8') {
                c = cJSON_GetArrayItem(arr(d, "temps"), field[1] - '1');
            }
            item.value = fmt_temp(c, unit);
            item.short_value = fmt_short(c, unit);
            item.unit = cJSON_IsNumber(c) ? kDeg + u : "";
            if (field == "core") {
                const cJSON *surf = cJSON_GetObjectItemCaseSensitive(d, "surface_c");
                if (cJSON_IsNumber(surf))
                    item.subs.push_back("Surf " + fmt_short(surf, unit) + kDeg + "  Amb " +
                                        fmt_short(cJSON_GetObjectItemCaseSensitive(d, "ambient_c"), unit) + kDeg);
                if (const cJSON *pr = predicting(d)) item.subs.push_back("Ready in " + fmt_eta(num(pr, "seconds")));
            }
        }
        std::string src = str(d, "src");
        std::string info = src == "link" ? "linked" : src == "advert" ? "direct" : src == "node" ? "via node"
                         : src == "repeated" ? "repeated" : src == "meater" ? "MEATER" : src;
        const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(d, "rssi");
        if (cJSON_IsNumber(rssi) && (src == "link" || src == "advert" || src == "meater"))
            info += " " + std::to_string(static_cast<int>(rssi->valuedouble)) + "dBm";
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "low_battery"))) item.subs.push_back("LOW BATTERY");
        item.subs.push_back(info);
    } else {
        std::string name = str(d, "name");
        if (name.empty()) name = "Node";
        if (label.empty()) label = field != "temp" ? name + " " + field_label(field) : name;
        const cJSON *c = field == "temp" ? cJSON_GetObjectItemCaseSensitive(d, "temp_c")
                       : field == "setpoint" ? cJSON_GetObjectItemCaseSensitive(d, "setpoint_c") : nullptr;
        item.value = fmt_temp(c, unit);
        item.short_value = fmt_short(c, unit);
        item.unit = cJSON_IsNumber(c) ? kDeg + u : "";
        item.subs.push_back(str(d, "serial") + " " + std::to_string(static_cast<int>(num(d, "rssi"))) + "dBm");
    }
    if (item.stale) item.subs.push_back("stale " + std::to_string(static_cast<int>(age)) + "s");
    item.label = label;
    return item;
}

Item status_item(const cJSON *status, const std::string &wifi_mode, const std::string &ip,
                 const std::string &ap_ssid, const std::string &title, const std::vector<std::string> *lines)
{
    Item it;
    it.label = title;
    it.value = "";
    it.short_value = "";
    it.stale = false;
    it.status = true;
    it.accent = {120, 170, 255};
    auto &subs = it.subs;
    if (wifi_mode == "ap") {
        subs.push_back("Join WiFi " + ap_ssid);
        subs.push_back("http://" + ip + "/");
    } else if (!ip.empty()) {
        subs.push_back(ip);
    } else {
        subs.push_back("WiFi " + (wifi_mode.empty() ? std::string("off") : wifi_mode));
    }
    if (lines) {
        subs.insert(subs.end(), lines->begin(), lines->end());
        return it;
    }
    int ready = 0;
    const cJSON *l;
    cJSON_ArrayForEach(l, arr(status, "links")) if (str(l, "state") == "ready") ready++;
    subs.push_back("phones " + std::to_string(static_cast<int>(num(status, "inbound"))) + "  links " +
                   std::to_string(ready));
    subs.push_back("probes heard " + std::to_string(cJSON_GetArraySize(arr(status, "probes"))));
    const cJSON *bat = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(status, "power"), "battery");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(bat, "present")))
        subs.push_back(printf_str("battery %.2fV %d%%", num(bat, "v"), static_cast<int>(num(bat, "pct"))));
    return it;
}

std::vector<Item> build_items(const core::Settings &cfg, const cJSON *status, const std::string &wifi_mode,
                              const std::string &ip, const std::string &ap_ssid)
{
    std::vector<core::Reading> rd = cfg.readings.empty() ? default_readings(status) : cfg.readings;
    std::vector<Item> items;
    for (auto &r : rd) items.push_back(resolve(r, status, cfg.unit));
    if (wifi_mode == "ap" || cfg.status_page || items.empty()) {
        Item st = status_item(status, wifi_mode, ip, ap_ssid);
        if (wifi_mode == "ap") items.insert(items.begin(), st);
        else items.push_back(st);
    }
    return items;
}

std::vector<std::vector<Item>> pages(const core::Settings &cfg, const std::vector<Item> &items)
{
    std::vector<std::vector<Item>> out;
    if (cfg.layout == "quad") {
        std::vector<Item> temps, other;
        for (auto &i : items) (i.status ? other : temps).push_back(i);
        for (size_t k = 0; k < temps.size(); k += 4)
            out.emplace_back(temps.begin() + k, temps.begin() + std::min(k + 4, temps.size()));
        for (auto &i : other) out.push_back({i});
        return out;
    }
    for (auto &i : items) out.push_back({i});
    return out;
}

}  // namespace readings
