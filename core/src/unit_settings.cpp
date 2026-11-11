#include "unit_settings.h"

#include "cJSON.h"

namespace core {

static void take(const cJSON *o, const char *k, std::string &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) out = v->valuestring;
}

static void take(const cJSON *o, const char *k, int &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) out = static_cast<int>(v->valuedouble);
}

static void take(const cJSON *o, const char *k, double &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) out = v->valuedouble;
}

static void take(const cJSON *o, const char *k, bool &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) out = cJSON_IsTrue(v);
}

static void take(const cJSON *o, const char *k, std::vector<std::string> &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v)) return;
    out.clear();
    const cJSON *e;
    cJSON_ArrayForEach(e, v)
        if (cJSON_IsString(e)) out.push_back(e->valuestring);
}

void merge_settings(Settings &s, const cJSON *o)
{
    if (!cJSON_IsObject(o)) return;
    take(o, "role", s.role);
    take(o, "split_peer", s.split_peer);
    take(o, "split_key", s.split_key);
    take(o, "split_port", s.split_port);
    take(o, "wifi_ssid", s.wifi_ssid);
    take(o, "wifi_password", s.wifi_password);
    take(o, "ap_password", s.ap_password);
    take(o, "hostname", s.hostname);
    take(o, "serial", s.serial);
    take(o, "dis_model", s.dis_model);
    take(o, "dis_firmware", s.dis_firmware);
    take(o, "unit", s.unit);
    take(o, "layout", s.layout);
    take(o, "rotate_s", s.rotate_s);
    take(o, "brightness", s.brightness);
    take(o, "screen", s.screen);
    take(o, "status_page", s.status_page);
    const cJSON *rd = cJSON_GetObjectItemCaseSensitive(o, "readings");
    if (cJSON_IsArray(rd)) {
        s.readings.clear();
        const cJSON *e;
        cJSON_ArrayForEach(e, rd) {
            if (!cJSON_IsObject(e)) continue;
            Reading r;
            take(e, "dev", r.dev);
            take(e, "field", r.field);
            take(e, "label", r.label);
            s.readings.push_back(r);
        }
    }
    take(o, "linked_probes", s.linked_probes);
    const cJSON *up = cJSON_GetObjectItemCaseSensitive(o, "upstream_node");
    if (cJSON_IsString(up)) {
        s.upstream_node.clear();
        if (*up->valuestring) s.upstream_node.push_back(up->valuestring);
    } else {
        take(o, "upstream_node", s.upstream_node);
    }
    take(o, "repeat_adverts", s.repeat_adverts);
    take(o, "meatnet_peer", s.meatnet_peer);
    take(o, "upstream_push", s.upstream_push);
    take(o, "max_conn", s.max_conn);
    take(o, "linked_scan_duty", s.linked_scan_duty);
    take(o, "inbound_reserve", s.inbound_reserve);
    take(o, "meater_bridge", s.meater_bridge);
    const cJSON *mp = cJSON_GetObjectItemCaseSensitive(o, "meater_probes");
    if (cJSON_IsArray(mp)) {
        s.meater_probes.clear();
        const cJSON *e;
        cJSON_ArrayForEach(e, mp) {
            if (!cJSON_IsObject(e)) continue;
            MeaterProbe m;
            take(e, "addr", m.addr);
            take(e, "id", m.id);
            take(e, "color", m.color);
            s.meater_probes.push_back(m);
        }
    }
    take(o, "meater_direct_adverts", s.meater_direct_adverts);
    take(o, "standby", s.standby);
    take(o, "auto_standby_min", s.auto_standby_min);
    take(o, "battery_pin", s.battery_pin);
    take(o, "battery_divider", s.battery_divider);
    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(o, "pins");
    if (cJSON_IsObject(pins)) {
        take(pins, "sck", s.pins.sck);
        take(pins, "mosi", s.pins.mosi);
        take(pins, "dc", s.pins.dc);
        take(pins, "cs", s.pins.cs);
        take(pins, "rst", s.pins.rst);
        take(pins, "bl", s.pins.bl);
        take(pins, "spi", s.pins.spi);
        take(pins, "baud", s.pins.baud);
        take(pins, "rotation", s.pins.rotation);
    }
    take(o, "supervision_probe_ms", s.supervision_probe_ms);
    take(o, "supervision_node_ms", s.supervision_node_ms);
    take(o, "peer_latency_max", s.peer_latency_max);
    take(o, "coex", s.coex);
}

void merge_settings(Settings &s, const std::string &json)
{
    cJSON *o = cJSON_Parse(json.c_str());
    merge_settings(s, o);
    cJSON_Delete(o);
}

static cJSON *strings(const std::vector<std::string> &v)
{
    cJSON *a = cJSON_CreateArray();
    for (auto &x : v) cJSON_AddItemToArray(a, cJSON_CreateString(x.c_str()));
    return a;
}

