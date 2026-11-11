#include "unit_settings.h"

#include <cctype>
#include <cstring>
#include <set>

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

namespace core {

namespace {

struct Check {
    const cJSON *v;
    const char *k;
    std::string &err;

    bool fail(const std::string &what)
    {
        err = std::string(k) + ": " + what;
        return false;
    }
    bool str(std::string &out, size_t max)
    {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > max)
            return fail("text of at most " + std::to_string(max) + " characters");
        out = v->valuestring;
        return true;
    }
    bool one_of(std::string &out, std::initializer_list<const char *> opts)
    {
        std::string all;
        for (const char *o : opts) {
            if (cJSON_IsString(v) && strcmp(v->valuestring, o) == 0) {
                out = o;
                return true;
            }
            all += all.empty() ? o : std::string(", ") + o;
        }
        return fail("one of " + all);
    }
    bool num(int &out, int lo, int hi)
    {
        if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
            v->valuedouble != static_cast<int>(v->valuedouble))
            return fail("a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
        out = static_cast<int>(v->valuedouble);
        return true;
    }
    bool real(double &out, double lo, double hi)
    {
        if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) return fail("out of range");
        out = v->valuedouble;
        return true;
    }
    bool flag(bool &out)
    {
        if (!cJSON_IsBool(v)) return fail("true or false");
        out = cJSON_IsTrue(v);
        return true;
    }
    bool password(std::string &out)
    {
        if (!cJSON_IsString(v)) return fail("text");
        size_t n = strlen(v->valuestring);
        if (n != 0 && (n < 8 || n > 63)) return fail("8 to 63 characters (empty = open network)");
        out = v->valuestring;
        return true;
    }
};

bool is_hex(const std::string &s)
{
    for (char c : s)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// Keys that take effect only after a restart (web.py's _EDITABLE).
const std::set<std::string> kRebootKeys = {
    "linked_scan_duty", "screen", "battery_pin", "hostname", "ap_password", "serial", "dis_model",
    "max_conn", "pins", "role", "split_peer", "split_key", "split_port", "wifi_ssid", "wifi_password",
    "supervision_probe_ms", "supervision_node_ms", "peer_latency_max", "coex"};

}  // namespace

