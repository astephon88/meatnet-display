// Port of tests/test_meater.py.
#include <algorithm>
#include <cmath>

#include "doctest.h"
#include "helpers.h"
#include "meater.h"
#include "node.h"
#include "proto.h"

namespace p = protocol;
namespace m = meater;
using namespace testing;
using core::Node;
using core::Settings;

static Bytes notification(const std::vector<double> &tips, double ambient)
{
    Bytes out;
    std::vector<double> all = tips;
    all.push_back(ambient);
    for (double t : all) out += le16(static_cast<unsigned>(std::lround(t * 32 - 4)));
    return out;
}

static m::Temps6 t6(const std::vector<double> &tips, double ambient)
{
    return {tips[0], tips[1], tips[2], tips[3], tips[4], ambient};
}

TEST_CASE("decode matches the receiver formula")
{
    Bytes data = le16(1916) + le16(1948) + le16(1980) + le16(2012) + le16(2044) + le16(3196);
    auto t = m::decode_temps(data);
    REQUIRE(t);
    CHECK((*t)[0] == (1916 + 4) / 32.0);
    CHECK((*t)[5] == (3196 + 4) / 32.0);
    CHECK_FALSE(m::decode_temps(Bytes(11, '\0')));
}

TEST_CASE("linear mapping: endpoints and ambient")
{
    auto t8 = m::map_sensors(t6({50, 52, 54, 56, 58}, 120));
    CHECK(t8[0] == 50.0);
    CHECK(t8[6] == 58.0);
    CHECK(t8[7] == 120.0);
    for (int i = 0; i < 6; i++) CHECK(std::fabs((t8[i + 1] - t8[i]) - 8.0 / 6) < 1e-6);   // even steps
    CHECK(std::fabs(t8[3] - 54.0) < 1e-9);  // T4 sits exactly on tip 3
}

TEST_CASE("core is the lowest of T1..T6")
{
    CHECK(m::core_index({60, 55, 50, 52, 58, 62, 40, 120}) == 2);   // T7 lowest but not allowed
}

TEST_CASE("advert round-trips through the Combustion decoder")
{
    Bytes addr;
    REQUIRE(proto::unhex("A1B2C3D4E5F6", addr));
    Bytes serial = m::serial_for(addr);
    CHECK(p::probe_serial_str(serial) == "4DD4E5F6");
    auto t8 = m::map_sensors(t6({62.0, 58.5, 55.0, 57.0, 61.0}, 130.0));
    auto adv = m::build_advert(serial, t8, 7, 0, true);
    CHECK(adv.first.substr(0, 2) == p::VENDOR_LE);
    CHECK(adv.first.size() == 24);
    auto d = p::parse_probe_advert(adv.first.substr(2));
    REQUIRE(d);
    CHECK(d->ptype == p::PRODUCT_TYPE_NODE);
    CHECK(d->hops == 1);
    CHECK(d->serial == serial);
    CHECK(d->probe_id == 7);
    CHECK(d->low_battery);
    CHECK(d->core_sensor == adv.second + 1);
    CHECK(std::fabs(*d->core_c - *std::min_element(t8.begin(), t8.begin() + 6)) < 0.05);
    CHECK(std::fabs(*d->surface_c - t8[6]) < 0.05);
    CHECK(std::fabs(*d->ambient_c - 130.0) < 0.05);
}

TEST_CASE("adv_match by name and by service UUID")
{
    Bytes name = "MEATER Pro";
    Bytes adv = Bytes{2, 1, 6, char(name.size() + 1), 0x09} + name;
    CHECK(m::adv_match(adv) == std::optional<std::string>("MEATER Pro"));
    for (const char *u : m::SERVICE_UUIDS) {
        uint8_t le[16];
        REQUIRE(proto::uuid128_le(u, le));
        CHECK(m::adv_match(Bytes{17, 0x07} + Bytes(reinterpret_cast<char *>(le), 16)) ==
              std::optional<std::string>(""));
    }
    CHECK_FALSE(m::adv_match(Bytes{2, 1, 6, 4, 0x09} + "Foo"));
}

