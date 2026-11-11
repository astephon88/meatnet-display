// Two-unit mode (port of split.py): a phone-side "app" unit and a
// device-side "field" unit coordinating over WiFi.
//
// The field unit is a remote radio: it scans and holds the outbound BLE links
// (probes, MEATERs, upstream Gauge/Engine nodes) and runs no MeatNet logic.
// The app unit runs the Node on a RemoteRadio, which mirrors the field unit's
// scan tables and links and hands phones/advertising to its own peripheral.
//
// Wire protocol: TCP (the app unit listens on the split port),
// newline-delimited JSON, binary as base64, keys as hex. The field unit finds
// the app unit by a configured address or the app unit's UDP beacon.
//   both:  hello {role, serial, key, max_conn, mac}, ping {ts}, pong {ts}
//   field: scan {d, r, n, m}          scan-table entries updated since last batch
//          link {k, kind, state, desc, mtu, count, info, stats}
//          rx   {k, sub, data}         a link notification
//   app:   open {k, kind, at, addr}, close {k}, write {k, data},
//          scan_active {on}, standby {on}
//
// Threads: SplitClient::run() and SplitServer::run() block, each on its own
// thread/task. The server's thread only moves message lines in and out of
// RemoteRadio's locked queues; the Node's thread applies them (pump()), so
// the Node stays single-threaded.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "transport.h"

struct cJSON;

namespace split {

using protocol::Bytes;
using LogFn = std::function<void(const std::string &)>;

constexpr int VERSION = 1;
constexpr int64_t PING_MS = 2000;
constexpr int64_t DEAD_MS = 6000;
constexpr int64_t SCAN_BATCH_MS = 250;
constexpr int64_t FIELD_STALE_MS = 60000;
constexpr size_t RX_KEEP = 128;

std::string b64(const Bytes &data);
Bytes unb64(const std::string &text);       // "" on bad input
// One message per line: encode() takes ownership of msg; decode() returns a
// JSON object with a "t" field (caller deletes) or nullptr.
std::string encode(cJSON *msg);
cJSON *decode(const std::string &line);
// The subscriptions a link of this kind delivers.
const std::vector<std::string> &link_subs(const std::string &kind);

// -- field side ---------------------------------------------------------------------------

// One link as the field unit's radio reports it (state + notifications since
// the last report), plus diagnostics the C++ radio keeps.
struct LinkReport {
    Bytes key;
    std::string kind, state, desc;
    int mtu = 23;
    uint32_t count = 0;
    std::map<std::string, std::string> info;
    std::vector<std::pair<std::string, Bytes>> rx;     // (sub, data)
    bool has_stats = false;
    uint32_t connects = 0;
    std::map<std::string, uint32_t> drops;              // by reason
    std::string params, asked;
};

// The field unit's radio, as FieldRadio needs it. Must be safe to call from
// the split client's thread.
class FieldSource {
public:
    virtual ~FieldSource() = default;
    struct Tables {
        std::map<Bytes, core::Observation> direct, repeated, nodes, meaters;
    };
    virtual Tables tables() = 0;                        // a snapshot
    virtual void prune(int64_t now, int64_t max_age_ms) = 0;
    virtual std::vector<LinkReport> collect_links() = 0;   // drains notifications
    virtual std::vector<Bytes> take_removed_links() = 0;
    virtual void open_link(const Bytes &key, const std::string &kind, int addr_type,
                           const Bytes &addr) = 0;
    virtual void close_link(const Bytes &key) = 0;
    virtual void write_link(const Bytes &key, const Bytes &data) = 0;
    virtual void close_all_links() = 0;
    virtual void set_scan_active(bool on) = 0;
    virtual void set_standby(bool on) = 0;
    // Never report the app unit's own advertisements back to it.
    virtual void set_ignore(const Bytes &mac, const Bytes &serial10) = 0;
};

// Turns the field radio's tables and links into messages for the app unit,
// and applies the app unit's commands.
class FieldRadio {
public:
    FieldRadio(FieldSource &src, LogFn log = nullptr) : src_(src), log_(std::move(log)) {}
    void reset();                               // new connection: resend everything
    void set_ignore(const Bytes &mac, const std::string &serial);
    void prune(int64_t now);
    std::string scan_message(int64_t now);      // "" if nothing changed
    std::vector<std::string> link_messages();
    void apply(const std::string &line);
    void apply(cJSON *msg);
    void drop_links() { src_.close_all_links(); }
    std::map<std::string, int> stats{{"scan", 0}, {"rx", 0}, {"cmd", 0}};

private:
    FieldSource &src_;
    LogFn log_;
    Bytes ignore_serial_;
    std::map<std::string, int64_t> sent_;       // table letter + key -> sighting time sent
    std::map<Bytes, std::string> links_;        // key -> last reported signature
};

struct ClientConfig {
    std::string peer;           // app unit host/IP; "" = find it by its beacon
    std::string key;            // shared key
    uint16_t port = 7389;
    std::string serial;         // our serial (10 chars)
    int max_conn = 4;           // outbound connections we offer
    Bytes mac;                  // our BLE address, display order
    std::string impl;           // implementation tag in hello ("idf", ...); "" = none
};

class SplitClient {
public:
    SplitClient(FieldRadio &fr, ClientConfig cfg, LogFn log = nullptr)
        : fr_(fr), cfg_(std::move(cfg)), log_(std::move(log)) {}
    void run();                 // blocks: find, connect, stream, reconnect; until stop()
    void stop() { stop_ = true; }
    // One session with the app unit at host (returns when it ends).
    void session(const std::string &host);

