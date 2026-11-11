#include "esp_transport.h"

#include "links.h"
#include "peripheral.h"
#include "radio.h"

namespace esp {

// A link held by this unit's radio, as the Node sees it: radio::Links owns
// the connection; pump() copies its state and notifications in.
class EspLink : public core::Link {
public:
    EspLink(core::Bytes key, std::string kind) : key_(std::move(key)), kind_(std::move(kind)) {}
    const std::string &kind() const override { return kind_; }
    const std::string &state() const override { return state_; }
    uint32_t count() const override { return count_; }
    int mtu() const override { return mtu_; }
    std::vector<core::Bytes> take(const std::string &sub) override
    {
        auto it = rx_.find(sub);
        if (it == rx_.end()) return {};
        std::vector<core::Bytes> q = std::move(it->second);
        rx_.erase(it);
        return q;
    }
    bool write(const core::Bytes &data) override { return radio::Links::get().write(key_, data); }
    const std::map<std::string, std::string> &info() const override { return info_; }

    std::string state_ = "idle";
    uint32_t count_ = 0;
    int mtu_ = 23;
    std::map<std::string, std::string> info_;
    std::map<std::string, std::vector<core::Bytes>> rx_;

private:
    core::Bytes key_;
    std::string kind_;
};

static constexpr size_t kRxKeep = 160;

EspTransport::EspTransport(bool central) : central_(central)
{
    max_conn = 4;
    mac = radio::Radio::get().address();
}

static void copy_table(const radio::Table &from, std::map<core::Bytes, core::Observation> &to)
{
    to.clear();
    for (auto &kv : from) {
        core::Observation o;
        o.body = kv.second.body;
        o.name = kv.second.name;
        o.ticks = kv.second.ms;
        o.addr_type = kv.second.addr_type;
        o.addr = kv.second.addr;
        o.rssi = kv.second.rssi;
        to[kv.first] = o;
    }
}

void EspTransport::pump()
{
    mac = radio::Radio::get().address();
    inbound = periph::Peripheral::get().inbound();
    if (!central_) return;
    radio::Radio &r = radio::Radio::get();
    r.prune(radio::Radio::now_ms());
    r.lock();
    copy_table(r.direct, direct);
    copy_table(r.repeated, repeated);
    copy_table(r.nodes, nodes);
    copy_table(r.meaters, meaters);
    r.unlock();
    for (auto &rep : radio::Links::get().collect()) {
        auto it = links.find(rep.key);
        if (it == links.end()) continue;
        auto &link = static_cast<EspLink &>(*it->second);
        link.state_ = rep.state;
        link.mtu_ = rep.mtu;
        link.count_ = rep.count;
        link.info_ = rep.info;
        for (auto &rx : rep.rx) {
            auto &q = link.rx_[rx.first];
            q.push_back(rx.second);
            if (q.size() > kRxKeep) q.erase(q.begin());
        }
    }
    radio::Links::get().take_removed();
}

std::vector<std::pair<int, core::Bytes>> EspTransport::poll_rx()
{
    pump();
    return periph::Peripheral::get().poll_rx();
}

void EspTransport::send(const core::Bytes &frame, const std::vector<int> &conns)
{
    periph::Peripheral::get().send(frame, conns);
}

std::shared_ptr<core::Link> EspTransport::open_link(const core::Bytes &key, const std::string &kind,
                                                    int addr_type, const core::Bytes &addr)
{
    auto &slot = links[key];
    if (!slot) slot = std::make_shared<EspLink>(key, kind);
    if (central_) radio::Links::get().open(key, kind, static_cast<uint8_t>(addr_type), addr);
    return slot;
}

void EspTransport::close_link(const core::Bytes &key)
{
    links.erase(key);
    if (central_) radio::Links::get().close(key);
}

void EspTransport::set_advertising(const std::vector<Advert> &adverts, bool fast)
{
    periph::Peripheral::get().set_advertising(adverts, fast);
}

void EspTransport::rotate_advertisement() { periph::Peripheral::get().rotate(); }

void EspTransport::set_scan_active(bool on)
{
    if (central_) radio::Radio::get().set_scan_active(on);
}

void EspTransport::set_standby(bool on)
{
    periph::Peripheral::get().set_standby(on);
    if (central_) radio::Radio::get().set_standby(on);
}

void EspTransport::set_firmware(const std::string &fw) { periph::Peripheral::get().set_firmware(fw); }

size_t EspTransport::txq_size() const { return periph::Peripheral::get().txq_size(); }

uint32_t EspTransport::tx_dropped() const { return periph::Peripheral::get().tx_dropped(); }

}  // namespace esp
