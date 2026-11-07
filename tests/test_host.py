"""Host (Linux) units: advertising reconciliation and scan ingestion, with
fakes standing in for D-Bus and bleak."""

import asyncio
import types

import pytest

import meater
import protocol as p
from tests.test_meatnet import PROBE
from tests.test_protocol import _probe_body

bluez = pytest.importorskip("host.bluez_peripheral")
central = pytest.importorskip("host.bleak_central")


class FakeAdvMgr:
    def __init__(self):
        self.registered = []

    async def call_register_advertisement(self, path, opts):
        self.registered.append(path)

    async def call_unregister_advertisement(self, path):
        self.registered.remove(path)


class FakeBus:
    def __init__(self):
        self.exported = {}

    def export(self, path, obj):
        self.exported[path] = obj

    def unexport(self, path):
        self.exported.pop(path, None)


def test_advert_instances_follow_payloads():
    per = bluez.HostPeripheral("ME00000001", log=lambda *a: None)
    per._bus, per._adv_mgr = FakeBus(), FakeAdvMgr()
    display = p.build_display_advertisement("ME00000001")
    probe = p.repeat_probe_advert(_probe_body(PROBE, [40] * 8), 1)
    direct = meater.build_direct_advert(b"\x01\x02\x03\x4d", [50.0] * 8)

    async def go():
        per.set_advertising([display, probe, (direct, "probe")])
        await per._sync_adverts()
        assert len(per._adv_mgr.registered) == 3
        ads = list(per._ads.values())
        assert sum(1 for a in ads if not a.connectable) == 1   # MEATER direct
        assert all(len(a.body) + 4 + 4 <= 31 for a in ads)      # MSD + FE59 fit
        # a changed probe reading re-registers only that instance
        before = dict(per._ads)
        per.set_advertising([display, p.repeat_probe_advert(_probe_body(PROBE, [41] * 8), 1),
                             (direct, "probe")])
        await per._sync_adverts()
        changed = [k for k in per._ads if per._ads[k] is not before.get(k)]
        assert changed == [bytes(probe[2:7])]
        per.set_standby(True)
        await per._sync_adverts()
        assert per._adv_mgr.registered == []

    asyncio.run(go())


def test_send_only_while_subscribed():
    per = bluez.HostPeripheral("ME00000001", log=lambda *a: None)
    sent = []
    per._tx = types.SimpleNamespace(notifying=False, notify=sent.append)
    per.send(b"x")
    assert per.txq == []
    per._tx.notifying = True
    per._on_notify(True)
    per._on_write(b"\xca\xfe", "/org/bluez/hci0/dev_X", 185)
    assert per.poll_rx() == [("/org/bluez/hci0/dev_X", b"\xca\xfe")]
    per.send(b"reply", ("/org/bluez/hci0/dev_X",))
    per.service()
    assert sent == [b"reply"]


def test_scan_ingest_fills_field_tables():
    c = central.HostCentral(log=lambda *a: None)
    dev = types.SimpleNamespace(address="C2:71:3C:E5:66:60", name=None)
    body = _probe_body(PROBE, [40] * 8)
    adv = types.SimpleNamespace(manufacturer_data={p.VENDOR_ID: body}, rssi=-60,
                                local_name=None, service_uuids=[])
    c._on_adv(dev, adv)
    assert c.direct[PROBE][0] == body and c.direct[PROBE][3] == bytes.fromhex("C2713CE56660")
    mdev = types.SimpleNamespace(address="90:21:2E:0C:A0:DF", name="MEATER+")
    madv = types.SimpleNamespace(manufacturer_data={891: b"\x70"}, rssi=-40,
                                 local_name="MEATER+", service_uuids=[])
    c._on_adv(mdev, madv)
    addr = bytes.fromhex("90212E0CA0DF")
    assert c.meaters[addr][0] == "MEATER+"
    assert p.probe_serial_str(meater.serial_for(addr)) == "4D0CA0DF"   # as on the ESP32
