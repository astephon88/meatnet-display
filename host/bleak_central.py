"""Field-unit radio for a Linux host, on bleak (BlueZ).

HostCentral offers the same interface as ble.BleTransport's observer and
central side, which is all split.FieldRadio uses:
  - scan tables: direct, repeated, nodes, meaters (same tuple shapes);
  - links: open_link / close_link / links {key: HostLink};
  - set_scan_active, set_standby, max_conn, mac, service().
HostLink offers CentralLink's interface (kind, state, ready, enabled, count,
mtu, take(sub), write(data), describe()).
"""

import asyncio

from bleak import BleakClient, BleakScanner

import meater
import protocol
from util import ticks_ms

PROBE_SUBS = (("status", protocol.PROBE_STATUS_CHAR_UUID, True),
              ("uart", protocol.UART_TX_CHAR_UUID, False))
NODE_SUBS = (("uart", protocol.UART_TX_CHAR_UUID, True),)
MEATER_SUBS = (("temp", meater.TEMP_CHAR_UUID, True),
               ("battery", meater.BATTERY_CHAR_UUID, False))
SUBS = {"probe": PROBE_SUBS, "node": NODE_SUBS, "meater": MEATER_SUBS}
WRITE_CHAR = {"probe": protocol.UART_RX_CHAR_UUID, "node": protocol.UART_RX_CHAR_UUID}
ERROR_BACKOFF_S = 10
CONNECT_TIMEOUT_S = 15
KEEP = 128


def addr_text(addr):
    return ":".join("%02X" % b for b in bytes(addr))


def addr_bytes(text):
    return bytes.fromhex(text.replace(":", ""))


class HostLink:
    def __init__(self, central, key, kind, address):
        self.central = central
        self.key = key
        self.kind = kind
        self.address = address
        self.enabled = True
        self.state = "idle"
        self.count = 0
        self.mtu = 23
        self.rx = {}
        self.desc = ""
        self.info = {}              # probe Device Information strings
        self._client = None
        self._write = None          # (char, response)
        self._txq = asyncio.Queue()
        self._task = asyncio.get_event_loop().create_task(self._run())

    @property
    def ready(self):
        return self.state == "ready"

    def take(self, name):
        q = self.rx.get(name)
        if not q:
            return []
        self.rx[name] = []
        return q

    def write(self, data):
        if not self.ready or self._write is None:
            return False
        self._txq.put_nowait(bytes(data))
        return True

    def describe(self):
        return self.desc or self.state

    def close(self):
        self.enabled = False

    def _on_notify(self, name):
        def cb(_char, data):
            q = self.rx.setdefault(name, [])
            q.append(bytes(data))
            self.count += 1
            if len(q) > KEEP:
                q.pop(0)
        return cb

    async def _run(self):
        while self.enabled:
            gone = asyncio.Event()
            try:
                async with self.central.connect_lock:
                    if not self.enabled:
                        break
                    self.state = "connecting"
                    target = self.central.devices.get(self.address, self.address)
                    client = BleakClient(target, timeout=CONNECT_TIMEOUT_S,
                                         disconnected_callback=lambda c: gone.set())
                    await client.connect()
                self._client = client
                self.state = "discovering"
                subs_on = []
                for name, uuid, required in SUBS[self.kind]:
                    char = client.services.get_characteristic(uuid)
                    if char is None:
                        if required:
                            raise RuntimeError("characteristic %s not found" % uuid[:8])
                        continue
                    await client.start_notify(char, self._on_notify(name))
                    subs_on.append(name)
                self._write = None
                wuuid = WRITE_CHAR.get(self.kind)
                if wuuid:
                    char = client.services.get_characteristic(wuuid)
                    if char is not None:
                        resp = "write-without-response" not in char.properties
                        self._write = (char, resp)
                self.mtu = client.mtu_size
                if self.kind == "probe":
                    for name, _svc, uuid in protocol.PROBE_DIS_READS:
                        try:
                            raw = bytes(await client.read_gatt_char(uuid))
                            self.info[name] = raw.split(b"\x00")[0].decode(errors="replace")
                        except Exception:
                            pass
                self.desc = ", ".join("%s notify on" % n for n in subs_on) + (
                    ", write (%s)" % ("with response" if self._write[1] else "no response")
                    if self._write else "")
                self.state = "ready"
                while self.enabled and not gone.is_set():
                    try:
                        data = await asyncio.wait_for(self._txq.get(), 0.5)
                    except asyncio.TimeoutError:
                        continue
                    n = max(20, self.mtu - 3)
                    for i in range(0, len(data), n):
                        await client.write_gatt_char(self._write[0], data[i:i + n],
                                                     response=self._write[1])
                if not self.enabled:
                    self.state = "disconnecting"
                    await client.disconnect()
                self.state = "idle"
            except Exception as exc:
                self.state = "error: %s" % (str(exc) or type(exc).__name__)
                try:
                    if self._client is not None:
                        await self._client.disconnect()
                except Exception:
                    pass
                for _ in range(ERROR_BACKOFF_S * 2):
                    if not self.enabled:
                        break
                    await asyncio.sleep(0.5)
                if self.enabled:
                    self.state = "idle"
            finally:
                self._client = None
        self.state = "idle"


