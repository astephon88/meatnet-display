// The core::Transport the Node runs on, on Linux: phones and adverts on the
// BlueZ peripheral; with a Central (standalone role) also its scan tables and
// outbound links. On an app unit it is the local half of split::RemoteRadio,
// without a central. Loop thread only.
#pragma once

#include "central.h"
#include "peripheral.h"

namespace linux_unit {

class LinuxTransport : public core::Transport {
public:
    LinuxTransport(Peripheral &p, Central *c);

    std::vector<std::pair<int, Bytes>> poll_rx() override;
    void send(const Bytes &frame, const std::vector<int> &conns) override { p_.send(frame, conns); }
    std::shared_ptr<core::Link> open_link(const Bytes &key, const std::string &kind, int addr_type,
                                          const Bytes &addr) override;
    void close_link(const Bytes &key) override;
    void set_advertising(const std::vector<Advert> &adverts, bool) override { p_.set_advertising(adverts); }
    void set_scan_active(bool) override {}
    void set_standby(bool on) override;
    void set_firmware(const std::string &fw) override { p_.set_firmware(fw); }
    size_t txq_size() const override { return p_.txq_size(); }
    uint32_t tx_dropped() const override { return p_.tx_dropped(); }

private:
    void pump();
    Peripheral &p_;
    Central *c_;
};

}  // namespace linux_unit
