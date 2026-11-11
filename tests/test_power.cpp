// Port of tests/test_power.py.
#include "doctest.h"
#include "helpers.h"
#include "power.h"

using namespace testing;

namespace {

struct Rig {
    FakeTransport t;
    core::Settings cfg;
    std::unique_ptr<core::Node> n;
    std::unique_ptr<core::PowerManager> p;
    double uv;
    bool saved = false;
    Rig(core::Settings c = core::Settings(), double uv_ = 1300000) : cfg(std::move(c)), uv(uv_)
    {
        n = std::make_unique<core::Node>(t, cfg, "ME00000001");
        core::PowerManager::Hooks h;
        h.read_uv = [this]() -> std::optional<double> { return uv; };
        h.persist = [this](bool on) { saved = on; };
        p = std::make_unique<core::PowerManager>(cfg, t, *n, h, nullptr, 0);
    }
};

const Bytes PROBE("\x01\x02\x03\x04", 4);

void hear_probe(FakeTransport &t, int64_t now)
{
    core::Observation o;
    o.body = probe_body(PROBE, std::vector<double>(8, 40));
    o.ticks = now;
    o.addr = Bytes(6, '\xAA');
    o.rssi = -50;
    t.direct[PROBE] = o;
}

}  // namespace

TEST_CASE("power: battery percent curve")
{
    CHECK(core::battery_percent(4.25) == 100);
    CHECK(core::battery_percent(3.80) == 52);
    CHECK(core::battery_percent(3.75) == 44);
    CHECK(core::battery_percent(3.0) == 0);
}

TEST_CASE("power: battery reading through the divider")
{
    Rig a;                                  // 1.3 V at the pin -> 3.9 V cell
    core::Battery b = a.p->read_battery();
    CHECK(b.present);
    CHECK(b.v == doctest::Approx(3.9));
    CHECK(b.pct == 66);
    Rig c(core::Settings(), 20000);
    CHECK_FALSE(c.p->read_battery().present);
}

TEST_CASE("power: manual standby drops links and is saved")
{
    core::Settings cfg;
    cfg.linked_probes = {"04030201"};
    Rig r(cfg);
    hear_probe(r.t, 0);
    r.n->tick(0);
    CHECK(r.t.links.count(PROBE));
    r.p->enter("manual", 0, false);
    CHECK(r.t.standby);
    CHECK(r.n->standby);
    r.n->tick(1000);
    CHECK(r.t.links.empty());
    r.p->tick(2000);                        // probe still heard: manual stays
    CHECK(r.p->standby);
    r.cfg.standby = true;
    r.p->exit("manual", 3000);
    CHECK_FALSE(r.t.standby);
    CHECK_FALSE(r.n->standby);
    CHECK_FALSE(r.cfg.standby);
    CHECK_FALSE(r.saved);
}

TEST_CASE("power: auto standby, woken by a probe")
{
    core::Settings cfg;
    cfg.auto_standby_min = 1;
    Rig r(cfg);
    r.p->last_probe = 0;
    r.p->tick(30000);
    CHECK_FALSE(r.p->standby);
    r.p->tick(61000);
    CHECK(r.p->standby);
    CHECK(r.p->reason == "auto");
    hear_probe(r.t, 70000);
    r.p->tick(71000);
    CHECK_FALSE(r.p->standby);
}
