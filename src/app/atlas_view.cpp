// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "app/atlas_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <map>

#include "app/rgba_draw.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "json/json.hpp"
#include "platform/platform.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pg {

namespace {
long floor_div_l(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
constexpr uint32_t kBackground = 0xFF0A0B12u;
}  // namespace

AtlasView::AtlasView(const GameData& data, std::vector<ShapeFont>& fonts) : data_(data), fonts_(fonts), cache_(data) {
    fps_ = compute_footprints(data);
}

void AtlasView::load_layout(const std::string& path) {
    path_ = path;
    std::string err;
    if (!path.empty() && layout_.load(path, err)) {
        layout_status_ = "layout " + path;
    } else {
        layout_ = generate_atlas_layout(data_);
        layout_status_ = "layout derived from the data" + (path.empty() ? std::string() : " (" + err + ")");
    }
    // A stitched world layout (world/world_layout.h): layers, and S / G leave the file alone
    // (it also holds the seams, links and the editor's areas; the editor saves it).
    world_file_ = false;
    if (!path.empty()) {
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        world_file_ = text.find("\"pagan-world-layout\"") != std::string::npos || text.find("\"seams\"") != std::string::npos;
    }
    layered_ = false;
    for (const AtlasGroup& g : layout_.groups) layered_ |= g.layer != 0;
    layer_ = 0;
    PG_LOG("atlas: %s, %zu groups%s", layout_status_.c_str(), layout_.groups.size(), layered_ ? " on layers" : "");
    refresh_exclusions();
}

void AtlasView::refresh_exclusions() {
    std::map<unsigned, std::bitset<64 * 64>> ex;
    for (const AtlasGroup& g : layout_.groups)
        for (unsigned m : g.maps) ex[m];
    excl_ = atlas_exclusions(layout_, fps_);
    for (auto& [m, b] : excl_) ex[m] = b;
    for (auto& [m, b] : ex) cache_.set_exclusion(m, b);
}

std::vector<PlacedMap> AtlasView::placed() const {
    std::vector<PlacedMap> out;
    for (const AtlasGroup& g : layout_.groups) {
        if (!on_layer(g)) continue;
        int sx, sy;
        atlas_screen_offset(g.dx, g.dy, sx, sy);
        for (unsigned m : g.maps) out.push_back({m, sx, sy});
    }
    return out;
}

void AtlasView::group_screen_box(const AtlasGroup& g, int& l, int& t, int& r, int& b) const {
    l = t = std::numeric_limits<int>::max();
    r = b = std::numeric_limits<int>::min();
    for (unsigned m : g.maps)
        for (const MapFootprint& f : fps_) {
            if (f.map != m) continue;
            int x0 = f.cx0 * 512 + g.dx, x1 = (f.cx1 + 1) * 512 + g.dx;
            int y0 = f.cy0 * 512 + g.dy, y1 = (f.cy1 + 1) * 512 + g.dy;
            l = std::min(l, (x0 - y1) / 4);
            r = std::max(r, (x1 - y0) / 4);
            t = std::min(t, (x0 + y0) / 8 - 160);
            b = std::max(b, (x1 + y1) / 8);
        }
}

void AtlasView::show_all() {
    long l = std::numeric_limits<long>::max(), t = l, r = std::numeric_limits<long>::min(), b = r;
    for (const AtlasGroup& g : layout_.groups) {
        if (!on_layer(g)) continue;
        int gl, gt, gr, gb;
        group_screen_box(g, gl, gt, gr, gb);
        if (gl > gr) continue;
        l = std::min<long>(l, gl); t = std::min<long>(t, gt);
        r = std::max<long>(r, gr); b = std::max<long>(b, gb);
    }
    if (l > r) { cx_ = cy_ = 0; level_ = 6; return; }
    cx_ = (l + r) / 2;
    cy_ = (t + b) / 2;
    level_ = 0;
    while (level_ < TileCache::kMaxLevel && ((r - l) >> level_ > kW - 16 || (b - t) >> level_ > kH - 40)) ++level_;
}

void AtlasView::show(unsigned map, int x, int y, int z, int level) {
    int gi = layout_.group_of(map);
    level_ = std::clamp(level, 0, TileCache::kMaxLevel);
    if (gi < 0) { show_all(); return; }
    const AtlasGroup& g = layout_.groups[size_t(gi)];
    layer_ = g.layer;
    int sx, sy;
    atlas_screen_offset(g.dx, g.dy, sx, sy);
    cx_ = sx + x / 4 - y / 4;
    cy_ = sy + x / 8 + y / 8 - z;
}

void AtlasView::to_native(int u, int v, long& nx, long& ny) const {
    nx = cx_ + long(u - kW / 2) * (1L << level_);
    ny = cy_ + long(v - kH / 2) * (1L << level_);
}

bool AtlasView::hit(long nx, long ny, unsigned& map, int& group, int at[3]) const {
    const auto& ex = excl_;
    for (int gi = int(layout_.groups.size()) - 1; gi >= 0; --gi) {
        const AtlasGroup& g = layout_.groups[size_t(gi)];
        if (!on_layer(g)) continue;
        int ox, oy;
        atlas_screen_offset(g.dx, g.dy, ox, oy);
        for (auto mit = g.maps.rbegin(); mit != g.maps.rend(); ++mit) {
            const MapFootprint* f = nullptr;
            for (const MapFootprint& q : fps_)
                if (q.map == *mit) f = &q;
            if (!f) continue;
            const std::bitset<64 * 64>* exb = nullptr;
            for (auto& [m, b] : ex)
                if (m == *mit) exb = &b;
            long sx = nx - ox, sy = ny - oy;
            for (int z : {f->median_z, 0, 48, 96}) {
                long x = 2 * sx + 4 * (sy + z), y = 4 * (sy + z) - 2 * sx;
                if (x < 0 || y < 0 || x >= 32768 || y >= 32768) continue;
                int cx = int(x / 512), cy = int(y / 512);
                if (!f->has(cx, cy) || (exb && (*exb)[size_t(cy) * 64 + cx])) continue;
                map = *mit;
                group = gi;
                at[0] = int(x); at[1] = int(y); at[2] = z;
                return true;
            }
        }
    }
    return false;
}

void AtlasView::set_level(int lv, int u, int v) {
    lv = std::clamp(lv, 0, TileCache::kMaxLevel);
    long nx, ny;
    to_native(u, v, nx, ny);
    level_ = lv;
    cx_ = nx - long(u - kW / 2) * (1L << level_);
    cy_ = ny - long(v - kH / 2) * (1L << level_);
}

void AtlasView::handle(const InputEvent& ev) {
    auto set_held = [&](int key, bool down) {
        switch (key) {
        case kKeyUp: held_[0] = down; break;
        case kKeyDown: held_[1] = down; break;
        case kKeyLeft: held_[2] = down; break;
        case kKeyRight: held_[3] = down; break;
        default: break;
        }
    };
    const int u = mouse_x_ * 2, v = mouse_y_ * 2;
    switch (ev.type) {
    case InputEvent::kKeyDown:
        set_held(ev.key, true);
        fast_ = ev.mods & kModShift;
        if (ev.key == '=' || ev.key == '+' || ev.key == ']') set_level(level_ - 1, kW / 2, kH / 2);
        if (ev.key == '-' || ev.key == '[') set_level(level_ + 1, kW / 2, kH / 2);
        if (ev.repeat) break;
        if (ev.key == kKeyHome) show_all();
        if (ev.key == 'l') labels_ = !labels_;
        if (ev.key == 'm') back_ = true;
        if (layered_ && (ev.key == kKeyPageUp || ev.key == kKeyPageDown)) {
            std::set<int> ls;
            for (const AtlasGroup& g : layout_.groups) ls.insert(g.layer);
            auto it = ls.find(layer_);
            if (ev.key == kKeyPageUp && it != ls.end() && std::next(it) != ls.end()) layer_ = *std::next(it);
            if (ev.key == kKeyPageDown && it != ls.end() && it != ls.begin()) layer_ = *std::prev(it);
            if (it == ls.end() && !ls.empty()) layer_ = *ls.begin();
            message_ = "layer " + std::to_string(layer_);
            message_ticks_ = 60;
        }
        if ((ev.key == 'g' || ev.key == 's') && world_file_) {
            message_ = "the stitched world layout is edited in the editor (--edit / ?edit) or regenerated with --write-world-layout";
            message_ticks_ = 120;
            break;
        }
        if (ev.key == 'g') {
            layout_ = generate_atlas_layout(data_);
            layout_status_ = "layout derived from the data";
            refresh_exclusions();
            dirty_ = true;
            message_ = "layout regenerated from the data (S saves)";
            message_ticks_ = 90;
        }
        if (ev.key == 's') {
            std::string json = layout_.to_json();
#ifdef __EMSCRIPTEN__
            std::string js = "(function(){const s=" + nlohmann::json(json).dump() +
                             ";console.log(s);const a=document.createElement('a');"
                             "a.href=URL.createObjectURL(new Blob([s],{type:'application/json'}));"
                             "a.download='atlas_layout.json';document.body.appendChild(a);a.click();a.remove();})()";
            emscripten_run_script(js.c_str());
            message_ = "layout downloaded as atlas_layout.json (also in the console)";
            dirty_ = false;
#else
            if (!path_.empty() && layout_.save(path_)) {
                message_ = "saved " + path_;
                dirty_ = false;
            } else {
                std::fputs(json.c_str(), stdout);
                message_ = "could not write the layout file; printed to stdout";
            }
#endif
            PG_LOG("atlas: %s", message_.c_str());
            message_ticks_ = 120;
        }
        break;
    case InputEvent::kKeyUp:
        set_held(ev.key, false);
        fast_ = ev.mods & kModShift;
        break;
    case InputEvent::kWheel:
        if (ev.wheel > 0) set_level(level_ - 1, u, v);
        else if (ev.wheel < 0) set_level(level_ + 1, u, v);
        break;
    case InputEvent::kMouseDown: {
        mouse_x_ = ev.x; mouse_y_ = ev.y;
        down_ = true; moved_ = false;
        down_x_ = last_x_ = ev.x; down_y_ = last_y_ = ev.y;
        drag_acc_x_ = drag_acc_y_ = 0;
        drag_group_ = -1;
        if (ev.button == 3 || (ev.button == 1 && (ev.mods & kModCtrl))) {
            long nx, ny;
            to_native(ev.x * 2, ev.y * 2, nx, ny);
            unsigned m;
            int at[3];
            if (!hit(nx, ny, m, drag_group_, at)) drag_group_ = -1;
            if (drag_group_ < 0) down_ = ev.button == 1;  // nothing to move; a ctrl-drag pans
        }
        break;
    }
    case InputEvent::kMouseMove:
        mouse_x_ = ev.x; mouse_y_ = ev.y;
        if (down_) {
            int dx = ev.x - last_x_, dy = ev.y - last_y_;
            last_x_ = ev.x; last_y_ = ev.y;
            if (std::abs(ev.x - down_x_) + std::abs(ev.y - down_y_) > 2) moved_ = true;
            long nsx = long(dx) * 2 * (1L << level_), nsy = long(dy) * 2 * (1L << level_);
            if (drag_group_ >= 0) {
                // Screen delta (native pixels) to world: x - y = 4 sx, x + y = 8 sy.
                AtlasGroup& g = layout_.groups[size_t(drag_group_)];
                g.dx += int(2 * nsx + 4 * nsy);
                g.dy += int(4 * nsy - 2 * nsx);
            } else {
                cx_ -= nsx;
                cy_ -= nsy;
            }
        }
        break;
    case InputEvent::kMouseUp:
        mouse_x_ = ev.x; mouse_y_ = ev.y;
        if (!down_) break;
        down_ = false;
        if (drag_group_ >= 0 && moved_) {
            AtlasGroup& g = layout_.groups[size_t(drag_group_)];
            if (g.from_data || g.derived.find("moved by hand") == std::string::npos)
                g.derived = "manual: moved by hand in atlas mode (was: " + g.derived + ")";
            g.from_data = false;
            refresh_exclusions();
            dirty_ = true;
            message_ = g.label + " moved to " + std::to_string(g.dx) + "," + std::to_string(g.dy) + " (S saves)";
            message_ticks_ = 120;
        } else if (!moved_ && ev.button == 1) {
            long nx, ny;
            to_native(ev.x * 2, ev.y * 2, nx, ny);
            int gi;
            if (!from_game_ && hit(nx, ny, jump_map_, gi, jump_at_)) jump_ = true;
        }
        drag_group_ = -1;
        break;
    default: break;
    }
}

bool AtlasView::take_jump(unsigned& map, int at[3]) {
    if (!jump_) return false;
    jump_ = false;
    map = jump_map_;
    std::copy(jump_at_, jump_at_ + 3, at);
    return true;
}

void AtlasView::tick() {
    long step = long(fast_ ? 32 : 8) << level_;
    if (held_[0]) cy_ -= step;
    if (held_[1]) cy_ += step;
    if (held_[2]) cx_ -= step;
    if (held_[3]) cx_ += step;
    if (message_ticks_ > 0) --message_ticks_;
}

void AtlasView::draw_overlay(RgbaCanvas& c, long vx0, long vy0, int level, bool labels) const {
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const long s = 1L << level;
    auto P = [&](long x, long y, int& u, int& v) {
        u = int(floor_div_l((x - y) / 4 - vx0, s));
        v = int(floor_div_l((x + y) / 8 - vy0, s));
    };
    struct Box { int x, y, w, h; };
    std::vector<Box> used;
    // Hovered / dragged group last so its outline and label are on top.
    std::vector<size_t> order;
    for (size_t gi = 0; gi < layout_.groups.size(); ++gi)
        if (int(gi) != hover_group_ && int(gi) != drag_group_) order.push_back(gi);
    for (int gi : {hover_group_, drag_group_})
        if (gi >= 0) order.insert(order.begin(), size_t(gi));  // labels: first claim wins
    for (size_t gi : order) {
        const AtlasGroup& g = layout_.groups[gi];
        bool ghost = !on_layer(g);
        if (ghost && std::abs(g.layer - layer_) != 1) continue;  // only the layers just above / below, as outlines
        bool hot = !ghost && (int(gi) == hover_group_ || int(gi) == drag_group_);
        uint32_t col = hot ? 0xFFFFE060u : ghost ? (g.layer > layer_ ? 0xFF6A4A2Au : 0xFF2A4A6Au) : g.from_data ? 0xFF3A5E8Cu : 0xFF5A5A66u;
        double wx = 0, wy = 0, wn = 0;
        for (unsigned m : g.maps)
            for (const MapFootprint& fp : fps_) {
                if (fp.map != m) continue;
                // The map's ground rectangle as an isometric diamond.
                long x0 = fp.cx0 * 512L + g.dx, x1 = (fp.cx1 + 1) * 512L + g.dx;
                long y0 = fp.cy0 * 512L + g.dy, y1 = (fp.cy1 + 1) * 512L + g.dy;
                int pu[4], pv[4];
                P(x0, y0, pu[0], pv[0]);
                P(x1, y0, pu[1], pv[1]);
                P(x1, y1, pu[2], pv[2]);
                P(x0, y1, pu[3], pv[3]);
                for (int k = 0; k < 4; ++k) c.line(pu[k], pv[k], pu[(k + 1) % 4], pv[(k + 1) % 4], col);
                // Label at the middle of the real content (chunks weighted by records).
                for (int cy = fp.cy0; cy <= fp.cy1; ++cy)
                    for (int cx = fp.cx0; cx <= fp.cx1; ++cx) {
                        double w = fp.count[size_t(cy) * 64 + cx];
                        if (w < 3) continue;
                        wx += w * (cx * 512 + 256 + g.dx);
                        wy += w * (cy * 512 + 256 + g.dy);
                        wn += w;
                    }
            }
        if (!labels || wn <= 0 || ghost) continue;
        int lu, lv;
        P(long(wx / wn), long(wy / wn), lu, lv);
        int tw = f.text_width(g.label) + 4, th = f.line_advance() + 3;
        Box bx{lu - tw / 2 - 2, lv - th / 2, tw + 2, th};
        if (bx.x + bx.w < 0 || bx.x > c.w || bx.y + bx.h < 0 || bx.y > c.h) continue;
        bool clash = false;
        for (const Box& o : used)
            clash |= bx.x < o.x + o.w && o.x < bx.x + bx.w && bx.y < o.y + o.h && o.y < bx.y + bx.h;
        if (clash) continue;  // crowded: the label shows when zoomed in or hovered
        used.push_back(bx);
        c.shade(bx.x, bx.y, bx.w, bx.h, 0xFF000000u, hot ? 200 : 140);
        c.text(f, bx.x + 2, bx.y + 1, g.label);
    }
}

void AtlasView::render(std::vector<uint32_t>& out, double budget_ms) {
    out.resize(size_t(kW) * kH);
    double deadline = budget_ms < 0 ? std::numeric_limits<double>::infinity() : TileCache::now_ms() + budget_ms;
    std::vector<PlacedMap> pm = placed();
    int missing = draw_tiles(cache_, pm, level_, int(cx_), int(cy_), out.data(), kW, kH, deadline, kBackground);
    pending_ = size_t(missing);

    long nx, ny;
    to_native(mouse_x_ * 2, mouse_y_ * 2, nx, ny);
    unsigned hm = 0;
    int hat[3] = {0, 0, 0};
    hover_group_ = -1;
    bool over = hit(nx, ny, hm, hover_group_, hat);

    RgbaCanvas c{out.data(), kW, kH, &data_.palette};
    draw_overlay(c, cx_ - long(kW / 2) * (1L << level_), cy_ - long(kH / 2) * (1L << level_), level_, labels_);

    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    char line[200];
    std::string lay = layered_ ? "  layer " + std::to_string(layer_) + " (PgUp/PgDn; outlines: the layers above / below)" : "";
    std::snprintf(line, sizeof line, "Atlas  zoom 1:%ld  %zu groups%s%s%s", 2L << level_, layout_.groups.size(), lay.c_str(),
                  dirty_ ? "  (unsaved)" : "", missing ? "  rendering..." : "");
    c.shade(0, 0, kW, 22, 0xFF000000u, 110);
    c.text(f, 4, 2, line, 2);
    std::string l2;
    if (message_ticks_ > 0) l2 = message_;
    else if (over) {
        const AtlasGroup& g = layout_.groups[size_t(hover_group_)];
        if (g.maps.size() > 1) std::snprintf(line, sizeof line, "%s: map %u at %d,%d,%d", g.label.c_str(), hm, hat[0], hat[1], hat[2]);
        else std::snprintf(line, sizeof line, "%s at %d,%d,%d", g.label.c_str(), hat[0], hat[1], hat[2]);
        l2 = line;
    }
    if (!l2.empty()) {
        c.shade(0, kH - 22, kW, 22, 0xFF000000u, 110);
        c.text(f, 4, kH - 20, l2, 2);
    }
    int mu, mv;
    if (marker_ && marker_pixel(mu, mv)) {
        // The Avatar: a blinking ring with a cross, and a label.
        ++blink_;
        uint32_t col = (blink_ / 8) % 2 ? 0xFFFFFF40u : 0xFFFF3030u;
        for (int r : {7, 8}) {
            for (int a = 0; a < 32; ++a) {
                double t0 = a * 6.2831853 / 32, t1 = (a + 1) * 6.2831853 / 32;
                c.line(mu + int(r * std::cos(t0)), mv + int(r * std::sin(t0)), mu + int(r * std::cos(t1)),
                       mv + int(r * std::sin(t1)), col);
            }
        }
        c.line(mu - 4, mv, mu + 4, mv, col);
        c.line(mu, mv - 4, mu, mv + 4, col);
        c.text(f, mu + 11, mv - 8, "Avatar", 2);
    }
    if (from_game_ && message_ticks_ <= 0 && !over) {
        c.shade(0, kH - 22, kW, 22, 0xFF000000u, 110);
        c.text(f, 4, kH - 20, "M / Tab / Esc: back to the game   wheel or +/-: zoom   drag or arrows: pan", 2);
    }
    if (const Shape* m = data_.mouse.shape(0))
        if (const ShapeFrame* fr = m->frame(drag_group_ >= 0 ? 36 : 34)) c.frame(*fr, mouse_x_ * 2, mouse_y_ * 2, 2);
}

bool AtlasView::marker_pixel(int& u, int& v) const {
    int gi = layout_.group_of(mk_map_);
    if (gi < 0) return false;
    const AtlasGroup& g = layout_.groups[size_t(gi)];
    int sx, sy;
    atlas_screen_offset(g.dx, g.dy, sx, sy);
    long nx = sx + mk_[0] / 4 - mk_[1] / 4, ny = sy + mk_[0] / 8 + mk_[1] / 8 - mk_[2];
    u = int((nx - cx_) / (1L << level_)) + kW / 2;
    v = int((ny - cy_) / (1L << level_)) + kH / 2;
    return true;
}

bool AtlasView::export_png(const std::string& path, int level, bool plain) {
    long l = std::numeric_limits<long>::max(), t = l, r = std::numeric_limits<long>::min(), b = r;
    for (const AtlasGroup& g : layout_.groups) {
        if (!on_layer(g)) continue;
        int gl, gt, gr, gb;
        group_screen_box(g, gl, gt, gr, gb);
        if (gl > gr) continue;
        l = std::min<long>(l, gl - 256); t = std::min<long>(t, gt - 256);
        r = std::max<long>(r, gr + 256); b = std::max<long>(b, gb + 256);
    }
    const long s = 1L << level;
    int w = int((r - l) / s), h = int((b - t) / s);
    h += h & 1;
    if (w <= 0 || h <= 0 || double(w) * h > 400e6) { PG_LOG("export: bad size %dx%d", w, h); return false; }
    PG_LOG("export: %dx%d at 1/%ld native", w, h, s);
    std::vector<uint32_t> img(size_t(w) * h);
    std::vector<PlacedMap> pm = placed();
    double t0 = TileCache::now_ms();
    const int strip = 256;
    for (int y = 0; y < h; y += strip) {
        int sh = std::min(strip, h - y);
        sh += sh & 1;
        if (y + sh > h) sh = h - y;
        long cx = l + long(w / 2) * s, cy = t + long(y) * s + long(sh / 2) * s;
        draw_tiles(cache_, pm, level, int(cx), int(cy), img.data() + size_t(y) * w, w, sh,
                   std::numeric_limits<double>::infinity(), kBackground);
    }
    PG_LOG("export: %zu block renders, %.0f ms", cache_.renders(), TileCache::now_ms() - t0);
    if (!plain) {
        RgbaCanvas c{img.data(), w, h, &data_.palette};
        int saved = hover_group_;
        hover_group_ = -1;
        draw_overlay(c, l, t, level, true);
        hover_group_ = saved;
    }
    return save_png(path, img.data(), w, h, 1);
}

}  // namespace pg
