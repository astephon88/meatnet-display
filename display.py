"""GC9A01A 240x240 round LCD output.

Uses the C `gc9a01` module (russhughes/gc9a01_mpy) when the firmware has it,
otherwise a small pure-MicroPython SPI driver. Either way the app draws into
an RGB565 framebuf (bytes in display order, see screen.color) and pushes row
bands with blit().
"""

import time

from machine import Pin, SPI, PWM

WIDTH = 240
HEIGHT = 240

# MADCTL per rotation (BGR panel), from the TFT_eSPI GC9A01 driver.
_MADCTL = (0x08, 0x68, 0xC8, 0xA8)

# Init sequence: (command, data bytes, delay ms)
_INIT = (
    (0xEF, b"", 0), (0xEB, b"\x14", 0), (0xFE, b"", 0), (0xEF, b"", 0),
    (0xEB, b"\x14", 0), (0x84, b"\x40", 0), (0x85, b"\xFF", 0),
    (0x86, b"\xFF", 0), (0x87, b"\xFF", 0), (0x88, b"\x0A", 0),
    (0x89, b"\x21", 0), (0x8A, b"\x00", 0), (0x8B, b"\x80", 0),
    (0x8C, b"\x01", 0), (0x8D, b"\x01", 0), (0x8E, b"\xFF", 0),
    (0x8F, b"\xFF", 0), (0xB6, b"\x00\x20", 0), (0x3A, b"\x05", 0),
    (0x90, b"\x08\x08\x08\x08", 0), (0xBD, b"\x06", 0), (0xBC, b"\x00", 0),
    (0xFF, b"\x60\x01\x04", 0), (0xC3, b"\x13", 0), (0xC4, b"\x13", 0),
    (0xC9, b"\x22", 0), (0xBE, b"\x11", 0), (0xE1, b"\x10\x0E", 0),
    (0xDF, b"\x21\x0C\x02", 0),
    (0xF0, b"\x45\x09\x08\x08\x26\x2A", 0),
    (0xF1, b"\x43\x70\x72\x36\x37\x6F", 0),
    (0xF2, b"\x45\x09\x08\x08\x26\x2A", 0),
    (0xF3, b"\x43\x70\x72\x36\x37\x6F", 0),
    (0xED, b"\x1B\x0B", 0), (0xAE, b"\x77", 0), (0xCD, b"\x63", 0),
    (0x70, b"\x07\x07\x04\x0E\x0F\x09\x07\x08\x03", 0), (0xE8, b"\x34", 0),
    (0x62, b"\x18\x0D\x71\xED\x70\x70\x18\x0F\x71\xEF\x70\x70", 0),
    (0x63, b"\x18\x11\x71\xF1\x70\x70\x18\x13\x71\xF3\x70\x70", 0),
    (0x64, b"\x28\x29\xF1\x01\xF1\x00\x07", 0),
    (0x66, b"\x3C\x00\xCD\x67\x45\x45\x10\x00\x00\x00", 0),
    (0x67, b"\x00\x3C\x00\x00\x00\x01\x54\x10\x32\x98", 0),
    (0x74, b"\x10\x85\x80\x00\x00\x4E\x00", 0), (0x98, b"\x3E\x07", 0),
    (0x35, b"", 0), (0x21, b"", 0), (0x11, b"", 120), (0x29, b"", 20),
)


class _PyGC9A01:
    def __init__(self, spi, dc, cs, rst, rotation=0):
        self.spi = spi
        self.dc = dc
        self.cs = cs
        self.rst = rst
        self.rotation = rotation

    def _cmd(self, c, data=b""):
        self.cs(0)
        self.dc(0)
        self.spi.write(bytes((c,)))
        if data:
            self.dc(1)
            self.spi.write(data)
        self.cs(1)

    def init(self):
        self.cs(1)
        if self.rst is not None:
            self.rst(1)
            time.sleep_ms(10)
            self.rst(0)
            time.sleep_ms(10)
            self.rst(1)
            time.sleep_ms(120)
        for c, data, delay in _INIT:
            self._cmd(c, data)
            if delay:
                time.sleep_ms(delay)
        self._cmd(0x36, bytes((_MADCTL[self.rotation % 4],)))

    def sleep_mode(self, on):
        if on:
            self._cmd(0x28)          # display off
            self._cmd(0x10)          # sleep in
            time.sleep_ms(5)
        else:
            self._cmd(0x11)          # sleep out
            time.sleep_ms(120)
            self._cmd(0x29)          # display on

    def blit_buffer(self, buf, x, y, w, h):
        x1 = x + w - 1
        y1 = y + h - 1
        self._cmd(0x2A, bytes((x >> 8, x & 0xFF, x1 >> 8, x1 & 0xFF)))
        self._cmd(0x2B, bytes((y >> 8, y & 0xFF, y1 >> 8, y1 & 0xFF)))
        self.cs(0)
        self.dc(0)
        self.spi.write(b"\x2C")
        self.dc(1)
        self.spi.write(buf)
        self.cs(1)


class Display:
    def __init__(self, pins):
        self.pins = pins
        spi = SPI(pins.get("spi", 2), baudrate=pins.get("baud", 80000000),
                  polarity=0, phase=0, sck=Pin(pins["sck"]), mosi=Pin(pins["mosi"]))
        dc = Pin(pins["dc"], Pin.OUT)
        cs = Pin(pins["cs"], Pin.OUT)
        rst = Pin(pins["rst"], Pin.OUT) if pins.get("rst", -1) >= 0 else None
        rotation = pins.get("rotation", 0)
        self.driver = "python"
        try:
            import gc9a01
            self.dev = gc9a01.GC9A01(spi, WIDTH, HEIGHT, reset=rst, cs=cs, dc=dc,
                                     rotation=rotation)
            self.driver = "gc9a01 (C module)"
        except (ImportError, AttributeError, TypeError):
            self.dev = _PyGC9A01(spi, dc, cs, rst, rotation)
        self.dev.init()
        self._bl = None
        if pins.get("bl", -1) >= 0:
            try:
                self._bl = PWM(Pin(pins["bl"], Pin.OUT), freq=2000)
            except Exception:
                Pin(pins["bl"], Pin.OUT).value(1)
        self.brightness(80)

    def brightness(self, pct):
        pct = max(0, min(100, int(pct)))
        if self._bl is not None:
            self._bl.duty_u16(pct * 65535 // 100)

    def sleep(self, on):
        """Backlight off + panel sleep (standby), or wake the panel."""
        if on:
            self.brightness(0)
        try:
            self.dev.sleep_mode(on)
        except AttributeError:
            pass

    def blit(self, buf, x, y, w, h):
        self.dev.blit_buffer(buf, x, y, w, h)
