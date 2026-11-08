#include "links.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "sdkconfig.h"
#include "host/ble_hs.h"
#include "proto.h"
#include "radio.h"

namespace radio {

static const char *TAG = "links";

static constexpr int64_t kConnectTimeoutMs = 12000;
static constexpr int64_t kSetupTimeoutMs = 20000;
static constexpr int64_t kDisconnectTimeoutMs = 8000;
static constexpr int64_t kWriteTimeoutMs = 3000;
static constexpr int64_t kDropRetryMs = 1000;   // after an unexpected drop

// Connection parameters (units: interval 1.25 ms, timeout 10 ms). The
// supervision timeout is generous (NimBLE's default is ~2.5 s; phones use
// 5 s+) so a moment of radio contention doesn't drop the link, and it is
// kept as a floor if the peer asks for different parameters.
static constexpr uint16_t kItvlMin = 24;        // 30 ms
static constexpr uint16_t kItvlMax = 40;        // 50 ms
static constexpr uint16_t kSupervisionProbe = CONFIG_MEATNET_SUPERVISION_PROBE_MS / 10;
static constexpr uint16_t kSupervisionNode = CONFIG_MEATNET_SUPERVISION_NODE_MS / 10;

// -- per-link state ----------------------------------------------------------------

struct Sub {
    std::string name;
    ble_uuid_any_t uuid;
    bool required;
    uint16_t val = 0, end = 0, cccd = 0;
    bool on = false;
};

struct Read {
    std::string name;
    ble_uuid_any_t uuid;
    uint16_t handle = 0;
    bool done = false;
};

struct Svc {
    uint16_t start, end;
};

struct Link {
    uint32_t id = 0;
    std::string key, kind, addr;
    uint8_t addr_type = 0;
    bool enabled = true;
    std::string state = "idle";
    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    int mtu = 23;
    uint32_t count = 0;
    std::vector<ble_uuid_any_t> svc_uuids;  // services to discover; empty: all
    size_t svc_q = 0;
    std::vector<Svc> svcs;
    size_t svc_i = 0;
    std::vector<Sub> subs;
    std::vector<Read> reads;
    bool has_write = false;
    ble_uuid_any_t write_uuid;
    uint16_t write_h = 0;
    bool write_rsp = false;
    std::map<std::string, std::string> info;
    std::map<std::string, std::deque<std::string>> rx;
    size_t keep = 64;
    std::deque<std::string> txq;
    bool tx_busy = false;
    int64_t tx_at = 0;
    int64_t started = 0, connected_at = 0, err_at = 0, disc_at = 0;
    int64_t backoff_ms = 10000;
    bool disconnect_sent = false;
    // diagnostics
    uint32_t connects = 0;
    std::map<std::string, uint32_t> drops;
    std::string params;
    std::string asked;          // what the peer last asked for
    int64_t step_at = 0;        // setup timing: when the current step began
    std::string timing;         // "connect 820ms, services 1500ms (5), ..."

    uint16_t supervision() const { return kind == "node" ? kSupervisionNode : kSupervisionProbe; }