TEST_CASE("encode clamps and flags overheating")
{
    auto r = m::encode_raw13({500.0, 20, 20, 20, 20, 20, 20, 20});
    CHECK(r.second == 1);
    CHECK(std::fabs(p::unpack_probe_temps(r.first)[0] - (0x1FFF * 0.05 - 20)) < 1e-6);
}

TEST_CASE("status matches the probe status layout")
{
    auto t8 = m::map_sensors(t6({60, 57, 54, 56, 59}, 140));
    auto f = m::encode_fields(t8, 3);
    Bytes st = m::build_status(f, 5, 42, 0xA1B2C3D4, 5000);
    CHECK(st.size() == 100);
    auto d = p::decode_probe_status(st);
    REQUIRE(d);
    CHECK(d->log_min == 5);
    CHECK(d->log_max == 42);
    CHECK(d->probe_id == 3);
    CHECK(std::fabs(*d->core_c - 54.0) < 0.05);
    CHECK(d->session_id == 0xA1B2C3D4);
    CHECK(d->sample_period == 5000);
    CHECK(d->prediction.state == 1);
    CHECK(std::fabs(d->prediction.est_core_c - 54.0) < 0.06);
    Bytes rec = m::log_record(f);
    CHECK(rec.size() == 20);
    CHECK(rec.substr(0, 13) == f.raw13);
}

TEST_CASE("direct probe advert layout")
{
    Bytes addr;
    REQUIRE(proto::unhex("A1B2C3D4E5F6", addr));
    auto t8 = m::map_sensors(t6({60, 57, 54, 56, 59}, 140));
    Bytes msd = m::build_direct_advert(m::serial_for(addr), t8, 4);
    CHECK(msd.substr(0, 2) == p::VENDOR_LE);
    CHECK(msd.size() == 25);                // like a real probe
    CHECK(msd.size() + 2 + 3 <= 31);        // fits with flags
    auto d = p::parse_probe_advert(msd.substr(2));
    REQUIRE(d);
    CHECK(d->ptype == p::PRODUCT_TYPE_PROBE);
    CHECK(d->hops == 0);
    CHECK(d->probe_id == 4);
    CHECK(std::fabs(*d->core_c - 54.0) < 0.05);
}

// -- bridge behaviour in the node --------------------------------------------------------

static Bytes ADDR()
{
    Bytes a;
    proto::unhex("A1B2C3D4E5F6", a);
    return a;
}

struct Bridge {
    FakeTransport t;
    Settings cfg;
    core::LinkTable lt;
    std::unique_ptr<Node> n;
    explicit Bridge(bool with_table = false, void (*tweak)(Settings &) = nullptr)
    {
        cfg.meater_bridge = true;
        cfg.meater_probes = {{m::addr_str(ADDR()), 5, 0}};
        if (tweak) tweak(cfg);
        n = std::make_unique<Node>(t, cfg, "ME00000001", nullptr, with_table ? &lt : nullptr);
        core::Observation o;
        o.name = "MEATER Pro";
        o.addr_type = 1;
        o.rssi = -58;
        t.meaters[ADDR()] = o;
        n->tick(0);
    }
    FakeLink &link() { return t.link(ADDR()); }
    void ready() { link().state_ = "ready"; }
    void feed(int64_t now, double core = 54.0)
    {
        link().rx["temp"] = {notification({60.0, 57.0, core, 56.0, 59.0}, 140.0)};
        n->service(now);
    }
};

TEST_CASE("bridge scans actively and links the selected MEATER")
{
    Bridge b;
    CHECK(b.t.scan_active);
    CHECK(b.link().kind() == "meater");
    b.cfg.meater_bridge = false;
    b.n->tick(100);
    CHECK_FALSE(b.t.links.count(ADDR()));
    CHECK_FALSE(b.t.scan_active);
}

