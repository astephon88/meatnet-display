// Shared test helpers: byte builders, sample probe data, and the fake
// transport/link the Node tests drive (ports of tests/test_protocol.py's
// helpers and tests/test_meatnet.py's fakes).
#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "protocol.h"
#include "transport.h"

namespace testing {

using protocol::Bytes;

inline Bytes B(const char *s, size_t n) { return Bytes(s, n); }

inline Bytes le16(unsigned v) { return Bytes{char(v & 0xFF), char(v >> 8 & 0xFF)}; }

inline Bytes le32(uint32_t v)
{
    Bytes b;
    for (int i = 0; i < 4; i++) b += char(v >> (8 * i) & 0xFF);
    return b;
}

inline uint32_t u32(const Bytes &b, size_t off)
{
    return uint8_t(b[off]) | uint8_t(b[off + 1]) << 8 | uint8_t(b[off + 2]) << 16 |
           uint32_t(uint8_t(b[off + 3])) << 24;
}

inline std::vector<protocol::Frame> split(const Bytes &raw)
{
    Bytes rest;
    return protocol::split_frames(raw, rest);
}

inline protocol::Frame one_frame(const Bytes &raw)
{
    auto fs = split(raw);
    REQUIRE(fs.size() == 1);
    return fs[0];
}

// 8 temperatures (C) -> 13-byte raw field.
inline Bytes raw13(const std::vector<double> &temps_c)
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

inline Bytes probe_body(const Bytes &serial, const std::vector<double> &temps, int mode_byte = 0,
                        int vs_byte = 0)
{
    return Bytes(1, 1) + serial + raw13(temps) + char(mode_byte) + char(vs_byte) + Bytes(3, '\0');
}

// A 100-byte Probe Status characteristic value.
inline Bytes status100(uint32_t session = 0x01020304, unsigned period = 1000)
{
    Bytes s = le32(10) + le32(20) + raw13(std::vector<double>(8, 60)) + Bytes(2, '\0');
    s += Bytes(7 + 10 + 8, '\0');           // prediction, food safe
    s += Bytes(2 + 44, '\0');               // overheat, prefs, alarms
    s += le32(session) + le16(period);
    return s;
}

// -- fakes ---------------------------------------------------------------------------------

struct FakeLink : core::Link {
    std::string kind_, state_ = "idle";
    bool enabled_ = true;
    std::vector<Bytes> written;
    std::map<std::string, std::vector<Bytes>> rx;
    std::map<std::string, std::string> info_;

    explicit FakeLink(std::string kind) : kind_(std::move(kind)) {}
    const std::string &kind() const override { return kind_; }
    const std::string &state() const override { return state_; }
    bool enabled() const override { return enabled_; }
    int mtu() const override { return 185; }
    std::vector<Bytes> take(const std::string &sub) override
    {
        auto q = std::move(rx[sub]);
        rx[sub].clear();
        return q;
    }
    bool write(const Bytes &data) override
    {
        written.push_back(data);
        return true;
    }
    const std::map<std::string, std::string> &info() const override { return info_; }
};

struct FakeTransport : core::Transport {
    bool scan_active = false, standby = false, fast = false;
    std::vector<std::pair<int, Bytes>> rx;
    std::vector<std::pair<Bytes, std::vector<int>>> sent;
    std::vector<Advert> adv;

    FakeTransport()
    {
        max_conn = 4;
        mac = Bytes("\x01\x02\x03\x04\x05\x06", 6);
        inbound = {1};
    }
    std::vector<std::pair<int, Bytes>> poll_rx() override
    {
        auto q = std::move(rx);
        rx.clear();
        return q;
    }
    void send(const Bytes &frame, const std::vector<int> &conns) override { sent.push_back({frame, conns}); }
    std::shared_ptr<core::Link> open_link(const Bytes &key, const std::string &kind, int, const Bytes &) override
    {
        auto &l = links[key];
        if (!l) l = std::make_shared<FakeLink>(kind);
        static_cast<FakeLink &>(*l).enabled_ = true;
        return l;
    }
    void close_link(const Bytes &key) override { links.erase(key); }
    void set_scan_active(bool on) override { scan_active = on; }
    void set_standby(bool on) override { standby = on; }
    void set_advertising(const std::vector<Advert> &a, bool f) override
    {
        adv = a;
        fast = f;
    }
    // Every frame sent to inbound connections since the last call.
    std::vector<protocol::Frame> frames()
    {
        std::vector<protocol::Frame> out;
        for (auto &s : sent)
            for (auto &f : split(s.first)) out.push_back(f);
        sent.clear();
        return out;
    }
    FakeLink &link(const Bytes &key) { return static_cast<FakeLink &>(*links.at(key)); }
};

}  // namespace testing
