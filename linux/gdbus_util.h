// Small GDBus helpers for talking to BlueZ.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

#include "protocol.h"

namespace gdbus {

constexpr const char *BLUEZ = "org.bluez";

GVariant *bytes(const std::string &data);           // "ay" (floating)
std::string bytes(GVariant *ay);                     // from "ay" (or a "v" holding one)
std::string addr_text(const std::string &addr6);    // "AA:BB:CC:DD:EE:FF"
std::string addr_bytes(const std::string &text);    // 6 bytes; "" on bad input
std::string uuid_lower(const char *uuid);

// An asynchronous call whose result we don't need. Always passes a reply
// callback: without one GDBus flags the message "no reply expected", which
// bluetoothd 5.64's GATT client mishandles (and on adapter power-down,
// aborts on).
void call(GDBusConnection *bus, const std::string &path, const char *iface, const char *method, GVariant *params,
          int timeout_ms = 5000);

// org.freedesktop.DBus.Properties.Get / Set (synchronous; for setup).
GVariant *get_property(GDBusConnection *bus, const std::string &path, const char *iface, const char *name);
bool set_property(GDBusConnection *bus, const std::string &path, const char *iface, const char *name,
                  GVariant *value);

// An exported object: one interface, its methods and read-only properties.
struct Handlers {
    std::function<void(const char *method, GVariant *params, GDBusMethodInvocation *inv)> call;
    std::function<GVariant *(const char *prop)> get;
};
// `xml` is the <interface> element. Returns the registration id (0 on failure).
guint export_object(GDBusConnection *bus, const std::string &path, const std::string &xml, Handlers *h);

}  // namespace gdbus
