#include "util.h"

#include <chrono>
#include <ctime>

namespace core {

int64_t ticks_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t unix_time()
{
    std::time_t t = std::time(nullptr);
    return t > 1600000000 ? static_cast<uint32_t>(t) : 0;
}

bool RecentIds::seen(uint64_t id)
{
    if (set_.count(id)) return true;
    set_.insert(id);
    order_.push_back(id);
    if (order_.size() > size_) {
        set_.erase(order_.front());
        order_.pop_front();
    }
    return false;
}

}  // namespace core
