"""MeatNet Display / repeater for ESP32-S3 + GC9A01A (MicroPython).

Starts automatically on boot. The `role` setting picks what this board
does: "standalone" (everything), or one half of a two-unit setup: "app"
(phone side) or "field" (device side), see split.py.

Serial console (mpremote repl / 115200 baud): 'status', 'log', 'reboot',
'help'.
"""

import asyncio
import gc
import sys
import time

import machine

import config

HELP = """Commands:
  status    node summary (probes, links, connections, WiFi)
  log       recent log lines
  stop      stop the app and drop to the REPL
  reboot    restart the board
  help"""

_log = {"seq": 0, "lines": []}   # streamed to the web UI


def log(msg):
    print(msg)
    _log["seq"] += 1
    _log["lines"].append((_log["seq"], msg))
    if len(_log["lines"]) > 80:
        _log["lines"] = _log["lines"][-80:]


def default_serial():
    # Node serials are exactly 10 alphanumeric chars (the app copies 10
    # bytes from the DIS string).
    return "".join("%02X" % b for b in machine.unique_id())[-10:]


async def ble_loop(transport, node):
    """10 Hz: BLE stack work, incoming frames, link data; the advertising
    rotation advances every 500 ms."""
    n = 0
    while True:
        try:
            transport.service()
            node.service()
            n += 1
            if n % 5 == 0:
                transport.rotate_advertisement()
        except Exception as exc:
            log("[ble] error: %s" % exc)
            sys.print_exception(exc)
        await asyncio.sleep_ms(100)


async def tick_loop(node, power):
    n = 0
    while True:
        try:
            node.tick()
            if power:
                power.tick()
        except Exception as exc:
            log("[node] error: %s" % exc)
            sys.print_exception(exc)
        n += 1
        if n % 30 == 0:
            gc.collect()
        await asyncio.sleep_ms(1000)


async def console_loop(node, wifi, power=None):
    reader = asyncio.StreamReader(sys.stdin)
    while True:
        sys.stdout.write("meatnet> ")
        line = await reader.readline()
        if not line:
            await asyncio.sleep_ms(200)
            continue
        cmd = (line.decode() if isinstance(line, bytes) else line).strip().lower()
        if cmd in ("help", "?"):
            print(HELP)
        elif cmd == "status":
            s = node.status()
            print("WiFi: %s %s" % (wifi.mode if wifi else "off",
                                   wifi.ip if wifi else ""))
            print("BLE: %d inbound, max %d, relay budget %d" % (
                s["inbound"], s["max_conn"], s["budget"]))
            if power:
                ps = power.status()
                print("Power: %s, battery %s" % (
                    "standby (%s)" % ps["reason"] if ps["standby"] else "on",
                    "%.2fV" % ps["battery"]["v"] if ps["battery"]["present"] else "none"))
            for l in s["links"]:
                print("  link %s %s: %s" % (l["kind"], l["serial"], l["state"]))
            for p in s["probes"]:
                print("  probe #%d %s core=%s src=%s age=%ds" % (
                    p["id"], p["serial"],
                    "--" if p["core_c"] is None else "%.1fC" % p["core_c"],
                    p["src"], p["age_s"]))
            for d in s["devices"]:
                print("  %s %s %ddBm" % (d["name"], d["serial"], d["rssi"]))
        elif cmd == "log":
            for _seq, text in _log["lines"][-30:]:
                print(text)
        elif cmd == "stop":
            raise KeyboardInterrupt
        elif cmd == "reboot":
            machine.reset()
        elif cmd:
            print("Unknown command (type 'help')")


def _start_wifi(cfg, serial, tasks):
    try:
        from wifi import WifiManager
        wifi = WifiManager(cfg, serial, log)
        tasks.append(wifi.run())
        return wifi
    except Exception as exc:
        sys.print_exception(exc)
        print("WiFi failed to start:", exc)
        return None


def _start_screen(cfg, node, wifi, tasks, power=None):
    """Optional: boards without the GC9A01A just log that it's missing."""
    if not cfg.get("screen", True):
        log("[screen] disabled in settings")
        return "disabled"
    try:
        from display import Display
        from screen import Screen
        disp = Display(cfg["pins"])
        if power:
            power.display = disp
        tasks.append(Screen(disp, node, wifi, cfg, power=power).run())
        log("[screen] GC9A01A via %s driver" % disp.driver)
        return disp.driver
    except Exception as exc:
        log("[screen] no display: %s" % exc)
        return "none (%s)" % exc


