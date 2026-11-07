"""Standby mode and battery monitoring.

Standby keeps WiFi up (in power-save mode) so the web UI can wake the
device, and idles everything else:
  - BLE: stop advertising, drop inbound and outbound connections, scan at a
    few percent duty (just enough to notice a probe leaving its charger);
  - screen: backlight off, panel in sleep mode;
  - CPU: 80 MHz instead of 240 MHz.
BLE itself stays active: ble.active(False) can hang the ESP32.

Auto standby: after `auto_standby_min` minutes with no probe heard (e.g. all
probes in the charger), enter standby; wake as soon as a probe advertises
again. A manual standby (web UI) only ends from the web UI.

Battery: the Waveshare board feeds the cell through a 200k/100k divider to
GPIO1, so V(bat) = 3 x V(adc).
"""

from util import ticks_ms, ticks_diff

# Approximate LiPo open-circuit voltage -> charge %.
_CURVE = ((4.20, 100), (4.10, 90), (4.00, 80), (3.90, 66), (3.80, 52),
          (3.70, 36), (3.60, 20), (3.50, 10), (3.40, 5), (3.30, 0))
BATTERY_EVERY_MS = 10000
WAKE_FRESH_MS = 10000


def battery_percent(v):
    if v is None:
        return None
    if v >= _CURVE[0][0]:
        return 100
    for (v1, p1), (v0, p0) in zip(_CURVE, _CURVE[1:]):
        if v >= v0:
            return int(round(p0 + (p1 - p0) * (v - v0) / (v1 - v0)))
    return 0


class PowerManager:
    def __init__(self, cfg, transport, node, display=None, log=print, adc=None):
        self.cfg = cfg
        self.t = transport
        self.node = node
        self.display = display
        self.log = log
        self.standby = False
        self.reason = None            # "manual" or "auto"
        self.since = ticks_ms()
        self._last_probe = ticks_ms()
        self._adc = adc
        self._adc_ok = adc is not None
        self._bat_at = None
        self.battery = {"present": False, "v": None, "pct": None}
        if adc is None:
            self._init_adc()
        if cfg.get("standby"):
            self.enter("manual", persist=False)

    # -- battery --------------------------------------------------------------

    def _init_adc(self):
        pin = self.cfg.get("battery_pin", 1)
        if pin is None or pin < 0:
            return
        try:
            from machine import ADC, Pin
            self._adc = ADC(Pin(pin), atten=ADC.ATTN_11DB)
            self._adc_ok = True
        except Exception as exc:
            self.log("[power] battery ADC unavailable: %s" % exc)

    def read_battery(self):
        if not self._adc_ok:
            return self.battery
        try:
            total = 0
            for _ in range(8):
                total += self._adc.read_uv()
            v = total / 8 / 1e6 * float(self.cfg.get("battery_divider", 3.0))
        except Exception:
            return self.battery
        present = v > 2.5             # floating/absent cell reads near 0
        self.battery = {"present": present, "v": round(v, 2) if present else None,
                        "pct": battery_percent(v) if present else None}
        return self.battery

    # -- standby --------------------------------------------------------------

    def _apply(self, on):
        self.t.set_standby(on)
        self.node.standby = on
        try:
            import machine
            machine.freq(80_000_000 if on else 240_000_000)
        except Exception:
            pass
        try:
            import network
            sta = network.WLAN(network.STA_IF)
            sta.config(pm=sta.PM_POWERSAVE if on else sta.PM_PERFORMANCE)
        except Exception:
            pass

    def enter(self, reason="manual", persist=True):
        if self.standby:
            if reason == "manual" and self.reason != "manual":
                self.reason = "manual"
                if persist:
                    self._persist(True)
            return
        self.standby = True
        self.reason = reason
        self.since = ticks_ms()
        self._apply(True)
        self.log("[power] standby (%s)" % reason)
        if persist and reason == "manual":
            self._persist(True)

    def exit(self, why="manual"):
        if not self.standby:
            return
        self.standby = False
        self.reason = None
        self.since = ticks_ms()
        self._last_probe = ticks_ms()      # don't drop straight back to auto
        self._apply(False)
        self.log("[power] awake (%s)" % why)
        if why == "manual":
            self._persist(False)

    def _persist(self, on):
        if bool(self.cfg.get("standby")) != on:
            self.cfg["standby"] = on
            try:
                import config
                config.save(self.cfg)
            except Exception:
                pass

    def _probe_heard(self, now):
        for entry in list(self.t.direct.values()):
            if ticks_diff(now, entry[1]) < WAKE_FRESH_MS:
                return True
        return False

    def tick(self, now=None):
        now = ticks_ms() if now is None else now
        if self._bat_at is None or ticks_diff(now, self._bat_at) >= BATTERY_EVERY_MS:
            self._bat_at = now
            self.read_battery()
        heard = self._probe_heard(now) or bool(self.t.repeated)
        if heard:
            self._last_probe = now
        mins = int(self.cfg.get("auto_standby_min", 0) or 0)
        if self.standby:
            if self.reason == "auto" and self._probe_heard(now):
                self.exit("probe heard")
        elif mins > 0 and ticks_diff(now, self._last_probe) >= mins * 60000:
            self.enter("auto")

    def status(self):
        return {"standby": self.standby, "reason": self.reason,
                "since_s": ticks_diff(ticks_ms(), self.since) // 1000,
                "auto_standby_min": int(self.cfg.get("auto_standby_min", 0) or 0),
                "battery": self.battery}
