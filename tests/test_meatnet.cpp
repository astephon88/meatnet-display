// Port of tests/test_meatnet.py.
#include <algorithm>
#include <set>

#include "doctest.h"
#include "helpers.h"
#include "linktable.h"
#include "node.h"

namespace p = protocol;
using namespace testing;
using core::Node;
using core::Settings;

static const std::string SERIAL = "ME00000001";
static const Bytes PROBE("\x01\x02\x03\x04", 4);

// A Node on a fake transport, with a link table, like the Python make().
struct Rig {
    FakeTransport t;
    Settings cfg;
    core::LinkTable lt;
    std::vector<std::string> logs;
    std::unique_ptr<Node> n;
    explicit Rig(const Settings &c = Settings(), bool with_table = true) : cfg(c)
    {
        n = std::make_unique<Node>(t, cfg, SERIAL, [this](const std::string &m) { logs.push_back(m); },
                                   with_table ? &lt : nullptr);
    }
};

static void hear_probe(FakeTransport &t, int64_t now = 0)
{
    core::Observation o;
    o.body = probe_body(PROBE, std::vector<double>(8, 40));
    o.ticks = now;
    o.addr = Bytes(6, '\xAA');
    o.rssi = -50;
    t.direct[PROBE] = o;
}

static void hear_node(FakeTransport &t, const std::string &serial, int ptype)
{
    Bytes key = p::encode_serial(serial);
    core::Observation o;
    o.body = Bytes(1, char(ptype)) + key + Bytes(12, '\0');
    o.addr = Bytes(6, '\xBB');
    o.rssi = -60;
    t.nodes[key] = o;
}

static Settings linked_cfg()
{
    Settings c;
    c.linked_probes = {"04030201"};
    c.meatnet_peer = false;
    return c;
}

// A Node with a ready link to PROBE that has sent one status.
static std::unique_ptr<Rig> linked(int64_t now = 0)
{
    auto r = std::make_unique<Rig>(linked_cfg());
    hear_probe(r->t, now);
    r->n->tick(now);
    auto &link = r->t.link(PROBE);
    link.state_ = "ready";
    link.rx["status"] = {status100()};
    r->n->service(now);
    return r;
}

static std::vector<uint8_t> types_written(const FakeLink &l)
{
    std::vector<uint8_t> out;
    for (auto &w : l.written) out.push_back(one_frame(w).msg_type);
    return out;
}

TEST_CASE("advertising repeats a heard probe")
{
    Rig r;
    hear_probe(r.t);
    r.n->tick(100);
    REQUIRE(r.t.adv.size() >= 2);
    CHECK(r.t.adv[0].msd[2] == p::PRODUCT_TYPE_DISPLAY);
    CHECK(r.t.adv[1].msd[2] == p::PRODUCT_TYPE_NODE);
    CHECK(r.t.adv[1].msd.substr(3, 4) == PROBE);
    CHECK_FALSE(r.t.fast);
    CHECK(r.n->probes.at(PROBE).src == "advert");
}

TEST_CASE("links only selected probes")
{
    Rig r;
    hear_probe(r.t);
    r.n->tick(100);
    CHECK(r.t.links.empty());
    r.cfg.linked_probes = {"04030201"};
    r.n->tick(200);
    CHECK(r.t.links.count(PROBE));
}

TEST_CASE("status forwarded as 0x45, and the probe list")
{
    auto r = linked();
    auto fs = r->t.frames();
    int st = 0;
    for (auto &f : fs)
        if (f.msg_type == 0x45) {
            st++;
            CHECK(f.payload.substr(0, 4) == PROBE);
        }
    CHECK(st == 1);
    r->t.rx.push_back({1, p::build_request(0x44, "", 77)});
    r->n->service(10);
    fs = r->t.frames();
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].is_response);
    CHECK(fs[0].request_id == 77);
    CHECK(fs[0].payload[0] == 1);
    CHECK(fs[0].payload.substr(1, 4) == PROBE);
}

