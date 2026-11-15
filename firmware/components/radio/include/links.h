// Outbound BLE links held by the field unit for the app unit: probes, nodes
// (Gauge/Engine/Display) and MEATERs:
//   - one connect in flight at a time (scanning paused around it);
//   - connect timeout 12 s, setup timeout 20 s, disconnect timeout 8 s;
//   - error back-off 10 s (2 s for nodes); busy errors retried next tick;
//   - services, characteristics, CCCDs, notifications enabled one at a time,
//     MTU exchange, then ready, then Device Information reads (probes);
//   - write type chosen from the characteristic's properties;
//   - every drop is reported with its HCI disconnect reason.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "radio_source.h"

struct ble_gap_event;

namespace radio {

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

    // Every link's state plus notifications received since the last call;
    // and links removed since the last call.
    std::vector<core::LinkReport> collect();
    std::vector<std::string> take_removed();
    std::vector<core::LinkSummary> summaries();     // for the status page (doesn't drain rx)

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
