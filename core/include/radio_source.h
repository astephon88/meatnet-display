// A unit's own radio, as core uses it: the scanning and outbound-link side
// (RadioSource: the ESP32's NimBLE radio, the Linux unit's BlueZ central) and
// the side phones connect to (PeripheralPort). LocalTransport puts the two
// together for a Node; the split client streams a RadioSource to the app unit.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "transport.h"

namespace core {

// One link as the radio reports it: its state, the notifications received
// since the last report, and connection diagnostics.
struct LinkReport {
    Bytes key;
    std::string kind, state;
    int mtu = 23;
    uint32_t count = 0;
    std::map<std::string, std::string> info;
    std::vector<std::pair<std::string, Bytes>> rx;      // (sub, data)
    bool has_stats = false;
    uint32_t connects = 0;
    std::map<std::string, uint32_t> drops;              // by reason
    std::string params, asked;
};

// A link for the status page.
struct LinkSummary {
    std::string kind, serial, state;
    uint32_t count = 0;
    int mtu = 23;
    std::string stats;          // JSON, "" = none
};

// A link's key as text: probe serial, node serial or MEATER address.
std::string link_name(const Bytes &key, const std::string &kind);
// A link's diagnostics in the shape the page (and the split protocol) use.
std::string link_stats(uint32_t connects, const std::map<std::string, uint32_t> &drops,
                       const std::string &params, const std::string &asked);

// Scan tables and outbound links. Must be safe to call from any thread.
class RadioSource {
public:
    virtual ~RadioSource() = default;
    struct Tables {
        std::map<Bytes, Observation> direct, repeated, nodes, meaters;
    };
    virtual Tables tables() = 0;                        // a snapshot
    virtual void prune(int64_t now, int64_t max_age_ms) = 0;
    virtual std::vector<LinkReport> collect_links() = 0;   // drains notifications
    virtual std::vector<LinkSummary> link_summaries() = 0; // doesn't
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

// The GATT server and advertising. Called from the Node's thread.
class PeripheralPort {
public:
    virtual ~PeripheralPort() = default;
    virtual Bytes mac_address() = 0;                    // display order
    virtual std::set<int> connections() = 0;
    virtual std::vector<std::pair<int, Bytes>> poll_rx() = 0;
    virtual void send(const Bytes &frame, const std::vector<int> &conns) = 0;
    virtual void set_advertising(const std::vector<Transport::Advert> &adverts, bool fast) = 0;
    virtual void rotate() {}
    virtual void set_firmware(const std::string &fw) = 0;
    virtual void set_standby(bool on) = 0;
    virtual size_t txq_size() = 0;
    virtual uint32_t tx_dropped() = 0;
};

// The Transport of a unit's own radio: phones and adverts on the peripheral;
// with a source (standalone role) also its scan tables and links. On an app
// unit it is the local half of split::RemoteRadio, without a source.
class LocalTransport : public Transport {
public:
    explicit LocalTransport(PeripheralPort &p, RadioSource *src = nullptr);
    std::vector<std::pair<int, Bytes>> poll_rx() override;
    void send(const Bytes &frame, const std::vector<int> &conns) override { p_.send(frame, conns); }
    std::shared_ptr<Link> open_link(const Bytes &key, const std::string &kind, int addr_type,
                                    const Bytes &addr) override;
    void close_link(const Bytes &key) override;
    void set_advertising(const std::vector<Advert> &adverts, bool fast) override
    {
        p_.set_advertising(adverts, fast);
    }
    void rotate_advertisement() override { p_.rotate(); }
    void set_scan_active(bool on) override;
    void set_standby(bool on) override;
    void set_firmware(const std::string &fw) override { p_.set_firmware(fw); }
    size_t txq_size() const override { return p_.txq_size(); }
    uint32_t tx_dropped() const override { return p_.tx_dropped(); }

private:
    void pump();                // scan tables + link reports from the source
    PeripheralPort &p_;
    RadioSource *src_;
};

}  // namespace core
