#include "unit_settings.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <variant>

#include "cJSON.h"

namespace core {

namespace {

// The scalar settings: one row drives loading, saving and the web page's
// validation. The list-valued ones (readings, linked_probes, upstream_node,
// meater_probes, pins) are handled by name below.
enum : unsigned {
    REBOOT = 1,         // takes effect only after a restart
    SECRET = 2,         // shown to the page as "<name>_set"
    PASSWORD = 4,       // "" (open network) or 8-63 characters
    KEEP_BLANK = 8,     // an empty value from the page keeps the saved one
    EXACT = 16,         // empty or exactly `hi` characters
    FIXED = 32,         // not changed from the page
};

struct Field {
    const char *name;
    std::variant<std::string Settings::*, int Settings::*, double Settings::*, bool Settings::*> member;
    double lo, hi;              // numbers: the range; text: hi = most characters
    const char *choices;        // text: one of these (comma-separated)
    unsigned flags;
};

const Field kFields[] = {
    {"role", &Settings::role, 0, 0, "standalone,app,field", REBOOT},
    {"split_peer", &Settings::split_peer, 0, 64, nullptr, REBOOT},
    {"split_key", &Settings::split_key, 0, 64, nullptr, REBOOT | SECRET | KEEP_BLANK},
    {"split_port", &Settings::split_port, 1, 65535, nullptr, REBOOT},
    {"wifi_ssid", &Settings::wifi_ssid, 0, 32, nullptr, REBOOT},
    {"wifi_password", &Settings::wifi_password, 0, 0, nullptr, REBOOT | SECRET | PASSWORD},
    {"ap_password", &Settings::ap_password, 0, 0, nullptr, REBOOT | SECRET | PASSWORD},
    {"hostname", &Settings::hostname, 0, 32, nullptr, REBOOT},
    {"serial", &Settings::serial, 0, 10, nullptr, REBOOT | EXACT},
    {"dis_model", &Settings::dis_model, 0, 32, nullptr, REBOOT},
    {"dis_firmware", &Settings::dis_firmware, 0, 16, nullptr, 0},
    {"unit", &Settings::unit, 0, 0, "c,f", 0},
    {"layout", &Settings::layout, 0, 0, "rotate,quad", 0},
    {"rotate_s", &Settings::rotate_s, 2, 120, nullptr, 0},
    {"brightness", &Settings::brightness, 5, 100, nullptr, 0},
    {"screen", &Settings::screen, 0, 0, nullptr, REBOOT},
    {"status_page", &Settings::status_page, 0, 0, nullptr, 0},
    {"repeat_adverts", &Settings::repeat_adverts, 0, 0, "all,linked,off", 0},
    {"meatnet_peer", &Settings::meatnet_peer, 0, 0, nullptr, 0},
    {"upstream_push", &Settings::upstream_push, 0, 0, nullptr, 0},
    {"max_conn", &Settings::max_conn, 1, 9, nullptr, REBOOT},
    {"linked_scan_duty", &Settings::linked_scan_duty, 5, 50, nullptr, REBOOT},
    {"inbound_reserve", &Settings::inbound_reserve, 0, 4, nullptr, 0},
    {"meater_bridge", &Settings::meater_bridge, 0, 0, nullptr, 0},
    {"meater_direct_adverts", &Settings::meater_direct_adverts, 0, 0, nullptr, 0},
    {"standby", &Settings::standby, 0, 0, nullptr, FIXED},
    {"auto_standby_min", &Settings::auto_standby_min, 0, 1440, nullptr, 0},
    {"battery_pin", &Settings::battery_pin, -1, 48, nullptr, REBOOT},
    {"battery_divider", &Settings::battery_divider, 0.1, 20, nullptr, 0},
    {"supervision_probe_ms", &Settings::supervision_probe_ms, 1000, 32000, nullptr, REBOOT},
    {"supervision_node_ms", &Settings::supervision_node_ms, 1000, 32000, nullptr, REBOOT},
    {"peer_latency_max", &Settings::peer_latency_max, 0, 10, nullptr, REBOOT},
    {"coex", &Settings::coex, 0, 0, "bt,balance,wifi", REBOOT},
};

struct PinField {
    const char *name;
    int Pins::*member;
    int lo, hi;
};

const PinField kPins[] = {
    {"sck", &Pins::sck, -1, 48},   {"mosi", &Pins::mosi, -1, 48}, {"dc", &Pins::dc, -1, 48},
    {"cs", &Pins::cs, -1, 48},     {"rst", &Pins::rst, -1, 48},   {"bl", &Pins::bl, -1, 48},
    {"spi", &Pins::spi, -1, 48},   {"baud", &Pins::baud, 1000000, 80000000},
    {"rotation", &Pins::rotation, 0, 3},
};

}  // namespace

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
    for (const Field &f : kFields) {
        if (auto m = std::get_if<std::string Settings::*>(&f.member)) take(o, f.name, s.**m);
        else if (auto m = std::get_if<int Settings::*>(&f.member)) take(o, f.name, s.**m);
        else if (auto m = std::get_if<double Settings::*>(&f.member)) take(o, f.name, s.**m);
        else if (auto m = std::get_if<bool Settings::*>(&f.member)) take(o, f.name, s.**m);
    }
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
    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(o, "pins");
    if (cJSON_IsObject(pins))
        for (const PinField &p : kPins) take(pins, p.name, s.pins.*p.member);
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

cJSON *settings_to_json(const Settings &s, bool secrets)
{
    cJSON *o = cJSON_CreateObject();
    for (const Field &f : kFields) {
        if (auto m = std::get_if<std::string Settings::*>(&f.member)) {
            if (!(f.flags & SECRET) || secrets) cJSON_AddStringToObject(o, f.name, (s.**m).c_str());
            else cJSON_AddBoolToObject(o, (std::string(f.name) + "_set").c_str(), !(s.**m).empty());
        } else if (auto m = std::get_if<int Settings::*>(&f.member)) {
            cJSON_AddNumberToObject(o, f.name, s.**m);
        } else if (auto m = std::get_if<double Settings::*>(&f.member)) {
            cJSON_AddNumberToObject(o, f.name, s.**m);
        } else if (auto m = std::get_if<bool Settings::*>(&f.member)) {
            cJSON_AddBoolToObject(o, f.name, s.**m);
        }
    }
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
    cJSON *mp = cJSON_AddArrayToObject(o, "meater_probes");
    for (auto &m : s.meater_probes) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", m.addr.c_str());
        cJSON_AddNumberToObject(e, "id", m.id);
        cJSON_AddNumberToObject(e, "color", m.color);
        cJSON_AddItemToArray(mp, e);
    }
    cJSON *p = cJSON_AddObjectToObject(o, "pins");
    for (const PinField &pf : kPins) cJSON_AddNumberToObject(p, pf.name, s.pins.*pf.member);
    return o;
}

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
    bool one_of(std::string &out, const std::string &choices)       // "a,b,c"
    {
        std::string all;
        for (size_t at = 0; at <= choices.size();) {
            size_t end = std::min(choices.find(',', at), choices.size());
            std::string opt = choices.substr(at, end - at);
            if (cJSON_IsString(v) && opt == v->valuestring) {
                out = opt;
                return true;
            }
            all += (all.empty() ? "" : ", ") + opt;
            at = end + 1;
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
    // One row of kFields.
    bool field(const Field &f, Settings &n)
    {
        if (auto m = std::get_if<int Settings::*>(&f.member))
            return num(n.**m, static_cast<int>(f.lo), static_cast<int>(f.hi));
        if (auto m = std::get_if<double Settings::*>(&f.member)) return real(n.**m, f.lo, f.hi);
        if (auto m = std::get_if<bool Settings::*>(&f.member)) return flag(n.**m);
        std::string &out = n.**std::get_if<std::string Settings::*>(&f.member);
        if (f.flags & PASSWORD) return password(out);
        if (f.choices) return one_of(out, f.choices);
        std::string text;
        size_t max = static_cast<size_t>(f.hi);
        if (!str(text, max)) return false;
        if ((f.flags & EXACT) && !text.empty() && text.size() != max)
            return fail(std::to_string(max) + " characters");
        if (!(f.flags & KEEP_BLANK) || !text.empty()) out = text;
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
        auto f = std::find_if(std::begin(kFields), std::end(kFields), [&](const Field &x) { return k == x.name; });
        if (f != std::end(kFields)) {
            if (f->flags & FIXED) continue;
            ok = c.field(*f, n);
            rb = rb || (f->flags & REBOOT);
        } else if (k == "readings") {
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
            if (cJSON_IsArray(v)) {
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
        } else if (k == "meater_probes") {
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
        } else if (k == "pins") {
            rb = true;
            if (!cJSON_IsObject(v)) {
                ok = c.fail("an object");
            } else {
                const cJSON *p;
                cJSON_ArrayForEach(p, v) {
                    Check pc{p, p->string, err};
                    for (const PinField &pf : kPins)
                        if (strcmp(p->string, pf.name) == 0) ok = pc.num(n.pins.*pf.member, pf.lo, pf.hi);
                    if (!ok) break;
                }
            }
        }
        if (!ok) return false;
    }
    s = n;
    reboot = rb;
    return true;
}

}  // namespace core
