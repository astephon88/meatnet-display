"""Pre-render TrueType fonts into the compact bitmap format screen.py reads.

Run on the host (needs Pillow):
    python tools/make_font.py

File format (little-endian):
    b"MNF1", height u8, reserved u8, count u16,
    count x (code u16, width u8, reserved u8, offset u32),
    glyph bitmaps, MONO_HLSB (rows padded to whole bytes), `height` rows each.
"""

import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "fonts")
TTF_DIR = "/usr/share/fonts/truetype/dejavu"

DIGITS = "0123456789-.:% CFhm" + chr(176)
ASCII = "".join(chr(c) for c in range(32, 127)) + chr(176)

FONTS = (
    # name, ttf, pixel size, characters
    ("big", "DejaVuSans-Bold.ttf", 68, DIGITS),
    ("mid", "DejaVuSans-Bold.ttf", 40, DIGITS),
    ("med", "DejaVuSans-Bold.ttf", 22, ASCII),
    ("small", "DejaVuSans.ttf", 16, ASCII),
)


def render(ttf_path, size, chars):
    font = ImageFont.truetype(ttf_path, size)
    ascent, descent = font.getmetrics()
    height = ascent + descent
    images = []
    top, bottom = height, 0
    for ch in chars:
        width = max(1, int(round(font.getlength(ch))))
        img = Image.new("1", (width, height), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=1)
        box = img.getbbox()
        if box:
            top = min(top, box[1])
            bottom = max(bottom, box[3])
        images.append((ch, width, img))
    # Crop every glyph to the rows any glyph uses (drops unused ascent).
    glyphs = []
    for ch, width, img in images:
        stride = (width + 7) // 8
        data = bytearray(stride * (bottom - top))
        px = img.load()
        for y in range(top, bottom):
            for x in range(width):
                if px[x, y]:
                    data[(y - top) * stride + (x >> 3)] |= 0x80 >> (x & 7)
        glyphs.append((ord(ch), width, bytes(data)))
    return bottom - top, glyphs


def write(path, height, glyphs):
    table = b""
    blob = b""
    for code, width, data in glyphs:
        table += struct.pack("<HBBI", code, width, 0, len(blob))
        blob += data
    with open(path, "wb") as f:
        f.write(b"MNF1" + struct.pack("<BBH", height, 0, len(glyphs)) + table + blob)
    return 8 + len(table) + len(blob)


def main():
    ttf_dir = sys.argv[1] if len(sys.argv) > 1 else TTF_DIR
    os.makedirs(OUT, exist_ok=True)
    for name, ttf, size, chars in FONTS:
        height, glyphs = render(os.path.join(ttf_dir, ttf), size, chars)
        n = write(os.path.join(OUT, name + ".bin"), height, glyphs)
        print("%s: %d glyphs, %d px high, %d bytes" % (name, len(glyphs), height, n))


if __name__ == "__main__":
    main()
