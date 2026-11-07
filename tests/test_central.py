"""CentralLink discovery sequence against a fake `bluetooth` module."""

import sys
import types

fake = types.ModuleType("bluetooth")


class UUID:
    def __init__(self, v):
        self.v = v if isinstance(v, int) else v.upper()

    def __eq__(self, o):
        return isinstance(o, UUID) and o.v == self.v

    def __hash__(self):
        return hash(self.v)

    def __repr__(self):
        return "UUID(%r)" % (self.v,)


fake.UUID = UUID
sys.modules.setdefault("bluetooth", fake)

import protocol as p  # noqa: E402
from central import CentralLink  # noqa: E402


class FakeBLE:
    def __init__(self):
        self.calls = []

    def __getattr__(self, name):
        def call(*a):
            self.calls.append((name,) + a)
        return call


def test_probe_link_discovery_enables_both_notifications():
    ble = FakeBLE()
    link = CentralLink(ble, "probe",
                       [("status", p.PROBE_STATUS_SERVICE_UUID, p.PROBE_STATUS_CHAR_UUID, True),
                        ("uart", p.UART_SERVICE_UUID, p.UART_TX_CHAR_UUID, False)],
                       write=(p.UART_SERVICE_UUID, p.UART_RX_CHAR_UUID))
    link.enabled = True
    link.set_target(0, b"\x01" * 6, b"KEY1")
    link.service(True)
    assert ble.calls[-1][0] == "gap_connect"
    link.on_connect(5)
    link.service(True)
    assert ble.calls[-1][0] == "gattc_discover_services"
    link.on_service(1, 9, UUID(0x180A))
    link.on_service(10, 14, UUID(p.PROBE_STATUS_SERVICE_UUID))
    link.on_service(20, 28, UUID(p.UART_SERVICE_UUID))
    link.on_service_done()
    link.service(True)
    assert ble.calls[-1][0] == "gattc_discover_characteristics"
    link.on_char(12, UUID(p.PROBE_STATUS_CHAR_UUID))
    link.on_char_done()
    link.service(True)
    link.on_char(22, UUID(p.UART_RX_CHAR_UUID))
    link.on_char(25, UUID(p.UART_TX_CHAR_UUID))
    link.on_char_done()
    for h, cccd in ((12, 13), (25, 26)):
        link.service(True)
        assert ble.calls[-1][0] == "gattc_discover_descriptors" and ble.calls[-1][2] == h
        link.on_desc(cccd, UUID(0x2902))
        link.on_desc_done()
    writes = []
    for _ in range(2):
        link.service(True)
        writes.append(ble.calls[-1])
        link.on_write_done()
    assert [w[2] for w in writes] == [13, 26]
    link.service(True)   # notify list exhausted -> mtu step queued
    link.service(True)
    assert ble.calls[-1][0] == "gattc_exchange_mtu" and link.ready
    link.on_notify(12, b"status")
    link.on_notify(25, b"uart")
    assert link.take("status") == [b"status"] and link.take("uart") == [b"uart"]
    link.on_mtu(185)
    assert link.write(b"x" * 10)
    link.service(True)
    assert ble.calls[-1][0] == "gattc_write" and ble.calls[-1][2] == 22


def test_missing_required_service_fails():
    ble = FakeBLE()
    link = CentralLink(ble, "probe",
                       [("status", p.PROBE_STATUS_SERVICE_UUID, p.PROBE_STATUS_CHAR_UUID, True)])
    link.enabled = True
    link.set_target(0, b"\x01" * 6)
    link.service(True)
    link.on_connect(3)
    link.service(True)
    link.on_service_done()
    assert link.state.startswith("error") and link.action == "disconnect"


