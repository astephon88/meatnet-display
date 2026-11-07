import asyncio
import socket
import struct

import protocol as p
import split
from meatnet import Node
from util import ticks_ms
from tests.test_meatnet import FakeTransport, PROBE
from tests.test_protocol import _probe_body, _status


class FakeLocal(FakeTransport):
    """App unit's own radio (peripheral side only)."""

    def __init__(self):
        super().__init__()
        self.scan_wanted = True
        self.txq = []
        self.tx_dropped = 0
        self.standby = False

    def rotate_advertisement(self):
        pass

    def service(self):
        pass

    def set_standby(self, on):
        self.standby = on

    def set_firmware(self, fw):
        pass


def test_encode_decode_and_b64():
    m = {"t": "rx", "k": "01020304", "data": split.b64(b"\x00\xff")}
    line = split.encode(m)
    assert line.endswith(b"\n")
    back = split.decode(line)
    assert back == m and split.unb64(back["data"]) == b"\x00\xff"
    assert split.decode(b"not json") is None and split.decode(b"[1]") is None


def test_field_radio_scan_deltas_and_link_messages():
    t = FakeTransport()
    fr = split.FieldRadio(t, log=lambda *a: None)
    now = ticks_ms()
    body = _probe_body(PROBE, [40] * 8)
    t.direct[PROBE] = (body, now, 1, b"\xAA" * 6, -50)
    m = fr.scan_message(now + 10)
    assert m["d"][0][0] == PROBE.hex() and m["d"][0][2] == 10
    assert split.unb64(m["d"][0][1]) == body
    assert fr.scan_message(now + 20) is None          # unchanged: nothing sent
    fr.apply({"t": "open", "k": PROBE.hex(), "kind": "probe", "at": 1,
              "addr": "aa" * 6})
    link = t.links[PROBE]
    link.state = "ready"
    link.rx["status"] = [b"S" * 100]
    msgs = fr.link_messages()
    assert msgs[0]["t"] == "link" and msgs[0]["state"] == "ready"
    assert msgs[1] == {"t": "rx", "k": PROBE.hex(), "sub": "status",
                       "data": split.b64(b"S" * 100)}
    fr.apply({"t": "write", "k": PROBE.hex(), "data": split.b64(b"cmd")})
    assert link.written == [b"cmd"]
    fr.apply({"t": "close", "k": PROBE.hex()})
    assert fr.link_messages()[0]["state"] == "closed"


def test_remote_radio_mirrors_scan_and_proxies_links():
    r = split.RemoteRadio(log=lambda *a: None)
    r.on_connect({"max_conn": 3, "serial": "F1"})
    body = _probe_body(PROBE, [40] * 8)
    r.apply({"t": "scan", "d": [[PROBE.hex(), split.b64(body), 100, 1, "aa" * 6, -55]]}, 5000)
    assert r.direct[PROBE] == (body, 4900, 1, b"\xAA" * 6, -55)
    r.take_outbox()
    link = r.open_link(PROBE, "probe", 1, b"\xAA" * 6)
    assert r.take_outbox()[0]["t"] == "open"
    assert not link.write(b"x")                      # not ready yet
    r.apply({"t": "link", "k": PROBE.hex(), "state": "ready", "desc": "ok", "mtu": 185}, 0)
    r.apply({"t": "rx", "k": PROBE.hex(), "sub": "uart", "data": split.b64(b"u")}, 0)
    assert link.ready and link.take("uart") == [b"u"] and link.describe() == "ok"
    assert link.write(b"x") and r.take_outbox()[0]["t"] == "write"
    r.on_disconnect()
    assert link.state == "error: field unit offline"
    r.on_connect({"max_conn": 3})
    assert any(m["t"] == "open" for m in r.take_outbox())   # re-opened


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_end_to_end_over_tcp():
    asyncio.run(_e2e())


async def _wait(cond, node, timeout=5.0):
    t0 = asyncio.get_event_loop().time()
    while not cond():
        node.tick()
        node.service()
        if asyncio.get_event_loop().time() - t0 > timeout:
            raise AssertionError("timed out waiting")
        await asyncio.sleep(0.05)


