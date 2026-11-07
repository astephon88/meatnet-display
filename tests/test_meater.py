import struct

import meater as m
import protocol as p


def notification(tips, ambient):
    return struct.pack("<6H", *[int(round(t * 32 - 4)) for t in tips + [ambient]])


def test_decode_matches_receiver_formula():
    data = struct.pack("<6H", 1916, 1948, 1980, 2012, 2044, 3196)
    t = m.decode_temps(data)
    assert t[0] == (1916 + 4) / 32 and t[5] == (3196 + 4) / 32
    assert m.decode_temps(b"\x00" * 11) is None


def test_linear_mapping_endpoints_and_ambient():
    tips = [50.0, 52.0, 54.0, 56.0, 58.0]
    t8 = m.map_sensors(tips, 120.0)
    assert len(t8) == 8
    assert t8[0] == 50.0 and t8[6] == 58.0 and t8[7] == 120.0
    # evenly spaced tips -> evenly spaced T1..T7
    steps = [round(t8[i + 1] - t8[i], 6) for i in range(6)]
    assert all(abs(s - 8.0 / 6) < 1e-6 for s in steps)
    # T4 sits exactly on tip 3
    assert abs(t8[3] - 54.0) < 1e-9


def test_core_is_lowest_of_t1_to_t6():
    t8 = [60, 55, 50, 52, 58, 62, 40, 120]      # T7 lowest but not allowed
    assert m.core_index(t8) == 2


def test_advert_roundtrips_through_combustion_decoder():
    tips = [62.0, 58.5, 55.0, 57.0, 61.0]
    addr = bytes.fromhex("A1B2C3D4E5F6")
    serial = m.serial_for(addr)
    assert p.probe_serial_str(serial) == "4DD4E5F6"
    msd, core = m.build_advert(serial, m.map_sensors(tips, 130.0), probe_id=7,
                               low_battery=True)
    assert msd[:2] == p.VENDOR_LE and len(msd) == 24
    d = p.parse_probe_advert(msd[2:])
    assert d["ptype"] == p.PRODUCT_TYPE_NODE and d["hops"] == 1
    assert d["serial"] == serial and d["probe_id"] == 7 and d["low_battery"]
    t8 = m.map_sensors(tips, 130.0)
    assert d["core_sensor"] == core + 1 and abs(d["core_c"] - min(t8[:6])) < 0.05
    assert abs(d["surface_c"] - t8[6]) < 0.05
    assert abs(d["ambient_c"] - 130.0) < 0.05


def test_adv_match_by_name_and_uuid():
    name = b"MEATER Pro"
    adv = bytes((2, 1, 6, len(name) + 1, 0x09)) + name
    assert m.adv_match(adv) == "MEATER Pro"
    for le in m.SERVICE_UUIDS_LE:
        assert m.adv_match(bytes((17, 0x07)) + le) == ""
    assert m.adv_match(bytes((2, 1, 6, 4, 0x09)) + b"Foo") is None


def test_encode_clamps_and_flags_overheat():
    raw, over = m.encode_raw13([500.0] + [20.0] * 7)
    assert over == 1
    assert abs(p.unpack_probe_temps(raw)[0] - (0x1FFF * 0.05 - 20)) < 1e-6


# -- bridge behaviour in the node ----------------------------------------------

from meatnet import Node  # noqa: E402
from tests.test_meatnet import FakeTransport, hear_probe  # noqa: E402

ADDR = bytes.fromhex("A1B2C3D4E5F6")


def bridged(cfg_extra=None):
    cfg = {"meater_bridge": True,
           "meater_probes": [{"addr": m.addr_str(ADDR), "id": 5}]}
    cfg.update(cfg_extra or {})
    t = FakeTransport()
    n = Node(t, cfg, "ME00000001", log=lambda *a: None)
    t.meaters[ADDR] = ("MEATER Pro", 0, 1, -58)
    n.tick(now=0)
    return n, t


def test_bridge_scans_actively_and_links_selected_meater():
    n, t = bridged()
    assert t.scan_active
    link = t.links[ADDR]
    assert link.kind == "meater"
    n.cfg["meater_bridge"] = False
    n.tick(now=100)
    assert ADDR not in t.links and not t.scan_active


