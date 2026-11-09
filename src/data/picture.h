// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Engine-made pictures (engine/data/portraits/): RGBA PNG files whose opaque pixels are colours of
// the game palette, read back into 8-bit frames. Pixels with alpha below 128 are transparent;
// every other pixel takes the palette index of exactly its colour (a non-translucent index from
// 16 to 254), or the nearest such colour if the file was not made for this palette.
#pragma once
#include <string>

#include "data/palette.h"
#include "data/shape.h"

namespace pg {

bool load_picture(const std::string& path, const Palette& pal, const XForm& xform, ShapeFrame& out,
                  std::string* error = nullptr);

}  // namespace pg
