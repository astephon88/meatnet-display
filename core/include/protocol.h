// Combustion MeatNet BLE wire formats: advertising data, MeatNet Node UART
// frames (10/15-byte headers with request IDs), Probe UART frames (6/7-byte
// headers, no IDs), and the payloads a Display/repeater node sends and
// receives. Port of protocol.py; byte buffers are std::string, and "no
// value" is an empty buffer or std::nullopt.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace protocol {

using Bytes = std::string;

constexpr uint16_t VENDOR_ID = 0x09C7;
inline const Bytes VENDOR_LE("\xC7\x09", 2);

constexpr int PRODUCT_TYPE_UNKNOWN = 0;
constexpr int PRODUCT_TYPE_PROBE = 1;
constexpr int PRODUCT_TYPE_NODE = 2;        // repeated probe data in advertisements
constexpr int PRODUCT_TYPE_GAUGE = 3;
constexpr int PRODUCT_TYPE_DISPLAY = 4;
constexpr int PRODUCT_TYPE_BOOSTER = 5;
constexpr int PRODUCT_TYPE_ENGINE = 6;
const char *product_name(int ptype);        // "Probe", "Gauge", ... or ""

constexpr const char *UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *UART_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *UART_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *PROBE_STATUS_SERVICE_UUID = "00000100-CAAB-3792-3D44-97AE51C1407A";
constexpr const char *PROBE_STATUS_CHAR_UUID = "00000101-CAAB-3792-3D44-97AE51C1407A";
constexpr const char *DIS_SERVICE_UUID = "0000180A-0000-1000-8000-00805F9B34FB";
// 128-bit UUIDs little-endian, as in advertising data. The Combustion apps
// filter their scans on the DFU service (0xFE59).
extern const Bytes PROBE_STATUS_SERVICE_UUID_LE;
extern const Bytes DFU_SERVICE_UUID_LE;

// Device Information strings a node reads from a linked probe, to answer the
// app's Probe Firmware/Hardware/Model requests (0x46-0x48).
struct DisRead {
    const char *name, *service, *characteristic;
};
extern const DisRead PROBE_DIS_READS[3];

inline const Bytes SYNC_BYTES("\xCA\xFE", 2);
constexpr uint8_t RESPONSE_FLAG = 0x80;

// Probe-addressed messages (node payload = probe serial(4) + probe payload)
constexpr uint8_t MSG_SET_PROBE_ID = 0x01;
constexpr uint8_t MSG_SET_PROBE_COLOR = 0x02;
constexpr uint8_t MSG_READ_SESSION_INFO = 0x03;
constexpr uint8_t MSG_READ_LOGS = 0x04;
constexpr uint8_t MSG_SET_PREDICTION = 0x05;
constexpr uint8_t MSG_READ_OVER_TEMPERATURE = 0x06;
constexpr uint8_t MSG_CONFIGURE_FOOD_SAFE = 0x07;
constexpr uint8_t MSG_RESET_FOOD_SAFE = 0x08;
constexpr uint8_t MSG_SET_POWER_MODE = 0x09;
constexpr uint8_t MSG_RESET_THERMOMETER = 0x0A;
constexpr uint8_t MSG_SET_PROBE_HIGH_LOW_ALARM = 0x0B;
constexpr uint8_t MSG_SILENCE_ALARMS = 0x0C;
bool probe_addressed(uint8_t msg_type);     // 0x01..0x0B
// Node messages
constexpr uint8_t MSG_DEVICE_CONNECTED = 0x40;
constexpr uint8_t MSG_DEVICE_DISCONNECTED = 0x41;
constexpr uint8_t MSG_READ_NODE_LIST = 0x42;
constexpr uint8_t MSG_READ_NETWORK_TOPOLOGY = 0x43;
constexpr uint8_t MSG_READ_PROBE_LIST = 0x44;
constexpr uint8_t MSG_PROBE_STATUS = 0x45;
constexpr uint8_t MSG_PROBE_FIRMWARE_REVISION = 0x46;
constexpr uint8_t MSG_PROBE_HARDWARE_REVISION = 0x47;
constexpr uint8_t MSG_PROBE_MODEL_INFORMATION = 0x48;
constexpr uint8_t MSG_HEARTBEAT = 0x49;
constexpr uint8_t MSG_ASSOCIATE_NODE = 0x4A;
constexpr uint8_t MSG_SYNC_THERMOMETER_LIST = 0x4B;
constexpr uint8_t MSG_GAUGE_STATUS = 0x60;
constexpr uint8_t MSG_ENGINE_STATUS = 0x70;
constexpr uint8_t MSG_READ_LINKED_DEVICES = 0x73;
constexpr uint8_t MSG_READ_UNLINKED_DEVICES = 0x74;
constexpr uint8_t MSG_LINK_DEVICE = 0x75;
constexpr uint8_t MSG_UNLINK_DEVICE = 0x76;
constexpr uint8_t MSG_DEVICE_LINKED = 0x77;
constexpr uint8_t MSG_DEVICE_UNLINKED = 0x78;
constexpr uint8_t MSG_SET_PROXIMITY_LINKING = 0x79;
constexpr uint8_t MSG_READ_PROXIMITY_LINKING = 0x7A;
constexpr uint8_t MSG_SYNC_LINK_TABLE = 0x7B;
constexpr uint8_t MSG_DISPLAY_STATUS = 0x80;

