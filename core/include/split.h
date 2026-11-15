// Two-unit mode: a phone-side "app" unit and a
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
//          link {k, kind, state, mtu, count, info, stats}
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

#include "radio_source.h"
#include "util.h"

struct cJSON;

namespace split {

using protocol::Bytes;
using core::LogFn;

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

// -- field side ---------------------------------------------------------------------------

// Turns the field radio's tables and links into messages for the app unit,
// and applies the app unit's commands.
class FieldRadio {
public:
    explicit FieldRadio(core::RadioSource &src) : src_(src) {}
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
    core::RadioSource &src_;
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
    uint32_t count() const override { return count_; }
    int mtu() const override { return mtu_; }
    std::vector<Bytes> take(const std::string &sub) override;
    bool write(const Bytes &data) override;
    const std::map<std::string, std::string> &info() const override { return info_; }
    std::string stats() const override { return stats_; }

    std::string state_ = "idle";
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

    bool connected = false;                 // as the Node's thread last saw it
    std::string peer_serial, peer_ip;
    Bytes own_serial;                       // our Display serial: never mirror it
    std::atomic<int> rtt_ms{-1};

    // -- network thread (thread-safe) --
    void net_connected(const std::string &hello_line, const std::string &ip);
    void net_received(const std::string &line);
    void net_disconnected();
    std::vector<std::string> take_outbox();

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
    struct Candidate {
        int fd;
        std::string ip, buf;
        int64_t deadline;
        bool readable;
    };
    void poll_candidates(int64_t now);
    void poll_session(bool readable, int64_t now);
    void end_session();

    RemoteRadio &remote_;
    ServerConfig cfg_;
    LogFn log_;
    std::atomic<bool> stop_{false};
    // run()'s thread only.
    std::vector<Candidate> candidates_;
    int sfd_ = -1;              // the field unit's session
    std::string sbuf_;
    int64_t rx_at_ = 0, ping_at_ = 0;
};

}  // namespace split
