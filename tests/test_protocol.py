import struct

import protocol as p


def test_crc_known_vector():
    # CRC-16/CCITT-FALSE check value
    assert p.crc16_ccitt(b"123456789") == 0x29B1


def test_request_roundtrip():
    frame = p.build_request(0x44, b"", request_id=0x11223344)
    assert frame[:2] == b"\xCA\xFE" and len(frame) == 10
    frames, rest = p.split_frames(b"\x00garbage" + frame + b"\xCA")
    assert rest == b"\xCA"
    assert len(frames) == 1
    f = frames[0]
    assert not f.is_response and f.msg_type == 0x44 and f.request_id == 0x11223344


def test_response_roundtrip_and_split_across_chunks():
    frame = p.build_response(0x04, 7, True, b"abc", response_id=9)
    frames, rest = p.split_frames(frame[:8])
    assert frames == [] and rest == frame[:8]
    frames, rest = p.split_frames(rest + frame[8:])
    f = frames[0]
    assert f.is_response and f.request_id == 7 and f.response_id == 9
    assert f.success and f.payload == b"abc" and rest == b""


def test_bad_crc_is_skipped():
    frame = bytearray(p.build_request(0x44, b"x", 1))
    frame[-1] ^= 0xFF
    good = p.build_request(0x42, b"\x00", 2)
    frames, _ = p.split_frames(bytes(frame) + good)
    assert [f.msg_type for f in frames] == [0x42]


def test_probe_frames():
    req = p.build_probe_request(0x05, b"\x01\x02")
    assert len(req) == 8 and req[4] == 0x05 and req[5] == 2
    assert p.crc16_ccitt(req[4:]) == struct.unpack_from("<H", req, 2)[0]
    rsp = p.build_probe_response(0x04, True, b"\x01\x00\x00\x00" + b"\x00" * 20)
    frames, rest = p.split_probe_frames(rsp + rsp[:3])
    assert frames == [(0x04, True, b"\x01\x00\x00\x00" + b"\x00" * 20)]
    assert rest == rsp[:3]


def test_node_probe_translation():
    serial = bytes((0xCD, 0xAB, 0x00, 0x10))
    assert p.probe_serial_str(serial) == "1000ABCD"
    assert p.probe_serial_bytes("1000ABCD") == serial
    s, payload = p.node_to_probe(0x04, serial + struct.pack("<II", 5, 9))
    assert s == serial and payload == struct.pack("<II", 5, 9)
    assert p.node_to_probe(0x44, serial) is None
    assert p.probe_to_node_payload(serial, b"") == b""
    assert p.probe_to_node_payload(serial, b"\x01") == serial + b"\x01"


def _raw13(temps_c):
    v = 0
    for i, t in enumerate(temps_c):
        v |= (int(round((t + 20) / 0.05)) & 0x1FFF) << (13 * i)
    return v.to_bytes(13, "little")


def _probe_body(serial, temps, mode_byte=0, vs_byte=0):
    return (bytes((1,)) + serial + _raw13(temps) + bytes((mode_byte, vs_byte))
            + b"\x00\x00\x00")


def test_probe_advert_decode_and_repeat():
    serial = b"\x01\x02\x03\x04"
    temps = [50, 51, 52, 53, 54, 55, 56, 57]
    # core = T2 (1), surface = T5 (index 4 -> code 1), ambient = T8 (code 3)
    vs = (1 | (1 << 3) | (3 << 5)) << 1
    body = _probe_body(serial, temps, mode_byte=(2 << 5) | (1 << 2), vs_byte=vs)
    d = p.parse_probe_advert(body)
    assert d["serial"] == serial and d["probe_id"] == 3 and d["color"] == 1
    assert abs(d["core_c"] - 51) < 0.06
    assert abs(d["surface_c"] - 54) < 0.06
    assert abs(d["ambient_c"] - 57) < 0.06
    msd = p.repeat_probe_advert(body, 2)
    assert msd[:2] == p.VENDOR_LE and msd[2] == 2 and len(msd) == 24
    rd = p.parse_probe_advert(msd[2:])
    assert rd["hops"] == 2 and abs(rd["core_c"] - 51) < 0.06


def test_display_advert_layout():
    msd = p.build_display_advertisement("ABCDEFGHIJ")
    assert msd[:2] == b"\xC7\x09" and msd[2] == 4
    assert msd[3:13] == b"ABCDEFGHIJ" and msd[13] == 1
    assert len(msd) == 2 + 1 + 10 + 1 + 11


def _status(serial_temps=None, session=0x01020304, period=1000):
    s = struct.pack("<II", 10, 20) + _raw13([60] * 8) + b"\x00\x00"
    s += b"\x00" * 7 + b"\x00" * 10 + b"\x00" * 8   # prediction, food safe
    s += b"\x00\x00" + b"\x00" * 44                 # overheat, prefs, alarms
    s += struct.pack("<IH", session, period)
    assert len(s) == 100
    return s


