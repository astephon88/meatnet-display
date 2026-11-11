#include "central.h"

#include <algorithm>
#include <cstring>

#include "meater.h"
#include "util.h"

namespace linux_unit {

static constexpr int SETUP_TIMEOUT_MS = 20000, CONNECT_TIMEOUT_MS = 15000, ERROR_BACKOFF_MS = 10000, DROP_RETRY_MS = 1000,
                     ABORT_RETRY_MS = 500;
static constexpr size_t KEEP = 128;

// A link's state, rx queues and D-Bus objects. Fields other than the
// reported ones are touched on the loop thread only.
struct Central::Link {
    Bytes key;
    std::string kind, address, state = "idle", desc;
    bool enabled = true;
    int mtu = 23;
    uint32_t count = 0, connects = 0;
    std::map<std::string, uint32_t> drops;
    std::map<std::string, std::string> info;
    std::vector<std::pair<std::string, Bytes>> rx;
    std::string reported;                           // last state reported to collect_links()

    std::string dev_path;
    std::map<std::string, std::string> notify;      // char path -> sub name
    std::string write_path;
    bool write_response = false;
    std::vector<Bytes> txq;
    bool writing = false;
    guint timer = 0;
    int64_t connect_at = 0, up_since = 0;
    bool discovering = false;           // a discovery is in flight
};

static const struct {
    const char *name, *uuid;
    bool required;
} kProbeSubs[] = {{"status", protocol::PROBE_STATUS_CHAR_UUID, true}, {"uart", protocol::UART_TX_CHAR_UUID, false}},
  kNodeSubs[] = {{"uart", protocol::UART_TX_CHAR_UUID, true}},
  kMeaterSubs[] = {{"temp", meater::TEMP_CHAR_UUID, true}, {"battery", meater::BATTERY_CHAR_UUID, false}};

Central::Central(GDBusConnection *bus, GMainContext *loop, std::string adapter_path, LogFn log)
    : bus_(bus), loop_(loop), adapter_(std::move(adapter_path)), log_(std::move(log))
{
}

void Central::run(std::function<void()> fn)
{
    auto *f = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(
        loop_, G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
            (*static_cast<std::function<void()> *>(p))();
            return G_SOURCE_REMOVE;
        },
        f, [](gpointer p) { delete static_cast<std::function<void()> *>(p); });
}

// -- scanning ---------------------------------------------------------------------------------

void Central::start()
{
    if (GVariant *a = gdbus::get_property(bus_, adapter_, "org.bluez.Adapter1", "Address")) {
        mac = gdbus::addr_bytes(g_variant_get_string(a, nullptr));
        g_variant_unref(a);
    }
    gdbus::set_property(bus_, adapter_, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE));
    g_dbus_connection_signal_subscribe(
        bus_, gdbus::BLUEZ, nullptr, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const gchar *, const gchar *path, const gchar *iface, const gchar *signal,
           GVariant *params, gpointer user) { static_cast<Central *>(user)->on_signal(path, iface, signal, params); },
        this, nullptr);
    load_objects();
    // BlueZ keeps a connection when the program that opened it exits, and a
    // connected device stops advertising: we would never hear it again.
    // Drop such leftovers to MeatNet devices and MEATERs (phones connected to
    // us as a peripheral carry neither).
    for (auto &kv : devs_) {
        const Dev &d = kv.second;
        bool ours = !d.mfr.empty() || d.name.compare(0, 6, "MEATER") == 0;
        for (const char *u : meater::SERVICE_UUIDS) ours = ours || d.uuids.count(gdbus::uuid_lower(u));
        if (!d.connected || !ours) continue;
        log_("[links] dropping a leftover connection to " + d.address);
        gdbus::call(bus_, kv.first.c_str(), "org.bluez.Device1", "Disconnect", nullptr, 5000);
    }
    GVariantBuilder f;
    g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("le"));
    // As bleak: with duplicates filtered the controller reports far less, and
    // BlueZ still signals RSSI and data changes.
    g_variant_builder_add(&f, "{sv}", "DuplicateData", g_variant_new_boolean(FALSE));
    GVariant *r = g_dbus_connection_call_sync(bus_, gdbus::BLUEZ, adapter_.c_str(), "org.bluez.Adapter1",
                                              "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (r) g_variant_unref(r);
    set_standby(false);
}