TEST_CASE("command translated to the probe, response back")
{
    auto r = linked();
    r->t.frames();
    auto &link = r->t.link(PROBE);
    r->t.rx.push_back({1, p::build_request(p::MSG_SET_PREDICTION, PROBE + B("\x8a\x02", 2), 1234)});
    r->n->service(10);
    REQUIRE(link.written.size() == 1);
    CHECK(link.written[0] == p::build_probe_request(p::MSG_SET_PREDICTION, B("\x8a\x02", 2)));
    link.rx["uart"] = {p::build_probe_response(p::MSG_SET_PREDICTION, true)};
    r->n->service(20);
    auto fs = r->t.frames();
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].is_response);
    CHECK(fs[0].msg_type == p::MSG_SET_PREDICTION);
    CHECK(fs[0].request_id == 1234);
    CHECK(fs[0].success);
    CHECK(fs[0].payload.empty());
}

TEST_CASE("duplicate request ignored")
{
    auto r = linked();
    Bytes req = p::build_request(p::MSG_RESET_FOOD_SAFE, PROBE, 55);
    r->t.rx.push_back({1, req});
    r->t.rx.push_back({1, req});
    r->n->service(10);
    CHECK(r->t.link(PROBE).written.size() == 1);
}

static Bytes log_rec(uint32_t seq)
{
    return p::build_probe_response(p::MSG_READ_LOGS, true, le32(seq) + Bytes(20, '\0'));
}

TEST_CASE("log read streams until the end")
{
    auto r = linked();
    r->t.frames();
    r->t.rx.push_back({1, p::build_request(p::MSG_READ_LOGS, PROBE + le32(0) + le32(2), 9)});
    r->n->service(10);
    Bytes recs = log_rec(0) + log_rec(1) + log_rec(2);
    r->t.link(PROBE).rx["uart"] = {recs.substr(0, 30), recs.substr(30)};
    r->n->service(20);
    auto fs = r->t.frames();
    REQUIRE(fs.size() == 3);
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(u32(fs[i].payload, 4) == i);
        CHECK(fs[i].payload.substr(0, 4) == PROBE);
        CHECK(fs[i].request_id == 9);
    }
    CHECK(r->n->pending.empty());
}

TEST_CASE("command timeout sends a failure")
{
    auto r = linked();
    r->t.frames();
    r->t.rx.push_back({1, p::build_request(p::MSG_RESET_THERMOMETER, PROBE, 3)});
    r->n->service(10);
    r->n->tick(10 + 7000);
    std::vector<p::Frame> fail;
    for (auto &f : r->t.frames())
        if (f.is_response) fail.push_back(f);
    REQUIRE_FALSE(fail.empty());
    CHECK(fail[0].request_id == 3);
    CHECK_FALSE(fail[0].success);
}

TEST_CASE("unknown probe not answered")
{
    auto r = linked();
    r->t.frames();
    r->t.rx.push_back({1, p::build_request(p::MSG_SET_PROBE_ID, B("\x09\x09\x09\x09\x01", 5), 4)});
    r->n->service(10);
    CHECK(r->t.frames().empty());
    CHECK(r->t.link(PROBE).written.empty());
}

TEST_CASE("global silence reaches the probe and the Display itself")
{
    auto r = linked();
    r->t.frames();
    auto &link = r->t.link(PROBE);
    r->t.rx.push_back({1, p::build_request(p::MSG_SILENCE_ALARMS, B("\x01\x00", 2) + Bytes(14, '\0'), 8)});
    r->n->service(10);
    REQUIRE(link.written.size() == 1);
    CHECK(link.written[0] == p::build_probe_request(p::MSG_SILENCE_ALARMS));
    std::vector<p::Frame> rs;
    for (auto &f : r->t.frames())
        if (f.is_response) rs.push_back(f);
    REQUIRE_FALSE(rs.empty());
    CHECK(rs[0].payload[0] == p::PRODUCT_TYPE_DISPLAY);
    link.rx["uart"] = {p::build_probe_response(p::MSG_SILENCE_ALARMS, true)};
    r->n->service(20);
    auto fs = r->t.frames();
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].payload[0] == 1);
    CHECK(fs[0].payload.substr(1, 4) == PROBE);
}

