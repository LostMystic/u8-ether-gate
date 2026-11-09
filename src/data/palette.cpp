// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/palette.h"

#include <algorithm>

#include "data/flex.h"

namespace pg {

bool Palette::load(const Bytes& f) {
    if (f.size() < 4 + 768) return false;
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) {
            uint8_t v = f[4 + 3 * i + c] & 63;
            rgb6[i][c] = v;
            rgb8[i][c] = uint8_t(v * 255 / 63);
        }
    return true;
}

bool XForm::load(const Bytes& file) {
    Flex flex;
    if (!flex.load(file)) return false;
    ByteView t = flex.entry(0), b = flex.entry(1);
    if (t.size < 9 * 256 || b.size < 32) return false;
    for (int k = 0; k < 9; ++k)
        for (int i = 0; i < 256; ++i) tables[k][i] = t.data[256 * k + i];
    for (int k = 0; k < 8; ++k)
        for (int c = 0; c < 4; ++c) blend[k][c] = b.data[4 * k + c];
    loaded = true;
    return true;
}

PaletteTransform PaletteTransform::preset(int n) {
    PaletteTransform t;
    auto set = [&](std::initializer_list<int32_t> v) { std::copy(v.begin(), v.end(), t.m.begin()); };
    switch (n) {
    case 1: set({768, 1024, 256, 0, 768, 1024, 256, 0, 768, 1024, 256, 0}); break;
    case 2: set({0, 0, 0, 0, 0, 2048, 0, 0, 0, 0, 2048, 0}); break;
    case 3: set({192 + 512, 256, 64, 384, 192, 256 + 512, 64, 384, 192, 256, 64 + 512, 384}); break;
    case 4: set({1408, 512, 128, 384, 192, 1280, 64, 0, 0, 0, 1024, 0}); break;
    case 5: set({-768 + 4096, -1024, -256, 0, -768, -1024 + 4096, -256, 0, -768, -1024, -256 + 4096, 0}); break;
    case 6: set({0, 0, 2048, 0, 2048, 0, 0, 0, 0, 2048, 0, 0}); break;
    default: break;  // 0 none; 7 (BRG) has no known preset
    }
    return t;
}

void PaletteTransform::apply(const uint8_t in[3], uint8_t out[3]) const {
    for (int row = 0; row < 3; ++row) {
        const int32_t* r = &m[4 * row];
        int32_t v = (r[0] * in[0] + r[1] * in[1] + r[2] * in[2] + r[3] * 255) / 2048;
        out[row] = uint8_t(std::clamp(v, 0, 255));
    }
}

}  // namespace pg
