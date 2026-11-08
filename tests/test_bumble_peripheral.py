"""BumblePeripheral against a phone stand-in, both on Bumble's virtual
controller (no radio needed). Skipped unless Bumble is installed."""

import asyncio

import pytest

pytest.importorskip("bumble")

from bumble.controller import Controller  # noqa: E402
from bumble.device import Device, Peer  # noqa: E402
from bumble import hci  # noqa: E402
from bumble.hci import Address  # noqa: E402
from bumble.host import Host  # noqa: E402
from bumble.link import LocalLink  # noqa: E402
from bumble.transport.common import AsyncPipeSink  # noqa: E402

import protocol  # noqa: E402
from host.bumble_peripheral import BumblePeripheral  # noqa: E402

UNIT_ADDR = "F0:F0:F0:F0:F0:F0"
NODE_MSD = bytes((0xC7, 0x09, protocol.PRODUCT_TYPE_NODE)) + bytes(range(1, 21))
PROBE_MSD = bytes((0xC7, 0x09, protocol.PRODUCT_TYPE_PROBE)) + bytes(range(21, 41))


async def _setup(sets):
    link = LocalLink()
    unit_ctl = Controller("unit", link=link, public_address=UNIT_ADDR)
    if sets:
        # The virtual controller handles advertising sets but doesn't list
        # the command that says how many it has.
        unit_ctl.supported_commands = unit_ctl.supported_commands | {
            hci.HCI_LE_READ_NUMBER_OF_SUPPORTED_ADVERTISING_SETS_COMMAND}
    phone_ctl = Controller("phone", link=link, public_address="F1:F1:F1:F1:F1:F1")
    unit = BumblePeripheral("HB00000001", hci_transport="virtual", log=lambda *_: None)
    await unit.start((unit_ctl, AsyncPipeSink(unit_ctl)))
    # The virtual link always sends LE data from the random address; the unit
    # advertises (and is connected) on its public one, as real controllers do.
    unit_ctl.random_address = Address(UNIT_ADDR, Address.PUBLIC_DEVICE_ADDRESS)
    phone = Device(address=Address("F1:F1:F1:F1:F1:F1"),
                   host=Host(phone_ctl, AsyncPipeSink(phone_ctl)))
    await phone.power_on()
    return unit, phone


async def _pump(unit, seconds):
    for _ in range(int(seconds / 0.05)):
        unit.service()
        await asyncio.sleep(0.05)


@pytest.mark.parametrize("sets", [False, True], ids=["rotating", "sets"])
def test_adverts_connect_write_notify(sets):
    async def run():
        unit, phone = await _setup(sets)
        assert bool(unit._max_sets) == sets
        assert unit.mac == bytes.fromhex(UNIT_ADDR.replace(":", ""))
        unit.set_advertising([NODE_MSD, (PROBE_MSD, "probe")])

        heard = {}

        def on_adv(a):
            heard[bytes(a.data.get(0xFF)[1])] = a.is_connectable

        phone.on("advertisement", on_adv)
        await phone.start_scanning()
        await _pump(unit, 1.0)
        await phone.stop_scanning()
        assert heard.get(NODE_MSD[2:]) is True
        if sets:
            # Both sets run at once. The virtual controller ignores the event
            # properties on the air, so check what the unit asked for.
            assert PROBE_MSD[2:] in heard
            props = {k: s.advertising_parameters.advertising_event_properties
                     for k, (s, _sig) in unit._sets.items()}
            assert props[unit._key(NODE_MSD)].is_connectable
            assert not props[unit._key(PROBE_MSD)].is_connectable
            assert all(p.is_legacy for p in props.values())

        conn = await phone.connect(Address(UNIT_ADDR, Address.PUBLIC_DEVICE_ADDRESS))
        await _pump(unit, 0.3)
        assert len(unit.inbound) == 1
        peer = Peer(conn)
        await peer.discover_services()
        await peer.discover_characteristics()
        tx = peer.get_characteristics_by_uuid(protocol.UART_TX_CHAR_UUID)[0]
        rx = peer.get_characteristics_by_uuid(protocol.UART_RX_CHAR_UUID)[0]
        got = []
        await peer.subscribe(tx, got.append)

        await peer.write_value(rx, b"\xca\xfe\x01", with_response=False)
        await _pump(unit, 0.2)
        (handle, data), = unit.poll_rx()
        assert data == b"\xca\xfe\x01" and handle in unit.inbound

        unit.send(b"to everyone")
        unit.send(b"to this phone", [handle])
        unit.send(b"to nobody", [handle + 99])
        await _pump(unit, 0.3)
        assert [bytes(v) for v in got] == [b"to everyone", b"to this phone"]

        await conn.disconnect()
        await _pump(unit, 0.3)
        assert not unit.inbound
        await unit.stop()

    asyncio.run(run())


def test_standby_stops_adverts():
    async def run():
        unit, phone = await _setup(True)
        unit.set_advertising([NODE_MSD])
        await _pump(unit, 0.3)
        unit.set_standby(True)
        await _pump(unit, 0.3)
        heard = []
        phone.on("advertisement", heard.append)
        await phone.start_scanning()
        await asyncio.sleep(0.5)
        assert not heard
        await unit.stop()

    asyncio.run(run())