TEST_CASE("topology for self")
{
    auto r = linked();
    r->t.frames();
    r->t.rx.push_back({1, p::build_request(0x43, p::encode_serial(SERIAL), 5)});
    r->n->service(10);
    auto fs = r->t.frames();
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].payload[0] == 20);
    CHECK(fs[0].payload[1] == 4);
    CHECK(fs[0].payload[21] == 1);          // 1 outbound...
    CHECK(fs[0].payload[22] == 1);          // ...device #1
}

TEST_CASE("upstream status routes commands to the node")
{
    Settings c;
    c.upstream_node = {"BOOSTER001"};
    Rig r(c);
    Bytes key = p::encode_serial("BOOSTER001");
    hear_node(r.t, "BOOSTER001", 5);
    r.n->tick(0);
    auto &up = r.t.link(key);
    up.state_ = "ready";
    r.n->service(0);
    up.written.clear();
    Bytes other("\x0A\x0B\x0C\x0D", 4);
    up.rx["uart"] = {p::build_node_probe_status(other, status100(), 1, 42)};
    r.n->service(10);
    bool bumped = false;
    for (auto &f : r.t.frames())
        if (f.msg_type == 0x45 && f.payload[52] == 1) bumped = true;   // 2 hops
    CHECK(bumped);
    CHECK(r.n->via_node.count(other));
    r.t.rx.push_back({1, p::build_request(p::MSG_SET_PROBE_COLOR, other + Bytes(1, 2), 66)});
    r.n->service(20);
    REQUIRE_FALSE(up.written.empty());
    CHECK(one_frame(up.written.back()).request_id == 66);
    up.rx["uart"] = {p::build_response(p::MSG_SET_PROBE_COLOR, 66, true, "", 67)};
    r.n->service(30);
    std::vector<p::Frame> rs;
    for (auto &f : r.t.frames())
        if (f.is_response) rs.push_back(f);
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].request_id == 66);
}

TEST_CASE("link table merge substitutes our own serial")
{
    core::LinkTable lt;
    lt.merge({{4, "ME00000001", 1, false}}, true, SERIAL, "PEER000001");
    REQUIRE(lt.linked.size() == 1);
    CHECK(lt.linked[0].serial == "PEER000001");
    core::LinkTable lt2;
    lt2.merge({{4, "ME00000001", 1, false}}, true, SERIAL, "");
    CHECK(lt2.linked.empty());
}

TEST_CASE("link device and read it back")
{
    Rig r;
    Bytes entry = p::encode_product_serial(1, PROBE);
    r.t.rx.push_back({1, p::build_request(p::MSG_LINK_DEVICE, Bytes(1, 1) + entry + Bytes(4, '\0'), 11)});
    r.n->service(0);
    bool ok = false, note = false;
    for (auto &f : r.t.frames()) {
        if (f.is_response && f.request_id == 11 && f.success) ok = true;
        if (f.msg_type == p::MSG_DEVICE_LINKED && !f.is_response) note = true;
    }
    CHECK(ok);
    CHECK(note);
    r.t.rx.push_back({1, p::build_request(p::MSG_READ_LINKED_DEVICES, Bytes(1, 0), 12)});
    r.n->service(1);
    auto fs = r.t.frames();
    REQUIRE(fs.size() == 1);
    auto page = p::parse_link_entries_response(fs[0].payload);
    REQUIRE(page);
    REQUIRE_FALSE(page->entries.empty());
    CHECK(page->entries[0].serial == PROBE);
}

