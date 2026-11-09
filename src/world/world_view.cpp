// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/world_view.h"

#include <cstdio>

#include "data/gamedata.h"
#include "gfx/surface.h"
#include "world/current_map.h"

namespace pg {

void WorldView::render(CurrentMap& map, Surface8& surf, const int cam[3], const int probe[3], const Options& opt) {
    const int W = surf.width(), H = surf.height();
    centre_x_ = W / 2;
    centre_y_ = H / 2;
    const int camSx = opt.use_screen_camera ? opt.camera_sx : (cam[0] - cam[1]) / 4;
    const int camSy = opt.use_screen_camera ? opt.camera_sy : (cam[0] + cam[1]) / 8 - cam[2];
    roof_limit_ = opt.roof_hiding ? map.roof_limit(probe) : 65536;

    sorter_.begin();
    constexpr uint32_t kExtraBit = 0x40000000u;  // SortItem::item of a neighbour's item (opt.extra)
    auto add_item = [&](const WorldItem& it, uint32_t idx) {
        const ShapeInfo& si = data_.typeflags.get(it.shape);
        if (it.z >= roof_limit_ && !si.is(ShapeInfo::kDraw)) return;
        if (si.is(ShapeInfo::kEditor) && !opt.show_editor) return;
        if (opt.hide_id && it.id == opt.hide_id) return;
        bool ghost = it.ext_flags & 0x0080;
        if ((it.flags & kItemInvisible) && !opt.show_invisible) {
            if (it.id != 1) return;
            ghost = true;  // the invisible Avatar is drawn as a ghost
        }
        unsigned draw_shape = it.shape, draw_frame = it.frame;
        if (it.id && it.id < 256 && opt.appearance) {
            Options::Look lk;
            lk.shape = it.shape;
            lk.frame = it.frame;
            if (opt.appearance(it.id, lk)) {
                if (lk.hide) return;
                draw_shape = lk.shape;
                draw_frame = lk.frame;
            }
        }
        const ShapeFrame* f = data_.shapes.frame(draw_shape, draw_frame);
        if (!f) return;
        bool flipped = it.flags & kItemFlipped;
        int xd, yd, zd;
        si.footprint(flipped, xd, yd, zd);

        int ix = it.x, iy = it.y, iz = it.z;
        if (it.id < 256 && opt.display_pos) opt.display_pos(it.id, ix, iy, iz);
        SortItem s;
        s.item = idx;
        s.id = it.id;
        s.shape = uint16_t(draw_shape);
        s.frame = uint16_t(draw_frame);
        s.flipped = flipped;
        s.x = ix;
        s.y = iy;
        s.z = iz;
        s.xLeft = ix - xd;
        s.yFar = iy - yd;
        s.zTop = iz + zd;
        s.sxLeft = s.xLeft / 4 - s.y / 4 - camSx;
        s.sxRight = s.x / 4 - s.yFar / 4 - camSx;
        s.sxTop = s.xLeft / 4 - s.yFar / 4 - camSx;
        s.syTop = s.xLeft / 8 + s.yFar / 8 - s.zTop - camSy;
        s.sxBot = s.x / 4 - s.y / 4 - camSx;
        s.syBot = s.x / 8 + s.y / 8 - s.z - camSy;
        s.frLeft = flipped ? s.sxBot + f->xoff - f->w : s.sxBot - f->xoff;
        s.frTop = s.syBot - f->yoff;
        s.frRight = s.frLeft + f->w;
        s.frBottom = s.frTop + f->h;
        // Off-screen (rectangle relative to the screen centre).
        if (s.frRight <= -centre_x_ || s.frLeft >= W - centre_x_ || s.frBottom <= -centre_y_ ||
            s.frTop >= H - centre_y_)
            return;
        s.flat = zd == 0;
        s.bigsq = xd == yd && xd >= 128;
        s.trans = data_.typeflags.get(draw_shape).is(ShapeInfo::kTransl);
        s.occl = si.is(ShapeInfo::kOccl) && !s.trans && !(it.flags & kItemInvisible) && !ghost;
        s.solid = si.is(ShapeInfo::kSolid);
        s.draw = si.is(ShapeInfo::kDraw);
        s.roof = si.is(ShapeInfo::kRoof);
        s.land = si.is(ShapeInfo::kLand);
        s.fixed = si.is(ShapeInfo::kFixed);
        s.noisy = si.is(ShapeInfo::kNoisy);
        s.anim = si.anim_type() != 0;
        sorter_.add(std::move(s));
    };
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            if (!map.is_fast(cx, cy)) continue;
            for (uint32_t idx : map.chunk(cx, cy)) add_item(map.item(idx), idx);
        }
    if (opt.extra)
        for (size_t i = 0; i < opt.extra->size(); ++i) add_item(*(*opt.extra)[i], kExtraBit | uint32_t(i));

    sorter_.paint([&](const SortItem& s) {
        const WorldItem& it = (s.item & kExtraBit) ? *(*opt.extra)[s.item & ~kExtraBit] : map.item(s.item);
        const ShapeFrame* f = data_.shapes.frame(s.shape, s.frame);
        bool ghost = (it.ext_flags & 0x0080) || ((it.flags & kItemInvisible) && it.id == 1 && !opt.show_invisible);
        if (ghost && ghost_.empty()) ghost_ = build_ghost_table(data_.palette);
        const uint8_t* rm = (opt.remap && s.id < 256) ? opt.remap(uint16_t(s.id)) : nullptr;
        if (s.id && s.id < 256 && opt.appearance) {
            Options::Look lk;
            lk.shape = s.shape;
            lk.frame = s.frame;
            if (opt.appearance(uint16_t(s.id), lk)) {
                ghost = ghost || lk.ghost;
                if (lk.remap) rm = lk.remap;
            }
        }
        if (ghost && ghost_.empty()) ghost_ = build_ghost_table(data_.palette);
        surf.draw(*f, centre_x_ + s.sxBot, centre_y_ + s.syBot, s.flipped, s.trans ? &data_.xform : nullptr,
                  ghost ? ghost_.data() : nullptr, rm);
        if (opt.log_items)
            std::printf("paint %4d id %5u shape %4u frame %3u at %5d,%5d,%3d screen %4d,%4d%s%s\n", s.order < 0 ? int(sorter_.painted_count()) : s.order,
                        s.id, s.shape, s.frame, s.x, s.y, s.z, centre_x_ + s.sxBot, centre_y_ + s.syBot,
                        s.flipped ? " flipped" : "", s.trans ? " transl" : "");
        if (opt.after_paint) opt.after_paint(surf, s, centre_x_ + s.sxBot, centre_y_ + s.syBot);
    });
}

unsigned WorldView::pick(int px, int py) {
    int i = sorter_.pick(px - centre_x_, py - centre_y_, [&](const SortItem& s, int x, int y) {
        const ShapeFrame* f = data_.shapes.frame(s.shape, s.frame);
        if (!f) return false;
        int ax = centre_x_ + s.sxBot, ay = centre_y_ + s.syBot;
        int col = s.flipped ? ax + f->xoff - (x + centre_x_) : (x + centre_x_) - (ax - f->xoff);
        int row = (y + centre_y_) - (ay - f->yoff);
        return f->opaque(col, row);
    });
    return i < 0 ? 0 : sorter_.items()[size_t(i)].id;
}

}  // namespace pg