constexpr int PROBE_MODE_NORMAL = 0;
constexpr int PROBE_MODE_INSTANT_READ = 1;
constexpr int PROBE_MODE_ERROR = 3;

constexpr int MAX_HOPS = 4;                 // hop count field is 2 bits: 1..4 hops

inline const Bytes ZERO4(4, '\0');
inline const Bytes ZERO10(10, '\0');

// -- basic encodings ---------------------------------------------------------------

double decode_raw_temperature(unsigned raw);   // 13-bit, 0.1 C steps, -20 C offset
Bytes encode_serial(const Bytes &serial);      // node serial -> exactly 10 bytes
std::string serial10_str(const Bytes &serial10);
std::string probe_serial_str(const Bytes &serial4);   // "1000ABCD", as the app shows it
Bytes probe_serial_bytes(const std::string &text);    // "1000ABCD" -> 4 bytes LE, "" if bad
uint16_t crc16_ccitt(const Bytes &data, uint16_t initial = 0xFFFF);

// Request/response IDs are random. The platform may install its own source
// (the ESP32 uses its hardware RNG); the default is a seeded mt19937.
void set_random_source(uint32_t (*fn)());
uint32_t new_message_id();

// -- MeatNet Node UART frames --------------------------------------------------------

Bytes build_request(uint8_t message_type, const Bytes &payload,
                    std::optional<uint32_t> request_id = std::nullopt);
Bytes build_response(uint8_t message_type, uint32_t request_id, bool success,
                     const Bytes &payload, std::optional<uint32_t> response_id = std::nullopt);

struct Frame {
    bool is_response = false;
    uint8_t msg_type = 0;
    uint32_t request_id = 0;
    uint32_t response_id = 0;       // responses only
    bool success = true;
    Bytes payload;
    Bytes raw;
};

// Complete, CRC-valid frames from a UART byte stream; `rest` gets what is
// left (an incomplete frame, or a trailing 0xCA that may start a sync).
// Garbage and CRC failures are skipped by resynchronising.
std::vector<Frame> split_frames(const Bytes &buf, Bytes &rest);
Bytes with_request_payload(const Frame &frame, const Bytes &payload);

// -- Probe UART frames (direct link: no request/response IDs) -------------------------

Bytes build_probe_request(uint8_t message_type, const Bytes &payload = Bytes());
// Probe-side response (for tests' fake probe): like a real probe, no
// response bit in the type.
Bytes build_probe_response(uint8_t message_type, bool success, const Bytes &payload = Bytes());

struct ProbeFrame {
    uint8_t msg_type = 0;
    bool success = false;
    Bytes payload;
    bool operator==(const ProbeFrame &o) const
    {
        return msg_type == o.msg_type && success == o.success && payload == o.payload;
    }
};
std::vector<ProbeFrame> split_probe_frames(const Bytes &buf, Bytes &rest);

