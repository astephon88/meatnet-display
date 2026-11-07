import struct

import protocol as p
from meatnet import Node
from linktable import LinkTable
from tests.test_protocol import _probe_body, _status

SERIAL = "ME00000001"
PROBE = b"\x01\x02\x03\x04"


class FakeLink:
    def __init__(self, kind):
        self.kind = kind
        self.state = "idle"
        self.enabled = True
        self.count = 0
        self.mtu = 185
        self.written = []
        self.rx = {}

    @property
    def ready(self):
        return self.state == "ready"

    def take(self, name):
        q = self.rx.get(name, [])
        self.rx[name] = []
        return q

    def write(self, data):
        self.written.append(bytes(data))
        return True


class FakeTransport:
    def __init__(self):
        self.max_conn = 4
        self.mac = b"\x01\x02\x03\x04\x05\x06"
        self.inbound = {1}
        self.direct = {}
        self.repeated = {}
        self.nodes = {}
        self.meaters = {}
        self.scan_active = False
        self.links = {}
        self.rx = []
        self.sent = []
        self.adv = None

    def poll_rx(self):
        q, self.rx = self.rx, []
        return q

    def send(self, frame, conns=None):
        self.sent.append((bytes(frame), tuple(conns) if conns else None))

    def open_link(self, key, kind, at, addr):
        self.links.setdefault(key, FakeLink(kind)).enabled = True
        return self.links[key]

    def close_link(self, key):
        self.links.pop(key, None)

    def set_scan_active(self, on):
        self.scan_active = on

    def set_standby(self, on):
        self.standby = on

    def set_advertising(self, payloads, fast=False):
        self.adv = (payloads, fast)

    def frames(self):
        out = []
        for raw, conns in self.sent:
            out.extend(p.split_frames(raw)[0])
        self.sent = []
        return out


def make(cfg=None):
    t = FakeTransport()
    cfg = cfg or {}
    n = Node(t, cfg, SERIAL, log=lambda *a: None, links=LinkTable(path=None))
    return n, t


def hear_probe(t, now=0):
    body = _probe_body(PROBE, [40] * 8)
    t.direct[PROBE] = (body, now, 0, b"\xAA" * 6, -50)


def test_advertising_repeats_heard_probe():
    n, t = make()
    hear_probe(t)
    n.tick(now=100)
    payloads, fast = t.adv
    assert payloads[0][2] == p.PRODUCT_TYPE_DISPLAY
    assert payloads[1][2] == p.PRODUCT_TYPE_NODE and payloads[1][3:7] == PROBE
    assert not fast
    assert n.probes[PROBE]["src"] == "advert"


def test_link_only_selected_probes():
    n, t = make()
    hear_probe(t)
    n.tick(now=100)
    assert t.links == {}
    n.cfg["linked_probes"] = ["04030201"]
    n.tick(now=200)
    assert PROBE in t.links


def linked(now=0):
    n, t = make({"linked_probes": ["04030201"], "meatnet_peer": False})
    hear_probe(t, now)
    n.tick(now=now)
    link = t.links[PROBE]
    link.state = "ready"
    link.rx["status"] = [_status()]
    n.service(now=now)
    return n, t, link


def test_status_forwarded_as_0x45_and_probe_list():
    n, t, link = linked()
    fs = t.frames()
    st = [f for f in fs if f.msg_type == 0x45]
    assert len(st) == 1 and st[0].payload[0:4] == PROBE
    t.rx.append((1, p.build_request(0x44, b"", 77)))
    n.service(now=10)
    (f,) = t.frames()
    assert f.is_response and f.request_id == 77
    assert f.payload[0] == 1 and f.payload[1:5] == PROBE


def test_command_translated_to_probe_and_response_back():
    n, t, link = linked()
    t.frames()
    req = p.build_request(p.MSG_SET_PREDICTION, PROBE + b"\x8a\x02", 1234)
    t.rx.append((1, req))
    n.service(now=10)
    (w,) = link.written
    assert w == p.build_probe_request(p.MSG_SET_PREDICTION, b"\x8a\x02")
    link.rx["uart"] = [p.build_probe_response(p.MSG_SET_PREDICTION, True)]
    n.service(now=20)
    (f,) = t.frames()
    assert f.is_response and f.msg_type == p.MSG_SET_PREDICTION
    assert f.request_id == 1234 and f.success and f.payload == b""


