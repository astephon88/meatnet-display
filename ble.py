"""ESP32 BLE transport for a MeatNet Display node (MicroPython).

- Peripheral: Nordic UART service + Device Information service, advertising
  our Display device-info interleaved with repeated probe advertisements;
  the scan response carries the DFU service UUID (0xFE59) that the
  Combustion apps filter on.
- Observer: continuous passive scan recording Combustion probes, repeated
  probe data and other nodes.
- Central: a pool of CentralLinks to probes (Probe Status + UART) and other
  nodes (UART).

The IRQ handler only records state; all BLE stack calls happen in service()
on the main loop (calling them in the IRQ panics the interrupt watchdog when
WiFi is also running).
"""

import bluetooth

import meater
import protocol
from central import CentralLink
from util import ticks_ms, ticks_diff, ticks_add

_IRQ_CENTRAL_CONNECT = 1
_IRQ_CENTRAL_DISCONNECT = 2
_IRQ_GATTS_WRITE = 3
_IRQ_SCAN_RESULT = 5
_IRQ_SCAN_DONE = 6
_IRQ_PERIPHERAL_CONNECT = 7
_IRQ_PERIPHERAL_DISCONNECT = 8
_IRQ_GATTC_SERVICE_RESULT = 9
_IRQ_GATTC_SERVICE_DONE = 10
_IRQ_GATTC_CHARACTERISTIC_RESULT = 11
_IRQ_GATTC_CHARACTERISTIC_DONE = 12
_IRQ_GATTC_DESCRIPTOR_RESULT = 13
_IRQ_GATTC_DESCRIPTOR_DONE = 14
_IRQ_GATTC_READ_RESULT = 15
_IRQ_GATTC_READ_DONE = 16
_IRQ_GATTC_WRITE_DONE = 17
_IRQ_GATTC_NOTIFY = 18
_IRQ_MTU_EXCHANGED = 21

_FLAGS_AD = b"\x02\x01\x06"   # LE General Discoverable, BR/EDR unsupported
_TXQ_MAX = 240                # queued notifications (log reads burst)
_TX_PER_SERVICE = 12          # notifications sent per service() call
_BURST_RATE = 20              # notifications/s across links = "streaming"
_BURST_HOLD_MS = 5000