    void reset_conn()
    {
        conn = BLE_HS_CONN_HANDLE_NONE;
        mtu = 23;
        svc_q = 0;
        svcs.clear();
        svc_i = 0;
        for (auto &s : subs) s = Sub{s.name, s.uuid, s.required};
        for (auto &r : reads) {
            r.handle = 0;
            r.done = false;
        }
        write_h = 0;
        write_rsp = false;
        txq.clear();
        tx_busy = false;
        disconnect_sent = false;
        connected_at = 0;
        step_at = 0;
        timing.clear();
    }
};

static ble_uuid_any_t uuid128(const char *text)
{
    ble_uuid_any_t u;
    memset(&u, 0, sizeof(u));
    u.u.type = BLE_UUID_TYPE_128;
    proto::uuid128_le(text, u.u128.value);
    return u;
}

static ble_uuid_any_t uuid16(uint16_t v)
{
    ble_uuid_any_t u;
    memset(&u, 0, sizeof(u));
    u.u.type = BLE_UUID_TYPE_16;
    u.u16.value = v;
    return u;
}

static bool same(const ble_uuid_any_t &a, const ble_uuid_any_t &b)
{
    return ble_uuid_cmp(&a.u, &b.u) == 0;
}

static const char *kProbeStatusSvc = "00000100-CAAB-3792-3D44-97AE51C1407A";
static const char *kUartSvc = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *kProbeStatus = "00000101-CAAB-3792-3D44-97AE51C1407A";
static const char *kUartTx = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *kUartRx = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *kMeaterTemp = "7EDDA774-045E-4BBF-909B-45D1991A2876";
static const char *kMeaterBattery = "2ADB4877-68D8-4884-BD3C-D83853BF27B8";

static void configure(Link &l)
{
    // Discovering only the services we use saves seconds per connect. MEATER
    // service UUIDs vary by model, so those links discover everything.
    if (l.kind == "probe") {
        l.svc_uuids = {uuid128(kProbeStatusSvc), uuid128(kUartSvc), uuid16(0x180A)};
        l.subs = {{"status", uuid128(kProbeStatus), true}, {"uart", uuid128(kUartTx), false}};
        l.has_write = true;
        l.write_uuid = uuid128(kUartRx);
        l.reads = {{"firmware", uuid16(0x2A26)}, {"hardware", uuid16(0x2A27)},
                   {"model", uuid16(0x2A24)}};
        l.keep = 64;
        l.backoff_ms = 10000;
    } else if (l.kind == "meater") {
        l.subs = {{"temp", uuid128(kMeaterTemp), true}, {"battery", uuid128(kMeaterBattery), false}};
        l.keep = 4;
        l.backoff_ms = 10000;
    } else {                                // node: Gauge / Engine / Display
        l.kind = "node";
        l.svc_uuids = {uuid128(kUartSvc)};
        l.subs = {{"uart", uuid128(kUartTx), true}};
        l.has_write = true;
        l.write_uuid = uuid128(kUartRx);
        l.keep = 160;                       // log reads stream hard
        l.backoff_ms = 2000;                // relays devices: get it back fast
    }
}

static std::string describe(const Link &l)
{
    std::string d;
    for (const auto &s : l.subs) {
        if (!d.empty()) d += ", ";
        d += s.name + (s.on ? " notify on" : " notify OFF");
    }
    if (l.has_write) {
        char buf[48];
        snprintf(buf, sizeof(buf), ", write h%u (%s)", l.write_h,
                 l.write_rsp ? "with response" : "no response");
        d += l.write_h ? buf : ", write MISSING";
    }
    return d;
}

std::string disconnect_reason(int reason)
{
    int code = reason >= BLE_HS_ERR_HCI_BASE ? reason - BLE_HS_ERR_HCI_BASE : -1;
    const char *text;
    switch (code) {
    case 0x08: text = "supervision timeout (signal lost)"; break;
    case 0x13: text = "peer disconnected"; break;
    case 0x14: text = "peer disconnected (low resources)"; break;
    case 0x15: text = "peer powered off"; break;
    case 0x16: text = "disconnected locally"; break;
    case 0x22: text = "link-layer response timeout"; break;
    case 0x28: text = "instant passed"; break;
    case 0x3B: text = "unacceptable connection parameters"; break;
    case 0x3D: text = "MIC failure"; break;
    case 0x3E: text = "failed to establish"; break;
    default: text = nullptr;
    }
    char buf[64];
    if (text)
        snprintf(buf, sizeof(buf), "%s (0x%02x)", text, code);
    else
        snprintf(buf, sizeof(buf), "disconnected (reason %d)", reason);
    return buf;
}

// -- GATT procedure chain (runs in NimBLE callbacks, host task) --------------------

// "interval ms/latency/timeout ms" of a live connection.
static std::string current_params(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return "";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.1fms/%u/%ums", d.conn_itvl * 1.25, d.conn_latency,
             d.supervision_timeout * 10);
    return buf;
}

static inline void *arg_of(const Link &l)
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(l.id));
}

