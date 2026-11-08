"""Run the app unit or the field unit of a two-unit setup on a Linux host.

    python3 -m host.run app   [--config host/app.json]   [--web-port 8080]
    python3 -m host.run field [--config host/field.json] [--web-port 8081]

The app unit uses BlueZ over D-Bus to advertise as the Display and serve the
UART GATT service to the Combustion app. The field unit uses bleak to scan
and hold links to probes, MEATERs and nodes. They talk to each other, or to
an ESP32 in the opposite role, with the same WiFi protocol (split.py).
Both run on the same adapter if you want to try the whole chain on one
host (start the app unit first).

Needs: Python 3.10+, bleak, dbus_fast (installed with bleak), BlueZ 5.56+.
"""

import argparse
import asyncio
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
os.chdir(ROOT)                    # web.py serves www/ relative to the cwd

from host import shims  # noqa: E402

shims.install()

import config  # noqa: E402
import split  # noqa: E402

_log = {"seq": 0, "lines": []}


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)
    _log["seq"] += 1
    _log["lines"].append((_log["seq"], msg))
    if len(_log["lines"]) > 200:
        _log["lines"] = _log["lines"][-200:]


def default_serial(role):
    uid = "".join("%02X" % b for b in shims._machine_id())
    return ("H" + role[0].upper() + uid)[:10]


async def _every(ms, fn, name):
    while True:
        try:
            fn()
        except Exception as exc:
            log("[%s] error: %r" % (name, exc))
        await asyncio.sleep(ms / 1000)


async def run_app(cfg, args, serial):
    from linktable import LinkTable
    from meatnet import Node
    from web import WebUI
    model = cfg.get("dis_model", "Timer REL-00001")
    firmware = cfg.get("dis_firmware", "v2.6.0")
    if args.hci:
        from host.bumble_peripheral import BumblePeripheral
        local = BumblePeripheral(serial, model, firmware, args.hci, log)
        radio = "Bumble " + args.hci
    else:
        from host.bluez_peripheral import HostPeripheral
        local = HostPeripheral(serial, model, firmware, args.adapter, log)
        radio = "BlueZ " + args.adapter
    await local.start()
    remote = split.RemoteRadio(log)
    transport = split.SplitTransport(local, remote)
    node = Node(transport, cfg, serial, log=log,
                links=LinkTable(os.path.splitext(args.config)[0] + "-links.json"))
    server = split.SplitServer(remote, cfg, serial, log, mac=local.mac)
    web = WebUI(node, shims.HostNet(), cfg, _log, serial, "none (host)",
                split=remote, port=args.web_port)

    def fast():
        transport.service()
        node.service()

    log("[host] app unit %s: phones -> %s, devices via field unit on "
        "TCP %d, web UI http://%s:%d/" % (serial, radio, server.port,
                                          shims.local_ip(), args.web_port))
    await asyncio.gather(server.run(), web.run(), _every(100, fast, "ble"),
                         _every(1000, node.tick, "node"))


async def run_field(cfg, args, serial):
    from host.bleak_central import HostCentral
    from web import WebUI
    central = HostCentral(max_conn=int(cfg.get("max_conn", 4)), log=log)
    await central.start()
    fradio = split.FieldRadio(central, log)
    client = split.SplitClient(fradio, cfg, serial, log,
                               max_conn=central.max_conn, mac=central.mac)
    view = split.FieldView(central, client, fradio)
    web = WebUI(view, shims.HostNet(), cfg, _log, serial, "none (host)",
                split=client, port=args.web_port)
    peer = cfg.get("split_peer") or "auto (beacon)"
    log("[host] field unit %s: scanning with bleak, app unit %s, web UI "
        "http://%s:%d/" % (serial, peer, shims.local_ip(), args.web_port))
    await asyncio.gather(client.run(), web.run())


def main():
    ap = argparse.ArgumentParser(description="MeatNet two-unit setup on a Linux host")
    ap.add_argument("role", choices=("app", "field"))
    ap.add_argument("--config", help="settings file (default host/<role>.json)")
    ap.add_argument("--web-port", type=int, help="web UI port (default 8080 app / 8081 field)")
    ap.add_argument("--adapter", default="hci0", help="BlueZ adapter (app unit)")
    ap.add_argument("--hci", help="app unit: use Bumble instead of BlueZ, on this "
                    "transport (e.g. usb:0, serial:/dev/ttyACM0)")
    ap.add_argument("--peer", help="field unit: app unit host/IP (default: beacon)")
    ap.add_argument("--key", help="shared key (overrides the settings file)")
    args = ap.parse_args()
    args.config = args.config or os.path.join("host", args.role + ".json")
    args.web_port = args.web_port or (8080 if args.role == "app" else 8081)
    config.PATH = args.config
    cfg = config.load()
    cfg["role"] = args.role
    if args.peer is not None:
        cfg["split_peer"] = args.peer
    if args.key is not None:
        cfg["split_key"] = args.key
    serial = (cfg.get("serial") or default_serial(args.role))[:10]
    try:
        asyncio.run(run_app(cfg, args, serial) if args.role == "app"
                    else run_field(cfg, args, serial))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