class BleTransport:
    def __init__(self, serial, model="Timer REL-00001", firmware="v2.6.0",
                 max_conn=4, advertise=True):
        self.serial = serial
        # advertise=False: field unit of a two-unit setup (scan + links only,
        # never visible to phones).
        self.advertise = advertise
        self.max_conn = max_conn
        self.ble = bluetooth.BLE()
        self.ble.active(True)
        try:
            self.ble.config(mtu=247)
        except Exception:
            pass
        try:
            self.ble.config(gap_name=serial)
        except Exception:
            pass
        try:
            self.mac = bytes(self.ble.config("mac")[1])
        except Exception:
            self.mac = b"\x00" * 6
        self.ble.irq(self._irq)

        uart = (
            bluetooth.UUID(protocol.UART_SERVICE_UUID),
            (
                (bluetooth.UUID(protocol.UART_TX_CHAR_UUID),
                 bluetooth.FLAG_READ | bluetooth.FLAG_NOTIFY),
                (bluetooth.UUID(protocol.UART_RX_CHAR_UUID),
                 bluetooth.FLAG_WRITE | bluetooth.FLAG_WRITE_NO_RESPONSE),
            ),
        )
        dis = (
            bluetooth.UUID(0x180A),
            (
                (bluetooth.UUID(0x2A25), bluetooth.FLAG_READ),  # serial
                (bluetooth.UUID(0x2A26), bluetooth.FLAG_READ),  # firmware
                (bluetooth.UUID(0x2A27), bluetooth.FLAG_READ),  # hardware
                (bluetooth.UUID(0x2A24), bluetooth.FLAG_READ),  # model
                (bluetooth.UUID(0x2A29), bluetooth.FLAG_READ),  # manufacturer
            ),
        )
        ((self._tx_handle, self._rx_handle),
         (h_serial, h_fw, h_hw, h_model, h_mfr)) = \
            self.ble.gatts_register_services((uart, dis))
        self.ble.gatts_write(h_serial, serial.encode())
        self._h_fw = h_fw
        self.set_firmware(firmware)
        self.ble.gatts_write(h_hw, b"1.0")
        self.ble.gatts_write(h_model, model.encode())
        self.ble.gatts_write(h_mfr, b"Combustion Inc.")
        self.ble.gatts_set_buffer(self._rx_handle, 512, True)

        # Inbound (we are the peripheral): phones and other nodes.
        self.inbound = set()
        self.client_addr = {}
        self.conn_mtu = {}
        self.rx_queue = []          # (conn, bytes) written to our RX
        self._pending_mtu = []
        self._need_advertise = True
        # Scan observations, stored raw in the IRQ and decoded by meatnet.
        self.ignore_addrs = set()   # our own other half (two-unit setup)
        self.direct = {}            # serial4 -> (body, ticks, atype, addr, rssi)
        self.repeated = {}          # serial4 -> (body, ticks, rssi)
        self.nodes = {}             # serial10 -> (body, ticks, atype, addr, rssi)
        self.meaters = {}           # addr -> (name, ticks, atype, rssi)
        self._scanning = False
        self.scan_wanted = True
        # Active scanning (scan requests) is needed to see MEATER names/UUIDs
        # that only appear in the scan response; on only while bridging.
        self.scan_active = False
        self._scan_change = False
        self.linked_scan_duty = 20    # % scan duty while any link is up
        self._burst_until = None
        self._rate_at = None
        self._rate_count = 0
        self._scan_duty = None
        # Outbound links.
        self.links = {}             # key -> CentralLink
        self._stray_conns = []      # outbound connections no link claimed
        # Advertising.
        self.adv_payloads = ([protocol.build_display_advertisement(serial)]
                             if advertise else [])
        self.adv_fast = False
        self._adv_index = 0
        self._last_adv = None
        self._advertising = False
        # Outgoing notifications: [frame, set(conns)].
        self.txq = []
        self.tx_dropped = 0
        # Standby: no advertising/connections, low-duty scan only.
        self.standby = False
        self._standby_change = False

    def set_firmware(self, firmware):
        """DIS Firmware Revision string; centrals read it on connect."""
        self.ble.gatts_write(self._h_fw, (firmware or "").encode())

    # -- outbound links -------------------------------------------------------

    def open_link(self, key, kind, addr_type, addr):
        link = self.links.get(key)
        if link is None:
            if kind == "meater":
                link = CentralLink(
                    self.ble, "meater",
                    [("temp", None, meater.TEMP_CHAR_UUID, True),
                     ("battery", None, meater.BATTERY_CHAR_UUID, False)],
                    keep=4, kind=kind)
            elif kind == "probe":
                link = CentralLink(
                    self.ble, "probe",
                    [("status", protocol.PROBE_STATUS_SERVICE_UUID,
                      protocol.PROBE_STATUS_CHAR_UUID, True),
                     ("uart", protocol.UART_SERVICE_UUID,
                      protocol.UART_TX_CHAR_UUID, False)],
                    write=(protocol.UART_SERVICE_UUID,
                           protocol.UART_RX_CHAR_UUID),
                    kind=kind, reads=protocol.PROBE_DIS_READS)
            else:
                # A node (e.g. a Gauge relaying an Engine) drops now and
                # then while busy; get it back fast so the app keeps the
                # devices it relays. Larger buffer: log reads stream hard.
                link = CentralLink(
                    self.ble, "node",
                    [("uart", protocol.UART_SERVICE_UUID,
                      protocol.UART_TX_CHAR_UUID, True)],
                    write=(protocol.UART_SERVICE_UUID,
                           protocol.UART_RX_CHAR_UUID),
                    kind=kind, backoff_ms=2000, keep=160)
            self.links[key] = link
        if link.target != (addr_type, bytes(addr)) and link.conn is None:
            link.set_target(addr_type, addr, key)
        link.enabled = True
        return link

    def close_link(self, key):
        link = self.links.get(key)
        if link is None:
            return
        link.enabled = False      # service() disconnects, then forgets it

    def outbound_count(self):
        return sum(1 for l in self.links.values()
                   if l.conn is not None or l.state == "connecting")

    def connection_count(self):
        return len(self.inbound) + self.outbound_count()

    def _link_for_conn(self, conn):
        for link in self.links.values():
            if link.conn == conn:
                return link
        return None

    def _link_connecting(self, addr):
        # Exact address only: a late connection from an attempt we gave up on
        # must not be handed to another link that happens to be connecting.
        for link in self.links.values():
            if (link.state == "connecting" and link.target
                    and link.target[1] == addr):
                return link
        return None

    # -- scanning -------------------------------------------------------------

    def _start_scan(self):
        if self._scanning:
            return
        try:
            if self.standby:
                # ~3% duty: enough to notice a probe leaving its charger.
                self.ble.gap_scan(0, 1280000, 40000, False)
                self._scan_duty = 3
            else:
                # 50% duty while nothing is linked; less once links are up, so
                # their connection events (and WiFi) get the radio time. A
                # far device's link otherwise times out. Devices advertise
                # several times a second, so a lower duty still hears them.
                duty = self._wanted_duty()
                window = 30000
                self.ble.gap_scan(0, window * 100 // duty, window, self.scan_active)
                self._scan_duty = duty
            self._scanning = True
        except OSError:
            pass

    def _wanted_duty(self):
        if self._burst_until is not None and ticks_diff(self._burst_until, ticks_ms()) > 0:
            return 5            # a link is streaming (e.g. a log read): get out of its way
        linked = any(l.conn is not None for l in self.links.values())
        return max(5, min(50, int(self.linked_scan_duty))) if linked else 50

    def _track_bursts(self):
        """Note when any link receives more than BURST_RATE notifications a
        second; scanning backs off until BURST_HOLD_MS after it ends."""
        now = ticks_ms()
        total = 0
        for l in self.links.values():
            total += l.count
        if self._rate_at is None:
            self._rate_at, self._rate_count = now, total
            return
        dt = ticks_diff(now, self._rate_at)
        if dt >= 1000:
            if (total - self._rate_count) * 1000 // dt > _BURST_RATE:
                self._burst_until = ticks_add(now, _BURST_HOLD_MS)
            self._rate_at, self._rate_count = now, total

    def _scan_ingest(self, payload, addr_type, addr, rssi):
        """Record a Combustion advertisement. Runs in the IRQ: slicing and
        dict stores only."""
        if addr in self.ignore_addrs:
            return
        body = protocol.manufacturer_data(payload)
        if not body:
            if self.scan_active:
                name = meater.adv_match(payload)
                if name is not None:
                    old = self.meaters.get(addr)
                    if not name and old:
                        name = old[0]       # keep the name from a scan response
                    self.meaters[addr] = (name, ticks_ms(), addr_type, rssi)
            return
        ptype = body[0]
        now = ticks_ms()
        if ptype == protocol.PRODUCT_TYPE_PROBE and len(body) >= 20:
            self.direct[body[1:5]] = (body, now, addr_type, addr, rssi)
        elif ptype == protocol.PRODUCT_TYPE_NODE and len(body) >= 20:
            if body[1:5] != protocol.ZERO4:
                self.repeated[body[1:5]] = (body, now, rssi)
        elif ptype >= protocol.PRODUCT_TYPE_GAUGE and len(body) >= 11:
            self.nodes[body[1:11]] = (body, now, addr_type, addr, rssi)

    # -- advertising ----------------------------------------------------------

    def set_advertising(self, payloads, fast=False):
        if not self.advertise:
            return
        self.adv_payloads = payloads
        if fast != self.adv_fast:
            self.adv_fast = fast
            self._last_adv = None

    def _set_adv(self, msd, connectable, probe=False):
        adv = _FLAGS_AD + bytes((len(msd) + 1, 0xFF)) + msd
        if probe:
            # Like a real probe's scan response: DFU (0xFE59) + Probe Status
            # service, so the advert passes the apps' scan filters.
            resp = (b"\x03\x03\x59\xFE" + bytes((17, 0x07))
                    + protocol.PROBE_STATUS_SERVICE_UUID_LE)
        else:
            name = self.serial.encode()[:11]
            resp = (bytes((17, 0x07)) + protocol.DFU_SERVICE_UUID_LE
                    + bytes((len(name) + 1, 0x09)) + name)
        interval = 100_000 if self.adv_fast else 250_000
        self.ble.gap_advertise(interval, adv_data=adv, resp_data=resp,
                               connectable=connectable)
        self._last_adv = (msd, connectable)
        self._advertising = True

    @staticmethod
    def _entry(p):
        """Advertising entry: bytes (node advert) or (bytes, "probe") for a
        non-connectable direct-probe advert."""
        if isinstance(p, tuple):
            return p[0], p[1] == "probe"
        return p, False

    def set_scan_active(self, on):
        if on != self.scan_active:
            self.scan_active = on
            self._scan_change = True

    def set_standby(self, on):
        if on != self.standby:
            self.standby = on
            self._standby_change = True

    def _service_standby(self):
        self._standby_change = False
        if self._scanning:          # restart the scan with the new duty cycle
            try:
                self.ble.gap_scan(None)
            except OSError:
                pass
            self._scanning = False
        if self.standby:
            try:
                self.ble.gap_advertise(None)
            except OSError:
                pass
            self._advertising = False
            self._last_adv = None
            for conn in list(self.inbound):
                try:
                    self.ble.gap_disconnect(conn)
                except OSError:
                    pass
            self.txq = []
        else:
            self._need_advertise = True

    def rotate_advertisement(self):
        """Advance to the next advertisement (own Display info, then each
        repeated probe in turn)."""
        if self.standby:
            return
        payloads = self.adv_payloads
        if not payloads:
            return
        connectable = self.connection_count() < self.max_conn
        if len(payloads) == 1:
            entry = payloads[0]
        else:
            self._adv_index = (self._adv_index + 1) % len(payloads)
            entry = payloads[self._adv_index]
        msd, probe = self._entry(entry)
        if probe:
            connectable = False     # never let the app connect to a fake probe
        if (msd, connectable) == self._last_adv:
            return
        try:
            self._set_adv(msd, connectable, probe)
        except OSError:
            self._last_adv = None

    # -- IRQ --------------------------------------------------------------------

    def _irq(self, event, data):
        if event == _IRQ_SCAN_RESULT:
            addr_type, addr, _, rssi, adv_data = data
            self._scan_ingest(bytes(adv_data), addr_type, bytes(addr), rssi)
        elif event == _IRQ_CENTRAL_CONNECT:
            conn, _, addr = data
            self.inbound.add(conn)
            self.client_addr[conn] = bytes(addr)
            self.conn_mtu[conn] = 23
            self._pending_mtu.append(conn)
            self._need_advertise = True
        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn = data[0]
            self.inbound.discard(conn)
            self.client_addr.pop(conn, None)
            self.conn_mtu.pop(conn, None)
            self._need_advertise = True
        elif event == _IRQ_GATTS_WRITE:
            conn, attr = data
            if attr == self._rx_handle:
                self.rx_queue.append((conn, self.ble.gatts_read(self._rx_handle)))
        elif event == _IRQ_SCAN_DONE:
            self._scanning = False
        elif event == _IRQ_PERIPHERAL_CONNECT:
            conn, _, addr = data
            link = self._link_connecting(bytes(addr))
            if link:
                link.on_connect(conn)
                mtu = self.conn_mtu.pop(conn, None)
                if mtu:
                    link.on_mtu(mtu)
            else:
                self._stray_conns.append(conn)   # nobody wants it: drop it
            self._need_advertise = True
        elif event == _IRQ_PERIPHERAL_DISCONNECT:
            self.conn_mtu.pop(data[0], None)
            link = self._link_for_conn(data[0])
            if link:
                link.on_disconnect()
            self._need_advertise = True
        elif event == _IRQ_GATTC_SERVICE_RESULT:
            conn, start, end, uuid = data
            link = self._link_for_conn(conn)
            if link:
                link.on_service(start, end, uuid)
        elif event == _IRQ_GATTC_SERVICE_DONE:
            link = self._link_for_conn(data[0])
            if link:
                link.on_service_done()
        elif event == _IRQ_GATTC_CHARACTERISTIC_RESULT:
            conn, _, value_handle, properties, uuid = data
            link = self._link_for_conn(conn)
            if link:
                link.on_char(value_handle, uuid, properties)
        elif event == _IRQ_GATTC_CHARACTERISTIC_DONE:
            link = self._link_for_conn(data[0])
            if link:
                link.on_char_done()
        elif event == _IRQ_GATTC_DESCRIPTOR_RESULT:
            conn, handle, uuid = data
            link = self._link_for_conn(conn)
            if link:
                link.on_desc(handle, uuid)
        elif event == _IRQ_GATTC_DESCRIPTOR_DONE:
            link = self._link_for_conn(data[0])
            if link:
                link.on_desc_done()
        elif event == _IRQ_GATTC_WRITE_DONE:
            link = self._link_for_conn(data[0])
            if link:
                link.on_write_done()
        elif event == _IRQ_GATTC_NOTIFY:
            conn, value_handle, notify_data = data
            link = self._link_for_conn(conn)
            if link:
                link.on_notify(value_handle, notify_data)
        elif event == _IRQ_GATTC_READ_RESULT:
            conn, value_handle, char_data = data
            link = self._link_for_conn(conn)
            if link:
                link.on_read_result(value_handle, char_data)
        elif event == _IRQ_GATTC_READ_DONE:
            link = self._link_for_conn(data[0])
            if link:
                link.on_read_done()
        elif event == _IRQ_MTU_EXCHANGED:
            conn, mtu = data
            link = self._link_for_conn(conn)
            if link:
                link.on_mtu(mtu)
            else:
                self.conn_mtu[conn] = mtu

    # -- main loop ----------------------------------------------------------------

    def service(self):
        """Deferred BLE work; call at ~10 Hz from the asyncio loop."""
        if self._standby_change:
            self._service_standby()
        if self._scan_change:
            self._scan_change = False
            if self._scanning:          # restart with the new scan type
                try:
                    self.ble.gap_scan(None)
                except OSError:
                    pass
                self._scanning = False
        while self._pending_mtu:
            conn = self._pending_mtu.pop()
            # The Combustion app doesn't start the MTU exchange itself; real
            # nodes do, otherwise larger frames would be truncated.
            try:
                self.ble.gattc_exchange_mtu(conn)
            except OSError:
                pass
        if self._need_advertise:
            self._need_advertise = False
            self._last_adv = None
            self.rotate_advertisement()
        self._track_bursts()
        while self._stray_conns:
            try:
                self.ble.gap_disconnect(self._stray_conns.pop())
            except OSError:
                pass
        # One outstanding gap_connect at a time across all links.
        may_connect = (not any(l.state == "connecting"
                               for l in self.links.values())
                       and self.connection_count() < self.max_conn)
        for key in list(self.links):
            link = self.links[key]
            link.service(may_connect)
            if link.state == "connecting":
                may_connect = False
            if (not link.enabled and link.conn is None
                    and link.state != "connecting"):
                del self.links[key]
        if (self._scanning and not self.standby and self._scan_duty is not None
                and self._scan_duty != self._wanted_duty()):
            try:                        # links came or went: re-pace the scan
                self.ble.gap_scan(None)
            except OSError:
                pass
            self._scanning = False
        if self.scan_wanted and not self._scanning and may_connect:
            self._start_scan()
        self._drain_tx()

    def poll_rx(self):
        queue, self.rx_queue = self.rx_queue, []
        return queue

    def send(self, frame, conns=None):
        """Queue a notification on our TX to the given inbound connections
        (default: all of them)."""
        targets = set(self.inbound if conns is None else conns) & self.inbound
        if not targets:
            return
        if len(self.txq) >= _TXQ_MAX:
            self.txq.pop(0)
            self.tx_dropped += 1
        self.txq.append([frame, targets])

    def _drain_tx(self):
        sent = 0
        while self.txq and sent < _TX_PER_SERVICE:
            frame, targets = self.txq[0]
            for conn in list(targets):
                if conn not in self.inbound:
                    targets.discard(conn)
                    continue
                # A frame larger than the MTU would be truncated, and the app
                # rejects partial frames; skip until the MTU is exchanged.
                if self.conn_mtu.get(conn, 23) < len(frame) + 3:
                    targets.discard(conn)
                    continue
                try:
                    self.ble.gatts_notify(conn, self._tx_handle, frame)
                    targets.discard(conn)
                    sent += 1
                except OSError:
                    return          # stack buffers full: retry next tick
            self.txq.pop(0)
