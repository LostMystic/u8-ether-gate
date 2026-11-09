// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6 (PLAN-M6.md; the engine's own design): the composed game frame. The world is
// drawn on its own 8-bit surface at the zoom level, from a smooth camera, with actors at
// their interpolated (displayed) positions, then enlarged into a frame of `scale` output
// pixels per game pixel and offset by the camera's sub-pixel fraction; the UI (windows,
// barks, the answer menu, the cursor...) is drawn on a 320 x 200 layer with a coverage mask
// and enlarged on top, so it keeps its size at every zoom. Also: zoom, the atlas opened from
// the game, the help page, toasts and the auto-target marker.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "app/atlas_view.h"
#include "data/compat.h"
#include "world/tile_cache.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/assist.h"
#include "game/processes.h"
#include "game/world_stages.h"
#include "gfx/font.h"
#include "gfx/surface.h"

namespace pg {

GameMode::~GameMode() = default;

// ---------------------------------------------------------------------------------------------
// Picking through the view

void GameMode::screen_to_world_px(float ux, float uy, int& wx, int& wy) const {
    if (!vs_.layered) { wx = int(std::floor(ux)); wy = int(std::floor(uy)); return; }
    double sc = double(vs_.S) / vs_.kf;
    wx = int(std::floor((ux - 160.0) * sc + vs_.ww / 2 + vs_.frac_x));
    wy = int(std::floor((uy - 100.0) * sc + vs_.wh / 2 + vs_.frac_y));
}

uint16_t GameMode::pick(int x, int y) { return pick_f(x + 0.5f, y + 0.5f); }

uint16_t GameMode::pick_f(float x, float y) {
    if (vs_.layered && vs_.far) {
        // Far zoom: the live items drawn on top of the cached map, topmost first.
        double fx = x * vs_.S, fy = y * vs_.S;
        for (auto it = far_drawn_.rbegin(); it != far_drawn_.rend(); ++it) {
            const FarDrawn& d = *it;
            if (fx < d.x0 || fy < d.y0 || fx >= d.x0 + d.w || fy >= d.y0 + d.h) continue;
            int col = int((fx - d.x0) * d.m), row = int((fy - d.y0) * d.m);
            if (d.flipped) col = d.f->w - 1 - col;
            if (col >= 0 && row >= 0 && col < d.f->w && row < d.f->h && d.f->pixels[size_t(row) * d.f->w + col] != ShapeFrame::kTransparent)
                return d.id;
        }
        return 0;
    }
    int wx, wy;
    screen_to_world_px(x, y, wx, wy);
    return uint16_t(view_.pick(wx, wy));
}

// ---------------------------------------------------------------------------------------------
// Smooth motion

// Per world tick: where each actor on the map is. A change of position starts a glide from
// the position shown at that moment to the new one, over the time since the previous change
// (at most 6 ticks): steady walking becomes steady motion, a teleport (> 96 units) snaps.
void GameMode::record_motion() {
    World& w = *world_;
    const unsigned map = w.map().map_number();
    const int now = frames_;
    if (w.region_shift.serial != shift_seen_) {
        // A stitched seam crossing: the same world, the new map's coordinates. The smoothing
        // state moves with it, so the view glides on instead of snapping (game/world_stitch.cpp).
        shift_seen_ = w.region_shift.serial;
        const World::RegionShift& d = w.region_shift;
        if (d.to == map && cam_map_ == d.from) {
            for (auto& [id, m] : motion_) {
                if (m.map != d.from) continue;
                m.from[0] += d.dx; m.from[1] += d.dy; m.from[2] += d.dz;
                m.to[0] += d.dx; m.to[1] += d.dy; m.to[2] += d.dz;
                m.map = map;
            }
            cam_prev_[0] += d.dx; cam_prev_[1] += d.dy; cam_prev_[2] += d.dz;
            cam_cur_[0] += d.dx; cam_cur_[1] += d.dy; cam_cur_[2] += d.dz;
            cam_map_ = map;
            last_view_[0] += (d.dx - d.dy) / 4.0;
            last_view_[1] += (d.dx + d.dy) / 8.0 - d.dz;
        }
    }
    for (unsigned n = 1; n < 256; ++n) {
        const Actor* a = w.actor(uint16_t(n));
        if (!a || a->chunk < 0) { motion_.erase(uint16_t(n)); continue; }
        int x, y, z;
        w.location(*a, x, y, z);
        auto it = motion_.find(uint16_t(n));
        if (it == motion_.end() || it->second.map != map) {
            Motion m;
            m.from[0] = m.to[0] = x; m.from[1] = m.to[1] = y; m.from[2] = m.to[2] = z;
            m.t = now; m.dur = 1; m.last_change = now; m.map = map;
            motion_[uint16_t(n)] = m;
            continue;
        }
        Motion& m = it->second;
        if (m.to[0] == x && m.to[1] == y && m.to[2] == z) continue;
        int shown[3];
        double u = std::clamp(double(now - m.t) / m.dur, 0.0, 1.0);
        for (int i = 0; i < 3; ++i) shown[i] = m.from[i] + int(std::lround((m.to[i] - m.from[i]) * u));
        bool jump = std::abs(x - shown[0]) > 96 || std::abs(y - shown[1]) > 96 || std::abs(z - shown[2]) > 96;
        m.dur = std::clamp(now - m.last_change, 1, 6);
        m.last_change = now;
        m.t = now;
        for (int i = 0; i < 3; ++i) m.from[i] = jump ? (i == 0 ? x : i == 1 ? y : z) : shown[i];
        m.to[0] = x; m.to[1] = y; m.to[2] = z;
    }
    // The camera's own target per tick (for cameras that do not follow an actor).
    int c[3];
    w.camera_point(c);
    bool snap = cam_map_ != map || std::abs(c[0] - cam_cur_[0]) + std::abs(c[1] - cam_cur_[1]) > 2048;
    for (int i = 0; i < 3; ++i) { cam_prev_[i] = snap ? c[i] : cam_cur_[i]; cam_cur_[i] = c[i]; }
    if (snap) { have_last_view_ = false; ease_[0] = ease_[1] = 0; }
    cam_map_ = map;
}

bool GameMode::display_pos(uint16_t id, int& x, int& y, int& z) const {
    auto it = motion_.find(id);
    if (it == motion_.end()) return false;
    const Motion& m = it->second;
    // The view between ticks: alpha_ of the way from the last tick to the next.
    double u = std::clamp((frames_ - 1 + alpha_ - m.t + 1) / m.dur, 0.0, 1.0);
    if (frames_ - 1 + alpha_ + 1 - m.t >= m.dur) u = 1;
    x = m.from[0] + int(std::lround((m.to[0] - m.from[0]) * u));
    y = m.from[1] + int(std::lround((m.to[1] - m.from[1]) * u));
    z = m.from[2] + int(std::lround((m.to[2] - m.from[2]) * u));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Zoom

// The furthest zoom-out allowed at frame scale S: the world render grows faster than its area
// (the sorter), measured in Tenebrae: native 0.4 / 0.7 / 1.6 / 7.8 ms at 1, 3/4, 1/2, 1/4.
// Native allows 1/4; the browser stops at 1/3 to stay well inside a 60 Hz frame.
int GameMode::max_zoom_step(int S) {
    S = std::max(1, S);
    int m = 0;
    for (int st = 1; st < S && st <= kMaxZoomStep; ++st) {
        int k = S - st;
#ifdef __EMSCRIPTEN__
        if (3 * k < S) break;  // below 1/3
#else
        if (4 * k < S) break;  // below 1/4
#endif
        m = st;
    }
    return m;
}

// The furthest zoom-out the player may use at frame scale S. Debug mode: all of it. Otherwise at
// most twice the normal view: the lowest whole-pixel step not below 1/2 (at 3x, that is 2/3).
int GameMode::zoom_limit(int S) const {
    S = std::max(1, S);
    if (debug_ && debug_->enabled()) return max_zoom_step(S) + kFarSteps;
    int m = 0;
    for (int st = 1; st <= max_zoom_step(S) && 2 * (S - st) >= S; ++st) m = st;
    return m;
}

void GameMode::zoom_fraction(int& n, int& d) const {
    int S = std::max(1, vs_.S);
    int live = max_zoom_step(S);
    int step = std::min(world_->controls.zoom_step, zoom_limit(S));
    if (step > live) {  // far zoom: 2^f world pixels per output pixel
        n = 1;
        d = S << (step - live);
        return;
    }
    n = S - step;
    d = S;
    for (int g = 2; g <= 4; ++g)
        while (n % g == 0 && d % g == 0) { n /= g; d /= g; }
}

void GameMode::zoom_by(int delta) {
    World& w = *world_;
    int S = std::max(1, vs_.S);
    int limit = zoom_limit(S);
    int before = std::min(w.controls.zoom_step, limit);
    int step = std::clamp(before + delta, 0, limit);
    if (limit == 0) { toast("Zoom needs a window at least twice the game size"); return; }
    w.controls.zoom_step = step;
    int n, d;
    zoom_fraction(n, d);
    char buf[48];
    if (100 * n / d >= 10) std::snprintf(buf, sizeof buf, "Zoom %d%%", 100 * n / d);
    else std::snprintf(buf, sizeof buf, "Zoom %.1f%%", 100.0 * n / d);
    toast(buf, 45);
    if (step != before && on_settings_changed) on_settings_changed();
}

void GameMode::toast(const std::string& text, int frames) {
    toast_ = text;
    toast_frames_ = frames;
    PG_LOG("toast: %s", text.c_str());
    if (toast_ != last_logged_toast_) world_->game_log.add(GameLog::kMessages, GameLog::kInfo, text);  // the HUD's message log
    last_logged_toast_ = toast_;
}

// ---------------------------------------------------------------------------------------------
// The composed frame

void GameMode::render_frame(std::vector<uint32_t>& out, int& fw, int& fh, int scale, double alpha, double dt) {
    World& w = *world_;
    if (atlas_on_ && atlas_) {
        atlas_->render(out, 8.0);
        fw = AtlasView::kW;
        fh = AtlasView::kH;
        return;
    }
    alpha_ = std::clamp(alpha, 0.0, 1.0);
    const int S = std::clamp(scale, 1, 4);
    const int live = max_zoom_step(S);
    const int want = std::min(w.controls.zoom_step, S > 1 ? zoom_limit(S) : 0);
    // Far zoom (beyond the live limit): 2^far world pixels per output pixel, the fixed map
    // from cached tiles, what moves drawn live on top (render_far).
    const int far = want > live ? want - live : 0;
    const int step = far ? live : want;
    const int k = S - step;
    const int ww = (320 * S + k - 1) / k + 2, wh = (200 * S + k - 1) / k + 2;
    const bool smooth = w.controls.smooth_scroll;
    const bool glide = w.controls.responsive;  // actors drawn at their displayed positions

    // The camera: the displayed position of the followed actor, or the camera's own point
    // between the last two ticks; a jump of the target eases in, a teleport snaps.
    double cx, cy, cz;
    bool exact = true;
    uint16_t followed = w.camera_followed();
    int dp[3];
    int gc[3];
    w.camera_point(gc);
    if ((smooth || glide) && followed && w.item(followed) && display_pos(followed, dp[0], dp[1], dp[2])) {
        cx = dp[0]; cy = dp[1]; cz = dp[2] + 20;
    } else if (smooth) {
        cx = cam_prev_[0] + (cam_cur_[0] - cam_prev_[0]) * alpha_;
        cy = cam_prev_[1] + (cam_cur_[1] - cam_prev_[1]) * alpha_;
        cz = cam_prev_[2] + (cam_cur_[2] - cam_prev_[2]) * alpha_;
        exact = cam_prev_[0] == cam_cur_[0] && cam_prev_[1] == cam_cur_[1] && cam_prev_[2] == cam_cur_[2];
    } else {
        cx = gc[0]; cy = gc[1]; cz = gc[2];
    }
    if (w.quake > 0) { cx += w.quake_offset[0]; cy += w.quake_offset[1]; }
    double tsx, tsy;
    if (exact) {
        int ix = int(cx), iy = int(cy), iz = int(cz);
        tsx = (ix - iy) / 4;
        tsy = (ix + iy) / 8 - iz;
    } else {
        tsx = (cx - cy) / 4.0;
        tsy = (cx + cy) / 8.0 - cz;
    }
    if (smooth && have_last_view_) {
        double jx = last_view_[0] - (tsx + ease_[0]), jy = last_view_[1] - (tsy + ease_[1]);
        double jump = std::abs(jx) + std::abs(jy);
        if (jump > 12 && jump < 600) { ease_[0] += jx; ease_[1] += jy; }
        else if (jump >= 600) ease_[0] = ease_[1] = 0;
    } else {
        ease_[0] = ease_[1] = 0;
    }
    double decay = std::exp(-std::max(0.0, dt) / 0.09);
    ease_[0] *= decay;
    ease_[1] *= decay;
    if (std::abs(ease_[0]) < 0.25) ease_[0] = 0;
    if (std::abs(ease_[1]) < 0.25) ease_[1] = 0;
    double vx = tsx + ease_[0], vy = tsy + ease_[1];
    last_view_[0] = vx;
    last_view_[1] = vy;
    have_last_view_ = true;
    // Keep the integer pixel grid: the sub-pixel part is shown in whole output pixels.
    double fx0 = std::floor(vx), fy0 = std::floor(vy);
    double frx = std::round((vx - fx0) * k) / k, fry = std::round((vy - fy0) * k) / k;

    vs_.layered = true;
    vs_.S = S;
    vs_.k = k;
    vs_.far = far;
    vs_.kf = far ? 1.0 / (1 << far) : double(k);
    vs_.ww = ww;
    vs_.wh = wh;
    vs_.cam_sx = int(fx0);
    vs_.cam_sy = int(fy0);
    vs_.frac_x = frx;
    vs_.frac_y = fry;
    vs_.cam_sx_f = vx;
    vs_.cam_sy_f = vy;
    w.view_w = ww;  // far zoom keeps the live limit's fast area
    w.view_h = wh;
    if (far) {
        // The virtual world surface of the far view (for picking and the pointer direction).
        vs_.ww = 320 * S * (1 << far);
        vs_.wh = 200 * S * (1 << far);
        vs_.frac_x = vs_.frac_y = 0;
    }

    if (!world_surf_own_) world_surf_own_ = std::make_unique<Surface8>(ww, wh);
    Surface8& ws = *world_surf_own_;
    if (ws.width() != ww || ws.height() != wh) ws.resize(ww, wh);
    ws.reset_clip();
    ws.clear(0);
    opt_.use_screen_camera = true;
    opt_.camera_sx = vs_.cam_sx;
    opt_.camera_sy = vs_.cam_sy;
    // Responsive: every actor at its displayed position. Classic with smooth scrolling: only the
    // actor the camera follows, so he does not wobble against the smoothly scrolling world.
    if (glide) opt_.display_pos = [this](uint16_t id, int& x, int& y, int& z) { return display_pos(id, x, y, z); };
    else if (smooth && followed)
        opt_.display_pos = [this, followed](uint16_t id, int& x, int& y, int& z) {
            return id == followed && display_pos(id, x, y, z);
        };
    else opt_.display_pos = nullptr;
    auto t0 = std::chrono::steady_clock::now();
    if (!far) render_world(ws, gc);
    else render_far(out, S, far, glide || smooth);
    world_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // World points on the UI layer (barks, windows anchored to items).
    w.screen_mapper = [this, glide](int x, int y, int z, int& sx, int& sy) {
        (void)glide;
        double px = (x / 4 - y / 4) - (vs_.cam_sx + vs_.frac_x);
        double py = (x / 8 + y / 8 - z) - (vs_.cam_sy + vs_.frac_y);
        sx = int(std::floor(160 + px * vs_.kf / vs_.S));
        sy = int(std::floor(100 + py * vs_.kf / vs_.S));
    };

    if (!ui_surf_own_) ui_surf_own_ = std::make_unique<Surface8>(320, 200);
    Surface8& ui = *ui_surf_own_;
    ui.reset_clip();
    ui.clear(0);
    ui_mask_.assign(320 * 200, 0);
    ui.set_mask(&ui_mask_);
    bool inverted = w.invert_state >= 0x4000;
    // Part 1 of the UI; the inversion turns world and UI over; then the rest.
    debug_->draw_under(ui);  // critical-path line, boxes, routes (app/debug_tools.h)
    turn_ui_->draw_world(ui);  // turn-based combat: marker, reachable area, route, targets
    w.gumps.paint(ui, -100, kLayerAbove - 1);
    draw_barks(ui);
    draw_ask(ui);
    w.gumps.paint(ui, kLayerAbove, 100);
    if (inverted) {
        for (Surface8* sp : {&ws, &ui}) {
            if (far && sp == &ws) continue;
            for (int y = 0; y < sp->height() / 2; ++y) {
                uint8_t* a = sp->row(y);
                uint8_t* b = sp->row(sp->height() - 1 - y);
                std::swap_ranges(a, a + sp->width(), b);
            }
        }
        for (int y = 0; y < 100; ++y)
            std::swap_ranges(ui_mask_.begin() + y * 320, ui_mask_.begin() + y * 320 + 320, ui_mask_.begin() + (199 - y) * 320);
    }
    if (hud_) {
        const ShapeFont& f = w.font(0);
        char line[200];
        const Actor* av = w.avatar();
        int zn, zd;
        zoom_fraction(zn, zd);
        std::snprintf(line, sizeof line, "map %u  %d,%d,%d  procs %zu  frame %u  zoom %d%%  world %.1f ms  %s%s",
                      w.map().map_number(), av ? av->x : 0, av ? av->y : 0, av ? av->z : 0, w.kernel().size(),
                      w.kernel().frame_number(), 100 * zn / zd, world_ms_, w.stasis ? "stasis " : "",
                      av && av->is(Actor::kInCombat) ? "combat" : "");
        f.draw_top(ui, 2, 1, line);
    }
    debug_->draw_over(ui);
    draw_overlays(ui);
    turn_ui_->draw_hud(ui);  // turn-based combat: the turn strip and the action menu
    draw_cursor(ui);
    ui.set_mask(nullptr);

    auto t1 = std::chrono::steady_clock::now();
    // Compose: palette, enlargement with the sub-pixel offset, the UI blocks on top.
    uint32_t lut[256];
    const PaletteTransform& t = w.palette;
    for (int i = 0; i < 256; ++i) {
        uint8_t c[3];
        t.apply(data_.palette.rgb8[i].data(), c);
        lut[i] = 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
    // The virtue stages (game/world_stages.h): the world's colour shift and sun rays, never the UI's.
    uint32_t wlut_buf[256];
    const uint32_t* wlut = lut;
    WorldStages* stg = w.stages.get();
    if (stg) {
        stg->view_frame(dt);
        if (!far && stg->world_lut(lut, wlut_buf)) wlut = wlut_buf;
    }
    const bool rays = stg && !far && !inverted && stg->rays_level() > 0;
    const bool glows = stg && !far && !inverted && stg->glow_level() > 0 && stg->glows_in_view() > 0;
    fw = 320 * S;
    fh = 200 * S;
    out.resize(size_t(fw) * fh);
    if (far) {
        // The world is already in `out`: only the UI blocks.
        if (inverted)
            for (int y = 0; y < fh / 2; ++y)
                std::swap_ranges(out.begin() + long(y) * fw, out.begin() + long(y + 1) * fw, out.begin() + long(fh - 1 - y) * fw);
        for (int y = 0; y < fh; ++y) {
            uint32_t* dst = &out[size_t(y) * fw];
            int uy = y / S;
            const uint8_t* m = &ui_mask_[size_t(uy) * 320];
            const uint8_t* up = ui.row(uy);
            for (int ux = 0; ux < 320; ++ux)
                if (m[ux]) {
                    uint32_t c = lut[up[ux]];
                    for (int q = 0; q < S; ++q) dst[ux * S + q] = c;
                }
        }
        flash_picture(out);
        compose_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        return;
    }
    // Frame pixel f shows world pixel floor((f + 0.5 - centre) / k + ww/2 + frac).
    static thread_local std::vector<int> col;
    col.resize(size_t(fw));
    for (int x = 0; x < fw; ++x)
        col[size_t(x)] = std::clamp(int(std::floor((x + 0.5 - 160.0 * S) / k + ww / 2 + frx)), 0, ww - 1);
    for (int y = 0; y < fh; ++y) {
        int wy = std::clamp(int(std::floor((y + 0.5 - 100.0 * S) / k + wh / 2 + fry)), 0, wh - 1);
        const uint8_t* src = ws.row(wy);
        uint32_t* dst = &out[size_t(y) * fw];
        for (int x = 0; x < fw; ++x) dst[x] = wlut[src[col[size_t(x)]]];
        if (rays)
            stg->rays_row(dst, fw, (0.5 - 160.0 * S) / k + frx + vs_.cam_sx, 1.0 / k,
                          (y + 0.5 - 100.0 * S) / k + fry + vs_.cam_sy, double(y) / fh);
        if (glows)  // lamps at night (world_stages.h)
            stg->glows_row(dst, fw, (0.5 - 160.0 * S) / k + frx + vs_.cam_sx, 1.0 / k, (y + 0.5 - 100.0 * S) / k + fry + vs_.cam_sy);
        int uy = y / S;
        const uint8_t* m = &ui_mask_[size_t(uy) * 320];
        const uint8_t* up = ui.row(uy);
        for (int ux = 0; ux < 320; ++ux)
            if (m[ux]) {
                uint32_t c = lut[up[ux]];
                for (int s = 0; s < S; ++s) dst[ux * S + s] = c;
            }
    }
    flash_picture(out);
    compose_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
}

// The Lightning spell's flash: one presented frame of the game picture gone pale (the side panel
// is drawn apart and stays as it is).
void GameMode::flash_picture(std::vector<uint32_t>& out) {
    if (!world_->sky_flash) return;
    world_->sky_flash = 0;
    for (uint32_t& c : out) {
        uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        c = 0xFF000000u | (((r * 3 + 240 * 7) / 10) << 16) | (((g * 3 + 244 * 7) / 10) << 8) | ((b * 3 + 255 * 7) / 10);
    }
}

// ---------------------------------------------------------------------------------------------
// Far zoom: the fixed map from the tile cache (the map viewer's tiles of the fixed items only),
// everything else (actors, items that can move, sprites) drawn live on top, shrunk by 2^far
// with nearest sampling, in a rough back-to-front order. Sorting against the static map is
// not done (at these scales it cannot be seen); picking works on the live items.

void GameMode::render_far(std::vector<uint32_t>& out, int S, int far, bool glide) {
    World& w = *world_;
    const int fw = 320 * S, fh = 200 * S, m = 1 << far;
    out.assign(size_t(fw) * fh, 0xFF000000u);
    if (!far_cache_) {
        far_cache_ = std::make_unique<TileCache>(data_);
        far_cache_->set_fixed_only(true);
        far_cache_->set_budget_bytes(size_t(96) << 20);
    }
    const unsigned map = w.map().map_number();
    std::vector<PlacedMap> pm{{map, 0, 0}};
    far_cache_->next_frame(far);
    double budget = far_budget_ms_ < 0 ? std::numeric_limits<double>::infinity() : far_budget_ms_;
    far_missing_ = draw_tiles(*far_cache_, pm, far, vs_.cam_sx, vs_.cam_sy, out.data(), fw, fh, TileCache::now_ms() + budget);
    // The live items.
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i) {
        uint8_t c[3];
        w.palette.apply(data_.palette.rgb8[i].data(), c);
        lut[i] = 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
    struct D { int key; uint16_t id; int x, y, z; };
    std::vector<D> list;
    const int half_w = fw / 2 * m + 256, half_h = fh / 2 * m + 256;
    CurrentMap& cm = w.map();
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            const auto& ids = cm.chunk(cx, cy);
            if (ids.empty()) continue;
            // Chunk centre on screen, quick reject.
            int wx = cx * 512 + 256, wy = cy * 512 + 256;
            int sx = (wx - wy) / 4 - vs_.cam_sx, sy = (wx + wy) / 8 - vs_.cam_sy;
            if (std::abs(sx) > half_w + 256 || std::abs(sy) > half_h + 384) continue;
            for (uint16_t id : ids) {
                const Item* it = w.item(id);
                if (!it || it->origin == Item::kFixed || it->origin == Item::kGlob) continue;
                if (opt_.hide_id && id == opt_.hide_id) continue;
                const ShapeInfo& si = data_.typeflags.get(it->shape);
                if (si.is(ShapeInfo::kEditor) && !opt_.show_editor) continue;
                if ((it->flags & kItemInvisible) && id != 1) continue;
                int x = it->x, y = it->y, z = it->z;
                if (id < 256 && (glide || id == w.camera_followed())) display_pos(id, x, y, z);
                list.push_back({x + y + 4 * z, id, x, y, z});
            }
        }
    std::sort(list.begin(), list.end(), [](const D& a, const D& b) { return a.key < b.key || (a.key == b.key && a.id < b.id); });
    far_drawn_.clear();
    for (const D& d : list) {
        const Item* it = w.item(d.id);
        const ShapeFrame* f = data_.shapes.frame(it->shape, it->frame);
        if (!f) continue;
        bool flipped = it->flags & kItemFlipped;
        int sx = (d.x - d.y) / 4 - vs_.cam_sx, sy = (d.x + d.y) / 8 - d.z - vs_.cam_sy;  // anchor, native
        int left = flipped ? sx + f->xoff - f->w + 1 : sx - f->xoff, top = sy - f->yoff;
        double x0 = fw / 2.0 + double(left) / m, y0 = fh / 2.0 + double(top) / m;
        int dw = std::max(1, (f->w + m - 1) / m), dh = std::max(1, (f->h + m - 1) / m);
        if (x0 + dw < 0 || y0 + dh < 0 || x0 >= fw || y0 >= fh) continue;
        far_drawn_.push_back({d.id, f, flipped, x0, y0, double(dw), double(dh), double(m)});
        int ix0 = int(std::floor(x0)), iy0 = int(std::floor(y0));
        for (int r = 0; r < dh; ++r) {
            int oy = iy0 + r;
            if (oy < 0 || oy >= fh) continue;
            int srow = std::min(f->h - 1, r * m + m / 2);
            const uint8_t* src = &f->pixels[size_t(srow) * f->w];
            uint32_t* dst = &out[size_t(oy) * fw];
            for (int c = 0; c < dw; ++c) {
                int ox = ix0 + c;
                if (ox < 0 || ox >= fw) continue;
                int scol = std::min(f->w - 1, c * m + m / 2);
                if (flipped) scol = f->w - 1 - scol;
                uint8_t p = src[scol];
                if (p != ShapeFrame::kTransparent) dst[ox] = lut[p];
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Overlays: the auto-target, toasts, the help page

namespace {
uint8_t nearest_index(const Palette& pal, int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 16; i < 256; ++i) {
        const auto& c = pal.rgb8[i];
        int d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}
}  // namespace

void GameMode::draw_overlays(Surface8& surf) {
    World& w = *world_;
    // The controller's auto-target: the target cursor over it.
    if (pads_ > 0 && target_ && !w.gumps.top_modal() && !w.stasis) {
        if (const Item* it = w.item(target_)) {
            int sx, sy;
            w.screen_point(*w.root_container(*it), sx, sy);
            int zt = int(data_.typeflags.get(it->shape).z()) * 8;
            if (vs_.layered) zt = int(zt * vs_.kf / vs_.S);
            if (const Shape* m = data_.mouse.shape(0))
                if (const ShapeFrame* fr = m->frame(kCursorTarget)) {
                    int bob = (frames_ / 4) % 2;
                    surf.draw(*fr, sx, sy - zt / 2 - bob);
                }
        }
    }
    // --debug-assist: the last movement-assist probes (red: cannot stand, yellow: the way on
    // is blocked or unsafe, green: chosen).
    if (w.debug_assist) {
        auto nearest = [&](int r, int g, int b) {
            int best = 0, bd = 1 << 30;
            for (int i = 16; i < 256; ++i) {
                const auto& c = data_.palette.rgb8[i];
                int d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
                if (d < bd) { bd = d; best = i; }
            }
            return uint8_t(best);
        };
        static uint8_t col[3] = {0, 0, 0};
        if (!col[0]) { col[0] = nearest(255, 0, 0); col[1] = nearest(255, 255, 0); col[2] = nearest(0, 255, 0); }
        for (const auto& p : assist::last_probes) {
            int sx, sy;
            if (w.screen_mapper) w.screen_mapper(p[0], p[1], p[2], sx, sy);
            else {
                int cam[3];
                w.camera_point(cam);
                sx = 160 + (p[0] - p[1]) / 4 - (cam[0] - cam[1]) / 4;
                sy = 100 + (p[0] + p[1]) / 8 - p[2] - ((cam[0] + cam[1]) / 8 - cam[2]);
            }
            surf.fill_rect(sx - 1, sy - 1, 3, 3, col[std::clamp(p[3], 0, 2)]);
        }
    }
    // Easy ledges: the ledge a held jump is going for, a bright edge line and a chevron above
    // it for a few frames (game/ledge.h kHintFrames).
    if (w.ledge_hint.frames > 0 && w.controls.responsive && !w.gumps.top_modal()) {
        auto to_screen = [&](int x, int y, int z, int& sx, int& sy) {
            if (w.screen_mapper) { w.screen_mapper(x, y, z, sx, sy); return; }
            int cam[3];
            w.camera_point(cam);
            sx = 160 + (x - y) / 4 - (cam[0] - cam[1]) / 4;
            sy = 100 + (x + y) / 8 - z - ((cam[0] + cam[1]) / 8 - cam[2]);
        };
        static uint8_t hi = 0, lo = 0;
        if (!hi) { hi = nearest_index(data_.palette, 255, 236, 96); lo = nearest_index(data_.palette, 40, 24, 8); }
        const World::LedgeHint& h = w.ledge_hint;
        int px = -dir_y(h.dir), py = dir_x(h.dir);
        int ax, ay, bx, by;
        to_screen(h.x - 28 * px, h.y - 28 * py, h.z, ax, ay);
        to_screen(h.x + 28 * px, h.y + 28 * py, h.z, bx, by);
        int n = std::max(std::abs(bx - ax), std::abs(by - ay));
        for (int i = 0; i <= n; ++i) {
            int x = ax + (n ? (bx - ax) * i / n : 0), y = ay + (n ? (by - ay) * i / n : 0);
            surf.fill_rect(x, y + 1, 1, 1, lo);
            surf.fill_rect(x, y - 1, 1, 2, hi);
        }
        int mx = (ax + bx) / 2, my = std::max(1, (ay + by) / 2 - 8 - (h.frames / 3) % 2);  // (on screen)
        for (int i = 0; i < 5; ++i) {
            surf.fill_rect(mx - i, my + i + 1, 1, 1, lo);
            surf.fill_rect(mx + i, my + i + 1, 1, 1, lo);
            surf.fill_rect(mx - i, my + i, 1, 1, hi);
            surf.fill_rect(mx + i, my + i, 1, 1, hi);
        }
    }
    if (toast_frames_ > 0 && !toast_.empty()) {
        const ShapeFont& f = w.font(1);
        int tw = f.text_width(toast_);
        int x = (320 - tw) / 2, y = 22;
        static uint8_t paper = 0;
        if (!paper) paper = nearest_index(data_.palette, 222, 206, 160);
        surf.fill_rect(x - 3, y - 2, tw + 6, f.height() + 4, paper);
        f.draw_top(surf, x, y, toast_);
    }
    if (help_page_ >= 0) draw_help(surf);
}

namespace {
struct HelpLine { const char* key; const char* what; };
const HelpLine kHelpMouse[] = {
    {"Right button held", "walk / run toward the pointer (longer arrow = faster)"},
    {"Right click", "one step; double click on the Avatar: combat on / off"},
    {"Left click", "look (shows the name); double click: use, talk, open"},
    {"Left drag", "move an item; onto the Avatar = into the backpack"},
    {"Both buttons", "jump toward the pointer (Classic: standing = jump to"},
    {"", "the spot pointed at, if it is ground in reach (v2.12))"},
    {"Middle click", "walk there (pathfinder)"},
    {"Wheel", "zoom the view in / out"},
    {"In combat", "left double click attack, left held block, right double kick"},
    {"Click (speech)", "ends a speech line (in a scene: anywhere); hold: faster"},
    {"Windows", "drag the frame to move; click look, double click use"},
    {"", ""},
    {"Movement assist", "pushing at a doorway or a narrow path lines him up"},
    {"", "(Responsive): walking never takes him into water, lava"},
    {"", "or a deadly drop - jump to do that on purpose."},
    {"Jumps", "(Responsive) Space / X jump forward; a jump that"},
    {"", "would land in water or a drop lands on a safe platform"},
    {"", "within 30 degrees if there is one."},
    {"Hold jump", "(Responsive) climbs any ledge in reach (Easy Ledges:"},
    {"", "walks him to it, jump-grab-pull-up); catches falling ones."},
    {"Open doors", "(Responsive) do not block; a door opens even when he"},
    {"", "stands in its way, and never closes on anyone."},
};
const HelpLine kHelpKeys[] = {
    {"Arrows", "forward, turn round, turn left / right"},
    {"Shift / Ctrl", "run / careful step;  Space: jump, climb"},
    {"M or Tab", "world map (atlas); M / Tab / Esc back to the game"},
    {"+ / -", "zoom in / out"},
    {"F1", "this help"},
    {"C", "combat mode on / off"},
    {"I  Z", "backpack, paperdoll"},
    {"B  R  K", "use the bedroll, the recall item, the keyring"},
    {"G", "the spellbook (double-click a spell to cast it)"},
    {"F7  F8", "quick-save, quick-load (one slot outside the diary)"},
    {"1  2  3", "level-up prompt: a point to STR, DEX, INT"},
    {"O", "options (controls, smooth scrolling, sound...)"},
    {"Esc", "main menu (Map, Controls, Quit...); closes books"},
    {"Backspace", "close all item windows"},
    {"1-9", "choose an answer (arrows + Enter: the highlighted one)"},
    {"P", "party bar on / off (companions: double click one to talk)"},
    {"Enter", "end a line of speech (hold: fast-forward a scene)"},
    {"Space (scene)", "the same while the Avatar waits in a scene"},
    {"H  Shift+R", "status line, roof hiding"},
    {"Home", "camera back to the Avatar"},
    {"Alt+X", "quit box (also the main menu's Quit)"},
    {"F9 F11", "scale mode, fullscreen (F10: 4:3)"},
    {"F10", "debug menu (debug mode only; 4:3 is then Shift+F10)"},
};
const HelpLine kHelpPad[] = {
    {"Left stick", "move (walk; run when pushed further)"},
    {"A", "click the target / pointer: tap look, tap twice use / talk"},
    {"", "(combat: attack), hold + right stick: drag; scene: next line"},
    {"X", "jump (forward); hold: grab a ledge and climb;"},
    {"", "pointer on a container: sort"},
    {"B", "close the window under the pointer, else the newest one"},
    {"Y", "backpack"},
    {"LB  RB", "paperdoll, combat mode on / off"},
    {"LB + D-pad L / R", "pick a party member on the bar; A or Y: his windows"},
    {"LB + Up / Down", "quick-save, quick-load"},
    {"LB + B / Y / X", "use the bedroll, the recall item, the keyring"},
    {"LT  RT", "block, attack (RT in a scene: fast-forward)"},
    {"D-pad Left / Right", "next / previous target (nearest in front)"},
    {"D-pad Up / Down", "zoom in / out"},
    {"Answers", "stick / D-pad: the answer above, below, left, right;"},
    {"", "A answers, B: bye / back (the hint under the answers)"},
    {"Right stick", "the pointer (A is its left button)"},
    {"R3 (press it)", "side panel: D-pad moves, A press, LB / RB scroll"},
    {"L3 (press it)", "level-up prompt: D-pad picks, A chooses, B leaves"},
    {"Start", "main menu (hold 1 s: debug menu, in debug mode)"},
    {"Back", "world map; hold 1 s: this help"},
    {"Menus, books", "pointer + A; B: back; D-pad: arrows; LB / RB: pages"},
    {"Movies", "Start / A / B: skip prompt; then A yes, B no"},
    {"Atlas", "left stick pans, D-pad / triggers zoom, B closes"},
};

}  // namespace

namespace {
struct HelpPage {
    std::string title;
    std::vector<std::pair<std::string, std::string>> lines;
};
}  // namespace

// The help book: the four sections (mouse, keyboard, controller, settings), each split over as
// many pages as its lines need (font 1 on the 192-pixel parchment), so nothing runs off the page.
static std::vector<HelpPage> help_book(const GameMode& gm, const ShapeFont& f, const World& w, int pads,
                                       const std::string& pad_name, int zoom_pct) {
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;
    auto table = [](const HelpLine* l, size_t n) {
        std::vector<std::pair<std::string, std::string>> v;
        for (size_t i = 0; i < n; ++i) v.emplace_back(l[i].key, l[i].what);
        return v;
    };
    sections.emplace_back("Mouse", table(kHelpMouse, sizeof kHelpMouse / sizeof kHelpMouse[0]));
    sections.emplace_back("Keyboard", table(kHelpKeys, sizeof kHelpKeys / sizeof kHelpKeys[0]));
    auto pad = table(kHelpPad, sizeof kHelpPad / sizeof kHelpPad[0]);
    pad.emplace_back("", "");
    pad.emplace_back(pads > 0 ? "Connected: " + pad_name : "No controller connected", "");
    sections.emplace_back("Controller", pad);
    const World::Controls& c = w.controls;
    std::vector<std::pair<std::string, std::string>> st = {
        {"Controls", c.responsive ? "Responsive (instant clicks, turns, interrupts)" : "Classic (the original's timing)"},
        {"Smooth scroll", c.smooth_scroll ? "on" : "off"},
        {"Click text", c.click_text ? "on" : "off"},
        {"Move assist", c.assist ? "on (Responsive only)" : "off"},
        {"Zoom", std::to_string(zoom_pct) + "% (wheel, + / -, D-pad Up / Down)"},
        {"", ""},
        {"Change these in the options (O, or Esc then Options).", ""},
    };
    sections.emplace_back("Settings", st);
    std::vector<std::pair<std::string, std::string>> about = {
        {"U8 Ether Gate", "an engine for Ultima VIII: Pagan"},
        {"Your copy", compat::about_line()},
        {"License", "GNU GPL v3 or later; see LICENSE"},
    };
    if (!compat::copy_known()) about.emplace_back("", "Extras made for other data are off.");
    sections.emplace_back("About", about);
    (void)gm;
    const int top = 7 + f.line_advance() + 4, bottom = 192;
    const size_t per = size_t(std::max(1, (bottom - top) / (f.line_advance() + 1)));
    std::vector<HelpPage> book;
    for (auto& sec : sections) {
        std::vector<HelpPage> parts;
        for (size_t a = 0; a < sec.second.size();) {
            size_t b = std::min(sec.second.size(), a + per);
            // A page never starts with a continuation line: it goes over with its key.
            if (b < sec.second.size())
                while (b > a + 1 && sec.second[b].first.empty() && !sec.second[b].second.empty()) --b;
            HelpPage pg;
            pg.lines.assign(sec.second.begin() + long(a), sec.second.begin() + long(b));
            parts.push_back(std::move(pg));
            a = b;
        }
        for (size_t k = 0; k < parts.size(); ++k) {
            parts[k].title = sec.first + (parts.size() > 1 ? " (" + std::to_string(k + 1) + "/" + std::to_string(parts.size()) + ")" : "");
            book.push_back(std::move(parts[k]));
        }
    }
    return book;
}

int GameMode::help_pages() const {
    int zn, zd;
    zoom_fraction(zn, zd);
    return int(help_book(*this, world_->font(1), *world_, pads_, pad_name_, 100 * zn / zd).size());
}

int GameMode::help_overflows() const {
    int zn, zd;
    zoom_fraction(zn, zd);
    const ShapeFont& f = world_->font(1);
    int bad = 0;
    for (const HelpPage& pg : help_book(*this, f, *world_, pads_, pad_name_, 100 * zn / zd))
        for (const auto& l : pg.lines) {
            bool over = l.second.empty() ? 8 + f.text_width(l.first) > 313 : 8 + f.text_width(l.first) > 86 || 88 + f.text_width(l.second) > 313;
            if (over) { ++bad; PG_LOG("help: too wide: %s | %s", l.first.c_str(), l.second.c_str()); }
        }
    return bad;
}

void GameMode::draw_help(Surface8& surf) {
    World& w = *world_;
    const ShapeFont& f = w.font(1);
    static uint8_t paper = 0, edge = 0;
    if (!paper) { paper = nearest_index(data_.palette, 222, 206, 160); edge = nearest_index(data_.palette, 96, 64, 32); }
    surf.fill_rect(4, 4, 312, 192, edge);
    surf.fill_rect(6, 6, 308, 188, paper);
    int zn, zd;
    zoom_fraction(zn, zd);
    std::vector<HelpPage> book = help_book(*this, f, w, pads_, pad_name_, 100 * zn / zd);
    int n = int(book.size());
    help_page_ = std::clamp(help_page_, 0, n - 1);
    const HelpPage& pg = book[size_t(help_page_)];
    char head[128];
    std::snprintf(head, sizeof head, "Help %d/%d: %s     (click or Right: next page; any other key closes)", help_page_ + 1, n,
                  pg.title.c_str());
    f.draw_top(surf, 8, 7, head);
    int y = 7 + f.line_advance() + 4;
    surf.set_clip(6, 6, 308, 188);
    for (const auto& l : pg.lines) {
        f.draw_top(surf, 8, y, l.first);
        f.draw_top(surf, 88, y, l.second);
        y += f.line_advance() + 1;
    }
    surf.reset_clip();
}

// ---------------------------------------------------------------------------------------------
// The atlas from the game

void GameMode::open_atlas() {
    World& w = *world_;
    if (atlas_on_) return;
    // The stitched world shows its own layout (layers, seams placed exactly); the atlas is
    // rebuilt when the World option changed since it was opened last.
    std::string want = w.stitch && !w.world_layout_path.empty() ? w.world_layout_path
                                                               : (atlas_path_.empty() ? std::string("data/atlas_layout.json") : atlas_path_);
    if (atlas_ && atlas_loaded_path_ != want) atlas_.reset();
    if (!atlas_) {
        if (atlas_fonts_.empty())
            for (unsigned i = 0; i < data_.fonts.count(); ++i) atlas_fonts_.emplace_back(data_.fonts.shape(i), int(i));
        atlas_ = std::make_unique<AtlasView>(data_, atlas_fonts_);
        atlas_->load_layout(want);
        atlas_loaded_path_ = want;
        atlas_->set_from_game(true);
    }
    const Actor* av = w.avatar();
    int x = 0, y = 0, z = 0;
    if (av) w.location(*av, x, y, z);
    atlas_->show(w.map().map_number(), x, y, z, 3);
    atlas_->set_marker(w.map().map_number(), x, y, z);
    atlas_->set_mouse(mouse_x_, mouse_y_);
    // Nothing the player holds carries over into the atlas.
    PlayerInput& in = w.input;
    for (ButtonState& b : in.button) { b.down = false; b.handled = true; }
    in.key_forward = in.key_back = in.key_turn_left = in.key_turn_right = in.key_jump = false;
    in.pad_move = false;
    atlas_on_ = true;
    PG_LOG("atlas opened from the game (map %u at %d,%d,%d)", w.map().map_number(), x, y, z);
}

void GameMode::close_atlas() {
    if (!atlas_on_) return;
    atlas_on_ = false;
    for (int i = 0; i < 4; ++i) atlas_keys_[i] = false;
    PG_LOG("atlas closed");
}

void GameMode::game_atlas_input(const InputEvent& ev) {
    if (!atlas_) return;
    atlas_->handle(ev);
    unsigned m;
    int at[3];
    // Clicks never teleport from the game, except the debug menu's atlas warp.
    if (atlas_->take_jump(m, at) && debug_->atlas_jump(m, at)) { close_atlas(); return; }
    if (atlas_->wants_viewer()) close_atlas();
}

void GameMode::atlas_tick() {
    if (!atlas_) return;
    pad_tick();
    atlas_->set_mouse(mouse_x_, mouse_y_);
    atlas_->tick();
}

}  // namespace pg
