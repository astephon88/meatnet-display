"""App-unit radio on Bumble, a userspace Bluetooth stack (bumble.readthedocs.io).

For hosts without BlueZ (e.g. a container on a node with no kernel
Bluetooth): Bumble drives the controller itself, over USB, a serial HCI
port or anything else it has a transport for (`usb:0`, `serial:/dev/ttyACM0`,
`hci-socket:0`). BumblePeripheral offers the same interface as
bluez_peripheral.HostPeripheral.

Adverts follow the MicroPython radio (ble.py): flags + Combustion
manufacturer data, a scan response with the DFU service (+ name, or the
Probe Status service for direct-probe adverts, which are never connectable).
With extended advertising each payload gets its own advertising set (legacy
PDUs, so phones see them as normal adverts); without it one advert is
rotated, like the MicroPython radio.

Unlike BlueZ, Bumble notifies one connection at a time, so `inbound` holds
the connection handles of the phones actually connected.
"""

import asyncio

from bumble import hci, transport
from bumble.device import (AdvertisingEventProperties, AdvertisingParameters,
                           AdvertisingType, Device, Peer)
from bumble.gatt import Characteristic, CharacteristicValue, Service

import protocol

TX_PER_SERVICE = 12
TXQ_MAX = 240
MTU = 247
FLAGS_AD = b"\x02\x01\x06"      # LE General Discoverable, BR/EDR unsupported


def _uuid16(short):
    return "0000%s-0000-1000-8000-00805F9B34FB" % short


