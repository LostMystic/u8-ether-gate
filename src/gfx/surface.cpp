// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "gfx/surface.h"

#include <algorithm>
#include <climits>

namespace pg {

void Surface8::resize(int w, int h) {
    w_ = w;
    h_ = h;
    px_.assign(size_t(w) * h, 0);
    reset_clip();
}

void Surface8::clear(uint8_t index) { std::fill(px_.begin(), px_.end(), index); }

void Surface8::set_clip(int x, int y, int w, int h) {
    cx0_ = std::clamp(x, 0, w_);
    cy0_ = std::clamp(y, 0, h_);
    cx1_ = std::clamp(x + w, 0, w_);
    cy1_ = std::clamp(y + h, 0, h_);
}

void Surface8::fill_rect(int x, int y, int w, int h, uint8_t index) {
    int x0 = std::max(x, cx0_), y0 = std::max(y, cy0_);
    int x1 = std::min(x + w, cx1_), y1 = std::min(y + h, cy1_);
    if (x1 <= x0 || y1 <= y0) return;  // entirely clipped (or empty)
    for (int yy = y0; yy < y1; ++yy) {
        std::fill(row(yy) + x0, row(yy) + x1, index);
        if (mask_ && x1 > x0) std::fill(mask_->begin() + long(yy) * w_ + x0, mask_->begin() + long(yy) * w_ + x1, 1);
    }
}

void Surface8::draw(const ShapeFrame& f, int x, int y, bool flipped, const XForm* xform, const uint8_t* ghost,
                    const uint8_t* remap) {
    int top = y - f.yoff;
    for (int r = 0; r < f.h; ++r) {
        int sy = top + r;
        if (sy < cy0_ || sy >= cy1_) continue;
        const uint8_t* src = &f.pixels[size_t(r) * f.w];
        uint8_t* dst = row(sy);
        for (int c = 0; c < f.w; ++c) {
            uint8_t s = src[c];
            if (s == ShapeFrame::kTransparent) continue;
            if (remap) s = remap[s];
            int sx = flipped ? x + f.xoff - c : x - f.xoff + c;
            if (sx < cx0_ || sx >= cx1_) continue;
            uint8_t d = dst[sx];
            uint8_t out = s;
            if (xform && xform->is_xform(s)) out = xform->apply(s, d);
            if (ghost) out = ghost[size_t(out) * 256 + d];
            dst[sx] = out;
            if (mask_) (*mask_)[size_t(sy) * w_ + sx] = 1;
        }
    }
}

void Surface8::to_rgba(const Palette& pal, const PaletteTransform& t, std::vector<uint32_t>& out) const {
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i) {
        uint8_t c[3];
        t.apply(pal.rgb8[i].data(), c);
        lut[i] = 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
    out.resize(px_.size());
    for (size_t i = 0; i < px_.size(); ++i) out[i] = lut[px_[i]];
}

std::vector<uint8_t> build_ghost_table(const Palette& pal) {
    std::vector<uint8_t> t(256 * 256);
    for (int a = 0; a < 256; ++a)
        for (int b = a; b < 256; ++b) {
            int r = (pal.rgb8[a][0] + pal.rgb8[b][0]) / 2, g = (pal.rgb8[a][1] + pal.rgb8[b][1]) / 2,
                bl = (pal.rgb8[a][2] + pal.rgb8[b][2]) / 2;
            int best = 0, bestd = INT_MAX;
            for (int i = 0; i < 256; ++i) {
                // Skip the translucent indices 8..14 so a ghost never produces xform colours.
                if (i >= 8 && i <= 14) continue;
                int dr = pal.rgb8[i][0] - r, dg = pal.rgb8[i][1] - g, db = pal.rgb8[i][2] - bl;
                int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bestd) { bestd = d; best = i; }
            }
            t[size_t(a) * 256 + b] = t[size_t(b) * 256 + a] = uint8_t(best);
        }
    return t;
}

}  // namespace pg
