// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include <vector>

#include "platform/platform.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace pg {

bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale) {
    if (scale < 1) scale = 1;
    int ow = w * scale, oh = h * scale;
    std::vector<uint8_t> rgb(size_t(ow) * oh * 3);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            uint32_t p = rgba[size_t(y / scale) * w + x / scale];
            uint8_t* d = &rgb[(size_t(y) * ow + x) * 3];
            d[0] = uint8_t(p >> 16);
            d[1] = uint8_t(p >> 8);
            d[2] = uint8_t(p);
        }
    return stbi_write_png(path.c_str(), ow, oh, 3, rgb.data(), ow * 3) != 0;
}

std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& in, int quality) {
    int n = 0;
    unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(in.data()), int(in.size()), &n, quality);
    if (!z) return {};
    std::vector<uint8_t> out(z, z + n);
    STBIW_FREE(z);
    return out;
}

}  // namespace pg
