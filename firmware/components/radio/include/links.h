// Outbound BLE links held by the field unit for the app unit: probes, nodes
// (Gauge/Engine/Display) and MEATERs. A C++ port of the MicroPython
// central.CentralLink + the link half of ble.BleTransport, keeping the
// lessons learned there:
//   - one connect in flight at a time (scanning paused around it);
//   - connect timeout 12 s, setup timeout 20 s, disconnect timeout 8 s;
//   - error back-off 10 s (2 s for nodes); busy errors retried next tick;
//   - services, characteristics, CCCDs, notifications enabled one at a time,
//     MTU exchange, then ready, then Device Information reads (probes);
//   - write type chosen from the characteristic's properties.
// New: every drop is reported with its HCI disconnect reason.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct ble_gap_event;

namespace radio {

// What the WiFi session reports for one link.
struct LinkReport {
    std::string key, kind, state, desc;
    int mtu = 23;
    uint32_t count = 0;
    std::map<std::string, std::string> info;
    std::vector<std::pair<std::string, std::string>> rx;   // (sub, data)
    // Diagnostics: successful connects, drops by reason, and the connection
    // parameters in use ("interval ms / latency / supervision timeout ms").
    uint32_t connects = 0;
    std::map<std::string, uint32_t> drops;
    std::string params;
    std::string asked;          // parameters the peer last requested
};

// Just a link's identity and state (for the status screen).
struct LinkBrief {
    std::string key, kind, state;
    int mtu = 23;
    uint32_t count = 0;
    uint32_t connects = 0;
    std::map<std::string, uint32_t> drops;
    std::string params, asked;
};

struct Link;

class Links {
public:
    static Links &get();

    // Commands from the app unit. Keys: probe serial (4), node serial (10),
    // MEATER address (6). addr is the peer address in display order.
    void open(const std::string &key, const std::string &kind, uint8_t addr_type,
              const std::string &addr);
    void close(const std::string &key);
    bool write(const std::string &key, const std::string &data);
    void close_all();

    // 10 Hz housekeeping (timeouts, back-off, connect scheduling, writes).
    void tick();

    // For the WiFi session: every link's state plus notifications received
    // since the last call; and links removed since the last call.
    std::vector<LinkReport> collect();
    std::vector<std::string> take_removed();
    std::vector<std::string> collect_states();   // link states only (doesn't drain rx)
    std::vector<LinkBrief> collect_brief();      // key/kind/state (doesn't drain rx)

    bool any_connected();
    bool connecting();
    size_t active();            // links connected or connecting (radio connections in use)
    uint32_t total_rx() const { return total_rx_; }
    size_t max_links = 6;

    // Called from NimBLE callbacks (host task).
    static int gap_event(::ble_gap_event *event, void *arg);

private:
    Links();
    Link *find(uint32_t id);
    void lock() { xSemaphoreTakeRecursive(mutex_, portMAX_DELAY); }
    void unlock() { xSemaphoreGiveRecursive(mutex_); }

    friend struct LinkOps;
    SemaphoreHandle_t mutex_;
    std::map<std::string, std::unique_ptr<Link>> links_;
    std::vector<std::string> removed_;
    uint32_t next_id_ = 1;
    uint32_t total_rx_ = 0;
};

// Text for an HCI disconnect reason (NimBLE reports BLE_HS_ERR_HCI_BASE + code).
std::string disconnect_reason(int reason);

}  // namespace radio
