#include "screen_draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace screen_draw {

static constexpr uint16_t kBlack = 0, kWhite = color(255, 255, 255), kGrey = color(110, 110, 110),
                          kDim = color(160, 160, 160), kRed = color(230, 40, 40);

Font::Font(const uint8_t *data) : data_(data)
{
    if (memcmp(data, "MNF1", 4) != 0) return;
    height = data[4];
    int count = data[6] | data[7] << 8;
    const uint8_t *e = data + 8;
    uint32_t base = 8 + 8 * count;
    for (int i = 0; i < count; i++, e += 8) {
        int code = e[0] | e[1] << 8;
        if (code > 255) continue;
        g_[code].width = e[2];
        g_[code].off = base + (e[4] | e[5] << 8 | e[6] << 16 | static_cast<uint32_t>(e[7]) << 24);
    }
}

int Font::glyph(uint8_t ch, const uint8_t **bits) const
{
    *bits = data_ + g_[ch].off;
    return g_[ch].width;
}

int Font::width(const std::string &s) const
{
    int w = 0;
    for (char c : s) w += g_[static_cast<uint8_t>(c)].width;
    return w;
}

bool Font::has(const std::string &s) const
{
    for (char c : s)
        if (!g_[static_cast<uint8_t>(c)].width) return false;
    return true;
}

namespace {

// Usable width of the round panel across rows y..y+h.
int chord(int y, int h)
{
    int d = std::max(std::abs(y - 120), std::abs(y + h - 120));
    if (d >= 120) return 0;
    return static_cast<int>(2 * std::sqrt(120.0 * 120 - d * d)) - 12;
}

struct Canvas {
    const Fonts &f;
    std::vector<Prim> out;

    int text(const Font *font, const std::string &s, int y, uint16_t col, int cx = W / 2)
    {
        if (!font->has(s) && (font == f.big || font == f.mid)) font = f.med;
        int w = font->width(s);
        out.push_back({Prim::TEXT, cx - w / 2, y, w, font->height, col, font, s});
        return w;
    }
    const Font *fit(const std::string &s, int max_w, std::initializer_list<const Font *> fonts)
    {
        for (const Font *ft : fonts)
            if (ft->has(s) && ft->width(s) <= max_w) return ft;
        return *(fonts.end() - 1);
    }
    // The largest font that fits the circle (or max_w), truncating as a last resort.
    const Font *line(std::string s, int y, uint16_t col, std::initializer_list<const Font *> fonts, int cx = W / 2,
                     int max_w = 0)
    {
        for (const Font *ft : fonts) {
            int lim = max_w ? max_w : chord(y, ft->height);
            if (ft->has(s) && ft->width(s) <= lim) {
                text(ft, s, y, col, cx);
                return ft;
            }
        }
        const Font *ft = *(fonts.end() - 1);
        int lim = max_w ? max_w : chord(y, ft->height);
        while (!s.empty() && ft->width(s) > lim) s.pop_back();
        text(ft, s, y, col, cx);
        return ft;
    }
    void disc(int cx, int cy, int r, uint16_t col) { out.push_back({Prim::DISC, cx, cy, r, r, col, nullptr, {}}); }
    void rect(int x, int y, int w, int h, uint16_t col) { out.push_back({Prim::RECT, x, y, w, h, col, nullptr, {}}); }
};

uint16_t accent(const readings::Item &i) { return color(i.accent[0], i.accent[1], i.accent[2]); }

void single(Canvas &c, const readings::Item &item, int page, int total)
{
    if (item.alarm) {
        c.disc(120, 120, 119, kRed);
        c.disc(120, 120, 111, kBlack);
    }
    c.line(item.label, 34, accent(item), {c.f.med, c.f.small});
    uint16_t val_col = item.stale ? kGrey : kWhite;
    if (item.status) {
        int y = 72;
        for (size_t i = 0; i < item.subs.size(); i++) {
            const Font *ft = i == 0 ? c.line(item.subs[i], y, kWhite, {c.f.med, c.f.small})
                                    : c.line(item.subs[i], y, kDim, {c.f.small});
            y += ft->height + 10;
        }
    } else {
        std::string value = item.value + item.unit;
        const Font *ft = c.fit(value, 224, {c.f.big, c.f.mid, c.f.med});
        c.text(ft, value, 120 - ft->height / 2 - 8, val_col);
        int y = 150;
        for (size_t i = 0; i < item.subs.size() && i < 3; i++) {
            const std::string &s = item.subs[i];
            const Font *f2 = i == 0 && s.size() < 16 ? c.line(s, y, kWhite, {c.f.med, c.f.small})
                                                     : c.line(s, y, i ? kDim : kWhite, {c.f.small});
            y += f2->height + 4;
        }
    }
    if (total > 1) {
        int x0 = 120 - (total - 1) * 6;
        for (int i = 0; i < std::min(total, 12); i++) c.disc(x0 + i * 12, 220, 3, i == page ? kWhite : kGrey);
    }
}

void quad(Canvas &c, const std::vector<readings::Item> &items, int page, int total, const std::string &unit)
{
    c.rect(30, 120, 180, 1, kGrey);
    c.rect(120, 30, 1, 180, kGrey);
    static const int cells[4][2] = {{76, 44}, {164, 44}, {76, 128}, {164, 128}};
    for (size_t k = 0; k < items.size() && k < 4; k++) {
        const readings::Item &item = items[k];
        int cx = cells[k][0], y = cells[k][1];
        if (item.alarm) c.rect(cx - 40, y - 4, 80, 4, kRed);
        c.line(item.label, y, accent(item), {c.f.small}, cx, 80);
        const Font *ft = c.fit(item.short_value, 84, {c.f.mid, c.f.med});
        c.text(ft, item.short_value, y + 22, item.stale ? kGrey : kWhite, cx);
    }
    c.text(c.f.small, std::string("\xB0") + (unit == "f" ? "F" : "C"), 214, kDim);
    if (total > 1) c.text(c.f.small, std::to_string(page + 1) + "/" + std::to_string(total), 14, kDim);
}

}  // namespace

