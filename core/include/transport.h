// What the Node needs from the radio side: the scan tables, the outbound
// links, phone (inbound) connections and advertising. Implemented by the
// ESP32 and Linux radios, by the app role's split transport, and by the
// tests' fake (the Python units used the same shape as a duck type).
//
// The Node is single-threaded: an implementation fills the tables and link
// queues from its radio thread under its own lock, and hands them over
// during calls from the Node's thread (e.g. in poll_rx()).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "protocol.h"

namespace core {

using protocol::Bytes;

// One device heard while scanning. Which fields are used depends on the
// table: direct probes and nodes carry the advert body, address and RSSI;
// repeated probes body + RSSI; MEATERs name, address type and RSSI.
struct Observation {
    Bytes body;
    int64_t ticks = 0;          // when last heard
    int addr_type = 0;
    Bytes addr;                 // display order (6 bytes)
    int rssi = 0;
    std::string name;
};

// An outbound connection the Node asked for (probe, node or MEATER).
class Link {
public:
    virtual ~Link() = default;
    virtual const std::string &kind() const = 0;    // "probe", "node", "meater"
    virtual const std::string &state() const = 0;   // "idle", "connecting", ..., "ready", "error: ..."
    bool ready() const { return state() == "ready"; }
    virtual bool enabled() const { return true; }
    virtual uint32_t count() const { return 0; }    // notifications received
    virtual int mtu() const { return 23; }
    // Notifications received on a subscription ("status", "uart", "temp",
    // "battery") since the last call.
    virtual std::vector<Bytes> take(const std::string &sub) = 0;
    virtual bool write(const Bytes &data) = 0;
    // Device Information strings read when the link came up ("firmware",
    // "hardware", "model").
    virtual const std::map<std::string, std::string> &info() const;
};

class Transport {
public:
    virtual ~Transport() = default;

    int max_conn = 4;           // BLE connections the radio offers
    Bytes mac;                  // our BLE address, display order
    std::set<int> inbound;      // connected phones / peer nodes

    // Scan tables: probe serial4 / node serial10 / MEATER address -> observation.
    std::map<Bytes, Observation> direct, repeated, nodes, meaters;
    // Outbound links by key (probe serial4, node serial10, MEATER address).
    std::map<Bytes, std::shared_ptr<Link>> links;

    // True when phones and device links use different radios (the app role):
    // no connections need keeping free for phones.
    virtual bool separate_radios() const { return false; }

    // Data written to us by inbound connections: (conn, bytes).
    virtual std::vector<std::pair<int, Bytes>> poll_rx() = 0;
    virtual void send(const Bytes &frame, const std::vector<int> &conns) = 0;
    virtual std::shared_ptr<Link> open_link(const Bytes &key, const std::string &kind, int addr_type,
                                            const Bytes &addr) = 0;
    virtual void close_link(const Bytes &key) = 0;

    struct Advert {
        Bytes msd;              // manufacturer data with company ID
        bool probe = false;     // a direct-probe advert: never connectable
        bool operator==(const Advert &o) const { return msd == o.msd && probe == o.probe; }
    };
    virtual void set_advertising(const std::vector<Advert> &adverts, bool fast) = 0;
    virtual void rotate_advertisement() {}
    virtual void set_scan_active(bool) {}
    virtual void set_standby(bool) {}
    virtual void set_firmware(const std::string &) {}
    virtual size_t txq_size() const { return 0; }
    virtual uint32_t tx_dropped() const { return 0; }
};

}  // namespace core
