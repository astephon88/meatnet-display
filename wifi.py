"""WiFi manager: station mode from config, falling back to a setup access
point (with a catch-all DNS responder so phones open the setup page)."""

import asyncio
import socket

import network

from util import ticks_ms, ticks_diff

STA_TIMEOUT_MS = 20000
RETRY_MS = 60000
AP_IP = "192.168.4.1"


class WifiManager:
    def __init__(self, cfg, serial, log=print):
        self.cfg = cfg
        self.log = log
        self.ap_ssid = "MeatNet-" + serial[-4:]
        self.mode = "off"            # "connecting", "sta", "ap"
        self.ip = None
        self.ntp_ok = False
        self.sta = network.WLAN(network.STA_IF)
        self.ap = network.WLAN(network.AP_IF)
        hostname = cfg.get("hostname") or "meatnet"
        try:
            network.hostname(hostname)
        except Exception:
            pass
        self._dns = None

    @property
    def connected(self):
        return self.sta.isconnected()

    async def _try_sta(self):
        ssid = self.cfg.get("wifi_ssid")
        if not ssid:
            return False
        self.sta.active(True)
        if self.sta.isconnected():
            return True
        self.mode = "connecting" if self.mode != "ap" else "ap"
        self.log("[wifi] connecting to '%s'" % ssid)
        try:
            self.sta.connect(ssid, self.cfg.get("wifi_password", ""))
        except OSError as exc:
            self.log("[wifi] connect error: %s" % exc)
            return False
        t0 = ticks_ms()
        while ticks_diff(ticks_ms(), t0) < STA_TIMEOUT_MS:
            if self.sta.isconnected():
                return True
            await asyncio.sleep_ms(250)
        try:
            self.sta.disconnect()
        except OSError:
            pass
        return False

    def _start_ap(self):
        self.ap.active(True)
        pw = self.cfg.get("ap_password") or ""
        try:
            if len(pw) >= 8:
                self.ap.config(essid=self.ap_ssid, password=pw,
                               authmode=network.AUTH_WPA2_PSK)
            else:
                self.ap.config(essid=self.ap_ssid, authmode=network.AUTH_OPEN)
        except Exception as exc:
            self.log("[wifi] AP config error: %s" % exc)
        self.mode = "ap"
        self.ip = AP_IP
        self.log("[wifi] setup access point '%s' at http://%s/" % (self.ap_ssid, AP_IP))
        self._start_dns()

    def _stop_ap(self):
        self._stop_dns()
        self.ap.active(False)

    def _sync_time(self):
        try:
            import ntptime
            ntptime.settime()
            self.ntp_ok = True
            self.log("[time] clock synced via NTP")
        except Exception:
            pass

    def _on_sta_up(self):
        self.mode = "sta"
        self.ip = self.sta.ifconfig()[0]
        self.log("[wifi] connected, http://%s/ (%s.local)" % (
            self.ip, self.cfg.get("hostname") or "meatnet"))
        if not self.ntp_ok:
            self._sync_time()

    async def run(self):
        if await self._try_sta():
            self._on_sta_up()
        else:
            self._start_ap()
        last_try = ticks_ms()
        while True:
            await asyncio.sleep_ms(500)
            if self._dns:
                self._serve_dns()
            if self.mode == "sta":
                if not self.sta.isconnected():
                    self.log("[wifi] connection lost, reconnecting")
                    self.mode = "connecting"
                    if await self._try_sta():
                        self._on_sta_up()
                    else:
                        self._start_ap()
                        last_try = ticks_ms()
            elif self.mode == "ap" and self.cfg.get("wifi_ssid"):
                # Keep retrying the configured network unless someone is
                # using the setup AP right now.
                if (ticks_diff(ticks_ms(), last_try) > RETRY_MS
                        and not self._ap_has_clients()):
                    last_try = ticks_ms()
                    if await self._try_sta():
                        self._stop_ap()
                        self._on_sta_up()

    def _ap_has_clients(self):
        try:
            return len(self.ap.status("stations")) > 0
        except Exception:
            return False

    def scan(self):
        """Blocking (~2 s) scan -> [(ssid, rssi, secure)], strongest first."""
        self.sta.active(True)
        best = {}
        try:
            for ssid, _bssid, _ch, rssi, auth, _hidden in self.sta.scan():
                try:
                    name = ssid.decode()
                except Exception:
                    continue
                if name and (name not in best or best[name][0] < rssi):
                    best[name] = (rssi, auth != 0)
        except OSError as exc:
            self.log("[wifi] scan failed: %s" % exc)
        out = [(k, v[0], v[1]) for k, v in best.items()]
        out.sort(key=lambda x: -x[1])
        return out

    # -- captive DNS: answer every A query with our AP address ---------------

    def _start_dns(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", 53))
            s.setblocking(False)
            self._dns = s
        except Exception as exc:
            self.log("[wifi] DNS responder failed: %s" % exc)

    def _stop_dns(self):
        if self._dns:
            try:
                self._dns.close()
            except Exception:
                pass
            self._dns = None

    def _serve_dns(self):
        for _ in range(4):
            try:
                data, addr = self._dns.recvfrom(256)
            except OSError:
                return
            if len(data) < 12:
                continue
            # Copy the question, answer with a pointer to it and our IP.
            end = 12
            while end < len(data) and data[end]:
                end += data[end] + 1
            question = data[12:end + 5]
            resp = (data[0:2] + b"\x81\x80" + data[4:6] + b"\x00\x01"
                    + b"\x00\x00\x00\x00" + question
                    + b"\xC0\x0C\x00\x01\x00\x01\x00\x00\x00\x3C\x00\x04"
                    + bytes(int(x) for x in AP_IP.split(".")))
            try:
                self._dns.sendto(resp, addr)
            except OSError:
                pass
