// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <vector>

#include "data/palette.h"
#include "data/shape.h"

namespace pg {

// An 8-bit palette-indexed drawing surface, like the original's 320 x 200 VGA screen.
// All drawing clips to the clip rectangle (the whole surface by default).
class Surface8 {
public:
    Surface8(int w = 320, int h = 200) { resize(w, h); }
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    uint8_t* row(int y) { return &px_[size_t(y) * w_]; }
    const uint8_t* data() const { return px_.data(); }
    void clear(uint8_t index);
    void fill_rect(int x, int y, int w, int h, uint8_t index);
    void set_clip(int x, int y, int w, int h);
    void reset_clip() { set_clip(0, 0, w_, h_); }

    // Draws `f` with its anchor at (x, y). flipped mirrors about the anchor column
    // (source column c -> x + xoff - c). xform (for TRANSL shapes): indices 8..14 tint the
    // screen through XFORMPAL. ghost: a 256x256 average table for 50% drawing.
    // remap: a 256-entry colour table applied to the shape's pixels first (companion recolours).
    void draw(const ShapeFrame& f, int x, int y, bool flipped = false, const XForm* xform = nullptr,
              const uint8_t* ghost = nullptr, const uint8_t* remap = nullptr);

    // Coverage mask (milestone 6, the UI layer over a scaled world): while set, every pixel
    // written also sets its byte in `mask` (w x h, the caller clears it). nullptr = off.
    void set_mask(std::vector<uint8_t>* mask) { mask_ = mask; }

    // Converts to 0xAARRGGBB pixels through a palette (with an optional transform).
    void to_rgba(const Palette& pal, const PaletteTransform& t, std::vector<uint32_t>& out) const;

private:
    int w_ = 0, h_ = 0;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;  // clip, half-open
    std::vector<uint8_t> px_;
    std::vector<uint8_t>* mask_ = nullptr;
};

// Builds a table where table[a * 256 + b] is the palette index nearest to the average of
// colours a and b (used for 50% ghost drawing in 8-bit mode).
std::vector<uint8_t> build_ghost_table(const Palette& pal);

}  // namespace pg
