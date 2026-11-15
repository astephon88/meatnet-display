#include "doctest.h"
#include "helpers.h"
#include "radio_source.h"
#include "util.h"

using namespace testing;

static const Bytes PROBE("\x01\x02\x03\x04", 4);

struct FakePeripheral : core::PeripheralPort {
    Bytes mac{"\x0A\x0B\x0C\x0D\x0E\x0F", 6};
    std::set<int> conns;
    std::vector<std::pair<int, Bytes>> rx;
    std::vector<Bytes> sent;
    std::vector<core::Transport::Advert> adv;
    bool standby = false;
    int rotations = 0;

    Bytes mac_address() override { return mac; }
    std::set<int> connections() override { return conns; }
    std::vector<std::pair<int, Bytes>> poll_rx() override { return std::move(rx); }
    void send(const Bytes &frame, const std::vector<int> &) override { sent.push_back(frame); }
    void set_advertising(const std::vector<core::Transport::Advert> &a, bool) override { adv = a; }
    void rotate() override { rotations++; }
    void set_firmware(const std::string &) override {}
    void set_standby(bool on) override { standby = on; }
    size_t txq_size() override { return 3; }
    uint32_t tx_dropped() override { return 7; }
};

TEST_CASE("local transport: peripheral only (app unit's local half)")
{
    FakePeripheral p;
    core::LocalTransport t(p);
    CHECK(t.mac == p.mac);
    p.conns = {4};
    p.rx = {{4, "hi"}};
    auto rx = t.poll_rx();
    REQUIRE(rx.size() == 1);
    CHECK(rx[0].second == "hi");
    CHECK(t.inbound == std::set<int>{4});
    t.send("frame", {4});
    CHECK(p.sent == std::vector<Bytes>{"frame"});
    t.set_advertising({{"msd", false}}, false);
    CHECK(p.adv.size() == 1);
    t.rotate_advertisement();
    CHECK(p.rotations == 1);
    CHECK(t.txq_size() == 3);
    CHECK(t.tx_dropped() == 7);
    // No radio of its own: links belong to the field unit.
    bool opened = t.open_link(PROBE, "probe", 0, Bytes(6, '\xAA')) != nullptr;
    CHECK_FALSE(opened);
    CHECK(t.links.empty());
    t.set_standby(true);
    CHECK(p.standby);
}

TEST_CASE("local transport: scan tables and links from the source")
{
    FakePeripheral p;
    FakeRadioSource src;
    core::LocalTransport t(p, &src);
    core::Observation o;
    o.body = "body";
    o.ticks = core::ticks_ms();
    o.rssi = -60;
    src.with([&] { src.t.direct[PROBE] = o; });
    t.poll_rx();
    REQUIRE(t.direct.count(PROBE));
    CHECK(t.direct[PROBE].rssi == -60);

    auto link = t.open_link(PROBE, "probe", 1, Bytes(6, '\xAA'));
    REQUIRE(link.get() != nullptr);
    CHECK(link->kind() == "probe");
    CHECK_FALSE(link->ready());
    CHECK_FALSE(link->write("early"));
    src.with([&] {
        src.links[PROBE]->state_ = "ready";
        src.links[PROBE]->info_["model"] = "probe";
        src.links[PROBE]->rx["status"].push_back("s1");
        src.links[PROBE]->rx["uart"].push_back("u1");
    });
    t.poll_rx();
    CHECK(link->ready());
    CHECK(link->mtu() == 185);
    CHECK(link->info().at("model") == "probe");
    CHECK(link->take("status") == std::vector<Bytes>{"s1"});
    CHECK(link->take("status").empty());
    CHECK(link->take("uart") == std::vector<Bytes>{"u1"});
    CHECK(link->write("cmd"));
    src.with([&] { CHECK(src.links[PROBE]->written == std::vector<Bytes>{"cmd"}); });

    t.set_scan_active(true);
    t.set_standby(true);
    src.with([&] {
        CHECK(src.scan_active);
        CHECK(src.standby);
    });
    CHECK(p.standby);

    t.close_link(PROBE);
    CHECK(t.links.empty());
    src.with([&] { CHECK(src.links.empty()); });
}

TEST_CASE("local transport: stale sightings are pruned")
{
    FakePeripheral p;
    FakeRadioSource src;
    core::LocalTransport t(p, &src);
    core::Observation o;
    o.ticks = core::ticks_ms() - 61000;
    src.with([&] { src.t.nodes["G000001CA8"] = o; });
    t.poll_rx();
    CHECK(t.nodes.empty());
}

TEST_CASE("link names")
{
    CHECK(core::link_name(PROBE, "probe") == "04030201");
    CHECK(core::link_name("G000001CA8", "node") == "G000001CA8");
    CHECK(core::link_name(Bytes("\xAA\xBB\xCC\xDD\xEE\xFF", 6), "meater") == "AA:BB:CC:DD:EE:FF");
}
