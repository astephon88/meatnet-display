// Port of tests/test_protocol.py.
#include <cmath>
#include <cstring>

#include "doctest.h"
#include "proto.h"
#include "protocol.h"

namespace p = protocol;
using p::Bytes;

static Bytes B(const char *s, size_t n) { return Bytes(s, n); }

static Bytes le16(unsigned v) { return Bytes{char(v & 0xFF), char(v >> 8 & 0xFF)}; }

static Bytes le32(uint32_t v)
{
    Bytes b;
    for (int i = 0; i < 4; i++) b += char(v >> (8 * i) & 0xFF);
    return b;
}

static Bytes one_frame_payload(const Bytes &frame, p::Frame *out = nullptr)
{
    Bytes rest;
    auto frames = p::split_frames(frame, rest);
    REQUIRE(frames.size() == 1);
    if (out) *out = frames[0];
    return frames[0].payload;
}

static p::Frame one_frame(const Bytes &frame)
{
    p::Frame f;
    one_frame_payload(frame, &f);
    return f;
}

TEST_CASE("crc known vector")
{
    CHECK(p::crc16_ccitt("123456789") == 0x29B1);   // CRC-16/CCITT-FALSE check value
}

TEST_CASE("request round trip")
{
    Bytes frame = p::build_request(0x44, "", 0x11223344);
    CHECK(frame.substr(0, 2) == B("\xCA\xFE", 2));
    CHECK(frame.size() == 10);
    Bytes rest;
    auto frames = p::split_frames(B("\x00garbage", 8) + frame + "\xCA", rest);
    CHECK(rest == "\xCA");
    REQUIRE(frames.size() == 1);
    CHECK_FALSE(frames[0].is_response);
    CHECK(frames[0].msg_type == 0x44);
    CHECK(frames[0].request_id == 0x11223344);
}

TEST_CASE("response round trip, split across chunks")
{
    Bytes frame = p::build_response(0x04, 7, true, "abc", 9);
    Bytes rest;
    auto frames = p::split_frames(frame.substr(0, 8), rest);
    CHECK(frames.empty());
    CHECK(rest == frame.substr(0, 8));
    frames = p::split_frames(rest + frame.substr(8), rest);
    REQUIRE(frames.size() == 1);
    const auto &f = frames[0];
    CHECK(f.is_response);
    CHECK(f.request_id == 7);
    CHECK(f.response_id == 9);
    CHECK(f.success);
    CHECK(f.payload == "abc");
    CHECK(rest.empty());
}

TEST_CASE("bad crc is skipped")
{
    Bytes frame = p::build_request(0x44, "x", 1);
    frame.back() ^= '\xFF';
    Bytes good = p::build_request(0x42, B("\x00", 1), 2), rest;
    auto frames = p::split_frames(frame + good, rest);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].msg_type == 0x42);
}

TEST_CASE("probe frames")
{
    Bytes req = p::build_probe_request(0x05, B("\x01\x02", 2));
    CHECK(req.size() == 8);
    CHECK(req[4] == 0x05);
    CHECK(req[5] == 2);
    CHECK(p::crc16_ccitt(req.substr(4)) == (uint8_t(req[2]) | uint8_t(req[3]) << 8));
    Bytes payload = B("\x01\x00\x00\x00", 4) + Bytes(20, '\0');
    Bytes rsp = p::build_probe_response(0x04, true, payload), rest;
    auto frames = p::split_probe_frames(rsp + rsp.substr(0, 3), rest);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == p::ProbeFrame{0x04, true, payload});
    CHECK(rest == rsp.substr(0, 3));
}

TEST_CASE("node <-> probe translation")
{
    Bytes serial("\xCD\xAB\x00\x10", 4);
    CHECK(p::probe_serial_str(serial) == "1000ABCD");
    CHECK(p::probe_serial_bytes("1000ABCD") == serial);
    auto t = p::node_to_probe(0x04, serial + le32(5) + le32(9));
    REQUIRE(t);
    CHECK(t->first == serial);
    CHECK(t->second == le32(5) + le32(9));
    CHECK_FALSE(p::node_to_probe(0x44, serial));
    CHECK(p::probe_to_node_payload(serial, "") == "");
    CHECK(p::probe_to_node_payload(serial, "\x01") == serial + "\x01");
}

static Bytes raw13(const std::vector<double> &temps_c)
{
    Bytes out(13, '\0');
    for (size_t i = 0; i < temps_c.size(); i++) {
        unsigned v = static_cast<unsigned>(std::lround((temps_c[i] + 20) / 0.05)) & 0x1FFF;
        for (int b = 0; b < 13; b++)
            if (v >> b & 1) {
                unsigned bit = 13 * i + b;
                out[bit / 8] = char(uint8_t(out[bit / 8]) | 1 << (bit % 8));
            }
    }
    return out;
}

static Bytes probe_body(const Bytes &serial, const std::vector<double> &temps, int mode_byte = 0,
                        int vs_byte = 0)
{
    return Bytes(1, 1) + serial + raw13(temps) + char(mode_byte) + char(vs_byte) + Bytes(3, '\0');
}

