#include "protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace protocol {

const Bytes PROBE_STATUS_SERVICE_UUID_LE(
    "\x7A\x40\xC1\x51\xAE\x97\x44\x3D\x92\x37\xAB\xCA\x00\x01\x00\x00", 16);
const Bytes DFU_SERVICE_UUID_LE(
    "\xFB\x34\x9B\x5F\x80\x00\x00\x80\x00\x10\x00\x00\x59\xFE\x00\x00", 16);

const DisRead PROBE_DIS_READS[3] = {
    {"firmware", DIS_SERVICE_UUID, "00002A26-0000-1000-8000-00805F9B34FB"},
    {"hardware", DIS_SERVICE_UUID, "00002A27-0000-1000-8000-00805F9B34FB"},
    {"model", DIS_SERVICE_UUID, "00002A24-0000-1000-8000-00805F9B34FB"},
};

const char *const PREDICTION_STATES[5] = {"not inserted", "inserted", "warming",
                                          "predicting", "removal done"};

const char *product_name(int ptype)
{
    switch (ptype) {
    case 1: return "Probe";
    case 2: return "Repeater";
    case 3: return "Gauge";
    case 4: return "Display";
    case 5: return "Booster";
    case 6: return "Engine";
    default: return "";
    }
}

bool probe_addressed(uint8_t msg_type) { return msg_type >= 0x01 && msg_type <= 0x0B; }

// -- little helpers ----------------------------------------------------------------------

static uint8_t at(const Bytes &b, size_t i) { return static_cast<uint8_t>(b[i]); }

static uint16_t u16(const Bytes &b, size_t off) { return at(b, off) | at(b, off + 1) << 8; }

static uint32_t u32(const Bytes &b, size_t off)
{
    return at(b, off) | at(b, off + 1) << 8 | at(b, off + 2) << 16 | (uint32_t)at(b, off + 3) << 24;
}

static void put16(Bytes &b, uint16_t v)
{
    b += static_cast<char>(v & 0xFF);
    b += static_cast<char>(v >> 8);
}

static void put32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; i++) b += static_cast<char>((v >> (8 * i)) & 0xFF);
}

static Bytes byte(int v) { return Bytes(1, static_cast<char>(v & 0xFF)); }

static Bytes sub(const Bytes &b, size_t from, size_t to)   // Python b[from:to]
{
    if (from >= b.size()) return Bytes();
    return b.substr(from, std::min(to, b.size()) - from);
}

static Bytes pad(const Bytes &b, size_t n)
{
    Bytes out = b.substr(0, n);
    out.resize(n, '\0');
    return out;
}

// Little-endian bit field from a byte string (Python int.from_bytes + shift).
static uint64_t bits(const Bytes &b, unsigned shift, unsigned width)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < width; i++) {
        unsigned bit = shift + i, idx = bit / 8;
        if (idx < b.size() && (at(b, idx) >> (bit % 8) & 1)) v |= uint64_t(1) << i;
    }
    return v;
}

static uint8_t int8(int v) { return static_cast<uint8_t>(std::max(-128, std::min(127, v))); }

// -- basic encodings -----------------------------------------------------------------------

double decode_raw_temperature(unsigned raw) { return (raw & 0x1FFF) * 0.1 - 20.0; }

Bytes encode_serial(const Bytes &serial) { return pad(serial, 10); }

std::string serial10_str(const Bytes &serial10)
{
    Bytes raw = serial10.substr(0, serial10.find('\0'));
    bool text = std::all_of(raw.begin(), raw.end(),
                            [](char c) { return c >= 0x20 && c < 0x7F; });
    if (text) return raw;
    std::string out;
    char buf[3];
    for (char c : raw) {
        snprintf(buf, sizeof(buf), "%02X", static_cast<uint8_t>(c));
        out += buf;
    }
    return out;
}

std::string probe_serial_str(const Bytes &serial4)
{
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", (unsigned)u32(pad(serial4, 4), 0));
    return buf;
}

