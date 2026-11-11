#include "peripheral.h"

#include <cstring>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "proto.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace periph {

static const char *TAG = "periph";
static constexpr size_t kTxqMax = 240;
static constexpr int kTxPerService = 12;
static const Bytes kFlagsAd("\x02\x01\x06", 3);    // LE General Discoverable, BR/EDR unsupported

static ble_uuid128_t g_uart_svc, g_uart_tx, g_uart_rx;
static ble_uuid16_t g_dis_svc = BLE_UUID16_INIT(0x180A), g_serial_chr = BLE_UUID16_INIT(0x2A25),
                    g_fw_chr = BLE_UUID16_INIT(0x2A26), g_hw_chr = BLE_UUID16_INIT(0x2A27),
                    g_model_chr = BLE_UUID16_INIT(0x2A24), g_mfr_chr = BLE_UUID16_INIT(0x2A29);
static uint16_t g_tx_handle, g_rx_handle;
static ble_gatt_chr_def g_uart_chrs[3], g_dis_chrs[6];
static ble_gatt_svc_def g_svcs[3];

Peripheral &Peripheral::get()
{
    static Peripheral p;
    return p;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *)
{
    return Peripheral::get().access(conn, attr, ctxt);
}

static void uuid128(const char *text, ble_uuid128_t &out)
{
    out.u.type = BLE_UUID_TYPE_128;
    proto::uuid128_le(text, out.value);
}

void Peripheral::init(const std::string &serial, const std::string &model, const std::string &firmware)
{
    serial_ = serial;
    model_ = model;
    firmware_ = firmware;
    uuid128(protocol::UART_SERVICE_UUID, g_uart_svc);
    uuid128(protocol::UART_TX_CHAR_UUID, g_uart_tx);
    uuid128(protocol::UART_RX_CHAR_UUID, g_uart_rx);

    memset(g_uart_chrs, 0, sizeof(g_uart_chrs));
    g_uart_chrs[0].uuid = &g_uart_tx.u;
    g_uart_chrs[0].access_cb = access_cb;
    g_uart_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    g_uart_chrs[0].val_handle = &g_tx_handle;
    g_uart_chrs[1].uuid = &g_uart_rx.u;
    g_uart_chrs[1].access_cb = access_cb;
    g_uart_chrs[1].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_uart_chrs[1].val_handle = &g_rx_handle;

    memset(g_dis_chrs, 0, sizeof(g_dis_chrs));
    ble_uuid16_t *dis[] = {&g_serial_chr, &g_fw_chr, &g_hw_chr, &g_model_chr, &g_mfr_chr};
    for (int i = 0; i < 5; i++) {
        g_dis_chrs[i].uuid = &dis[i]->u;
        g_dis_chrs[i].access_cb = access_cb;
        g_dis_chrs[i].flags = BLE_GATT_CHR_F_READ;
    }

    memset(g_svcs, 0, sizeof(g_svcs));
    g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[0].uuid = &g_uart_svc.u;
    g_svcs[0].characteristics = g_uart_chrs;
    g_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[1].uuid = &g_dis_svc.u;
    g_svcs[1].characteristics = g_dis_chrs;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(serial.c_str());
    if (ble_gatts_count_cfg(g_svcs) != 0 || ble_gatts_add_svcs(g_svcs) != 0)
        ESP_LOGE(TAG, "GATT services not registered");
}

void Peripheral::set_limits(int max_conn, size_t (*outbound)())
{
    max_conn_ = max_conn;
    outbound_ = outbound;
}

int Peripheral::access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr != g_rx_handle) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        Bytes data(len, '\0');
        ble_hs_mbuf_to_flat(ctxt->om, &data[0], len, nullptr);
        std::lock_guard<std::mutex> g(mu_);
        rx_.push_back({conn, std::move(data)});
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    const ble_uuid_t *u = ctxt->chr->uuid;
    std::string value;
    if (ble_uuid_cmp(u, &g_serial_chr.u) == 0) value = serial_;
    else if (ble_uuid_cmp(u, &g_fw_chr.u) == 0) {
        std::lock_guard<std::mutex> g(mu_);
        value = firmware_;
    } else if (ble_uuid_cmp(u, &g_hw_chr.u) == 0) value = "1.0";
    else if (ble_uuid_cmp(u, &g_model_chr.u) == 0) value = model_;
    else if (ble_uuid_cmp(u, &g_mfr_chr.u) == 0) value = "Combustion Inc.";
    return os_mbuf_append(ctxt->om, value.data(), value.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int Peripheral::gap_event(struct ble_gap_event *event, void *)
{
    Peripheral &p = get();
    std::lock_guard<std::mutex> g(p.mu_);
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            int c = event->connect.conn_handle;
            p.inbound_.insert(c);
            p.mtu_[c] = 23;
            p.pending_mtu_.push_back(c);
        }
        p.need_adv_ = true;         // advertising stops on a connection
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        p.inbound_.erase(event->disconnect.conn.conn_handle);
        p.mtu_.erase(event->disconnect.conn.conn_handle);
        p.need_adv_ = true;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        p.need_adv_ = true;
        break;
    case BLE_GAP_EVENT_MTU:
        p.mtu_[event->mtu.conn_handle] = event->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

std::set<int> Peripheral::inbound()
{
    std::lock_guard<std::mutex> g(mu_);
    return inbound_;
}

std::vector<std::pair<int, Bytes>> Peripheral::poll_rx()
{
    std::lock_guard<std::mutex> g(mu_);
    return std::move(rx_);
}

void Peripheral::send(const Bytes &frame, const std::vector<int> &conns)
{
    std::set<int> to;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (int c : conns)
            if (inbound_.count(c)) to.insert(c);
    }
    if (to.empty()) return;
    if (txq_.size() >= kTxqMax) {
        txq_.erase(txq_.begin());
        tx_dropped_++;
    }
    txq_.push_back({frame, to});
}

