#include "status.h"

#include <algorithm>
#include <set>

#include "cJSON.h"
#include "meater.h"

namespace core {

static void opt_num(cJSON *o, const char *k, const std::optional<double> &v)
{
    if (v) cJSON_AddNumberToObject(o, k, *v);
    else cJSON_AddNullToObject(o, k);
}

static void add_stats(cJSON *o, const std::string &stats)
{
    cJSON *st = stats.empty() ? nullptr : cJSON_Parse(stats.c_str());
    if (st) cJSON_AddItemToObject(o, "stats", st);
    else cJSON_AddNullToObject(o, "stats");
}

static void probe_fields(cJSON *e, const protocol::ProbeData &d)
{
    cJSON_AddNumberToObject(e, "id", d.probe_id);
    cJSON_AddNumberToObject(e, "color", d.color);
    cJSON_AddNumberToObject(e, "mode", d.mode);
    opt_num(e, "core_c", d.core_c);
    opt_num(e, "surface_c", d.surface_c);
    opt_num(e, "ambient_c", d.ambient_c);
    opt_num(e, "instant_c", d.instant_c);
    cJSON *t = cJSON_AddArrayToObject(e, "temps");
    for (double v : d.temps) cJSON_AddItemToArray(t, cJSON_CreateNumber(v));
    cJSON_AddBoolToObject(e, "low_battery", d.low_battery);
}

static void device_fields(cJSON *e, const protocol::NodeAdvert &d)
{
    cJSON_AddNumberToObject(e, "ptype", d.ptype);
    if (d.ptype == protocol::PRODUCT_TYPE_GAUGE) {
        opt_num(e, "temp_c", d.temp_c);
        cJSON_AddBoolToObject(e, "sensor_present", d.sensor_present);
        cJSON_AddBoolToObject(e, "overheating", d.overheating);
        cJSON_AddBoolToObject(e, "low_battery", d.low_battery);
    } else if (d.ptype == protocol::PRODUCT_TYPE_ENGINE) {
        cJSON_AddNumberToObject(e, "setpoint_c", d.setpoint_c);
        cJSON_AddBoolToObject(e, "app_mode", d.app_mode);
        cJSON_AddBoolToObject(e, "ctrl_connected", d.ctrl_connected);
        cJSON_AddBoolToObject(e, "lid_open", d.lid_open);
    }
}

static std::string device_name(int ptype)
{
    const char *n = protocol::product_name(ptype);
    return n && *n ? n : "Device";
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

cJSON *node_status(Node &node, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *links = cJSON_AddArrayToObject(o, "links");
    for (auto &kv : node.t.links) {
        const Link &l = *kv.second;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "kind", l.kind().c_str());
        cJSON_AddStringToObject(e, "serial", Node::link_name(kv.first, l).c_str());
        add_stats(e, l.stats());
        cJSON_AddStringToObject(e, "state", l.state().c_str());
        cJSON_AddNumberToObject(e, "count", l.count());
        cJSON_AddNumberToObject(e, "mtu", l.mtu());
        cJSON_AddItemToArray(links, e);
    }

    std::vector<Bytes> wanted = node.wanted_probes();
    cJSON *probes = cJSON_AddArrayToObject(o, "probes");
    for (auto &kv : node.probes) {
        const ProbeReading &d = kv.second;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "serial", d.serial.c_str());
        probe_fields(e, d);
        cJSON_AddStringToObject(e, "src", d.src.c_str());
        cJSON_AddNumberToObject(e, "hops", d.hops);
        if (d.rssi) cJSON_AddNumberToObject(e, "rssi", *d.rssi);
        else cJSON_AddNullToObject(e, "rssi");
        cJSON_AddNumberToObject(e, "age_s", static_cast<double>(d.age_ms / 1000));
        if (d.prediction) {
            cJSON *p = cJSON_AddObjectToObject(e, "prediction");
            cJSON_AddNumberToObject(p, "state", d.prediction->state);
            cJSON_AddNumberToObject(p, "mode", d.prediction->mode);
            cJSON_AddNumberToObject(p, "type", d.prediction->type);
            cJSON_AddNumberToObject(p, "setpoint_c", d.prediction->setpoint_c);
            cJSON_AddNumberToObject(p, "heat_start_c", d.prediction->heat_start_c);
            cJSON_AddNumberToObject(p, "seconds", d.prediction->seconds);
            cJSON_AddNumberToObject(p, "est_core_c", d.prediction->est_core_c);
        } else {
            cJSON_AddNullToObject(e, "prediction");
        }
        cJSON_AddBoolToObject(e, "alarming", d.alarming);
        cJSON_AddBoolToObject(e, "linked", std::find(wanted.begin(), wanted.end(), kv.first) != wanted.end());
        cJSON_AddBoolToObject(e, "direct", node.t.direct.count(kv.first) > 0);
        auto slot = std::find(node.slots.begin(), node.slots.end(), kv.first);
        if (slot != node.slots.end()) cJSON_AddNumberToObject(e, "slot", slot - node.slots.begin() + 1);
        else cJSON_AddNullToObject(e, "slot");
        cJSON_AddItemToArray(probes, e);
    }

