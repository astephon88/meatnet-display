#include "peripheral.h"

#include <cstring>


namespace linux_unit {

static constexpr const char *APP_PATH = "/org/meatnet";
static constexpr size_t TX_PER_SERVICE = 12, TXQ_MAX = 240, MAX_INSTANCES = 10;

static const char *kChrXml =
    "<interface name='org.bluez.GattCharacteristic1'>"
    "<method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method>"
    "<method name='WriteValue'><arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/></method>"
    "<method name='StartNotify'/><method name='StopNotify'/>"
    "<property name='UUID' type='s' access='read'/><property name='Service' type='o' access='read'/>"
    "<property name='Flags' type='as' access='read'/><property name='Value' type='ay' access='read'/>"
    "<property name='Notifying' type='b' access='read'/></interface>";
static const char *kSvcXml =
    "<interface name='org.bluez.GattService1'>"
    "<property name='UUID' type='s' access='read'/><property name='Primary' type='b' access='read'/></interface>";
static const char *kRootXml =
    "<interface name='org.freedesktop.DBus.ObjectManager'>"
    "<method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method></interface>";
static const char *kAdXml =
    "<interface name='org.bluez.LEAdvertisement1'><method name='Release'/>"
    "<property name='Type' type='s' access='read'/><property name='ManufacturerData' type='a{qv}' access='read'/>"
    "<property name='ServiceUUIDs' type='as' access='read'/>"
    "<property name='Discoverable' type='b' access='read'/></interface>";

static GVariant *strv(const std::vector<std::string> &v)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (auto &s : v) g_variant_builder_add(&b, "s", s.c_str());
    return g_variant_builder_end(&b);
}

Peripheral::Peripheral(GDBusConnection *bus, std::string adapter_path, std::string serial, std::string model,
                       std::string firmware, LogFn log)
    : bus_(bus), adapter_(std::move(adapter_path)), serial_(std::move(serial)), model_(std::move(model)),
      firmware_(std::move(firmware)), log_(std::move(log))
{
}

