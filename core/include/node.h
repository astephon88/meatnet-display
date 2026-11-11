// MeatNet node logic for the Display/repeater (port of meatnet.py).
//
// The Node sits between the radio transport and the rest of the unit:
// - decodes scan observations into a probe table and a device table;
// - keeps outbound links to the probes selected in the web UI (and to the
//   upstream nodes), within the BLE connection budget;
// - answers / routes MeatNet UART requests from phones and peer nodes:
//   probe list, node list, topology, probe-addressed commands (translated to
//   the probe's own UART format), log reads, silence alarms, link tables;
// - floods network messages (0x45 status, heartbeats, link notifications)
//   with request-ID de-duplication and hop counting;
// - builds the advertising rotation (own Display info + repeated probes);
// - bridges selected MEATER Pro probes, publishing each as a Combustion probe.
//
// Message sources/destinations are ports: an inbound connection (phone or
// peer node) or one of our outbound node links.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "linktable.h"
#include "meater.h"
#include "protocol.h"
#include "unit_settings.h"
#include "transport.h"
#include "util.h"

namespace core {

constexpr int64_t PROBE_STALE_MS = 30000;   // forget a probe not heard for this long
constexpr int64_t NODE_STALE_MS = 60000;
constexpr int64_t FRESH_MS = 5000;          // a source younger than this is "live"
constexpr int64_t PENDING_TIMEOUT_MS = 6000;    // probe command without a response
constexpr int64_t LOG_IDLE_MS = 5000;       // log read with no record for this long is done
constexpr int64_t LOG_FIRST_MS = 20000;     // ...but a probe takes ~10 s to start streaming
constexpr int64_t BUSY_TIMEOUT_MS = 20000;  // other commands wait behind a log read this long
constexpr size_t MAX_LOG_READS = 4;         // concurrent log reads kept per probe
constexpr int64_t STRAY_LOG_MS = 30000;     // rate limit for "no request waiting" messages
constexpr int64_t HEARTBEAT_MS = 5000;
constexpr int64_t SYNC_LIST_MS = 30000;
constexpr int64_t FWD_TIMEOUT_MS = 15000;
constexpr int64_t TABLE_PULL_MS = 30 * 60000;   // pull a peer's link table at most this often
constexpr int64_t MEATER_FRESH_MS = 10000;  // stop advertising a MEATER this long after its
                                            // last temperature notification
constexpr uint16_t MEATER_SAMPLE_MS = 5000; // log period of a bridged MEATER (like a probe)
constexpr size_t MEATER_LOG_MAX = 4320;     // records kept per MEATER (6 h at 5 s)
constexpr int LOG_JOB_BURST = 20;           // log responses queued per service() call
constexpr size_t TXQ_HIGH = 150;            // ...while the transport's queue is below this

struct Port {
    bool in = true;             // inbound connection (else one of our node links)
    int conn = 0;
    Bytes key;
    static Port inbound(int c) { return {true, c, Bytes()}; }
    static Port link(const Bytes &k) { return {false, 0, k}; }
    bool operator==(const Port &o) const
    {
        return in == o.in && (in ? conn == o.conn : key == o.key);
    }
    bool operator!=(const Port &o) const { return !(*this == o); }
    bool operator<(const Port &o) const
    {
        if (in != o.in) return in;
        return in ? conn < o.conn : key < o.key;
    }
};

// One probe as the unit sees it, merged from the best source.
struct ProbeReading : protocol::ProbeData {
    std::string serial;         // "1000ABCD"
    std::string src;            // "link", "advert", "node", "repeated", "meater"
    int64_t age_ms = 0;
    std::optional<int> rssi;
    int hops = 0;
    std::optional<protocol::Prediction> prediction;     // from a status
    bool alarming = false;
    std::optional<int> battery_pct;                     // MEATERs
};

struct DeviceReading : protocol::NodeAdvert {
    std::string serial_str;     // "G000001CA8"
    std::string name;           // "Gauge", "Engine", ...
    int rssi = 0;
    int64_t age_ms = 0;
};

struct Pending {
    Bytes probe;
    uint8_t type = 0;
    uint32_t req = 0;
    Port port;
    int64_t at = 0;
    bool range = false;         // a log read with a sequence range
    uint32_t start = 0, end = 0, next = 0;
    bool got = false;           // a log read that has had a record
};

struct ViaNode {
    Bytes status;
    int64_t ticks = 0;
    int hops = 0;
    Port port;
};

struct MyReq {
    std::string purpose;        // "assoc" or "table"
    Port port;
    uint8_t msg_type = 0;
    int page = 0;
    int64_t at = 0;
};

struct MeaterData {
    std::optional<meater::Temps6> temps;
    std::optional<int64_t> at;  // last temperature notification
    std::optional<int> battery;
};

struct MeaterSession {
    Bytes serial;
    uint32_t session = 0;
    uint32_t base = 0;          // sequence number of log.front()
    std::deque<Bytes> log;
    int64_t next = 0;           // when the next sample is due
    Bytes status;
    int64_t at = 0;
};

struct LogJob {
    Bytes addr;
    Port port;
    uint32_t req = 0;
    uint32_t next = 0, end = 0;
};

struct MeaterReading {
    Bytes serial4, msd;
    protocol::ProbeAdvert decoded;
    int64_t age_ms = 0;
};

class Node {
public:
    using LogFn = std::function<void(const std::string &)>;
    Node(Transport &t, Settings &cfg, std::string serial, LogFn log = nullptr,
         LinkTable *links = nullptr);

    void tick(std::optional<int64_t> now = std::nullopt);       // 1 Hz housekeeping
    void service(std::optional<int64_t> now = std::nullopt);    // fast path (~10 Hz)