async def _e2e():
    port = _free_port()
    quiet = lambda *a: None
    app_cfg = {"split_port": port, "split_key": "k", "meatnet_peer": False}
    field_cfg = {"split_port": port, "split_key": "k", "split_peer": "127.0.0.1"}
    local = FakeLocal()
    remote = split.RemoteRadio(quiet)
    st = split.SplitTransport(local, remote)
    assert local.scan_wanted is False
    node = Node(st, app_cfg, "APP0000001", log=quiet)
    server = split.SplitServer(remote, app_cfg, "APP0000001", quiet)
    srv_task = asyncio.create_task(server.run(host="127.0.0.1", beacon=False))
    field_t = FakeTransport()
    fradio = split.FieldRadio(field_t, quiet)
    client = split.SplitClient(fradio, field_cfg, "FIELD00001", quiet, max_conn=4)
    cli_task = asyncio.create_task(client.run())
    try:
        await _wait(lambda: remote.connected and client.connected, node)
        assert node.link_budget() == 4          # separate radios: no reserve

        # A probe heard by the field unit is repeated by the app unit.
        field_t.direct[PROBE] = (_probe_body(PROBE, [40] * 8), ticks_ms(), 1,
                                 b"\xAA" * 6, -50)
        await _wait(lambda: local.adv and any(
            isinstance(x, bytes) and x[2] == 2 and x[3:7] == PROBE
            for x in local.adv[0]), node)

        # Selecting it opens the link on the field unit.
        app_cfg["linked_probes"] = ["04030201"]
        await _wait(lambda: PROBE in field_t.links, node)
        flink = field_t.links[PROBE]
        flink.state = "ready"
        flink.rx["status"] = [_status()]
        await _wait(lambda: any(p.split_frames(f)[0] and
                                p.split_frames(f)[0][0].msg_type == 0x45
                                for f, _ in local.sent), node)

        # A phone command reaches the probe; its response reaches the phone.
        local.sent = []
        local.rx.append((1, p.build_request(p.MSG_SET_PREDICTION, PROBE + b"\x8a\x02", 77)))
        await _wait(lambda: flink.written, node)
        assert flink.written[0] == p.build_probe_request(p.MSG_SET_PREDICTION, b"\x8a\x02")
        flink.rx["uart"] = [p.build_probe_response(p.MSG_SET_PREDICTION, True)]

        def got_response():
            for f, _ in local.sent:
                for fr in p.split_frames(f)[0]:
                    if fr.is_response and fr.request_id == 77:
                        return True
            return False
        await _wait(got_response, node)

        # A MEATER next to the field unit is bridged and advertised by the
        # app unit.
        import meater
        from tests.test_meater import notification
        maddr = bytes.fromhex("A1B2C3D4E5F6")
        field_t.meaters[maddr] = ("MEATER+", ticks_ms(), 0, -60)
        app_cfg["linked_probes"] = []
        app_cfg["meater_bridge"] = True
        app_cfg["meater_probes"] = [{"addr": meater.addr_str(maddr), "id": 3}]
        await _wait(lambda: maddr in field_t.links, node)
        assert field_t.links[maddr].kind == "meater" and field_t.scan_active
        mlink = field_t.links[maddr]
        mlink.state = "ready"
        mlink.rx["temp"] = [notification([60.0, 57.0, 54.0, 56.0, 59.0], 140.0)]
        mserial = meater.serial_for(maddr)
        await _wait(lambda: any(
            (x[0] if isinstance(x, tuple) else x)[3:7] == mserial
            for x in (local.adv[0] if local.adv else [])), node)
        app_cfg["meater_bridge"] = False
        app_cfg["linked_probes"] = ["04030201"]

        # Field unit goes away: links offline; comes back: link re-opened.
        cli_task.cancel()
        await _wait(lambda: not remote.connected, node)
        assert remote.links[PROBE].state == "error: field unit offline"
        field_t.links.clear()
        cli_task = asyncio.create_task(client.run())
        await _wait(lambda: remote.connected and PROBE in field_t.links, node)
    finally:
        cli_task.cancel()
        srv_task.cancel()
        await asyncio.sleep(0)