// Node request for a probe -> (probe serial4, probe payload).
std::optional<std::pair<Bytes, Bytes>> node_to_probe(uint8_t msg_type, const Bytes &payload);
// Probe response payload -> node response payload (serial prefixed when the
// response has a payload at all).
Bytes probe_to_node_payload(const Bytes &serial4, const Bytes &probe_payload);

// -- advertising ----------------------------------------------------------------------

Bytes build_display_advertisement(const Bytes &serial, bool high_radio_power = true);
uint8_t network_info(int hop_count);        // 1..4 hops -> Network Information byte
int hops_from_network_info(uint8_t b);
// Probe advert body (no company ID) -> repeated-node manufacturer data (with
// company ID), product type 2 and the given hop count.
Bytes repeat_probe_advert(const Bytes &body, int hop_count = 1);
// Repeated-node manufacturer data from a probe status characteristic value
// (for a linked probe whose advert we missed).
Bytes probe_advert_from_status(const Bytes &serial4, const Bytes &status, int hop_count = 1);
// Raw advertising payload -> Combustion manufacturer body (no company ID).
std::optional<Bytes> manufacturer_data(const Bytes &adv_payload);

// -- probe data decoding ----------------------------------------------------------------

std::array<double, 8> unpack_probe_temps(const Bytes &raw13);   // LSB-first 13-bit, 0.05 C

struct ProbeData {
    int mode = 0, color = 0, probe_id = 0, core_sensor = 0;
    std::array<double, 8> temps{};
    // Core/surface/ambient are empty outside Normal mode (e.g. Instant Read).
    std::optional<double> instant_c, core_c, surface_c, ambient_c;
    bool low_battery = false;
};
ProbeData decode_probe(const Bytes &raw13, uint8_t mode_byte, uint8_t status_byte);

struct ProbeAdvert : ProbeData {
    Bytes serial;
    int ptype = 0;
    int hops = 0;               // as received (0 for a direct probe advert)
    int overheating = 0;
};
// Probe (type 1) or repeated-probe node (type 2) advert body.
std::optional<ProbeAdvert> parse_probe_advert(const Bytes &body);

struct Prediction {
    int state = 0, mode = 0, type = 0;
    double setpoint_c = 0, heat_start_c = 0, est_core_c = 0;
    uint32_t seconds = 0;
};
Prediction decode_prediction(const Bytes &b7);
extern const char *const PREDICTION_STATES[5];

struct Alarm {
    bool set = false, tripped = false, alarming = false;
    double temp_c = 0;
};
Alarm decode_alarm(unsigned value);

struct ProbeStatus : ProbeData {
    uint32_t log_min = 0, log_max = 0;
    Prediction prediction;
    int food_safe_state = 0;
    int overheating = 0;
    bool alarming = false;
    uint32_t session_id = 0;
    uint16_t sample_period = 0;
};
// Probe Status characteristic value (94 or 100 bytes).
std::optional<ProbeStatus> decode_probe_status(const Bytes &status);

// Probe Status characteristic value -> Node Probe Status (0x45) frame:
// Serial(4) + [LogRange..FoodSafeStatus (48)] + NetworkInfo +
// [Overheating..LowAlarm (46)] + SessionID(4) + SamplePeriod(2) = 105.
Bytes build_node_probe_status(const Bytes &serial4, const Bytes &status, int hop_count = 1,
                              std::optional<uint32_t> request_id = std::nullopt);
std::optional<int> node_probe_status_hops(const Bytes &payload);
// Forward a received 0x45 one more hop; nullopt if already at max hops.
std::optional<Bytes> bump_node_probe_status(const Frame &frame);

struct NodeProbeStatus {
    Bytes serial4, status;      // status shaped like the characteristic value
    int hops = 0;
};
std::optional<NodeProbeStatus> parse_node_probe_status(const Bytes &payload);

// -- other devices' advertisements ------------------------------------------------------