TEST_CASE("link table survives a save/load round trip")
{
    std::string saved;
    core::LinkTable lt;
    lt.saver = [&](const std::string &j) { saved = j; };
    lt.link({1, PROBE, 5, false});
    lt.unlink({3, "G000001CA8", 6, true});
    lt.proximity = true;
    lt.save();
    core::LinkTable back;
    back.load(saved);
    REQUIRE(back.linked.size() == 1);
    CHECK(back.linked[0].serial == PROBE);
    CHECK(back.linked[0].ts == 5);
    REQUIRE(back.unlinked.size() == 1);
    CHECK(back.unlinked[0].serial == "G000001CA8");
    CHECK(back.unlinked[0].legacy);
    CHECK(back.proximity);
}

TEST_CASE("overlapping log reads are matched by sequence")
{
    auto r = linked();
    r->t.frames();
    auto &link = r->t.link(PROBE);
    r->t.rx.push_back({1, p::build_request(p::MSG_READ_LOGS, PROBE + le32(0) + le32(5), 101)});
    r->t.rx.push_back({1, p::build_request(p::MSG_READ_LOGS, PROBE + le32(3) + le32(8), 102)});
    r->n->service(10);
    Bytes all;
    for (uint32_t s = 0; s < 6; s++) all += log_rec(s);       // the probe answers the
    for (uint32_t s = 3; s < 9; s++) all += log_rec(s);       // two reads in turn
    link.rx["uart"] = {all};
    r->n->service(20);
    std::vector<std::pair<uint32_t, uint32_t>> got, want;
    for (auto &f : r->t.frames())
        if (f.msg_type == p::MSG_READ_LOGS) got.push_back({f.request_id, u32(f.payload, 4)});
    for (uint32_t s = 0; s < 6; s++) want.push_back({101, s});
    for (uint32_t s = 3; s < 9; s++) want.push_back({102, s});
    CHECK(got == want);
    CHECK(r->n->pending.empty());
    link.rx["uart"] = {log_rec(4)};        // a stray record afterwards is ignored (and counted)
    r->n->service(30);
    CHECK(r->t.frames().empty());
    CHECK(r->n->stats["stray"] == 1);
}

TEST_CASE("probe device info answered from the link")
{
    auto r = linked();
    r->t.frames();
    r->t.link(PROBE).info_ = {{"firmware", "v2.4.0"}, {"hardware", "v2.16-A1"}, {"model", "00001011:20260527"}};
    struct Case {
        uint8_t mt;
        size_t size;
        std::string text;
    };
    for (const Case &c : {Case{0x46, 20, "v2.4.0"}, Case{0x47, 16, "v2.16-A1"},
                          Case{0x48, 50, "00001011:20260527"}}) {
        r->t.rx.push_back({1, p::build_request(c.mt, PROBE, 300 + c.mt)});
        r->n->service(10);
        auto fs = r->t.frames();
        REQUIRE(fs.size() == 1);
        CHECK(fs[0].success);
        CHECK(fs[0].payload.substr(0, 4) == PROBE);
        CHECK(fs[0].payload.size() == 4 + c.size);
        std::string text = fs[0].payload.substr(4);
        CHECK(text.substr(0, text.find('\0')) == c.text);
    }
}