void Central::load_objects()
{
    GVariant *r = g_dbus_connection_call_sync(bus_, gdbus::BLUEZ, "/", "org.freedesktop.DBus.ObjectManager",
                                              "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (!r) return;
    GVariant *objs = g_variant_get_child_value(r, 0);
    GVariantIter it;
    g_variant_iter_init(&it, objs);
    const char *path;
    GVariant *ifaces;
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        if (dev && g_str_has_prefix(path, (adapter_ + "/").c_str())) {
            update_device(path, dev, false);
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_unref(objs);
    g_variant_unref(r);
}

void Central::on_signal(const char *path, const char *iface, const char *signal, GVariant *params)
{
    if (!strcmp(iface, "org.freedesktop.DBus.ObjectManager") && !strcmp(signal, "InterfacesAdded")) {
        const char *obj;
        GVariant *ifaces;
        g_variant_get(params, "(&o@a{sa{sv}})", &obj, &ifaces);
        if (GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"))) {
            if (g_str_has_prefix(obj, (adapter_ + "/").c_str())) update_device(obj, dev, true);
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    } else if (!strcmp(iface, "org.freedesktop.DBus.ObjectManager") && !strcmp(signal, "InterfacesRemoved")) {
        const char *obj;
        GVariant *names;
        g_variant_get(params, "(&o@as)", &obj, &names);
        devs_.erase(obj);
        g_variant_unref(names);
    } else if (!strcmp(iface, "org.freedesktop.DBus.Properties") && !strcmp(signal, "PropertiesChanged")) {
        const char *which;
        GVariant *changed, *gone;
        g_variant_get(params, "(&s@a{sv}@as)", &which, &changed, &gone);
        gboolean on;
        if (!strcmp(which, "org.bluez.Adapter1") && adapter_ == path &&
            ((g_variant_lookup(changed, "Powered", "b", &on) && on) ||
             (g_variant_lookup(changed, "Discovering", "b", &on) && !on))) {
            // The adapter was powered off and on (or discovery stopped under
            // us): scanning doesn't resume by itself.
            bool standby;
            {
                std::lock_guard<std::mutex> g(mu_);
                standby = standby_;
            }
            if (!standby) discovery(true);
        } else if (!strcmp(which, "org.bluez.Device1") && devs_.count(path)) {
            update_device(path, changed, true);
        } else if (!strcmp(which, "org.bluez.GattCharacteristic1")) {
            GVariant *v = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE_BYTESTRING);
            if (v) {
                Bytes data = gdbus::bytes(v);
                g_variant_unref(v);
                std::lock_guard<std::mutex> g(mu_);
                for (auto &kv : links_) {
                    Link &l = *kv.second;
                    auto sub = l.notify.find(path);
                    if (sub == l.notify.end()) continue;
                    l.rx.push_back({sub->second, data});
                    if (l.rx.size() > KEEP * 2) l.rx.erase(l.rx.begin());
                    l.count++;
                }
            }
        }
        g_variant_unref(changed);
        g_variant_unref(gone);
    }
}

// A device's properties changed (or it appeared): an advertisement when
// `advert`, so its sighting time is refreshed.
void Central::update_device(const std::string &path, GVariant *props, bool advert)
{
    Dev &d = devs_[path];
    GVariantIter it;
    g_variant_iter_init(&it, props);
    const char *k;
    GVariant *v;
    bool seen = false;
    bool was_connected = d.connected;
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
        if (!strcmp(k, "Address")) d.address = g_variant_get_string(v, nullptr);
        else if (!strcmp(k, "AddressType")) d.random = !strcmp(g_variant_get_string(v, nullptr), "random");
        else if (!strcmp(k, "Name")) d.name = g_variant_get_string(v, nullptr);
        else if (!strcmp(k, "RSSI")) {
            d.rssi = g_variant_get_int16(v);
            seen = true;
        } else if (!strcmp(k, "Connected")) d.connected = g_variant_get_boolean(v);
        else if (!strcmp(k, "ServicesResolved")) d.resolved = g_variant_get_boolean(v);
        else if (!strcmp(k, "UUIDs")) {
            d.uuids.clear();
            GVariantIter u;
            g_variant_iter_init(&u, v);
            const char *s;
            while (g_variant_iter_next(&u, "&s", &s)) d.uuids.insert(gdbus::uuid_lower(s));
        } else if (!strcmp(k, "ManufacturerData")) {
            GVariantIter m;
            g_variant_iter_init(&m, v);
            guint16 id;
            GVariant *data;
            while (g_variant_iter_next(&m, "{qv}", &id, &data)) {
                if (id == protocol::VENDOR_ID) d.mfr = gdbus::bytes(data);
                g_variant_unref(data);
            }
            seen = true;
        }
        g_variant_unref(v);
    }

    // Links: a connection that went away, or services now resolved.
    std::vector<std::shared_ptr<Link>> touched;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto &kv : links_)
            if (kv.second->dev_path == path) touched.push_back(kv.second);
    }
    for (auto &l : touched) {
        if (was_connected && !d.connected && (l->state == "ready" || l->state == "discovering")) {
            std::lock_guard<std::mutex> g(mu_);
            l->drops["disconnected"]++;
        }
        if (!d.connected && l->state != "idle" && l->state != "connecting" && l->enabled)
            link_fail(*l, "", DROP_RETRY_MS);
        else if (l->state == "discovering" && d.resolved && l->notify.empty())
            link_discover(*l);
    }

    if (!advert || !seen || d.address.empty()) return;
    Bytes addr = gdbus::addr_bytes(d.address);
    int64_t now = core::ticks_ms();
    std::lock_guard<std::mutex> g(mu_);
    if (addr == ignore_mac_) return;
    core::Observation o;
    o.ticks = now;
    o.addr_type = d.random ? 1 : 0;
    o.addr = addr;
    o.rssi = d.rssi;
    o.name = d.name;
    const Bytes &body = d.mfr;
    if (!body.empty()) {
        int ptype = static_cast<uint8_t>(body[0]);
        o.body = body;
        if (ptype == protocol::PRODUCT_TYPE_PROBE && body.size() >= 20) {
            tables_.direct[body.substr(1, 4)] = o;
        } else if (ptype == protocol::PRODUCT_TYPE_NODE && body.size() >= 20) {
            if (body.substr(1, 4) != Bytes(4, '\0')) tables_.repeated[body.substr(1, 4)] = o;
        } else if (ptype >= protocol::PRODUCT_TYPE_GAUGE && body.size() >= 11) {
            if (body.substr(1, 10) != ignore_serial_) tables_.nodes[body.substr(1, 10)] = o;
        }
        return;
    }
    bool meater = d.name.compare(0, 6, "MEATER") == 0;
    for (const char *u : meater::SERVICE_UUIDS) meater = meater || d.uuids.count(gdbus::uuid_lower(u));
    if (meater) {
        o.body.clear();
        tables_.meaters[addr] = o;
    }
}