static inline uint32_t id_of(void *arg)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
}

struct LinkOps {
    // Ends the current setup step, adding it to the link's timing summary.
    static void mark(Link &l, const std::string &step)
    {
        int64_t now = Radio::now_ms();
        if (!l.timing.empty()) l.timing += ", ";
        l.timing += step + " " + std::to_string(now - l.step_at) + "ms";
        l.step_at = now;
    }

    static void fail(Link &l, const std::string &why)
    {
        l.state = "error: " + why;
        l.err_at = Radio::now_ms();
        ESP_LOGW(TAG, "%s %s: %s", l.kind.c_str(), proto::hex(l.key).c_str(), l.state.c_str());
        if (l.conn != BLE_HS_CONN_HANDLE_NONE && !l.disconnect_sent) {
            l.disconnect_sent = true;
            l.disc_at = l.err_at;
            ble_gap_terminate(l.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }

    static int svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) {
            if (err->status == 0 && svc) {
                l->svcs.push_back({svc->start_handle, svc->end_handle});
            } else if (err->status == BLE_HS_EDONE) {
                l->svc_q++;
                next_svc(*l);
            } else {
                fail(*l, "service discovery failed (" + std::to_string(err->status) + ")");
            }
        }
        ls.unlock();
        return 0;
    }

    // Discovers the link's services one UUID at a time (or all at once).
    static void next_svc(Link &l)
    {
        int rc = 0;
        if (l.svc_uuids.empty() && l.svc_q == 0)
            rc = ble_gattc_disc_all_svcs(l.conn, svc_cb, arg_of(l));
        else if (l.svc_q < l.svc_uuids.size())
            rc = ble_gattc_disc_svc_by_uuid(l.conn, &l.svc_uuids[l.svc_q].u, svc_cb, arg_of(l));
        else {
            mark(l, "services");
            l.timing += " (" + std::to_string(l.svcs.size()) + ")";
            if (l.svcs.empty()) fail(l, "no services");
            else next_chrs(l);
            return;
        }
        if (rc != 0) fail(l, "service discovery failed (" + std::to_string(rc) + ")");
    }

    static void next_chrs(Link &l)
    {
        if (l.svc_i < l.svcs.size()) {
            const Svc &s = l.svcs[l.svc_i];
            int rc = ble_gattc_disc_all_chrs(l.conn, s.start, s.end, chr_cb, arg_of(l));
            if (rc != 0) fail(l, "characteristic discovery failed (" + std::to_string(rc) + ")");
            return;
        }
        for (const auto &s : l.subs)
            if (s.required && !s.val) {
                fail(l, "characteristic not found");
                return;
            }
        mark(l, "characteristics");
        next_dsc(l);
    }

