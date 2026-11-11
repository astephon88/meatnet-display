// Port of tests/test_split.py (the field status view comes with the web UI).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>

#include "cJSON.h"
#include "doctest.h"
#include "helpers.h"
#include "meater.h"
#include "node.h"
#include "proto.h"
#include "split.h"

namespace p = protocol;
using namespace testing;
using core::Node;
using core::Settings;

static const Bytes PROBE("\x01\x02\x03\x04", 4);

static std::string hexs(const Bytes &b) { return proto::hex(b); }

// cJSON message from a line, deleted with the holder.
struct Msg {
    cJSON *m;
    explicit Msg(const std::string &line) : m(split::decode(line)) {}
    ~Msg() { cJSON_Delete(m); }
    std::string str(const char *k) const
    {
        cJSON *v = cJSON_GetObjectItem(m, k);
        return cJSON_IsString(v) ? v->valuestring : "";
    }
    double num(const char *k) const { return cJSON_GetObjectItem(m, k)->valuedouble; }
};

// The field unit's radio, backed by fake links; locked, as the split client
// reads it from its own thread.
struct FakeFieldSource : split::FieldSource {
    std::mutex mu;
    Tables t;
    std::map<Bytes, std::shared_ptr<FakeLink>> links;
    std::vector<Bytes> removed;
    bool scan_active = false, standby = false;
    Bytes ignore_mac;

    template <class F> void with(F fn)
    {
        std::lock_guard<std::mutex> g(mu);
        fn();
    }
    Tables tables() override
    {
        std::lock_guard<std::mutex> g(mu);
        return t;
    }
    void prune(int64_t now, int64_t max_age) override
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto *tb : {&t.direct, &t.repeated, &t.nodes, &t.meaters})
            for (auto it = tb->begin(); it != tb->end();)
                it = now - it->second.ticks > max_age ? tb->erase(it) : std::next(it);
    }
    std::vector<split::LinkReport> collect_links() override
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<split::LinkReport> out;
        for (auto &kv : links) {
            split::LinkReport r;
            r.key = kv.first;
            r.kind = kv.second->kind();
            r.state = kv.second->state();
            r.mtu = kv.second->mtu();
            r.info = kv.second->info_;
            if (kv.second->ready())
                for (auto &sub : split::link_subs(r.kind))
                    for (auto &d : kv.second->take(sub)) r.rx.push_back({sub, d});
            out.push_back(r);
        }
        return out;
    }
    std::vector<Bytes> take_removed_links() override
    {
        std::lock_guard<std::mutex> g(mu);
        return std::move(removed);
    }
    void open_link(const Bytes &key, const std::string &kind, int, const Bytes &) override
    {
        std::lock_guard<std::mutex> g(mu);
        auto &l = links[key];
        if (!l) l = std::make_shared<FakeLink>(kind);
    }
    void close_link(const Bytes &key) override
    {
        std::lock_guard<std::mutex> g(mu);
        if (links.erase(key)) removed.push_back(key);
    }
    void write_link(const Bytes &key, const Bytes &data) override
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = links.find(key);
        if (it != links.end() && it->second->ready()) it->second->write(data);
    }
    void close_all_links() override
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto &kv : links) removed.push_back(kv.first);
        links.clear();
    }
    void set_scan_active(bool on) override
    {
        std::lock_guard<std::mutex> g(mu);
        scan_active = on;
    }
    void set_standby(bool on) override
    {
        std::lock_guard<std::mutex> g(mu);
        standby = on;
    }
    // As the radios do: forget what was collected from the app unit.
    void set_ignore(const Bytes &mac, const Bytes &serial10) override
    {
        std::lock_guard<std::mutex> g(mu);
        ignore_mac = mac;
        if (!mac.empty()) {
            for (auto *tb : {&t.direct, &t.nodes})
                for (auto it = tb->begin(); it != tb->end();)
                    it = it->second.addr == mac ? tb->erase(it) : std::next(it);
            t.meaters.erase(mac);
            t.repeated.clear();     // can't tell whose: refills within seconds
        }
        if (!serial10.empty()) t.nodes.erase(serial10);
    }
};

