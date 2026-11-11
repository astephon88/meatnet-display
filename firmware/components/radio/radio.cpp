#include "radio.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "proto.h"

namespace radio {

static const char *TAG = "radio";

Radio &Radio::get()
{
    static Radio instance;
    return instance;
}

Radio::Radio() : mutex_(xSemaphoreCreateMutex()) {}

int64_t Radio::now_ms()
{
    return esp_timer_get_time() / 1000;
}

void Radio::on_ready()
{
    uint8_t le[6] = {0};
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, le, nullptr);
    address_ = proto::addr_display(le);
    ready_ = true;
    restart_scan();
}

void Radio::restart_scan()
{
    if (!ready_ || !scan_enabled_) return;
    ble_gap_disc_cancel();          // fine if no scan is running
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE identity address");
        return;
    }
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    // Units of 0.625 ms: a 30 ms window every 30*100/duty ms; about 3% in
    // standby.
    if (standby_) {
        p.itvl = 2048;              // 1.28 s
        p.window = 64;              // 40 ms
    } else {
        p.window = 48;              // 30 ms
        p.itvl = static_cast<uint16_t>(48 * 100 / (duty_ < 5 ? 5 : duty_));
    }
    // Active scanning (scan requests) only when the app unit asks for it:
    // MEATER names/UUIDs may only be in the scan response.
    p.passive = scan_active_ && !standby_ ? 0 : 1;
    p.filter_duplicates = 0;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, this);
    scanning_ = rc == 0;
    if (rc != 0 && rc != BLE_HS_EBUSY)       // EBUSY: a connect is in flight
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else if (rc == 0)
        ESP_LOGD(TAG, "scanning: %s, %d%% duty", p.passive ? "passive" : "active",
                 standby_ ? 3 : duty_);
}

void Radio::set_duty(int percent)
{
    if (percent == duty_) return;
    duty_ = percent;
    if (scanning_) restart_scan();
}

void Radio::pause_scan()
{
    if (scanning_) ble_gap_disc_cancel();
    scanning_ = false;
}

void Radio::ensure_scan()
{
    if (!scanning_) restart_scan();
}

void Radio::set_scan_active(bool on)
{
    if (on == scan_active_) return;
    scan_active_ = on;
    restart_scan();
}

void Radio::set_standby(bool on)
{
    if (on == standby_) return;
    standby_ = on;
    restart_scan();
}

void Radio::set_ignore(const std::string &mac_display, const std::string &serial10)
{
    lock();
    ignore_mac_ = mac_display;
    ignore_serial_ = serial10;
    // Forget anything already collected from the app unit.
    for (Table *t : {&direct, &nodes}) {
        for (auto it = t->begin(); it != t->end();) {
            if (!mac_display.empty() && it->second.addr == mac_display)
                it = t->erase(it);
            else
                ++it;
        }
    }
    if (!serial10.empty()) nodes.erase(serial10);
    meaters.erase(mac_display);
    repeated.clear();               // can't tell whose: refills within seconds
    unlock();
}

void Radio::prune(int64_t now_ms, int64_t max_age_ms)
{
    lock();
    for (Table *t : {&direct, &repeated, &nodes, &meaters}) {
        for (auto it = t->begin(); it != t->end();) {
            if (now_ms - it->second.ms > max_age_ms)
                it = t->erase(it);
            else
                ++it;
        }
    }
    unlock();
}

int Radio::gap_event(struct ble_gap_event *event, void *arg)
{
    auto *self = static_cast<Radio *>(arg);
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        self->ingest(event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        // Stopped: the radio task resumes it (not while a connect is in flight).
        self->scanning_ = false;
        break;
    default:
        break;
    }
    return 0;
}

void Radio::ingest(const struct ble_gap_disc_desc &d)
{
    std::string addr = proto::addr_display(d.addr.val);
    if (!ignore_mac_.empty() && addr == ignore_mac_) return;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) != 0) return;
    int64_t now = now_ms();

    if (f.mfg_data && f.mfg_data_len >= 3 &&
        (f.mfg_data[0] | f.mfg_data[1] << 8) == proto::kCombustionVendor) {
        std::string body(reinterpret_cast<const char *>(f.mfg_data + 2), f.mfg_data_len - 2);
        uint8_t ptype = static_cast<uint8_t>(body[0]);
        lock();
        adverts_++;
        if (ptype == proto::kProductProbe && body.size() >= 20) {
            Sighting &s = direct[body.substr(1, 4)];
            s.body = body; s.ms = now; s.addr_type = d.addr.type; s.addr = addr; s.rssi = d.rssi;
        } else if (ptype == proto::kProductRepeated && body.size() >= 20) {
            std::string serial = body.substr(1, 4);
            if (serial != std::string(4, '\0')) {
                Sighting &s = repeated[serial];
                s.body = body; s.ms = now; s.rssi = d.rssi;
            }
        } else if (ptype >= proto::kProductGauge && body.size() >= 11) {
            std::string serial = body.substr(1, 10);
            if (serial != ignore_serial_) {
                Sighting &s = nodes[serial];
                s.body = body; s.ms = now; s.addr_type = d.addr.type; s.addr = addr; s.rssi = d.rssi;
            }
        }
        unlock();
        return;
    }

    bool uuid_hit = false;
    for (int i = 0; i < f.num_uuids128 && !uuid_hit; i++)
        uuid_hit = proto::meater_service_uuid(f.uuids128[i].value);
    bool name_hit = proto::meater_name(f.name, f.name_len);
    if (!uuid_hit && !name_hit) return;
    lock();
    adverts_++;
    Sighting &s = meaters[addr];
    if (f.name && f.name_len) s.name.assign(reinterpret_cast<const char *>(f.name), f.name_len);
    s.ms = now; s.addr_type = d.addr.type; s.addr = addr; s.rssi = d.rssi;
    unlock();
}

}  // namespace radio
