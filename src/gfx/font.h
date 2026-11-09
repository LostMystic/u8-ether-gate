// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>

#include "data/shape.h"
#include "gfx/surface.h"

namespace pg {

// A U8 shape font (specs/formats/fonts.md): frame number = character code.
class ShapeFont {
public:
    ShapeFont() = default;
    ShapeFont(const Shape* shape, int font_number);
    bool valid() const { return shape_ != nullptr; }
    int height() const { return height_; }
    int baseline() const { return baseline_; }
    int line_advance() const { return height_ + vlead_; }
    int char_width(unsigned char c) const;
    int text_width(const std::string& s) const;
    // Draws one line with its baseline at y; returns the pen x after the text. remap: an
    // optional 256-entry colour table for the glyphs' pixels (a tinted copy of the font).
    int draw(Surface8& surf, int x, int y, const std::string& s, const uint8_t* remap = nullptr) const;
    // Draws with the top of the line box at (x, top).
    int draw_top(Surface8& surf, int x, int top, const std::string& s, const uint8_t* remap = nullptr) const {
        return draw(surf, x, top + baseline_, s, remap);
    }

private:
    const Shape* shape_ = nullptr;
    int height_ = 0, baseline_ = 0, vlead_ = 0;
};

}  // namespace pg