class BumblePeripheral:
    def __init__(self, serial, model="Timer REL-00001", firmware="v2.6.0",
                 hci_transport="usb:0", log=print, max_instances=10):
        self.serial = serial
        self.model = model
        self.firmware = firmware
        self.hci_transport = hci_transport
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
        self.adv_errors = 0
        self._transport = None
        self._device = None
        self._conns = {}           # handle -> Connection
        self._tx = None
        self._fw = None
        self._sets = {}            # key -> (AdvertisingSet, signature)
        self._max_sets = 0
        self._adv_index = -1
        self._adv_dirty = False
        self._adv_task = None
        self._tx_task = None

    # -- setup ----------------------------------------------------------------

    async def start(self, hci_pair=None):
        """Open the controller; `hci_pair` (source, sink) bypasses the
        transport string (tests use a virtual controller)."""
        if hci_pair is None:
            self._transport = await transport.open_transport(self.hci_transport)
            hci_pair = (self._transport.source, self._transport.sink)
        # Bumble needs an address to start with; the public one replaces it.
        dev = self._device = Device.with_hci(self.serial, hci.Address("F0:00:00:00:00:01"),
                                             *hci_pair)
        self._tx = Characteristic(protocol.UART_TX_CHAR_UUID,
                                  Characteristic.Properties.READ
                                  | Characteristic.Properties.NOTIFY,
                                  Characteristic.READABLE, b"")
        rx = Characteristic(protocol.UART_RX_CHAR_UUID,
                            Characteristic.Properties.WRITE
                            | Characteristic.Properties.WRITE_WITHOUT_RESPONSE,
                            Characteristic.WRITEABLE, CharacteristicValue(write=self._on_write))

        def dis_char(short, value):
            return Characteristic(_uuid16(short), Characteristic.Properties.READ,
                                  Characteristic.READABLE, value.encode())

        self._fw = dis_char("2A26", self.firmware)
        dev.add_services([
            Service(protocol.UART_SERVICE_UUID, [self._tx, rx]),
            Service(protocol.DIS_SERVICE_UUID, [
                dis_char("2A25", self.serial), self._fw, dis_char("2A27", "1.0"),
                dis_char("2A24", self.model), dis_char("2A29", "Combustion Inc.")]),
        ])
        dev.on("connection", self._on_connection)
        await dev.power_on()
        addr = dev.public_address
        if addr == hci.Address.ANY:
            addr = dev.random_address
        self.mac = bytes.fromhex(addr.to_string(False).replace(":", ""))
        if dev.supports_le_extended_advertising:
            self._max_sets = min(self.max_instances,
                                 dev.host.number_of_supported_advertising_sets)
        self.log("[host] Bumble on %s (%s), %s" % (
            self.hci_transport, ":".join("%02X" % b for b in self.mac),
            "%d advertising sets" % self._max_sets if self._max_sets
            else "one rotating advert"))
        self._adv_dirty = True

    # -- connections and GATT callbacks ------------------------------------------

    def _on_connection(self, conn):
        if conn.role != hci.Role.PERIPHERAL:
            return
        self._conns[conn.handle] = conn
        self.inbound.add(conn.handle)

        def gone(_reason):
            self._conns.pop(conn.handle, None)
            self.inbound.discard(conn.handle)
            self._adv_dirty = True        # connectable again if we were full

        conn.on("disconnection", gone)
        # Ask for a large MTU, as the Display does, so frames aren't truncated.
        asyncio.ensure_future(self._request_mtu(conn))
        self._adv_dirty = True

    async def _request_mtu(self, conn):
        try:
            await Peer(conn).request_mtu(MTU)
        except Exception:
            pass

    def _on_write(self, conn, value):
        self.rx_queue.append((conn.handle, bytes(value)))

    # -- transport interface -------------------------------------------------------

    def poll_rx(self):
        q, self.rx_queue = self.rx_queue, []
        return q

    def send(self, frame, conns=None):
        if not self.inbound:
            return
        if conns is not None:
            conns = set(conns) & self.inbound
            if not conns:
                return
        if len(self.txq) >= TXQ_MAX:
            self.txq.pop(0)
            self.tx_dropped += 1
        self.txq.append((frame, conns))

    def set_advertising(self, payloads, fast=False):
        if payloads != self.adv_payloads or fast != self.adv_fast:
            self.adv_payloads = payloads
            self.adv_fast = fast
            self._adv_dirty = True

    def rotate_advertisement(self):
        if not self._max_sets:
            self._adv_dirty = True        # next payload on the next sync

    def set_firmware(self, firmware):
        if self._fw is not None:
            self._fw.value = (firmware or "").encode()

    def set_standby(self, on):
        if on != self.standby:
            self.standby = on
            self._adv_dirty = True
            if on:
                self.txq = []
                for conn in list(self._conns.values()):
                    asyncio.ensure_future(conn.disconnect())

    def service(self):
        if self._device is None:
            return
        loop = asyncio.get_event_loop()
        if self.txq and (self._tx_task is None or self._tx_task.done()):
            self._tx_task = loop.create_task(self._send_batch())
        if self._adv_dirty and (self._adv_task is None or self._adv_task.done()):
            self._adv_dirty = False
            self._adv_task = loop.create_task(self._sync_adverts())

    async def _send_batch(self):
        for _ in range(min(TX_PER_SERVICE, len(self.txq))):
            frame, conns = self.txq.pop(0)
            try:
                if conns is None:
                    await self._device.notify_subscribers(self._tx, frame)
                else:
                    for handle in conns:
                        conn = self._conns.get(handle)
                        if conn is not None:
                            await self._device.notify_subscriber(conn, self._tx, frame)
            except Exception as exc:
                self.log("[host] notify failed: %r" % exc)

    # -- advertising ----------------------------------------------------------------

    @staticmethod
    def _key(msd):
        body = bytes(msd[2:])
        if body[0] in (protocol.PRODUCT_TYPE_PROBE, protocol.PRODUCT_TYPE_NODE):
            return body[0:5]
        return body[0:11]

    @staticmethod
    def _entry(p):
        if isinstance(p, tuple):
            return bytes(p[0]), p[1] == "probe"
        return bytes(p), False

    def _adv_data(self, msd, probe):
        adv = FLAGS_AD + bytes((len(msd) + 1, 0xFF)) + msd
        if probe:
            # Like a real probe's scan response: DFU + Probe Status service.
            resp = (b"\x03\x03\x59\xFE" + bytes((17, 0x07))
                    + protocol.PROBE_STATUS_SERVICE_UUID_LE)
        else:
            name = self.serial.encode()[:11]
            resp = (bytes((17, 0x07)) + protocol.DFU_SERVICE_UUID_LE
                    + bytes((len(name) + 1, 0x09)) + name)
        return adv, resp

    async def _sync_adverts(self):
        try:
            if self._max_sets:
                await self._sync_sets()
            else:
                await self._rotate_legacy()
        except Exception as exc:
            self.adv_errors += 1
            if self.adv_errors <= 5:
                self.log("[host] advertising failed: %r" % exc)

    async def _sync_sets(self):
        want = {}
        if not self.standby:
            room = len(self.inbound) < self.max_conn
            interval = 100 if self.adv_fast else 250
            for p in self.adv_payloads[:self._max_sets]:
                msd, probe = self._entry(p)
                want[self._key(msd)] = (msd, probe, (probe, room and not probe, interval))
        for key in list(self._sets):
            adv_set, sig = self._sets[key]
            w = want.get(key)
            if w is None or w[2] != sig:
                del self._sets[key]
                await adv_set.stop()
                await adv_set.remove()
        for key, (msd, probe, sig) in want.items():
            adv, resp = self._adv_data(msd, probe)
            if key in self._sets:
                await self._sets[key][0].set_advertising_data(adv)
                continue
            _probe, connectable, interval = sig
            params = AdvertisingParameters(
                advertising_event_properties=AdvertisingEventProperties(
                    is_connectable=connectable, is_scannable=True, is_legacy=True),
                primary_advertising_interval_min=interval,
                primary_advertising_interval_max=interval,
                own_address_type=hci.OwnAddressType.PUBLIC)
            adv_set = await self._device.create_advertising_set(
                advertising_parameters=params, advertising_data=adv,
                scan_response_data=resp, auto_restart=connectable)
            self._sets[key] = (adv_set, sig)

    async def _rotate_legacy(self):
        dev = self._device
        if dev.is_advertising:
            await dev.stop_advertising()
        payloads = self.adv_payloads
        if self.standby or not payloads:
            return
        self._adv_index = (self._adv_index + 1) % len(payloads)
        msd, probe = self._entry(payloads[self._adv_index])
        connectable = not probe and len(self.inbound) < self.max_conn
        adv, resp = self._adv_data(msd, probe)
        kind = (AdvertisingType.UNDIRECTED_CONNECTABLE_SCANNABLE if connectable
                else AdvertisingType.UNDIRECTED_SCANNABLE)
        interval = 100 if self.adv_fast else 250
        await dev.start_advertising(advertising_type=kind,
                                    own_address_type=hci.OwnAddressType.PUBLIC,
                                    advertising_data=adv, scan_response_data=resp,
                                    advertising_interval_min=interval,
                                    advertising_interval_max=interval)

    async def stop(self):
        for adv_set, _sig in list(self._sets.values()):
            try:
                await adv_set.stop()
            except Exception:
                pass
        self._sets.clear()
        if self._transport is not None:
            await self._transport.close()
