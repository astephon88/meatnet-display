"""Combustion MeatNet BLE wire formats (MicroPython + CPython).

Advertising data, MeatNet Node UART frames (10/15-byte headers with request
IDs), Probe UART frames (6/7-byte headers, no IDs), and the payloads a
Display/repeater node sends and receives. No dataclasses/typing/enum so it
runs on MicroPython; the host tests import it under CPython.
"""

import random
import struct

VENDOR_ID = 0x09C7
VENDOR_LE = b"\xC7\x09"

PRODUCT_TYPE_UNKNOWN = 0
PRODUCT_TYPE_PROBE = 1
PRODUCT_TYPE_NODE = 2        # repeated probe data in advertisements
PRODUCT_TYPE_GAUGE = 3
PRODUCT_TYPE_DISPLAY = 4
PRODUCT_TYPE_BOOSTER = 5
PRODUCT_TYPE_ENGINE = 6

PRODUCT_NAMES = {1: "Probe", 2: "Repeater", 3: "Gauge", 4: "Display",
                 5: "Booster", 6: "Engine"}

UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
UART_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
UART_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
PROBE_STATUS_SERVICE_UUID = "00000100-CAAB-3792-3D44-97AE51C1407A"
PROBE_STATUS_CHAR_UUID = "00000101-CAAB-3792-3D44-97AE51C1407A"
PROBE_STATUS_SERVICE_UUID_LE = bytes((
    0x7A, 0x40, 0xC1, 0x51, 0xAE, 0x97, 0x44, 0x3D,
    0x92, 0x37, 0xAB, 0xCA, 0x00, 0x01, 0x00, 0x00))
DIS_SERVICE_UUID = "0000180A-0000-1000-8000-00805F9B34FB"
# Device Information strings a node reads from a linked probe, to answer the
# app's Probe Firmware/Hardware/Model requests (0x46-0x48).
PROBE_DIS_READS = (
    ("firmware", DIS_SERVICE_UUID, "00002A26-0000-1000-8000-00805F9B34FB"),
    ("hardware", DIS_SERVICE_UUID, "00002A27-0000-1000-8000-00805F9B34FB"),
    ("model", DIS_SERVICE_UUID, "00002A24-0000-1000-8000-00805F9B34FB"),
)
# Nordic Secure DFU service (0xFE59) as 128-bit little-endian bytes; the
# Combustion apps filter their scans on it (scan response).
DFU_SERVICE_UUID_LE = bytes((
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x59, 0xFE, 0x00, 0x00))

SYNC_BYTES = b"\xCA\xFE"
RESPONSE_FLAG = 0x80

# Probe-addressed messages (node payload = probe serial(4) + probe payload)
MSG_SET_PROBE_ID = 0x01
MSG_SET_PROBE_COLOR = 0x02
MSG_READ_SESSION_INFO = 0x03
MSG_READ_LOGS = 0x04
MSG_SET_PREDICTION = 0x05
MSG_READ_OVER_TEMPERATURE = 0x06
MSG_CONFIGURE_FOOD_SAFE = 0x07
MSG_RESET_FOOD_SAFE = 0x08
MSG_SET_POWER_MODE = 0x09
MSG_RESET_THERMOMETER = 0x0A
MSG_SET_PROBE_HIGH_LOW_ALARM = 0x0B
MSG_SILENCE_ALARMS = 0x0C
PROBE_ADDRESSED = (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                   0x0A, 0x0B)
# Node messages
MSG_DEVICE_CONNECTED = 0x40
MSG_DEVICE_DISCONNECTED = 0x41
MSG_READ_NODE_LIST = 0x42
MSG_READ_NETWORK_TOPOLOGY = 0x43
MSG_READ_PROBE_LIST = 0x44
MSG_PROBE_STATUS = 0x45
MSG_PROBE_FIRMWARE_REVISION = 0x46
MSG_PROBE_HARDWARE_REVISION = 0x47
MSG_PROBE_MODEL_INFORMATION = 0x48
MSG_HEARTBEAT = 0x49
MSG_ASSOCIATE_NODE = 0x4A
MSG_SYNC_THERMOMETER_LIST = 0x4B
MSG_GAUGE_STATUS = 0x60
MSG_ENGINE_STATUS = 0x70
MSG_READ_LINKED_DEVICES = 0x73
MSG_READ_UNLINKED_DEVICES = 0x74
MSG_LINK_DEVICE = 0x75
MSG_UNLINK_DEVICE = 0x76
MSG_DEVICE_LINKED = 0x77
MSG_DEVICE_UNLINKED = 0x78
MSG_SET_PROXIMITY_LINKING = 0x79
MSG_READ_PROXIMITY_LINKING = 0x7A
MSG_SYNC_LINK_TABLE = 0x7B
MSG_DISPLAY_STATUS = 0x80