bool apply_config(Settings &s, const cJSON *req, std::string &err, bool &reboot)
{
    if (!cJSON_IsObject(req)) {
        err = "expected a JSON object";
        return false;
    }
    Settings n = s;
    bool rb = false;
    const cJSON *v;
    cJSON_ArrayForEach(v, req) {
        const std::string k = v->string;
        Check c{v, v->string, err};
        bool ok = true;
        if (k == "unit") ok = c.one_of(n.unit, {"c", "f"});
        else if (k == "layout") ok = c.one_of(n.layout, {"rotate", "quad"});
        else if (k == "rotate_s") ok = c.num(n.rotate_s, 2, 120);
        else if (k == "brightness") ok = c.num(n.brightness, 5, 100);
        else if (k == "status_page") ok = c.flag(n.status_page);
        else if (k == "readings") {
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 16) {
                ok = c.fail("a list of at most 16 readings");
            } else {
                n.readings.clear();
                const cJSON *e;
                cJSON_ArrayForEach(e, v) {
                    Reading r;
                    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(e, "dev");
                    const cJSON *field = cJSON_GetObjectItemCaseSensitive(e, "field");
                    const cJSON *label = cJSON_GetObjectItemCaseSensitive(e, "label");
                    if (!cJSON_IsString(dev) || !cJSON_IsString(field) || (label && !cJSON_IsString(label)) ||
                        strlen(dev->valuestring) > 24 || strlen(field->valuestring) > 12 ||
                        (label && strlen(label->valuestring) > 16)) {
                        ok = c.fail("each reading needs dev, field and an optional label (16 characters)");
                        break;
                    }
                    r.dev = dev->valuestring;
                    r.field = field->valuestring;
                    if (label) r.label = label->valuestring;
                    n.readings.push_back(r);
                }
            }
        } else if (k == "linked_probes" || k == "upstream_node") {
            bool probes = k == "linked_probes";
            size_t len = probes ? 8 : 10;
            std::vector<std::string> out;
            if (!probes && cJSON_IsString(v)) {         // a single serial (older pages)
                if (*v->valuestring) out.push_back(v->valuestring);
            } else if (cJSON_IsArray(v)) {
                const cJSON *e;
                cJSON_ArrayForEach(e, v) {
                    if (!cJSON_IsString(e)) {
                        ok = false;
                        break;
                    }
                    out.push_back(e->valuestring);
                }
            } else {
                ok = false;
            }
            for (auto &x : out) {
                x = upper(x);
                if (x.size() != len || (probes && !is_hex(x))) ok = false;
            }
            if (probes && out.size() > 8) out.resize(8);
            if (!ok) c.fail(probes ? "a list of 8-digit probe serials" : "a list of 10-character node serials");
            else (probes ? n.linked_probes : n.upstream_node) = out;
        } else if (k == "repeat_adverts") ok = c.one_of(n.repeat_adverts, {"all", "linked", "off"});
        else if (k == "meatnet_peer") ok = c.flag(n.meatnet_peer);
        else if (k == "upstream_push") ok = c.flag(n.upstream_push);
        else if (k == "inbound_reserve") ok = c.num(n.inbound_reserve, 0, 4);
        else if (k == "auto_standby_min") ok = c.num(n.auto_standby_min, 0, 1440);
        else if (k == "meater_bridge") ok = c.flag(n.meater_bridge);
        else if (k == "meater_direct_adverts") ok = c.flag(n.meater_direct_adverts);
        else if (k == "meater_probes") {
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 8) {
                ok = c.fail("a list of at most 8 MEATERs");
            } else {
                n.meater_probes.clear();
                const cJSON *e;
                cJSON_ArrayForEach(e, v) {
                    MeaterProbe m;
                    const cJSON *addr = cJSON_GetObjectItemCaseSensitive(e, "addr");
                    const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
                    const cJSON *color = cJSON_GetObjectItemCaseSensitive(e, "color");
                    if (!cJSON_IsString(addr) || strlen(addr->valuestring) != 17 ||
                        (id && (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 8)) ||
                        (color && (!cJSON_IsNumber(color) || color->valuedouble < 0 || color->valuedouble > 7))) {
                        ok = c.fail("each needs addr (AA:BB:CC:DD:EE:FF), id 1-8 and color 0-7");
                        break;
                    }
                    m.addr = upper(addr->valuestring);
                    if (id) m.id = static_cast<int>(id->valuedouble);
                    if (color) m.color = static_cast<int>(color->valuedouble);
                    n.meater_probes.push_back(m);
                }
            }
        } else if (k == "battery_divider") ok = c.real(n.battery_divider, 0.1, 20);
        else if (k == "dis_firmware") ok = c.str(n.dis_firmware, 16);
        else if (k == "linked_scan_duty") ok = c.num(n.linked_scan_duty, 5, 50);
        else if (k == "screen") ok = c.flag(n.screen);
        else if (k == "battery_pin") ok = c.num(n.battery_pin, -1, 48);
        else if (k == "hostname") ok = c.str(n.hostname, 32);
        else if (k == "ap_password") ok = c.password(n.ap_password);
        else if (k == "serial") {
            ok = c.str(n.serial, 10) && (n.serial.empty() || n.serial.size() == 10 || c.fail("10 characters"));
        } else if (k == "dis_model") ok = c.str(n.dis_model, 32);
        else if (k == "max_conn") ok = c.num(n.max_conn, 1, 9);
        else if (k == "pins") {
            if (!cJSON_IsObject(v)) {
                ok = c.fail("an object");
            } else {
                const cJSON *p;
                cJSON_ArrayForEach(p, v) {
                    Check pc{p, p->string, err};
                    std::string pk = p->string;
                    int *slot = pk == "sck" ? &n.pins.sck : pk == "mosi" ? &n.pins.mosi : pk == "dc" ? &n.pins.dc
                              : pk == "cs" ? &n.pins.cs : pk == "rst" ? &n.pins.rst : pk == "bl" ? &n.pins.bl
                              : pk == "spi" ? &n.pins.spi : pk == "rotation" ? &n.pins.rotation : nullptr;
                    if (pk == "baud") ok = pc.num(n.pins.baud, 1000000, 80000000);
                    else if (pk == "rotation") ok = pc.num(n.pins.rotation, 0, 3);
                    else if (slot) ok = pc.num(*slot, -1, 48);
                    if (!ok) break;
                }
            }
        } else if (k == "role") ok = c.one_of(n.role, {"standalone", "app", "field"});
        else if (k == "split_peer") ok = c.str(n.split_peer, 64);
        else if (k == "split_key") {
            std::string key;
            ok = c.str(key, 64);
            if (ok && !key.empty()) n.split_key = key;      // blank = keep
        } else if (k == "split_port") ok = c.num(n.split_port, 1, 65535);
        else if (k == "wifi_ssid") ok = c.str(n.wifi_ssid, 32);
        else if (k == "wifi_password") ok = c.password(n.wifi_password);
        else if (k == "supervision_probe_ms") ok = c.num(n.supervision_probe_ms, 1000, 32000);
        else if (k == "supervision_node_ms") ok = c.num(n.supervision_node_ms, 1000, 32000);
        else if (k == "peer_latency_max") ok = c.num(n.peer_latency_max, 0, 10);
        else if (k == "coex") ok = c.one_of(n.coex, {"bt", "balance", "wifi"});
        if (!ok) return false;
        rb = rb || kRebootKeys.count(k);
    }
    s = n;
    reboot = rb;
    return true;
}

}  // namespace core