TEST_CASE("node link associates, and pulls the link table once")
{
    Settings c;
    c.upstream_node = {"BOOSTER001"};
    c.upstream_push = true;
    Rig r(c);
    Bytes key = p::encode_serial("BOOSTER001");
    hear_node(r.t, "BOOSTER001", 5);
    r.n->tick(0);
    auto &up = r.t.link(key);
    up.state_ = "ready";
    r.n->service(0);
    CHECK(types_written(up) == std::vector<uint8_t>{p::MSG_ASSOCIATE_NODE, p::MSG_READ_LINKED_DEVICES,
                                                    p::MSG_READ_UNLINKED_DEVICES});
    uint32_t assoc = one_frame(up.written[0]).request_id;
    up.rx["uart"] = {p::build_response(p::MSG_ASSOCIATE_NODE, assoc, true, "", 5)};
    r.n->service(10);
    // reconnect: associates again, but doesn't re-pull the link table
    up.written.clear();
    up.state_ = "connecting";
    r.n->service(20);
    up.state_ = "ready";
    r.n->service(30);
    CHECK(types_written(up) == std::vector<uint8_t>{p::MSG_ASSOCIATE_NODE});
}

TEST_CASE("gauge log read replies route back to the phone")
{
    Settings c;
    c.upstream_node = {"G000001CA8"};
    Rig r(c);
    Bytes key = p::encode_serial("G000001CA8");
    hear_node(r.t, "G000001CA8", 3);
    r.n->tick(0);
    auto &up = r.t.link(key);
    up.state_ = "ready";
    r.n->service(0);
    up.written.clear();
    r.t.frames();
    r.t.rx.push_back({1, p::build_request(0x62, key + le32(0) + le32(2), 555)});
    r.n->service(10);
    REQUIRE_FALSE(up.written.empty());
    CHECK(one_frame(up.written.back()).request_id == 555);
    for (int i = 0; i < 3; i++)
        up.rx["uart"].push_back(p::build_response(0x62, 555, true, "r" + std::to_string(i), 900 + i));
    r.n->service(20);
    int back = 0;
    for (auto &f : r.t.frames())
        if (f.is_response && f.request_id == 555) back++;
    CHECK(back == 3);
}

TEST_CASE("the upstream node gets no unsolicited traffic by default")
{
    Settings c;
    c.upstream_node = {"G000001CA8"};
    c.linked_probes = {"04030201"};
    Rig r(c);
    Bytes key = p::encode_serial("G000001CA8");
    hear_node(r.t, "G000001CA8", 3);
    hear_probe(r.t);
    r.n->tick(0);
    auto &up = r.t.link(key);
    up.state_ = "ready";
    auto &plink = r.t.link(PROBE);
    plink.state_ = "ready";
    plink.rx["status"] = {status100()};
    bool hb_to_phones = false;
    for (int now = 0; now < 12000; now += 1000) {
        r.n->service(now);
        r.n->tick(now);
        for (auto &f : r.t.frames())
            if (f.msg_type == 0x49) hb_to_phones = true;
    }
    CHECK(up.written.empty());              // no 0x4A/0x73/0x45/0x49/0x4B
    CHECK(hb_to_phones);                    // phones still get heartbeats
    r.t.rx.push_back({1, p::build_request(0x62, key + le32(0) + le32(2), 7)});
    r.n->service(13000);
    CHECK(up.written.size() == 1);          // but phone requests go through
}

TEST_CASE("relay counters show the app using the Display route")
{
    Settings c;
    c.upstream_node = {"G000001CA8"};
    Rig r(c, false);
    Bytes key = p::encode_serial("G000001CA8");
    hear_node(r.t, "G000001CA8", 3);
    r.n->tick(0);
    auto &up = r.t.link(key);
    up.state_ = "ready";
    r.n->service(0);
    up.rx["uart"] = {p::build_request(0x60, key + Bytes(25, '\0'), 41)};   // gauge status
    r.n->service(5);
    r.t.rx.push_back({1, p::build_request(0x62, key + le32(0) + le32(1), 42)});
    r.n->service(10);
    up.rx["uart"] = {p::build_response(0x62, 42, true, "x", 43)};
    r.n->service(20);
    CHECK(r.n->via["app_to_node"] == std::map<std::string, int>{{"0x62", 1}});
    CHECK(r.n->via["node_to_app"] == std::map<std::string, int>{{"0x60", 1}, {"0x62 rsp", 1}});
    CHECK(std::any_of(r.logs.begin(), r.logs.end(), [](const std::string &m) {
        return m.find("app reached G000001CA8 through the Display (0x62)") != std::string::npos;
    }));
}