    // Config helpers.
    std::vector<Bytes> wanted_probes() const;
    std::vector<Bytes> upstream_serials() const;
    bool peer_features() const { return cfg.meatnet_peer; }
    // Send our own traffic (status, heartbeats, syncs, link notices,
    // association) to the upstream node too. Off by default: a Giant Grill
    // Gauge drops a node that writes to it unprompted.
    bool push_upstream() const { return peer_features() && cfg.upstream_push; }
    bool meater_enabled() const { return cfg.meater_bridge; }
    std::map<Bytes, MeaterProbe> meater_selected() const;
    int link_budget() const;    // outbound probe + MEATER links we may hold

    std::optional<MeaterReading> meater_reading(const Bytes &addr, int64_t now);
    bool probe_in_network(const Bytes &serial4);
    std::vector<Bytes> network_slots();
    std::vector<protocol::NodeEntry> node_list();
    static std::string link_name(const Bytes &key, const Link &link);   // serial or address

    Transport &t;
    Settings &cfg;
    std::string serial;
    LogFn log;
    LinkTable *links_table;

    RecentIds ids{192};
    std::map<Bytes, ProbeReading> probes;
    std::map<Bytes, DeviceReading> devices;
    std::map<Bytes, std::pair<Bytes, int64_t>> link_status;     // serial4 -> (status, ticks)
    std::map<Bytes, ViaNode> via_node;                          // probes reachable via a node
    std::array<Bytes, 10> slots;    // probe device numbers (index + 1); "" = free
    std::map<Bytes, int> node_numbers;                          // serial10 -> device number
    std::vector<Pending> pending;   // probe commands awaiting a response
    std::map<uint32_t, std::pair<Port, int64_t>> fwd;           // request id -> who asked
    std::map<uint32_t, MyReq> my_reqs;
    std::map<Port, Bytes> peer_serial;                          // port -> peer node serial10
    std::map<int, Bytes> conn_buf;  // inbound conn -> partial frame bytes
    std::map<Bytes, Bytes> link_buf;
    std::map<Bytes, std::string> link_state;
    std::map<Bytes, int64_t> table_pulled;
    std::map<Bytes, MeaterData> meater_data;
    std::map<Bytes, MeaterSession> meater_sess;
    std::vector<LogJob> log_jobs;   // paced log-read replies for bridged MEATERs
    bool standby = false;           // set by the power manager
    std::map<std::string, int> stats;
    // Traffic relayed between phones and node links, by message type: shows
    // whether the app reaches a node through us.
    std::map<std::string, std::map<std::string, int>> via;

private:
    void logf(const char *fmt, ...);
    std::vector<std::pair<Bytes, Link *>> node_links();
    Link *probe_link(const Bytes &serial4);
    void count_via(const std::string &direction, const Bytes &frame, const Bytes &key = Bytes());
    void send(const Port &port, const Bytes &frame);
    // Send to every port except the one the message came from. links: node
    // links only when push_upstream() (nullopt); true: always.
    void flood(const Bytes &frame, const std::optional<Port> &origin = std::nullopt,
               bool inbound = true, std::optional<bool> links = std::nullopt);

    void service_link(const Bytes &key, Link &link, int64_t now);

    meater::Fields meater_fields(const Bytes &addr);
    Bytes meater_direct(const Bytes &addr);
    void meater_update(const Bytes &addr, int64_t now);
    std::optional<Bytes> meater_for_serial(const Bytes &serial4, std::optional<int64_t> now = std::nullopt);
    void meater_request(const Port &port, const protocol::Frame &f, const Bytes &addr,
                        const Bytes &probe_payload, int64_t now);
    bool probe_device_info(const Port &port, const protocol::Frame &f);
    void meater_device_info(const Port &port, const protocol::Frame &f, const Bytes &addr);
    void meater_link(const Bytes &serial4);
    void service_log_jobs();

    void refresh(int64_t now);
    std::optional<int> assign_slot(const Bytes &serial4);
    int node_number(const Bytes &serial10);
    void manage_links(int64_t now);
    void on_node_link_ready(const Bytes &key);
    void update_adverts(int64_t now);
    bool log_read_active(const Bytes &serial4) const;
    void expire(int64_t now);

    void on_frame(const Port &port, const protocol::Frame &f, int64_t now);
    void on_response(const Port &port, const protocol::Frame &f, int64_t now);
    bool forward_to_owner(const Port &port, const protocol::Frame &f, const Bytes &serial4,
                          int64_t now);
    void route_probe_request(const Port &port, const protocol::Frame &f, int64_t now);
    Pending *match_log_record(const Bytes &serial4, uint32_t seq);
    void on_probe_response(const Bytes &serial4, uint8_t mtype, bool success,
                           const Bytes &payload, int64_t now);
    void stray(const Bytes &serial4, uint8_t mtype, int64_t now, const std::string &what);
    void on_silence(const Port &port, const protocol::Frame &f, int64_t now);
    void on_remote_status(const Port &port, const protocol::Frame &f, int64_t now);
    void on_topology(const Port &port, const protocol::Frame &f, int64_t now);
    void send_heartbeat();
    void on_sync_list(const protocol::Frame &f);
    void request_link_table(const Port &port, uint8_t msg_type, int page);
    void on_my_response(const Port &port, const protocol::Frame &f, const MyReq &mine);
    void on_link_msg(const Port &port, const protocol::Frame &f, int64_t now);

    int64_t now_ = 0;           // time of the current tick()/service() pass
    std::optional<int64_t> hb_at_, sync_at_, stray_at_;
    bool hb_inbound_ = false;
};

}  // namespace core