static core::Observation obs(const Bytes &body, int64_t ticks, int at, const Bytes &addr, int rssi)
{
    core::Observation o;
    o.body = body;
    o.ticks = ticks;
    o.addr_type = at;
    o.addr = addr;
    o.rssi = rssi;
    return o;
}

TEST_CASE("encode/decode and base64")
{
    CHECK(split::b64(B("\x00\xff", 2)) == "AP8=");
    CHECK(split::b64("") == "");
    CHECK(split::b64("abc") == "YWJj");
    CHECK(split::b64("ab") == "YWI=");
    for (size_t n = 0; n < 40; n++) {           // round trip every padding case
        Bytes b;
        for (size_t i = 0; i < n; i++) b += char(i * 37 + 11);
        CHECK(split::unb64(split::b64(b)) == b);
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "rx");
    cJSON_AddStringToObject(m, "data", split::b64(B("\x00\xff", 2)).c_str());
    std::string line = split::encode(m);
    CHECK(line.back() == '\n');
    Msg back(line);
    REQUIRE(back.m);
    CHECK(back.str("t") == "rx");
    CHECK(split::unb64(back.str("data")) == B("\x00\xff", 2));
    CHECK(split::decode("not json") == nullptr);
    CHECK(split::decode("[1]") == nullptr);
}

TEST_CASE("field radio: scan deltas and link messages")
{
    FakeFieldSource src;
    split::FieldRadio fr(src);
    int64_t now = 1000000;
    Bytes body = probe_body(PROBE, std::vector<double>(8, 40));
    src.t.direct[PROBE] = obs(body, now, 1, Bytes(6, '\xAA'), -50);
    Msg m(fr.scan_message(now + 10));
    REQUIRE(m.m);
    cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItem(m.m, "d"), 0);
    CHECK(std::string(cJSON_GetArrayItem(row, 0)->valuestring) == hexs(PROBE));
    CHECK(cJSON_GetArrayItem(row, 2)->valuedouble == 10);
    CHECK(split::unb64(cJSON_GetArrayItem(row, 1)->valuestring) == body);
    CHECK(fr.scan_message(now + 20).empty());   // unchanged: nothing sent
    fr.apply("{\"t\":\"open\",\"k\":\"" + hexs(PROBE) + "\",\"kind\":\"probe\",\"at\":1,\"addr\":\"aaaaaaaaaaaa\"}");
    auto &link = *src.links.at(PROBE);
    link.state_ = "ready";
    link.rx["status"] = {Bytes(100, 'S')};
    auto msgs = fr.link_messages();
    REQUIRE(msgs.size() == 2);
    Msg l0(msgs[0]), l1(msgs[1]);
    CHECK(l0.str("t") == "link");
    CHECK(l0.str("state") == "ready");
    CHECK(l1.str("t") == "rx");
    CHECK(l1.str("k") == hexs(PROBE));
    CHECK(l1.str("sub") == "status");
    CHECK(split::unb64(l1.str("data")) == Bytes(100, 'S'));
    fr.apply("{\"t\":\"write\",\"k\":\"" + hexs(PROBE) + "\",\"data\":\"" + split::b64("cmd") + "\"}");
    CHECK(link.written == std::vector<Bytes>{"cmd"});
    fr.apply("{\"t\":\"close\",\"k\":\"" + hexs(PROBE) + "\"}");
    msgs = fr.link_messages();
    REQUIRE(msgs.size() == 1);
    CHECK(Msg(msgs[0]).str("state") == "closed");
}