std::vector<Prim> draw(const Fonts &f, const View &v)
{
    Canvas c{f, {}};
    if (v.message) {
        c.text(f.med, v.text, 96, kWhite);
        if (!v.sub.empty()) c.text(f.small, v.sub, 128, kDim);
    } else if (!v.items.empty()) {
        if (v.layout == "quad" && !v.items[0].status) quad(c, v.items, v.page, v.total, v.unit);
        else single(c, v.items[0], v.page, v.total);
    }
    return std::move(c.out);
}

void raster(const std::vector<Prim> &prims, int y0, int rows, uint16_t *buf)
{
    std::fill(buf, buf + W * rows, kBlack);
    int y1 = y0 + rows;
    for (const Prim &p : prims) {
        if (p.kind == Prim::RECT) {
            for (int y = std::max(p.y, y0); y < std::min(p.y + p.h, y1); y++)
                for (int x = std::max(p.x, 0); x < std::min(p.x + p.w, W); x++) buf[(y - y0) * W + x] = p.color;
        } else if (p.kind == Prim::DISC) {
            long r2 = static_cast<long>(p.w) * p.w;
            for (int y = std::max(p.y - p.h, y0); y < std::min(p.y + p.h + 1, y1); y++) {
                long dy = y - p.y;
                for (int x = std::max(p.x - p.w, 0); x < std::min(p.x + p.w + 1, W); x++) {
                    long dx = x - p.x;
                    if (dx * dx + dy * dy <= r2) buf[(y - y0) * W + x] = p.color;
                }
            }
        } else {
            int h = p.font->height;
            if (p.y >= y1 || p.y + h <= y0) continue;
            int x = p.x;
            for (char ch : p.text) {
                const uint8_t *bits;
                int gw = p.font->glyph(static_cast<uint8_t>(ch), &bits);
                int stride = (gw + 7) / 8;
                for (int row = std::max(0, y0 - p.y); row < h && p.y + row < y1; row++) {
                    uint16_t *line = buf + (p.y + row - y0) * W;
                    const uint8_t *src = bits + row * stride;
                    for (int col = 0; col < gw; col++)
                        if (x + col >= 0 && x + col < W && (src[col >> 3] & (0x80 >> (col & 7))))
                            line[x + col] = p.color;
                }
                x += gw;
            }
        }
    }
}

}  // namespace screen_draw
