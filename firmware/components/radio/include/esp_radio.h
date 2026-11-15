// The unit's radio (Radio's scan tables + Links) as core uses it: the split
// client streams it (field role), the Node's transport reads it (standalone).
#pragma once

#include "radio_source.h"

namespace radio {

class EspRadio : public core::RadioSource {
public:
    Tables tables() override;
    void prune(int64_t now, int64_t max_age_ms) override;
    std::vector<core::LinkReport> collect_links() override;
    std::vector<core::LinkSummary> link_summaries() override;
    std::vector<core::Bytes> take_removed_links() override;
    void open_link(const core::Bytes &key, const std::string &kind, int addr_type,
                   const core::Bytes &addr) override;
    void close_link(const core::Bytes &key) override;
    void write_link(const core::Bytes &key, const core::Bytes &data) override;
    void close_all_links() override;
    void set_scan_active(bool on) override;
    void set_standby(bool on) override;
    void set_ignore(const core::Bytes &mac, const core::Bytes &serial10) override;
};

}  // namespace radio
