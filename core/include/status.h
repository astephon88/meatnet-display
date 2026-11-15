// /api/state content shared by every unit: the Node's tables and, for a
// field unit, what its radio hears. The platform adds serial, wifi, heap, uptime,
// display, power, role and split.
#pragma once

#include <string>
#include <vector>

#include "node.h"
#include "radio_source.h"

namespace core {

cJSON *node_status(Node &node, int64_t now);

// The "split" part of /api/state: the link to the other unit. max_conn (app
// unit): the connections the field unit offers.
cJSON *split_status(bool connected, const std::string &peer, const std::string &ip, int rtt_ms,
                    int max_conn = -1);

cJSON *field_status(const std::map<Bytes, Observation> &direct, const std::map<Bytes, Observation> &nodes,
                    const std::map<Bytes, Observation> &meaters, const std::vector<LinkSummary> &links,
                    int max_conn, int64_t now);

}  // namespace core
