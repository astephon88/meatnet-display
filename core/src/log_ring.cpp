#include "log_ring.h"

#include "cJSON.h"

namespace core {

void LogRing::push(std::string line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.size() > max_len_) line.resize(max_len_);
    std::lock_guard<std::mutex> g(mu_);
    lines_.emplace_back(++seq_, std::move(line));
    if (lines_.size() > keep_) lines_.pop_front();
}

cJSON *LogRing::since(uint32_t since, size_t first)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "lines");
    std::lock_guard<std::mutex> g(mu_);
    // A newer seq than ours means the unit restarted: send from the start.
    if (since > seq_) since = 0;
    size_t skip = 0;
    if (since == 0 && lines_.size() > first) skip = lines_.size() - first;
    for (auto &l : lines_) {
        if (skip) {
            skip--;
            continue;
        }
        if (l.first > since) cJSON_AddItemToArray(arr, cJSON_CreateString(l.second.c_str()));
    }
    cJSON_AddNumberToObject(o, "seq", seq_);
    return o;
}

}  // namespace core