def test_write_type_follows_characteristic_properties():
    def link_with(props):
        link = CentralLink(FakeBLE(), "probe",
                           [("status", p.PROBE_STATUS_SERVICE_UUID, p.PROBE_STATUS_CHAR_UUID, True)],
                           write=(p.UART_SERVICE_UUID, p.UART_RX_CHAR_UUID))
        link.on_char(22, UUID(p.UART_RX_CHAR_UUID), props)
        return link
    assert link_with(0x08).write_mode == 1            # Write only -> acked writes
    assert link_with(0x0C).write_mode == 0            # no-response allowed
    assert "with response" in link_with(0x08).describe()


def test_any_service_finds_meater_characteristics():
    """Handle layout captured from a MEATER+ (service C9E2746C-...)."""
    import meater
    ble = FakeBLE()
    link = CentralLink(ble, "meater",
                       [("temp", None, meater.TEMP_CHAR_UUID, True),
                        ("battery", None, meater.BATTERY_CHAR_UUID, False)],
                       kind="meater")
    link.enabled = True
    link.set_target(0, b"\x02" * 6)
    link.service(True)
    link.on_connect(7)
    link.service(True)
    link.on_service(13, 22, UUID(0x180A))
    link.on_service(24, 64, UUID("C9E2746C-59F1-4E54-A0DD-E1E54555CF8B"))
    link.on_service(1, 12, UUID(0x1801))
    link.on_service_done()
    assert not link.state.startswith("error")
    for _ in range(3):                   # characteristic discovery per service
        link.service(True)
        if ble.calls[-1][2] == 24:
            link.on_char(29, UUID(meater.TEMP_CHAR_UUID), 0x12)
            link.on_char(33, UUID(meater.BATTERY_CHAR_UUID), 0x12)
        link.on_char_done()
    for h, cccd in ((29, 30), (33, 34)):
        link.service(True)
        assert ble.calls[-1][0] == "gattc_discover_descriptors" and ble.calls[-1][2] == h
        assert ble.calls[-1][3] == 64
        link.on_desc(cccd, UUID(0x2902))
        link.on_desc_done()
    for _ in range(2):
        link.service(True)
        link.on_write_done()
    link.service(True)
    link.service(True)
    assert link.ready
    assert "temp notify on" in link.describe()


def test_missing_required_service_error():
    ble = FakeBLE()
    link = CentralLink(ble, "probe",
                       [("status", p.PROBE_STATUS_SERVICE_UUID, p.PROBE_STATUS_CHAR_UUID, True)])
    link.enabled = True
    link.set_target(0, b"\x01" * 6)
    link.service(True)
    link.on_connect(3)
    link.service(True)
    link.on_service(1, 9, UUID(0x180A))
    link.on_service_done()
    assert link.state == "error: service not found"


def test_single_service_link_discovers_by_uuid_and_reads_errors():
    import central
    ble = FakeBLE()
    link = CentralLink(ble, "node",
                       [("uart", p.UART_SERVICE_UUID, p.UART_TX_CHAR_UUID, True)],
                       write=(p.UART_SERVICE_UUID, p.UART_RX_CHAR_UUID), kind="node")
    link.enabled = True
    link.set_target(0, b"\x03" * 6)
    link.service(True)
    link.on_connect(4)
    link.service(True)
    assert ble.calls[-1] == ("gattc_discover_services", 4, UUID(p.UART_SERVICE_UUID))
    assert central._describe_error(OSError(-128)) == "connection lost (out of range?)"
    assert central._describe_error(OSError(-5)) == "-5"


