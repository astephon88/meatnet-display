"""Round-screen renderer (MicroPython): draws readings.py items into an
RGB565 framebuf and pushes it to the GC9A01A in row bands.

Only redraws when what's on screen would change, and yields to the asyncio
loop between bands so BLE servicing isn't starved.
"""

import asyncio
import framebuf
import struct

import readings
from util import ticks_ms, ticks_diff

W = 240
H = 240
BAND = 24                 # rows pushed per SPI transfer


def color(r, g, b):
    """RGB -> RGB565 stored byte-swapped, so framebuf's little-endian pixels
    come out in the big-endian order the panel expects."""
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v & 0xFF) << 8) | (v >> 8)


BLACK = 0
WHITE = color(255, 255, 255)
GREY = color(110, 110, 110)
DIM = color(160, 160, 160)
RED = color(230, 40, 40)


class Font:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] != b"MNF1":
            raise ValueError("bad font " + path)
        self.height = data[4]
        count = struct.unpack_from("<H", data, 6)[0]
        base = 8 + 8 * count
        self.data = memoryview(bytearray(data))
        self.glyphs = {}
        for i in range(count):
            code, width, _r, off = struct.unpack_from("<HBBI", data, 8 + 8 * i)
            self.glyphs[code] = (width, base + off)
        self._cache = {}

    def has(self, text):
        for ch in text:
            if ord(ch) not in self.glyphs:
                return False
        return True

    def width(self, text):
        w = 0
        for ch in text:
            g = self.glyphs.get(ord(ch))
            if g:
                w += g[0]
        return w

    def _glyph(self, code):
        fb = self._cache.get(code)
        if fb is None:
            g = self.glyphs.get(code)
            if g is None:
                return None, 0
            width, off = g
            size = ((width + 7) // 8) * self.height
            fb = framebuf.FrameBuffer(self.data[off:off + size], width,
                                      self.height, framebuf.MONO_HLSB)
            self._cache[code] = (fb, width)
            return fb, width
        return fb

    def draw(self, target, text, x, y, palette):
        for ch in text:
            fb, width = self._glyph(ord(ch))
            if fb is not None:
                target.blit(fb, x, y, 0, palette)
            x += width
        return x


class Screen:
    def __init__(self, display, node, wifi, cfg, font_dir="fonts", power=None):
        self.display = display
        self.power = power
        self._asleep = False
        self.node = node
        self.wifi = wifi
        self.cfg = cfg
        self.buf = bytearray(W * H * 2)
        self.fb = framebuf.FrameBuffer(self.buf, W, H, framebuf.RGB565)
        self.mv = memoryview(self.buf)
        self.big = Font(font_dir + "/big.bin")
        self.mid = Font(font_dir + "/mid.bin")
        self.med = Font(font_dir + "/med.bin")
        self.small = Font(font_dir + "/small.bin")
        self._pal = framebuf.FrameBuffer(bytearray(4), 2, 1, framebuf.RGB565)
        self._sig = None
        self._page = 0
        self._page_at = ticks_ms()
        self._brightness = None
        self.message = None      # (text, sub) shown instead of readings

    # -- drawing helpers ------------------------------------------------------

    def _text(self, font, text, y, col, x=None, cx=W // 2):
        if not font.has(text):
            font = self.med if font in (self.big, self.mid) else font
        self._pal.pixel(1, 0, col)
        w = font.width(text)
        if x is None:
            x = cx - w // 2
        font.draw(self.fb, text, x, y, self._pal)
        return w

    def _fit(self, text, max_w, fonts):
        for f in fonts:
            if f.has(text) and f.width(text) <= max_w:
                return f
        return fonts[-1]

    @staticmethod
    def chord(y, h):
        """Usable width of the round panel across rows y..y+h."""
        d = max(abs(y - 120), abs(y + h - 120))
        if d >= 120:
            return 0
        return int(2 * (120 * 120 - d * d) ** 0.5) - 12

    def _line(self, text, y, col, fonts, cx=W // 2, max_w=None):
        """Draw text with the largest font that fits the circle (or max_w),
        truncating as a last resort. Returns the font used."""
        for f in fonts:
            lim = max_w or self.chord(y, f.height)
            if f.has(text) and f.width(text) <= lim:
                self._text(f, text, y, col, cx=cx)
                return f
        f = fonts[-1]
        lim = max_w or self.chord(y, f.height)
        while text and f.width(text) > lim:
            text = text[:-1]
        self._text(f, text, y, col, cx=cx)
        return f

    # -- layouts ----------------------------------------------------------------

    def _draw_single(self, item, page, total):
        fb = self.fb
        if item["alarm"]:
            fb.ellipse(120, 120, 119, 119, RED, True)
            fb.ellipse(120, 120, 111, 111, BLACK, True)
        accent = color(*item["accent"])
        self._line(item["label"], 34, accent, (self.med, self.small))
        val_col = GREY if item["stale"] else WHITE
        if item.get("status"):
            y = 72
            for i, s in enumerate(item["subs"]):
                f = self._line(s, y, WHITE if i == 0 else DIM,
                               (self.med, self.small) if i == 0 else (self.small,))
                y += f.height + 10
        else:
            value = item["value"] + item["unit"]
            f = self._fit(value, 224, (self.big, self.mid, self.med))
            self._text(f, value, 120 - f.height // 2 - 8, val_col)
            y = 150
            for i, s in enumerate(item["subs"][:3]):
                f = self._line(s, y, DIM if i else WHITE,
                               (self.med, self.small) if i == 0 and len(s) < 16
                               else (self.small,))
                y += f.height + 4
        if total > 1:
            x0 = 120 - (total - 1) * 6
            for i in range(min(total, 12)):
                c = WHITE if i == page else GREY
                fb.ellipse(x0 + i * 12, 220, 3, 3, c, True)

    def _draw_quad(self, items, page, total):
        fb = self.fb
        fb.hline(30, 120, 180, GREY)
        fb.vline(120, 30, 180, GREY)
        cells = ((76, 44), (164, 44), (76, 128), (164, 128))
        unit = "F" if self.cfg.get("unit") == "f" else "C"
        for item, (cx, y) in zip(items, cells):
            if item["alarm"]:
                fb.fill_rect(cx - 40, y - 4, 80, 4, RED)
            self._line(item["label"], y, color(*item["accent"]), (self.small,),
                       cx=cx, max_w=80)
            v = item["short"]
            f = self._fit(v, 84, (self.mid, self.med))
            self._text(f, v, y + 22, GREY if item["stale"] else WHITE, cx=cx)
        self._text(self.small, chr(176) + unit, 214, DIM)
        if total > 1:
            self._text(self.small, "%d/%d" % (page + 1, total), 14, DIM)

    def _draw_message(self, text, sub):
        self._text(self.med, text, 96, WHITE)
        if sub:
            self._text(self.small, sub, 128, DIM)

    # -- loop ---------------------------------------------------------------------

    def _view(self):
        if self.message:
            return ("msg",) + tuple(self.message), None
        status = self.node.status()
        if self.power is not None:
            status["battery"] = self.power.battery
        w = self.wifi
        items = readings.build_items(self.cfg, status, w.mode if w else None,
                                     w.ip if w else None, w.ap_ssid if w else "")
        pgs = readings.pages(self.cfg, items)
        if not pgs:
            return ("msg", "Waiting for", "MeatNet devices"), None
        now = ticks_ms()
        if ticks_diff(now, self._page_at) >= int(self.cfg.get("rotate_s", 5)) * 1000:
            self._page_at = now
            self._page += 1
        self._page %= len(pgs)
        view = (self.cfg.get("layout"), self._page, len(pgs), pgs[self._page])
        return view, pgs

    def _sig_of(self, view):
        # Cheap change detection: the repr of what will be drawn.
        return repr(view)

    async def _push(self):
        stride = W * 2
        for y in range(0, H, BAND):
            self.display.blit(self.mv[y * stride:(y + BAND) * stride], 0, y, W, BAND)
            await asyncio.sleep_ms(0)

    async def run(self):
        while True:
            if self.power is not None and self.power.standby:
                if not self._asleep:
                    self._asleep = True
                    self.display.sleep(True)
                await asyncio.sleep_ms(500)
                continue
            if self._asleep:
                self._asleep = False
                self.display.sleep(False)
                self._sig = None          # panel RAM may be lost: redraw
                self._brightness = None
            b = self.cfg.get("brightness", 80)
            if b != self._brightness:
                self._brightness = b
                self.display.brightness(b)
            try:
                view, _ = self._view()
                sig = self._sig_of(view)
                if sig != self._sig:
                    self._sig = sig
                    self.fb.fill(BLACK)
                    if view[0] == "msg":
                        self._draw_message(view[1], view[2])
                    elif view[0] == "quad" and not view[3][0].get("status"):
                        self._draw_quad(view[3], view[1], view[2])
                    else:
                        self._draw_single(view[3][0], view[1], view[2])
                    await self._push()
            except Exception as exc:
                print("[screen] error:", exc)
            await asyncio.sleep_ms(500)
