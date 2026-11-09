// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "app/rgba_draw.h"

#include <algorithm>
#include <cstdlib>

#include "data/palette.h"
#include "data/shape.h"
#include "gfx/font.h"
#include "gfx/surface.h"

namespace pg {

namespace {
// Draws with `draw` into two surfaces cleared to 0 and 255: a pixel was drawn where both
// agree. Copies the drawn pixels to the canvas at (x, y).
template <class F>
void overlay(const RgbaCanvas& c, int x, int y, int sw, int sh, int scale, F draw) {
    if (sw <= 0 || sh <= 0) return;
    Surface8 a(sw, sh), b(sw, sh);
    a.clear(0);
    b.clear(255);
    draw(a);
    draw(b);
    for (int yy = 0; yy < sh; ++yy) {
        const uint8_t* ra = a.data() + size_t(yy) * sw;
        const uint8_t* rb = b.data() + size_t(yy) * sw;
        for (int xx = 0; xx < sw; ++xx) {
            if (ra[xx] != rb[xx]) continue;
            const auto& p = c.pal->rgb8[ra[xx]];
            uint32_t col = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            for (int dy = 0; dy < scale; ++dy) {
                int oy = y + yy * scale + dy;
                if (oy < 0 || oy >= c.h) continue;
                for (int dx = 0; dx < scale; ++dx) {
                    int ox = x + xx * scale + dx;
                    if (ox >= 0 && ox < c.w) c.px[size_t(oy) * c.w + ox] = col;
                }
            }
        }
    }
}
}  // namespace

int RgbaCanvas::text(const ShapeFont& f, int x, int y, const std::string& s, int scale) const {
    int tw = f.text_width(s) + 2, th = f.line_advance() + 2;
    overlay(*this, x, y, tw, th, scale, [&](Surface8& surf) { f.draw_top(surf, 0, 0, s); });
    return tw * scale;
}

void RgbaCanvas::frame(const ShapeFrame& f, int x, int y, int scale) const {
    overlay(*this, x - f.xoff * scale, y - f.yoff * scale, f.w, f.h, scale,
            [&](Surface8& surf) { surf.draw(f, f.xoff, f.yoff); });
}

void RgbaCanvas::line(int x0, int y0, int x1, int y1, uint32_t color) const {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    // Clip crudely: skip lines that are far outside.
    if (std::max(x0, x1) < -4096 || std::min(x0, x1) > w + 4096 || std::max(y0, y1) < -4096 || std::min(y0, y1) > h + 4096)
        return;
    for (int n = 0; n < 20000; ++n) {
        if (x0 >= 0 && y0 >= 0 && x0 < w && y0 < h) px[size_t(y0) * w + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void RgbaCanvas::shade(int x, int y, int rw, int rh, uint32_t color, int alpha) const {
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(w, x + rw), y1 = std::min(h, y + rh);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t& d = px[size_t(yy) * w + xx];
            uint32_t r = (((d >> 16) & 255) * (255 - alpha) + ((color >> 16) & 255) * alpha) / 255;
            uint32_t g = (((d >> 8) & 255) * (255 - alpha) + ((color >> 8) & 255) * alpha) / 255;
            uint32_t b = ((d & 255) * (255 - alpha) + (color & 255) * alpha) / 255;
            d = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}

}  // namespace pg