PROBE_MODE_NORMAL = 0
PROBE_MODE_INSTANT_READ = 1
PROBE_MODE_ERROR = 3

MAX_HOPS = 4                 # hop count field is 2 bits: 1..4 hops

ZERO4 = b"\x00\x00\x00\x00"
ZERO10 = b"\x00" * 10


# ---------------------------------------------------------------------------
# Basic encodings
# ---------------------------------------------------------------------------

def decode_raw_temperature(raw):
    """13-bit, 0.1 C steps, -20 C offset (Gauge, Engine, alarms)."""
    return (raw & 0x1FFF) * 0.1 - 20.0


def encode_serial(serial):
    """Node serial (str or bytes) -> exactly 10 bytes."""
    if isinstance(serial, str):
        serial = serial.encode()
    data = bytes(serial[:10])
    return data + b"\x00" * (10 - len(data))


def serial10_str(serial10):
    raw = bytes(serial10).split(b"\x00")[0]
    try:
        return raw.decode()
    except Exception:
        return "".join("%02X" % c for c in raw)


def probe_serial_str(serial4):
    """Render a 4-byte little-endian probe serial the way the app does."""
    return "%08X" % (serial4[0] | serial4[1] << 8 | serial4[2] << 16
                     | serial4[3] << 24)


def probe_serial_bytes(text):
    """'1000ABCD' -> 4 bytes little-endian."""
    v = int(text, 16)
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))


def crc16_ccitt(data, initial=0xFFFF):
    crc = initial
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def new_message_id():
    return random.getrandbits(32)


def _pad(b, n):
    b = bytes(b[:n])
    return b + b"\x00" * (n - len(b))


def _bits(value, shift, width):
    return (value >> shift) & ((1 << width) - 1)


# ---------------------------------------------------------------------------
# MeatNet Node UART frames
# ---------------------------------------------------------------------------

def build_request(message_type, payload, request_id=None):
    if request_id is None:
        request_id = new_message_id()
    crc_region = struct.pack("<BIB", message_type & 0x7F, request_id,
                             len(payload)) + bytes(payload)
    return SYNC_BYTES + struct.pack("<H", crc16_ccitt(crc_region)) + crc_region


def build_response(message_type, request_id, success, payload,
                   response_id=None):
    if response_id is None:
        response_id = new_message_id()
    crc_region = struct.pack(
        "<BIIBB", (message_type & 0x7F) | RESPONSE_FLAG, request_id,
        response_id, 1 if success else 0, len(payload)) + bytes(payload)
    return SYNC_BYTES + struct.pack("<H", crc16_ccitt(crc_region)) + crc_region


class Frame:
    """One parsed MeatNet Node UART frame."""

    def __init__(self, is_response, msg_type, request_id, response_id,
                 success, payload, raw):
        self.is_response = is_response
        self.msg_type = msg_type
        self.request_id = request_id
        self.response_id = response_id
        self.success = success
        self.payload = payload
        self.raw = raw

    def __repr__(self):
        return "<Frame %s 0x%02x id=%08x len=%d>" % (
            "rsp" if self.is_response else "req", self.msg_type,
            self.request_id, len(self.payload))