TEST_CASE("remote radio mirrors the scan tables and proxies links")
{
    split::RemoteRadio r;
    r.on_connect("{\"t\":\"hello\",\"max_conn\":3,\"serial\":\"F1\"}", "");
    Bytes body = probe_body(PROBE, std::vector<double>(8, 40));
    r.apply("{\"t\":\"scan\",\"d\":[[\"" + hexs(PROBE) + "\",\"" + split::b64(body) +
                "\",100,1,\"aaaaaaaaaaaa\",-55]]}",
            5000);
    auto &o = r.direct.at(PROBE);
    CHECK(o.body == body);
    CHECK(o.ticks == 4900);
    CHECK(o.addr_type == 1);
    CHECK(o.addr == Bytes(6, '\xAA'));
    CHECK(o.rssi == -55);
    CHECK(r.max_conn == 3);
    r.take_outbox();
    auto link = r.open_link(PROBE, "probe", 1, Bytes(6, '\xAA'));
    auto out = r.take_outbox();
    REQUIRE(out.size() == 1);
    CHECK(Msg(out[0]).str("t") == "open");
    CHECK_FALSE(link->write("x"));              // not ready yet
    r.apply("{\"t\":\"link\",\"k\":\"" + hexs(PROBE) + "\",\"state\":\"ready\",\"desc\":\"ok\",\"mtu\":185}", 0);
    r.apply("{\"t\":\"rx\",\"k\":\"" + hexs(PROBE) + "\",\"sub\":\"uart\",\"data\":\"" + split::b64("u") + "\"}", 0);
    CHECK(link->ready());
    CHECK(link->take("uart") == std::vector<Bytes>{"u"});
    CHECK(static_cast<split::RemoteLink &>(*link).describe() == "ok");
    CHECK(link->mtu() == 185);
    CHECK(link->write("x"));
    out = r.take_outbox();
    REQUIRE(out.size() == 1);
    CHECK(Msg(out[0]).str("t") == "write");
    r.on_disconnect();
    CHECK(link->state() == "error: field unit offline");
    r.on_connect("{\"t\":\"hello\",\"max_conn\":3}", "");
    bool reopened = false;
    for (auto &l : r.take_outbox()) reopened |= Msg(l).str("t") == "open";
    CHECK(reopened);
}

TEST_CASE("remote link keeps the field unit's stats")
{
    split::RemoteRadio r;
    r.on_connect("{\"t\":\"hello\",\"max_conn\":6}", "");
    auto link = r.open_link(PROBE, "node", 0, Bytes(6, 1));
    r.apply("{\"t\":\"link\",\"k\":\"" + hexs(PROBE) +
                "\",\"state\":\"ready\",\"desc\":\"\",\"mtu\":177,\"stats\":{\"connects\":3,"
                "\"drops\":{\"supervision timeout (signal lost) (0x08)\":2},\"params\":\"50.0ms/0/8000ms\"}}",
            0);
    CHECK(static_cast<split::RemoteLink &>(*link).stats_ ==
          "{\"connects\":3,\"drops\":{\"supervision timeout (signal lost) (0x08)\":2},"
          "\"params\":\"50.0ms/0/8000ms\"}");
}

TEST_CASE("the field unit ignores the app unit's own adverts")
{
    FakeFieldSource src;
    split::FieldRadio fr(src);
    int64_t now = 1000000;
    Bytes app_mac;
    proto::unhex("4C796ED5B3E8", app_mac);
    Bytes own_display = p::build_display_advertisement("APP0000001").substr(2);
    src.t.nodes[p::encode_serial("APP0000001")] = obs(own_display, now, 0, app_mac, -40);
    meater::Temps8 t8;
    t8.fill(50.0);
    Bytes fake_meater = meater::build_direct_advert(B("\x01\x02\x03\x4d", 4), t8).substr(2);
    src.t.direct[B("\x01\x02\x03\x4d", 4)] = obs(fake_meater, now, 0, app_mac, -40);
    src.t.repeated[PROBE] = obs(probe_body(PROBE, std::vector<double>(8, 40)), now, 0, Bytes(), -40);
    Bytes real("\x09\x09\x09\x10", 4);
    src.t.direct[real] = obs(probe_body(real, std::vector<double>(8, 40)), now, 0, Bytes(6, '\xC2'), -70);
    fr.set_ignore(app_mac, "APP0000001");
    CHECK(src.ignore_mac == app_mac);
    Msg m(fr.scan_message(now));
    REQUIRE(m.m);
    cJSON *d = cJSON_GetObjectItem(m.m, "d");
    REQUIRE(cJSON_GetArraySize(d) == 1);
    CHECK(std::string(cJSON_GetArrayItem(cJSON_GetArrayItem(d, 0), 0)->valuestring) == hexs(real));
    CHECK_FALSE(cJSON_GetObjectItem(m.m, "n"));
    CHECK_FALSE(cJSON_GetObjectItem(m.m, "r"));
    // a Display advert with our serial is dropped even from another address
    src.t.nodes[p::encode_serial("APP0000001")] = obs(own_display, now + 1, 0, Bytes(6, 1), -40);
    CHECK(fr.scan_message(now + 2).empty());
    // and the app side never mirrors its own serial either
    split::RemoteRadio r;
    r.own_serial = p::encode_serial("APP0000001");
    r.apply("{\"t\":\"scan\",\"n\":[[\"" + hexs(p::encode_serial("APP0000001")) + "\",\"" +
                split::b64(own_display) + "\",0,0,\"010101010101\",-40]]}",
            1000);
    CHECK(r.nodes.empty());
}