def test_duplicate_request_ignored():
    n, t, link = linked()
    req = p.build_request(p.MSG_RESET_FOOD_SAFE, PROBE, 55)
    t.rx.append((1, req))
    t.rx.append((1, req))
    n.service(now=10)
    assert len(link.written) == 1


def test_log_read_streams_until_end():
    n, t, link = linked()
    t.frames()
    req = p.build_request(p.MSG_READ_LOGS, PROBE + struct.pack("<II", 0, 2), 9)
    t.rx.append((1, req))
    n.service(now=10)
    recs = b"".join(p.build_probe_response(
        p.MSG_READ_LOGS, True, struct.pack("<I", seq) + b"\x00" * 20) for seq in range(3))
    link.rx["uart"] = [recs[:30], recs[30:]]
    n.service(now=20)
    fs = t.frames()
    assert [struct.unpack_from("<I", f.payload, 4)[0] for f in fs] == [0, 1, 2]
    assert all(f.payload[0:4] == PROBE and f.request_id == 9 for f in fs)
    assert n.pending == []


def test_command_timeout_sends_failure():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(p.MSG_RESET_THERMOMETER, PROBE, 3)))
    n.service(now=10)
    n.tick(now=10 + 7000)
    fail = [f for f in t.frames() if f.is_response]
    assert fail and fail[0].request_id == 3 and not fail[0].success


def test_unknown_probe_not_answered():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(p.MSG_SET_PROBE_ID, b"\x09\x09\x09\x09\x01", 4)))
    n.service(now=10)
    assert t.frames() == [] and link.written == []


def test_silence_global_reaches_probe_and_self():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(p.MSG_SILENCE_ALARMS, b"\x01\x00" + b"\x00" * 14, 8)))
    n.service(now=10)
    assert link.written == [p.build_probe_request(p.MSG_SILENCE_ALARMS)]
    rs = [f for f in t.frames() if f.is_response]
    assert rs[0].payload[0] == p.PRODUCT_TYPE_DISPLAY
    link.rx["uart"] = [p.build_probe_response(p.MSG_SILENCE_ALARMS, True)]
    n.service(now=20)
    (f,) = t.frames()
    assert f.payload[0] == 1 and f.payload[1:5] == PROBE


def test_topology_for_self():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(0x43, p.encode_serial(SERIAL), 5)))
    n.service(now=10)
    (f,) = t.frames()
    assert f.payload[0] == 20 and f.payload[1] == 4
    assert f.payload[21] == 1 and f.payload[22] == 1   # 1 outbound: device #1