def split_frames(buf):
    """Pull complete, CRC-valid node frames out of a UART byte stream.

    Returns (frames, remainder). Garbage and CRC failures are skipped by
    resynchronising on the next sync bytes.
    """
    buf = bytes(buf)
    frames = []
    i = 0
    n = len(buf)
    while True:
        i = buf.find(SYNC_BYTES, i)
        if i < 0:
            # keep a trailing 0xCA in case it's the first half of a sync
            return frames, (buf[-1:] if n and buf[-1] == 0xCA else b"")
        if i + 5 > n:
            return frames, buf[i:]
        mtype = buf[i + 4]
        is_resp = bool(mtype & RESPONSE_FLAG)
        hdr = 15 if is_resp else 10
        if i + hdr > n:
            return frames, buf[i:]
        length = buf[i + hdr - 1]
        end = i + hdr + length
        if end > n:
            return frames, buf[i:]
        crc = buf[i + 2] | buf[i + 3] << 8
        if crc16_ccitt(buf[i + 4:end]) != crc:
            i += 1          # bad frame: resync past this sync byte
            continue
        req_id = struct.unpack_from("<I", buf, i + 5)[0]
        if is_resp:
            resp_id = struct.unpack_from("<I", buf, i + 9)[0]
            success = bool(buf[i + 13])
        else:
            resp_id = None
            success = True
        frames.append(Frame(is_resp, mtype & 0x7F, req_id, resp_id, success,
                            buf[i + hdr:end], buf[i:end]))
        i = end


def with_request_payload(frame, payload):
    """Re-encode a request frame with a new payload, keeping its ID."""
    return build_request(frame.msg_type, payload, frame.request_id)


# ---------------------------------------------------------------------------
# Probe UART frames (direct link to a probe: no request/response IDs)
# ---------------------------------------------------------------------------

def build_probe_request(message_type, payload=b""):
    crc_region = bytes((message_type & 0x7F, len(payload))) + bytes(payload)
    return SYNC_BYTES + struct.pack("<H", crc16_ccitt(crc_region)) + crc_region


def build_probe_response(message_type, success, payload=b""):
    """Probe-side response (used by the host tests' fake probe). Like a real
    probe, it doesn't set the response bit in the message type."""
    crc_region = (bytes((message_type & 0x7F, 1 if success else 0,
                         len(payload))) + bytes(payload))
    return SYNC_BYTES + struct.pack("<H", crc16_ccitt(crc_region)) + crc_region


def split_probe_frames(buf):
    """Probe UART stream -> ([(msg_type, success, payload)], remainder).

    Probes only send responses: 7-byte header (sync, CRC, type, success,
    length). Real probes leave the response bit of the type clear, and the
    CRC covers type, success, length and payload (checked against a capture
    from a probe).
    """
    buf = bytes(buf)
    out = []
    i = 0
    n = len(buf)
    while True:
        i = buf.find(SYNC_BYTES, i)
        if i < 0:
            return out, (buf[-1:] if n and buf[-1] == 0xCA else b"")
        if i + 7 > n:
            return out, buf[i:]
        mtype = buf[i + 4]
        length = buf[i + 6]
        end = i + 7 + length
        if end > n:
            return out, buf[i:]
        crc = buf[i + 2] | buf[i + 3] << 8
        if crc16_ccitt(buf[i + 4:end]) != crc:
            i += 1
            continue
        out.append((mtype & 0x7F, bool(buf[i + 5]), buf[i + 7:end]))
        i = end


def node_to_probe(msg_type, payload):
    """Node request for a probe -> (probe serial4, probe payload) or None."""
    if msg_type not in PROBE_ADDRESSED or len(payload) < 4:
        return None
    return bytes(payload[0:4]), bytes(payload[4:])


def probe_to_node_payload(serial4, probe_payload):
    """Probe response payload -> node response payload (serial prefixed when
    the response has a payload at all)."""
    if not probe_payload:
        return b""
    return bytes(serial4) + bytes(probe_payload)


# ---------------------------------------------------------------------------
# Advertising
# ---------------------------------------------------------------------------

def build_display_advertisement(serial, high_radio_power=True):
    """Display device-info manufacturer data (with company ID)."""
    return (VENDOR_LE + bytes((PRODUCT_TYPE_DISPLAY,)) + encode_serial(serial)
            + bytes((0x01 if high_radio_power else 0x00,)) + b"\x00" * 11)


def network_info(hop_count):
    """Hop count (1..4) -> Network Information byte."""
    return (max(1, min(MAX_HOPS, hop_count)) - 1) & 0x03


def hops_from_network_info(b):
    return (b & 0x03) + 1