    static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) {
            if (err->status == 0 && chr) {
                ble_uuid_any_t u;
                memcpy(&u, &chr->uuid, sizeof(u));
                uint16_t end = l->svcs[l->svc_i].end;
                for (auto &s : l->subs)
                    if (!s.val && same(s.uuid, u)) {
                        s.val = chr->val_handle;
                        s.end = end;
                    }
                for (auto &r : l->reads)
                    if (!r.handle && same(r.uuid, u)) r.handle = chr->val_handle;
                if (l->has_write && !l->write_h && same(l->write_uuid, u)) {
                    l->write_h = chr->val_handle;
                    // Write without response if the peer allows it (what the
                    // Combustion apps use), else acked writes.
                    l->write_rsp = !(chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP);
                }
            } else if (err->status == BLE_HS_EDONE) {
                l->svc_i++;
                next_chrs(*l);
            } else {
                fail(*l, "characteristic discovery failed (" + std::to_string(err->status) + ")");
            }
        }
        ls.unlock();
        return 0;
    }

    static void next_dsc(Link &l)
    {
        for (auto &s : l.subs) {
            if (s.val && !s.cccd && !s.on) {
                int rc = ble_gattc_disc_all_dscs(l.conn, s.val, s.end, dsc_cb, arg_of(l));
                if (rc != 0) fail(l, "descriptor discovery failed (" + std::to_string(rc) + ")");
                return;
            }
        }
        mark(l, "descriptors");
        next_notify(l);
    }

    static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                      const struct ble_gatt_dsc *dsc, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) {
            Sub *sub = nullptr;
            for (auto &s : l->subs)
                if (s.val == chr_val) sub = &s;
            if (err->status == 0 && dsc && sub) {
                if (!sub->cccd && dsc->handle > chr_val &&
                    ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
                    sub->cccd = dsc->handle;
            } else if (err->status == BLE_HS_EDONE) {
                if (sub && !sub->cccd) sub->on = false, sub->cccd = 0xFFFF;   // none: skip
                next_dsc(*l);
            } else {
                fail(*l, "descriptor discovery failed (" + std::to_string(err->status) + ")");
            }
        }
        ls.unlock();
        return 0;
    }

    static void next_notify(Link &l)
    {
        for (auto &s : l.subs) {
            if (s.cccd && s.cccd != 0xFFFF && !s.on) {
                static const uint8_t enable[2] = {1, 0};
                int rc = ble_gattc_write_flat(l.conn, s.cccd, enable, 2, notify_cb, arg_of(l));
                if (rc != 0) fail(l, "enabling notifications failed (" + std::to_string(rc) + ")");
                return;
            }
        }
        for (const auto &s : l.subs)
            if (s.required && !s.on) {
                fail(l, "no CCCD for notifications");
                return;
            }
        mark(l, "notify");
        // Exchange the MTU before reporting ready, so large frames fit.
        int rc = ble_gattc_exchange_mtu(l.conn, mtu_cb, arg_of(l));
        if (rc != 0) ready(l);              // peer may have exchanged already
    }

    static int notify_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) {
            if (err->status == 0) {
                for (auto &s : l->subs)
                    if (attr && s.cccd == attr->handle) s.on = true;
                next_notify(*l);
            } else {
                fail(*l, "enabling notifications failed (" + std::to_string(err->status) + ")");
            }
        }
        ls.unlock();
        return 0;
    }

    static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) {
            if (err->status == 0) l->mtu = mtu;
            ready(*l);
        }
        ls.unlock();
        return 0;
    }

    static void ready(Link &l)
    {
        if (l.mtu <= 23) l.mtu = ble_att_mtu(l.conn);
        l.state = "ready";
        mark(l, "mtu");
        ESP_LOGI(TAG, "%s %s ready: %s, mtu %d", l.kind.c_str(), proto::hex(l.key).c_str(),
                 describe(l).c_str(), l.mtu);
        ESP_LOGI(TAG, "%s %s setup took %lldms: %s", l.kind.c_str(), proto::hex(l.key).c_str(),
                 (long long)(l.step_at - l.started), l.timing.c_str());
        next_read(l);
    }

    // One-off reads (probe Device Information) after the link is ready.
    static void next_read(Link &l)
    {
        for (auto &r : l.reads) {
            if (r.handle && !r.done) {
                int rc = ble_gattc_read(l.conn, r.handle, read_cb, arg_of(l));
                if (rc != 0) r.done = true;
                return;
            }
        }
    }

    static int read_cb(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn && attr) {
            for (auto &r : l->reads) {
                if (r.handle == attr->handle && !r.done) {
                    r.done = true;
                    if (err->status == 0 && attr->om) {
                        uint16_t len = OS_MBUF_PKTLEN(attr->om);
                        std::string v(len, '\0');
                        os_mbuf_copydata(attr->om, 0, len, &v[0]);
                        v.erase(std::find(v.begin(), v.end(), '\0'), v.end());
                        l->info[r.name] = v;
                    }
                }
            }
            next_read(*l);
        }
        ls.unlock();
        return 0;
    }

    static int write_cb(uint16_t conn, const struct ble_gatt_error *, struct ble_gatt_attr *,
                        void *arg)
    {
        Links &ls = Links::get();
        ls.lock();
        Link *l = ls.find(id_of(arg));
        if (l && l->conn == conn) l->tx_busy = false;
        ls.unlock();
        return 0;
    }
};

