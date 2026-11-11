// The Linux unit as a BLE peripheral on BlueZ (port of
// host/bluez_peripheral.py): what phones connect to.
//
// GATT: Nordic UART (RX write / TX notify) and Device Information, exported
// as a BlueZ GATT application. Advertising: one BlueZ advertising instance
// per payload (the controller cycles them, so there is no software
// rotation); each carries the Combustion manufacturer data and the 16-bit
// DFU service UUID the apps filter on. BlueZ notifies every subscribed
// central (it can't target one connection), so `inbound` holds the devices
// that wrote to RX plus an entry (0) while anyone is subscribed.
//
// Everything runs on the GLib main loop thread.
#pragma once

#include <gio/gio.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gdbus_util.h"
#include "transport.h"

namespace linux_unit {

using core::Bytes;
using LogFn = std::function<void(const std::string &)>;

class Peripheral {
public:
    Peripheral(GDBusConnection *bus, std::string adapter_path, std::string serial, std::string model,
               std::string firmware, LogFn log);
    ~Peripheral();
    void start();                       // powers the adapter, exports and registers the GATT app

    Bytes mac;                          // adapter address, display order
    std::set<int> inbound;
    std::vector<std::pair<int, Bytes>> poll_rx();
    void send(const Bytes &frame, const std::vector<int> &conns);
    void set_advertising(const std::vector<core::Transport::Advert> &adverts);
    void set_firmware(const std::string &fw) { firmware_ = fw; }
    void set_standby(bool on);
    void service();                     // queued notifications, advertising changes
    size_t txq_size() const { return txq_.size(); }
    uint32_t tx_dropped() const { return tx_dropped_; }

private:
    struct Chr {
        std::string path, uuid, service;
        std::vector<std::string> flags;
        std::function<std::string()> value;
        bool notifying = false;
        gdbus::Handlers h;
    };
    struct Svc {
        std::string path, uuid;
        gdbus::Handlers h;
    };
    struct Ad {
        std::string path;
        std::string body;               // manufacturer data after the company ID
        bool connectable = true;
        guint reg = 0;
        gdbus::Handlers h;
    };

    void on_write(const std::string &data, GVariant *options);
    void on_notify(bool on);
    static std::string key_of(const std::string &body);
    void sync_adverts();
    void next_ad_op();
    GVariant *managed_objects();

    GDBusConnection *bus_;
    std::string adapter_, serial_, model_, firmware_;
    LogFn log_;
    std::vector<std::unique_ptr<Svc>> svcs_;
    std::vector<std::unique_ptr<Chr>> chrs_;
    Chr *tx_ = nullptr;
    gdbus::Handlers root_;
    std::vector<guint> regs_;

    std::map<std::string, int> device_ids_;     // BlueZ device path -> inbound id
    std::vector<std::pair<int, Bytes>> rx_;
    std::deque<Bytes> txq_;
    uint32_t tx_dropped_ = 0;
    bool standby_ = false;

    std::vector<core::Transport::Advert> adverts_;
    bool adv_dirty_ = false, adv_busy_ = false, started_ = false;
    std::map<std::string, std::unique_ptr<Ad>> ads_;   // key -> registered advertisement
    struct AdOp {
        bool add;
        std::string key;
        std::unique_ptr<Ad> ad;         // add only
    };
    std::deque<AdOp> ad_ops_;
    int ad_seq_ = 0, adv_errors_ = 0;
};

}  // namespace linux_unit