def repeat_probe_advert(body, hop_count=1):
    """Probe advert body (no company ID) -> repeated-node manufacturer data
    (with company ID), product type 2 and the given hop count."""
    b = bytearray(body[:22])
    while len(b) < 22:
        b.append(0)
    b[0] = PRODUCT_TYPE_NODE
    b[20] = network_info(hop_count)
    return VENDOR_LE + bytes(b)


def probe_advert_from_status(serial4, status, hop_count=1):
    """Build repeated-node manufacturer data from a probe status
    characteristic value (for a linked probe whose advert we missed)."""
    body = (bytes((PRODUCT_TYPE_NODE,)) + bytes(serial4[:4])
            + bytes(status[8:23]) + bytes((network_info(hop_count),
                                           status[48] if len(status) > 48 else 0)))
    return VENDOR_LE + body


def manufacturer_data(adv_payload):
    """Raw advertising payload -> Combustion manufacturer body (no company
    ID) or None."""
    i = 0
    n = len(adv_payload)
    while i + 1 < n:
        length = adv_payload[i]
        if length == 0:
            break
        if adv_payload[i + 1] == 0xFF and length >= 4:
            ad = adv_payload[i + 2:i + 1 + length]
            if ad[:2] == VENDOR_LE and len(ad) > 2:
                return bytes(ad[2:])
        i += 1 + length
    return None


# ---------------------------------------------------------------------------
# Probe data decoding
# ---------------------------------------------------------------------------

def unpack_probe_temps(raw):
    """13 bytes -> 8 thermistor temps (C). LSB-first 13-bit packing, 0.05 C."""
    v = int.from_bytes(bytes(raw[:13]), "little")
    return [((v >> (13 * i)) & 0x1FFF) * 0.05 - 20.0 for i in range(8)]


def decode_probe(raw13, mode_byte, status_byte):
    """Decode probe temps + virtual sensors. Core/surface/ambient are None
    when the probe is not in Normal mode (e.g. Instant Read)."""
    temps = unpack_probe_temps(raw13)
    mode = mode_byte & 0x03
    vs = (status_byte & 0xFE) >> 1
    core_i = vs & 0x07          # 0..5 -> T1..T6
    surf_i = 3 + ((vs >> 3) & 0x03)   # T4..T7
    amb_i = 4 + ((vs >> 5) & 0x03)    # T5..T8
    ok = mode == PROBE_MODE_NORMAL and core_i <= 5
    return {
        "mode": mode,
        "color": (mode_byte >> 2) & 0x07,
        "probe_id": ((mode_byte >> 5) & 0x07) + 1,
        "temps": temps,
        "instant_c": temps[0] if mode == PROBE_MODE_INSTANT_READ else None,
        "core_sensor": core_i + 1,
        "core_c": temps[core_i] if ok else None,
        "surface_c": temps[surf_i] if ok else None,
        "ambient_c": temps[amb_i] if ok else None,
        "low_battery": bool(status_byte & 0x01),
    }


def parse_probe_advert(body):
    """Probe (type 1) or repeated-probe node (type 2) advert body -> dict
    or None. "hops" is the hop count as received."""
    if len(body) < 20 or body[0] not in (PRODUCT_TYPE_PROBE, PRODUCT_TYPE_NODE):
        return None
    serial = bytes(body[1:5])
    if serial == ZERO4:          # a repeater with no probe connected
        return None
    d = decode_probe(body[5:18], body[18], body[19])
    d["serial"] = serial
    d["ptype"] = body[0]
    d["hops"] = (hops_from_network_info(body[20])
                 if body[0] == PRODUCT_TYPE_NODE and len(body) > 20 else 0)
    d["overheating"] = body[21] if len(body) > 21 else 0
    return d


def decode_prediction(b7):
    v = int.from_bytes(bytes(b7[:7]), "little")
    state = _bits(v, 0, 4)
    return {
        "state": state,
        "mode": _bits(v, 4, 2),
        "type": _bits(v, 6, 2),
        "setpoint_c": _bits(v, 8, 10) * 0.1,
        "heat_start_c": _bits(v, 18, 10) * 0.1,
        "seconds": _bits(v, 28, 17),
        "est_core_c": _bits(v, 45, 11) * 0.1 - 20.0,
    }


PREDICTION_STATES = ("not inserted", "inserted", "warming", "predicting",
                     "removal done")