void Central::shutdown()
{
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto &kv : links_)
            if (!kv.second->dev_path.empty() && kv.second->state != "idle") paths.push_back(kv.second->dev_path);
        links_.clear();
    }
    for (auto &p : paths) {
        GVariant *r = g_dbus_connection_call_sync(bus_, gdbus::BLUEZ, p.c_str(), "org.bluez.Device1", "Disconnect",
                                                  nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
        if (r) g_variant_unref(r);
    }
}

split::FieldSource::Tables Central::tables()
{
    std::lock_guard<std::mutex> g(mu_);
    return tables_;
}

void Central::prune(int64_t now, int64_t max_age_ms)
{
    std::lock_guard<std::mutex> g(mu_);
    for (auto *t : {&tables_.direct, &tables_.repeated, &tables_.nodes, &tables_.meaters})
        for (auto it = t->begin(); it != t->end();)
            it = now - it->second.ticks > max_age_ms ? t->erase(it) : std::next(it);
}

void Central::set_ignore(const Bytes &mac_, const Bytes &serial10)
{
    std::lock_guard<std::mutex> g(mu_);
    ignore_mac_ = mac_;
    ignore_serial_ = serial10;
}

void Central::set_standby(bool on)
{
    run([this, on] {
        {
            std::lock_guard<std::mutex> g(mu_);
            standby_ = on;
        }
        discovery(!on);
        if (on) close_all_links();
    });
}

void Central::discovery(bool on)
{
    gdbus::call(bus_, adapter_.c_str(), "org.bluez.Adapter1", on ? "StartDiscovery" : "StopDiscovery", nullptr, 5000);
}

// -- links ------------------------------------------------------------------------------------

