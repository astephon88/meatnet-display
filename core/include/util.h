// Small helpers shared across core.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>

namespace core {

// Monotonic milliseconds (steady clock; esp_timer-backed on the ESP32).
int64_t ticks_ms();

// Seconds since 1970 once the clock has been set (NTP), else 0 ("not
// available" in link-table timestamps).
uint32_t unix_time();

// A unit's log: lines go to the platform (serial/stdout and the web page).
using LogFn = std::function<void(const std::string &)>;
// printf-style line to `log` (nothing when it is empty).
void logf(const LogFn &log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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
