#include "cJSON.h"
#include "doctest.h"
#include "helpers.h"
#include "log_ring.h"
#include "status.h"

using namespace testing;

TEST_CASE("log ring: since, restart, cap")
{
    core::LogRing r(5);
    for (int i = 1; i <= 7; i++) r.push("line " + std::to_string(i) + "\n");
    cJSON *o = r.since(0, 2);
    CHECK(cJSON_GetObjectItem(o, "seq")->valuedouble == 7);
    cJSON *lines = cJSON_GetObjectItem(o, "lines");
    REQUIRE(cJSON_GetArraySize(lines) == 2);
    CHECK(std::string(cJSON_GetArrayItem(lines, 0)->valuestring) == "line 6");
    cJSON_Delete(o);
    o = r.since(4);                             // 5..7 left after the cap
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(o, "lines")) == 3);
    cJSON_Delete(o);
    o = r.since(99);                            // a seq from before a restart
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(o, "lines")) == 5);
    cJSON_Delete(o);
}

TEST_CASE("node status: probes, devices and links in the web page's shape")
{
    FakeTransport t;
    core::Settings cfg;
    cfg.linked_probes = {"04030201"};
    core::Node n(t, cfg, "ME00000001");
    Bytes probe("\x01\x02\x03\x04", 4);
    core::Observation o;
    o.body = probe_body(probe, std::vector<double>(8, 40));
    o.addr = Bytes(6, '\xAA');
    o.rssi = -50;
    t.direct[probe] = o;
    Bytes gauge = protocol::encode_serial("G000001CA8");
    core::Observation g;
    g.body = Bytes(1, char(protocol::PRODUCT_TYPE_GAUGE)) + gauge + Bytes(12, '\0');
    g.rssi = -60;
    t.nodes[gauge] = g;
    n.tick(1000);

    cJSON *s = core::node_status(n, 1000);
    cJSON *probes = cJSON_GetObjectItem(s, "probes");
    REQUIRE(cJSON_GetArraySize(probes) == 1);
    cJSON *p = cJSON_GetArrayItem(probes, 0);
    CHECK(std::string(cJSON_GetObjectItem(p, "serial")->valuestring) == "04030201");
    CHECK(cJSON_GetObjectItem(p, "core_c")->valuedouble == doctest::Approx(40).epsilon(0.01));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "linked")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "direct")));
    cJSON *devices = cJSON_GetObjectItem(s, "devices");
    REQUIRE(cJSON_GetArraySize(devices) == 1);
    cJSON *d = cJSON_GetArrayItem(devices, 0);
    CHECK(std::string(cJSON_GetObjectItem(d, "serial")->valuestring) == "G000001CA8");
    CHECK(std::string(cJSON_GetObjectItem(d, "name")->valuestring) == "Gauge");
    cJSON *links = cJSON_GetObjectItem(s, "links");
    REQUIRE(cJSON_GetArraySize(links) == 1);         // the link to the ticked probe
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(links, 0), "serial")->valuestring) == "04030201");
    CHECK(cJSON_GetObjectItem(s, "budget")->valuedouble >= 1);
    CHECK(cJSON_IsNumber(cJSON_GetObjectItem(cJSON_GetObjectItem(s, "stats"), "routed")));
    cJSON_Delete(s);

    std::vector<core::LinkSummary> ls = {{"probe", "04030201", "ready", 3, 100, R"({"connects": 1})"}};
    cJSON *f = core::field_status(t.direct, t.nodes, t.meaters, ls, 6, 1000);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(f, "probes")) == 1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(f, "devices")) == 1);
    cJSON *l0 = cJSON_GetArrayItem(cJSON_GetObjectItem(f, "links"), 0);
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(l0, "stats"), "connects")->valuedouble == 1);
    cJSON_Delete(f);
}
