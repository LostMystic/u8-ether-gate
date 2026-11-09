// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Encodes frames into the U8 shape layout (specs/formats/shapes.md), so engine-made art (custom
// actor shapes, data/shapes/) loads through the same Shape parser as the original data.
#include <cstdint>
#include <vector>

#include "data/shape.h"

namespace pg {

// One shape file: the 6-byte header, the frame table and each frame (encoding 1: runs may be
// fills). `shape_number` is written into each frame's header as the world shapes do.
std::vector<uint8_t> encode_shape(const std::vector<ShapeFrame>& frames, unsigned shape_number);

}  // namespace pg