// -- connection events ----------------------------------------------------------------

int Links::gap_event(::ble_gap_event *event, void *arg)
{
    Links &ls = Links::get();
    ls.lock();
    Link *l = ls.find(id_of(arg));
    if (!l) {
        // A connection nobody wants any more (link closed meanwhile): drop it.
        if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0)
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        ls.unlock();
        return 0;
    }
    int64_t now = Radio::now_ms();
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            l->conn = event->connect.conn_handle;
            l->connected_at = now;
            l->connects++;
            l->params = current_params(l->conn);
            l->state = "discovering";
            l->step_at = l->started;
            LinkOps::mark(*l, "connect");
            LinkOps::next_svc(*l);
        } else {
            l->reset_conn();
            l->state = "error: connect failed (" + std::to_string(event->connect.status) + ")";
            l->err_at = now;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        std::string why = disconnect_reason(event->disconnect.reason);
        bool was_error = l->state.rfind("error", 0) == 0;
        bool expected = !l->enabled || l->disconnect_sent;
        int64_t up_s = l->connected_at ? (now - l->connected_at) / 1000 : 0;
        if (!expected) l->drops[why]++;
        l->reset_conn();
        if (!l->enabled) {
            l->state = "idle";
        } else if (!was_error) {
            // Unexpected drop: report why, then reconnect promptly.
            l->state = "error: " + why;
            l->err_at = now - l->backoff_ms + kDropRetryMs;
        }
        if (!expected)
            ESP_LOGW(TAG, "%s %s dropped after %llds: %s (params %s, %u drops so far)",
                     l->kind.c_str(), proto::hex(l->key).c_str(), (long long)up_s,
                     why.c_str(), l->params.c_str(), (unsigned)l->drops[why]);
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        // The peer asks for new connection parameters (Combustion devices ask
        // for a 500 ms interval with latency 2 right after connecting). Keep
        // their interval, but cap the latency so they answer more often, and
        // keep our supervision timeout as a floor: at 500 ms x (1+2) a 6 s
        // timeout leaves only ~4 chances to hear the peer.
        const struct ble_gap_upd_params *peer = event->conn_update_req.peer_params;
        struct ble_gap_upd_params *self = event->conn_update_req.self_params;
        if (self->latency > CONFIG_MEATNET_PEER_LATENCY_MAX) self->latency = CONFIG_MEATNET_PEER_LATENCY_MAX;
        uint16_t floor = l->supervision();
        uint32_t min_valid = ((1u + self->latency) * self->itvl_max * 125u * 2u) / 1000u + 1u;
        if (self->supervision_timeout < floor) self->supervision_timeout = floor;
        if (self->supervision_timeout < min_valid) self->supervision_timeout = min_valid;
        ESP_LOGI(TAG, "%s %s asks for interval %.1f-%.1f ms, latency %u, timeout %u ms; "
                      "using latency %u, timeout %u ms",
                 l->kind.c_str(), proto::hex(l->key).c_str(), peer->itvl_min * 1.25,
                 peer->itvl_max * 1.25, peer->latency, peer->supervision_timeout * 10,
                 self->latency, self->supervision_timeout * 10);
        l->asked = "";
        char asked[64];
        snprintf(asked, sizeof(asked), "%.1fms/%u/%ums", peer->itvl_max * 1.25, peer->latency,
                 peer->supervision_timeout * 10);
        l->asked = asked;
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            l->params = current_params(l->conn);
            ESP_LOGI(TAG, "%s %s connection parameters now %s (%lldms after connecting)",
                     l->kind.c_str(), proto::hex(l->key).c_str(), l->params.c_str(),
                     (long long)(l->connected_at ? now - l->connected_at : 0));
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        for (auto &s : l->subs) {
            if (s.val == event->notify_rx.attr_handle) {
                uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
                std::string data(len, '\0');
                os_mbuf_copydata(event->notify_rx.om, 0, len, &data[0]);
                auto &q = l->rx[s.name];
                q.push_back(std::move(data));
                if (q.size() > l->keep) q.pop_front();
                l->count++;
                ls.total_rx_++;
                break;
            }
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        l->mtu = event->mtu.value;
        break;
    default:
        break;
    }
    ls.unlock();
    return 0;
}