def _start_web(node, wifi, cfg, serial, display_info, tasks, power=None, split=None):
    if not wifi:
        return
    try:
        from web import WebUI
        tasks.append(WebUI(node, wifi, cfg, _log, serial, display_info,
                           power=power, split=split).run())
    except Exception as exc:
        sys.print_exception(exc)
        print("Web UI failed to start:", exc)


async def field_loop(transport):
    """Field unit: BLE stack work only; messages go out from SplitClient."""
    n = 0
    while True:
        try:
            transport.service()
        except Exception as exc:
            log("[ble] error: %s" % exc)
        n += 1
        if n % 300 == 0:
            gc.collect()
        await asyncio.sleep_ms(100)


async def amain_field(cfg, serial):
    """Device-side unit of a two-unit setup: scans and holds the BLE links,
    streams everything to the app unit over WiFi."""
    from ble import BleTransport
    from split import FieldRadio, SplitClient, FieldView
    transport = BleTransport(serial, max_conn=int(cfg.get("max_conn", 4)),
                             advertise=False)
    transport.linked_scan_duty = int(cfg.get("linked_scan_duty", 20))
    fradio = FieldRadio(transport, log)
    client = SplitClient(fradio, cfg, serial, log,
                         max_conn=transport.max_conn, mac=transport.mac)
    view = FieldView(transport, client, fradio)
    tasks = [field_loop(transport), client.run()]
    wifi = _start_wifi(cfg, serial, tasks)
    display_info = _start_screen(cfg, view, wifi, tasks)
    _start_web(view, wifi, cfg, serial, display_info, tasks, split=client)
    tasks.append(console_loop(view, wifi))
    await asyncio.gather(*tasks)


async def amain():
    cfg = config.load()
    serial = (cfg.get("serial") or default_serial())[:10]
    role = cfg.get("role", "standalone")
    print("\nMeatNet Display  serial %s  role %s" % (serial, role))
    if role == "field":
        await amain_field(cfg, serial)
        return

    from ble import BleTransport
    from linktable import LinkTable
    from meatnet import Node
    local = BleTransport(serial, cfg.get("dis_model", "Timer REL-00001"),
                         firmware=cfg.get("dis_firmware", "v2.6.0"),
                         max_conn=int(cfg.get("max_conn", 4)))
    local.linked_scan_duty = int(cfg.get("linked_scan_duty", 20))
    tasks = []
    split = None
    if role == "app":
        # Phone-side unit: devices are reached through the field unit.
        from split import RemoteRadio, SplitTransport, SplitServer
        remote = RemoteRadio(log)
        transport = SplitTransport(local, remote)
        server = SplitServer(remote, cfg, serial, log, mac=local.mac)
        split = remote
        tasks.append(server.run())
    else:
        transport = local
    node = Node(transport, cfg, serial, log=log, links=LinkTable())
    power = None
    try:
        from power import PowerManager
        power = PowerManager(cfg, transport, node, log=log)
    except Exception as exc:
        sys.print_exception(exc)
        print("Power management failed to start:", exc)
    tasks += [ble_loop(transport, node), tick_loop(node, power)]
    wifi = _start_wifi(cfg, serial, tasks)
    display_info = _start_screen(cfg, node, wifi, tasks, power)
    _start_web(node, wifi, cfg, serial, display_info, tasks, power, split)
    tasks.append(console_loop(node, wifi, power))
    await asyncio.gather(*tasks)


BOOT_DELAY_S = 3


def run():
    # Escape hatch: Ctrl-C during this window leaves a clean REPL with no
    # radios started, e.g. to replace a main.py that misbehaves. (Don't turn
    # BLE off on exit instead: ble.active(False) can hang the ESP32 while
    # connections/scans are active.)
    try:
        print("MeatNet starting in %d s - press Ctrl-C for the REPL" % BOOT_DELAY_S)
        time.sleep(BOOT_DELAY_S)
    except KeyboardInterrupt:
        print("Start-up skipped. Reset the board to run the app.")
        return
    try:
        asyncio.run(amain())
    except KeyboardInterrupt:
        print("\nMeatNet app stopped. Reset the board to restart.")
    finally:
        asyncio.new_event_loop()


run()