TEST_CASE("a log read waits for a slow start and holds other commands")
{
    auto r = linked();
    r->t.frames();
    r->t.rx.push_back({1, p::build_request(p::MSG_READ_LOGS, PROBE + le32(0) + le32(1), 201)});
    r->t.rx.push_back({1, p::build_request(p::MSG_READ_SESSION_INFO, PROBE, 202)});
    r->n->service(0);
    r->n->tick(9000);                       // probe still preparing its log after 9 s
    std::set<uint8_t> types;
    for (auto &q : r->n->pending) types.insert(q.type);
    CHECK(types == std::set<uint8_t>{p::MSG_READ_LOGS, p::MSG_READ_SESSION_INFO});
    for (auto &f : r->t.frames()) CHECK_FALSE(f.is_response);   // no premature failures
    r->t.link(PROBE).rx["uart"] = {log_rec(0) + log_rec(1) +
                                   p::build_probe_response(p::MSG_READ_SESSION_INFO, true, Bytes(6, 1))};
    r->n->service(9500);
    std::set<std::pair<uint8_t, uint32_t>> got;
    for (auto &f : r->t.frames())
        if (f.is_response) got.insert({f.msg_type, f.request_id});
    CHECK(got.count({p::MSG_READ_LOGS, 201}));
    CHECK(got.count({p::MSG_READ_SESSION_INFO, 202}));
    CHECK(r->n->pending.empty());
}

TEST_CASE("links to several upstream nodes and drops unticked ones")
{
    Bytes gauge = p::encode_serial("G000001CA8"), engine = p::encode_serial("E000002067");
    Settings c;
    c.upstream_node = {"G000001CA8", "E000002067"};
    Rig r(c);
    int budget = r.n->link_budget();
    hear_node(r.t, "G000001CA8", 3);
    hear_node(r.t, "E000002067", 6);
    r.n->tick(0);
    CHECK(r.t.link(gauge).kind() == "node");
    CHECK(r.t.link(engine).kind() == "node");
    r.t.link(gauge).state_ = "ready";
    r.t.link(engine).state_ = "ready";
    r.n->service(0);
    r.t.frames();
    // A request for another node goes out on both links, once each.
    r.t.rx.push_back({1, p::build_request(0x62, gauge + le32(0) + le32(2), 777)});
    r.n->service(10);
    for (auto *key : {&gauge, &engine}) {
        auto &l = r.t.link(*key);
        REQUIRE(l.written.size() == 1);
        CHECK(one_frame(l.written[0]).request_id == 777);
    }
    r.cfg.upstream_node = {"E000002067"};
    CHECK(r.n->link_budget() == budget + 1);
    r.n->tick(1000);
    CHECK_FALSE(r.t.links.count(gauge));
    CHECK(r.t.links.count(engine));
}

TEST_CASE("upstream serials from the setting")
{
    Settings c;
    c.upstream_node = {"G000001CA8"};
    Rig r(c);
    CHECK(r.n->upstream_serials() == std::vector<Bytes>{p::encode_serial("G000001CA8")});
}

TEST_CASE("a flooded message is not echoed back to the connection it came from")
{
    Rig r;
    r.t.inbound = {1, 2};
    // A peer node's heartbeat arrives on connection 1: it goes on to 2 only.
    Bytes hb = p::build_heartbeat("PEER000001", Bytes(6, 7), 4, false, {}, 0, 31);
    r.t.rx.push_back({1, hb});
    r.n->service(10);
    REQUIRE(r.t.sent.size() == 1);
    CHECK(r.t.sent[0].second == std::vector<int>{2});
    CHECK(one_frame(r.t.sent[0].first).payload[17] == 1);   // one hop further
}
