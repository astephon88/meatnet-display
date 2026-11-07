"""MEATER Pro -> MeatNet bridge: decoding and Combustion advert synthesis.

Pure Python (MicroPython + CPython host tests). The MEATER Pro sends its
temperatures only over a GATT connection: the temperature characteristic
notifies 12 bytes = six little-endian uint16 (tip 1..5, ambient), each
(raw + 4) / 32 degrees C; the battery characteristic notifies the charge %
in byte 0 (protocol from meater-receiver/meater_receiver.py).

The bridge re-publishes a MEATER as a Combustion probe in repeated-probe
(product type 2, "via a node") advertisements:
  - T8 (virtual ambient)      = MEATER ambient
  - T1..T7                    = the 5 MEATER tip sensors spread linearly
                                (T1 = tip 1, T7 = tip 5, interpolated between)
  - virtual core              = the lowest of T1..T6 (the Combustion core
                                field can only name T1-T6)
  - virtual surface           = T7 (the internal sensor nearest the meat
                                surface); virtual ambient = T8
"""

import protocol

# Service UUIDs differ between MEATER models (a MEATER+ uses C9E2746C...,
# ESPHome configs list DCBB67CA...), so the link finds the characteristics
# below in whatever service holds them; these are only for spotting adverts.
SERVICE_UUIDS = ("C9E2746C-59F1-4E54-A0DD-E1E54555CF8B",
                 "DCBB67CA-64FB-41A3-99D1-5D9FD8CF33CA")
TEMP_CHAR_UUID = "7EDDA774-045E-4BBF-909B-45D1991A2876"
BATTERY_CHAR_UUID = "2ADB4877-68D8-4884-BD3C-D83853BF27B8"


def _uuid_le(text):
    b = bytes.fromhex(text.replace("-", ""))
    return bytes(b[i] for i in range(len(b) - 1, -1, -1))


SERVICE_UUIDS_LE = tuple(_uuid_le(u) for u in SERVICE_UUIDS)

# Device information reported for a bridged MEATER when the app asks the node
# (0x46-0x48). Shaped like a real Combustion probe's (model "SKU:lot"); the
# firmware matches a current probe so the app doesn't offer an update.
DIS_MODEL = "00001011:MEATER"
DIS_FIRMWARE = "v2.4.0"
DIS_HARDWARE = "v2.16-A1"

LOW_BATTERY_PCT = 20
SURFACE_CODE = 3            # virtual surface T7 (codes 0..3 = T4..T7)
AMBIENT_CODE = 3            # virtual ambient T8 (codes 0..3 = T5..T8)
SERIAL_PREFIX = 0x4D        # 'M': synthesized serials read 4Dxxxxxx


def adv_match(payload):
    """Advertising/scan-response payload -> device name (str, "" if it only
    matched on the MEATER service UUID) or None if it isn't a MEATER.
    Cheap enough for the BLE IRQ."""
    i = 0
    n = len(payload)
    name = None
    uuid_hit = False
    while i + 1 < n:
        length = payload[i]
        if length == 0:
            break
        t = payload[i + 1]
        data = payload[i + 2:i + 1 + length]
        if t in (0x08, 0x09):
            name = bytes(data)
        elif t in (0x06, 0x07):
            for k in range(0, len(data) - 15, 16):
                if bytes(data[k:k + 16]) in SERVICE_UUIDS_LE:
                    uuid_hit = True
        i += 1 + length
    # Case-insensitive "MEATER" prefix without bytes.upper() (not in every
    # MicroPython build): OR-ing 0x20 lower-cases ASCII letters.
    if name is not None and bytes(c | 0x20 for c in name[:6]) == b"meater":
        try:
            return name.decode()
        except Exception:
            return "MEATER"
    if uuid_hit:
        return name.decode() if name else ""
    return None


def decode_temps(data):
    """12-byte temperature notification -> [tip1..tip5, ambient] in C."""
    if len(data) < 12:
        return None
    out = []
    for i in range(6):
        raw = data[2 * i] | data[2 * i + 1] << 8
        out.append((raw + 4.0) / 32.0)
    return out


def map_sensors(tips, ambient):
    """5 tip temps + ambient -> 8 Combustion thermistor temps (T1..T8)."""
    out = []
    span = len(tips) - 1                     # 4 intervals across T1..T7
    for i in range(7):
        x = i * span / 6.0
        lo = int(x)
        if lo >= span:
            out.append(tips[span])
            continue
        f = x - lo
        out.append(tips[lo] + (tips[lo + 1] - tips[lo]) * f)
    out.append(ambient)
    return out


