"""Stand-ins for the MicroPython-only bits the shared modules use, so they
run unchanged under CPython on a Linux host."""

import asyncio
import gc
import os
import sys
import types


def install():
    if not hasattr(asyncio, "sleep_ms"):
        asyncio.sleep_ms = lambda ms: asyncio.sleep(ms / 1000)
    if not hasattr(gc, "mem_free"):
        gc.mem_free = lambda: 0
    if "machine" not in sys.modules:
        machine = types.ModuleType("machine")

        def reset():
            # "Reboot" from the web UI: restart this process with the same args.
            os.execv(sys.executable, [sys.executable] + sys.argv)

        machine.reset = reset
        machine.unique_id = lambda: _machine_id()
        sys.modules["machine"] = machine


def _machine_id():
    try:
        with open("/etc/machine-id") as f:
            return bytes.fromhex(f.read().strip())[:6]
    except Exception:
        return b"\x00\x00\x00\x00\x00\x01"


class HostNet:
    """What web.py/screen expect from wifi.WifiManager: the host is simply
    on its network already."""

    mode = "sta"
    ap_ssid = ""

    def __init__(self):
        self.ip = local_ip()

    def scan(self):
        return []


def local_ip():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))      # no packet is sent for UDP connect
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()
