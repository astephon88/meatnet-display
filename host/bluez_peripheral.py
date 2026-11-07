"""App-unit radio for a Linux host: a BlueZ peripheral over D-Bus (dbus_fast).

HostPeripheral offers what meatnet.Node / split.SplitTransport use from the
local radio in the app role:
  inbound, send(frame, conns), poll_rx(), set_advertising(payloads, fast),
  rotate_advertisement(), set_standby(on), set_firmware(fw), mac, txq,
  tx_dropped, scan_wanted, service().

Advertising: one BlueZ advertising instance per payload (the controller
cycles them, so there is no software rotation). Each carries the Combustion
manufacturer data plus the 16-bit DFU service UUID (0xFE59) the apps filter
on. Discoverable=False keeps BlueZ from adding a flags AD, so both fit in a
legacy 31-byte advertisement.

GATT: Nordic UART service (RX write / TX notify) + Device Information.
BlueZ notifies every subscribed central (it can't target one connection), so
`inbound` holds the devices that wrote to RX plus a "notify" entry while
anyone is subscribed.
"""

import asyncio

from dbus_fast import BusType, PropertyAccess, Variant
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, dbus_property, method

import protocol

BLUEZ = "org.bluez"
APP_PATH = "/org/meatnet"
ADV_BASE = "/org/meatnet/adv"
TX_PER_SERVICE = 12
TXQ_MAX = 240


class _Service(ServiceInterface):
    def __init__(self, path, uuid):
        super().__init__("org.bluez.GattService1")
        self.path = path
        self._uuid = uuid

    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> "s":
        return self._uuid

    @dbus_property(access=PropertyAccess.READ)
    def Primary(self) -> "b":
        return True


class _Characteristic(ServiceInterface):
    def __init__(self, path, uuid, service_path, flags, value=b"",
                 on_write=None, on_notify=None):
        super().__init__("org.bluez.GattCharacteristic1")
        self.path = path
        self._uuid = uuid
        self._service = service_path
        self._flags = flags
        self.value = bytes(value)
        self.notifying = False
        self._on_write = on_write
        self._on_notify = on_notify

    @method()
    def ReadValue(self, options: "a{sv}") -> "ay":
        off = options.get("offset")
        off = off.value if off is not None else 0
        return self.value[off:]

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):
        if self._on_write:
            dev = options.get("device")
            mtu = options.get("mtu")
            self._on_write(bytes(value), dev.value if dev is not None else "?",
                           mtu.value if mtu is not None else None)

    @method()
    def StartNotify(self):
        self.notifying = True
        if self._on_notify:
            self._on_notify(True)

    @method()
    def StopNotify(self):
        self.notifying = False
        if self._on_notify:
            self._on_notify(False)

    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> "s":
        return self._uuid

    @dbus_property(access=PropertyAccess.READ)
    def Service(self) -> "o":
        return self._service

    @dbus_property(access=PropertyAccess.READ)
    def Flags(self) -> "as":
        return self._flags

    @dbus_property(access=PropertyAccess.READ)
    def Value(self) -> "ay":
        return self.value

    @dbus_property(access=PropertyAccess.READ)
    def Notifying(self) -> "b":
        return self.notifying

    def notify(self, data):
        self.value = bytes(data)
        self.emit_properties_changed({"Value": self.value})


class _Advertisement(ServiceInterface):
    def __init__(self, path, body, connectable, interval_ms=None):
        super().__init__("org.bluez.LEAdvertisement1")
        self.path = path
        self.body = body
        self.connectable = connectable
        self.interval_ms = interval_ms

    @method()
    def Release(self):
        pass

    @dbus_property(access=PropertyAccess.READ)
    def Type(self) -> "s":
        return "peripheral" if self.connectable else "broadcast"

    @dbus_property(access=PropertyAccess.READ)
    def ManufacturerData(self) -> "a{qv}":
        return {protocol.VENDOR_ID: Variant("ay", self.body)}

    @dbus_property(access=PropertyAccess.READ)
    def ServiceUUIDs(self) -> "as":
        return ["fe59"]

    @dbus_property(access=PropertyAccess.READ)
    def Discoverable(self) -> "b":
        return False


