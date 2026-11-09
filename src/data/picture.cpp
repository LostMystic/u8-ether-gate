// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/picture.h"

#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

namespace pg {

bool load_picture(const std::string& path, const Palette& pal, const XForm& xform, ShapeFrame& out,
                  std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "missing " + path;
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) {
        if (error) *error = path + ": not a PNG";
        return false;
    }
    auto usable = [&](int i) { return i >= 16 && i < 255 && !(xform.loaded && xform.is_xform(uint8_t(i))); };
    std::map<uint32_t, uint8_t> cache;
    auto index_of = [&](int r, int g, int b) -> uint8_t {
        uint32_t key = uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        int best = 16, bd = 1 << 30;
        for (int i = 16; i < 255; ++i) {
            if (!usable(i)) continue;
            int dr = pal.rgb8[size_t(i)][0] - r, dg = pal.rgb8[size_t(i)][1] - g, db = pal.rgb8[size_t(i)][2] - b;
            int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = i; }
            if (d == 0) break;
        }
        cache[key] = uint8_t(best);
        return uint8_t(best);
    };
    out = ShapeFrame{};
    out.w = w;
    out.h = h;
    out.pixels.assign(size_t(w) * h, ShapeFrame::kTransparent);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* p = px + 4 * i;
        if (p[3] >= 128) out.pixels[size_t(i)] = index_of(p[0], p[1], p[2]);
    }
    stbi_image_free(px);
    return true;
}

}  // namespace pg
