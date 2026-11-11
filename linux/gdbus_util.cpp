#include "gdbus_util.h"

#include <cctype>
#include <cstdio>

namespace gdbus {

GVariant *bytes(const std::string &data)
{
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.data(), data.size(), 1);
}

std::string bytes(GVariant *v)
{
    if (!v) return std::string();
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(v);
        std::string out = bytes(inner);
        g_variant_unref(inner);
        return out;
    }
    if (!g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) return std::string();
    gsize n = 0;
    const void *p = g_variant_get_fixed_array(v, &n, 1);
    return std::string(static_cast<const char *>(p), n);
}

std::string addr_text(const std::string &a)
{
    char buf[18];
    if (a.size() != 6) return std::string();
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2],
             (uint8_t)a[3], (uint8_t)a[4], (uint8_t)a[5]);
    return buf;
}

std::string addr_bytes(const std::string &text)
{
    std::string out;
    unsigned v[6];
    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return out;
    for (unsigned x : v) out.push_back(static_cast<char>(x));
    return out;
}

std::string uuid_lower(const char *uuid)
{
    std::string s(uuid);
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void call(GDBusConnection *bus, const std::string &path, const char *iface, const char *method, GVariant *params,
          int timeout_ms)
{
    g_dbus_connection_call(bus, BLUEZ, path.c_str(), iface, method, params, nullptr, G_DBUS_CALL_FLAGS_NONE,
                           timeout_ms, nullptr,
                           [](GObject *src, GAsyncResult *res, gpointer) {
                               GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
                               if (r) g_variant_unref(r);
                           },
                           nullptr);
}

GVariant *get_property(GDBusConnection *bus, const std::string &path, const char *iface, const char *name)
{
    GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ, path.c_str(), "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (!r) return nullptr;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

bool set_property(GDBusConnection *bus, const std::string &path, const char *iface, const char *name,
                  GVariant *value)
{
    GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ, path.c_str(), "org.freedesktop.DBus.Properties", "Set",
                                              g_variant_new("(ssv)", iface, name, value), nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (!r) return false;
    g_variant_unref(r);
    return true;
}

static void on_call(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                    GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
    auto *h = static_cast<Handlers *>(user);
    if (h->call) h->call(method, params, inv);
    else g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported", "not supported");
}

static GVariant *on_get(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *prop,
                        GError **err, gpointer user)
{
    auto *h = static_cast<Handlers *>(user);
    GVariant *v = h->get ? h->get(prop) : nullptr;
    if (!v) g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
    return v;
}

static const GDBusInterfaceVTable kVtable = {on_call, on_get, nullptr, {nullptr}};

guint export_object(GDBusConnection *bus, const std::string &path, const std::string &xml, Handlers *h)
{
    GError *err = nullptr;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(("<node>" + xml + "</node>").c_str(), &err);
    if (!info) {
        g_printerr("bad introspection XML: %s\n", err->message);
        g_error_free(err);
        return 0;
    }
    guint id = g_dbus_connection_register_object(bus, path.c_str(), info->interfaces[0], &kVtable, h, nullptr, &err);
    if (!id) {
        g_printerr("can't export %s: %s\n", path.c_str(), err->message);
        g_error_free(err);
    }
    g_dbus_node_info_unref(info);
    return id;
}

}  // namespace gdbus