def test_node_probe_status_layout():
    serial = b"\x01\x02\x03\x04"
    status = _status()
    frame = p.build_node_probe_status(serial, status, hop_count=1)
    f = p.split_frames(frame)[0][0]
    assert f.msg_type == 0x45 and len(f.payload) == 105
    assert f.payload[0:4] == serial
    assert f.payload[52] == 0              # 1 hop
    s4, back, hops = p.parse_node_probe_status(f.payload)
    assert s4 == serial and back == status and hops == 1
    bumped = p.split_frames(p.bump_node_probe_status(f))[0][0]
    assert bumped.payload[52] == 1 and bumped.request_id == f.request_id
    d = p.decode_probe_status(status)
    assert d["log_min"] == 10 and d["log_max"] == 20
    assert d["session_id"] == 0x01020304 and d["sample_period"] == 1000


def test_prediction_decode():
    v = 3 | (1 << 4) | (1 << 6) | (650 << 8) | (50 << 18) | (1234 << 28) | (700 << 45)
    d = p.decode_prediction(v.to_bytes(7, "little"))
    assert d["state"] == 3 and d["mode"] == 1 and d["type"] == 1
    assert abs(d["setpoint_c"] - 65.0) < 1e-6 and d["seconds"] == 1234
    assert abs(d["est_core_c"] - 50.0) < 1e-6


def test_probe_list_and_node_list():
    s = b"\x01\x02\x03\x04"
    f = p.split_frames(p.build_read_probe_list_response(5, [None, s]))[0][0]
    assert len(f.payload) == 50 and f.payload[5] == 2 and f.payload[6:10] == s
    nodes = [(20, 4, "ME00000001")] + [(21 + i, 5, "B%09d" % i) for i in range(6)]
    f = p.split_frames(p.build_read_node_list_response(1, 0, nodes))[0][0]
    assert f.payload[0:4] == bytes((0, 2, 7, 5)) and len(f.payload) == 4 + 5 * 12
    f = p.split_frames(p.build_read_node_list_response(1, 1, nodes))[0][0]
    assert f.payload[3] == 2


def test_topology_layout():
    f = p.split_frames(p.build_topology_response(
        1, 20, 4, "ME00000001", [], [(1, -60), (21, -70)]))[0][0]
    assert len(f.payload) == 12 + 1 + 8 + 1 + 8
    assert f.payload[12] == 0 and f.payload[21] == 2
    assert f.payload[22] == 1 and f.payload[23] == (-60) & 0xFF


def test_heartbeat_roundtrip_and_bump():
    frame = p.build_heartbeat("ME00000001", b"\x01\x02\x03\x04\x05\x06", 4, False,
                              [(1, b"\x01\x02\x03\x04", -55), (5, "BOOSTER001", -70)])
    f = p.split_frames(frame)[0][0]
    assert len(f.payload) == 19 + 52
    d = p.parse_heartbeat(f.payload)
    assert d["serial"] == b"ME00000001" and d["ptype"] == 4 and not d["inbound"]
    assert d["records"][0] == (1, b"\x01\x02\x03\x04", -55)
    assert d["records"][1] == (5, b"BOOSTER001", -70)
    b = p.split_frames(p.bump_heartbeat(f))[0][0]
    assert b.payload[17] == 1


def test_sync_thermometer_list():
    f = p.split_frames(p.build_sync_thermometer_list(
        b"\x01" * 6, [b"\x01\x02\x03\x04", None]))[0][0]
    assert len(f.payload) == 26
    mac, slots = p.parse_sync_thermometer_list(f.payload)
    assert slots == [b"\x01\x02\x03\x04", None, None, None]


def test_link_entries_roundtrip():
    entries = [{"ptype": 1, "serial": b"\x01\x02\x03\x04", "ts": 5},
               {"ptype": 4, "serial": b"DISPLAY001", "ts": 6}] * 3
    f = p.split_frames(p.build_link_entries_response(0x73, 1, 1, entries))[0][0]
    page, total, got = p.parse_link_entries_response(f.payload)
    assert (page, total) == (1, 2) and len(got) == 2
    assert got[0]["serial"] == b"\x01\x02\x03\x04" and got[1]["serial"] == b"DISPLAY001"
    raw = p.encode_product_serial(1, b"\x01\x02\x03\x04")
    assert raw == b"\x01\x02\x03\x04" + b"\xFF" * 8


def test_gauge_and_engine_adverts():
    gauge = struct.pack("<B10sHBBIB3s", 3, b"GAUGE00001", int((100 + 20) / 0.1),
                        1, 0, 0, 0, b"\x00" * 3)
    d = p.parse_node_advert(gauge)
    assert d["ptype"] == 3 and abs(d["temp_c"] - 100) < 0.06
    eng = bytes((6,)) + b"ENGINE0001" + struct.pack("<H", int((110 + 20) / 0.1)) + b"\x01\x00"
    d = p.parse_node_advert(eng)
    assert d["ptype"] == 6 and abs(d["setpoint_c"] - 110) < 0.06 and d["app_mode"]


def test_probe_frame_captured_from_real_probe():
    # Read Session Information response from probe 10018F16: no response bit
    # in the type, CRC over type/success/length/payload.
    raw = bytes.fromhex("cafe10d3030106ebf05b618813")
    frames, rest = p.split_probe_frames(raw)
    assert rest == b""
    (mtype, ok, payload), = frames
    assert mtype == 0x03 and ok
    session, period = struct.unpack("<IH", payload)
    assert session == 0x615BF0EB and period == 5000
