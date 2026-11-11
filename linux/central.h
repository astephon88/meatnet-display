// The Linux unit's scanning and outbound links on BlueZ (port of
// host/bleak_central.py, without bleak): a split::FieldSource, so the field
// role streams it to the app unit and the standalone role's transport reads
// it directly.
//
// D-Bus work runs on the GLib main loop thread; the FieldSource methods may
// be called from any thread (tables and link reports are locked, commands
// are handed to the loop).
#pragma once

#include <gio/gio.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gdbus_util.h"
#include "split.h"
#include "status.h"

namespace linux_unit {

using core::Bytes;
using LogFn = std::function<void(const std::string &)>;

class Central : public split::FieldSource {
public:
    Central(GDBusConnection *bus, GMainContext *loop, std::string adapter_path, LogFn log);
    void start();                       // on the loop thread
    void shutdown();                    // disconnects our links (the loop has stopped)

    // split::FieldSource (any thread)
    Tables tables() override;
    void prune(int64_t now, int64_t max_age_ms) override;
    std::vector<split::LinkReport> collect_links() override;
    std::vector<Bytes> take_removed_links() override;
    void open_link(const Bytes &key, const std::string &kind, int addr_type, const Bytes &addr) override;
    void close_link(const Bytes &key) override;
    void write_link(const Bytes &key, const Bytes &data) override;
    void close_all_links() override;
    void set_scan_active(bool) override {}         // BlueZ scans actively anyway
    void set_standby(bool on) override;
    void set_ignore(const Bytes &mac, const Bytes &serial10) override;

    Bytes mac;                          // adapter address, display order
    std::vector<core::LinkSummary> link_summaries();   // for the status page (doesn't drain)

private:
    struct Dev {
        std::string address;            // "AA:BB:..."
        bool random = false;
        std::string name;
        int rssi = 0;
        Bytes mfr;                      // Combustion manufacturer data, "" = none
        std::set<std::string> uuids;    // lower case
        bool connected = false, resolved = false;
    };
    struct Link;

    void run(std::function<void()> fn);             // on the loop thread
    void load_objects();
    void discovery(bool on);
    void update_device(const std::string &path, GVariant *props, bool advert);
    void on_signal(const char *path, const char *iface, const char *signal, GVariant *params);
    void link_step(Link &l);
    void link_connect(Link &l);
    struct Discovery;
    void link_discover(Link &l);
    std::shared_ptr<Link> discovery_link(Discovery *d);
    void discover_objects(Discovery *d, GVariant *r);
    void discover_next_dis(Discovery *d);
    void link_fail(Link &l, const std::string &why, int retry_ms);
    void watch_setup(Link &l);
    void link_write_next(Link &l);
    void schedule(Link &l, int ms);
    void remove_link(const Bytes &key);

    GDBusConnection *bus_;
    GMainContext *loop_;
    std::string adapter_;
    LogFn log_;
    std::map<std::string, Dev> devs_;               // loop thread: device path -> what we know

    std::mutex mu_;                                 // guards everything below
    Tables tables_;
    Bytes ignore_mac_, ignore_serial_;
    std::map<Bytes, std::shared_ptr<Link>> links_;
    std::vector<Bytes> removed_;
    bool standby_ = false, connecting_ = false;
};

}  // namespace linux_unit