class HostCentral:
    def __init__(self, max_conn=4, log=print):
        self.max_conn = max_conn
        self.log = log
        self.mac = b"\x00" * 6
        self.direct = {}
        self.repeated = {}
        self.nodes = {}
        self.meaters = {}
        self.links = {}
        self.ignore_addrs = set()  # the app unit's own adverts
        self.devices = {}         # address text -> BLEDevice (for connects)
        self.connect_lock = asyncio.Lock()
        self.standby = False
        self.scan_active = True    # BlueZ scans actively anyway
        self._scanner = None

    async def start(self):
        self._scanner = BleakScanner(self._on_adv)
        await self._scanner.start()

    def _on_adv(self, dev, adv):
        now = ticks_ms()
        addr = addr_bytes(dev.address)
        if addr in self.ignore_addrs:
            return
        body = adv.manufacturer_data.get(protocol.VENDOR_ID)
        if body:
            body = bytes(body)
            ptype = body[0]
            self.devices[dev.address] = dev
            if ptype == protocol.PRODUCT_TYPE_PROBE and len(body) >= 20:
                self.direct[body[1:5]] = (body, now, 0, addr, adv.rssi)
            elif ptype == protocol.PRODUCT_TYPE_NODE and len(body) >= 20:
                if body[1:5] != protocol.ZERO4:
                    self.repeated[body[1:5]] = (body, now, adv.rssi)
            elif ptype >= protocol.PRODUCT_TYPE_GAUGE and len(body) >= 11:
                self.nodes[body[1:11]] = (body, now, 0, addr, adv.rssi)
            return
        name = adv.local_name or dev.name or ""
        uuids = [u.upper() for u in adv.service_uuids]
        if name.upper().startswith("MEATER") or any(u in uuids for u in meater.SERVICE_UUIDS):
            self.devices[dev.address] = dev
            self.meaters[addr] = (name, now, 0, adv.rssi)

    # -- transport interface -------------------------------------------------

    def open_link(self, key, kind, addr_type, addr):
        key = bytes(key)
        link = self.links.get(key)
        if link is None or not link.enabled:
            link = self.links[key] = HostLink(self, key, kind, addr_text(addr))
        return link

    def close_link(self, key):
        link = self.links.pop(bytes(key), None)
        if link is not None:
            link.close()

    def set_scan_active(self, on):
        pass

    def set_standby(self, on):
        if on == self.standby:
            return
        self.standby = on
        if self._scanner is not None:
            loop = asyncio.get_event_loop()
            loop.create_task(self._scanner.stop() if on else self._scanner.start())
        if on:
            for key in list(self.links):
                self.close_link(key)

    def set_advertising(self, payloads, fast=False):
        pass

    def service(self):
        pass
