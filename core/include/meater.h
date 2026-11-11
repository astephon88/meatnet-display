// MEATER Pro -> MeatNet bridge: decoding and Combustion advert synthesis
// (port of meater.py).
//
// The MEATER Pro sends its temperatures only over a GATT connection: the
// temperature characteristic notifies 12 bytes = six little-endian uint16
// (tip 1..5, ambient), each (raw + 4) / 32 degrees C; the battery
// characteristic notifies the charge % in byte 0.
//
// The bridge re-publishes a MEATER as a Combustion probe in repeated-probe
// (product type 2) advertisements: T8 (virtual ambient) = MEATER ambient;
// T1..T7 = the 5 tip sensors spread linearly (T1 = tip 1, T7 = tip 5);
// virtual core = the lowest of T1..T6; virtual surface = T7.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "protocol.h"

namespace meater {

using protocol::Bytes;

// Service UUIDs differ between models, so a link finds the characteristics
// in whatever service holds them; these only spot adverts.
extern const char *const SERVICE_UUIDS[2];
constexpr const char *TEMP_CHAR_UUID = "7EDDA774-045E-4BBF-909B-45D1991A2876";
constexpr const char *BATTERY_CHAR_UUID = "2ADB4877-68D8-4884-BD3C-D83853BF27B8";

// Device information reported for a bridged MEATER (0x46-0x48), shaped like
// a real Combustion probe's; the firmware matches a current probe so the app
// doesn't offer an update.
constexpr const char *DIS_MODEL = "00001011:MEATER";
constexpr const char *DIS_FIRMWARE = "v2.4.0";
constexpr const char *DIS_HARDWARE = "v2.16-A1";

constexpr int LOW_BATTERY_PCT = 20;
constexpr int SURFACE_CODE = 3;     // virtual surface T7 (codes 0..3 = T4..T7)
constexpr int AMBIENT_CODE = 3;     // virtual ambient T8 (codes 0..3 = T5..T8)
constexpr int SERIAL_PREFIX = 0x4D; // 'M': synthesized serials read 4Dxxxxxx

using Temps6 = std::array<double, 6>;   // tip 1..5, ambient
using Temps8 = std::array<double, 8>;   // Combustion T1..T8

// Advertising/scan-response payload -> device name ("" if it only matched on
// a MEATER service UUID), or nullopt if it isn't a MEATER.
std::optional<std::string> adv_match(const Bytes &payload);
std::optional<Temps6> decode_temps(const Bytes &data);
Temps8 map_sensors(const Temps6 &t6);       // uses t6[0..4] as tips, t6[5] as ambient
int core_index(const Temps8 &t8);           // lowest of T1..T6
// 8 temps -> 13-byte raw field (0.05 C, -20 C offset, 13 bits each) and an
// overheating mask flagging sensors clamped at the top of the range.
std::pair<Bytes, int> encode_raw13(const Temps8 &t8);
Bytes serial_for(const Bytes &addr6);       // stable synthesized probe serial (4 bytes LE)
std::string addr_str(const Bytes &addr6);   // "AA:BB:CC:DD:EE:FF"
Bytes addr_bytes(const std::string &text);  // "" on bad input

struct Fields {
    Bytes raw13;
    int mode = 0;       // Mode/ID byte
    int status = 0;     // battery + virtual sensors byte
    int vs = 0;         // virtual sensor bits
    int overheat = 0;
    int core = 0;
    double core_c = 0;
};
Fields encode_fields(const Temps8 &t8, int probe_id = 8, int color = 0, bool low_battery = false);

// Repeated-probe manufacturer data (with company ID) -> (msd, core index).
std::pair<Bytes, int> build_advert(const Bytes &serial4, const Temps8 &t8, int probe_id = 8,
                                   int color = 0, bool low_battery = false, int hop_count = 1);
// Direct-probe manufacturer data (type 1), the advert a probe sends itself:
// the apps only discover new probes from these.
Bytes build_direct_advert(const Bytes &serial4, const Temps8 &t8, int probe_id = 8, int color = 0,
                          bool low_battery = false);

Bytes prediction_status(double core_c);     // 7 bytes: inserted, est. core = core_c
Bytes prediction_log(int vs);               // 7 bytes: virtual sensors + state
// 100-byte Probe Status characteristic value, laid out as a real probe's.
Bytes build_status(const Fields &f, uint32_t log_min, uint32_t log_max, uint32_t session_id,
                   uint16_t sample_period_ms);
Bytes log_record(const Fields &f);          // 20-byte log body

}  // namespace meater