TEST_CASE("a bridged MEATER is advertised and listed")
{
    Bridge b;
    b.ready();
    b.link().rx["temp"] = {notification({60.0, 57.0, 54.0, 56.0, 59.0}, 140.0)};
    b.link().rx["battery"] = {Bytes(1, 15)};   // 15% -> low battery
    b.n->service(1000);
    b.n->tick(1500);
    Bytes serial = m::serial_for(ADDR());
    std::vector<Bytes> ours;
    for (auto &a : b.t.adv)
        if (!a.probe && a.msd.substr(3, 4) == serial) ours.push_back(a.msd);
    REQUIRE(ours.size() == 1);
    auto d = p::parse_probe_advert(ours[0].substr(2));
    REQUIRE(d);
    CHECK(d->probe_id == 5);
    CHECK(d->low_battery);
    CHECK(std::fabs(*d->ambient_c - 140.0) < 0.05);
    auto &probe = b.n->probes.at(serial);
    CHECK(probe.src == "meater");
    CHECK(std::fabs(*probe.core_c - 54.0) < 0.05);
    CHECK(probe.battery_pct == std::optional<int>(15));
    // goes quiet -> stops being advertised
    b.n->tick(1000 + 11000);
    for (auto &a : b.t.adv) CHECK(a.msd.substr(3, 4) != serial);
}

TEST_CASE("a MEATER shares the link budget with probes")
{
    Bridge b(false, [](Settings &c) {
        c.linked_probes = {"04030201"};
        c.inbound_reserve = 3;
    });
    core::Observation o;
    o.body = probe_body(Bytes("\x01\x02\x03\x04", 4), std::vector<double>(8, 40));
    o.addr = Bytes(6, '\xAA');
    o.rssi = -50;
    b.t.direct[Bytes("\x01\x02\x03\x04", 4)] = o;
    b.t.links.clear();
    b.n->tick(10);
    CHECK(b.t.links.size() == 1);           // budget 1: the probe wins, the MEATER waits
}

TEST_CASE("a MEATER appears as a node probe with a status")
{
    Bridge b;
    b.ready();
    b.t.frames();
    b.feed(1000);
    Bytes serial = m::serial_for(ADDR());
    std::vector<p::Frame> st;
    for (auto &f : b.t.frames())
        if (f.msg_type == p::MSG_PROBE_STATUS) st.push_back(f);
    REQUIRE(st.size() == 1);
    CHECK(st[0].payload.substr(0, 4) == serial);
    CHECK(st[0].payload.size() == 105);
    auto ns = p::parse_node_probe_status(st[0].payload);
    REQUIRE(ns);
    auto d = p::decode_probe_status(ns->status);
    REQUIRE(d);
    CHECK(ns->hops == 1);
    CHECK(std::fabs(*d->core_c - 54.0) < 0.05);
    CHECK(d->session_id != 0);
    b.t.rx.push_back({1, p::build_request(p::MSG_READ_PROBE_LIST, "", 5)});
    b.n->service(1100);
    auto fs = b.t.frames();
    REQUIRE(fs.size() == 1);
    bool listed = false;
    for (size_t i = 0; i < 50; i += 5)
        if (fs[0].payload.substr(i + 1, 4) == serial) listed = true;
    CHECK(listed);
}

