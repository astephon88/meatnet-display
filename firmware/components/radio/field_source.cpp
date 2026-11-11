#include "field_source.h"

#include "links.h"
#include "radio.h"

namespace radio {

static void copy_table(const Table &from, std::map<split::Bytes, core::Observation> &to)
{
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

split::FieldSource::Tables FieldSource::tables()
{
    Radio &r = Radio::get();
    Tables t;
    r.lock();
    copy_table(r.direct, t.direct);
    copy_table(r.repeated, t.repeated);
    copy_table(r.nodes, t.nodes);
    copy_table(r.meaters, t.meaters);
    r.unlock();
    return t;
}

void FieldSource::prune(int64_t now, int64_t max_age_ms) { Radio::get().prune(now, max_age_ms); }

std::vector<split::LinkReport> FieldSource::collect_links()
{
    std::vector<split::LinkReport> out;
    for (auto &r : Links::get().collect()) {
        split::LinkReport s;
        s.key = r.key;
        s.kind = r.kind;
        s.state = r.state;
        s.desc = r.desc;
        s.mtu = r.mtu;
        s.count = r.count;
        s.info = r.info;
        s.rx = r.rx;
        s.has_stats = true;
        s.connects = r.connects;
        s.drops = r.drops;
        s.params = r.params;
        s.asked = r.asked;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<split::Bytes> FieldSource::take_removed_links() { return Links::get().take_removed(); }

void FieldSource::open_link(const split::Bytes &key, const std::string &kind, int addr_type,
                            const split::Bytes &addr)
{
    Links::get().open(key, kind, static_cast<uint8_t>(addr_type), addr);
}

void FieldSource::close_link(const split::Bytes &key) { Links::get().close(key); }

void FieldSource::write_link(const split::Bytes &key, const split::Bytes &data)
{
    Links::get().write(key, data);
}

void FieldSource::close_all_links() { Links::get().close_all(); }

void FieldSource::set_scan_active(bool on) { Radio::get().set_scan_active(on); }

void FieldSource::set_standby(bool on) { Radio::get().set_standby(on); }

void FieldSource::set_ignore(const split::Bytes &mac, const split::Bytes &serial10)
{
    Radio::get().set_ignore(mac, serial10);
}

}  // namespace radio