def test_upstream_status_routes_commands_to_node():
    n, t = make({"upstream_node": "BOOSTER001"})
    key = p.encode_serial("BOOSTER001")
    t.nodes[key] = (bytes((5,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    n.tick(now=0)
    up = t.links[key]
    up.state = "ready"
    n.service(now=0)
    up.written = []
    other = b"\x0A\x0B\x0C\x0D"
    status = p.build_node_probe_status(other, _status(), hop_count=1, request_id=42)
    up.rx["uart"] = [status]
    n.service(now=10)
    fs = t.frames()
    st = [f for f in fs if f.msg_type == 0x45]
    assert st and st[0].payload[52] == 1           # bumped to 2 hops
    assert other in n.via_node
    t.rx.append((1, p.build_request(p.MSG_SET_PROBE_COLOR, other + b"\x02", 66)))
    n.service(now=20)
    assert up.written and p.split_frames(up.written[-1])[0][0].request_id == 66
    up.rx["uart"] = [p.build_response(p.MSG_SET_PROBE_COLOR, 66, True, b"", 67)]
    n.service(now=30)
    (f,) = [f for f in t.frames() if f.is_response]
    assert f.request_id == 66


def test_link_table_merge_substitutes_own_serial():
    lt = LinkTable(path=None)
    lt.merge([{"ptype": 4, "serial": b"ME00000001", "ts": 1}], True, SERIAL, b"PEER000001")
    assert lt.linked[0]["serial"] == b"PEER000001"
    lt2 = LinkTable(path=None)
    lt2.merge([{"ptype": 4, "serial": b"ME00000001", "ts": 1}], True, SERIAL, None)
    assert lt2.linked == []


def test_link_device_and_read_back():
    n, t = make()
    entry = p.encode_product_serial(1, PROBE)
    t.rx.append((1, p.build_request(p.MSG_LINK_DEVICE, b"\x01" + entry + b"\x00" * 4, 11)))
    n.service(now=0)
    fs = t.frames()
    assert any(f.is_response and f.request_id == 11 and f.success for f in fs)
    assert any(f.msg_type == p.MSG_DEVICE_LINKED and not f.is_response for f in fs)
    t.rx.append((1, p.build_request(p.MSG_READ_LINKED_DEVICES, b"\x00", 12)))
    n.service(now=1)
    (f,) = t.frames()
    _page, _total, entries = p.parse_link_entries_response(f.payload)
    assert entries[0]["serial"] == PROBE


def test_overlapping_log_reads_are_matched_by_sequence():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(p.MSG_READ_LOGS, PROBE + struct.pack("<II", 0, 5), 101)))
    t.rx.append((1, p.build_request(p.MSG_READ_LOGS, PROBE + struct.pack("<II", 3, 8), 102)))
    n.service(now=10)

    def rec(seq):
        return p.build_probe_response(p.MSG_READ_LOGS, True,
                                      struct.pack("<I", seq) + b"\x00" * 20)
    # the probe answers the two reads one after the other
    link.rx["uart"] = [b"".join(rec(s) for s in list(range(0, 6)) + list(range(3, 9)))]
    n.service(now=20)
    got = [(f.request_id, struct.unpack_from("<I", f.payload, 4)[0])
           for f in t.frames() if f.msg_type == p.MSG_READ_LOGS]
    assert got == [(101, s) for s in range(0, 6)] + [(102, s) for s in range(3, 9)]
    assert n.pending == []
    # a stray record afterwards is ignored (and counted)
    link.rx["uart"] = [rec(4)]
    n.service(now=30)
    assert t.frames() == [] and n.stats["stray"] == 1


def test_probe_device_info_answered_from_link():
    n, t, link = linked()
    t.frames()
    link.info = {"firmware": "v2.4.0", "hardware": "v2.16-A1",
                 "model": "00001011:20260527"}
    for mt, size, text in ((0x46, 20, b"v2.4.0"), (0x47, 16, b"v2.16-A1"),
                           (0x48, 50, b"00001011:20260527")):
        t.rx.append((1, p.build_request(mt, PROBE, 300 + mt)))
        n.service(now=10)
        (f,) = t.frames()
        assert f.success and f.payload[0:4] == PROBE and len(f.payload) == 4 + size
        assert f.payload[4:].rstrip(b"\x00") == text


def test_node_link_associates_and_pulls_table_once():
    n, t = make({"upstream_node": "BOOSTER001", "upstream_push": True})
    key = p.encode_serial("BOOSTER001")
    t.nodes[key] = (bytes((5,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    n.tick(now=0)
    up = t.links[key]
    up.state = "ready"
    n.service(now=0)
    sent = [p.split_frames(w)[0][0].msg_type for w in up.written]
    assert sent == [p.MSG_ASSOCIATE_NODE, p.MSG_READ_LINKED_DEVICES,
                    p.MSG_READ_UNLINKED_DEVICES]
    assoc_id = p.split_frames(up.written[0])[0][0].request_id
    up.rx["uart"] = [p.build_response(p.MSG_ASSOCIATE_NODE, assoc_id, True, b"", 5)]
    n.service(now=10)
    # reconnect: associates again, but doesn't re-pull the link table
    up.written = []
    up.state = "connecting"
    n.service(now=20)
    up.state = "ready"
    n.service(now=30)
    assert [p.split_frames(w)[0][0].msg_type for w in up.written] == [p.MSG_ASSOCIATE_NODE]


def test_gauge_log_read_replies_route_back_to_the_phone():
    n, t = make({"upstream_node": "G000001CA8"})
    key = p.encode_serial("G000001CA8")
    t.nodes[key] = (bytes((3,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    n.tick(now=0)
    up = t.links[key]
    up.state = "ready"
    n.service(now=0)
    up.written = []
    t.frames()
    req = p.build_request(0x62, key + struct.pack("<II", 0, 2), 555)
    t.rx.append((1, req))
    n.service(now=10)
    assert up.written and p.split_frames(up.written[-1])[0][0].request_id == 555
    up.rx["uart"] = [p.build_response(0x62, 555, True, b"r%d" % i, 900 + i) for i in range(3)]
    n.service(now=20)
    back = [f for f in t.frames() if f.is_response and f.request_id == 555]
    assert len(back) == 3


def test_upstream_node_gets_no_unsolicited_traffic_by_default():
    n, t = make({"upstream_node": "G000001CA8", "linked_probes": ["04030201"]})
    key = p.encode_serial("G000001CA8")
    t.nodes[key] = (bytes((3,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    hear_probe(t)
    n.tick(now=0)
    up = t.links[key]
    up.state = "ready"
    plink = t.links[PROBE]
    plink.state = "ready"
    plink.rx["status"] = [_status()]
    for now in range(0, 12000, 1000):
        n.service(now=now)
        n.tick(now=now)
    assert up.written == []                    # no 0x4A/0x73/0x45/0x49/0x4B
    assert any(p.split_frames(f)[0][0].msg_type == 0x49 for f, _ in t.sent)  # phones still
    t.rx.append((1, p.build_request(0x62, key + struct.pack("<II", 0, 2), 7)))
    n.service(now=13000)
    assert len(up.written) == 1                # but phone requests go through


def test_relay_counters_show_app_using_the_display_route():
    logs = []
    t = FakeTransport()
    n = Node(t, {"upstream_node": "G000001CA8"}, SERIAL, log=logs.append)
    key = p.encode_serial("G000001CA8")
    t.nodes[key] = (bytes((3,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    n.tick(now=0)
    up = t.links[key]
    up.state = "ready"
    n.service(now=0)
    up.rx["uart"] = [p.build_request(0x60, key + b"\x00" * 25, 41)]   # gauge status
    n.service(now=5)
    t.rx.append((1, p.build_request(0x62, key + struct.pack("<II", 0, 1), 42)))
    n.service(now=10)
    up.rx["uart"] = [p.build_response(0x62, 42, True, b"x", 43)]
    n.service(now=20)
    assert n.via["app_to_node"] == {"0x62": 1}
    assert n.via["node_to_app"] == {"0x60": 1, "0x62 rsp": 1}
    assert any("app reached G000001CA8 through the Display (0x62)" in m for m in logs)


def test_log_read_waits_for_a_slow_start_and_holds_other_commands():
    n, t, link = linked()
    t.frames()
    t.rx.append((1, p.build_request(p.MSG_READ_LOGS, PROBE + struct.pack("<II", 0, 1), 201)))
    t.rx.append((1, p.build_request(p.MSG_READ_SESSION_INFO, PROBE, 202)))
    n.service(now=0)
    n.tick(now=9000)                  # probe still preparing its log after 9 s
    assert {q["type"] for q in n.pending} == {p.MSG_READ_LOGS, p.MSG_READ_SESSION_INFO}
    assert not [f for f in t.frames() if f.is_response]   # no premature failures

    def rec(seq):
        return p.build_probe_response(p.MSG_READ_LOGS, True,
                                      struct.pack("<I", seq) + b"\x00" * 20)
    link.rx["uart"] = [rec(0) + rec(1) +
                       p.build_probe_response(p.MSG_READ_SESSION_INFO, True, b"\x01" * 6)]
    n.service(now=9500)
    got = {(f.msg_type, f.request_id) for f in t.frames() if f.is_response}
    assert (p.MSG_READ_LOGS, 201) in got and (p.MSG_READ_SESSION_INFO, 202) in got
    assert n.pending == []


def test_links_to_several_upstream_nodes_and_drops_unticked_ones():
    gauge, engine = p.encode_serial("G000001CA8"), p.encode_serial("E000002067")
    n, t = make({"upstream_node": ["G000001CA8", "E000002067"]})
    budget = n.link_budget()
    for key, ptype in ((gauge, 3), (engine, 6)):
        t.nodes[key] = (bytes((ptype,)) + key + b"\x00" * 12, 0, 0, b"\xBB" * 6, -60)
    n.tick(now=0)
    assert t.links[gauge].kind == "node" and t.links[engine].kind == "node"
    for key in (gauge, engine):
        t.links[key].state = "ready"
    n.service(now=0)
    t.frames()
    # A request for another node goes out on both links, once each.
    t.rx.append((1, p.build_request(0x62, gauge + struct.pack("<II", 0, 2), 777)))
    n.service(now=10)
    for key in (gauge, engine):
        assert [p.split_frames(w)[0][0].request_id for w in t.links[key].written] == [777]
    n.cfg["upstream_node"] = ["E000002067"]
    assert n.link_budget() == budget + 1
    n.tick(now=1000)
    assert gauge not in t.links and engine in t.links


def test_single_upstream_serial_string_still_works(tmp_path):
    import json
    import config
    path = tmp_path / "config.json"
    path.write_text(json.dumps({"upstream_node": "G000001CA8"}))
    assert config.load(str(path))["upstream_node"] == ["G000001CA8"]
    n, _t = make({"upstream_node": "G000001CA8"})
    assert n.upstream_serials() == [p.encode_serial("G000001CA8")]
