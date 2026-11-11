#include "host_transport.h"

#include "status.h"

namespace linux_unit {

// A link held by the central, as the Node sees it: pump() copies its state
// and notifications in.
class HostLink : public core::Link {
public:
    HostLink(Central &c, Bytes key, std::string kind) : c_(c), key_(std::move(key)), kind_(std::move(kind)) {}
    const std::string &kind() const override { return kind_; }
    const std::string &state() const override { return state_; }
    uint32_t count() const override { return count_; }
    int mtu() const override { return mtu_; }
    std::vector<Bytes> take(const std::string &sub) override
    {
        auto it = rx_.find(sub);
        if (it == rx_.end()) return {};
        std::vector<Bytes> q = std::move(it->second);
        rx_.erase(it);
        return q;
    }
    bool write(const Bytes &data) override
    {
        if (state_ != "ready") return false;
        c_.write_link(key_, data);
        return true;
    }
    const std::map<std::string, std::string> &info() const override { return info_; }
    std::string stats() const override { return stats_; }

    std::string state_ = "idle", stats_;
    uint32_t count_ = 0;
    int mtu_ = 23;
    std::map<std::string, std::string> info_;
    std::map<std::string, std::vector<Bytes>> rx_;

private:
    Central &c_;
    Bytes key_;
    std::string kind_;
};

static constexpr size_t kRxKeep = 160;

LinuxTransport::LinuxTransport(Peripheral &p, Central *c) : p_(p), c_(c) { mac = p.mac; }

void LinuxTransport::pump()
{
    mac = p_.mac;
    inbound = p_.inbound;
    if (!c_) return;
    int64_t now = core::ticks_ms();
    c_->prune(now, 60000);
    split::FieldSource::Tables t = c_->tables();
    direct = std::move(t.direct);
    repeated = std::move(t.repeated);
    nodes = std::move(t.nodes);
    meaters = std::move(t.meaters);
    for (auto &rep : c_->collect_links()) {
        auto it = links.find(rep.key);
        if (it == links.end()) continue;
        auto &link = static_cast<HostLink &>(*it->second);
        link.state_ = rep.state;
        link.mtu_ = rep.mtu;
        link.count_ = rep.count;
        link.info_ = rep.info;
        link.stats_ = core::link_stats(rep.connects, rep.drops, rep.params, rep.asked);
        for (auto &rx : rep.rx) {
            auto &q = link.rx_[rx.first];
            q.push_back(rx.second);
            if (q.size() > kRxKeep) q.erase(q.begin());
        }
    }
    c_->take_removed_links();
}

std::vector<std::pair<int, Bytes>> LinuxTransport::poll_rx()
{
    pump();
    return p_.poll_rx();
}

std::shared_ptr<core::Link> LinuxTransport::open_link(const Bytes &key, const std::string &kind, int addr_type,
                                                      const Bytes &addr)
{
    auto &slot = links[key];
    if (!slot && c_) slot = std::make_shared<HostLink>(*c_, key, kind);
    if (c_) c_->open_link(key, kind, addr_type, addr);
    return slot;
}

void LinuxTransport::close_link(const Bytes &key)
{
    links.erase(key);
    if (c_) c_->close_link(key);
}

void LinuxTransport::set_standby(bool on)
{
    p_.set_standby(on);
    if (c_) c_->set_standby(on);
}

}  // namespace linux_unit
