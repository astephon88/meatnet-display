"""Render screen.py pages to PNGs on the host (no hardware), using a tiny
pure-Python stand-in for MicroPython's framebuf. Needs Pillow.

    python tools/preview.py [out_dir]
"""

import asyncio
import os
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

fb_mod = types.ModuleType("framebuf")
fb_mod.RGB565 = 1
fb_mod.MONO_HLSB = 3


class FrameBuffer:
    def __init__(self, buf, w, h, fmt):
        self.buf, self.w, self.h, self.fmt = buf, w, h, fmt

    def pixel(self, x, y, c=None):
        if not (0 <= x < self.w and 0 <= y < self.h):
            return 0
        if self.fmt == 3:
            stride = (self.w + 7) // 8
            return (self.buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
        i = (y * self.w + x) * 2
        if c is None:
            return self.buf[i] | self.buf[i + 1] << 8
        self.buf[i] = c & 0xFF
        self.buf[i + 1] = c >> 8

    def fill_rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            for xx in range(max(0, x), min(self.w, x + w)):
                self.pixel(xx, yy, c)

    def fill(self, c):
        self.fill_rect(0, 0, self.w, self.h, c)

    def hline(self, x, y, w, c):
        self.fill_rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill_rect(x, y, 1, h, c)

    def ellipse(self, cx, cy, rx, ry, c, f=False):
        for y in range(-ry, ry + 1):
            for x in range(-rx, rx + 1):
                d = (x * x) / (rx * rx or 1) + (y * y) / (ry * ry or 1)
                if d <= 1 and (f or d > 0.85):
                    self.pixel(cx + x, cy + y, c)

    def blit(self, src, x, y, key=-1, palette=None):
        for yy in range(src.h):
            for xx in range(src.w):
                v = src.pixel(xx, yy)
                if v == key:
                    continue
                if palette is not None:
                    v = palette.pixel(v, 0)
                self.pixel(x + xx, y + yy, v)


fb_mod.FrameBuffer = FrameBuffer
sys.modules["framebuf"] = fb_mod

import asyncio as _aio  # noqa: E402
if not hasattr(_aio, "sleep_ms"):
    _aio.sleep_ms = lambda ms: _aio.sleep(ms / 1000)
import screen  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402


class FakeDisplay:
    def __init__(self):
        self.buf = bytearray(240 * 240 * 2)

    def brightness(self, pct):
        pass

    def blit(self, mv, x, y, w, h):
        start = (y * 240 + x) * 2
        self.buf[start:start + len(mv)] = bytes(mv)


def to_png(buf, path):
    img = Image.new("RGB", (240, 240))
    px = img.load()
    for y in range(240):
        for x in range(240):
            i = (y * 240 + x) * 2
            v = buf[i] << 8 | buf[i + 1]          # panel byte order
            px[x, y] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
    mask = Image.new("L", (240, 240), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 239, 239), fill=255)
    out = Image.new("RGB", (240, 240), (40, 40, 40))
    out.paste(img, (0, 0), mask)
    out.save(path)


STATUS = {
    "probes": [
        {"serial": "1000ABCD", "id": 1, "color": 0, "mode": 0, "core_c": 57.3,
         "surface_c": 71.0, "ambient_c": 121.5, "instant_c": None,
         "temps": [57.3] * 8, "low_battery": False, "src": "link", "hops": 0,
         "rssi": -61, "age_s": 1, "alarming": False,
         "prediction": {"state": 3, "type": 1, "seconds": 4380, "setpoint_c": 63.0}},
        {"serial": "1000BEEF", "id": 2, "color": 1, "mode": 0, "core_c": 72.0,
         "surface_c": 80.0, "ambient_c": 150.0, "instant_c": None,
         "temps": [72.0] * 8, "low_battery": True, "src": "advert", "hops": 0,
         "rssi": -78, "age_s": 2, "alarming": True, "prediction": None},
    ],
    "devices": [{"serial": "GAUGE00001", "ptype": 3, "name": "Gauge",
                 "temp_c": 135.0, "rssi": -70, "age_s": 3}],
    "links": [{"kind": "probe", "serial": "1000ABCD", "state": "ready"}],
    "inbound": 1,
}


class FakeNode:
    def status(self):
        return STATUS


class FakeWifi:
    def __init__(self, mode):
        self.mode = mode
        self.ip = "192.168.4.1" if mode == "ap" else "192.168.1.57"
        self.ap_ssid = "MeatNet-B2C4"


async def render(cfg, wifi_mode, out, n_pages):
    disp = FakeDisplay()
    s = screen.Screen(disp, FakeNode(), FakeWifi(wifi_mode), cfg,
                      font_dir=os.path.join(ROOT, "fonts"))
    paths = []
    for i in range(n_pages):
        s._page = i
        s._page_at = screen.ticks_ms()
        s._sig = None
        view, pgs = s._view()
        s.fb.fill(screen.BLACK)
        if view[0] == "msg":
            s._draw_message(view[1], view[2])
        elif view[0] == "quad" and not view[3][0].get("status"):
            s._draw_quad(view[3], view[1], view[2])
        else:
            s._draw_single(view[3][0], view[1], view[2])
        await s._push()
        p = os.path.join(out, "%s_%d.png" % (cfg.get("layout", "rotate") + "_" + wifi_mode, i))
        to_png(disp.buf, p)
        paths.append(p)
    return paths


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "preview")
    os.makedirs(out, exist_ok=True)
    readings = [{"dev": "probe:1000ABCD", "field": "core"},
                {"dev": "probe:1000ABCD", "field": "eta"},
                {"dev": "probe:1000BEEF", "field": "core"},
                {"dev": "node:GAUGE00001", "field": "temp"}]
    cfg = {"unit": "f", "layout": "rotate", "readings": readings, "status_page": True}
    paths = asyncio.run(render(cfg, "sta", out, 5))
    paths += asyncio.run(render(dict(cfg, layout="quad"), "sta", out, 1))
    paths += asyncio.run(render({"unit": "c", "readings": []}, "ap", out, 1))
    for p in paths:
        print(p)


if __name__ == "__main__":
    main()