Peripheral::~Peripheral()
{
    // Let a registration in flight finish first: bluetoothd 5.64 aborts when
    // an advertisement registration fails after its client has gone.
    ad_ops_.clear();
    for (int i = 0; adv_busy_ && i < 300; i++) {
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(10000);
    }
    for (auto &kv : ads_)
        g_dbus_connection_call_sync(bus_, gdbus::BLUEZ, adapter_.c_str(), "org.bluez.LEAdvertisingManager1",
                                    "UnregisterAdvertisement", g_variant_new("(o)", kv.second->path.c_str()),
                                    nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
}

void Peripheral::start()
{
    if (GVariant *a = gdbus::get_property(bus_, adapter_, "org.bluez.Adapter1", "Address")) {
        mac = gdbus::addr_bytes(g_variant_get_string(a, nullptr));
        g_variant_unref(a);
    }
    gdbus::set_property(bus_, adapter_, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE));

    auto svc = [this](const std::string &path, const char *uuid) {
        auto s = std::make_unique<Svc>();
        s->path = path;
        s->uuid = gdbus::uuid_lower(uuid);
        Svc *p = s.get();
        p->h.get = [p](const char *prop) -> GVariant * {
            if (!strcmp(prop, "UUID")) return g_variant_new_string(p->uuid.c_str());
            if (!strcmp(prop, "Primary")) return g_variant_new_boolean(TRUE);
            return nullptr;
        };
        svcs_.push_back(std::move(s));
        return p;
    };
    auto chr = [this](const Svc *s, int i, const char *uuid, std::vector<std::string> flags,
                      std::function<std::string()> value) {
        auto c = std::make_unique<Chr>();
        c->path = s->path + "/char" + std::to_string(i);
        c->uuid = gdbus::uuid_lower(uuid);
        c->service = s->path;
        c->flags = std::move(flags);
        c->value = std::move(value);
        Chr *p = c.get();
        p->h.get = [p](const char *prop) -> GVariant * {
            if (!strcmp(prop, "UUID")) return g_variant_new_string(p->uuid.c_str());
            if (!strcmp(prop, "Service")) return g_variant_new_object_path(p->service.c_str());
            if (!strcmp(prop, "Flags")) return strv(p->flags);
            if (!strcmp(prop, "Value")) return gdbus::bytes(p->value ? p->value() : std::string());
            if (!strcmp(prop, "Notifying")) return g_variant_new_boolean(p->notifying);
            return nullptr;
        };
        chrs_.push_back(std::move(c));
        return p;
    };

    Svc *uart = svc(std::string(APP_PATH) + "/service0", protocol::UART_SERVICE_UUID);
    tx_ = chr(uart, 0, protocol::UART_TX_CHAR_UUID, {"read", "notify"}, nullptr);
    Chr *rx = chr(uart, 1, protocol::UART_RX_CHAR_UUID, {"write", "write-without-response"}, nullptr);
    Svc *dis = svc(std::string(APP_PATH) + "/service1", "0000180a-0000-1000-8000-00805f9b34fb");
    auto dis_uuid = [](const char *s) { return std::string("0000") + s + "-0000-1000-8000-00805f9b34fb"; };
    std::string u[5] = {dis_uuid("2a25"), dis_uuid("2a26"), dis_uuid("2a27"), dis_uuid("2a24"), dis_uuid("2a29")};
    chr(dis, 0, u[0].c_str(), {"read"}, [this] { return serial_; });
    chr(dis, 1, u[1].c_str(), {"read"}, [this] { return firmware_; });
    chr(dis, 2, u[2].c_str(), {"read"}, [] { return std::string("1.0"); });
    chr(dis, 3, u[3].c_str(), {"read"}, [this] { return model_; });
    chr(dis, 4, u[4].c_str(), {"read"}, [] { return std::string("Combustion Inc."); });

    tx_->h.call = [this](const char *m, GVariant *, GDBusMethodInvocation *inv) {
        if (!strcmp(m, "StartNotify")) on_notify(true);
        else if (!strcmp(m, "StopNotify")) on_notify(false);
        if (!strcmp(m, "ReadValue")) g_dbus_method_invocation_return_value(inv, g_variant_new("(@ay)", gdbus::bytes("")));
        else g_dbus_method_invocation_return_value(inv, nullptr);
    };
    rx->h.call = [this](const char *m, GVariant *params, GDBusMethodInvocation *inv) {
        if (strcmp(m, "WriteValue") != 0) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported", "not supported");
            return;
        }
        GVariant *value = g_variant_get_child_value(params, 0), *options = g_variant_get_child_value(params, 1);
        on_write(gdbus::bytes(value), options);
        g_variant_unref(value);
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(inv, nullptr);
    };
    for (auto &c : chrs_) {
        if (c->h.call) continue;
        Chr *p = c.get();
        p->h.call = [p](const char *m, GVariant *params, GDBusMethodInvocation *inv) {
            if (strcmp(m, "ReadValue") != 0) {
                g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported", "not supported");
                return;
            }
            std::string v = p->value();
            guint16 off = 0;
            GVariant *options = g_variant_get_child_value(params, 0);
            g_variant_lookup(options, "offset", "q", &off);
            g_variant_unref(options);
            v = off < v.size() ? v.substr(off) : std::string();
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@ay)", gdbus::bytes(v)));
        };
    }

    root_.call = [this](const char *, GVariant *, GDBusMethodInvocation *inv) {
        GVariant *objs = managed_objects();
        g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&objs, 1));
    };
    regs_.push_back(gdbus::export_object(bus_, APP_PATH, kRootXml, &root_));
    for (auto &s : svcs_) regs_.push_back(gdbus::export_object(bus_, s->path, kSvcXml, &s->h));
    for (auto &c : chrs_) regs_.push_back(gdbus::export_object(bus_, c->path, kChrXml, &c->h));

    g_dbus_connection_call(
        bus_, gdbus::BLUEZ, adapter_.c_str(), "org.bluez.GattManager1", "RegisterApplication",
        g_variant_new("(oa{sv})", APP_PATH, nullptr), nullptr, G_DBUS_CALL_FLAGS_NONE, 10000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer user) {
            auto *self = static_cast<Peripheral *>(user);
            GError *err = nullptr;
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (r) {
                g_variant_unref(r);
                self->log_("[host] GATT server registered on " + self->adapter_ + " (" +
                           gdbus::addr_text(self->mac) + ")");
                self->started_ = true;
                self->adv_dirty_ = true;
            } else {
                self->log_(std::string("[host] GATT registration failed: ") + err->message);
                g_error_free(err);
            }
        },
        this);
}