// -- over TCP ---------------------------------------------------------------------------------

static uint16_t free_port()
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<struct sockaddr *>(&a), sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(s, reinterpret_cast<struct sockaddr *>(&a), &l);
    close(s);
    return ntohs(a.sin_port);
}

// Runs node ticks until cond() holds (5 s at most).
static bool wait_for(const std::function<bool()> &cond, Node *node = nullptr, int timeout_ms = 5000)
{
    auto t0 = std::chrono::steady_clock::now();
    while (!cond()) {
        if (node) {
            node->tick();
            node->service();
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

static bool sent_has(FakeTransport &local, const std::function<bool(const p::Frame &)> &pred)
{
    for (auto &s : local.sent)
        for (auto &f : testing::split(s.first))
            if (pred(f)) return true;
    return false;
}

TEST_CASE("end to end over TCP")
{
    uint16_t port = free_port();
    Settings app_cfg;
    app_cfg.meatnet_peer = false;
    FakeTransport local;
    split::RemoteRadio remote(&local);
    Node node(remote, app_cfg, "APP0000001");
    split::ServerConfig scfg;
    scfg.key = "k";
    scfg.serial = "APP0000001";
    scfg.port = port;
    scfg.bind = "127.0.0.1";
    scfg.beacon = false;
    split::SplitServer server(remote, scfg);
    std::thread srv([&] { server.run(); });

    FakeFieldSource fsrc;
    split::FieldRadio fradio(fsrc);
    split::ClientConfig ccfg;
    ccfg.peer = "127.0.0.1";
    ccfg.key = "k";
    ccfg.port = port;
    ccfg.serial = "FIELD00001";
    auto client = std::make_unique<split::SplitClient>(fradio, ccfg);
    std::thread cli([&] { client->run(); });

    auto field_has_link = [&](const Bytes &k) {
        bool has = false;
        fsrc.with([&] { has = fsrc.links.count(k) > 0; });
        return has;
    };

    CHECK(wait_for([&] { return remote.connected && client->connected(); }, &node));
    CHECK(node.link_budget() == 4);         // separate radios: no reserve

    // A probe heard by the field unit is repeated by the app unit.
    fsrc.with([&] {
        fsrc.t.direct[PROBE] = obs(probe_body(PROBE, std::vector<double>(8, 40)), core::ticks_ms(), 1,
                                   Bytes(6, '\xAA'), -50);
    });
    CHECK(wait_for([&] {
        for (auto &a : local.adv)
            if (a.msd[2] == 2 && a.msd.substr(3, 4) == PROBE) return true;
        return false;
    }, &node));

    // Selecting it opens the link on the field unit.
    app_cfg.linked_probes = {"04030201"};
    CHECK(wait_for([&] { return field_has_link(PROBE); }, &node));
    std::shared_ptr<FakeLink> flink;
    fsrc.with([&] {
        flink = fsrc.links.at(PROBE);
        flink->state_ = "ready";
        flink->rx["status"] = {status100()};
    });
    CHECK(wait_for([&] { return sent_has(local, [](const p::Frame &f) { return f.msg_type == 0x45; }); }, &node));

    // A phone command reaches the probe; its response reaches the phone.
    local.sent.clear();
    local.rx.push_back({1, p::build_request(p::MSG_SET_PREDICTION, PROBE + B("\x8a\x02", 2), 77)});
    CHECK(wait_for([&] {
        bool w = false;
        fsrc.with([&] { w = !flink->written.empty(); });
        return w;
    }, &node));
    fsrc.with([&] {
        CHECK(flink->written[0] == p::build_probe_request(p::MSG_SET_PREDICTION, B("\x8a\x02", 2)));
        flink->rx["uart"] = {p::build_probe_response(p::MSG_SET_PREDICTION, true)};
    });
    CHECK(wait_for([&] {
        return sent_has(local, [](const p::Frame &f) { return f.is_response && f.request_id == 77; });
    }, &node));

    // A MEATER next to the field unit is bridged and advertised by the app unit.
    Bytes maddr;
    proto::unhex("A1B2C3D4E5F6", maddr);
    fsrc.with([&] {
        core::Observation o;
        o.name = "MEATER+";
        o.ticks = core::ticks_ms();
        o.rssi = -60;
        fsrc.t.meaters[maddr] = o;
    });
    app_cfg.linked_probes.clear();
    app_cfg.meater_bridge = true;
    app_cfg.meater_probes = {{meater::addr_str(maddr), 3, 0}};
    CHECK(wait_for([&] { return field_has_link(maddr); }, &node));
    CHECK(wait_for([&] {
        bool on = false;
        fsrc.with([&] { on = fsrc.scan_active; });
        return on;
    }, &node));
    fsrc.with([&] {
        auto &ml = *fsrc.links.at(maddr);
        CHECK(ml.kind() == "meater");
        ml.state_ = "ready";
        Bytes temps;
        for (double t : {60.0, 57.0, 54.0, 56.0, 59.0, 140.0}) temps += le16(unsigned(std::lround(t * 32 - 4)));
        ml.rx["temp"] = {temps};
    });
    Bytes mserial = meater::serial_for(maddr);
    CHECK(wait_for([&] {
        for (auto &a : local.adv)
            if (a.msd.substr(3, 4) == mserial) return true;
        return false;
    }, &node));
    app_cfg.meater_bridge = false;
    app_cfg.linked_probes = {"04030201"};

    // The field unit goes away: links offline; it comes back: link re-opened.
    client->stop();
    cli.join();
    CHECK(wait_for([&] { return !remote.connected; }, &node, 10000));
    CHECK(remote.links.at(PROBE)->state() == "error: field unit offline");
    fsrc.with([&] { fsrc.links.clear(); });
    client = std::make_unique<split::SplitClient>(fradio, ccfg);
    cli = std::thread([&] { client->run(); });
    CHECK(wait_for([&] { return remote.connected && field_has_link(PROBE); }, &node));

    client->stop();
    server.stop();
    cli.join();
    srv.join();
}

TEST_CASE("a wrong key is rejected")
{
    uint16_t port = free_port();
    split::RemoteRadio remote;
    std::vector<std::string> logs;
    std::mutex logmu;
    split::ServerConfig scfg;
    scfg.key = "right";
    scfg.serial = "A";
    scfg.port = port;
    scfg.bind = "127.0.0.1";
    scfg.beacon = false;
    split::SplitServer server(remote, scfg, [&](const std::string &m) {
        std::lock_guard<std::mutex> g(logmu);
        logs.push_back(m);
    });
    std::thread srv([&] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    FakeFieldSource fsrc;
    split::FieldRadio fr(fsrc);
    split::ClientConfig ccfg;
    ccfg.key = "wrong";
    ccfg.port = port;
    ccfg.serial = "F";
    split::SplitClient client(fr, ccfg);
    client.session("127.0.0.1");
    remote.pump(0);
    CHECK_FALSE(remote.connected);
    CHECK_FALSE(client.connected());
    server.stop();
    srv.join();
    std::lock_guard<std::mutex> g(logmu);
    CHECK(std::any_of(logs.begin(), logs.end(),
                      [](const std::string &m) { return m.find("rejected: wrong key") != std::string::npos; }));
}
