// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "data/flex.h"

namespace pg {

// One decoded frame (specs/formats/shapes.md). Pixels are palette indices, row-major,
// w * h bytes; kTransparent marks pixels the RLE skipped (0xFF never occurs in U8 art).
struct ShapeFrame {
    static constexpr uint8_t kTransparent = 0xFF;
    int w = 0, h = 0;
    int xoff = 0, yoff = 0;  // anchor, from the frame's top-left
    std::vector<uint8_t> pixels;
    bool opaque(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h && pixels[size_t(y) * w + x] != kTransparent;
    }
};

// A shape: a frame table over raw bytes, decoded lazily and cached.
class Shape {
public:
    // `data` must stay alive as long as the Shape (it points into an archive).
    bool parse(ByteView data, std::string* error = nullptr);
    unsigned frame_count() const { return unsigned(offsets_.size()); }
    // Decoded frame, or nullptr if the index is out of range or the frame is corrupt.
    const ShapeFrame* frame(unsigned index) const;
    // Decodes without caching; reports why a frame is bad. Used by the tests.
    bool decode(unsigned index, ShapeFrame& out, std::string* error = nullptr) const;
    int header_w() const { return header_w_; }
    int header_h() const { return header_h_; }

private:
    ByteView data_;
    int header_w_ = 0, header_h_ = 0;
    std::vector<uint32_t> offsets_;
    mutable std::vector<std::unique_ptr<ShapeFrame>> cache_;
    mutable std::vector<uint8_t> bad_;
};

// A Flex of shapes (U8SHAPES.FLX, U8GUMPS.FLX, U8FONTS.FLX) or a single bare shape file
// (U8MOUSE.SHP, loaded with load_single as shape 0).
class ShapeArchive {
public:
    bool load_flex(Bytes file, std::string* error = nullptr);
    bool load_single(Bytes file, std::string* error = nullptr);
    unsigned count() const { return unsigned(shapes_.size()); }
    // nullptr for empty entries.
    const Shape* shape(unsigned index) const {
        return index < shapes_.size() ? shapes_[index].get() : nullptr;
    }
    const ShapeFrame* frame(unsigned shape, unsigned frame) const {
        const Shape* s = this->shape(shape);
        return s ? s->frame(frame) : nullptr;
    }
    // Engine-made shapes (data/shapes/, NOTES.md "Custom shapes"): a bare shape file put at
    // `index` (the archive grows; numbers past the original range are fine).
    bool add_shape(unsigned index, Bytes file, std::string* error = nullptr);
    // A copy of an original shape's frames at `index` (data/shapes/shapes.json "frames_from": the
    // new creatures' bodies, PLAN-CREATURES.md).
    bool copy_shape(unsigned from, unsigned index, std::string* error = nullptr);

private:
    Flex flex_;
    Bytes single_;
    std::vector<std::unique_ptr<Bytes>> extra_;  // the bytes of added shapes
    std::vector<std::unique_ptr<Shape>> shapes_;
};

}  // namespace pg