def test_bridged_meater_is_advertised_and_listed():
    n, t = bridged()
    link = t.links[ADDR]
    link.state = "ready"
    link.rx["temp"] = [notification([60.0, 57.0, 54.0, 56.0, 59.0], 140.0)]
    link.rx["battery"] = [b"\x0f"]                 # 15% -> low battery
    n.service(now=1000)
    n.tick(now=1500)
    payloads, _ = t.adv
    serial = m.serial_for(ADDR)
    ours = [x for x in payloads if x[3:7] == serial]
    assert len(ours) == 1
    d = p.parse_probe_advert(ours[0][2:])
    assert d["probe_id"] == 5 and d["low_battery"]
    assert abs(d["ambient_c"] - 140.0) < 0.05
    probe = n.probes[serial]
    assert probe["src"] == "meater" and abs(probe["core_c"] - 54.0) < 0.05
    st = n.status()
    assert st["meaters"][0]["bridged"] and st["meaters"][0]["battery"] == 15
    # goes quiet -> stops being advertised
    n.tick(now=1000 + 11000)
    assert not [x for x in t.adv[0] if x[3:7] == serial]


def test_meater_shares_the_link_budget_with_probes():
    n, t = bridged({"linked_probes": ["04030201"], "max_conn": 4,
                    "inbound_reserve": 3})
    hear_probe(t)
    t.links.clear()
    n.tick(now=10)
    assert len(t.links) == 1                    # budget 1: probe wins, MEATER waits


def test_status_matches_probe_status_layout():
    t8 = m.map_sensors([60.0, 57.0, 54.0, 56.0, 59.0], 140.0)
    f = m.encode_fields(t8, probe_id=3)
    st = m.build_status(f, 5, 42, 0xA1B2C3D4, 5000)
    assert len(st) == 100
    d = p.decode_probe_status(st)
    assert d["log_min"] == 5 and d["log_max"] == 42
    assert d["probe_id"] == 3 and abs(d["core_c"] - 54.0) < 0.05
    assert d["session_id"] == 0xA1B2C3D4 and d["sample_period"] == 5000
    assert d["prediction"]["state"] == 1 and abs(d["prediction"]["est_core_c"] - 54.0) < 0.06
    rec = m.log_record(f)
    assert len(rec) == 20 and rec[:13] == f["raw13"]


def ready_bridge():
    n, t = bridged()
    link = t.links[ADDR]
    link.state = "ready"
    return n, t, link


def feed(n, link, now, core=54.0):
    link.rx["temp"] = [notification([60.0, 57.0, core, 56.0, 59.0], 140.0)]
    n.service(now=now)


def test_meater_appears_as_node_probe_with_status():
    n, t, link = ready_bridge()
    t.frames()
    feed(n, link, 1000)
    serial = m.serial_for(ADDR)
    st = [f for f in t.frames() if f.msg_type == p.MSG_PROBE_STATUS]
    assert len(st) == 1 and st[0].payload[0:4] == serial and len(st[0].payload) == 105
    s4, status, hops = p.parse_node_probe_status(st[0].payload)
    d = p.decode_probe_status(status)
    assert hops == 1 and abs(d["core_c"] - 54.0) < 0.05 and d["session_id"] != 0
    t.rx.append((1, p.build_request(p.MSG_READ_PROBE_LIST, b"", 5)))
    n.service(now=1100)
    (f,) = t.frames()
    assert serial in [f.payload[i + 1:i + 5] for i in range(0, 50, 5)]