GVariant *Peripheral::managed_objects()
{
    GVariantBuilder objs;
    g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    auto add = [&objs](const std::string &path, const char *iface, gdbus::Handlers &h,
                       std::initializer_list<const char *> props) {
        GVariantBuilder ifaces, pv;
        g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_init(&pv, G_VARIANT_TYPE("a{sv}"));
        for (const char *p : props)
            if (GVariant *v = h.get(p)) g_variant_builder_add(&pv, "{sv}", p, v);
        g_variant_builder_add(&ifaces, "{sa{sv}}", iface, &pv);
        g_variant_builder_add(&objs, "{oa{sa{sv}}}", path.c_str(), &ifaces);
    };
    for (auto &s : svcs_) add(s->path, "org.bluez.GattService1", s->h, {"UUID", "Primary"});
    for (auto &c : chrs_)
        add(c->path, "org.bluez.GattCharacteristic1", c->h, {"UUID", "Service", "Flags", "Value", "Notifying"});
    return g_variant_builder_end(&objs);
}

void Peripheral::on_write(const std::string &data, GVariant *options)
{
    const char *dev = nullptr;
    std::string device = g_variant_lookup(options, "device", "&o", &dev) ? dev : "?";
    auto it = device_ids_.find(device);
    bool fresh = it == device_ids_.end();
    int id = !fresh ? it->second : (device_ids_[device] = static_cast<int>(device_ids_.size()) + 1);
    guint16 mtu = 0;
    if (fresh && g_variant_lookup(options, "mtu", "q", &mtu))
        log_("[host] phone " + device.substr(device.rfind('/') + 1) + ": ATT MTU " + std::to_string(mtu));
    inbound.insert(id);
    rx_.push_back({id, data});
}

void Peripheral::on_notify(bool on)
{
    tx_->notifying = on;
    if (on) inbound.insert(0);
    else inbound.clear();               // BlueZ: the last subscriber went away
}

std::vector<std::pair<int, Bytes>> Peripheral::poll_rx() { return std::move(rx_); }

void Peripheral::send(const Bytes &frame, const std::vector<int> &conns)
{
    if (inbound.empty() || !tx_ || !tx_->notifying) return;
    bool any = conns.empty();
    for (int c : conns) any = any || inbound.count(c);
    if (!any) return;
    if (txq_.size() >= TXQ_MAX) {
        txq_.pop_front();
        tx_dropped_++;
    }
    txq_.push_back(frame);
}

void Peripheral::set_advertising(const std::vector<core::Transport::Advert> &adverts)
{
    if (adverts == adverts_) return;
    adverts_ = adverts;
    adv_dirty_ = true;
}

void Peripheral::set_standby(bool on)
{
    if (on == standby_) return;
    standby_ = on;
    adv_dirty_ = true;
    if (on) txq_.clear();
}

void Peripheral::service()
{
    for (size_t i = 0; i < TX_PER_SERVICE && !txq_.empty(); i++) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Value", gdbus::bytes(txq_.front()));
        g_dbus_connection_emit_signal(bus_, nullptr, tx_->path.c_str(), "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", "org.bluez.GattCharacteristic1", &b, nullptr),
                                      nullptr);
        txq_.pop_front();
    }
    if (adv_dirty_ && started_ && !adv_busy_) {
        adv_dirty_ = false;
        sync_adverts();
    }
}

// -- advertising -------------------------------------------------------------------------

std::string Peripheral::key_of(const std::string &body)
{
    if (body.empty()) return body;
    int t = static_cast<uint8_t>(body[0]);
    return body.substr(0, t == protocol::PRODUCT_TYPE_PROBE || t == protocol::PRODUCT_TYPE_NODE ? 5 : 11);
}