def test_probe_link_reads_device_information_after_ready():
    ble = FakeBLE()
    link = CentralLink(ble, "probe",
                       [("status", p.PROBE_STATUS_SERVICE_UUID, p.PROBE_STATUS_CHAR_UUID, True)],
                       reads=p.PROBE_DIS_READS)
    link.enabled = True
    link.set_target(0, b"\x01" * 6)
    link.service(True)
    link.on_connect(5)
    link.service(True)
    assert ble.calls[-1] == ("gattc_discover_services", 5)   # two services: all
    link.on_service(10, 14, UUID(p.PROBE_STATUS_SERVICE_UUID))
    link.on_service(1, 9, UUID(0x180A))            # how the stack reports it
    link.on_service_done()
    for _ in range(2):
        link.service(True)
        if ble.calls[-1][2] == 10:
            link.on_char(12, UUID(p.PROBE_STATUS_CHAR_UUID))
        else:
            link.on_char(3, UUID(0x2A26))
            link.on_char(5, UUID(0x2A27))
            link.on_char(7, UUID(0x2A24))
        link.on_char_done()
    link.service(True)
    link.on_desc(13, UUID(0x2902))
    link.on_desc_done()
    link.service(True)
    link.on_write_done()
    link.service(True)
    link.service(True)
    assert link.ready
    link.on_mtu(185)
    for value in (b"v2.4.0\x00", b"v2.16-A1", b"00001011:20260527"):
        link.service(True)
        assert ble.calls[-1][0] == "gattc_read"
        link.on_read_result(ble.calls[-1][2], value)
        link.on_read_done()
    assert link.info == {"firmware": "v2.4.0", "hardware": "v2.16-A1",
                         "model": "00001011:20260527"}


def _connected_link(addr=b"\x01" * 6):
    ble = FakeBLE()
    link = CentralLink(ble, "node",
                       [("uart", p.UART_SERVICE_UUID, p.UART_TX_CHAR_UUID, True)],
                       write=(p.UART_SERVICE_UUID, p.UART_RX_CHAR_UUID), kind="node")
    link.enabled = True
    link.set_target(0, addr)
    link.service(True)
    return ble, link


def test_busy_errors_are_retried_not_failed():
    import central
    ble, link = _connected_link()
    link.on_connect(9)

    def busy(*a):
        raise OSError(16)                      # EBUSY: another discovery running
    ble.gattc_discover_services = busy
    link.service(True)
    assert link.state == "discovering" and link.action == "discover_svc"
    del ble.gattc_discover_services            # stack free again
    link.service(True)
    assert ble.calls[-1][0] == "gattc_discover_services"


def test_setup_timeout_gives_up_on_stuck_discovery(monkeypatch):
    import central
    ble, link = _connected_link()
    link.on_connect(9)
    link.service(True)                          # discovery requested, no IRQs come
    link.connected_at -= central.SETUP_TIMEOUT_MS + 1
    link.service(True)
    assert link.state.startswith("error: setup timed out")


def test_stray_connection_is_not_handed_to_another_link():
    import ble as blemod
    t = object.__new__(blemod.BleTransport)
    t.links = {}
    _, a = _connected_link(b"\xAA" * 6)
    t.links[b"a"] = a
    assert a.state == "connecting"
    assert t._link_connecting(b"\xBB" * 6) is None      # someone else's address
    assert t._link_connecting(b"\xAA" * 6) is a


def test_scan_duty_drops_once_links_are_up():
    import ble as blemod
    t = object.__new__(blemod.BleTransport)
    t.links = {}
    t.linked_scan_duty = 20
    t._burst_until = None
    assert t._wanted_duty() == 50
    _, a = _connected_link()
    a.on_connect(3)
    t.links[b"a"] = a
    assert t._wanted_duty() == 20
    t.linked_scan_duty = 1
    assert t._wanted_duty() == 5               # clamped


def test_scan_backs_off_during_a_link_burst():
    import ble as blemod
    import util
    t = object.__new__(blemod.BleTransport)
    t.links = {}
    t.linked_scan_duty = 20
    t._burst_until = None
    t._rate_at = None
    t._rate_count = 0
    _, a = _connected_link()
    a.on_connect(3)
    t.links[b"a"] = a
    now = [1000]
    real = blemod.ticks_ms
    blemod.ticks_ms = lambda: now[0]
    try:
        t._track_bursts()
        a.count += 100                          # 100 notifications in 1 s
        now[0] += 1000
        t._track_bursts()
        assert t._wanted_duty() == 5
        now[0] += blemod._BURST_HOLD_MS + 1
        t._track_bursts()
        assert t._wanted_duty() == 20
    finally:
        blemod.ticks_ms = real
