// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>

#include "core/bytes.h"

namespace pg {

// U8PAL.PAL (specs/formats/palette.md): 4-byte header, then 256 x RGB in 6-bit VGA units.
struct Palette {
    std::array<std::array<uint8_t, 3>, 256> rgb6{};  // as stored, 0..63
    std::array<std::array<uint8_t, 3>, 256> rgb8{};  // c * 255 / 63
    bool load(const Bytes& file);
};

// XFORMPAL.DAT: translucency for palette indices 8..14 of TRANSL shapes.
struct XForm {
    std::array<std::array<uint8_t, 256>, 9> tables{};  // table 1 = selector, 2..8 = blends
    std::array<std::array<uint8_t, 4>, 8> blend{};     // indices 8..15: r, g, b, alpha (0..63)
    bool loaded = false;
    bool load(const Bytes& file);  // the whole XFORMPAL.DAT Flex
    // 8-bit translucent pixel: src index s over screen index d. Returns the new screen index.
    uint8_t apply(uint8_t s, uint8_t d) const {
        uint8_t n = tables[1][s];
        return n == 0xFF || n + 1 > 8 ? s : tables[n + 1][d];
    }
    bool is_xform(uint8_t s) const { return tables[1][s] != 0xFF; }
};

// The whole-palette colour matrix (specs/formats/palette.md, "Palette transforms").
// Fixed point, 0x800 = 1.0; rows R, G, B of {r, g, b, offset}.
struct PaletteTransform {
    std::array<int32_t, 12> m{2048, 0, 0, 0, 0, 2048, 0, 0, 0, 0, 2048, 0};
    static PaletteTransform preset(int n);
    void apply(const uint8_t in[3], uint8_t out[3]) const;
};

}  // namespace pg
