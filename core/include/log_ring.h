// The last lines of the unit's log for the web page (/api/log?since=N):
// each line gets a sequence number; a page polls with the last one it has.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

struct cJSON;

namespace core {

class LogRing {
public:
    explicit LogRing(size_t keep = 100, size_t max_len = 200) : keep_(keep), max_len_(max_len) {}
    void push(std::string line);                // thread-safe
    // {"seq": newest, "lines": [text, ...]} with the lines after `since`
    // (at most the last `first` of them when since is 0: a fresh page).
    cJSON *since(uint32_t since, size_t first = 40);

private:
    std::mutex mu_;
    std::deque<std::pair<uint32_t, std::string>> lines_;
    uint32_t seq_ = 0;
    size_t keep_, max_len_;
};

}  // namespace core