std::vector<split::LinkReport> Central::collect_links()
{
    std::vector<split::LinkReport> out;
    std::lock_guard<std::mutex> g(mu_);
    for (auto &kv : links_) {
        Link &l = *kv.second;
        split::LinkReport r;
        r.key = l.key;
        r.kind = l.kind;
        r.state = l.state;
        r.desc = l.desc;
        r.mtu = l.mtu;
        r.count = l.count;
        r.info = l.info;
        r.rx = std::move(l.rx);
        l.rx.clear();
        r.has_stats = true;
        r.connects = l.connects;
        r.drops = l.drops;
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<core::LinkSummary> Central::link_summaries()
{
    std::vector<core::LinkSummary> out;
    std::lock_guard<std::mutex> g(mu_);
    for (auto &kv : links_) {
        const Link &l = *kv.second;
        std::string name = l.kind == "probe" ? protocol::probe_serial_str(l.key)
                         : l.kind == "meater" ? l.address : protocol::serial10_str(l.key);
        out.push_back({l.kind, name, l.state, l.count, l.mtu, core::link_stats(l.connects, l.drops, "", "")});
    }
    return out;
}

std::vector<Bytes> Central::take_removed_links()
{
    std::lock_guard<std::mutex> g(mu_);
    return std::move(removed_);
}

void Central::open_link(const Bytes &key, const std::string &kind, int, const Bytes &addr)
{
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = links_.find(key);
        if (it != links_.end() && it->second->enabled) return;
        auto l = std::make_shared<Link>();
        l->key = key;
        l->kind = kind;
        l->address = gdbus::addr_text(addr);
        links_[key] = l;
    }
    run([this, key] {
        std::shared_ptr<Link> l;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = links_.find(key);
            if (it == links_.end()) return;
            l = it->second;
        }
        schedule(*l, 0);
    });
}

void Central::close_link(const Bytes &key)
{
    run([this, key] { remove_link(key); });
}

void Central::close_all_links()
{
    run([this] {
        std::vector<Bytes> keys;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto &kv : links_) keys.push_back(kv.first);
        }
        for (auto &k : keys) remove_link(k);
    });
}

void Central::remove_link(const Bytes &key)
{
    std::shared_ptr<Link> l;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = links_.find(key);
        if (it == links_.end()) return;
        l = it->second;
        links_.erase(it);
        removed_.push_back(key);
        if (l->state == "connecting") connecting_ = false;
    }
    l->enabled = false;
    if (l->timer) g_source_remove(l->timer);
    l->timer = 0;
    if (!l->dev_path.empty() && l->state != "idle")
        gdbus::call(bus_, l->dev_path.c_str(), "org.bluez.Device1", "Disconnect", nullptr, 5000);
}

void Central::write_link(const Bytes &key, const Bytes &data)
{
    run([this, key, data] {
        std::shared_ptr<Link> l;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = links_.find(key);
            if (it == links_.end() || it->second->state != "ready" || it->second->write_path.empty()) return;
            l = it->second;
        }
        size_t n = static_cast<size_t>(std::max(20, l->mtu - 3));
        for (size_t i = 0; i < data.size(); i += n) l->txq.push_back(data.substr(i, n));
        link_write_next(*l);
    });
}

void Central::schedule(Link &l, int ms)
{
    if (l.timer) g_source_remove(l.timer);
    struct Ctx {
        Central *self;
        Bytes key;
    };
    auto *ctx = new Ctx{this, l.key};
    l.timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        [](gpointer p) -> gboolean {
            auto *c = static_cast<Ctx *>(p);
            std::shared_ptr<Link> link;
            {
                std::lock_guard<std::mutex> g(c->self->mu_);
                auto it = c->self->links_.find(c->key);
                if (it != c->self->links_.end()) link = it->second;
            }
            if (link) {
                link->timer = 0;
                c->self->link_step(*link);
            }
            return G_SOURCE_REMOVE;
        },
        ctx, [](gpointer p) { delete static_cast<Ctx *>(p); });
}

static void set_state(std::mutex &mu, std::string &field, const std::string &v)
{
    std::lock_guard<std::mutex> g(mu);
    field = v;
}

void Central::link_step(Link &l)
{
    if (!l.enabled || l.state != "idle") return;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (standby_) return;
        if (connecting_) {                  // one connection attempt at a time
            schedule(l, 500);
            return;
        }
        connecting_ = true;
    }
    link_connect(l);
}

