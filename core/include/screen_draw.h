// The round screen's pages (port of screen.py's drawing): readings::Item
// pages laid out for the 240x240 GC9A01A as a list of primitives, which
// raster() turns into RGB565 rows a band at a time (a whole frame would need
// 115 KB of RAM on the ESP32).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "readings.h"

namespace screen_draw {

constexpr int W = 240, H = 240;

// tools/make_font.py's format: "MNF1", height, -, count u16, then count x
// (code u16, width u8, -, offset u32), then MONO_HLSB bitmaps.
class Font {
public:
    explicit Font(const uint8_t *data);
    bool ok() const { return height > 0; }
    int width(const std::string &s) const;
    bool has(const std::string &s) const;
    int glyph(uint8_t ch, const uint8_t **bits) const;    // width, 0 = missing
    int height = 0;

private:
    const uint8_t *data_;
    struct G {
        uint8_t width = 0;
        uint32_t off = 0;
    } g_[256];
};

struct Fonts {
    const Font *big, *mid, *med, *small;
};

// RGB565, byte-swapped: the panel takes it big-endian.
constexpr uint16_t color(int r, int g, int b)
{
    uint16_t v = static_cast<uint16_t>((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
    return static_cast<uint16_t>(v << 8 | v >> 8);
}

struct Prim {
    enum Kind { TEXT, DISC, RECT } kind;
    int x, y, w, h;             // text: position; disc: centre + radii; rect: box
    uint16_t color;
    const Font *font = nullptr;
    std::string text;
};

struct View {
    bool message = false;
    std::string text, sub;                  // message
    std::string layout, unit;               // "rotate"/"quad", "c"/"f"
    int page = 0, total = 0;
    std::vector<readings::Item> items;      // this page's
    bool operator==(const View &o) const
    {
        return message == o.message && text == o.text && sub == o.sub && layout == o.layout && unit == o.unit &&
               page == o.page && total == o.total && items == o.items;
    }
};

std::vector<Prim> draw(const Fonts &f, const View &v);
// Rows y0..y0+rows of the frame into buf (W x rows pixels), black background.
void raster(const std::vector<Prim> &prims, int y0, int rows, uint16_t *buf);

}  // namespace screen_draw
