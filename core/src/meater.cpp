#include "meater.h"

#include "proto.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace meater {

const char *const SERVICE_UUIDS[2] = {"C9E2746C-59F1-4E54-A0DD-E1E54555CF8B",
                                      "DCBB67CA-64FB-41A3-99D1-5D9FD8CF33CA"};

static uint8_t at(const Bytes &b, size_t i) { return static_cast<uint8_t>(b[i]); }

static Bytes uuid_le(const char *text)
{
    uint8_t raw[16];
    proto::uuid128_le(text, raw);
    return Bytes(reinterpret_cast<const char *>(raw), 16);
}

std::optional<std::string> adv_match(const Bytes &payload)
{
    static const Bytes uuids[2] = {uuid_le(SERVICE_UUIDS[0]), uuid_le(SERVICE_UUIDS[1])};
    size_t i = 0, n = payload.size();
    std::optional<Bytes> name;
    bool uuid_hit = false;
    while (i + 1 < n) {
        size_t length = at(payload, i);
        if (length == 0) break;
        int t = at(payload, i + 1);
        Bytes data = payload.substr(i + 2, std::min(length - 1, n - std::min(n, i + 2)));
        if (t == 0x08 || t == 0x09) {
            name = data;
        } else if (t == 0x06 || t == 0x07) {
            for (size_t k = 0; k + 16 <= data.size(); k += 16)
                if (data.compare(k, 16, uuids[0]) == 0 || data.compare(k, 16, uuids[1]) == 0)
                    uuid_hit = true;
        }
        i += 1 + length;
    }
    if (name && name->size() >= 6) {
        std::string pre = name->substr(0, 6);
        for (auto &c : pre) c = static_cast<char>(c | 0x20);
        if (pre == "meater") return *name;
    }
    if (uuid_hit) return name ? *name : std::string();
    return std::nullopt;
}

std::optional<Temps6> decode_temps(const Bytes &data)
{
    if (data.size() < 12) return std::nullopt;
    Temps6 t;
    for (int i = 0; i < 6; i++) t[i] = ((at(data, 2 * i) | at(data, 2 * i + 1) << 8) + 4.0) / 32.0;
    return t;
}

Temps8 map_sensors(const Temps6 &t6)
{
    Temps8 out{};
    const int span = 4;                     // 4 intervals across T1..T7
    for (int i = 0; i < 7; i++) {
        double x = i * span / 6.0;
        int lo = static_cast<int>(x);
        if (lo >= span) {
            out[i] = t6[span];
            continue;
        }
        double f = x - lo;
        out[i] = t6[lo] + (t6[lo + 1] - t6[lo]) * f;
    }
    out[7] = t6[5];
    return out;
}

int core_index(const Temps8 &t8)
{
    int best = 0;
    for (int i = 1; i < 6; i++)
        if (t8[i] < t8[best]) best = i;
    return best;
}

std::pair<Bytes, int> encode_raw13(const Temps8 &t8)
{
    Bytes raw(13, '\0');
    int over = 0;
    for (int i = 0; i < 8; i++) {
        long v = std::lround((t8[i] + 20.0) / 0.05);
        if (v > 0x1FFF) {
            v = 0x1FFF;
            over |= 1 << i;
        } else if (v < 0) {
            v = 0;
        }
        for (int b = 0; b < 13; b++)
            if (v >> b & 1) {
                int bit = 13 * i + b;
                raw[bit / 8] = static_cast<char>(at(raw, bit / 8) | 1 << (bit % 8));
            }
    }
    return {raw, over};
}

Bytes serial_for(const Bytes &addr6)
{
    Bytes a = addr6;
    a.resize(6, '\0');
    return Bytes{a[5], a[4], a[3], static_cast<char>(SERIAL_PREFIX)};
}

std::string addr_str(const Bytes &addr6)
{
    std::string out;
    char buf[4];
    for (size_t i = 0; i < addr6.size(); i++) {
        snprintf(buf, sizeof(buf), i ? ":%02X" : "%02X", at(addr6, i));
        out += buf;
    }
    return out;
}

Bytes addr_bytes(const std::string &text)
{
    Bytes out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t next = text.find(':', pos);
        std::string part = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (part.empty() || part.size() > 2) return Bytes();
        char *end;
        long v = std::strtol(part.c_str(), &end, 16);
        if (*end) return Bytes();
        out += static_cast<char>(v);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

Fields encode_fields(const Temps8 &t8, int probe_id, int color, bool low_battery)
{
    Fields f;
    std::tie(f.raw13, f.overheat) = encode_raw13(t8);
    f.core = core_index(t8);
    f.vs = f.core | (SURFACE_CODE << 3) | (AMBIENT_CODE << 5);
    f.mode = (((probe_id - 1) & 0x07) << 5) | ((color & 0x07) << 2);
    f.status = (low_battery ? 1 : 0) | (f.vs << 1);
    f.core_c = t8[f.core];
    return f;
}

static Bytes byte(int v) { return Bytes(1, static_cast<char>(v & 0xFF)); }

std::pair<Bytes, int> build_advert(const Bytes &serial4, const Temps8 &t8, int probe_id, int color,
                                   bool low_battery, int hop_count)
{
    Fields f = encode_fields(t8, probe_id, color, low_battery);
    Bytes body = byte(protocol::PRODUCT_TYPE_NODE) + serial4.substr(0, 4) + f.raw13 + byte(f.mode) +
                 byte(f.status) + byte(protocol::network_info(hop_count)) + byte(f.overheat);
    return {protocol::VENDOR_LE + body, f.core};
}

Bytes build_direct_advert(const Bytes &serial4, const Temps8 &t8, int probe_id, int color,
                          bool low_battery)
{
    Fields f = encode_fields(t8, probe_id, color, low_battery);
    Bytes body = byte(protocol::PRODUCT_TYPE_PROBE) + serial4.substr(0, 4) + f.raw13 +
                 byte(f.mode) + byte(f.status) + byte(0) + byte(f.overheat) + byte(0);
    return protocol::VENDOR_LE + body;
}

static constexpr int PREDICTION_STATE_INSERTED = 1;

static Bytes le_bytes(uint64_t v, int n)
{
    Bytes out;
    for (int i = 0; i < n; i++) out += static_cast<char>(v >> (8 * i) & 0xFF);
    return out;
}

Bytes prediction_status(double core_c)
{
    long est = std::max(0L, std::min(0x7FFL, std::lround((core_c + 20.0) / 0.1)));
    return le_bytes(PREDICTION_STATE_INSERTED | (static_cast<uint64_t>(est) << 45), 7);
}

Bytes prediction_log(int vs) { return le_bytes((vs & 0x7F) | (PREDICTION_STATE_INSERTED << 7), 7); }

Bytes build_status(const Fields &f, uint32_t log_min, uint32_t log_max, uint32_t session_id,
                   uint16_t sample_period_ms)
{
    return le_bytes(log_min, 4) + le_bytes(log_max, 4) + f.raw13 + byte(f.mode) + byte(f.status) +
           prediction_status(f.core_c) + Bytes(10 + 8, '\0')   // food safe data / status
           + byte(f.overheat) + byte(0)                         // overheating, preferences
           + Bytes(44, '\0')                                    // high/low alarm arrays
           + le_bytes(session_id, 4) + le_bytes(sample_period_ms, 2);
}

Bytes log_record(const Fields &f) { return f.raw13 + prediction_log(f.vs); }

}  // namespace meater