TEST_CASE("probe advert decode and repeat")
{
    Bytes serial("\x01\x02\x03\x04", 4);
    std::vector<double> temps = {50, 51, 52, 53, 54, 55, 56, 57};
    // core = T2 (1), surface = T5 (index 4 -> code 1), ambient = T8 (code 3)
    int vs = (1 | (1 << 3) | (3 << 5)) << 1;
    Bytes body = probe_body(serial, temps, (2 << 5) | (1 << 2), vs);
    auto d = p::parse_probe_advert(body);
    REQUIRE(d);
    CHECK(d->serial == serial);
    CHECK(d->probe_id == 3);
    CHECK(d->color == 1);
    CHECK(std::fabs(*d->core_c - 51) < 0.06);
    CHECK(std::fabs(*d->surface_c - 54) < 0.06);
    CHECK(std::fabs(*d->ambient_c - 57) < 0.06);
    Bytes msd = p::repeat_probe_advert(body, 2);
    CHECK(msd.substr(0, 2) == p::VENDOR_LE);
    CHECK(msd[2] == 2);
    CHECK(msd.size() == 24);
    auto rd = p::parse_probe_advert(msd.substr(2));
    REQUIRE(rd);
    CHECK(rd->hops == 2);
    CHECK(std::fabs(*rd->core_c - 51) < 0.06);
}

TEST_CASE("display advert layout")
{
    Bytes msd = p::build_display_advertisement("ABCDEFGHIJ");
    CHECK(msd.substr(0, 2) == B("\xC7\x09", 2));
    CHECK(msd[2] == 4);
    CHECK(msd.substr(3, 10) == "ABCDEFGHIJ");
    CHECK(msd[13] == 1);
    CHECK(msd.size() == 2 + 1 + 10 + 1 + 11);
}

static Bytes status100(uint32_t session = 0x01020304, unsigned period = 1000)
{
    Bytes s = le32(10) + le32(20) + raw13(std::vector<double>(8, 60)) + Bytes(2, '\0');
    s += Bytes(7 + 10 + 8, '\0');           // prediction, food safe
    s += Bytes(2 + 44, '\0');               // overheat, prefs, alarms
    s += le32(session) + le16(period);
    REQUIRE(s.size() == 100);
    return s;
}

TEST_CASE("node probe status layout")
{
    Bytes serial("\x01\x02\x03\x04", 4), status = status100();
    p::Frame f = one_frame(p::build_node_probe_status(serial, status, 1));
    CHECK(f.msg_type == 0x45);
    CHECK(f.payload.size() == 105);
    CHECK(f.payload.substr(0, 4) == serial);
    CHECK(f.payload[52] == 0);              // 1 hop
    auto back = p::parse_node_probe_status(f.payload);
    REQUIRE(back);
    CHECK(back->serial4 == serial);
    CHECK(back->status == status);
    CHECK(back->hops == 1);
    auto bumped = p::bump_node_probe_status(f);
    REQUIRE(bumped);
    p::Frame b = one_frame(*bumped);
    CHECK(b.payload[52] == 1);
    CHECK(b.request_id == f.request_id);
    auto d = p::decode_probe_status(status);
    REQUIRE(d);
    CHECK(d->log_min == 10);
    CHECK(d->log_max == 20);
    CHECK(d->session_id == 0x01020304);
    CHECK(d->sample_period == 1000);
}

TEST_CASE("prediction decode")
{
    uint64_t v = 3 | (1 << 4) | (1 << 6) | (650ull << 8) | (50ull << 18) | (1234ull << 28) |
                 (700ull << 45);
    Bytes b7;
    for (int i = 0; i < 7; i++) b7 += char(v >> (8 * i) & 0xFF);
    auto d = p::decode_prediction(b7);
    CHECK(d.state == 3);
    CHECK(d.mode == 1);
    CHECK(d.type == 1);
    CHECK(std::fabs(d.setpoint_c - 65.0) < 1e-6);
    CHECK(d.seconds == 1234);
    CHECK(std::fabs(d.est_core_c - 50.0) < 1e-6);
}

TEST_CASE("probe list and node list")
{
    Bytes s("\x01\x02\x03\x04", 4);
    Bytes pl = one_frame_payload(p::build_read_probe_list_response(5, {"", s}));
    CHECK(pl.size() == 50);
    CHECK(pl[5] == 2);
    CHECK(pl.substr(6, 4) == s);
    std::vector<p::NodeEntry> nodes = {{20, 4, "ME00000001"}};
    for (int i = 0; i < 6; i++) {
        char buf[11];
        snprintf(buf, sizeof(buf), "B%09d", i);
        nodes.push_back({21 + i, 5, buf});
    }
    Bytes n0 = one_frame_payload(p::build_read_node_list_response(1, 0, nodes));
    CHECK(n0.substr(0, 4) == Bytes{0, 2, 7, 5});
    CHECK(n0.size() == 4 + 5 * 12);
    Bytes n1 = one_frame_payload(p::build_read_node_list_response(1, 1, nodes));
    CHECK(n1[3] == 2);
}

