#include "esp_radio.h"

#include "links.h"
#include "radio.h"

namespace radio {

core::RadioSource::Tables EspRadio::tables()
{
    Radio &r = Radio::get();
    r.lock();
    Tables t{r.direct, r.repeated, r.nodes, r.meaters};
    r.unlock();
    return t;
}

void EspRadio::prune(int64_t now, int64_t max_age_ms) { Radio::get().prune(now, max_age_ms); }

std::vector<core::LinkReport> EspRadio::collect_links() { return Links::get().collect(); }

std::vector<core::LinkSummary> EspRadio::link_summaries() { return Links::get().summaries(); }

std::vector<core::Bytes> EspRadio::take_removed_links() { return Links::get().take_removed(); }

void EspRadio::open_link(const core::Bytes &key, const std::string &kind, int addr_type,
                         const core::Bytes &addr)
{
    Links::get().open(key, kind, static_cast<uint8_t>(addr_type), addr);
}

void EspRadio::close_link(const core::Bytes &key) { Links::get().close(key); }

void EspRadio::write_link(const core::Bytes &key, const core::Bytes &data) { Links::get().write(key, data); }

void EspRadio::close_all_links() { Links::get().close_all(); }

void EspRadio::set_scan_active(bool on) { Radio::get().set_scan_active(on); }

void EspRadio::set_standby(bool on) { Radio::get().set_standby(on); }

void EspRadio::set_ignore(const core::Bytes &mac, const core::Bytes &serial10)
{
    Radio::get().set_ignore(mac, serial10);
}

}  // namespace radio
