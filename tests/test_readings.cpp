// Port of tests/test_readings.py.
#include <algorithm>

#include "cJSON.h"
#include "doctest.h"
#include "readings.h"

namespace r = readings;

static cJSON *status()
{
    return cJSON_Parse(R"({
        "probes": [{"serial": "1000ABCD", "id": 2, "color": 0, "mode": 0,
                    "core_c": 60.0, "surface_c": 70.0, "ambient_c": 120.0,
                    "instant_c": null, "temps": [60, 61, 62, 63, 64, 65, 66, 67],
                    "low_battery": false, "src": "link", "hops": 0,
                    "rssi": -60, "age_s": 1, "alarming": false,
                    "prediction": {"state": 3, "type": 1, "seconds": 3900, "setpoint_c": 63.0}}],
        "devices": [{"serial": "GAUGE00001", "ptype": 3, "name": "Gauge",
                     "temp_c": 110.0, "rssi": -70, "age_s": 30}],
        "links": [], "inbound": 0})");
}

static bool has_sub(const r::Item &i, const std::string &s)
{
    return std::any_of(i.subs.begin(), i.subs.end(), [&](const std::string &x) { return x.find(s) != std::string::npos; });
}

TEST_CASE("readings: formatting")
{
    CHECK(r::fmt_temp(60.0, "c") == "60.0");
    CHECK(r::fmt_temp(60.0, "f") == "140");
    CHECK(r::fmt_temp(nullptr, "c") == "--");
    CHECK(r::fmt_eta(3900) == "1h05");
    CHECK(r::fmt_eta(300) == "5m");
}

TEST_CASE("readings: default readings and a probe")
{
    cJSON *s = status();
    auto rd = r::default_readings(s);
    REQUIRE(rd.size() == 1);
    CHECK(rd[0].dev == "probe:1000ABCD");
    CHECK(rd[0].field == "core");
    r::Item item = r::resolve(rd[0], s, "c");
    CHECK(item.label == "P2 Core");
    CHECK(item.value == "60.0");
    CHECK(item.unit == "\xB0" "C");
    CHECK_FALSE(item.stale);
    CHECK(has_sub(item, "Ready in 1h05"));
    CHECK(r::resolve({"probe:1000ABCD", "eta", ""}, s, "c").value == "1h05");
    CHECK(r::resolve({"probe:1000ABCD", "t3", ""}, s, "c").value == "62.0");
    cJSON_Delete(s);
}

TEST_CASE("readings: node reading, stale")
{
    cJSON *s = status();
    r::Item item = r::resolve({"node:GAUGE00001", "temp", ""}, s, "f");
    CHECK(item.value == "230");
    CHECK(item.stale);
    CHECK(item.label == "Gauge");
    cJSON_Delete(s);
}

TEST_CASE("readings: missing device")
{
    cJSON *s = status();
    r::Item item = r::resolve({"probe:FFFFFFFF", "core", ""}, s, "c");
    CHECK(item.value == "--");
    CHECK(item.subs == std::vector<std::string>{"not found"});
    cJSON_Delete(s);
}

TEST_CASE("readings: pages for both layouts, setup network first")
{
    cJSON *s = status();
    core::Settings cfg;
    for (const char *f : {"core", "surface", "ambient", "t1", "t2"}) cfg.readings.push_back({"probe:1000ABCD", f, ""});
    auto items = r::build_items(cfg, s, "sta", "10.0.0.2", "");
    CHECK(r::pages(cfg, items).size() == 6);
    cfg.layout = "quad";
    auto ps = r::pages(cfg, items);
    REQUIRE(ps.size() == 3);
    CHECK(ps[0].size() == 4);
    CHECK(ps[1].size() == 1);
    CHECK(ps[2].size() == 1);
    auto ap = r::build_items(core::Settings(), s, "ap", "192.168.4.1", "MeatNet-0001");
    CHECK(ap[0].status);
    CHECK(has_sub(ap[0], "Join WiFi MeatNet-0001"));
    cJSON_Delete(s);
}