def decode_alarm(value):
    return {"set": bool(value & 1), "tripped": bool(value & 2),
            "alarming": bool(value & 4),
            "temp_c": decode_raw_temperature(value >> 3)}


def decode_probe_status(status):
    """Probe Status characteristic value (94 or 100 bytes) -> dict."""
    if len(status) < 48:
        return None
    lo, hi = struct.unpack_from("<II", status, 0)
    d = decode_probe(status[8:21], status[21], status[22])
    d["log_min"] = lo
    d["log_max"] = hi
    d["prediction"] = decode_prediction(status[23:30])
    d["food_safe_state"] = status[40] & 0x07
    d["overheating"] = status[48] if len(status) > 48 else 0
    alarming = False
    if len(status) >= 94:
        for off in range(50, 94, 2):
            if struct.unpack_from("<H", status, off)[0] & 0x04:
                alarming = True
                break
    d["alarming"] = alarming
    if len(status) >= 100:
        d["session_id"], d["sample_period"] = struct.unpack_from("<IH", status, 94)
    else:
        d["session_id"], d["sample_period"] = 0, 0
    return d


def build_node_probe_status(serial4, status, hop_count=1, request_id=None):
    """Probe Status characteristic value -> Node Probe Status (0x45) frame.

    Node payload = Serial(4) + [LogRange..FoodSafeStatus (48)] + NetworkInfo
    + [Overheating..LowAlarm (46)] + SessionID(4) + SamplePeriod(2) = 105.
    """
    s = bytes(status)
    tail = s[94:100] if len(s) >= 100 else b"\x00" * 6
    payload = (bytes(serial4[:4]) + s[0:48] + bytes((network_info(hop_count),))
               + s[48:94] + tail)
    return build_request(MSG_PROBE_STATUS, payload, request_id)


def node_probe_status_hops(payload):
    """Hop count carried in a 0x45 payload (or None)."""
    if len(payload) < 53:
        return None
    return hops_from_network_info(payload[52])


def bump_node_probe_status(frame):
    """Forward a received 0x45 one more hop. None if already at max hops."""
    p = bytearray(frame.payload)
    if len(p) < 53:
        return None
    hops = hops_from_network_info(p[52])
    if hops >= MAX_HOPS:
        return None
    p[52] = network_info(hops + 1)
    return build_request(MSG_PROBE_STATUS, bytes(p), frame.request_id)


def parse_node_probe_status(payload):
    """0x45 payload -> (serial4, status-characteristic-shaped bytes, hops)."""
    if len(payload) < 99:
        return None
    p = bytes(payload)
    status = p[4:52] + p[53:99] + (p[99:105] if len(p) >= 105 else b"")
    return p[0:4], status, hops_from_network_info(p[52])


# ---------------------------------------------------------------------------
# Other devices' advertisements
# ---------------------------------------------------------------------------

def parse_gauge_advert(body):
    """Gauge (type 3): [1:11] serial, [11:13] raw temp, [13] status flags."""
    if len(body) < 14 or body[0] != PRODUCT_TYPE_GAUGE:
        return None
    flags = body[13]
    present = bool(flags & 0x01)
    raw = struct.unpack_from("<H", body, 11)[0]
    return {
        "serial": bytes(body[1:11]),
        "ptype": PRODUCT_TYPE_GAUGE,
        "temp_c": decode_raw_temperature(raw) if present else None,
        "sensor_present": present,
        "overheating": bool(flags & 0x02),
        "low_battery": bool(flags & 0x04),
    }


def parse_engine_advert(body):
    """Engine (type 6): [1:11] serial, [11:13] set point, [13] flags."""
    if len(body) < 15 or body[0] != PRODUCT_TYPE_ENGINE:
        return None
    flags = body[13]
    return {
        "serial": bytes(body[1:11]),
        "ptype": PRODUCT_TYPE_ENGINE,
        "setpoint_c": decode_raw_temperature(struct.unpack_from("<H", body, 11)[0]),
        "app_mode": bool(flags & 0x01),
        "ctrl_connected": bool(flags & 0x02),
        "lid_open": bool(flags & 0x04),
    }


