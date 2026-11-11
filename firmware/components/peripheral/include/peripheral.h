// The unit as a BLE peripheral (app and standalone roles): what phones and
// other nodes connect to. Port of the peripheral half of the MicroPython
// ble.BleTransport.
//
// GATT: Nordic UART (TX read/notify, RX write/write-without-response) and
// Device Information; the GAP name is the serial. Advertising rotates one
// legacy advert at a time (own Display info, repeated probes, ...): the
// field radio scans with legacy commands, and a controller can't mix those
// with extended advertising.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "transport.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace periph {

using protocol::Bytes;

class Peripheral {
public:
    static Peripheral &get();

    // Before the NimBLE host starts: registers the GATT services.
    void init(const std::string &serial, const std::string &model, const std::string &firmware);
    // Connections the unit may hold in all (phones + outbound links); adverts
    // stop being connectable when they're in use. `outbound` counts the
    // links' connections.
    void set_limits(int max_conn, size_t (*outbound)());

    // Node's thread (via the transport).
    std::set<int> inbound();
    std::vector<std::pair<int, Bytes>> poll_rx();
    void send(const Bytes &frame, const std::vector<int> &conns);
    void set_advertising(const std::vector<core::Transport::Advert> &adverts, bool fast);
    void rotate();                      // next advert (every 500 ms)
    void set_firmware(const std::string &fw);
    void set_standby(bool on);
    void service();                     // MTU exchanges, adverts, queued notifications
    size_t txq_size();
    uint32_t tx_dropped() const { return tx_dropped_; }

    // NimBLE host callbacks.
    static int gap_event(struct ble_gap_event *event, void *arg);
    int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt);

private:
    void advertise();
    void drain_tx();

    std::mutex mu_;                     // state the NimBLE host task touches
    std::set<int> inbound_;
    std::vector<std::pair<int, Bytes>> rx_;
    std::vector<int> pending_mtu_;
    std::map<int, int> mtu_;            // conn -> ATT MTU
    bool need_adv_ = true;

    std::string serial_, model_, firmware_;
    std::vector<core::Transport::Advert> adverts_;
    bool fast_ = false;
    size_t adv_index_ = 0;
    bool standby_ = false;
    int max_conn_ = 4;
    size_t (*outbound_)() = nullptr;
    struct Tx {
        Bytes frame;
        std::set<int> to;
    };
    std::vector<Tx> txq_;
    uint32_t tx_dropped_ = 0;
};

}  // namespace periph