Bytes probe_serial_bytes(const std::string &text)
{
    uint32_t v = static_cast<uint32_t>(std::stoul(text, nullptr, 16));
    Bytes out;
    put32(out, v);
    return out;
}

uint16_t crc16_ccitt(const Bytes &data, uint16_t initial)
{
    uint16_t crc = initial;
    for (char c : data) {
        crc ^= static_cast<uint8_t>(c) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

static uint32_t default_random()
{
    static std::mt19937 gen(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return gen();
}

static uint32_t (*g_random)() = default_random;

void set_random_source(uint32_t (*fn)()) { g_random = fn ? fn : default_random; }

uint32_t new_message_id() { return g_random(); }

// -- MeatNet Node UART frames ---------------------------------------------------------------

static Bytes framed(const Bytes &crc_region)
{
    Bytes out = SYNC_BYTES;
    put16(out, crc16_ccitt(crc_region));
    return out + crc_region;
}

Bytes build_request(uint8_t message_type, const Bytes &payload, std::optional<uint32_t> request_id)
{
    Bytes r = byte(message_type & 0x7F);
    put32(r, request_id ? *request_id : new_message_id());
    r += byte(payload.size());
    return framed(r + payload);
}

Bytes build_response(uint8_t message_type, uint32_t request_id, bool success, const Bytes &payload,
                     std::optional<uint32_t> response_id)
{
    Bytes r = byte((message_type & 0x7F) | RESPONSE_FLAG);
    put32(r, request_id);
    put32(r, response_id ? *response_id : new_message_id());
    r += byte(success ? 1 : 0);
    r += byte(payload.size());
    return framed(r + payload);
}

// The remainder when no further sync is found: keep a trailing 0xCA in case
// it's the first half of a sync.
static Bytes tail_rest(const Bytes &buf)
{
    return !buf.empty() && at(buf, buf.size() - 1) == 0xCA ? buf.substr(buf.size() - 1) : Bytes();
}

std::vector<Frame> split_frames(const Bytes &buf, Bytes &rest)
{
    std::vector<Frame> frames;
    size_t i = 0, n = buf.size();
    while (true) {
        i = buf.find(SYNC_BYTES, i);
        if (i == Bytes::npos) {
            rest = tail_rest(buf);
            return frames;
        }
        if (i + 5 > n) break;
        uint8_t mtype = at(buf, i + 4);
        bool is_resp = mtype & RESPONSE_FLAG;
        size_t hdr = is_resp ? 15 : 10;
        if (i + hdr > n) break;
        size_t end = i + hdr + at(buf, i + hdr - 1);
        if (end > n) break;
        if (crc16_ccitt(buf.substr(i + 4, end - i - 4)) != u16(buf, i + 2)) {
            i += 1;             // bad frame: resync past this sync byte
            continue;
        }
        Frame f;
        f.is_response = is_resp;
        f.msg_type = mtype & 0x7F;
        f.request_id = u32(buf, i + 5);
        if (is_resp) {
            f.response_id = u32(buf, i + 9);
            f.success = at(buf, i + 13) != 0;
        }
        f.payload = buf.substr(i + hdr, end - i - hdr);
        f.raw = buf.substr(i, end - i);
        frames.push_back(std::move(f));
        i = end;
    }
    rest = buf.substr(i);
    return frames;
}

Bytes with_request_payload(const Frame &frame, const Bytes &payload)
{
    return build_request(frame.msg_type, payload, frame.request_id);
}

// -- Probe UART frames ------------------------------------------------------------------------

Bytes build_probe_request(uint8_t message_type, const Bytes &payload)
{
    return framed(byte(message_type & 0x7F) + byte(payload.size()) + payload);
}

Bytes build_probe_response(uint8_t message_type, bool success, const Bytes &payload)
{
    return framed(byte(message_type & 0x7F) + byte(success ? 1 : 0) + byte(payload.size()) +
                  payload);
}

// Probes only send responses: 7-byte header (sync, CRC, type, success,
// length). Real probes leave the response bit of the type clear, and the CRC
// covers type, success, length and payload.
std::vector<ProbeFrame> split_probe_frames(const Bytes &buf, Bytes &rest)
{
    std::vector<ProbeFrame> out;
    size_t i = 0, n = buf.size();
    while (true) {
        i = buf.find(SYNC_BYTES, i);
        if (i == Bytes::npos) {
            rest = tail_rest(buf);
            return out;
        }
        if (i + 7 > n) break;
        size_t end = i + 7 + at(buf, i + 6);
        if (end > n) break;
        if (crc16_ccitt(buf.substr(i + 4, end - i - 4)) != u16(buf, i + 2)) {
            i += 1;
            continue;
        }
        out.push_back({static_cast<uint8_t>(at(buf, i + 4) & 0x7F), at(buf, i + 5) != 0,
                       buf.substr(i + 7, end - i - 7)});
        i = end;
    }
    rest = buf.substr(i);
    return out;
}

std::optional<std::pair<Bytes, Bytes>> node_to_probe(uint8_t msg_type, const Bytes &payload)
{
    if (!probe_addressed(msg_type) || payload.size() < 4) return std::nullopt;
    return std::make_pair(payload.substr(0, 4), payload.substr(4));
}

Bytes probe_to_node_payload(const Bytes &serial4, const Bytes &probe_payload)
{
    return probe_payload.empty() ? Bytes() : serial4 + probe_payload;
}

// -- advertising ---------------------------------------------------------------------------------

Bytes build_display_advertisement(const Bytes &serial, bool high_radio_power)
{
    return VENDOR_LE + byte(PRODUCT_TYPE_DISPLAY) + encode_serial(serial) +
           byte(high_radio_power ? 1 : 0) + Bytes(11, '\0');
}

uint8_t network_info(int hop_count)
{
    return (std::max(1, std::min(MAX_HOPS, hop_count)) - 1) & 0x03;
}

int hops_from_network_info(uint8_t b) { return (b & 0x03) + 1; }

Bytes repeat_probe_advert(const Bytes &body, int hop_count)
{
    Bytes b = pad(body, 22);
    b[0] = static_cast<char>(PRODUCT_TYPE_NODE);
    b[20] = static_cast<char>(network_info(hop_count));
    return VENDOR_LE + b;
}

Bytes probe_advert_from_status(const Bytes &serial4, const Bytes &status, int hop_count)
{
    return VENDOR_LE + byte(PRODUCT_TYPE_NODE) + serial4.substr(0, 4) + sub(status, 8, 23) +
           byte(network_info(hop_count)) + byte(status.size() > 48 ? at(status, 48) : 0);
}

std::optional<Bytes> manufacturer_data(const Bytes &adv)
{
    size_t i = 0, n = adv.size();
    while (i + 1 < n) {
        size_t length = at(adv, i);
        if (length == 0) break;
        if (at(adv, i + 1) == 0xFF && length >= 4) {
            Bytes ad = sub(adv, i + 2, i + 1 + length);
            if (ad.size() > 2 && ad.compare(0, 2, VENDOR_LE) == 0) return ad.substr(2);
        }
        i += 1 + length;
    }
    return std::nullopt;
}

// -- probe data decoding -------------------------------------------------------------------------

std::array<double, 8> unpack_probe_temps(const Bytes &raw13)
{
    Bytes raw = raw13.substr(0, 13);
    std::array<double, 8> t{};
    for (int i = 0; i < 8; i++) t[i] = bits(raw, 13 * i, 13) * 0.05 - 20.0;
    return t;
}

ProbeData decode_probe(const Bytes &raw13, uint8_t mode_byte, uint8_t status_byte)
{
    ProbeData d;
    d.temps = unpack_probe_temps(raw13);
    d.mode = mode_byte & 0x03;
    int vs = (status_byte & 0xFE) >> 1;
    int core_i = vs & 0x07;                 // 0..5 -> T1..T6
    int surf_i = 3 + ((vs >> 3) & 0x03);    // T4..T7
    int amb_i = 4 + ((vs >> 5) & 0x03);     // T5..T8
    bool ok = d.mode == PROBE_MODE_NORMAL && core_i <= 5;
    d.color = (mode_byte >> 2) & 0x07;
    d.probe_id = ((mode_byte >> 5) & 0x07) + 1;
    if (d.mode == PROBE_MODE_INSTANT_READ) d.instant_c = d.temps[0];
    d.core_sensor = core_i + 1;
    if (ok) {
        d.core_c = d.temps[core_i];
        d.surface_c = d.temps[surf_i];
        d.ambient_c = d.temps[amb_i];
    }
    d.low_battery = status_byte & 0x01;
    return d;
}

std::optional<ProbeAdvert> parse_probe_advert(const Bytes &body)
{
    if (body.size() < 20 || (at(body, 0) != PRODUCT_TYPE_PROBE && at(body, 0) != PRODUCT_TYPE_NODE))
        return std::nullopt;
    Bytes serial = body.substr(1, 4);
    if (serial == ZERO4) return std::nullopt;   // a repeater with no probe connected
    ProbeAdvert d;
    static_cast<ProbeData &>(d) = decode_probe(body.substr(5, 13), at(body, 18), at(body, 19));
    d.serial = serial;
    d.ptype = at(body, 0);
    d.hops = d.ptype == PRODUCT_TYPE_NODE && body.size() > 20 ? hops_from_network_info(at(body, 20))
                                                              : 0;
    d.overheating = body.size() > 21 ? at(body, 21) : 0;
    return d;
}

Prediction decode_prediction(const Bytes &b7)
{
    Bytes b = b7.substr(0, 7);
    Prediction p;
    p.state = bits(b, 0, 4);
    p.mode = bits(b, 4, 2);
    p.type = bits(b, 6, 2);
    p.setpoint_c = bits(b, 8, 10) * 0.1;
    p.heat_start_c = bits(b, 18, 10) * 0.1;
    p.seconds = bits(b, 28, 17);
    p.est_core_c = bits(b, 45, 11) * 0.1 - 20.0;
    return p;
}

Alarm decode_alarm(unsigned value)
{
    return {(value & 1) != 0, (value & 2) != 0, (value & 4) != 0,
            decode_raw_temperature(value >> 3)};
}

std::optional<ProbeStatus> decode_probe_status(const Bytes &status)
{
    if (status.size() < 48) return std::nullopt;
    ProbeStatus d;
    static_cast<ProbeData &>(d) = decode_probe(status.substr(8, 13), at(status, 21), at(status, 22));
    d.log_min = u32(status, 0);
    d.log_max = u32(status, 4);
    d.prediction = decode_prediction(status.substr(23, 7));
    d.food_safe_state = at(status, 40) & 0x07;
    d.overheating = status.size() > 48 ? at(status, 48) : 0;
    if (status.size() >= 94)
        for (size_t off = 50; off < 94; off += 2)
            if (u16(status, off) & 0x04) {
                d.alarming = true;
                break;
            }
    if (status.size() >= 100) {
        d.session_id = u32(status, 94);
        d.sample_period = u16(status, 98);
    }
    return d;
}

Bytes build_node_probe_status(const Bytes &serial4, const Bytes &status, int hop_count,
                              std::optional<uint32_t> request_id)
{
    Bytes tail = status.size() >= 100 ? status.substr(94, 6) : Bytes(6, '\0');
    Bytes payload = serial4.substr(0, 4) + sub(status, 0, 48) + byte(network_info(hop_count)) +
                    sub(status, 48, 94) + tail;
    return build_request(MSG_PROBE_STATUS, payload, request_id);
}

std::optional<int> node_probe_status_hops(const Bytes &payload)
{
    if (payload.size() < 53) return std::nullopt;
    return hops_from_network_info(at(payload, 52));
}

std::optional<Bytes> bump_node_probe_status(const Frame &frame)
{
    Bytes p = frame.payload;
    if (p.size() < 53) return std::nullopt;
    int hops = hops_from_network_info(at(p, 52));
    if (hops >= MAX_HOPS) return std::nullopt;
    p[52] = static_cast<char>(network_info(hops + 1));
    return build_request(MSG_PROBE_STATUS, p, frame.request_id);
}

std::optional<NodeProbeStatus> parse_node_probe_status(const Bytes &payload)
{
    if (payload.size() < 99) return std::nullopt;
    NodeProbeStatus r;
    r.serial4 = payload.substr(0, 4);
    r.status = sub(payload, 4, 52) + sub(payload, 53, 99) +
               (payload.size() >= 105 ? sub(payload, 99, 105) : Bytes());
    r.hops = hops_from_network_info(at(payload, 52));
    return r;
}

// -- other devices' advertisements ---------------------------------------------------------------

std::optional<NodeAdvert> parse_gauge_advert(const Bytes &body)
{
    if (body.size() < 14 || at(body, 0) != PRODUCT_TYPE_GAUGE) return std::nullopt;
    uint8_t flags = at(body, 13);
    NodeAdvert d;
    d.serial = body.substr(1, 10);
    d.ptype = PRODUCT_TYPE_GAUGE;
    d.sensor_present = flags & 0x01;
    if (d.sensor_present) d.temp_c = decode_raw_temperature(u16(body, 11));
    d.overheating = flags & 0x02;
    d.low_battery = flags & 0x04;
    return d;
}

std::optional<NodeAdvert> parse_engine_advert(const Bytes &body)
{
    if (body.size() < 15 || at(body, 0) != PRODUCT_TYPE_ENGINE) return std::nullopt;
    uint8_t flags = at(body, 13);
    NodeAdvert d;
    d.serial = body.substr(1, 10);
    d.ptype = PRODUCT_TYPE_ENGINE;
    d.setpoint_c = decode_raw_temperature(u16(body, 11));
    d.app_mode = flags & 0x01;
    d.ctrl_connected = flags & 0x02;
    d.lid_open = flags & 0x04;
    return d;
}

std::optional<NodeAdvert> parse_node_advert(const Bytes &body)
{
    if (body.size() < 11) return std::nullopt;
    int t = at(body, 0);
    if (t == PRODUCT_TYPE_GAUGE) return parse_gauge_advert(body);
    if (t == PRODUCT_TYPE_ENGINE) return parse_engine_advert(body);
    if (t == PRODUCT_TYPE_DISPLAY || t == PRODUCT_TYPE_BOOSTER) {
        NodeAdvert d;
        d.serial = body.substr(1, 10);
        d.ptype = t;
        return d;
    }
    return std::nullopt;
}

// -- node message payloads -----------------------------------------------------------------------

Bytes build_read_probe_list_response(uint32_t request_id, const std::vector<Bytes> &slots)
{
    Bytes payload;
    for (int i = 0; i < 10; i++) {
        Bytes s = i < (int)slots.size() && !slots[i].empty() ? pad(slots[i], 4) : ZERO4;
        payload += byte(i + 1) + s;
    }
    return build_response(MSG_READ_PROBE_LIST, request_id, true, payload);
}

Bytes build_read_node_list_response(uint32_t request_id, int page,
                                    const std::vector<NodeEntry> &nodes)
{
    const int per = 5;
    int n = nodes.size();
    int total_pages = std::max(1, (n + per - 1) / per);
    int from = std::min(n, page * per), to = std::min(n, (page + 1) * per);
    Bytes payload = byte(page) + byte(total_pages) + byte(n) + byte(to - from);
    for (int i = from; i < to; i++)
        payload += byte(nodes[i].number) + byte(nodes[i].ptype) + encode_serial(nodes[i].serial);
    return build_response(MSG_READ_NODE_LIST, request_id, page < total_pages, payload);
}

Bytes build_topology_response(uint32_t request_id, int device_number, int ptype,
                              const Bytes &serial, const std::vector<Conn> &inbound,
                              const std::vector<Conn> &outbound)
{
    Bytes payload = byte(device_number) + byte(ptype) + encode_serial(serial);
    for (const auto *conns : {&inbound, &outbound}) {
        size_t k = std::min<size_t>(4, conns->size());
        payload += byte(k);
        for (size_t i = 0; i < 4; i++)
            payload += i < k ? byte((*conns)[i].first) + byte(int8((*conns)[i].second))
                             : Bytes(2, '\0');
    }
    return build_response(MSG_READ_NETWORK_TOPOLOGY, request_id, true, payload);
}

Bytes heartbeat_serial(int ptype, const Bytes &serial)
{
    return ptype == PRODUCT_TYPE_PROBE ? pad(serial, 4) + Bytes(6, '\0') : encode_serial(serial);
}

Bytes build_heartbeat(const Bytes &serial, const Bytes &mac6, int ptype, bool inbound,
                      const std::vector<HeartbeatRecord> &records, int hop_count,
                      std::optional<uint32_t> request_id)
{
    Bytes payload = encode_serial(serial) + pad(mac6, 6) + byte(ptype) + byte(hop_count) +
                    byte(inbound ? 1 : 0);
    for (size_t i = 0; i < 4; i++) {
        if (i < records.size()) {
            const auto &r = records[i];
            payload += heartbeat_serial(r.ptype, r.serial) + byte(r.ptype) + byte(0x01) +
                       byte(int8(r.rssi));
        } else {
            payload += Bytes(13, '\0');
        }
    }
    return build_request(MSG_HEARTBEAT, payload, request_id);
}

std::optional<Heartbeat> parse_heartbeat(const Bytes &payload)
{
    if (payload.size() < 19) return std::nullopt;
    Heartbeat d;
    d.serial = payload.substr(0, 10);
    d.mac = payload.substr(10, 6);
    d.ptype = at(payload, 16);
    d.hops = at(payload, 17);
    d.inbound = at(payload, 18) != 0;
    for (size_t i = 0; i < 4; i++) {
        size_t off = 19 + 13 * i;
        if (off + 13 > payload.size()) break;
        if (at(payload, off + 11) & 0x01) {
            int t = at(payload, off + 10);
            Bytes s = payload.substr(off, t == PRODUCT_TYPE_PROBE ? 4 : 10);
            int rssi = at(payload, off + 12);
            d.records.push_back({t, s, rssi > 127 ? rssi - 256 : rssi});
        }
    }
    return d;
}

std::optional<Bytes> bump_heartbeat(const Frame &frame)
{
    Bytes p = frame.payload;
    if (p.size() < 19 || at(p, 17) + 1 >= MAX_HOPS) return std::nullopt;
    p[17] = static_cast<char>(at(p, 17) + 1);
    return build_request(MSG_HEARTBEAT, p, frame.request_id);
}

// Device type + probe serial or node serial, as 0x40/0x41/0x0C payloads use.
static Bytes device_payload(int ptype, const Bytes &serial)
{
    return ptype == PRODUCT_TYPE_PROBE ? byte(ptype) + pad(serial, 4) + ZERO10
                                       : byte(ptype) + ZERO4 + encode_serial(serial);
}

Bytes build_device_connected(bool connected, int ptype, const Bytes &serial)
{
    return build_request(connected ? MSG_DEVICE_CONNECTED : MSG_DEVICE_DISCONNECTED,
                         device_payload(ptype, serial));
}

Bytes build_sync_thermometer_list(const Bytes &mac6, const std::vector<Bytes> &slots,
                                  std::optional<uint32_t> request_id)
{
    Bytes payload = pad(mac6, 6);
    for (size_t i = 0; i < 4; i++) {
        bool present = i < slots.size() && !slots[i].empty();
        payload += present ? byte(1) + pad(slots[i], 4) : byte(0) + ZERO4;
    }
    return build_request(MSG_SYNC_THERMOMETER_LIST, payload, request_id);
}

std::optional<std::pair<Bytes, std::array<Bytes, 4>>> parse_sync_thermometer_list(
    const Bytes &payload)
{
    if (payload.size() < 26) return std::nullopt;
    std::array<Bytes, 4> slots;
    for (size_t i = 0; i < 4; i++) {
        size_t off = 6 + 5 * i;
        if (at(payload, off)) slots[i] = payload.substr(off + 1, 4);
    }
    return std::make_pair(payload.substr(0, 6), slots);
}

Bytes build_silence_alarms_response(uint32_t request_id, int ptype, const Bytes &serial)
{
    return build_response(MSG_SILENCE_ALARMS, request_id, true, device_payload(ptype, serial));
}

SilenceAlarms parse_silence_alarms(const Bytes &payload)
{
    Bytes p = payload + Bytes(16, '\0');
    return {at(p, 0) != 0, at(p, 1), p.substr(2, 4), p.substr(6, 10)};
}

// -- link tables -------------------------------------------------------------------------------------

Bytes encode_product_serial(int ptype, const Bytes &serial)
{
    return ptype == PRODUCT_TYPE_PROBE ? pad(serial, 4) + Bytes(8, '\xFF')
                                       : encode_serial(serial) + Bytes(2, '\0');
}

Bytes decode_product_serial(int ptype, const Bytes &b12)
{
    return b12.substr(0, ptype == PRODUCT_TYPE_PROBE ? 4 : 10);
}

Bytes build_link_entries_response(uint8_t msg_type, uint32_t request_id, int page,
                                  const std::vector<LinkEntry> &entries)
{
    const int per = 4;
    int n = entries.size();
    int total_pages = std::max(1, (n + per - 1) / per);
    int from = std::min(n, page * per), to = std::min(n, (page + 1) * per);
    Bytes payload = byte(page) + byte(total_pages) + byte(n) + byte(to - from);
    for (int i = from; i < to; i++) {
        const auto &e = entries[i];
        payload += byte(e.ptype) + encode_product_serial(e.ptype, e.serial);
        put32(payload, e.ts);
        payload += byte(e.legacy ? 1 : 0);
    }
    return build_response(msg_type, request_id, page < total_pages, payload);
}

std::optional<LinkPage> parse_link_entries_response(const Bytes &payload)
{
    if (payload.size() < 4) return std::nullopt;
    LinkPage r;
    r.page = at(payload, 0);
    r.total_pages = at(payload, 1);
    int on_page = at(payload, 3);
    for (int i = 0; i < on_page; i++) {
        size_t off = 4 + 18 * i;
        if (off + 18 > payload.size()) break;
        int t = at(payload, off);
        r.entries.push_back({t, decode_product_serial(t, payload.substr(off + 1, 12)),
                             u32(payload, off + 13), at(payload, off + 17) != 0});
    }
    return r;
}

std::optional<LinkEntry> parse_link_device(const Bytes &payload)
{
    if (payload.size() < 17) return std::nullopt;
    int t = at(payload, 0);
    return LinkEntry{t, decode_product_serial(t, payload.substr(1, 12)), u32(payload, 13), false};
}

Bytes build_link_notification(bool linked, const LinkEntry &entry)
{
    Bytes payload = byte(entry.ptype) + encode_product_serial(entry.ptype, entry.serial);
    put32(payload, entry.ts);
    return build_request(linked ? MSG_DEVICE_LINKED : MSG_DEVICE_UNLINKED, payload);
}

}  // namespace protocol
