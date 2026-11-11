// Small helpers shared across core (port of util.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>

namespace core {

// Monotonic milliseconds (steady clock; esp_timer-backed on the ESP32).
int64_t ticks_ms();

// Seconds since 1970 once the clock has been set (NTP), else 0 ("not
// available" in link-table timestamps).
uint32_t unix_time();

// Bounded set of recently seen message IDs, for MeatNet de-duplication.
class RecentIds {
public:
    explicit RecentIds(size_t size = 128) : size_(size) {}
    // True if id was already recorded; records it otherwise.
    bool seen(uint64_t id);

private:
    size_t size_;
    std::deque<uint64_t> order_;
    std::set<uint64_t> set_;
};

}  // namespace core