class HostPeripheral:
    def __init__(self, serial, model="Timer REL-00001", firmware="v2.6.0",
                 adapter="hci0", log=print, max_instances=10):
        self.serial = serial
        self.model = model
        self.firmware = firmware
        self.adapter_path = "/org/bluez/" + adapter
        self.log = log
        self.max_instances = max_instances
        self.max_conn = 4
        self.mac = b"\x00" * 6
        self.inbound = set()
        self.rx_queue = []
        self.txq = []
        self.tx_dropped = 0
        self.scan_wanted = False
        self.standby = False
        self.adv_payloads = []
        self.adv_fast = False
        self._bus = None
        self._adv_mgr = None
        self._ads = {}            # key -> _Advertisement registered
        self._adv_seq = 0
        self._adv_dirty = False
        self._adv_task = None
        self._tx = None
        self._fw = None
        self.adv_errors = 0

    # -- setup ----------------------------------------------------------------

    async def start(self):
        bus = self._bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        intro = await bus.introspect(BLUEZ, self.adapter_path)
        adapter = bus.get_proxy_object(BLUEZ, self.adapter_path, intro)
        props = adapter.get_interface("org.freedesktop.DBus.Properties")
        try:
            addr = (await props.call_get("org.bluez.Adapter1", "Address")).value
            self.mac = bytes.fromhex(addr.replace(":", ""))
        except Exception:
            pass
        try:
            await props.call_set("org.bluez.Adapter1", "Powered", Variant("b", True))
        except Exception:
            pass
        self._adv_mgr = adapter.get_interface("org.bluez.LEAdvertisingManager1")
        gatt_mgr = adapter.get_interface("org.bluez.GattManager1")

        uart = _Service(APP_PATH + "/service0", protocol.UART_SERVICE_UUID.lower())
        self._tx = _Characteristic(uart.path + "/char0",
                                   protocol.UART_TX_CHAR_UUID.lower(), uart.path,
                                   ["read", "notify"], on_notify=self._on_notify)
        rx = _Characteristic(uart.path + "/char1", protocol.UART_RX_CHAR_UUID.lower(),
                             uart.path, ["write", "write-without-response"],
                             on_write=self._on_write)
        dis = _Service(APP_PATH + "/service1", "0000180a-0000-1000-8000-00805f9b34fb")

        def dis_char(i, short, value):
            return _Characteristic(dis.path + "/char%d" % i,
                                   "0000%s-0000-1000-8000-00805f9b34fb" % short,
                                   dis.path, ["read"], value=value.encode())

        self._fw = dis_char(1, "2a26", self.firmware)
        objects = [uart, self._tx, rx, dis, dis_char(0, "2a25", self.serial), self._fw,
                   dis_char(2, "2a27", "1.0"), dis_char(3, "2a24", self.model),
                   dis_char(4, "2a29", "Combustion Inc.")]
        for obj in objects:
            bus.export(obj.path, obj)
        await gatt_mgr.call_register_application(APP_PATH, {})
        self.log("[host] GATT server registered on %s (%s)" % (
            self.adapter_path, ":".join("%02X" % b for b in self.mac)))
        self._adv_dirty = True

    # -- GATT callbacks ----------------------------------------------------------

    def _on_write(self, data, device, mtu):
        self.inbound.add(device)
        self.rx_queue.append((device, data))

    def _on_notify(self, on):
        if on:
            self.inbound.add("notify")
        else:
            self.inbound.clear()      # BlueZ: the last subscriber went away

    # -- transport interface -------------------------------------------------------

    def poll_rx(self):
        q, self.rx_queue = self.rx_queue, []
        return q

    def send(self, frame, conns=None):
        if not self.inbound or self._tx is None or not self._tx.notifying:
            return
        if conns is not None and not (set(conns) & self.inbound):
            return
        if len(self.txq) >= TXQ_MAX:
            self.txq.pop(0)
            self.tx_dropped += 1
        self.txq.append(frame)

    def set_advertising(self, payloads, fast=False):
        if payloads != self.adv_payloads:
            self.adv_payloads = payloads
            self._adv_dirty = True

    def rotate_advertisement(self):
        pass                       # the controller cycles the instances

    def set_firmware(self, firmware):
        if self._fw is not None:
            self._fw.value = (firmware or "").encode()

    def set_standby(self, on):
        if on != self.standby:
            self.standby = on
            self._adv_dirty = True
            if on:
                self.txq = []

    def service(self):
        for _ in range(min(TX_PER_SERVICE, len(self.txq))):
            self._tx.notify(self.txq.pop(0))
        if self._adv_dirty and self._adv_mgr is not None and (
                self._adv_task is None or self._adv_task.done()):
            self._adv_dirty = False
            self._adv_task = asyncio.get_event_loop().create_task(self._sync_adverts())

    # -- advertising ----------------------------------------------------------------

    @staticmethod
    def _key(msd):
        body = bytes(msd[2:])
        if body[0] in (protocol.PRODUCT_TYPE_PROBE, protocol.PRODUCT_TYPE_NODE):
            return body[0:5]
        return body[0:11]

    async def _sync_adverts(self):
        want = {}
        if not self.standby:
            for entry in self.adv_payloads[:self.max_instances]:
                msd, probe = (entry[0], entry[1] == "probe") if isinstance(entry, tuple) \
                    else (entry, False)
                want[self._key(msd)] = (bytes(msd[2:]), not probe)
        for key in list(self._ads):
            ad = self._ads[key]
            w = want.get(key)
            if w is None or w != (ad.body, ad.connectable):
                await self._unregister(key)
        for key, (body, connectable) in want.items():
            if key not in self._ads:
                await self._register(key, body, connectable)

    async def _register(self, key, body, connectable):
        self._adv_seq += 1
        ad = _Advertisement("%s%d" % (ADV_BASE, self._adv_seq), body, connectable)
        self._bus.export(ad.path, ad)
        try:
            await self._adv_mgr.call_register_advertisement(ad.path, {})
            self._ads[key] = ad
        except Exception as exc:
            self.adv_errors += 1
            self._bus.unexport(ad.path)
            if self.adv_errors <= 5:
                self.log("[host] advertisement rejected by BlueZ: %s" % exc)

    async def _unregister(self, key):
        ad = self._ads.pop(key)
        try:
            await self._adv_mgr.call_unregister_advertisement(ad.path)
        except Exception:
            pass
        self._bus.unexport(ad.path)

    async def stop(self):
        for key in list(self._ads):
            await self._unregister(key)