def parse_node_advert(body):
    """Any non-probe device-info advert (Gauge/Display/Booster/Engine)."""
    if len(body) < 11:
        return None
    t = body[0]
    if t == PRODUCT_TYPE_GAUGE:
        return parse_gauge_advert(body)
    if t == PRODUCT_TYPE_ENGINE:
        return parse_engine_advert(body)
    if t in (PRODUCT_TYPE_DISPLAY, PRODUCT_TYPE_BOOSTER):
        return {"serial": bytes(body[1:11]), "ptype": t}
    return None


# ---------------------------------------------------------------------------
# Node message payloads
# ---------------------------------------------------------------------------

def build_read_probe_list_response(request_id, slots):
    """Read Probe List (0x44) response. slots: 10 entries, serial4 or None;
    the device number is the slot index + 1."""
    payload = b""
    for i in range(10):
        serial = slots[i] if i < len(slots) and slots[i] else ZERO4
        payload += bytes((i + 1,)) + bytes(serial[:4])
    return build_response(MSG_READ_PROBE_LIST, request_id, True, payload)


def build_read_node_list_response(request_id, page, nodes):
    """Read Node List (0x42). nodes: [(device#, product type, serial10)]."""
    per = 5
    total_pages = max(1, (len(nodes) + per - 1) // per)
    chunk = nodes[page * per:(page + 1) * per]
    payload = bytes((page, total_pages, len(nodes), len(chunk)))
    for num, ptype, serial in chunk:
        payload += bytes((num, ptype)) + encode_serial(serial)
    return build_response(MSG_READ_NODE_LIST, request_id, page < total_pages,
                          payload)


def _int8(v):
    return max(-128, min(127, int(v))) & 0xFF


def build_topology_response(request_id, device_number, ptype, serial,
                            inbound, outbound):
    """Read Network Topology (0x43). inbound/outbound: [(device#, rssi)]."""
    payload = bytes((device_number, ptype)) + encode_serial(serial)
    for conns in (inbound, outbound):
        conns = conns[:4]
        payload += bytes((len(conns),))
        for i in range(4):
            if i < len(conns):
                payload += bytes((conns[i][0], _int8(conns[i][1])))
            else:
                payload += b"\x00\x00"
    return build_response(MSG_READ_NETWORK_TOPOLOGY, request_id, True, payload)


def heartbeat_serial(ptype, serial):
    """Serial field of a heartbeat record: probe uint32 + 6 zero bytes, or a
    10-byte node serial."""
    if ptype == PRODUCT_TYPE_PROBE:
        return bytes(serial[:4]) + b"\x00" * 6
    return encode_serial(serial)


def build_heartbeat(serial, mac6, ptype, inbound, records, hop_count=0,
                    request_id=None):
    """Heartbeat (0x49). records: [(ptype, serial, rssi)] (max 4)."""
    payload = (encode_serial(serial) + _pad(mac6, 6)
               + bytes((ptype, hop_count, 1 if inbound else 0)))
    for i in range(4):
        if i < len(records):
            t, s, rssi = records[i]
            payload += heartbeat_serial(t, s) + bytes((t, 0x01, _int8(rssi)))
        else:
            payload += b"\x00" * 13
    return build_request(MSG_HEARTBEAT, payload, request_id)


def parse_heartbeat(payload):
    if len(payload) < 19:
        return None
    d = {"serial": bytes(payload[0:10]), "mac": bytes(payload[10:16]),
         "ptype": payload[16], "hops": payload[17],
         "inbound": bool(payload[18]), "records": []}
    for i in range(4):
        off = 19 + 13 * i
        if off + 13 > len(payload):
            break
        if payload[off + 11] & 0x01:
            t = payload[off + 10]
            s = payload[off:off + 4] if t == PRODUCT_TYPE_PROBE else payload[off:off + 10]
            rssi = payload[off + 12]
            d["records"].append((t, bytes(s), rssi - 256 if rssi > 127 else rssi))
    return d


def bump_heartbeat(frame):
    """Forward a heartbeat one more hop, or None at max hops."""
    p = bytearray(frame.payload)
    if len(p) < 19 or p[17] + 1 >= MAX_HOPS:
        return None
    p[17] += 1
    return build_request(MSG_HEARTBEAT, bytes(p), frame.request_id)


def build_device_connected(connected, ptype, serial):
    """Device Connected (0x40) / Disconnected (0x41)."""
    if ptype == PRODUCT_TYPE_PROBE:
        payload = bytes((ptype,)) + bytes(serial[:4]) + ZERO10
    else:
        payload = bytes((ptype,)) + ZERO4 + encode_serial(serial)
    return build_request(MSG_DEVICE_CONNECTED if connected
                         else MSG_DEVICE_DISCONNECTED, payload)


def build_sync_thermometer_list(mac6, slots, request_id=None):
    """Sync Thermometer List (0x4B): MAC + 4 x (present, serial4)."""
    payload = _pad(mac6, 6)
    for i in range(4):
        s = slots[i] if i < len(slots) else None
        payload += (b"\x01" + bytes(s[:4])) if s else b"\x00" + ZERO4
    return build_request(MSG_SYNC_THERMOMETER_LIST, payload, request_id)


def parse_sync_thermometer_list(payload):
    """-> (mac6, [serial4 or None] * 4) or None."""
    if len(payload) < 26:
        return None
    slots = []
    for i in range(4):
        off = 6 + 5 * i
        slots.append(bytes(payload[off + 1:off + 5]) if payload[off] else None)
    return bytes(payload[0:6]), slots


def build_silence_alarms_response(request_id, ptype, serial):
    if ptype == PRODUCT_TYPE_PROBE:
        payload = bytes((ptype,)) + bytes(serial[:4]) + ZERO10
    else:
        payload = bytes((ptype,)) + ZERO4 + encode_serial(serial)
    return build_response(MSG_SILENCE_ALARMS, request_id, True, payload)


def parse_silence_alarms(payload):
    """-> (global, ptype, serial4, serial10)."""
    p = bytes(payload) + b"\x00" * 16
    return bool(p[0]), p[1], p[2:6], p[6:16]


# ---------------------------------------------------------------------------
# Link tables (0x73-0x7B)
# ---------------------------------------------------------------------------

def encode_product_serial(ptype, serial):
    """12-byte Product Serial Number union."""
    if ptype == PRODUCT_TYPE_PROBE:
        return bytes(serial[:4]) + b"\xFF" * 8
    return encode_serial(serial) + b"\x00\x00"


def decode_product_serial(ptype, b12):
    if ptype == PRODUCT_TYPE_PROBE:
        return bytes(b12[0:4])
    return bytes(b12[0:10])


def build_link_entries_response(msg_type, request_id, page, entries):
    """Read Linked/Unlinked Devices response. entries: list of dicts with
    ptype, serial, ts, legacy."""
    per = 4
    total_pages = max(1, (len(entries) + per - 1) // per)
    chunk = entries[page * per:(page + 1) * per]
    payload = bytes((page, total_pages, len(entries), len(chunk)))
    for e in chunk:
        payload += (bytes((e["ptype"],)) + encode_product_serial(e["ptype"], e["serial"])
                    + struct.pack("<I", e.get("ts", 0) & 0xFFFFFFFF)
                    + bytes((1 if e.get("legacy") else 0,)))
    return build_response(msg_type, request_id, page < total_pages, payload)


def parse_link_entries_response(payload):
    """-> (page, total_pages, [entry dicts]) or None."""
    if len(payload) < 4:
        return None
    page, total, _count, on_page = payload[0], payload[1], payload[2], payload[3]
    out = []
    for i in range(on_page):
        off = 4 + 18 * i
        if off + 18 > len(payload):
            break
        t = payload[off]
        out.append({"ptype": t,
                    "serial": decode_product_serial(t, payload[off + 1:off + 13]),
                    "ts": struct.unpack_from("<I", payload, off + 13)[0],
                    "legacy": bool(payload[off + 17])})
    return page, total, out


def parse_link_device(payload):
    """Link/Unlink/Linked/Unlinked payload -> entry dict or None."""
    if len(payload) < 17:
        return None
    t = payload[0]
    return {"ptype": t, "serial": decode_product_serial(t, payload[1:13]),
            "ts": struct.unpack_from("<I", payload, 13)[0], "legacy": False}


def build_link_notification(linked, entry):
    payload = (bytes((entry["ptype"],))
               + encode_product_serial(entry["ptype"], entry["serial"])
               + struct.pack("<I", entry.get("ts", 0) & 0xFFFFFFFF))
    return build_request(MSG_DEVICE_LINKED if linked else MSG_DEVICE_UNLINKED,
                         payload)
