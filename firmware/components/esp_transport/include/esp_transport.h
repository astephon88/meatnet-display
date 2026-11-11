// The core::Transport the Node runs on, on the ESP32: phones and adverts on
// the peripheral; with `central` (standalone role) also the scan tables and
// outbound links of the radio. On an app unit it is the local half of
// split::RemoteRadio, and has no central.
#pragma once

#include "transport.h"

namespace esp {

class EspTransport : public core::Transport {
public:
    explicit EspTransport(bool central);

    std::vector<std::pair<int, core::Bytes>> poll_rx() override;
    void send(const core::Bytes &frame, const std::vector<int> &conns) override;
    std::shared_ptr<core::Link> open_link(const core::Bytes &key, const std::string &kind, int addr_type,
                                          const core::Bytes &addr) override;
    void close_link(const core::Bytes &key) override;
    void set_advertising(const std::vector<Advert> &adverts, bool fast) override;
    void rotate_advertisement() override;
    void set_scan_active(bool on) override;
    void set_standby(bool on) override;
    void set_firmware(const std::string &fw) override;
    size_t txq_size() const override;
    uint32_t tx_dropped() const override;

private:
    void pump();                // scan tables + link reports from the radio
    bool central_;
};

}  // namespace esp