TEST_CASE("topology layout")
{
    Bytes t = one_frame_payload(
        p::build_topology_response(1, 20, 4, "ME00000001", {}, {{1, -60}, {21, -70}}));
    CHECK(t.size() == 12 + 1 + 8 + 1 + 8);
    CHECK(t[12] == 0);
    CHECK(t[21] == 2);
    CHECK(t[22] == 1);
    CHECK(uint8_t(t[23]) == uint8_t(-60));
}

TEST_CASE("heartbeat round trip and bump")
{
    Bytes frame = p::build_heartbeat("ME00000001", B("\x01\x02\x03\x04\x05\x06", 6), 4, false,
                                     {{1, B("\x01\x02\x03\x04", 4), -55}, {5, "BOOSTER001", -70}});
    p::Frame f = one_frame(frame);
    CHECK(f.payload.size() == 19 + 52);
    auto d = p::parse_heartbeat(f.payload);
    REQUIRE(d);
    CHECK(d->serial == "ME00000001");
    CHECK(d->ptype == 4);
    CHECK_FALSE(d->inbound);
    REQUIRE(d->records.size() == 2);
    CHECK(d->records[0] == p::HeartbeatRecord{1, B("\x01\x02\x03\x04", 4), -55});
    CHECK(d->records[1] == p::HeartbeatRecord{5, "BOOSTER001", -70});
    auto b = p::bump_heartbeat(f);
    REQUIRE(b);
    CHECK(one_frame_payload(*b)[17] == 1);
}

TEST_CASE("sync thermometer list")
{
    Bytes pl = one_frame_payload(
        p::build_sync_thermometer_list(Bytes(6, 1), {B("\x01\x02\x03\x04", 4), ""}));
    CHECK(pl.size() == 26);
    auto r = p::parse_sync_thermometer_list(pl);
    REQUIRE(r);
    CHECK(r->second[0] == B("\x01\x02\x03\x04", 4));
    CHECK(r->second[1].empty());
    CHECK(r->second[2].empty());
    CHECK(r->second[3].empty());
}

TEST_CASE("link entries round trip")
{
    std::vector<p::LinkEntry> entries;
    for (int i = 0; i < 3; i++) {
        entries.push_back({1, B("\x01\x02\x03\x04", 4), 5, false});
        entries.push_back({4, "DISPLAY001", 6, false});
    }
    Bytes pl = one_frame_payload(p::build_link_entries_response(0x73, 1, 1, entries));
    auto got = p::parse_link_entries_response(pl);
    REQUIRE(got);
    CHECK(got->page == 1);
    CHECK(got->total_pages == 2);
    REQUIRE(got->entries.size() == 2);
    CHECK(got->entries[0].serial == B("\x01\x02\x03\x04", 4));
    CHECK(got->entries[1].serial == "DISPLAY001");
    CHECK(p::encode_product_serial(1, B("\x01\x02\x03\x04", 4)) ==
          B("\x01\x02\x03\x04", 4) + Bytes(8, '\xFF'));
}

TEST_CASE("gauge and engine adverts")
{
    Bytes gauge = Bytes(1, 3) + "GAUGE00001" + le16(unsigned((100 + 20) / 0.1)) + char(1) +
                  char(0) + le32(0) + char(0) + Bytes(3, '\0');
    auto d = p::parse_node_advert(gauge);
    REQUIRE(d);
    CHECK(d->ptype == 3);
    CHECK(std::fabs(*d->temp_c - 100) < 0.06);
    Bytes eng = Bytes(1, 6) + "ENGINE0001" + le16(unsigned((110 + 20) / 0.1)) + B("\x01\x00", 2);
    d = p::parse_node_advert(eng);
    REQUIRE(d);
    CHECK(d->ptype == 6);
    CHECK(std::fabs(d->setpoint_c - 110) < 0.06);
    CHECK(d->app_mode);
}

TEST_CASE("probe frame captured from a real probe")
{
    // Read Session Information response from probe 10018F16: no response bit
    // in the type, CRC over type/success/length/payload.
    Bytes raw, rest;
    REQUIRE(proto::unhex("cafe10d3030106ebf05b618813", raw));
    auto frames = p::split_probe_frames(raw, rest);
    CHECK(rest.empty());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].msg_type == 0x03);
    CHECK(frames[0].success);
    const Bytes &pl = frames[0].payload;
    REQUIRE(pl.size() == 6);
    uint32_t session = uint8_t(pl[0]) | uint8_t(pl[1]) << 8 | uint8_t(pl[2]) << 16 |
                       uint32_t(uint8_t(pl[3])) << 24;
    CHECK(session == 0x615BF0EB);
    CHECK((uint8_t(pl[4]) | uint8_t(pl[5]) << 8) == 5000);
}