static void secret(cJSON *o, const char *k, const std::string &v, bool show)
{
    if (show) cJSON_AddStringToObject(o, k, v.c_str());
    else cJSON_AddBoolToObject(o, (std::string(k) + "_set").c_str(), !v.empty());
}

cJSON *settings_to_json(const Settings &s, bool secrets)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", s.role.c_str());
    cJSON_AddStringToObject(o, "split_peer", s.split_peer.c_str());
    secret(o, "split_key", s.split_key, secrets);
    cJSON_AddNumberToObject(o, "split_port", s.split_port);
    cJSON_AddStringToObject(o, "wifi_ssid", s.wifi_ssid.c_str());
    secret(o, "wifi_password", s.wifi_password, secrets);
    secret(o, "ap_password", s.ap_password, secrets);
    cJSON_AddStringToObject(o, "hostname", s.hostname.c_str());
    cJSON_AddStringToObject(o, "serial", s.serial.c_str());
    cJSON_AddStringToObject(o, "dis_model", s.dis_model.c_str());
    cJSON_AddStringToObject(o, "dis_firmware", s.dis_firmware.c_str());
    cJSON_AddStringToObject(o, "unit", s.unit.c_str());
    cJSON_AddStringToObject(o, "layout", s.layout.c_str());
    cJSON_AddNumberToObject(o, "rotate_s", s.rotate_s);
    cJSON_AddNumberToObject(o, "brightness", s.brightness);
    cJSON_AddBoolToObject(o, "screen", s.screen);
    cJSON_AddBoolToObject(o, "status_page", s.status_page);
    cJSON *rd = cJSON_AddArrayToObject(o, "readings");
    for (auto &r : s.readings) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "dev", r.dev.c_str());
        cJSON_AddStringToObject(e, "field", r.field.c_str());
        cJSON_AddStringToObject(e, "label", r.label.c_str());
        cJSON_AddItemToArray(rd, e);
    }
    cJSON_AddItemToObject(o, "linked_probes", strings(s.linked_probes));
    cJSON_AddItemToObject(o, "upstream_node", strings(s.upstream_node));
    cJSON_AddStringToObject(o, "repeat_adverts", s.repeat_adverts.c_str());
    cJSON_AddBoolToObject(o, "meatnet_peer", s.meatnet_peer);
    cJSON_AddBoolToObject(o, "upstream_push", s.upstream_push);
    cJSON_AddNumberToObject(o, "max_conn", s.max_conn);
    cJSON_AddNumberToObject(o, "linked_scan_duty", s.linked_scan_duty);
    cJSON_AddNumberToObject(o, "inbound_reserve", s.inbound_reserve);
    cJSON_AddBoolToObject(o, "meater_bridge", s.meater_bridge);
    cJSON *mp = cJSON_AddArrayToObject(o, "meater_probes");
    for (auto &m : s.meater_probes) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", m.addr.c_str());
        cJSON_AddNumberToObject(e, "id", m.id);
        cJSON_AddNumberToObject(e, "color", m.color);
        cJSON_AddItemToArray(mp, e);
    }
    cJSON_AddBoolToObject(o, "meater_direct_adverts", s.meater_direct_adverts);
    cJSON_AddBoolToObject(o, "standby", s.standby);
    cJSON_AddNumberToObject(o, "auto_standby_min", s.auto_standby_min);
    cJSON_AddNumberToObject(o, "battery_pin", s.battery_pin);
    cJSON_AddNumberToObject(o, "battery_divider", s.battery_divider);
    cJSON *p = cJSON_AddObjectToObject(o, "pins");
    cJSON_AddNumberToObject(p, "sck", s.pins.sck);
    cJSON_AddNumberToObject(p, "mosi", s.pins.mosi);
    cJSON_AddNumberToObject(p, "dc", s.pins.dc);
    cJSON_AddNumberToObject(p, "cs", s.pins.cs);
    cJSON_AddNumberToObject(p, "rst", s.pins.rst);
    cJSON_AddNumberToObject(p, "bl", s.pins.bl);
    cJSON_AddNumberToObject(p, "spi", s.pins.spi);
    cJSON_AddNumberToObject(p, "baud", s.pins.baud);
    cJSON_AddNumberToObject(p, "rotation", s.pins.rotation);
    cJSON_AddNumberToObject(o, "supervision_probe_ms", s.supervision_probe_ms);
    cJSON_AddNumberToObject(o, "supervision_node_ms", s.supervision_node_ms);
    cJSON_AddNumberToObject(o, "peer_latency_max", s.peer_latency_max);
    cJSON_AddStringToObject(o, "coex", s.coex.c_str());
    return o;
}

}  // namespace core