void Central::link_connect(Link &l)
{
    l.dev_path.clear();
    for (auto &kv : devs_)
        if (kv.second.address == l.address) l.dev_path = kv.first;
    if (l.dev_path.empty()) {
        {
            std::lock_guard<std::mutex> g(mu_);
            connecting_ = false;
        }
        link_fail(l, "device not heard", ERROR_BACKOFF_MS);
        return;
    }
    set_state(mu_, l.state, "connecting");
    l.connect_at = core::ticks_ms();
    // Still connected from an attempt we gave up on (or left over): BlueZ
    // would hand back that same connection, so finish dropping it first.
    if (devs_[l.dev_path].connected) {
        {
            std::lock_guard<std::mutex> g(mu_);
            connecting_ = false;
            l.state = "idle";
        }
        gdbus::call(bus_, l.dev_path.c_str(), "org.bluez.Device1", "Disconnect", nullptr, 5000);
        schedule(l, ERROR_BACKOFF_MS);
        return;
    }
    struct Ctx {
        Central *self;
        Bytes key;
    };
    g_dbus_connection_call(
        bus_, gdbus::BLUEZ, l.dev_path.c_str(), "org.bluez.Device1", "Connect", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NONE, CONNECT_TIMEOUT_MS, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer p) {
            auto *c = static_cast<Ctx *>(p);
            Central *self = c->self;
            GError *err = nullptr;
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            std::shared_ptr<Link> l;
            {
                std::lock_guard<std::mutex> g(self->mu_);
                self->connecting_ = false;
                auto it = self->links_.find(c->key);
                if (it != self->links_.end()) l = it->second;
            }
            if (r) g_variant_unref(r);
            if (l && l->state == "connecting") {
                if (err && strstr(err->message, "le-connection-abort-by-local")) {
                    // Often RF interference; retrying usually works (as bleak does).
                    {
                        std::lock_guard<std::mutex> g(self->mu_);
                        l->drops["aborted"]++;
                        l->state = "idle";
                    }
                    self->schedule(*l, ABORT_RETRY_MS);
                } else if (err) {
                    self->link_fail(*l, err->message, ERROR_BACKOFF_MS);
                } else {
                    {
                        std::lock_guard<std::mutex> g(self->mu_);
                        l->connects++;
                        l->state = "discovering";
                        l->up_since = core::ticks_ms();
                    }
                    self->watch_setup(*l);
                    if (self->devs_[l->dev_path].resolved) self->link_discover(*l);
                }
            }
            if (err) g_error_free(err);
            delete c;
        },
        new Ctx{this, l.key});
}

// Services are resolved: find the characteristics under the device, start
// the notifications, read the probe's Device Information. All asynchronous:
// BlueZ calls back into our GATT server and advertisements meanwhile.
struct Central::Discovery {
    Central *self;
    Bytes key;
    uint32_t connects;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> chars;   // uuid -> (path, flags)
    int mtu = 23;
    std::map<std::string, std::string> notify, info;
    std::string desc, write_path;
    bool write_response = false;
    size_t dis_next = 0;
};

void Central::link_discover(Link &l)
{
    if (l.discovering) return;
    l.discovering = true;
    auto *d = new Discovery{this, l.key, l.connects, {}, 23, {}, {}, "", "", false, 0};
    g_dbus_connection_call(
        bus_, gdbus::BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer p) {
            auto *d = static_cast<Discovery *>(p);
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            d->self->discover_objects(d, r);
            if (r) g_variant_unref(r);
        },
        d);
}

// The live link a discovery step belongs to, or null if it has moved on
// (closed, dropped and reconnected).
std::shared_ptr<Central::Link> Central::discovery_link(Discovery *d)
{
    std::lock_guard<std::mutex> g(mu_);
    auto it = links_.find(d->key);
    if (it == links_.end() || it->second->connects != d->connects || it->second->state != "discovering")
        return nullptr;
    return it->second;
}