size_t Peripheral::txq_size() { return txq_.size(); }

void Peripheral::set_advertising(const std::vector<core::Transport::Advert> &adverts, bool fast)
{
    if (adverts == adverts_ && fast == fast_) return;
    bool same_count = adverts.size() == adverts_.size();
    adverts_ = adverts;
    fast_ = fast;
    if (!same_count || adverts_.size() == 1) {
        std::lock_guard<std::mutex> g(mu_);
        need_adv_ = true;
    }
}

void Peripheral::rotate()
{
    if (adverts_.size() < 2 || standby_) return;
    adv_index_ = (adv_index_ + 1) % adverts_.size();
    std::lock_guard<std::mutex> g(mu_);
    need_adv_ = true;
}

void Peripheral::set_firmware(const std::string &fw)
{
    std::lock_guard<std::mutex> g(mu_);
    firmware_ = fw;
}

void Peripheral::set_standby(bool on)
{
    if (on == standby_) return;
    standby_ = on;
    if (on) {
        ble_gap_adv_stop();
        txq_.clear();
        for (int c : inbound()) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        std::lock_guard<std::mutex> g(mu_);
        need_adv_ = true;
    }
}

// Advert data as the MicroPython unit built it: flags + manufacturer data; the
// scan response carries the DFU service the apps filter on, plus our name
// (or, for a direct-probe advert, the Probe Status service, like a real probe).
void Peripheral::advertise()
{
    if (standby_ || adverts_.empty()) {
        ble_gap_adv_stop();
        return;
    }
    if (adv_index_ >= adverts_.size()) adv_index_ = 0;
    const auto &a = adverts_[adv_index_];
    Bytes adv = kFlagsAd + static_cast<char>(a.msd.size() + 1) + '\xFF' + a.msd, rsp;
    if (a.probe) {
        rsp = Bytes("\x03\x03\x59\xFE", 4) + '\x11' + '\x07' + protocol::PROBE_STATUS_SERVICE_UUID_LE;
    } else {
        std::string name = serial_.substr(0, 11);
        rsp = Bytes("\x11\x07", 2) + protocol::DFU_SERVICE_UUID_LE + static_cast<char>(name.size() + 1) +
              '\x09' + name;
    }
    size_t in_use = inbound().size() + (outbound_ ? outbound_() : 0);
    bool connectable = !a.probe && in_use < static_cast<size_t>(max_conn_);   // never a fake probe
    ble_gap_adv_stop();
    ble_gap_adv_set_data(reinterpret_cast<const uint8_t *>(adv.data()), adv.size());
    ble_gap_adv_rsp_set_data(reinterpret_cast<const uint8_t *>(rsp.data()), rsp.size());
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = fast_ ? 160 : 400;     // 100 / 250 ms (0.625 ms units)
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &p, gap_event, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGD(TAG, "advertising failed (%d)", rc);
}

void Peripheral::service()
{
    std::vector<int> mtu_now;
    bool adv;
    {
        std::lock_guard<std::mutex> g(mu_);
        mtu_now.swap(pending_mtu_);
        adv = need_adv_;
        need_adv_ = false;
    }
    // The Combustion app doesn't start the MTU exchange itself; real nodes
    // do, otherwise larger frames would be truncated.
    for (int c : mtu_now) ble_gattc_exchange_mtu(c, nullptr, nullptr);
    if (adv) advertise();
    drain_tx();
}

void Peripheral::drain_tx()
{
    int sent = 0;
    while (!txq_.empty() && sent < kTxPerService) {
        Tx &tx = txq_.front();
        for (auto it = tx.to.begin(); it != tx.to.end();) {
            int c = *it;
            int mtu;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (!inbound_.count(c)) {
                    it = tx.to.erase(it);
                    continue;
                }
                mtu = mtu_.count(c) ? mtu_[c] : 23;
            }
            // A frame larger than the MTU would be truncated, and the app
            // rejects partial frames: skip it for this connection.
            if (mtu < static_cast<int>(tx.frame.size()) + 3) {
                it = tx.to.erase(it);
                continue;
            }
            struct os_mbuf *om = ble_hs_mbuf_from_flat(tx.frame.data(), tx.frame.size());
            if (!om) return;            // stack buffers full: retry next pass
            ble_gatts_notify_custom(c, g_tx_handle, om);
            it = tx.to.erase(it);
            sent++;
        }
        txq_.erase(txq_.begin());
    }
}

}  // namespace periph
