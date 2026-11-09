// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

class ShapeFont;
struct ShapeFrame;
struct Palette;

// Small drawing helpers for the high-resolution (zoomed / atlas) views, which are composed
// in 0xAARRGGBB rather than in the 8-bit game surface. 8-bit things (font text, the cursor)
// are drawn into a scratch Surface8 and copied over, each pixel enlarged `scale` times.
struct RgbaCanvas {
    uint32_t* px;
    int w, h;
    const Palette* pal;

    // Text with the top of the line box at (x, y); returns the width in canvas pixels.
    int text(const ShapeFont& f, int x, int y, const std::string& s, int scale = 1) const;
    // A shape frame with its anchor at (x, y).
    void frame(const ShapeFrame& f, int x, int y, int scale = 1) const;
    void line(int x0, int y0, int x1, int y1, uint32_t color) const;
    // Blends a rectangle with `color` at alpha 0..255.
    void shade(int x, int y, int rw, int rh, uint32_t color, int alpha) const;
};

}  // namespace pg
