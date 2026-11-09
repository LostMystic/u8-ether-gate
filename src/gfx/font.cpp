// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "gfx/font.h"

#include <algorithm>

namespace pg {

ShapeFont::ShapeFont(const Shape* shape, int n) : shape_(shape) {
    if (!shape_) return;
    for (unsigned i = 0; i < shape_->frame_count(); ++i)
        if (const ShapeFrame* f = shape_->frame(i)) {
            height_ = std::max(height_, f->h);
            baseline_ = std::max(baseline_, f->yoff);
        }
    // Vertical lead is not in the data (fonts.md "Metrics").
    vlead_ = (n == 4 || n == 9) ? 0 : (n == 10 || n == 11) ? 4 : -1;
}

int ShapeFont::char_width(unsigned char c) const {
    if (!shape_ || c >= shape_->frame_count()) return 7;
    const ShapeFrame* f = shape_->frame(c);
    return f ? f->w : 7;
}

int ShapeFont::text_width(const std::string& s) const {
    int w = 0;
    for (unsigned char c : s) w += char_width(c);
    return w;
}

int ShapeFont::draw(Surface8& surf, int x, int y, const std::string& s, const uint8_t* remap) const {
    if (!shape_) return x;
    for (unsigned char c : s) {
        if (c < shape_->frame_count())
            if (const ShapeFrame* f = shape_->frame(c)) surf.draw(*f, x, y, false, nullptr, nullptr, remap);
        x += char_width(c);
    }
    return x;
}

}  // namespace pg