    cJSON *devices = cJSON_AddArrayToObject(o, "devices");
    for (auto &kv : node.devices) {
        const DeviceReading &d = kv.second;
        cJSON *e = cJSON_CreateObject();
        device_fields(e, d);
        cJSON_AddStringToObject(e, "name", d.name.c_str());
        cJSON_AddNumberToObject(e, "rssi", d.rssi);
        cJSON_AddStringToObject(e, "serial", d.serial_str.c_str());
        cJSON_AddNumberToObject(e, "age_s", static_cast<double>(d.age_ms / 1000));
        cJSON_AddItemToArray(devices, e);
    }

    cJSON *meaters = cJSON_AddArrayToObject(o, "meaters");
    auto sel = node.meater_selected();
    std::set<Bytes> seen;
    for (auto &kv : node.t.meaters) seen.insert(kv.first);
    for (auto &kv : sel) seen.insert(kv.first);
    for (const Bytes &addr : seen) {
        auto obs = node.t.meaters.find(addr);
        auto data = node.meater_data.find(addr);
        auto link = node.t.links.find(addr);
        auto chosen = sel.find(addr);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", meater::addr_str(addr).c_str());
        bool heard = obs != node.t.meaters.end();
        cJSON_AddStringToObject(e, "name", heard && !obs->second.name.empty() ? obs->second.name.c_str() : "MEATER");
        if (heard) {
            cJSON_AddNumberToObject(e, "rssi", obs->second.rssi);
            cJSON_AddNumberToObject(e, "age_s", static_cast<double>((now - obs->second.ticks) / 1000));
        } else {
            cJSON_AddNullToObject(e, "rssi");
            cJSON_AddNullToObject(e, "age_s");
        }
        cJSON_AddBoolToObject(e, "bridged", chosen != sel.end());
        cJSON_AddNumberToObject(e, "id", chosen != sel.end() ? chosen->second.id : 8);
        cJSON_AddStringToObject(e, "serial", protocol::probe_serial_str(meater::serial_for(addr)).c_str());
        if (link != node.t.links.end()) cJSON_AddStringToObject(e, "state", link->second->state().c_str());
        else cJSON_AddNullToObject(e, "state");
        const MeaterData *md = data != node.meater_data.end() ? &data->second : nullptr;
        if (md && md->temps) {
            cJSON *tips = cJSON_AddArrayToObject(e, "tips");
            for (int i = 0; i < 5; i++) cJSON_AddItemToArray(tips, cJSON_CreateNumber((*md->temps)[i]));
            cJSON_AddNumberToObject(e, "ambient", (*md->temps)[5]);
        } else {
            cJSON_AddNullToObject(e, "tips");
            cJSON_AddNullToObject(e, "ambient");
        }
        if (md && md->battery) cJSON_AddNumberToObject(e, "battery", *md->battery);
        else cJSON_AddNullToObject(e, "battery");
        cJSON_AddItemToArray(meaters, e);
    }