// -- Links -----------------------------------------------------------------------------

Links &Links::get()
{
    static Links instance;
    return instance;
}

Links::Links() : mutex_(xSemaphoreCreateRecursiveMutex()) {}

Link *Links::find(uint32_t id)
{
    for (auto &kv : links_)
        if (kv.second->id == id) return kv.second.get();
    return nullptr;
}

void Links::open(const std::string &key, const std::string &kind, uint8_t addr_type,
                 const std::string &addr)
{
    lock();
    auto it = links_.find(key);
    if (it == links_.end() || !it->second->enabled) {
        if (it != links_.end() && it->second->conn == BLE_HS_CONN_HANDLE_NONE &&
            it->second->state != "connecting")
            links_.erase(it);                // a closed link not yet reaped
        if (links_.find(key) == links_.end()) {
            auto l = std::make_unique<Link>();
            l->id = next_id_++;
            l->key = key;
            l->kind = kind;
            configure(*l);
            links_[key] = std::move(l);
        }
    }
    Link &l = *links_[key];
    l.enabled = true;
    if (l.conn == BLE_HS_CONN_HANDLE_NONE && l.state != "connecting") {
        l.addr = addr;
        l.addr_type = addr_type;
    }
    unlock();
}

void Links::close(const std::string &key)
{
    lock();
    auto it = links_.find(key);
    if (it != links_.end()) it->second->enabled = false;   // tick() disconnects, then reaps
    unlock();
}

void Links::close_all()
{
    lock();
    for (auto &kv : links_) kv.second->enabled = false;
    unlock();
}

bool Links::write(const std::string &key, const std::string &data)
{
    lock();
    bool ok = false;
    auto it = links_.find(key);
    if (it != links_.end()) {
        Link &l = *it->second;
        if (l.state == "ready" && l.write_h) {
            l.txq.push_back(data);
            ok = true;
        }
    }
    unlock();
    return ok;
}

bool Links::any_connected()
{
    lock();
    bool any = false;
    for (auto &kv : links_)
        if (kv.second->conn != BLE_HS_CONN_HANDLE_NONE) any = true;
    unlock();
    return any;
}

bool Links::connecting()
{
    lock();
    bool c = false;
    for (auto &kv : links_)
        if (kv.second->state == "connecting") c = true;
    unlock();
    return c;
}