def test_session_info_and_paced_log_read():
    n, t, link = ready_bridge()
    for k in range(30):                       # 30 samples, 5 s apart
        feed(n, link, 1000 + k * 5000, core=40.0 + k)
    t.frames()
    serial = m.serial_for(ADDR)
    t.rx.append((1, p.build_request(p.MSG_READ_SESSION_INFO, serial, 6)))
    n.service(now=146000)
    (f,) = t.frames()
    s4, session, period = struct.unpack("<4sIH", f.payload)
    assert f.success and s4 == serial and session != 0 and period == 5000
    t.rx.append((1, p.build_request(p.MSG_READ_LOGS, serial + struct.pack("<II", 0, 29), 7)))
    n.service(now=146100)
    first = [f for f in t.frames() if f.msg_type == p.MSG_READ_LOGS]
    assert len(first) == 20                   # paced: one burst per service()
    n.service(now=146200)
    rest = [f for f in t.frames() if f.msg_type == p.MSG_READ_LOGS]
    recs = first + rest
    assert [struct.unpack_from("<I", f.payload, 4)[0] for f in recs] == list(range(30))
    temps = p.unpack_probe_temps(recs[29].payload[8:21])
    assert abs(temps[3] - 69.0) < 0.06         # T4 = tip 3 of the last sample
    assert all(f.request_id == 7 and f.payload[0:4] == serial for f in recs)


def test_unsupported_command_fails_and_set_id_applies():
    n, t, link = ready_bridge()
    feed(n, link, 1000)
    t.frames()
    serial = m.serial_for(ADDR)
    t.rx.append((1, p.build_request(p.MSG_SET_PREDICTION, serial + b"\x8a\x02", 8)))
    t.rx.append((1, p.build_request(p.MSG_SET_PROBE_ID, serial + b"\x02", 9)))
    n.service(now=1100)
    rs = {f.request_id: f for f in t.frames() if f.is_response}
    assert not rs[8].success and rs[9].success
    assert n.cfg["meater_probes"][0]["id"] == 3


def test_device_info_answers_and_link_announcement():
    from linktable import LinkTable
    cfg = {"meater_bridge": True,
           "meater_probes": [{"addr": m.addr_str(ADDR), "id": 5}]}
    t = FakeTransport()
    n = Node(t, cfg, "ME00000001", log=lambda *a: None, links=LinkTable(path=None))
    t.meaters[ADDR] = ("MEATER+", 0, 1, -58)
    n.tick(now=0)
    link = t.links[ADDR]
    link.state = "ready"
    t.frames()
    feed(n, link, 1000)
    serial = m.serial_for(ADDR)
    fs = t.frames()
    linked = [f for f in fs if f.msg_type == p.MSG_DEVICE_LINKED]
    assert len(linked) == 1 and p.parse_link_device(linked[0].payload)["serial"] == serial
    assert n.links_table.linked[0]["serial"] == serial
    feed(n, link, 2000)                          # announced once only
    assert not [f for f in t.frames() if f.msg_type == p.MSG_DEVICE_LINKED]
    for mt, size, text in ((0x46, 20, b"v2.4.0"), (0x47, 16, b"v2.16-A1"),
                           (0x48, 50, b"00001011:MEATER")):
        t.rx.append((1, p.build_request(mt, serial, 100 + mt)))
        n.service(now=2100)
        (f,) = t.frames()
        assert f.is_response and f.success and f.payload[0:4] == serial
        assert len(f.payload) == 4 + size and f.payload[4:].rstrip(b"\x00") == text


def test_direct_probe_advert_layout():
    t8 = m.map_sensors([60.0, 57.0, 54.0, 56.0, 59.0], 140.0)
    msd = m.build_direct_advert(m.serial_for(ADDR), t8, probe_id=4)
    assert msd[:2] == p.VENDOR_LE and len(msd) == 25      # like a real probe
    assert len(msd) + 2 + 3 <= 31                          # fits with flags
    d = p.parse_probe_advert(msd[2:])
    assert d["ptype"] == p.PRODUCT_TYPE_PROBE and d["hops"] == 0
    assert d["probe_id"] == 4 and abs(d["core_c"] - 54.0) < 0.05


def test_rotation_includes_non_connectable_direct_advert():
    n, t, link = ready_bridge()
    feed(n, link, 1000)
    n.tick(now=1500)
    direct = [e for e in t.adv[0] if isinstance(e, tuple)]
    assert len(direct) == 1 and direct[0][1] == "probe"
    assert direct[0][0][2] == p.PRODUCT_TYPE_PROBE
    n.cfg["meater_direct_adverts"] = False
    n.tick(now=1600)
    assert not [e for e in t.adv[0] if isinstance(e, tuple)]
