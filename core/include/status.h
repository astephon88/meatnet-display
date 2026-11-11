// /api/state content shared by every unit: the Node's tables (port of
// meatnet.Node.status) and, for a field unit, what its radio hears (port of
// split.FieldUnit.status). The platform adds serial, wifi, heap, uptime,
// display, power, role and split.
#pragma once

#include <string>
#include <vector>

#include "node.h"

namespace core {

cJSON *node_status(Node &node, int64_t now);

// A link's diagnostics in the shape the page (and the split protocol) use.
std::string link_stats(uint32_t connects, const std::map<std::string, uint32_t> &drops,
                       const std::string &params, const std::string &asked);

struct LinkSummary {
    std::string kind, serial, state;
    uint32_t count = 0;
    int mtu = 23;
    std::string stats;          // JSON, "" = none
};

cJSON *field_status(const std::map<Bytes, Observation> &direct, const std::map<Bytes, Observation> &nodes,
                    const std::map<Bytes, Observation> &meaters, const std::vector<LinkSummary> &links,
                    int max_conn, int64_t now);

}  // namespace core