void Links::tick()
{
    lock();
    int64_t now = Radio::now_ms();
    size_t conns = 0;
    bool busy = false;
    for (auto &kv : links_) {
        if (kv.second->conn != BLE_HS_CONN_HANDLE_NONE) conns++;
        if (kv.second->state == "connecting") busy = true;
    }
    for (auto it = links_.begin(); it != links_.end();) {
        Link &l = *it->second;
        if (l.state == "connecting" && now - l.started > kConnectTimeoutMs + 2000) {
            // NimBLE times the connect out itself; this is a backstop.
            ble_gap_conn_cancel();
            l.reset_conn();
            l.state = "idle";
            busy = false;
        }
        if (l.conn != BLE_HS_CONN_HANDLE_NONE && l.state != "ready" &&
            l.state != "disconnecting" && l.state.rfind("error", 0) != 0 &&
            now - l.connected_at > kSetupTimeoutMs)
            LinkOps::fail(l, "setup timed out");
        if (l.conn != BLE_HS_CONN_HANDLE_NONE && l.disconnect_sent &&
            now - l.disc_at > kDisconnectTimeoutMs) {
            l.reset_conn();                  // the disconnect event never came
            if (l.state.rfind("error", 0) != 0) l.state = "idle";
        }
        if (l.state.rfind("error", 0) == 0 && l.conn == BLE_HS_CONN_HANDLE_NONE &&
            now - l.err_at > l.backoff_ms)
            l.state = "idle";
        if (!l.enabled) {
            if (l.state == "connecting") {
                ble_gap_conn_cancel();
                l.reset_conn();
                l.state = "idle";
            } else if (l.conn != BLE_HS_CONN_HANDLE_NONE) {
                if (!l.disconnect_sent) {
                    l.disconnect_sent = true;
                    l.disc_at = now;
                    l.state = "disconnecting";
                    ble_gap_terminate(l.conn, BLE_ERR_REM_USER_CONN_TERM);
                }
            } else {
                removed_.push_back(l.key);
                it = links_.erase(it);
                continue;
            }
        }
        // Writes: one chunk per tick, one acked write outstanding at a time.
        if (l.state == "ready" && !l.txq.empty()) {
            if (l.tx_busy && now - l.tx_at > kWriteTimeoutMs) l.tx_busy = false;
            if (!l.tx_busy) {
                std::string &data = l.txq.front();
                size_t n = l.mtu > 23 ? static_cast<size_t>(l.mtu - 3) : 20;
                if (n > data.size()) n = data.size();
                int rc = l.write_rsp
                    ? ble_gattc_write_flat(l.conn, l.write_h, data.data(), n, LinkOps::write_cb, arg_of(l))
                    : ble_gattc_write_no_rsp_flat(l.conn, l.write_h, data.data(), n);
                if (rc == 0) {
                    if (l.write_rsp) {
                        l.tx_busy = true;
                        l.tx_at = now;
                    }
                    if (n < data.size()) data.erase(0, n);
                    else l.txq.pop_front();
                }                            // else: retry next tick
            }
        }
        ++it;
    }
    // One connect in flight at a time; scanning must be paused for it.
    if (!busy && conns < max_links) {
        for (auto &kv : links_) {
            Link &l = *kv.second;
            if (!l.enabled || l.state != "idle" || l.addr.size() != 6) continue;
            uint8_t own;
            if (ble_hs_id_infer_auto(0, &own) != 0) break;
            ble_addr_t peer;
            peer.type = l.addr_type;
            proto::addr_to_le(l.addr, peer.val);
            struct ble_gap_conn_params p;
            memset(&p, 0, sizeof(p));
            p.scan_itvl = 0x0010;
            p.scan_window = 0x0010;
            p.itvl_min = kItvlMin;
            p.itvl_max = kItvlMax;
            p.latency = 0;
            p.supervision_timeout = l.supervision();
            Radio::get().pause_scan();
            int rc = ble_gap_connect(own, &peer, kConnectTimeoutMs, &p, gap_event, arg_of(l));
            if (rc == 0) {
                l.state = "connecting";
                l.started = now;
            } else if (rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY) {
                // Stack busy: try again next tick.
            } else {
                l.state = "error: connect failed (" + std::to_string(rc) + ")";
                l.err_at = now;
            }
            break;
        }
    }
    unlock();
}

std::vector<LinkReport> Links::collect()
{
    std::vector<LinkReport> out;
    lock();
    for (auto &kv : links_) {
        Link &l = *kv.second;
        LinkReport r;
        r.key = l.key;
        r.kind = l.kind;
        r.state = l.state;
        r.desc = l.state == "ready" ? describe(l) : "";
        r.mtu = l.mtu;
        r.count = l.count;
        r.info = l.info;
        r.connects = l.connects;
        r.drops = l.drops;
        r.params = l.params;
        r.asked = l.asked;
        for (auto &q : l.rx)
            while (!q.second.empty()) {
                r.rx.emplace_back(q.first, std::move(q.second.front()));
                q.second.pop_front();
            }
        out.push_back(std::move(r));
    }
    unlock();
    return out;
}

std::vector<std::string> Links::collect_states()
{
    std::vector<std::string> out;
    lock();
    for (auto &kv : links_) out.push_back(kv.second->state);
    unlock();
    return out;
}

std::vector<std::string> Links::take_removed()
{
    lock();
    std::vector<std::string> out;
    out.swap(removed_);
    unlock();
    return out;
}

}  // namespace radio
