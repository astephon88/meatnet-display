#include "linktable.h"

#include <algorithm>

#include "cJSON.h"
#include "proto.h"

namespace core {

using protocol::Bytes;
using protocol::LinkEntry;

// Entries are the same device when the probe/non-probe kind and serial match.
static bool same(const LinkEntry &a, const LinkEntry &b)
{
    return (a.ptype == protocol::PRODUCT_TYPE_PROBE) == (b.ptype == protocol::PRODUCT_TYPE_PROBE) &&
           a.serial == b.serial;
}

static std::vector<LinkEntry> from_json(const cJSON *arr)
{
    std::vector<LinkEntry> out;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        const cJSON *t = cJSON_GetObjectItem(e, "t"), *s = cJSON_GetObjectItem(e, "s");
        Bytes serial;
        if (!cJSON_IsNumber(t) || !cJSON_IsString(s) || !proto::unhex(s->valuestring, serial))
            continue;
        const cJSON *ts = cJSON_GetObjectItem(e, "ts"), *l = cJSON_GetObjectItem(e, "l");
        out.push_back({t->valueint, serial,
                       cJSON_IsNumber(ts) ? static_cast<uint32_t>(ts->valuedouble) : 0,
                       cJSON_IsNumber(l) && l->valueint != 0});
    }
    return out;
}

void LinkTable::load(const std::string &json)
{
    linked.clear();
    unlinked.clear();
    proximity = false;
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) return;
    linked = from_json(cJSON_GetObjectItem(root, "linked"));
    unlinked = from_json(cJSON_GetObjectItem(root, "unlinked"));
    proximity = cJSON_IsTrue(cJSON_GetObjectItem(root, "proximity"));
    cJSON_Delete(root);
}

static cJSON *to_json_arr(const std::vector<LinkEntry> &lst)
{
    cJSON *arr = cJSON_CreateArray();
    for (const auto &e : lst) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", e.ptype);
        cJSON_AddStringToObject(o, "s", proto::hex(e.serial).c_str());
        cJSON_AddNumberToObject(o, "ts", e.ts);
        cJSON_AddNumberToObject(o, "l", e.legacy ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

std::string LinkTable::to_json() const
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "linked", to_json_arr(linked));
    cJSON_AddItemToObject(root, "unlinked", to_json_arr(unlinked));
    cJSON_AddBoolToObject(root, "proximity", proximity);
    char *text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

void LinkTable::save() const
{
    if (saver) saver(to_json());
}

// Insert/refresh e in lst and remove it from other. True if anything changed.
bool LinkTable::put(std::vector<LinkEntry> &lst, std::vector<LinkEntry> &other, const LinkEntry &e)
{
    size_t n = other.size();
    other.erase(std::remove_if(other.begin(), other.end(),
                               [&](const LinkEntry &x) { return same(x, e); }),
                other.end());
    bool changed = other.size() != n;
    for (auto &x : lst) {
        if (same(x, e)) {
            if (x.ts != e.ts && e.ts) {
                x.ts = e.ts;
                changed = true;
            }
            if (x.legacy && !e.legacy) {
                x.legacy = false;
                changed = true;
            }
            return changed;
        }
    }
    lst.push_back(e);
    return true;
}

bool LinkTable::link(const LinkEntry &e)
{
    bool changed = put(linked, unlinked, e);
    if (changed) save();
    return changed;
}

bool LinkTable::unlink(const LinkEntry &e)
{
    bool changed = put(unlinked, linked, e);
    if (changed) save();
    return changed;
}

bool LinkTable::merge(const std::vector<LinkEntry> &entries, bool linked_list,
                      const std::string &own_serial10, const Bytes &sender_serial10)
{
    Bytes own = protocol::encode_serial(own_serial10);
    bool changed = false;
    for (LinkEntry e : entries) {
        if (e.ptype != protocol::PRODUCT_TYPE_PROBE && protocol::encode_serial(e.serial) == own) {
            if (sender_serial10.empty()) continue;
            e.serial = protocol::encode_serial(sender_serial10);
        }
        if (linked_list ? put(linked, unlinked, e) : put(unlinked, linked, e)) changed = true;
    }
    if (changed) save();
    return changed;
}

}  // namespace core
