#include "node.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <set>

namespace core {

using protocol::Frame;

const std::map<std::string, std::string> &Link::info() const
{
    static const std::map<std::string, std::string> none;
    return none;
}

static uint32_t rid_of(const Bytes &frame)   // request ID of a built frame
{
    return uint8_t(frame[5]) | uint8_t(frame[6]) << 8 | uint8_t(frame[7]) << 16 |
           uint32_t(uint8_t(frame[8])) << 24;
}

static uint32_t u32(const Bytes &b, size_t off)
{
    return uint8_t(b[off]) | uint8_t(b[off + 1]) << 8 | uint8_t(b[off + 2]) << 16 |
           uint32_t(uint8_t(b[off + 3])) << 24;
}

static Bytes le32(uint32_t v)
{
    Bytes b;
    for (int i = 0; i < 4; i++) b += static_cast<char>(v >> (8 * i) & 0xFF);
    return b;
}

static constexpr uint64_t RESPONSE_ID = uint64_t(1) << 32;   // response IDs in `ids`

static bool contains(const std::vector<Bytes> &v, const Bytes &x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

Node::Node(Transport &t_, Settings &cfg_, std::string serial_, LogFn log_, LinkTable *links)
    : t(t_), cfg(cfg_), serial(std::move(serial_)), log(std::move(log_)), links_table(links)
{
    stats = {{"rx", 0}, {"tx", 0}, {"routed", 0}, {"status_sent", 0}};
    via = {{"app_to_node", {}}, {"node_to_app", {}}};
    now_ = ticks_ms();
}

void Node::logf(const char *fmt, ...)
{
    if (!log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(buf);
}

// -- config helpers ----------------------------------------------------------------------

std::vector<Bytes> Node::wanted_probes() const
{
    std::vector<Bytes> out;
    for (const auto &s : cfg.linked_probes) {
        Bytes b = protocol::probe_serial_bytes(s);
        if (!b.empty()) out.push_back(b);
    }
    return out;
}

std::vector<Bytes> Node::upstream_serials() const
{
    std::vector<Bytes> out;
    for (const auto &s : cfg.upstream_node)
        if (!s.empty()) out.push_back(protocol::encode_serial(s));
    return out;
}

std::map<Bytes, MeaterProbe> Node::meater_selected() const
{
    std::map<Bytes, MeaterProbe> out;
    if (!meater_enabled()) return out;
    for (const auto &e : cfg.meater_probes) {
        Bytes a = meater::addr_bytes(e.addr);
        if (a.size() == 6) out[a] = e;
    }
    return out;
}

int Node::link_budget() const
{
    int n = t.max_conn;
    if (!t.separate_radios()) n -= cfg.inbound_reserve;   // one radio: keep room for phones
    n -= static_cast<int>(upstream_serials().size());
    return std::max(0, n);
}

// -- ports ---------------------------------------------------------------------------------

std::vector<std::pair<Bytes, Link *>> Node::node_links()
{
    std::vector<std::pair<Bytes, Link *>> out;
    for (auto &kv : t.links)
        if (kv.second->kind() == "node" && kv.second->ready()) out.push_back({kv.first, kv.second.get()});
    return out;
}

Link *Node::probe_link(const Bytes &serial4)
{
    auto it = t.links.find(serial4);
    if (it != t.links.end() && it->second->kind() == "probe" && it->second->ready())
        return it->second.get();
    return nullptr;
}

void Node::count_via(const std::string &direction, const Bytes &frame, const Bytes &key)
{
    auto &d = via[direction];
    uint8_t mt = frame[4];
    char name[16];
    snprintf(name, sizeof(name), "0x%02x%s", mt & 0x7F, (mt & 0x80) ? " rsp" : "");
    bool first = !d.count(name);
    d[name]++;
    if (first && direction == "app_to_node")
        logf("[route] app reached %s through the Display (0x%s)",
             key.empty() ? "a node" : protocol::serial10_str(key).c_str(), name + 2);
}

void Node::send(const Port &port, const Bytes &frame)
{
    if (port.in) {
        t.send(frame, {port.conn});
    } else {
        auto it = t.links.find(port.key);
        if (it != t.links.end() && it->second->ready()) it->second->write(frame);
    }
    stats["tx"]++;
}

void Node::flood(const Bytes &frame, const std::optional<Port> &origin, bool inbound,
                 std::optional<bool> links)
{
    bool to_links = links ? *links : push_upstream();
    if (inbound) {
        std::vector<int> targets;
        for (int c : t.inbound)
            if (!origin || *origin != Port::inbound(c)) targets.push_back(c);
        if (!targets.empty()) {
            t.send(frame, targets);
            if (origin && !origin->in) count_via("node_to_app", frame);
        }
    }
    if (to_links) {
        for (auto &kl : node_links()) {
            if (!origin || *origin != Port::link(kl.first)) {
                kl.second->write(frame);
                if (origin && origin->in) count_via("app_to_node", frame, kl.first);
            }
        }
    }
}

// -- periodic work -----------------------------------------------------------------------------

void Node::tick(std::optional<int64_t> now_opt)
{
    int64_t now = now_opt ? *now_opt : ticks_ms();
    now_ = now;
    t.set_scan_active(meater_enabled() && !standby);
    refresh(now);
    if (standby) {
        // Drop every link so probes' connection slots are free; keep the
        // tables fresh for the screen/web UI and auto-wake.
        std::vector<Bytes> keys;
        for (auto &kv : t.links) keys.push_back(kv.first);
        for (auto &k : keys) t.close_link(k);
        pending.clear();
        return;
    }
    manage_links(now);
    update_adverts(now);
    expire(now);
    if (peer_features()) {
        if (!hb_at_ || now - *hb_at_ >= HEARTBEAT_MS) {
            hb_at_ = now;
            send_heartbeat();
        }
        if (push_upstream() && !node_links().empty() &&
            (!sync_at_ || now - *sync_at_ >= SYNC_LIST_MS)) {
            sync_at_ = now;
            flood(protocol::build_sync_thermometer_list(t.mac, {slots.begin(), slots.begin() + 4}),
                  std::nullopt, false);
        }
    }
}

void Node::service(std::optional<int64_t> now_opt)
{
    int64_t now = now_opt ? *now_opt : ticks_ms();
    now_ = now;
    for (auto &cd : t.poll_rx()) {
        Bytes buf = conn_buf[cd.first] + cd.second, rest;
        auto frames = protocol::split_frames(buf, rest);
        conn_buf[cd.first] = rest.size() > 512 ? rest.substr(rest.size() - 512) : rest;
        for (auto &f : frames) {
            stats["rx"]++;
            on_frame(Port::inbound(cd.first), f, now);
        }
    }
    for (auto it = conn_buf.begin(); it != conn_buf.end();) {
        if (!t.inbound.count(it->first)) {
            peer_serial.erase(Port::inbound(it->first));
            it = conn_buf.erase(it);
        } else {
            ++it;
        }
    }
    // Copy: handlers may open or close links.
    std::vector<std::pair<Bytes, std::shared_ptr<Link>>> links(t.links.begin(), t.links.end());
    for (auto &kl : links) service_link(kl.first, *kl.second, now);
    if (!log_jobs.empty()) service_log_jobs();
}

void Node::service_link(const Bytes &key, Link &link, int64_t now)
{
    const std::string state = link.state();
    auto prev_it = link_state.find(key);
    if (prev_it == link_state.end() || prev_it->second != state) {
        std::string prev = prev_it == link_state.end() ? "" : prev_it->second;
        link_state[key] = state;
        logf("[link] %s %s: %s", link.kind().c_str(), link_name(key, link).c_str(), state.c_str());
        if (link.kind() == "meater" && peer_features() && (state == "ready" || prev == "ready"))
            flood(protocol::build_device_connected(state == "ready", protocol::PRODUCT_TYPE_PROBE,
                                                   meater::serial_for(key)));
        if (link.kind() == "probe" && peer_features()) {
            if (state == "ready")
                flood(protocol::build_device_connected(true, protocol::PRODUCT_TYPE_PROBE, key));
            else if (prev == "ready")
                flood(protocol::build_device_connected(false, protocol::PRODUCT_TYPE_PROBE, key));
        }
        if (link.kind() == "node" && state == "ready") on_node_link_ready(key);
    }
    if (!link.ready()) return;
    if (link.kind() == "meater") {
        auto temps = link.take("temp");
        if (!temps.empty()) {
            if (auto t6 = meater::decode_temps(temps.back())) {
                auto &d = meater_data[key];
                d.temps = *t6;
                d.at = now;
            }
        }
        auto bat = link.take("battery");
        if (!bat.empty() && !bat.back().empty()) meater_data[key].battery = uint8_t(bat.back()[0]);
        if (!temps.empty()) meater_update(key, now);
        return;
    }
    if (link.kind() == "probe") {
        auto statuses = link.take("status");
        if (!statuses.empty()) {
            const Bytes &status = statuses.back();
            if (status.size() >= 48) {
                link_status[key] = {status, now};
                assign_slot(key);
                Bytes frame = protocol::build_node_probe_status(key, status, 1);
                ids.seen(rid_of(frame));
                flood(frame);
                stats["status_sent"]++;
            }
        }
        auto chunks = link.take("uart");
        if (!chunks.empty()) {
            Bytes buf = link_buf[key], rest;
            for (auto &c : chunks) buf += c;
            auto frames = protocol::split_probe_frames(buf, rest);
            link_buf[key] = rest.size() > 256 ? rest.substr(rest.size() - 256) : rest;
            for (auto &pf : frames) on_probe_response(key, pf.msg_type, pf.success, pf.payload, now);
        }
    } else if (link.kind() == "node") {
        auto chunks = link.take("uart");
        if (!chunks.empty()) {
            Bytes buf = link_buf[key], rest;
            for (auto &c : chunks) buf += c;
            auto frames = protocol::split_frames(buf, rest);
            link_buf[key] = rest.size() > 512 ? rest.substr(rest.size() - 512) : rest;
            for (auto &f : frames) {
                stats["rx"]++;
                on_frame(Port::link(key), f, now);
            }
        }
    }
}

std::string Node::link_name(const Bytes &key, const Link &link)
{
    if (link.kind() == "probe") return protocol::probe_serial_str(key);
    if (link.kind() == "meater") return meater::addr_str(key);
    return protocol::serial10_str(key);
}

// -- bridged MEATERs ---------------------------------------------------------------------------

static bool low_battery(const MeaterData &d)
{
    return d.battery && *d.battery < meater::LOW_BATTERY_PCT;
}

std::optional<MeaterReading> Node::meater_reading(const Bytes &addr, int64_t now)
{
    auto it = meater_data.find(addr);
    auto sel = meater_selected();
    auto s = sel.find(addr);
    if (it == meater_data.end() || !it->second.temps || s == sel.end()) return std::nullopt;
    int64_t age = now - *it->second.at;
    if (age > MEATER_FRESH_MS) return std::nullopt;
    MeaterReading r;
    r.serial4 = meater::serial_for(addr);
    r.msd = meater::build_advert(r.serial4, meater::map_sensors(*it->second.temps), s->second.id, 0,
                                 low_battery(it->second))
                .first;
    r.decoded = *protocol::parse_probe_advert(r.msd.substr(2));
    r.age_ms = age;
    return r;
}

meater::Fields Node::meater_fields(const Bytes &addr)
{
    const MeaterData &d = meater_data[addr];
    auto sel = meater_selected();
    MeaterProbe e = sel.count(addr) ? sel[addr] : MeaterProbe();
    return meater::encode_fields(meater::map_sensors(*d.temps), e.id, e.color, low_battery(d));
}

Bytes Node::meater_direct(const Bytes &addr)
{
    const MeaterData &d = meater_data[addr];
    auto sel = meater_selected();
    MeaterProbe e = sel.count(addr) ? sel[addr] : MeaterProbe();
    return meater::build_direct_advert(meater::serial_for(addr), meater::map_sensors(*d.temps), e.id,
                                       e.color, low_battery(d));
}

// New MEATER reading: log it (every MEATER_SAMPLE_MS), rebuild its Probe
// Status and send it to the network as 0x45.
void Node::meater_update(const Bytes &addr, int64_t now)
{
    if (!meater_selected().count(addr) || !meater_data.count(addr) || !meater_data[addr].temps)
        return;
    auto it = meater_sess.find(addr);
    if (it == meater_sess.end()) {
        MeaterSession s;
        s.serial = meater::serial_for(addr);
        s.session = protocol::new_message_id() | 1;   // 0 = "not supplied"
        s.next = now;
        s.at = now;
        it = meater_sess.emplace(addr, s).first;
        meater_link(s.serial);
    }
    MeaterSession &sess = it->second;
    meater::Fields fields = meater_fields(addr);
    if (now - sess.next >= 0) {
        sess.log.push_back(meater::log_record(fields));
        if (sess.log.size() > MEATER_LOG_MAX) {
            sess.log.pop_front();
            sess.base++;
        }
        sess.next = now + MEATER_SAMPLE_MS;
    }
    uint32_t log_max = sess.base + (sess.log.empty() ? 0 : static_cast<uint32_t>(sess.log.size() - 1));
    sess.status = meater::build_status(fields, sess.base, log_max, sess.session, MEATER_SAMPLE_MS);
    sess.at = now;
    assign_slot(sess.serial);
    Bytes frame = protocol::build_node_probe_status(sess.serial, sess.status, 1);
    ids.seen(rid_of(frame));
    flood(frame);
    stats["status_sent"]++;
}

// Address of the bridged MEATER published as serial4 (fresh data only).
std::optional<Bytes> Node::meater_for_serial(const Bytes &serial4, std::optional<int64_t> now_opt)
{
    int64_t now = now_opt ? *now_opt : now_;
    auto sel = meater_selected();
    for (auto &kv : meater_sess)
        if (kv.second.serial == serial4 && sel.count(kv.first) && now - kv.second.at <= MEATER_FRESH_MS)
            return kv.first;
    return std::nullopt;
}

// Answer a probe-addressed request for a bridged MEATER ourselves.
void Node::meater_request(const Port &port, const Frame &f, const Bytes &addr,
                          const Bytes &probe_payload, int64_t now)
{
    MeaterSession &sess = meater_sess[addr];
    const Bytes serial4 = sess.serial;
    uint8_t mt = f.msg_type;
    if (mt == protocol::MSG_READ_SESSION_INFO) {
        Bytes p = serial4 + le32(sess.session);
        p += static_cast<char>(MEATER_SAMPLE_MS & 0xFF);
        p += static_cast<char>(MEATER_SAMPLE_MS >> 8);
        send(port, protocol::build_response(mt, f.request_id, true, p));
    } else if (mt == protocol::MSG_READ_LOGS && probe_payload.size() >= 8) {
        uint32_t start = u32(probe_payload, 0), end = u32(probe_payload, 4);
        int64_t first = std::max<int64_t>(start, sess.base);
        int64_t last = std::min<int64_t>(end, int64_t(sess.base) + int64_t(sess.log.size()) - 1);
        log_jobs.erase(std::remove_if(log_jobs.begin(), log_jobs.end(),
                                      [&](const LogJob &j) { return j.addr == addr && j.port == port; }),
                       log_jobs.end());
        if (first <= last)
            log_jobs.push_back({addr, port, f.request_id, static_cast<uint32_t>(first),
                                static_cast<uint32_t>(last)});
        else
            send(port, protocol::build_response(mt, f.request_id, false, Bytes()));
    } else if (mt == protocol::MSG_READ_OVER_TEMPERATURE) {
        send(port, protocol::build_response(mt, f.request_id, true, serial4 + Bytes(1, '\0')));
    } else if ((mt == protocol::MSG_SET_PROBE_ID || mt == protocol::MSG_SET_PROBE_COLOR) &&
               !probe_payload.empty()) {
        // Keep the app's choice in the bridge settings.
        bool is_id = mt == protocol::MSG_SET_PROBE_ID;
        int val = uint8_t(probe_payload[0]) + (is_id ? 1 : 0);
        for (auto &e : cfg.meater_probes)
            if (e.addr == meater::addr_str(addr)) (is_id ? e.id : e.color) = val;
        send(port, protocol::build_response(mt, f.request_id, true, Bytes()));
        meater_update(addr, now);
    } else {
        // Predictions, alarms, food safe, power mode, reset: a MEATER can't
        // do these through the Combustion protocol.
        send(port, protocol::build_response(mt, f.request_id, false, Bytes()));
    }
    stats["routed"]++;
}

static std::pair<const char *, size_t> info_field(uint8_t mt)
{
    if (mt == protocol::MSG_PROBE_FIRMWARE_REVISION) return {"firmware", 20};
    if (mt == protocol::MSG_PROBE_HARDWARE_REVISION) return {"hardware", 16};
    return {"model", 50};
}

static Bytes padded(const std::string &text, size_t size)
{
    Bytes raw = text.substr(0, size);
    raw.resize(size, '\0');
    return raw;
}

// Answer 0x46-0x48 for a probe we hold a link to, from the Device
// Information strings the link read when it connected.
bool Node::probe_device_info(const Port &port, const Frame &f)
{
    Bytes serial4 = f.payload.substr(0, 4);
    Link *link = probe_link(serial4);
    if (!link) return false;
    auto field = info_field(f.msg_type);
    auto it = link->info().find(field.first);
    if (it == link->info().end()) return false;
    send(port, protocol::build_response(f.msg_type, f.request_id, true,
                                        serial4 + padded(it->second, field.second)));
    stats["routed"]++;
    return true;
}

void Node::meater_device_info(const Port &port, const Frame &f, const Bytes &addr)
{
    uint8_t mt = f.msg_type;
    const char *text = mt == protocol::MSG_PROBE_FIRMWARE_REVISION ? meater::DIS_FIRMWARE
                     : mt == protocol::MSG_PROBE_HARDWARE_REVISION ? meater::DIS_HARDWARE
                                                                   : meater::DIS_MODEL;
    send(port, protocol::build_response(mt, f.request_id, true,
                                        meater_sess[addr].serial + padded(text, info_field(mt).second)));
    stats["routed"]++;
}

// Record a bridged MEATER as a linked MeatNet device and announce it (Device
// Linked, 0x77), the way a node links a probe.
void Node::meater_link(const Bytes &serial4)
{
    if (!links_table) return;
    protocol::LinkEntry entry{protocol::PRODUCT_TYPE_PROBE, serial4, unix_time(), false};
    if (links_table->link(entry)) {
        Bytes note = protocol::build_link_notification(true, entry);
        ids.seen(rid_of(note));
        flood(note);
        logf("[meater] %s linked to MeatNet", protocol::probe_serial_str(serial4).c_str());
    }
}

void Node::service_log_jobs()
{
    int budget = LOG_JOB_BURST;
    for (size_t j = 0; j < log_jobs.size();) {
        LogJob &job = log_jobs[j];
        auto s = meater_sess.find(job.addr);
        if (s == meater_sess.end()) {
            log_jobs.erase(log_jobs.begin() + j);
            continue;
        }
        MeaterSession &sess = s->second;
        while (budget > 0 && job.next <= job.end) {
            if (t.txq_size() >= TXQ_HIGH) return;
            int64_t i = int64_t(job.next) - sess.base;
            if (i >= 0 && i < int64_t(sess.log.size())) {
                send(job.port, protocol::build_response(protocol::MSG_READ_LOGS, job.req, true,
                                                        sess.serial + le32(job.next) + sess.log[i]));
                budget--;
            }
            job.next++;
        }
        if (job.next > job.end) {
            log_jobs.erase(log_jobs.begin() + j);
        } else {
            ++j;
        }
        if (budget <= 0) return;
    }
}

// -- scan data -------------------------------------------------------------------------------------

void Node::refresh(int64_t now)
{
    auto prune = [&](std::map<Bytes, Observation> &d, int64_t limit) {
        for (auto it = d.begin(); it != d.end();)
            it = now - it->second.ticks > limit ? d.erase(it) : std::next(it);
    };
    prune(t.direct, PROBE_STALE_MS);
    prune(t.repeated, PROBE_STALE_MS);
    prune(t.nodes, NODE_STALE_MS);
    for (auto it = link_status.begin(); it != link_status.end();)
        it = now - it->second.second > PROBE_STALE_MS ? link_status.erase(it) : std::next(it);
    for (auto it = via_node.begin(); it != via_node.end();)
        it = now - it->second.ticks > PROBE_STALE_MS ? via_node.erase(it) : std::next(it);
    for (auto it = t.meaters.begin(); it != t.meaters.end();)
        it = now - it->second.ticks > NODE_STALE_MS && !t.links.count(it->first) ? t.meaters.erase(it)
                                                                                 : std::next(it);
    for (auto it = meater_data.begin(); it != meater_data.end();)
        it = now - it->second.at.value_or(now) > PROBE_STALE_MS ? meater_data.erase(it) : std::next(it);
    auto sel = meater_selected();
    for (auto it = meater_sess.begin(); it != meater_sess.end();)   // un-bridged: forget it
        it = !sel.count(it->first) ? meater_sess.erase(it) : std::next(it);

    std::map<Bytes, ProbeReading> out;
    std::map<Bytes, int> rank_of;
    auto put = [&](const Bytes &serial4, ProbeReading d, int64_t ticks, const char *src, int rank,
                   std::optional<int> rssi) {
        int64_t age = now - ticks;
        auto cur = out.find(serial4);
        bool live = age < FRESH_MS;
        if (cur != out.end()) {
            bool cur_live = cur->second.age_ms < FRESH_MS;
            if (cur_live && !live) return;
            int cur_rank = rank_of[serial4];
            if (cur_live == live && (cur_rank < rank || (cur_rank == rank && cur->second.age_ms <= age)))
                return;
        }
        d.serial = protocol::probe_serial_str(serial4);
        d.src = src;
        d.age_ms = age;
        if (rssi)
            d.rssi = rssi;
        else if (cur != out.end())
            d.rssi = cur->second.rssi;
        out[serial4] = d;
        rank_of[serial4] = rank;
    };
    auto from_status = [](const protocol::ProbeStatus &s, int hops) {
        ProbeReading r;
        static_cast<protocol::ProbeData &>(r) = s;
        r.prediction = s.prediction;
        r.alarming = s.alarming;
        r.hops = hops;
        return r;
    };
    auto from_advert = [](const protocol::ProbeAdvert &a) {
        ProbeReading r;
        static_cast<protocol::ProbeData &>(r) = a;
        r.hops = a.hops;
        return r;
    };
    for (auto &kv : link_status)
        if (auto d = protocol::decode_probe_status(kv.second.first))
            put(kv.first, from_status(*d, 0), kv.second.second, "link", 0, std::nullopt);
    for (auto &kv : t.direct)
        if (auto d = protocol::parse_probe_advert(kv.second.body))
            put(kv.first, from_advert(*d), kv.second.ticks, "advert", 1, kv.second.rssi);
    for (auto &kv : via_node)
        if (auto d = protocol::decode_probe_status(kv.second.status))
            put(kv.first, from_status(*d, kv.second.hops), kv.second.ticks, "node", 2, std::nullopt);
    for (auto &kv : t.repeated)
        if (auto d = protocol::parse_probe_advert(kv.second.body))
            put(kv.first, from_advert(*d), kv.second.ticks, "repeated", 3, std::nullopt);
    std::vector<Bytes> addrs;
    for (auto &kv : meater_data) addrs.push_back(kv.first);
    for (auto &addr : addrs) {
        if (auto r = meater_reading(addr, now)) {
            ProbeReading d = from_advert(r->decoded);
            d.hops = 0;
            d.battery_pct = meater_data[addr].battery;
            auto obs = t.meaters.find(addr);
            put(r->serial4, d, *meater_data[addr].at, "meater", 0,
                obs != t.meaters.end() ? std::optional<int>(obs->second.rssi) : std::nullopt);
        }
    }
    // Keep RSSI from the direct advert even when another source wins.
    for (auto &kv : t.direct)
        if (out.count(kv.first)) out[kv.first].rssi = kv.second.rssi;
    probes = std::move(out);

    std::map<Bytes, DeviceReading> devs;
    for (auto &kv : t.nodes) {
        auto d = protocol::parse_node_advert(kv.second.body);
        if (!d) continue;
        DeviceReading r;
        static_cast<protocol::NodeAdvert &>(r) = *d;
        r.serial_str = protocol::serial10_str(kv.first);
        const char *name = protocol::product_name(d->ptype);
        r.name = *name ? name : "Device";
        r.rssi = kv.second.rssi;
        r.age_ms = now - kv.second.ticks;
        devs[kv.first] = r;
    }
    devices = std::move(devs);
}

bool Node::probe_in_network(const Bytes &serial4)
{
    return (link_status.count(serial4) && probe_link(serial4)) || via_node.count(serial4) ||
           meater_for_serial(serial4).has_value();
}

std::optional<int> Node::assign_slot(const Bytes &serial4)
{
    for (int i = 0; i < 10; i++)
        if (slots[i] == serial4) return i + 1;
    for (int i = 0; i < 10; i++)
        if (slots[i].empty()) {
            slots[i] = serial4;
            return i + 1;
        }
    // Full: recycle a slot whose probe is no longer reachable.
    for (int i = 0; i < 10; i++)
        if (!probe_in_network(slots[i])) {
            slots[i] = serial4;
            return i + 1;
        }
    return std::nullopt;
}

int Node::node_number(const Bytes &serial10)
{
    auto it = node_numbers.find(serial10);
    if (it != node_numbers.end()) return it->second;
    int n = 21 + static_cast<int>(node_numbers.size());
    node_numbers[serial10] = n;
    return n;
}

// -- links ---------------------------------------------------------------------------------------------

void Node::manage_links(int64_t)
{
    std::vector<Bytes> wanted = wanted_probes();
    auto meaters = meater_selected();
    int budget = link_budget();
    std::vector<Bytes> drop;
    for (auto &kv : t.links) {
        const std::string &k = kv.second->kind();
        if ((k == "probe" && !contains(wanted, kv.first)) || (k == "meater" && !meaters.count(kv.first)))
            drop.push_back(kv.first);
    }
    for (auto &k : drop) t.close_link(k);
    // Probes and MEATERs share the outbound budget.
    std::vector<Bytes> active;
    for (auto &kv : t.links)
        if ((kv.second->kind() == "probe" || kv.second->kind() == "meater") && kv.second->enabled())
            active.push_back(kv.first);
    for (auto &s : wanted) {
        if (contains(active, s)) continue;
        if (static_cast<int>(active.size()) >= budget) break;
        auto obs = t.direct.find(s);
        if (obs == t.direct.end()) continue;    // need to hear it directly for its address
        t.open_link(s, "probe", obs->second.addr_type, obs->second.addr);
        active.push_back(s);
    }
    for (auto &kv : meaters) {
        if (contains(active, kv.first)) continue;
        if (static_cast<int>(active.size()) >= budget) break;
        auto obs = t.meaters.find(kv.first);
        if (obs == t.meaters.end()) continue;   // not heard yet (needs its address type)
        t.open_link(kv.first, "meater", obs->second.addr_type, kv.first);
        active.push_back(kv.first);
    }
    std::vector<Bytes> ups = upstream_serials();
    drop.clear();
    for (auto &kv : t.links)
        if (kv.second->kind() == "node" && !contains(ups, kv.first)) drop.push_back(kv.first);
    for (auto &k : drop) t.close_link(k);
    for (auto &up : ups) {
        if (t.links.count(up)) continue;
        auto obs = t.nodes.find(up);
        if (obs != t.nodes.end()) t.open_link(up, "node", obs->second.addr_type, obs->second.addr);
    }
}

void Node::on_node_link_ready(const Bytes &key)
{
    Port port = Port::link(key);
    peer_serial[port] = key;
    if (!push_upstream()) return;
    // Ask the node to associate with us, as a node joining another does.
    Bytes frame = protocol::build_request(protocol::MSG_ASSOCIATE_NODE, Bytes());
    uint32_t rid = rid_of(frame);
    ids.seen(rid);
    my_reqs[rid] = {"assoc", port, 0, 0, now_};
    send(port, frame);
    // Its link table rarely changes; don't re-pull it on every reconnect.
    auto last = table_pulled.find(key);
    if (links_table && (last == table_pulled.end() || now_ - last->second > TABLE_PULL_MS)) {
        table_pulled[key] = now_;
        request_link_table(port, protocol::MSG_READ_LINKED_DEVICES, 0);
        request_link_table(port, protocol::MSG_READ_UNLINKED_DEVICES, 0);
    }
}

// -- advertising -------------------------------------------------------------------------------------------

void Node::update_adverts(int64_t now)
{
    std::vector<Transport::Advert> payloads = {{protocol::build_display_advertisement(serial), false}};
    bool fast = false;
    const std::string &mode = cfg.repeat_adverts;
    std::vector<Bytes> wanted = wanted_probes();
    std::set<Bytes> done;
    if (mode != "off") {
        for (auto &kv : t.direct) {
            if (now - kv.second.ticks > 10000) continue;
            if (mode == "linked" && !contains(wanted, kv.first)) continue;
            payloads.push_back({protocol::repeat_probe_advert(kv.second.body, 1), false});
            done.insert(kv.first);
            if (kv.second.body.size() > 18 && (kv.second.body[18] & 0x03) == protocol::PROBE_MODE_INSTANT_READ)
                fast = true;
        }
        for (auto &kv : link_status) {
            if (done.count(kv.first) || now - kv.second.second > 10000) continue;
            payloads.push_back({protocol::probe_advert_from_status(kv.first, kv.second.first, 1), false});
            if (kv.second.first.size() > 21 &&
                (kv.second.first[21] & 0x03) == protocol::PROBE_MODE_INSTANT_READ)
                fast = true;
        }
    }
    // Bridged MEATERs, published as Combustion probes (always: bridging is
    // the point of selecting them).
    std::vector<Bytes> addrs;
    for (auto &kv : meater_data) addrs.push_back(kv.first);
    for (auto &addr : addrs) {
        if (auto r = meater_reading(addr, now)) {
            payloads.push_back({r->msd, false});
            if (cfg.meater_direct_adverts) payloads.push_back({meater_direct(addr), true});
        }
    }
    t.set_advertising(payloads, fast);
}

// -- timeouts ------------------------------------------------------------------------------------------------

// A probe answers other commands only after streaming a log read.
bool Node::log_read_active(const Bytes &serial4) const
{
    for (auto &q : pending)
        if (q.probe == serial4 && q.type == protocol::MSG_READ_LOGS) return true;
    return false;
}

void Node::expire(int64_t now)
{
    std::vector<Pending> keep;
    for (auto &p : pending) {
        int64_t age = now - p.at;
        if (p.type == protocol::MSG_READ_LOGS) {
            if (age > (p.got ? LOG_IDLE_MS : LOG_FIRST_MS)) continue;
        } else if (age > (log_read_active(p.probe) ? BUSY_TIMEOUT_MS : PENDING_TIMEOUT_MS)) {
            if (p.type != protocol::MSG_SILENCE_ALARMS)
                send(p.port, protocol::build_response(p.type, p.req, false, Bytes()));
            logf("[route] 0x%02x to %s timed out", p.type, protocol::probe_serial_str(p.probe).c_str());
            continue;
        }
        keep.push_back(p);
    }
    pending = std::move(keep);
    for (auto it = fwd.begin(); it != fwd.end();)
        it = now - it->second.second > FWD_TIMEOUT_MS ? fwd.erase(it) : std::next(it);
    for (auto it = my_reqs.begin(); it != my_reqs.end();)
        it = now - it->second.at > FWD_TIMEOUT_MS ? my_reqs.erase(it) : std::next(it);
}

// -- frame handling ------------------------------------------------------------------------------------------

void Node::on_frame(const Port &port, const Frame &f, int64_t now)
{
    if (f.is_response) {
        on_response(port, f, now);
        return;
    }
    if (ids.seen(f.request_id)) return;
    uint8_t mt = f.msg_type;
    using namespace protocol;
    if (mt == MSG_READ_PROBE_LIST) {
        send(port, build_read_probe_list_response(f.request_id, network_slots()));
    } else if (mt == MSG_READ_NODE_LIST) {
        int page = f.payload.empty() ? 0 : uint8_t(f.payload[0]);
        send(port, build_read_node_list_response(f.request_id, page, node_list()));
    } else if (mt == MSG_READ_NETWORK_TOPOLOGY) {
        on_topology(port, f, now);
    } else if (probe_addressed(mt)) {
        route_probe_request(port, f, now);
    } else if (mt == MSG_SILENCE_ALARMS) {
        on_silence(port, f, now);
    } else if (mt == MSG_PROBE_STATUS) {
        on_remote_status(port, f, now);
    } else if (mt == MSG_HEARTBEAT) {
        if (auto hb = parse_heartbeat(f.payload)) {
            peer_serial[port] = hb->serial;
            if (hb->ptype != PRODUCT_TYPE_UNKNOWN) node_number(hb->serial);
        }
        if (auto fw = bump_heartbeat(f)) flood(*fw, port);
    } else if (mt == MSG_SYNC_THERMOMETER_LIST) {
        on_sync_list(f);
        flood(f.raw, port, false);
    } else if (mt == MSG_ASSOCIATE_NODE) {
        send(port, build_response(mt, f.request_id, true, Bytes()));
    } else if (mt == MSG_LINK_DEVICE || mt == MSG_UNLINK_DEVICE || mt == MSG_DEVICE_LINKED ||
               mt == MSG_DEVICE_UNLINKED || mt == MSG_READ_LINKED_DEVICES ||
               mt == MSG_READ_UNLINKED_DEVICES || mt == MSG_SET_PROXIMITY_LINKING ||
               mt == MSG_READ_PROXIMITY_LINKING || mt == MSG_SYNC_LINK_TABLE) {
        on_link_msg(port, f, now);
    } else if (mt == MSG_PROBE_FIRMWARE_REVISION || mt == MSG_PROBE_HARDWARE_REVISION ||
               mt == MSG_PROBE_MODEL_INFORMATION) {
        Bytes serial4 = f.payload.substr(0, 4);
        if (auto addr = meater_for_serial(serial4, now)) {
            meater_device_info(port, f, *addr);
        } else if (probe_device_info(port, f)) {
        } else if (!forward_to_owner(port, f, serial4, now)) {
            logf("[route] 0x%02x for %s: no answer available", mt, probe_serial_str(serial4).c_str());
        }
    } else {
        // Gauge/Engine status, device connected/disconnected, requests for
        // other nodes (e.g. Read Gauge Logs 0x62, Set Engine Set Point):
        // propagate, remembering who asked so the replies find their way back.
        fwd[f.request_id] = {port, now};
        flood(f.raw, port, true, port.in);
    }
}

void Node::on_response(const Port &port, const Frame &f, int64_t now)
{
    if (ids.seen(RESPONSE_ID | f.response_id)) return;
    auto mine = my_reqs.find(f.request_id);
    if (mine != my_reqs.end()) {
        MyReq m = mine->second;
        on_my_response(port, f, m);
        return;
    }
    auto fw = fwd.find(f.request_id);
    if (fw != fwd.end()) {
        Port to = fw->second.first;
        send(to, f.raw);
        if (!port.in && to.in) count_via("node_to_app", f.raw);
        // Log reads (probe 0x04, gauge 0x62) answer with many responses:
        // keep the route open until it goes quiet (FWD_TIMEOUT_MS).
        fw->second.second = now;
    }
}

// -- probe routing --------------------------------------------------------------------------------------------

// Pass a request on to the upstream node that has the probe.
bool Node::forward_to_owner(const Port &port, const Frame &f, const Bytes &serial4, int64_t now)
{
    auto via = via_node.find(serial4);
    if (via == via_node.end() || via->second.port == port) return false;
    Port to = via->second.port;
    fwd[f.request_id] = {port, now};
    send(to, f.raw);
    if (port.in && !to.in) count_via("app_to_node", f.raw, to.key);
    stats["routed"]++;
    return true;
}

void Node::route_probe_request(const Port &port, const Frame &f, int64_t now)
{
    auto r = protocol::node_to_probe(f.msg_type, f.payload);
    if (!r) return;
    const Bytes &serial4 = r->first, &probe_payload = r->second;
    if (auto addr = meater_for_serial(serial4, now)) {
        meater_request(port, f, *addr, probe_payload, now);
        return;
    }
    if (Link *link = probe_link(serial4)) {
        Pending p;
        p.probe = serial4;
        p.type = f.msg_type;
        p.req = f.request_id;
        p.port = port;
        p.at = now;
        if (f.msg_type == protocol::MSG_READ_LOGS && probe_payload.size() >= 8) {
            p.range = true;
            p.start = u32(probe_payload, 0);
            p.end = u32(probe_payload, 4);
            p.next = p.start;
            // The app overlaps log reads (e.g. re-requesting a range); keep
            // them all and match records by sequence number, dropping the
            // oldest beyond MAX_LOG_READS.
            std::vector<size_t> reads;
            for (size_t i = 0; i < pending.size(); i++)
                if (pending[i].probe == serial4 && pending[i].type == protocol::MSG_READ_LOGS)
                    reads.push_back(i);
            size_t drop = reads.size() + 1 > MAX_LOG_READS ? reads.size() + 1 - MAX_LOG_READS : 0;
            for (size_t k = drop; k-- > 0;) pending.erase(pending.begin() + reads[k]);
        }
        pending.push_back(p);
        link->write(protocol::build_probe_request(f.msg_type, probe_payload));
        stats["routed"]++;
        return;
    }
    if (!forward_to_owner(port, f, serial4, now))
        logf("[route] 0x%02x for %s: probe not in network", f.msg_type,
             protocol::probe_serial_str(serial4).c_str());
}

// The log read this record belongs to. The probe answers overlapping reads
// one after the other, so take the oldest read whose range holds seq and
// that hasn't had it yet; else the newest whose range holds it.
Pending *Node::match_log_record(const Bytes &serial4, uint32_t seq)
{
    Pending *newest = nullptr;
    for (auto &p : pending) {
        if (p.probe == serial4 && p.type == protocol::MSG_READ_LOGS && p.range && p.start <= seq &&
            seq <= p.end) {
            if (seq >= p.next) return &p;
            newest = &p;
        }
    }
    return newest;
}

void Node::on_probe_response(const Bytes &serial4, uint8_t mtype, bool success, const Bytes &payload,
                             int64_t now)
{
    if (mtype == protocol::MSG_READ_LOGS && success && payload.size() >= 4) {
        // A log record: route it by sequence number. (Failed replies carry
        // no valid record; they end the oldest open read, below.)
        uint32_t seq = u32(payload, 0);
        Pending *p = match_log_record(serial4, seq);
        if (!p) {
            stray(serial4, mtype, now, "log record " + std::to_string(seq));
            return;
        }
        send(p->port, protocol::build_response(mtype, p->req, true,
                                               protocol::probe_to_node_payload(serial4, payload)));
        p->at = now;
        p->got = true;
        p->next = std::max(p->next, seq + 1);
        if (seq >= p->end) pending.erase(pending.begin() + (p - pending.data()));
        return;
    }
    auto it = std::find_if(pending.begin(), pending.end(),
                           [&](const Pending &q) { return q.probe == serial4 && q.type == mtype; });
    if (it == pending.end()) {
        if (mtype == protocol::MSG_READ_LOGS && !success) {
            // A probe ends/gaps a log read with a "failed" reply whose payload
            // is leftover buffer bytes; after the read is done it's expected
            // noise: count it, don't log it.
            stats["log_fail_late"]++;
            return;
        }
        stray(serial4, mtype, now,
              std::string(success ? "ok" : "failed") + ", " + std::to_string(payload.size()) + " bytes");
        return;
    }
    Pending p = *it;
    pending.erase(it);
    if (mtype == protocol::MSG_SILENCE_ALARMS) {
        send(p.port, protocol::build_silence_alarms_response(p.req, protocol::PRODUCT_TYPE_PROBE, serial4));
        return;
    }
    send(p.port, protocol::build_response(mtype, p.req, success,
                                          protocol::probe_to_node_payload(serial4, payload)));
}

// A probe response no request is waiting for (e.g. a log record after its
// read timed out): ignore it, logging at most every 30 s.
void Node::stray(const Bytes &serial4, uint8_t mtype, int64_t now, const std::string &what)
{
    int n = ++stats["stray"];
    if (!stray_at_ || now - *stray_at_ >= STRAY_LOG_MS) {
        stray_at_ = now;
        logf("[route] %s response 0x%02x%s with no request waiting (%d so far)",
             protocol::probe_serial_str(serial4).c_str(), mtype,
             what.empty() ? "" : (" (" + what + ")").c_str(), n);
    }
}

void Node::on_silence(const Port &port, const Frame &f, int64_t now)
{
    auto s = protocol::parse_silence_alarms(f.payload);
    std::vector<Bytes> targets;
    if (s.global) {
        for (auto &kv : t.links)
            if (kv.second->kind() == "probe" && kv.second->ready()) targets.push_back(kv.first);
        flood(f.raw, port, true, port.in);
    } else if (s.ptype == protocol::PRODUCT_TYPE_PROBE) {
        if (probe_link(s.serial4))
            targets.push_back(s.serial4);
        else
            forward_to_owner(port, f, s.serial4, now);
    }
    for (auto &probe : targets) {
        Pending p;
        p.probe = probe;
        p.type = protocol::MSG_SILENCE_ALARMS;
        p.req = f.request_id;
        p.port = port;
        p.at = now;
        pending.push_back(p);
        probe_link(probe)->write(protocol::build_probe_request(protocol::MSG_SILENCE_ALARMS));
    }
    if (s.global ||
        (s.ptype != protocol::PRODUCT_TYPE_PROBE && s.serial10 == protocol::encode_serial(serial)))
        send(port, protocol::build_silence_alarms_response(f.request_id, protocol::PRODUCT_TYPE_DISPLAY,
                                                           serial));
}

// 0x45 from another node: remember it (we can route to the probe via that
// port) and pass it on one hop further.
void Node::on_remote_status(const Port &port, const Frame &f, int64_t now)
{
    auto r = protocol::parse_node_probe_status(f.payload);
    if (!r) return;
    if (!link_status.count(r->serial4)) {
        via_node[r->serial4] = {r->status, now, r->hops + 1, port};
        assign_slot(r->serial4);
    }
    if (auto fw = protocol::bump_node_probe_status(f)) flood(*fw, port);
}

void Node::on_topology(const Port &port, const Frame &f, int64_t now)
{
    if (f.payload.substr(0, 10) != protocol::encode_serial(serial)) {
        for (auto &kl : node_links()) {
            if (Port::link(kl.first) != port) {
                fwd[f.request_id] = {port, now};
                send(Port::link(kl.first), f.raw);
            }
        }
        return;
    }
    std::vector<protocol::Conn> in, out;
    for (auto &kv : peer_serial)
        if (kv.first.in) in.push_back({node_number(kv.second), 0});
    std::vector<std::pair<Bytes, std::shared_ptr<Link>>> links(t.links.begin(), t.links.end());
    for (auto &kl : links) {
        Link &link = *kl.second;
        if (!link.ready()) continue;
        if (link.kind() == "probe") {
            auto obs = t.direct.find(kl.first);
            out.push_back({assign_slot(kl.first).value_or(0), obs != t.direct.end() ? obs->second.rssi : 0});
        } else if (link.kind() == "meater") {
            auto obs = t.meaters.find(kl.first);
            out.push_back({assign_slot(meater::serial_for(kl.first)).value_or(0),
                           obs != t.meaters.end() ? obs->second.rssi : 0});
        } else if (link.kind() == "node") {
            auto obs = t.nodes.find(kl.first);
            out.push_back({node_number(kl.first), obs != t.nodes.end() ? obs->second.rssi : 0});
        }
    }
    send(port, protocol::build_topology_response(f.request_id, 20, protocol::PRODUCT_TYPE_DISPLAY, serial,
                                                 in, out));
}

// Probe List slots: only probes we can actually reach.
std::vector<Bytes> Node::network_slots()
{
    std::vector<Bytes> out;
    for (auto &s : slots) out.push_back(!s.empty() && probe_in_network(s) ? s : Bytes());
    return out;
}

std::vector<protocol::NodeEntry> Node::node_list()
{
    std::vector<protocol::NodeEntry> nodes = {{20, protocol::PRODUCT_TYPE_DISPLAY, serial}};
    std::set<Bytes> seen;
    std::vector<Bytes> serials;
    for (auto &kv : peer_serial) serials.push_back(kv.second);
    for (auto &s : serials) {
        if (!seen.insert(s).second) continue;
        auto d = devices.find(s);
        int ptype = d != devices.end() ? d->second.ptype : protocol::PRODUCT_TYPE_UNKNOWN;
        nodes.push_back({node_number(s), ptype, s});
    }
    return nodes;
}

// -- heartbeat / thermometer list -------------------------------------------------------------------------------

void Node::send_heartbeat()
{
    hb_inbound_ = !hb_inbound_;
    std::vector<protocol::HeartbeatRecord> records;
    if (hb_inbound_) {
        for (auto &kv : peer_serial) {
            if (!kv.first.in) continue;
            auto d = devices.find(kv.second);
            records.push_back({d != devices.end() ? d->second.ptype : protocol::PRODUCT_TYPE_DISPLAY,
                               kv.second, d != devices.end() ? d->second.rssi : 0});
        }
    } else {
        for (auto &kv : t.links) {
            Link &link = *kv.second;
            if (!link.ready()) continue;
            if (link.kind() == "probe") {
                auto obs = t.direct.find(kv.first);
                records.push_back({protocol::PRODUCT_TYPE_PROBE, kv.first,
                                   obs != t.direct.end() ? obs->second.rssi : 0});
            } else if (link.kind() == "meater") {
                auto obs = t.meaters.find(kv.first);
                records.push_back({protocol::PRODUCT_TYPE_PROBE, meater::serial_for(kv.first),
                                   obs != t.meaters.end() ? obs->second.rssi : 0});
            } else if (link.kind() == "node") {
                auto d = devices.find(kv.first);
                records.push_back({d != devices.end() ? d->second.ptype : protocol::PRODUCT_TYPE_DISPLAY,
                                   kv.first, d != devices.end() ? d->second.rssi : 0});
            }
        }
    }
    if (records.size() > 4) records.resize(4);
    Bytes frame = protocol::build_heartbeat(serial, t.mac, protocol::PRODUCT_TYPE_DISPLAY, hb_inbound_,
                                            records);
    ids.seen(rid_of(frame));
    flood(frame);
}

void Node::on_sync_list(const Frame &f)
{
    auto r = protocol::parse_sync_thermometer_list(f.payload);
    if (!r) return;
    for (int i = 0; i < 4; i++) {
        const Bytes &s = r->second[i];
        if (s.empty() || slots[i] == s) continue;
        for (auto &x : slots)
            if (x == s) x.clear();
        Bytes displaced = slots[i];
        slots[i] = s;
        if (!displaced.empty()) assign_slot(displaced);
    }
}

// -- link tables ---------------------------------------------------------------------------------------------------

void Node::request_link_table(const Port &port, uint8_t msg_type, int page)
{
    Bytes frame = protocol::build_request(msg_type, Bytes(1, static_cast<char>(page)));
    uint32_t rid = rid_of(frame);
    ids.seen(rid);
    my_reqs[rid] = {"table", port, msg_type, page, now_};
    send(port, frame);
}

void Node::on_my_response(const Port &port, const Frame &f, const MyReq &mine)
{
    my_reqs.erase(f.request_id);
    if (mine.purpose == "assoc") {
        logf("[link] node %s %s association",
             mine.port.in ? "?" : protocol::serial10_str(mine.port.key).c_str(),
             f.success ? "accepted" : "refused");
        return;
    }
    if (mine.purpose != "table" || !f.success || !links_table) return;
    auto r = protocol::parse_link_entries_response(f.payload);
    if (!r) return;
    auto ps = peer_serial.find(port);
    links_table->merge(r->entries, mine.msg_type == protocol::MSG_READ_LINKED_DEVICES, serial,
                       ps != peer_serial.end() ? ps->second : Bytes());
    if (r->page + 1 < r->total_pages) request_link_table(port, mine.msg_type, r->page + 1);
}

void Node::on_link_msg(const Port &port, const Frame &f, int64_t)
{
    LinkTable *lt = links_table;
    uint8_t mt = f.msg_type;
    if (!lt) return;
    using namespace protocol;
    auto peer = [&]() {
        auto ps = peer_serial.find(port);
        return ps != peer_serial.end() ? ps->second : Bytes();
    };
    if (mt == MSG_READ_LINKED_DEVICES || mt == MSG_READ_UNLINKED_DEVICES) {
        int page = f.payload.empty() ? 0 : uint8_t(f.payload[0]);
        send(port, build_link_entries_response(mt, f.request_id, page,
                                               mt == MSG_READ_LINKED_DEVICES ? lt->linked : lt->unlinked));
    } else if (mt == MSG_LINK_DEVICE || mt == MSG_UNLINK_DEVICE) {
        auto e = parse_link_device(f.payload);
        if (e) {
            bool linked = mt == MSG_LINK_DEVICE;
            linked ? lt->link(*e) : lt->unlink(*e);
            Bytes note = build_link_notification(linked, *e);
            ids.seen(rid_of(note));
            flood(note);
        }
        send(port, build_response(mt, f.request_id, e.has_value(), Bytes()));
    } else if (mt == MSG_DEVICE_LINKED || mt == MSG_DEVICE_UNLINKED) {
        if (auto e = parse_link_device(f.payload)) lt->merge({*e}, mt == MSG_DEVICE_LINKED, serial, peer());
        flood(f.raw, port);
    } else if (mt == MSG_SET_PROXIMITY_LINKING) {
        lt->proximity = !f.payload.empty() && f.payload[0];
        lt->save();
        send(port, build_response(mt, f.request_id, true, Bytes()));
    } else if (mt == MSG_READ_PROXIMITY_LINKING) {
        send(port, build_response(mt, f.request_id, true, Bytes(1, lt->proximity ? 1 : 0)));
    } else if (mt == MSG_SYNC_LINK_TABLE) {
        send(port, build_response(mt, f.request_id, true, Bytes()));
        request_link_table(port, MSG_READ_LINKED_DEVICES, 0);
        request_link_table(port, MSG_READ_UNLINKED_DEVICES, 0);
    }
}

}  // namespace core
