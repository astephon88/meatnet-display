"""Asyncio web UI (MicroPython): status, devices, display and WiFi settings.

  GET  /                 www/index.html (streamed from flash)
  GET  /ws               WebSocket: {"t":"status"} every second + log lines
  GET  /api/state        live status JSON
  GET  /api/config       settings (passwords omitted)
  POST /api/config       partial settings update (JSON), applied live
  GET  /api/wifi/scan    nearby networks
  POST /api/wifi         {"ssid","password"}: save and reboot
  POST /api/power        {"standby": true|false}
  POST /api/reboot
Any other path redirects to / (captive-portal behaviour in AP mode).
"""

import asyncio
import binascii
import gc
import json

import machine

import config
from util import ticks_ms, ticks_diff

_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_SECRET = ("wifi_password", "ap_password", "split_key")
# Settings the page may change, and whether a reboot is needed to apply.
_EDITABLE = {
    "unit": False, "layout": False, "rotate_s": False, "brightness": False,
    "status_page": False, "readings": False, "linked_probes": False,
    "upstream_node": False, "repeat_adverts": False, "meatnet_peer": False,
    "inbound_reserve": False, "auto_standby_min": False,
    "meater_bridge": False, "meater_probes": False,
    "meater_direct_adverts": False, "upstream_push": False,
    "linked_scan_duty": True, "screen": True,
    "battery_divider": False, "battery_pin": True, "hostname": True, "ap_password": True,
    "serial": True, "dis_model": True, "dis_firmware": False,
    "max_conn": True, "pins": True,
    "role": True, "split_peer": True, "split_key": True, "split_port": True,
}


def _sha1(data):
    import hashlib
    return hashlib.sha1(data).digest()


def _ws_frame(text):
    payload = text.encode()
    n = len(payload)
    if n < 126:
        header = bytes((0x81, n))
    elif n < 65536:
        header = bytes((0x81, 126, (n >> 8) & 0xFF, n & 0xFF))
    else:
        header = bytes((0x81, 127)) + n.to_bytes(8, "big")
    return header + payload


