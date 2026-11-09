// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "app/map_viewer.h"

#include <algorithm>
#include <cstdio>

#include "app/atlas_view.h"
#include "app/rgba_draw.h"
#include "data/gamedata.h"
#include "gfx/surface.h"

namespace pg {

MapViewer::MapViewer(const GameData& data) : data_(data), map_(data), view_(data) {
    maps_ = CurrentMap::populated_maps(data);
    for (unsigned i = 0; i < data.fonts.count(); ++i) fonts_.emplace_back(data.fonts.shape(i), int(i));
}

MapViewer::~MapViewer() = default;

AtlasView& MapViewer::atlas() {
    if (!atlas_view_) {
        atlas_view_ = std::make_unique<AtlasView>(data_, fonts_);
        atlas_view_->load_layout(atlas_path_);
    }
    return *atlas_view_;
}

void MapViewer::open_atlas(bool show_all) {
    AtlasView& a = atlas();
    atlas_ = true;
    a.set_mouse(mouse_x_, mouse_y_);
    if (show_all) a.show_all();
    else if (zoom_ > 0) {
        // Keep the zoomed view's centre and scale.
        long x = 2 * centre_[0] + 4 * centre_[1], y = 4 * centre_[1] - 2 * centre_[0];
        a.show(map_.map_number(), int(x), int(y), 0, zoom_ - 1);
    } else {
        a.show(map_.map_number(), focus_[0], focus_[1], focus_[2], 4);
    }
}

void MapViewer::cam_screen(const int f[3], long& sx, long& sy) const {
    int cam[3] = {f[0], f[1], f[2] + 20};
    sx = (cam[0] - cam[1]) / 4;
    sy = (cam[0] + cam[1]) / 8 - cam[2];
}

void MapViewer::change_zoom(int z, int mx, int my) {
    z = std::clamp(z, 0, kMaxZoom);
    if (z == zoom_) return;
    // The native screen point under the pointer stays put.
    long px, py;
    if (zoom_ == 0) {
        long sx, sy;
        cam_screen(focus_, sx, sy);
        std::copy(focus_, focus_ + 3, saved_focus_);
        saved_cam_[0] = sx;
        saved_cam_[1] = sy;
        px = sx + (mx - 160);
        py = sy + (my - 100);
    } else {
        long s = 1L << (zoom_ - 1);
        px = centre_[0] + (2 * mx - 320) * s;
        py = centre_[1] + (2 * my - 200) * s;
    }
    zoom_ = z;
    if (z > 0) {
        long s = 1L << (z - 1);
        centre_[0] = px - (2 * mx - 320) * s;
        centre_[1] = py - (2 * my - 200) * s;
        return;
    }
    long sx = px - (mx - 160), sy = py - (my - 100);
    if (sx == saved_cam_[0] && sy == saved_cam_[1]) {
        std::copy(saved_focus_, saved_focus_ + 3, focus_);  // back exactly where we were
        return;
    }
    // Camera screen point (sx, sy) at the current height: x - y = 4 sx, x + y = 8 (sy + cz).
    long cz = focus_[2] + 20;
    focus_[0] = int(std::clamp(2 * sx + 4 * (sy + cz), 0L, 32767L));
    focus_[1] = int(std::clamp(4 * (sy + cz) - 2 * sx, 0L, 32767L));
}

void MapViewer::default_focus(unsigned map, int out[3]) const {
    if (const WorldItem* av = map_.npc(1)) {
        out[0] = av->x; out[1] = av->y; out[2] = av->z;
        return;
    }
    // A teleport destination (shape 508, frame 1) is where the game puts the Avatar.
    for (size_t i = 0; i < map_.item_slots(); ++i) {
        const WorldItem& it = map_.item(uint32_t(i));
        if (it.alive && it.shape == 508 && it.frame == 1) {
            out[0] = it.x; out[1] = it.y; out[2] = it.z;
            return;
        }
    }
    // Otherwise the median position of the map's items.
    std::vector<int> xs, ys, zs;
    for (size_t i = 0; i < map_.item_slots(); ++i) {
        const WorldItem& it = map_.item(uint32_t(i));
        if (!it.alive) continue;
        xs.push_back(it.x); ys.push_back(it.y); zs.push_back(it.z);
    }
    if (xs.empty()) { out[0] = out[1] = 16384; out[2] = 0; return; }
    for (auto* v : {&xs, &ys, &zs}) std::nth_element(v->begin(), v->begin() + long(v->size() / 2), v->end());
    out[0] = xs[xs.size() / 2]; out[1] = ys[ys.size() / 2]; out[2] = zs[zs.size() / 2];
    (void)map;
}

void MapViewer::open_map(unsigned map, const int* at) {
    map_.load(map);
    if (at) std::copy(at, at + 3, focus_);
    else default_focus(map, focus_);
    std::copy(focus_, focus_ + 3, start_);
    if (zoom_ > 0) cam_screen(focus_, centre_[0], centre_[1]);
    int cam[3] = {focus_[0], focus_[1], focus_[2] + 20};
    std::copy(cam, cam + 3, prev_cam_);
    map_.update_fast_area(cam, cam, 320, 200);
}

void MapViewer::handle(const InputEvent& ev) {
    auto set_held = [&](int key, bool down) {
        switch (key) {
        case kKeyUp: held_[0] = down; break;
        case kKeyDown: held_[1] = down; break;
        case kKeyLeft: held_[2] = down; break;
        case kKeyRight: held_[3] = down; break;
        default: break;
        }
    };
    if (atlas_) {
        atlas_view_->handle(ev);
        unsigned m;
        int at[3];
        if (atlas_view_->take_jump(m, at)) {
            atlas_ = false;
            zoom_ = 0;
            open_map(m, at);
        }
        if (atlas_view_->wants_viewer()) atlas_ = false;
        return;
    }
    if (ev.type == InputEvent::kWheel && ev.wheel) {
        change_zoom(zoom_ + (ev.wheel < 0 ? 1 : -1), mouse_x_, mouse_y_);
        return;
    }
    switch (ev.type) {
    case InputEvent::kKeyDown:
        set_held(ev.key, true);
        fast_ = ev.mods & kModShift;
        if (ev.key == '=' || ev.key == '+' || ev.key == ']') change_zoom(zoom_ - 1, 160, 100);
        if (ev.key == '-' || ev.key == '[') change_zoom(zoom_ + 1, 160, 100);
        if (ev.key == 'm' && !ev.repeat) { open_atlas(false); break; }
        if (ev.key == kKeyHome && zoom_ > 0) cam_screen(start_, centre_[0], centre_[1]);
        if (ev.key == kKeyPageUp) focus_[2] = std::min(255, focus_[2] + 8);
        if (ev.key == kKeyPageDown) focus_[2] = std::max(0, focus_[2] - 8);
        if (ev.key == kKeyHome) std::copy(start_, start_ + 3, focus_);
        if (ev.key == 'r' && !ev.repeat) opt_.roof_hiding = !opt_.roof_hiding;
        if (ev.key == 'e' && !ev.repeat) opt_.show_editor = !opt_.show_editor;
        if (ev.key == 't' && !ev.repeat) test_pattern_ = !test_pattern_;
        if ((ev.key == 'n' || ev.key == 'p' || ev.key == kKeyTab) && !maps_.empty()) {
            auto it = std::find(maps_.begin(), maps_.end(), map_.map_number());
            long i = it == maps_.end() ? 0 : long(it - maps_.begin());
            i += (ev.key == 'p' || (ev.mods & kModShift)) ? -1 : 1;
            i = (i + long(maps_.size())) % long(maps_.size());
            open_map(maps_[size_t(i)]);
        }
        break;
    case InputEvent::kKeyUp:
        set_held(ev.key, false);
        fast_ = ev.mods & kModShift;
        break;
    case InputEvent::kMouseMove:
        mouse_x_ = ev.x;
        mouse_y_ = ev.y;
        break;
    case InputEvent::kMouseDown:
        mouse_x_ = ev.x;
        mouse_y_ = ev.y;
        if (zoom_ == 0) picked_ = view_.pick(ev.x, ev.y);
        break;
    default: break;
    }
}

void MapViewer::tick() {
    if (atlas_) { atlas_view_->tick(); return; }
    if (zoom_ > 0) {
        // Zoomed: pan the tile view (8 game pixels a frame, Shift 32); the 1x map stays.
        long st = long(fast_ ? 32 : 8) << zoom_;
        if (held_[0]) centre_[1] -= st;
        if (held_[1]) centre_[1] += st;
        if (held_[2]) centre_[0] -= st;
        if (held_[3]) centre_[0] += st;
        return;
    }
    // Screen up = world -x -y; screen right = world +x -y.
    int step = fast_ ? 64 : 16;
    int dx = 0, dy = 0;
    if (held_[0]) { dx -= step; dy -= step; }
    if (held_[1]) { dx += step; dy += step; }
    if (held_[2]) { dx -= step; dy += step; }
    if (held_[3]) { dx += step; dy -= step; }
    focus_[0] = std::clamp(focus_[0] + dx, 0, 32767);
    focus_[1] = std::clamp(focus_[1] + dy, 0, 32767);
    int cam[3] = {focus_[0], focus_[1], focus_[2] + 20};
    map_.update_fast_area(prev_cam_, cam, 320, 200);
    std::copy(cam, cam + 3, prev_cam_);
}

void MapViewer::render(Surface8& surf) {
    surf.reset_clip();
    surf.clear(0);
    int cam[3] = {focus_[0], focus_[1], focus_[2] + 20};
    view_.render(map_, surf, cam, cam, opt_);

    if (test_pattern_) draw_test_pattern(surf);

    // Status line in the Avatar's speech font (font 6, outlined).
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    char line[160];
    std::snprintf(line, sizeof line, "Map %u  %d,%d,%d  %zu drawn%s%s", map_.map_number(), focus_[0], focus_[1],
                  focus_[2], view_.drawn_items(), view_.roof_limit() < 65536 ? "  (under roof)" : "",
                  opt_.roof_hiding ? "" : "  roof hiding off");
    f.draw_top(surf, 2, 1, line);
    if (picked_) {
        std::snprintf(line, sizeof line, "object %u", picked_);
        f.draw_top(surf, 2, 11, line);
    }
    draw_cursor(surf);
}

void MapViewer::render_rgba(std::vector<uint32_t>& out, int& w, int& h) {
    w = AtlasView::kW;
    h = AtlasView::kH;
    if (atlas_) {
        atlas_view_->render(out, budget_ms_);
        return;
    }
    out.resize(size_t(w) * h);
    if (!tiles_) tiles_ = std::make_unique<TileCache>(data_);
    tiles_->set_show_editor(opt_.show_editor);
    double deadline = budget_ms_ < 0 ? 1e300 : TileCache::now_ms() + budget_ms_;
    const int level = zoom_ - 1;
    pending_ = size_t(draw_tiles(*tiles_, {{map_.map_number(), 0, 0}}, level, int(centre_[0]), int(centre_[1]), out.data(), w,
                                 h, deadline));
    RgbaCanvas c{out.data(), w, h, &data_.palette};
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    // World point at the centre on the ground of the focus height.
    long cz = focus_[2];
    long wx = 2 * centre_[0] + 4 * (centre_[1] + cz), wy = 4 * (centre_[1] + cz) - 2 * centre_[0];
    char line[160];
    std::snprintf(line, sizeof line, "Map %u  zoom 1:%d  %ld,%ld,%ld%s", map_.map_number(), 1 << zoom_, wx, wy, cz,
                  pending_ ? "  rendering..." : "");
    c.text(f, 4, 2, line, 2);
    if (const Shape* m = data_.mouse.shape(0))
        if (const ShapeFrame* fr = m->frame(34)) c.frame(*fr, mouse_x_ * 2, mouse_y_ * 2, 2);
}

void MapViewer::draw_cursor(Surface8& surf) {
    const Shape* m = data_.mouse.shape(0);
    if (!m) return;
    // specs/world/avatar.md 2.1-2.2 and presentation/input.md 4.2: the screen direction is
    // the 8-way sector of (dx, 2*dy) from the Avatar's feet (centre, centre + 14).
    int dx = mouse_x_ - 160, dy = 114 - mouse_y_;
    int dir;
    if (dx == 0) {
        dir = dy > 0 ? 0 : 4;
    } else {
        long t = (1024L * 2 * dy) / dx;
        if (dx < 0) t = -t;  // mirror the left half onto the right
        int right = t > 2472 ? 0 : t > 424 ? 1 : t >= -424 ? 2 : t >= -2472 ? 3 : 4;
        dir = dx > 0 ? right : (8 - right) & 7;
    }
    // Length: long beyond R = 130 / R' = 81 from the true centre, medium beyond 30 / 19 from
    // the feet point (the original's approximate thresholds).
    int len = (std::abs(mouse_x_ - 160) > 130 || std::abs(mouse_y_ - 100) > 81) ? 2
              : (std::abs(dx) > 30 || std::abs(dy) > 19) ? 1 : 0;
    const ShapeFrame* fr = m->frame(unsigned(len * 8 + dir));
    if (fr) surf.draw(*fr, mouse_x_, mouse_y_);
}

void MapViewer::draw_test_pattern(Surface8& surf) {
    // All 42 cursor frames on a dark panel, then one line in each of the 16 fonts.
    surf.fill_rect(4, 22, 312, 174, 0);
    const Shape* m = data_.mouse.shape(0);
    int x = 10, y = 32;
    if (m)
        for (unsigned i = 0; i < m->frame_count(); ++i) {
            const ShapeFrame* fr = m->frame(i);
            if (!fr) continue;
            if (x + fr->w > 310) { x = 10; y += 24; }
            surf.draw(*fr, x + fr->xoff, y + fr->yoff);
            x += fr->w + 4;
        }
    y += 24;
    // Fonts in two columns on a grey strip (font 1 is black-only).
    surf.fill_rect(4, y - 2, 312, 196 - y, 22);
    for (size_t i = 0; i < fonts_.size(); i += 2) {
        int adv = 0;
        for (size_t k = i; k < i + 2 && k < fonts_.size(); ++k) {
            char buf[48];
            if (k == 12 || k == 13) std::snprintf(buf, sizeof buf, "%zu 0123456789", k);
            else if (k == 10 || k == 11) std::snprintf(buf, sizeof buf, "%zu AVATAR", k);
            else std::snprintf(buf, sizeof buf, "%zu: Pagan Avatar 0123", k);
            fonts_[k].draw_top(surf, k % 2 ? 162 : 8, y, buf);
            adv = std::max(adv, fonts_[k].line_advance());
        }
        y += adv;
    }
}

}  // namespace pg
