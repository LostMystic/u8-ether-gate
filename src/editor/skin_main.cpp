// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// pagan_skin: custom actor art (NOTES.md "Custom shapes"). Paints Nerissa's figure over the
// Avatar's frames (editor/skin_paint.h) and looks at actor shapes.
//   pagan_skin GAME_DIR build NUMBER out.shp      paint every frame, write + verify the shape file
//   pagan_skin GAME_DIR compare ACTION out.png [SCALE]   the Avatar's frames | hers, 8 directions
//   pagan_skin GAME_DIR roles FRAME out.png       base | body-part map | painted (8x); SKIN_TEXT=1
//                                                 prints the map, SKIN_DEBUG=1 the pack candidates
//   pagan_skin GAME_DIR sheet SHAPE ACTION out.png | actions [SHAPE] | hist [SHAPE] |
//              dump SHAPE FRAME | gump GUMP FRAME out.png | palette out.png
#include <algorithm>
#include <functional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "data/gamedata.h"
#include "data/shape_writer.h"
#include "editor/skin_paint.h"

namespace pg {
bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale);
}

using namespace pg;

namespace {

uint32_t rgb(const GameData& d, uint8_t i) {
    const auto& c = d.palette.rgb8[i];
    return 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
}

using FrameFn = std::function<const ShapeFrame*(unsigned frame)>;

// One row per direction, one column per frame of the action, frames anchored on a common point.
// With two frame sources the second one's frames follow the first one's in each row.
bool sheet(const GameData& d, const AnimSequence* seq, std::vector<FrameFn> srcs, const std::string& out, int scale) {
    int left = 0, right = 0, up = 0, down = 0;
    for (const FrameFn& fn : srcs)
        for (const AnimFrame& af : seq->frames)
            if (const ShapeFrame* f = fn(af.frame)) {
                int l = af.flipped() ? f->w - 1 - f->xoff : f->xoff;
                left = std::max(left, l);
                right = std::max(right, f->w - l);
                up = std::max(up, f->yoff);
                down = std::max(down, f->h - f->yoff);
            }
    int cw = left + right + 4, ch = up + down + 4;
    int cols = int(seq->length * srcs.size());
    int W = cw * cols + 6 * int(srcs.size() - 1), H = ch * 8;
    std::vector<uint32_t> px(size_t(W) * H, 0xFF202020u);
    for (size_t s = 0; s < srcs.size(); ++s)
        for (unsigned dir = 0; dir < 8; ++dir)
            for (unsigned i = 0; i < seq->length; ++i) {
                int cx = int(s * seq->length + i) * cw + 6 * int(s), cy = int(dir) * ch;
                for (int y = 0; y < ch; ++y)
                    for (int x = 0; x < cw; ++x)
                        px[size_t(cy + y) * W + cx + x] = (((x / 4) + (y / 4)) & 1) ? 0xFF4A6A48u : 0xFF406040u;
                const AnimFrame& af = seq->at(dir, i);
                const ShapeFrame* f = srcs[s](af.frame);
                int ax = cx + 2 + left, ay = cy + 2 + up;
                if (!f) continue;
                for (int y = 0; y < f->h; ++y)
                    for (int x = 0; x < f->w; ++x) {
                        uint8_t p = f->pixels[size_t(y) * f->w + x];
                        if (p == ShapeFrame::kTransparent) continue;
                        int sx = af.flipped() ? ax + f->xoff - x : ax - f->xoff + x, sy = ay - f->yoff + y;
                        if (sx >= 0 && sy >= 0 && sx < W && sy < H) px[size_t(sy) * W + sx] = rgb(d, p);
                    }
            }
    return save_png(out, px.data(), W, H, scale);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pagan_skin GAME_DIR actions|sheet|palette ...\n");
        return 2;
    }
    GameFS fs;
    GameData d;
    if (!fs.open(argv[1]) || !d.load(fs)) { std::fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    std::string cmd = argv[2];
    if (cmd == "actions") {
        unsigned shape = argc > 3 ? unsigned(std::atoi(argv[3])) : 1;
        std::set<unsigned> all;
        for (unsigned a = 0; a < AnimDat::kActions; ++a) {
            const AnimSequence* s = d.anim.get(shape, a);
            if (!s) continue;
            std::printf("action %2u L %2u flagsA %02x rep %u:", a, s->length, s->flags_a, s->repeat);
            for (unsigned dir = 0; dir < 8; ++dir) {
                std::printf(" |");
                for (unsigned i = 0; i < s->length; ++i) {
                    const AnimFrame& f = s->at(dir, i);
                    std::printf(" %u%s", f.frame, f.flipped() ? "f" : "");
                    all.insert(f.frame);
                }
            }
            std::printf("\n");
        }
        std::printf("%zu distinct frames referenced; shape has %u\n", all.size(), d.shapes.shape(shape)->frame_count());
        return 0;
    }
    if (cmd == "sheet" && argc >= 6) {  // SHAPE ACTION out.png
        unsigned shape = unsigned(std::atoi(argv[3]));
        const AnimSequence* seq = d.anim.get(shape, unsigned(std::atoi(argv[4])));
        if (!seq) return 1;
        return sheet(d, seq, {[&](unsigned f) { return d.shapes.frame(shape, f); }}, argv[5], 3) ? 0 : 1;
    }
    if (cmd == "compare" && argc >= 5) {  // ACTION out.png [scale]: the Avatar's frames, then the painted ones
        const AnimSequence* seq = d.anim.get(1, unsigned(std::atoi(argv[3])));
        if (!seq) return 1;
        SkinPainter painter(d, 1);
        std::map<unsigned, ShapeFrame> painted;
        FrameFn mine = [&](unsigned f) -> const ShapeFrame* {
            auto it = painted.find(f);
            if (it == painted.end()) it = painted.emplace(f, painter.paint(f)).first;
            return &it->second;
        };
        return sheet(d, seq, {[&](unsigned f) { return d.shapes.frame(1, f); }, mine}, argv[4], argc > 5 ? std::atoi(argv[5]) : 3) ? 0 : 1;
    }
    if (cmd == "roles" && argc >= 5) {  // FRAME out.png: base | roles | painted, 8x
        unsigned fr = unsigned(std::atoi(argv[3]));
        SkinPainter painter(d, 1);
        SkinKeys k;
        ShapeFrame p = painter.paint(fr, &k);
        const ShapeFrame* b = d.shapes.frame(1, fr);
        static const uint32_t col[] = {0xFF305030, 0xFF000000, 0xFFFF00FF, 0xFF808080, 0xFFFFC0A0, 0xFFE09070,
                                       0xFFC06040, 0xFFF0B090, 0xFF2020C0, 0xFF20A080, 0xFF40E0B0, 0xFFA0FFFF,
                                       0xFFFFFFFF, 0xFF804000, 0xFF603010, 0xFFFFE0C0, 0xFF0000FF, 0xFF4040FF};
        int W = b->w * 3 + 4, H = b->h;
        std::vector<uint32_t> px(size_t(W) * H, 0xFF305030);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < b->w; ++x) {
                uint8_t q = b->pixels[size_t(y) * b->w + x];
                if (q != ShapeFrame::kTransparent) px[size_t(y) * W + x] = rgb(d, q);
                px[size_t(y) * W + x + b->w + 2] = col[k.roles[size_t(y) * b->w + x] % 18];
                q = p.pixels[size_t(y) * b->w + x];
                if (q != ShapeFrame::kTransparent) px[size_t(y) * W + x + 2 * b->w + 4] = rgb(d, q);
            }
        std::printf("facing %d head %d,%d-%d,%d upright %d actions", painter.info(fr).facing, k.head_x0, k.head_y0, k.head_x1,
                    k.head_y1, k.upright);
        for (unsigned a : painter.info(fr).actions) std::printf(" %u", a);
        std::printf("\n");
        if (std::getenv("SKIN_TEXT"))
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < b->w; ++x) std::printf("%c", "_.*HNAhLFTSsbPRfeM"[k.roles[size_t(y) * b->w + x] % 18]);
                std::printf("\n");
            }
        return save_png(argv[4], px.data(), W, H, 8) ? 0 : 1;
    }
    if (cmd == "build" && argc >= 5) {  // NUMBER out.shp: paints every frame and writes the shape
        SkinPainter painter(d, 1);
        std::vector<ShapeFrame> frames = painter.paint_all();
        std::vector<uint8_t> bytes = encode_shape(frames, unsigned(std::atoi(argv[3])));
        Shape check;  // the written shape must decode to exactly the painted frames
        std::string err;
        if (!check.parse({bytes.data(), bytes.size()}, &err) || check.frame_count() != frames.size()) {
            std::fprintf(stderr, "encoded shape does not parse: %s\n", err.c_str());
            return 1;
        }
        for (unsigned i = 0; i < frames.size(); ++i) {
            ShapeFrame g;
            if (!check.decode(i, g, &err) || g.w != frames[i].w || g.h != frames[i].h || g.xoff != frames[i].xoff ||
                g.yoff != frames[i].yoff || g.pixels != frames[i].pixels) {
                std::fprintf(stderr, "frame %u does not round-trip: %s\n", i, err.c_str());
                return 1;
            }
        }
        FILE* fo = std::fopen(argv[4], "wb");
        if (!fo) return 1;
        std::fwrite(bytes.data(), 1, bytes.size(), fo);
        std::fclose(fo);
        std::printf("%zu frames, %zu bytes\n", frames.size(), bytes.size());
        return 0;
    }
    if (cmd == "hist") {  // palette indices used by the frames of a shape's actions
        unsigned shape = argc > 3 ? unsigned(std::atoi(argv[3])) : 1;
        std::set<unsigned> frames;
        for (unsigned a = 0; a < AnimDat::kActions; ++a)
            if (const AnimSequence* s = d.anim.get(shape, a))
                for (const AnimFrame& f : s->frames) frames.insert(f.frame);
        std::vector<long> n(256);
        for (unsigned fr : frames)
            if (const ShapeFrame* f = d.shapes.frame(shape, fr))
                for (uint8_t p : f->pixels) if (p != ShapeFrame::kTransparent) ++n[p];
        for (int i = 0; i < 256; ++i)
            if (n[size_t(i)]) {
                const auto& c = d.palette.rgb8[size_t(i)];
                std::printf("%3d %02x%02x%02x %ld\n", i, c[0], c[1], c[2], n[size_t(i)]);
            }
        return 0;
    }
    if (cmd == "dump" && argc >= 5) {  // a frame as palette indices
        const ShapeFrame* f = d.shapes.frame(unsigned(std::atoi(argv[3])), unsigned(std::atoi(argv[4])));
        if (!f) return 1;
        std::printf("%d x %d anchor %d,%d\n", f->w, f->h, f->xoff, f->yoff);
        for (int y = 0; y < f->h; ++y) {
            for (int x = 0; x < f->w; ++x) {
                uint8_t p = f->pixels[size_t(y) * f->w + x];
                if (p == ShapeFrame::kTransparent) std::printf("  .");
                else std::printf("%3d", p);
            }
            std::printf("\n");
        }
        return 0;
    }
    if (cmd == "gump" && argc >= 6) {  // GUMP FRAME out.png (4x)
        const ShapeFrame* f = d.gumps.frame(unsigned(std::atoi(argv[3])), unsigned(std::atoi(argv[4])));
        if (!f) return 1;
        std::vector<uint32_t> px(size_t(f->w) * f->h, 0xFF305030);
        for (size_t i = 0; i < px.size(); ++i)
            if (f->pixels[i] != ShapeFrame::kTransparent) px[i] = rgb(d, f->pixels[i]);
        std::printf("%d x %d\n", f->w, f->h);
        return save_png(argv[5], px.data(), f->w, f->h, 4) ? 0 : 1;
    }
    if (cmd == "palette" && argc >= 4) {
        std::vector<uint32_t> px(16 * 16);
        for (int i = 0; i < 256; ++i) px[size_t(i)] = rgb(d, uint8_t(i));
        return save_png(argv[3], px.data(), 16, 16, 24) ? 0 : 1;
    }
    std::fprintf(stderr, "bad command\n");
    return 2;
}
