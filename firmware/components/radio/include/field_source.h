// The field unit's radio (scan tables + links) as core's split client sees it.
#pragma once

#include "split.h"

namespace radio {

class FieldSource : public split::FieldSource {
public:
    Tables tables() override;
    void prune(int64_t now, int64_t max_age_ms) override;
    std::vector<split::LinkReport> collect_links() override;
    std::vector<split::Bytes> take_removed_links() override;
    void open_link(const split::Bytes &key, const std::string &kind, int addr_type,
                   const split::Bytes &addr) override;
    void close_link(const split::Bytes &key) override;
    void write_link(const split::Bytes &key, const split::Bytes &data) override;
    void close_all_links() override;
    void set_scan_active(bool on) override;
    void set_standby(bool on) override;
    void set_ignore(const split::Bytes &mac, const split::Bytes &serial10) override;
};

}  // namespace radio