void Central::discover_objects(Discovery *d, GVariant *r)
{
    std::shared_ptr<Link> lp = discovery_link(d);
    if (!lp) {
        delete d;
        return;
    }
    Link &l = *lp;
    l.discovering = false;
    if (!r) {
        delete d;
        link_fail(l, "can't list services", ERROR_BACKOFF_MS);
        return;
    }
    GVariant *objs = g_variant_get_child_value(r, 0);
    GVariantIter it;
    g_variant_iter_init(&it, objs);
    const char *path;
    GVariant *ifaces;
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (g_str_has_prefix(path, (l.dev_path + "/").c_str())) {
            if (GVariant *c = g_variant_lookup_value(ifaces, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE("a{sv}"))) {
                const char *uuid = nullptr;
                g_variant_lookup(c, "UUID", "&s", &uuid);
                std::vector<std::string> flags;
                if (GVariant *f = g_variant_lookup_value(c, "Flags", G_VARIANT_TYPE("as"))) {
                    GVariantIter fi;
                    g_variant_iter_init(&fi, f);
                    const char *s;
                    while (g_variant_iter_next(&fi, "&s", &s)) flags.push_back(s);
                    g_variant_unref(f);
                }
                guint16 m = 0;
                if (g_variant_lookup(c, "MTU", "q", &m) && m > d->mtu) d->mtu = m;
                if (uuid) d->chars[gdbus::uuid_lower(uuid)] = {path, flags};
                g_variant_unref(c);
            }
        }
        g_variant_unref(ifaces);
    }
    g_variant_unref(objs);

    struct Sub {
        const char *name, *uuid;
        bool required;
    };
    std::vector<Sub> subs;
    if (l.kind == "probe") for (auto &s : kProbeSubs) subs.push_back({s.name, s.uuid, s.required});
    else if (l.kind == "node") for (auto &s : kNodeSubs) subs.push_back({s.name, s.uuid, s.required});
    else for (auto &s : kMeaterSubs) subs.push_back({s.name, s.uuid, s.required});
    for (auto &s : subs) {
        auto c = d->chars.find(gdbus::uuid_lower(s.uuid));
        if (c == d->chars.end()) {
            if (s.required) {
                std::string uuid = s.uuid;
                delete d;
                link_fail(l, "characteristic " + uuid.substr(0, 8) + " not found", ERROR_BACKOFF_MS);
                return;
            }
            continue;
        }
        gdbus::call(bus_, c->second.first.c_str(), "org.bluez.GattCharacteristic1", "StartNotify", nullptr, 5000);
        d->notify[c->second.first] = s.name;
        d->desc += (d->desc.empty() ? "" : ", ") + std::string(s.name) + " notify on";
    }
    if (l.kind != "meater") {
        auto w = d->chars.find(gdbus::uuid_lower(protocol::UART_RX_CHAR_UUID));
        if (w != d->chars.end()) {
            d->write_path = w->second.first;
            auto &fl = w->second.second;
            d->write_response = std::find(fl.begin(), fl.end(), "write-without-response") == fl.end();
            d->desc += std::string(", write (") + (d->write_response ? "with response" : "no response") + ")";
        }
    }
    discover_next_dis(d);
}

// The probe's Device Information strings, one read at a time; then ready.
void Central::discover_next_dis(Discovery *d)
{
    std::shared_ptr<Link> lp = discovery_link(d);
    if (!lp) {
        delete d;
        return;
    }
    Link &l = *lp;
    const size_t n = sizeof(protocol::PROBE_DIS_READS) / sizeof(protocol::PROBE_DIS_READS[0]);
    while (l.kind == "probe" && d->dis_next < n) {
        auto c = d->chars.find(gdbus::uuid_lower(protocol::PROBE_DIS_READS[d->dis_next].characteristic));
        if (c == d->chars.end()) {
            d->dis_next++;
            continue;
        }
        GVariantBuilder o;
        g_variant_builder_init(&o, G_VARIANT_TYPE("a{sv}"));
        g_dbus_connection_call(
            bus_, gdbus::BLUEZ, c->second.first.c_str(), "org.bluez.GattCharacteristic1", "ReadValue",
            g_variant_new("(a{sv})", &o), G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
            [](GObject *src, GAsyncResult *res, gpointer p) {
                auto *d = static_cast<Discovery *>(p);
                GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
                if (v) {
                    GVariant *ay = g_variant_get_child_value(v, 0);
                    std::string s = gdbus::bytes(ay);
                    d->info[protocol::PROBE_DIS_READS[d->dis_next].name] = s.substr(0, s.find('\0'));
                    g_variant_unref(ay);
                    g_variant_unref(v);
                }
                d->dis_next++;
                d->self->discover_next_dis(d);
            },
            d);
        return;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        l.notify = std::move(d->notify);
        l.write_path = d->write_path;
        l.write_response = d->write_response;
        l.info = std::move(d->info);
        l.mtu = d->mtu;
        l.desc = d->desc;
        l.state = "ready";
    }
    log_("[links] " + l.kind + " " + l.address + " ready: setup took " + std::to_string(core::ticks_ms() - l.connect_at) +
         " ms (connect " + std::to_string(l.up_since - l.connect_at) + " ms)");
    delete d;
}