    cJSON_AddBoolToObject(o, "meater_bridge", node.meater_enabled());
    cJSON_AddNumberToObject(o, "budget", node.link_budget());
    cJSON_AddNumberToObject(o, "inbound", static_cast<double>(node.t.inbound.size()));
    cJSON_AddNumberToObject(o, "max_conn", node.t.max_conn);
    cJSON_AddNumberToObject(o, "pending", static_cast<double>(node.pending.size()));
    cJSON *stats = cJSON_AddObjectToObject(o, "stats");
    for (auto &kv : node.stats) cJSON_AddNumberToObject(stats, kv.first.c_str(), kv.second);
    cJSON_AddNumberToObject(o, "tx_dropped", node.t.tx_dropped());
    cJSON *via = cJSON_AddObjectToObject(o, "via");
    for (auto &dir : node.via) {
        cJSON *d = cJSON_AddObjectToObject(via, dir.first.c_str());
        for (auto &kv : dir.second) cJSON_AddNumberToObject(d, kv.first.c_str(), kv.second);
    }
    return o;
}

cJSON *field_status(const std::map<Bytes, Observation> &direct, const std::map<Bytes, Observation> &nodes,
                    const std::map<Bytes, Observation> &meaters, const std::vector<LinkSummary> &links,
                    int max_conn, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", "field");
    cJSON *probes = cJSON_AddArrayToObject(o, "probes");
    for (auto &kv : direct) {
        auto d = protocol::parse_probe_advert(kv.second.body);
        if (!d) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "serial", protocol::probe_serial_str(kv.first).c_str());
        probe_fields(e, *d);
        cJSON_AddStringToObject(e, "src", "advert");
        cJSON_AddNumberToObject(e, "hops", 0);
        cJSON_AddNumberToObject(e, "rssi", kv.second.rssi);
        cJSON_AddNumberToObject(e, "age_s", static_cast<double>((now - kv.second.ticks) / 1000));
        cJSON_AddNullToObject(e, "prediction");
        cJSON_AddBoolToObject(e, "alarming", false);
        cJSON_AddBoolToObject(e, "linked", false);
        cJSON_AddBoolToObject(e, "direct", true);
        cJSON_AddNullToObject(e, "slot");
        cJSON_AddItemToArray(probes, e);
    }
    cJSON *devices = cJSON_AddArrayToObject(o, "devices");
    for (auto &kv : nodes) {
        auto d = protocol::parse_node_advert(kv.second.body);
        if (!d) continue;
        cJSON *e = cJSON_CreateObject();
        device_fields(e, *d);
        cJSON_AddStringToObject(e, "serial", protocol::serial10_str(kv.first).c_str());
        cJSON_AddStringToObject(e, "name", device_name(d->ptype).c_str());
        cJSON_AddNumberToObject(e, "rssi", kv.second.rssi);
        cJSON_AddNumberToObject(e, "age_s", static_cast<double>((now - kv.second.ticks) / 1000));
        cJSON_AddItemToArray(devices, e);
    }
    cJSON *ms = cJSON_AddArrayToObject(o, "meaters");
    for (auto &kv : meaters) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", meater::addr_str(kv.first).c_str());
        cJSON_AddStringToObject(e, "name", kv.second.name.empty() ? "MEATER" : kv.second.name.c_str());
        cJSON_AddNumberToObject(e, "rssi", kv.second.rssi);
        cJSON_AddNumberToObject(e, "age_s", static_cast<double>((now - kv.second.ticks) / 1000));
        cJSON_AddItemToArray(ms, e);
    }
    cJSON *ls = cJSON_AddArrayToObject(o, "links");
    for (auto &l : links) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "kind", l.kind.c_str());
        cJSON_AddStringToObject(e, "serial", l.serial.c_str());
        cJSON_AddStringToObject(e, "state", l.state.c_str());
        cJSON_AddNumberToObject(e, "count", l.count);
        cJSON_AddNumberToObject(e, "mtu", l.mtu);
        add_stats(e, l.stats);
        cJSON_AddItemToArray(ls, e);
    }
    cJSON_AddNumberToObject(o, "budget", 0);
    cJSON_AddNumberToObject(o, "inbound", 0);
    cJSON_AddNumberToObject(o, "max_conn", max_conn);
    cJSON_AddNumberToObject(o, "pending", 0);
    cJSON *stats = cJSON_AddObjectToObject(o, "stats");
    cJSON_AddNumberToObject(stats, "routed", 0);
    cJSON_AddNumberToObject(stats, "status_sent", 0);
    cJSON_AddNumberToObject(o, "tx_dropped", 0);
    cJSON_AddBoolToObject(o, "meater_bridge", false);
    cJSON_AddItemToObject(o, "via", cJSON_CreateObject());
    return o;
}

}  // namespace core
