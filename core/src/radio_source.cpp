#include "radio_source.h"

#include "cJSON.h"
#include "meater.h"
#include "util.h"

namespace core {

static constexpr size_t RX_KEEP = 160;
static constexpr int64_t SCAN_STALE_MS = 60000;

std::string link_name(const Bytes &key, const std::string &kind)
{
    if (kind == "probe") return protocol::probe_serial_str(key);
    if (kind == "meater") return meater::addr_str(key);
    return protocol::serial10_str(key);
}

std::string link_stats(uint32_t connects, const std::map<std::string, uint32_t> &drops,
                       const std::string &params, const std::string &asked)
{
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "connects", connects);
    cJSON *d = cJSON_AddObjectToObject(st, "drops");
    for (auto &kv : drops) cJSON_AddNumberToObject(d, kv.first.c_str(), kv.second);
    cJSON_AddStringToObject(st, "params", params.c_str());
    cJSON_AddStringToObject(st, "asked", asked.c_str());
    char *text = cJSON_PrintUnformatted(st);
    cJSON_Delete(st);
    std::string out = text ? text : "";
    cJSON_free(text);
    return out;
}

namespace {

// A link held by the source, as the Node sees it: pump() copies its state
// and notifications in.
class SourceLink : public Link {
public:
    SourceLink(RadioSource &src, Bytes key, std::string kind)
        : src_(src), key_(std::move(key)), kind_(std::move(kind)) {}
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
        src_.write_link(key_, data);
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
    RadioSource &src_;
    Bytes key_;
    std::string kind_;
};

}  // namespace

LocalTransport::LocalTransport(PeripheralPort &p, RadioSource *src) : p_(p), src_(src)
{
    mac = p_.mac_address();
}

void LocalTransport::pump()
{
    mac = p_.mac_address();
    inbound = p_.connections();
    if (!src_) return;
    src_->prune(ticks_ms(), SCAN_STALE_MS);
    RadioSource::Tables t = src_->tables();
    direct = std::move(t.direct);
    repeated = std::move(t.repeated);
    nodes = std::move(t.nodes);
    meaters = std::move(t.meaters);
    for (auto &rep : src_->collect_links()) {
        auto it = links.find(rep.key);
        if (it == links.end()) continue;
        auto &link = static_cast<SourceLink &>(*it->second);
        link.state_ = rep.state;
        link.mtu_ = rep.mtu;
        link.count_ = rep.count;
        link.info_ = rep.info;
        link.stats_ = link_stats(rep.connects, rep.drops, rep.params, rep.asked);
        for (auto &rx : rep.rx) {
            auto &q = link.rx_[rx.first];
            q.push_back(std::move(rx.second));
            if (q.size() > RX_KEEP) q.erase(q.begin());
        }
    }
    src_->take_removed_links();
}

std::vector<std::pair<int, Bytes>> LocalTransport::poll_rx()
{
    pump();
    return p_.poll_rx();
}

std::shared_ptr<Link> LocalTransport::open_link(const Bytes &key, const std::string &kind, int addr_type,
                                                const Bytes &addr)
{
    if (!src_) return nullptr;
    auto &slot = links[key];
    if (!slot) slot = std::make_shared<SourceLink>(*src_, key, kind);
    src_->open_link(key, kind, addr_type, addr);
    return slot;
}

void LocalTransport::close_link(const Bytes &key)
{
    links.erase(key);
    if (src_) src_->close_link(key);
}

void LocalTransport::set_scan_active(bool on)
{
    if (src_) src_->set_scan_active(on);
}

void LocalTransport::set_standby(bool on)
{
    p_.set_standby(on);
    if (src_) src_->set_standby(on);
}

}  // namespace core