def test_wrong_key_is_rejected():
    asyncio.run(_wrong_key())


async def _wrong_key():
    port = _free_port()
    quiet = lambda *a: None
    remote = split.RemoteRadio(quiet)
    server = split.SplitServer(remote, {"split_port": port, "split_key": "right"}, "A", quiet)
    srv = asyncio.create_task(server.run(host="127.0.0.1", beacon=False))
    client = split.SplitClient(split.FieldRadio(FakeTransport(), quiet),
                               {"split_port": port, "split_key": "wrong",
                                "split_peer": "127.0.0.1"}, "F", quiet)
    await asyncio.sleep(0.05)
    try:
        await asyncio.wait_for(client._session("127.0.0.1"), 3)
    except Exception:
        pass
    assert not remote.connected and not client.connected
    srv.cancel()
    await asyncio.sleep(0)


def test_field_view_status_and_screen_items():
    import readings
    t = FakeTransport()
    t.direct[PROBE] = (_probe_body(PROBE, [40] * 8), ticks_ms(), 1, b"\xAA" * 6, -50)
    fr = split.FieldRadio(t, lambda *a: None)
    client = split.SplitClient(fr, {}, "F", lambda *a: None)
    st = split.FieldView(t, client, fr).status()
    assert st["role"] == "field" and st["probes"][0]["serial"] == "04030201"
    items = readings.build_items({}, st, "sta", "10.0.0.9")
    status = [i for i in items if i.get("status")][0]
    assert status["label"] == "Field unit"
    assert status["subs"][0] == "10.0.0.9" and status["subs"][1] == "app unit offline"


def test_field_ignores_the_app_units_own_adverts():
    import meater
    t = FakeTransport()
    fr = split.FieldRadio(t, lambda *a: None)
    now = ticks_ms()
    app_mac = bytes.fromhex("4C796ED5B3E8")
    own_display = p.build_display_advertisement("APP0000001")[2:]
    t.nodes[p.encode_serial("APP0000001")] = (own_display, now, 0, app_mac, -40)
    fake_meater = meater.build_direct_advert(b"\x01\x02\x03\x4d", [50.0] * 8)[2:]
    t.direct[b"\x01\x02\x03\x4d"] = (fake_meater, now, 0, app_mac, -40)
    t.repeated[PROBE] = (_probe_body(PROBE, [40] * 8), now, -40)
    real = b"\x09\x09\x09\x10"
    t.direct[real] = (_probe_body(real, [40] * 8), now, 0, b"\xC2" * 6, -70)
    fr.set_ignore(app_mac, "APP0000001")
    assert t.ignore_addrs == {app_mac}
    m = fr.scan_message(now)
    assert [row[0] for row in m.get("d", [])] == [real.hex()]
    assert "n" not in m and "r" not in m
    # a Display advert with our serial is dropped even from another address
    t.nodes[p.encode_serial("APP0000001")] = (own_display, now + 1, 0, b"\x01" * 6, -40)
    assert fr.scan_message(now + 2) is None
    # and the app side never mirrors its own serial either
    r = split.RemoteRadio(lambda *a: None)
    r.own_serial = p.encode_serial("APP0000001")
    r.apply({"t": "scan", "n": [[p.encode_serial("APP0000001").hex(),
                                 split.b64(own_display), 0, 0, "01" * 6, -40]]}, 1000)
    assert r.nodes == {}


def test_remote_link_keeps_field_unit_stats():
    r = split.RemoteRadio(lambda *a: None)
    r.on_connect({"max_conn": 6})
    link = r.open_link(PROBE, "node", 0, b"\x01" * 6)
    st = {"connects": 3, "drops": {"supervision timeout (signal lost) (0x08)": 2},
          "params": "50.0ms/0/8000ms"}
    r.apply({"t": "link", "k": PROBE.hex(), "state": "ready", "desc": "", "mtu": 177,
             "stats": st}, 0)
    assert link.stats == st