struct NodeAdvert {
    Bytes serial;               // 10 bytes
    int ptype = 0;
    // Gauge
    std::optional<double> temp_c;
    bool sensor_present = false, overheating = false, low_battery = false;
    // Engine
    double setpoint_c = 0;
    bool app_mode = false, ctrl_connected = false, lid_open = false;
};
std::optional<NodeAdvert> parse_gauge_advert(const Bytes &body);
std::optional<NodeAdvert> parse_engine_advert(const Bytes &body);
// Any non-probe device-info advert (Gauge/Display/Booster/Engine).
std::optional<NodeAdvert> parse_node_advert(const Bytes &body);

// -- node message payloads --------------------------------------------------------------

// Read Probe List (0x44) response; slots: up to 10 serial4 ("" = empty),
// device number = slot index + 1.
Bytes build_read_probe_list_response(uint32_t request_id, const std::vector<Bytes> &slots);

struct NodeEntry {
    int number, ptype;
    Bytes serial;
};
Bytes build_read_node_list_response(uint32_t request_id, int page,
                                    const std::vector<NodeEntry> &nodes);

using Conn = std::pair<int, int>;           // (device number, rssi)
Bytes build_topology_response(uint32_t request_id, int device_number, int ptype,
                              const Bytes &serial, const std::vector<Conn> &inbound,
                              const std::vector<Conn> &outbound);

// Serial field of a heartbeat record: probe uint32 + 6 zero bytes, or a
// 10-byte node serial.
Bytes heartbeat_serial(int ptype, const Bytes &serial);

struct HeartbeatRecord {
    int ptype;
    Bytes serial;
    int rssi;
    bool operator==(const HeartbeatRecord &o) const
    {
        return ptype == o.ptype && serial == o.serial && rssi == o.rssi;
    }
};
Bytes build_heartbeat(const Bytes &serial, const Bytes &mac6, int ptype, bool inbound,
                      const std::vector<HeartbeatRecord> &records, int hop_count = 0,
                      std::optional<uint32_t> request_id = std::nullopt);

struct Heartbeat {
    Bytes serial, mac;
    int ptype = 0, hops = 0;
    bool inbound = false;
    std::vector<HeartbeatRecord> records;
};
std::optional<Heartbeat> parse_heartbeat(const Bytes &payload);
std::optional<Bytes> bump_heartbeat(const Frame &frame);   // nullopt at max hops

Bytes build_device_connected(bool connected, int ptype, const Bytes &serial);

// Sync Thermometer List (0x4B): MAC + 4 x (present, serial4); "" = empty.
Bytes build_sync_thermometer_list(const Bytes &mac6, const std::vector<Bytes> &slots,
                                  std::optional<uint32_t> request_id = std::nullopt);
std::optional<std::pair<Bytes, std::array<Bytes, 4>>> parse_sync_thermometer_list(
    const Bytes &payload);

Bytes build_silence_alarms_response(uint32_t request_id, int ptype, const Bytes &serial);
struct SilenceAlarms {
    bool global = false;
    int ptype = 0;
    Bytes serial4, serial10;
};
SilenceAlarms parse_silence_alarms(const Bytes &payload);

// -- link tables (0x73-0x7B) ------------------------------------------------------------

Bytes encode_product_serial(int ptype, const Bytes &serial);   // 12-byte union
Bytes decode_product_serial(int ptype, const Bytes &b12);

struct LinkEntry {
    int ptype = 0;
    Bytes serial;
    uint32_t ts = 0;
    bool legacy = false;
};
Bytes build_link_entries_response(uint8_t msg_type, uint32_t request_id, int page,
                                  const std::vector<LinkEntry> &entries);

struct LinkPage {
    int page = 0, total_pages = 0;
    std::vector<LinkEntry> entries;
};
std::optional<LinkPage> parse_link_entries_response(const Bytes &payload);
std::optional<LinkEntry> parse_link_device(const Bytes &payload);
Bytes build_link_notification(bool linked, const LinkEntry &entry);

}  // namespace protocol