def core_index(temps8):
    """Index (0-5) of the lowest of T1..T6: the virtual core sensor."""
    best = 0
    for i in range(1, 6):
        if temps8[i] < temps8[best]:
            best = i
    return best


def encode_raw13(temps8):
    """8 temps (C) -> 13-byte Combustion raw temperature field (0.05 C,
    -20 C offset, 13 bits each). Returns (bytes, overheating mask) where the
    mask flags sensors clamped at the top of the range."""
    v = 0
    over = 0
    for i, t in enumerate(temps8):
        raw = int(round((t + 20.0) / 0.05))
        if raw > 0x1FFF:
            raw = 0x1FFF
            over |= 1 << i
        elif raw < 0:
            raw = 0
        v |= raw << (13 * i)
    return v.to_bytes(13, "little"), over


def serial_for(addr6):
    """Stable synthesized probe serial (4 bytes LE) from the MEATER address."""
    a = bytes(addr6)
    return bytes((a[5], a[4], a[3], SERIAL_PREFIX))


def addr_str(addr6):
    return ":".join("%02X" % b for b in bytes(addr6))


def addr_bytes(text):
    return bytes(int(x, 16) for x in text.split(":"))


def encode_fields(temps8, probe_id=8, color=0, low_battery=False):
    """Combustion probe fields for a bridged MEATER -> dict with raw13,
    mode (Mode/ID byte), status (battery + virtual sensors byte), vs
    (virtual sensor bits), overheat mask and core index."""
    raw13, over = encode_raw13(temps8)
    core = core_index(temps8)
    vs = core | (SURFACE_CODE << 3) | (AMBIENT_CODE << 5)
    return {
        "raw13": raw13,
        "mode": (((probe_id - 1) & 0x07) << 5) | ((color & 0x07) << 2),
        "status": (1 if low_battery else 0) | (vs << 1),
        "vs": vs,
        "overheat": over,
        "core": core,
        "core_c": temps8[core],
    }


def build_advert(serial4, temps8, probe_id=8, color=0, low_battery=False,
                 hop_count=1):
    """Repeated-probe manufacturer data (with company ID) for a bridged
    MEATER. Returns (msd, core sensor index)."""
    f = encode_fields(temps8, probe_id, color, low_battery)
    body = (bytes((protocol.PRODUCT_TYPE_NODE,)) + bytes(serial4[:4]) + f["raw13"]
            + bytes((f["mode"], f["status"], protocol.network_info(hop_count),
                     f["overheat"])))
    return protocol.VENDOR_LE + body, f["core"]


def build_direct_advert(serial4, temps8, probe_id=8, color=0, low_battery=False):
    """Direct-probe manufacturer data (product type 1, with company ID), the
    advert a probe sends itself. The apps only discover new probes from these;
    repeated (type 2) data only updates probes they already know."""
    f = encode_fields(temps8, probe_id, color, low_battery)
    body = (bytes((protocol.PRODUCT_TYPE_PROBE,)) + bytes(serial4[:4]) + f["raw13"]
            + bytes((f["mode"], f["status"], 0x00, f["overheat"], 0x00)))
    return protocol.VENDOR_LE + body


PREDICTION_STATE_INSERTED = 1


def prediction_status(core_c):
    """7-byte Prediction Status: probe inserted, no prediction set, estimated
    core = the virtual core temperature."""
    est = max(0, min(0x7FF, int(round((core_c + 20.0) / 0.1))))
    v = PREDICTION_STATE_INSERTED | (est << 45)
    return v.to_bytes(7, "little")


def prediction_log(vs):
    """7-byte Prediction Log for a log record: virtual sensors + state."""
    v = (vs & 0x7F) | (PREDICTION_STATE_INSERTED << 7)
    return v.to_bytes(7, "little")


def build_status(fields, log_min, log_max, session_id, sample_period_ms):
    """100-byte Probe Status characteristic value for a bridged MEATER, in
    the same layout a real probe sends (so the node code can forward it as
    0x45 like any linked probe)."""
    s = (log_min.to_bytes(4, "little") + log_max.to_bytes(4, "little")
         + fields["raw13"] + bytes((fields["mode"], fields["status"]))
         + prediction_status(fields["core_c"])
         + b"\x00" * 10 + b"\x00" * 8            # food safe data / status
         + bytes((fields["overheat"], 0x00))       # overheating, preferences
         + b"\x00" * 44                           # high/low alarm arrays
         + session_id.to_bytes(4, "little")
         + sample_period_ms.to_bytes(2, "little"))
    return s


def log_record(fields):
    """20-byte log body (raw temps + prediction log), stored per sample."""
    return fields["raw13"] + prediction_log(fields["vs"])