class WebUI:
    def __init__(self, node, wifi, cfg, log_state, serial, display_info="",
                 power=None, split=None, port=80):
        self.port = port
        self.node = node
        self.power = power
        self.split = split        # RemoteRadio (app unit) / SplitClient (field)
        self.wifi = wifi
        self.cfg = cfg
        self.log = log_state
        self.serial = serial
        self.display_info = display_info
        self._t0 = ticks_ms()

    async def run(self):
        await asyncio.start_server(self._handle, "0.0.0.0", self.port)
        while True:
            await asyncio.sleep(3600)

    # -- data -----------------------------------------------------------------

    def state(self):
        d = self.node.status()
        d.update({
            "serial": self.serial,
            "wifi": {"mode": self.wifi.mode, "ip": self.wifi.ip,
                     "ssid": self.cfg.get("wifi_ssid", ""),
                     "ap_ssid": self.wifi.ap_ssid},
            "heap": gc.mem_free(),
            "uptime": ticks_diff(ticks_ms(), self._t0) // 1000,
            "display": self.display_info,
            "power": self.power.status() if self.power else None,
            "role": self.cfg.get("role", "standalone"),
            "split": self.split.status() if self.split else None,
        })
        return d

    def public_config(self):
        out = {}
        for k, v in self.cfg.items():
            if k in _SECRET:
                out[k + "_set"] = bool(v)
            else:
                out[k] = v
        return out

    def update_config(self, req):
        reboot = False
        for k, v in req.items():
            if k not in _EDITABLE:
                continue
            if k == "pins" and isinstance(v, dict):
                self.cfg["pins"].update(v)
            else:
                self.cfg[k] = v
            reboot = reboot or _EDITABLE[k]
        if "dis_firmware" in req:
            self.node.t.set_firmware(self.cfg["dis_firmware"])
        if "linked_probes" in req:
            self.cfg["linked_probes"] = [s.upper() for s in self.cfg["linked_probes"]][:8]
        config.save(self.cfg)
        return {"ok": True, "reboot": reboot}

    # -- websocket ---------------------------------------------------------------

    async def _ws(self, writer, key):
        accept = binascii.b2a_base64(
            _sha1((key + _WS_GUID).encode())).strip().decode()
        writer.write(("HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        await writer.drain()
        last_seq = 0
        lines = self.log["lines"]
        if lines:
            last_seq = max(0, lines[-1][0] - 40)
        while True:
            writer.write(_ws_frame(json.dumps({"t": "status", "d": self.state()})))
            for seq, text in self.log["lines"]:
                if seq > last_seq:
                    writer.write(_ws_frame(json.dumps({"t": "log", "d": text})))
                    last_seq = seq
            await writer.drain()
            await asyncio.sleep_ms(1000)

    # -- http ------------------------------------------------------------------

    async def _handle(self, reader, writer):
        try:
            line = await reader.readline()
            if not line:
                return
            parts = line.decode().split(" ")
            if len(parts) < 2:
                return
            method, path = parts[0], parts[1].split("?")[0]
            clen = 0
            ws_key = None
            while True:
                h = await reader.readline()
                if h == b"\r\n" or not h:
                    break
                name, _, val = h.decode().partition(":")
                name = name.strip().lower()
                if name == "content-length":
                    clen = int(val.strip())
                elif name == "sec-websocket-key":
                    ws_key = val.strip()
            body = (await reader.readexactly(clen)).decode() if clen else ""

            if path == "/ws" and ws_key:
                await self._ws(writer, ws_key)
                return
            if path == "/":
                await self._send_file(writer, "www/index.html", "text/html")
            elif path.startswith("/api/"):
                try:
                    res = await self._api(method, path, json.loads(body) if body else {})
                except Exception as exc:
                    res = {"ok": False, "error": str(exc)}
                self._send(writer, "200 OK", "application/json", json.dumps(res))
                await writer.drain()
                if res.get("restart"):
                    await asyncio.sleep(1)
                    machine.reset()
            else:
                writer.write(b"HTTP/1.0 302 Found\r\nLocation: http://%s/\r\n"
                             b"Content-Length: 0\r\nConnection: close\r\n\r\n"
                             % (self.wifi.ip or "192.168.4.1").encode())
            await writer.drain()
        except Exception as exc:
            if not isinstance(exc, OSError):
                print("[web] error:", exc)
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    def _send(self, writer, status, ctype, body):
        if isinstance(body, str):
            body = body.encode()
        writer.write(b"HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     b"Cache-Control: no-store\r\nConnection: close\r\n\r\n" % (
                         status.encode(), ctype.encode(), len(body)))
        writer.write(body)

    async def _send_file(self, writer, path, ctype):
        import os
        size = os.stat(path)[6]
        writer.write(b"HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     b"Cache-Control: no-store\r\nConnection: close\r\n\r\n"
                     % (ctype.encode(), size))
        buf = bytearray(1024)
        with open(path, "rb") as f:
            while True:
                n = f.readinto(buf)
                if not n:
                    break
                writer.write(buf[:n])
                await writer.drain()

    async def _api(self, method, path, req):
        if path == "/api/state":
            return self.state()
        if path == "/api/config":
            if method == "POST":
                return self.update_config(req)
            return self.public_config()
        if path == "/api/wifi/scan":
            nets = self.wifi.scan()
            return {"ok": True, "networks": [{"ssid": s, "rssi": r, "secure": sec}
                                              for s, r, sec in nets]}
        if method != "POST":
            return {"ok": False, "error": "not found"}
        if path == "/api/wifi":
            ssid = (req.get("ssid") or "").strip()
            if not ssid:
                return {"ok": False, "error": "SSID required"}
            self.cfg["wifi_ssid"] = ssid
            if "password" in req:          # omitted = keep the saved one
                self.cfg["wifi_password"] = req["password"] or ""
            config.save(self.cfg)
            return {"ok": True, "restart": True}
        if path == "/api/power":
            if not self.power:
                return {"ok": False, "error": "power management unavailable"}
            if req.get("standby"):
                self.power.enter("manual")
            else:
                self.power.exit("manual")
            return {"ok": True, "power": self.power.status()}
        if path == "/api/reboot":
            return {"ok": True, "restart": True}
        return {"ok": False, "error": "not found"}