void Peripheral::sync_adverts()
{
    std::map<std::string, std::pair<std::string, bool>> want;     // key -> (body, connectable)
    if (!standby_)
        for (size_t i = 0; i < adverts_.size() && i < MAX_INSTANCES; i++) {
            std::string body = adverts_[i].msd.size() > 2 ? adverts_[i].msd.substr(2) : std::string();
            want[key_of(body)] = {body, !adverts_[i].probe};
        }
    for (auto &kv : ads_) {
        auto w = want.find(kv.first);
        if (w == want.end() || w->second.second != kv.second->connectable) {
            ad_ops_.push_back({false, kv.first, nullptr});
        } else if (w->second.first != kv.second->body) {
            // New data for the same device (live temperatures): BlueZ refreshes
            // a registered advertisement when its properties change, which
            // costs far less than registering it again.
            kv.second->body = w->second.first;
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&b, "{sv}", "ManufacturerData", kv.second->h.get("ManufacturerData"));
            g_dbus_connection_emit_signal(bus_, nullptr, kv.second->path.c_str(), "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged",
                                          g_variant_new("(sa{sv}as)", "org.bluez.LEAdvertisement1", &b, nullptr),
                                          nullptr);
        }
    }
    for (auto &kv : want) {
        auto it = ads_.find(kv.first);
        bool stays = it != ads_.end() && kv.second.second == it->second->connectable;
        if (stays) continue;
        auto ad = std::make_unique<Ad>();
        ad->path = std::string(APP_PATH) + "/adv" + std::to_string(++ad_seq_);
        ad->body = kv.second.first;
        ad->connectable = kv.second.second;
        ad_ops_.push_back({true, kv.first, std::move(ad)});
    }
    next_ad_op();
}

// Registrations one at a time: BlueZ calls back into the advertisement
// object while handling each.
void Peripheral::next_ad_op()
{
    if (ad_ops_.empty()) {
        adv_busy_ = false;
        return;
    }
    adv_busy_ = true;
    AdOp op = std::move(ad_ops_.front());
    ad_ops_.pop_front();
    struct Ctx {
        Peripheral *self;
        AdOp op;
    };
    auto *ctx = new Ctx{this, std::move(op)};
    if (!ctx->op.add) {
        auto it = ads_.find(ctx->op.key);
        if (it == ads_.end()) {
            delete ctx;
            next_ad_op();
            return;
        }
        std::unique_ptr<Ad> ad = std::move(it->second);
        ads_.erase(it);
        g_dbus_connection_call(bus_, gdbus::BLUEZ, adapter_.c_str(), "org.bluez.LEAdvertisingManager1",
                               "UnregisterAdvertisement", g_variant_new("(o)", ad->path.c_str()), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                               [](GObject *src, GAsyncResult *res, gpointer user) {
                                   auto *c = static_cast<Ctx *>(user);
                                   GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
                                   if (r) g_variant_unref(r);
                                   c->self->next_ad_op();
                                   delete c;
                               },
                               ctx);
        g_dbus_connection_unregister_object(bus_, ad->reg);
        return;
    }
    Ad *ad = ctx->op.ad.get();
    ad->h.get = [ad](const char *prop) -> GVariant * {
        if (!strcmp(prop, "Type")) return g_variant_new_string(ad->connectable ? "peripheral" : "broadcast");
        if (!strcmp(prop, "ManufacturerData")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{qv}"));
            g_variant_builder_add(&b, "{qv}", static_cast<guint16>(protocol::VENDOR_ID), gdbus::bytes(ad->body));
            return g_variant_builder_end(&b);
        }
        if (!strcmp(prop, "ServiceUUIDs")) return strv({"fe59"});
        if (!strcmp(prop, "Discoverable")) return g_variant_new_boolean(FALSE);
        return nullptr;
    };
    ad->h.call = [](const char *, GVariant *, GDBusMethodInvocation *inv) {
        g_dbus_method_invocation_return_value(inv, nullptr);         // Release
    };
    ad->reg = gdbus::export_object(bus_, ad->path, kAdXml, &ad->h);
    g_dbus_connection_call(
        bus_, gdbus::BLUEZ, adapter_.c_str(), "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
        g_variant_new("(oa{sv})", ad->path.c_str(), nullptr), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer user) {
            auto *c = static_cast<Ctx *>(user);
            Peripheral *self = c->self;
            GError *err = nullptr;
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (r) {
                g_variant_unref(r);
                self->ads_[c->op.key] = std::move(c->op.ad);
            } else {
                g_dbus_connection_unregister_object(self->bus_, c->op.ad->reg);
                if (++self->adv_errors_ <= 5)
                    self->log_(std::string("[host] advertisement rejected by BlueZ: ") + err->message);
                g_error_free(err);
            }
            self->next_ad_op();
            delete c;
        },
        ctx);
}

}  // namespace linux_unit