TEST_CASE("session info and a paced log read")
{
    Bridge b;
    b.ready();
    for (int k = 0; k < 30; k++) b.feed(1000 + k * 5000, 40.0 + k);   // 30 samples, 5 s apart
    b.t.frames();
    Bytes serial = m::serial_for(ADDR());
    b.t.rx.push_back({1, p::build_request(p::MSG_READ_SESSION_INFO, serial, 6)});
    b.n->service(146000);
    auto fs = b.t.frames();
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].success);
    CHECK(fs[0].payload.substr(0, 4) == serial);
    CHECK(u32(fs[0].payload, 4) != 0);
    CHECK((uint8_t(fs[0].payload[8]) | uint8_t(fs[0].payload[9]) << 8) == 5000);
    b.t.rx.push_back({1, p::build_request(p::MSG_READ_LOGS, serial + le32(0) + le32(29), 7)});
    b.n->service(146100);
    std::vector<p::Frame> recs;
    for (auto &f : b.t.frames())
        if (f.msg_type == p::MSG_READ_LOGS) recs.push_back(f);
    CHECK(recs.size() == 20);               // paced: one burst per service()
    b.n->service(146200);
    for (auto &f : b.t.frames())
        if (f.msg_type == p::MSG_READ_LOGS) recs.push_back(f);
    REQUIRE(recs.size() == 30);
    for (uint32_t i = 0; i < 30; i++) {
        CHECK(u32(recs[i].payload, 4) == i);
        CHECK(recs[i].request_id == 7);
        CHECK(recs[i].payload.substr(0, 4) == serial);
    }
    auto temps = p::unpack_probe_temps(recs[29].payload.substr(8, 13));
    CHECK(std::fabs(temps[3] - 69.0) < 0.06);   // T4 = tip 3 of the last sample
}

TEST_CASE("unsupported commands fail; Set Probe ID applies")
{
    Bridge b;
    b.ready();
    b.feed(1000);
    b.t.frames();
    Bytes serial = m::serial_for(ADDR());
    b.t.rx.push_back({1, p::build_request(p::MSG_SET_PREDICTION, serial + B("\x8a\x02", 2), 8)});
    b.t.rx.push_back({1, p::build_request(p::MSG_SET_PROBE_ID, serial + Bytes(1, 2), 9)});
    b.n->service(1100);
    std::map<uint32_t, p::Frame> rs;
    for (auto &f : b.t.frames())
        if (f.is_response) rs[f.request_id] = f;
    REQUIRE(rs.count(8));
    REQUIRE(rs.count(9));
    CHECK_FALSE(rs[8].success);
    CHECK(rs[9].success);
    CHECK(b.cfg.meater_probes[0].id == 3);
}

TEST_CASE("device info answers and the link announcement")
{
    Bridge b(true);
    b.ready();
    b.t.frames();
    b.feed(1000);
    Bytes serial = m::serial_for(ADDR());
    std::vector<p::Frame> linked;
    for (auto &f : b.t.frames())
        if (f.msg_type == p::MSG_DEVICE_LINKED) linked.push_back(f);
    REQUIRE(linked.size() == 1);
    CHECK(p::parse_link_device(linked[0].payload)->serial == serial);
    REQUIRE_FALSE(b.lt.linked.empty());
    CHECK(b.lt.linked[0].serial == serial);
    b.feed(2000);                           // announced once only
    for (auto &f : b.t.frames()) CHECK(f.msg_type != p::MSG_DEVICE_LINKED);
    struct Case {
        uint8_t mt;
        size_t size;
        std::string text;
    };
    for (const Case &c : {Case{0x46, 20, "v2.4.0"}, Case{0x47, 16, "v2.16-A1"},
                          Case{0x48, 50, "00001011:MEATER"}}) {
        b.t.rx.push_back({1, p::build_request(c.mt, serial, 100 + c.mt)});
        b.n->service(2100);
        auto fs = b.t.frames();
        REQUIRE(fs.size() == 1);
        CHECK(fs[0].is_response);
        CHECK(fs[0].success);
        CHECK(fs[0].payload.substr(0, 4) == serial);
        CHECK(fs[0].payload.size() == 4 + c.size);
        std::string text = fs[0].payload.substr(4);
        CHECK(text.substr(0, text.find('\0')) == c.text);
    }
}

TEST_CASE("the rotation includes a non-connectable direct advert")
{
    Bridge b;
    b.ready();
    b.feed(1000);
    b.n->tick(1500);
    std::vector<core::Transport::Advert> direct;
    for (auto &a : b.t.adv)
        if (a.probe) direct.push_back(a);
    REQUIRE(direct.size() == 1);
    CHECK(direct[0].msd[2] == p::PRODUCT_TYPE_PROBE);
    b.cfg.meater_direct_adverts = false;
    b.n->tick(1600);
    for (auto &a : b.t.adv) CHECK_FALSE(a.probe);
}