    bool connected() const { return connected_; }
    std::string peer() const;      // the app unit's serial
    std::string peer_ip() const;
    int rtt_ms() const { return rtt_ms_; }

private:
    bool find_app(std::string &host);
    void logf(const char *fmt, ...);

    FieldRadio &fr_;
    ClientConfig cfg_;
    LogFn log_;
    std::atomic<bool> stop_{false}, connected_{false};
    std::atomic<int> rtt_ms_{-1};
    mutable std::mutex mu_;
    std::string peer_, peer_ip_;
};

// -- app side -----------------------------------------------------------------------------

class RemoteRadio;

// A link held by the field unit, as the Node sees it.
class RemoteLink : public core::Link {
public:
    RemoteLink(RemoteRadio &radio, Bytes key, std::string kind)
        : radio_(radio), key_(std::move(key)), kind_(std::move(kind)) {}
    const std::string &kind() const override { return kind_; }
    const std::string &state() const override { return state_; }
    bool enabled() const override { return enabled_; }
    uint32_t count() const override { return count_; }
    int mtu() const override { return mtu_; }
    std::vector<Bytes> take(const std::string &sub) override;
    bool write(const Bytes &data) override;
    const std::map<std::string, std::string> &info() const override { return info_; }
    std::string stats() const override { return stats_; }
    std::string describe() const { return desc_.empty() ? state_ : desc_; }

    std::string state_ = "idle", desc_;
    bool enabled_ = true;
    uint32_t count_ = 0;
    int mtu_ = 23;
    std::map<std::string, std::string> info_;
    std::string stats_;         // field unit diagnostics (JSON), "" = none
    std::map<std::string, std::vector<Bytes>> rx_;
    bool opened_ = false;       // open command sent on the current connection

private:
    RemoteRadio &radio_;
    Bytes key_;
    std::string kind_;
};

// The field unit's radio as seen from the app unit; with `local` (the app
// unit's own peripheral transport) it is the whole transport the Node runs
// on: phones and advertising local, scanning and links remote.
class RemoteRadio : public core::Transport {
public:
    explicit RemoteRadio(core::Transport *local = nullptr, LogFn log = nullptr);

    // -- core::Transport (Node's thread) --
    bool separate_radios() const override { return true; }
    std::vector<std::pair<int, Bytes>> poll_rx() override;   // also pump()s
    void send(const Bytes &frame, const std::vector<int> &conns) override;
    std::shared_ptr<core::Link> open_link(const Bytes &key, const std::string &kind, int addr_type,
                                          const Bytes &addr) override;
    void close_link(const Bytes &key) override;
    void set_advertising(const std::vector<Advert> &adverts, bool fast) override;
    void rotate_advertisement() override;
    void set_scan_active(bool on) override;
    void set_standby(bool on) override;
    void set_firmware(const std::string &fw) override;
    size_t txq_size() const override;
    uint32_t tx_dropped() const override;

    // Node's thread: apply what the network thread received.
    void pump(int64_t now);
    void on_connect(const std::string &hello_line, const std::string &ip);
    void on_disconnect();
    void apply(const std::string &line, int64_t now);
    void queue(cJSON *msg);                 // takes ownership; dropped while offline
    std::vector<std::string> take_outbox();

    bool connected = false;                 // as the Node's thread last saw it
    std::string peer_serial, peer_ip;
    Bytes own_serial;                       // our Display serial: never mirror it
    std::atomic<int> rtt_ms{-1};

    // -- network thread (thread-safe) --
    void net_connected(const std::string &hello_line, const std::string &ip);
    void net_received(const std::string &line);
    void net_disconnected();
    std::vector<std::string> net_take_outbox();
    bool net_is_connected() const { return net_connected_; }

private:
    void apply(cJSON *msg, int64_t now);

    core::Transport *local_;
    LogFn log_;
    bool scan_active_ = false, standby_ = false;
    std::map<Bytes, std::string> link_args_;    // key -> open message (for reconnects)

    std::mutex mu_;
    struct Event {
        enum Kind { CONNECT, MESSAGE, DISCONNECT } kind;
        std::string line, ip;
    };
    std::vector<Event> inbox_;
    std::vector<std::string> outbox_;
    std::atomic<bool> net_connected_{false};
};

struct ServerConfig {
    std::string key;            // shared key the field unit must present
    std::string serial;         // our Display serial
    Bytes mac;                  // our BLE address (the field unit ignores its adverts)
    uint16_t port = 7389;
    std::string bind = "0.0.0.0";
    bool beacon = true;         // UDP broadcast so field units can find us
};

class SplitServer {
public:
    SplitServer(RemoteRadio &remote, ServerConfig cfg, LogFn log = nullptr);
    void run();                 // blocks until stop()
    void stop() { stop_ = true; }

private:
    void logf(const char *fmt, ...);
    RemoteRadio &remote_;
    ServerConfig cfg_;
    LogFn log_;
    std::atomic<bool> stop_{false};
};

}  // namespace split
