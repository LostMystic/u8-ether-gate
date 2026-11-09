// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/shape_writer.h"

namespace pg {

namespace {

void put16(std::vector<uint8_t>& b, unsigned v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}

// Row data: (skip, run) pairs. Each opaque span is cut into chunks of at most 127 pixels: a fill
// for 3+ equal pixels, otherwise literal bytes. Chunks after the first follow a zero skip.
void encode_row(const ShapeFrame& f, int y, std::vector<uint8_t>& out) {
    const uint8_t* row = &f.pixels[size_t(y) * f.w];
    int x = 0;
    while (x < f.w) {
        int skip = 0;
        while (x < f.w && row[x] == ShapeFrame::kTransparent) { ++x; ++skip; }
        while (skip > 255) {  // never in practice (frames are narrower), but stay valid
            out.push_back(255);
            out.push_back(0);  // an empty literal run
            skip -= 255;
        }
        if (x >= f.w) {
            if (skip) out.push_back(uint8_t(skip));
            return;
        }
        int end = x;
        while (end < f.w && row[end] != ShapeFrame::kTransparent) ++end;
        bool first = true;
        while (x < end) {
            out.push_back(first ? uint8_t(skip) : 0);
            first = false;
            int same = 1;
            while (x + same < end && row[x + same] == row[x] && same < 127) ++same;
            if (same >= 3) {
                out.push_back(uint8_t((same << 1) | 1));
                out.push_back(row[x]);
                x += same;
                continue;
            }
            int n = 0;  // literal until the next run of 3 or the end of the span
            while (x + n < end && n < 127) {
                if (x + n + 2 < end && row[x + n] == row[x + n + 1] && row[x + n] == row[x + n + 2]) break;
                ++n;
            }
            out.push_back(uint8_t(n << 1));
            for (int i = 0; i < n; ++i) out.push_back(row[x + i]);
            x += n;
        }
    }
}

}  // namespace

std::vector<uint8_t> encode_shape(const std::vector<ShapeFrame>& frames, unsigned shape_number) {
    std::vector<std::vector<uint8_t>> bodies;
    for (size_t n = 0; n < frames.size(); ++n) {
        const ShapeFrame& f = frames[n];
        std::vector<uint8_t> b;
        put16(b, shape_number);
        put16(b, unsigned(n));
        for (int i = 0; i < 4; ++i) b.push_back(0);
        put16(b, 1);
        put16(b, unsigned(f.w));
        put16(b, unsigned(f.h));
        put16(b, unsigned(uint16_t(f.xoff)));
        put16(b, unsigned(uint16_t(f.yoff)));
        std::vector<std::vector<uint8_t>> rows(size_t(f.h));
        for (int y = 0; y < f.h; ++y) encode_row(f, y, rows[size_t(y)]);
        // Line offsets are relative to their own position (18 + 2y).
        size_t data_start = 18 + 2 * size_t(f.h), pos = data_start;
        for (int y = 0; y < f.h; ++y) {
            put16(b, unsigned(pos - (18 + 2 * size_t(y))));
            pos += rows[size_t(y)].size();
        }
        for (auto& r : rows) b.insert(b.end(), r.begin(), r.end());
        bodies.push_back(std::move(b));
    }
    std::vector<uint8_t> out;
    put16(out, 1);
    put16(out, 1);
    put16(out, unsigned(frames.size()));
    size_t off = 6 + 6 * frames.size();
    for (auto& b : bodies) {
        out.push_back(uint8_t(off));
        out.push_back(uint8_t(off >> 8));
        out.push_back(uint8_t(off >> 16));
        out.push_back(0x80);
        put16(out, unsigned(b.size()));
        off += b.size();
    }
    for (auto& b : bodies) out.insert(out.end(), b.begin(), b.end());
    return out;
}

}  // namespace pg
