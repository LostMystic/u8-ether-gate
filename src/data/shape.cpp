// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/shape.h"

namespace pg {

bool Shape::parse(ByteView data, std::string* error) {
    data_ = data;
    offsets_.clear();
    if (data.size < 6) { if (error) *error = "shape header too short"; return false; }
    header_w_ = rd16(data.data);
    header_h_ = rd16(data.data + 2);
    unsigned n = rd16(data.data + 4);
    if (6 + size_t(n) * 6 > data.size) { if (error) *error = "frame table does not fit"; return false; }
    offsets_.resize(n);
    for (unsigned i = 0; i < n; ++i) offsets_[i] = rd24(data.data + 6 + 6 * i);  // byte 3 is a flag, ignored
    cache_.clear();
    cache_.resize(n);
    bad_.assign(n, 0);
    return true;
}

bool Shape::decode(unsigned index, ShapeFrame& out, std::string* error) const {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (index >= offsets_.size()) return fail("frame index out of range");
    size_t base = offsets_[index];
    if (base + 18 > data_.size) return fail("frame header outside the shape");
    const uint8_t* f = data_.data + base;
    const uint8_t* end = data_.data + data_.size;
    int enc = rd16(f + 8);
    int w = rds16(f + 10), h = rds16(f + 12);
    if (w < 0 || h < 0 || w > 4096 || h > 4096) return fail("bad frame size");
    if (enc > 1) return fail("unknown encoding");
    if (base + 18 + size_t(h) * 2 > data_.size) return fail("line table outside the shape");
    out.w = w;
    out.h = h;
    out.xoff = rds16(f + 14);
    out.yoff = rds16(f + 16);
    out.pixels.assign(size_t(w) * h, ShapeFrame::kTransparent);
    for (int y = 0; y < h; ++y) {
        // Line offsets count from their own position in the table.
        const uint8_t* p = f + 18 + 2 * y + rd16(f + 18 + 2 * y);
        uint8_t* row = &out.pixels[size_t(y) * w];
        int x = 0;
        while (x < w) {
            if (p >= end) return fail("row data runs off the shape");
            x += *p++;  // transparent skip
            if (x > w) return fail("skip overshoots the row");
            if (x == w) break;
            if (p >= end) return fail("row data runs off the shape");
            int n = *p++;
            bool fill = false;
            if (enc == 1) { fill = n & 1; n >>= 1; }
            if (x + n > w) return fail("run overshoots the row");
            if (fill) {
                if (p >= end) return fail("row data runs off the shape");
                uint8_t c = *p++;
                for (int i = 0; i < n; ++i) row[x + i] = c;
            } else {
                if (p + n > end) return fail("row data runs off the shape");
                for (int i = 0; i < n; ++i) row[x + i] = p[i];
                p += n;
            }
            x += n;
        }
    }
    return true;
}

const ShapeFrame* Shape::frame(unsigned index) const {
    if (index >= offsets_.size() || bad_[index]) return nullptr;
    if (!cache_[index]) {
        auto fr = std::make_unique<ShapeFrame>();
        if (!decode(index, *fr)) { bad_[index] = 1; return nullptr; }
        cache_[index] = std::move(fr);
    }
    return cache_[index].get();
}

bool ShapeArchive::load_flex(Bytes file, std::string* error) {
    if (!flex_.load(std::move(file))) { if (error) *error = flex_.error(); return false; }
    shapes_.clear();
    shapes_.resize(flex_.count());
    for (unsigned i = 0; i < flex_.count(); ++i) {
        ByteView v = flex_.entry(i);
        if (v.empty()) continue;
        auto s = std::make_unique<Shape>();
        std::string e;
        if (!s->parse(v, &e)) {
            if (error) *error = "shape " + std::to_string(i) + ": " + e;
            return false;
        }
        shapes_[i] = std::move(s);
    }
    return true;
}

bool ShapeArchive::add_shape(unsigned index, Bytes file, std::string* error) {
    auto bytes = std::make_unique<Bytes>(std::move(file));
    auto s = std::make_unique<Shape>();
    if (!s->parse({bytes->data(), bytes->size()}, error)) return false;
    if (index >= shapes_.size()) shapes_.resize(index + 1);
    shapes_[index] = std::move(s);
    extra_.push_back(std::move(bytes));
    return true;
}

bool ShapeArchive::copy_shape(unsigned from, unsigned index, std::string* error) {
    ByteView v = flex_.entry(from);
    if (v.empty()) {
        if (error) *error = "no shape " + std::to_string(from) + " to copy";
        return false;
    }
    return add_shape(index, Bytes(v.begin(), v.end()), error);
}

bool ShapeArchive::load_single(Bytes file, std::string* error) {
    single_ = std::move(file);
    shapes_.clear();
    auto s = std::make_unique<Shape>();
    if (!s->parse({single_.data(), single_.size()}, error)) return false;
    shapes_.push_back(std::move(s));
    return true;
}

}  // namespace pg