// A link still discovering after SETUP_TIMEOUT_MS gives up and starts over.
void Central::watch_setup(Link &l)
{
    struct Ctx {
        Central *self;
        Bytes key;
        uint32_t connects;
    };
    g_timeout_add_full(
        G_PRIORITY_DEFAULT, SETUP_TIMEOUT_MS,
        [](gpointer p) -> gboolean {
            auto *c = static_cast<Ctx *>(p);
            std::shared_ptr<Link> link;
            {
                std::lock_guard<std::mutex> g(c->self->mu_);
                auto it = c->self->links_.find(c->key);
                if (it != c->self->links_.end()) link = it->second;
            }
            if (link && link->state == "discovering" && link->connects == c->connects)
                c->self->link_fail(*link, "setup timed out", DROP_RETRY_MS);
            return G_SOURCE_REMOVE;
        },
        new Ctx{this, l.key, l.connects}, [](gpointer p) { delete static_cast<Ctx *>(p); });
}

void Central::link_fail(Link &l, const std::string &why, int retry_ms)
{
    if (!why.empty()) log_("[links] " + l.kind + " " + l.address + ": " + why);
    {
        std::lock_guard<std::mutex> g(mu_);
        l.state = why.empty() ? "idle" : "error: " + why;
        l.notify.clear();
        l.write_path.clear();
        l.txq.clear();
        l.writing = false;
        l.discovering = false;
        if (!why.empty()) l.drops[why.compare(0, 9, "Timeout w") == 0 ? "timeout" : "error"]++;
    }
    if (!why.empty() && !l.dev_path.empty())
        gdbus::call(bus_, l.dev_path.c_str(), "org.bluez.Device1", "Disconnect", nullptr, 5000);
    if (!l.enabled) return;
    struct Ctx {
        Central *self;
        Bytes key;
    };
    // Back to idle after the back-off, then reconnect.
    auto *ctx = new Ctx{this, l.key};
    if (l.timer) g_source_remove(l.timer);
    l.timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, retry_ms,
        [](gpointer p) -> gboolean {
            auto *c = static_cast<Ctx *>(p);
            std::shared_ptr<Link> link;
            {
                std::lock_guard<std::mutex> g(c->self->mu_);
                auto it = c->self->links_.find(c->key);
                if (it != c->self->links_.end()) link = it->second;
                if (link) link->state = "idle";
            }
            if (link) {
                link->timer = 0;
                c->self->link_step(*link);
            }
            return G_SOURCE_REMOVE;
        },
        ctx, [](gpointer p) { delete static_cast<Ctx *>(p); });
}

void Central::link_write_next(Link &l)
{
    if (l.writing || l.txq.empty() || l.write_path.empty()) return;
    l.writing = true;
    Bytes chunk = l.txq.front();
    l.txq.erase(l.txq.begin());
    GVariantBuilder o;
    g_variant_builder_init(&o, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&o, "{sv}", "type", g_variant_new_string(l.write_response ? "request" : "command"));
    struct Ctx {
        Central *self;
        Bytes key;
    };
    g_dbus_connection_call(
        bus_, gdbus::BLUEZ, l.write_path.c_str(), "org.bluez.GattCharacteristic1", "WriteValue",
        g_variant_new("(@aya{sv})", gdbus::bytes(chunk), &o), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer p) {
            auto *c = static_cast<Ctx *>(p);
            GError *err = nullptr;
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (r) g_variant_unref(r);
            if (err) g_error_free(err);
            std::shared_ptr<Link> link;
            {
                std::lock_guard<std::mutex> g(c->self->mu_);
                auto it = c->self->links_.find(c->key);
                if (it != c->self->links_.end()) link = it->second;
            }
            if (link) {
                link->writing = false;
                c->self->link_write_next(*link);
            }
            delete c;
        },
        new Ctx{this, l.key});
}

}  // namespace linux_unit
