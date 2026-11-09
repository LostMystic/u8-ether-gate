// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The critical-path walking route (game/route_guide.*, drawn by app/debug_tools.cpp): routes
// for the first steps of the critical path are found with the walk grid and the real
// pathfinder, and are walkable: the Avatar is driven along each one with the controller's
// stick (Responsive mode, the default) and the movement code, and reaches the target
// (camp -> dock; dock -> gate -> Central Tenebrae; the dragon-statue waypoint on the raised
// palace grounds; Bentic over the 40/41 seam in the stitched world). Planning is sliced and the
// time per frame is measured; re-planning (stray, timer, door state) works; the guide draws the
// route line in the game mode (shots critpath_route_*.png). Teleporters inside a map are part of
// the route: Mythran's house is reached through two (walked; shots critpath_mythran_*.png), and
// every step of the critical path is planned from where the step before leaves the Avatar.
// Probes: PG_ROUTE_FROM=map,x,y,z PG_ROUTE_TO=x,y,z [PG_ROUTE_GO PG_ROUTE_EGGS PG_ROUTE_ZMAP=x0,y0,x1,y1,step
// PG_ROUTE_WALK=dir,frames PG_ROUTE_GLOBALS="pos:n=v;..."]; PG_ROUTE_SWEEP / PG_ROUTE_MYTHRAN run one part.
//   pagan_route_tests GAME_DIR DATA_DIR [SHOT_DIR]
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "check.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/critpath.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/route_guide.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

int g_fail = 0, g_pass = 0;

namespace {

using namespace pg;

std::string g_data, g_shots;

// A World-only run with the 30 fps input clock, Responsive controls (the game's default).
struct Run {
    World w;
    int frames = 0;
    CritPath cp;
    WorldCritContext ctx{w};
    Run(const GameData& d, bool stitched) : w(d) {
        World::auto_ui_given().clear();
        w.auto_ui = true;
        w.ask_name = false;
        w.skip_opening = true;
        if (stitched) CHECK(w.set_world_mode(true, g_data + "/world_layout.json"));
        w.new_game();
        w.controls.responsive = true;
        w.controls.assist = true;
        run(30);
        std::string err;
        CHECK(cp.load_file(g_data + "/critpath.json", err));
        CHECK(cp.load_file(g_data + "/map_links.json", err));
    }
    void step() {
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    void run(int n) { for (int i = 0; i < n; ++i) step(); }
    Actor& av() { return *w.avatar(); }
    unsigned map() { return w.map().map_number(); }
    void warp(int x, int y, int z, unsigned m) {
        w.kernel().kill(1, kProcAny);
        w.avatar()->set(Actor::kAnimLock, false);
        if (w.camera_followed() != 1) w.camera_follow(1);
        w.teleport_actor(1, x, y, z, m);
        w.stasis = false;
        run(10);
    }
    void foot(int& x, int& y, int& z) {
        int xd, yd, zd;
        w.footprint(av(), xd, yd, zd);
        x = av().x - xd / 2; y = av().y - yd / 2; z = av().z;
    }
    // The guide's target for step `id` from here (the next link on the way if elsewhere).
    CritGuide guide(const std::string& id) {
        ctx.observe();
        cp.jump_to = cp.step_index(id);
        return cp.guide(ctx);
    }
};

void print_route(const Route& r) {
    std::printf("    route: status %d, %zu points, length %d, %d teleporters, refined %d%s%s\n", int(r.status), r.pts.size(), r.length,
                r.jumps, r.refined, r.reason.empty() ? "" : ", reason: ", r.reason.c_str());
}

// Drives the Avatar along the guide's route with the stick, re-planned live as he goes, until
// he is at the target (or the map changes when `until_map` is set). Climbs: stop, then the
// jump button toward the ledge; closed doors on the way: used (a double-click) when he is near.
struct Walk { bool reached = false; int frames = 0; int climbs = 0, doors = 0, replans = 0; double max_slice = 0; };

Walk walk_route(Run& r, RouteGuide& rg, const int& tx, const int& ty, const int& tz, int max_frames, int until_map = -1,
                int radius = 48, const std::function<bool()>& each = {}) {
    Walk res;
    PlayerInput& in = r.w.input;
    unsigned m0 = r.map();
    int stuck = 0, lx = -1, ly = -1, jump_wait = 0, unstick = 0;
    uint32_t serial = 0;
    size_t idx = 1;
    std::map<uint16_t, int> door_used;
    const bool trace = std::getenv("PG_ROUTE_TRACE") != nullptr;
    for (int f = 0; f < max_frames; ++f) {
        res.frames = f;
        if (until_map >= 0 && int(r.map()) == until_map) { res.reached = true; break; }
        if (until_map < 0 && r.map() != m0) {
            std::printf("    (map changed %u -> %u near %d,%d,%d)\n", m0, r.map(), lx, ly, r.av().z);
            break;
        }
        int fx, fy, fz;
        r.foot(fx, fy, fz);
        if (until_map < 0 && std::hypot(fx - tx, fy - ty) < radius && std::abs(fz - tz) <= 24) { res.reached = true; break; }
        if (each && each()) { res.reached = true; break; }
        if (!std::getenv("PG_ROUTE_NOLINKS")) rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
        rg.set_target(r.map(), tx, ty, tz);
        auto t0 = std::chrono::steady_clock::now();
        rg.update(2.0);
        res.max_slice = std::max(res.max_slice, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        const Route& rt = rg.route();
        in.pad_jump = false;
        if (trace && f % 300 == 0 && (r.w.stasis || r.w.ask_menu || !rt.usable() || rt.pts.size() < 2))
            std::printf("      f %d at %d,%d,%d waiting: stasis %d menu %d route status %d (%s) %zu points, plans %d target %d\n", f, fx, fy, fz,
                        r.w.stasis, r.w.ask_menu != nullptr, int(rt.status), rt.reason.c_str(), rt.pts.size(), rg.stats.plans,
                        rg.stats.replans_target);
        if (r.w.stasis || r.w.ask_menu || !rt.usable() || rt.pts.size() < 2) {
            in.pad_move = false;
            r.step();
            continue;
        }
        if (rt.serial != serial) { serial = rt.serial; idx = 1; ++res.replans; }
        RouteGuide::Where wh = rg.where(fx, fy, fz);
        idx = std::max(idx, wh.seg + 1);
        while (idx + 1 < rt.pts.size() && std::hypot(rt.pts[idx].x - fx, rt.pts[idx].y - fy) < 20 &&
               std::abs(rt.pts[idx].z - fz) <= 12)
            ++idx;
        const RoutePoint& p = rt.pts[std::min(idx, rt.pts.size() - 1)];
        if (trace && f % 150 == 0)
            std::printf("      f %d at %d,%d,%d -> point %zu/%zu %d,%d,%d flags %d (route %u, off %d, left %d)\n", f, fx, fy, fz, idx,
                        rt.pts.size(), p.x, p.y, p.z, p.flags, rt.serial, wh.off, wh.left);
        if (trace && f % 150 == 0)
            for (int cy = std::max(0, fy / 512 - 1); cy <= std::min(63, fy / 512 + 1); ++cy)
                for (int cx = std::max(0, fx / 512 - 1); cx <= std::min(63, fx / 512 + 1); ++cx)
                    for (uint16_t id : r.w.map().chunk(cx, cy)) {
                        const Item* dd = r.w.item(id);
                        if (!dd || !r.w.is_door_shape(dd->shape) || std::hypot(dd->x - fx, dd->y - fy) > 200) continue;
                        std::printf("        door %u shape %u frame %u q %u at %d,%d; avatar anim %u dir %u pad %d dir %d jw %d used %d\n", id,
                                    dd->shape, dd->frame, dd->quality, dd->x, dd->y, r.av().last_anim, r.av().dir, in.pad_move,
                                    in.pad_dir, jump_wait, door_used[id]);
                    }
        if (trace && f == 900)
            for (size_t k = 0; k < std::min<size_t>(rt.pts.size(), 12); ++k)
                std::printf("        %d,%d,%d flags %d\n", rt.pts[k].x, rt.pts[k].y, rt.pts[k].z, rt.pts[k].flags);
        // Steer at a point 48 units on along the route (not past a climb's foot).
        int ax = p.x, ay = p.y;
        {
            double left = 48;
            double cx = wh.px, cy = wh.py;
            for (size_t k = wh.seg + 1; k < rt.pts.size(); ++k) {
                const RoutePoint& q = rt.pts[k];
                if (q.flags & RoutePoint::kTeleport) {  // into the teleporter's range (on past its egg)
                    const RoutePoint& e = rt.pts[k - 1];
                    if (std::hypot(e.x - cx, e.y - cy) < 8 && k >= 2) {
                        ax = e.x + (e.x - rt.pts[k - 2].x);
                        ay = e.y + (e.y - rt.pts[k - 2].y);
                    }
                    break;
                }
                if (q.flags & RoutePoint::kClimb) { if (k == idx) { ax = q.x; ay = q.y; } break; }
                double d = std::hypot(q.x - cx, q.y - cy);
                if (d >= left || std::abs(q.z - fz) > 24) {
                    double t = d > 0 ? std::min(1.0, left / d) : 0;
                    ax = int(cx + (q.x - cx) * t);
                    ay = int(cy + (q.y - cy) * t);
                    break;
                }
                left -= d;
                cx = q.x; cy = q.y;
                ax = q.x; ay = q.y;
            }
        }
        int dir = direction_to(ax - fx, ay - fy);
        // A closed door near the way: use it.
        if (p.flags & RoutePoint::kDoor) {
            for (int cy = std::max(0, fy / 512 - 1); cy <= std::min(63, fy / 512 + 1); ++cy)
                for (int cx = std::max(0, fx / 512 - 1); cx <= std::min(63, fx / 512 + 1); ++cx)
                    for (uint16_t id : r.w.map().chunk(cx, cy)) {
                        const Item* it = r.w.item(id);
                        if (!it || !r.w.is_door_shape(it->shape) || r.w.door_is_open(*it)) continue;
                        if (std::hypot(it->x - 32 - fx, it->y - 32 - fy) > 112) continue;
                        if (door_used[id] && r.frames - door_used[id] < 90) continue;  // it is opening
                        door_used[id] = r.frames;
                        r.w.use_item(id);
                        ++res.doors;
                        cy = 99; cx = 99;
                        break;
                    }
        }
        bool climb = (p.flags & RoutePoint::kClimb) && p.z - fz > 12 && std::hypot(p.x - fx, p.y - fy) < 80;
        int cdir = dir;
        if (climb && idx > 0) cdir = direction_to(p.x - rt.pts[idx - 1].x, p.y - rt.pts[idx - 1].y);
        if (unstick > 0 && !climb) cdir = r.av().dir;
        if (jump_wait > 0) {
            --jump_wait;
            in.pad_move = false;
        } else if (climb || unstick > 0) {
            // Facing the ledge (a push toward it turns him), then the jump button while standing.
            if (r.av().dir != cdir) {
                in.pad_move = true;
                in.pad_dir = cdir;
                in.pad_len = 1;
            } else {
                in.pad_move = false;
                in.pad_dir = cdir;
                in.pad_jump = true;
                in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
                jump_wait = 40;
                ++res.climbs;
                if (unstick > 0) --unstick;
            }
        } else {
            in.pad_move = true;
            in.pad_dir = dir;
            in.pad_len = 1;
        }
        // Stuck: not 6 units from where he was for 45 frames.
        if (std::abs(r.av().x - lx) + std::abs(r.av().y - ly) < 6 && !r.av().gravity_pid) ++stuck;
        else { stuck = 0; lx = r.av().x; ly = r.av().y; }
        if (stuck > 45) { stuck = 0; unstick = 1; }
        r.step();
    }
    in.pad_move = false;
    in.pad_jump = false;
    return res;
}

// One leg: the guide's target for `step` from where the Avatar is, planned (sliced, timed),
// then walked. until_map >= 0: the leg ends when that map is reached (a teleporter / seam).
// npc: the target follows that NPC (the guide is asked again every second).
bool test_leg(Run& r, const char* name, const std::string& step, int max_frames, int until_map = -1, int radius = 48,
              const std::function<bool()>& done = {}) {
    CritGuide g = r.guide(step);
    std::printf("  %s: step %s; %s; target %d,%d,%d on map %u (link %d)\n", name, step.c_str(), g.line2.c_str(), g.x, g.y,
                g.z, r.map(), g.link);
    CHECK(g.have_point);
    if (!g.have_point) return false;
    RouteGuide rg(r.w);
    rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
    rg.set_target(r.map(), g.x, g.y, g.z);
    auto t0 = std::chrono::steady_clock::now();
    const Route& rt = rg.plan_now(2.0);
    double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    print_route(rt);
    std::printf("    plan: %.1f ms in %d slices of <= 2 ms (longest %.2f ms), %d coarse nodes, pathfinder %d nodes%s; via \"%s\"\n",
                wall, rg.stats.last_plan_frames, rg.stats.slice_ms_max, rg.stats.last_expanded, rg.stats.last_refine_nodes,
                rg.stats.last_refined ? " (used)" : "", rg.via(0).c_str());
    CHECK(rt.usable());
    CHECK(rg.stats.slice_ms_max < 3.0);
    if (std::getenv("PG_ROUTE_DUMP"))
        for (const RoutePoint& p : rt.pts) std::printf("      %d,%d,%d flags %d\n", p.x, p.y, p.z, p.flags);
    if (!rt.usable()) return false;
    int frame0 = r.frames;
    int gx = g.x, gy = g.y, gz = g.z;
    auto follow = [&]() {
        if ((r.frames - frame0) % 30 == 0) {
            CritGuide g2 = r.guide(step);
            if (g2.have_point) { gx = g2.x; gy = g2.y; gz = g2.z; }
        }
        return done && done();
    };
    rg.slice_nodes = 800;  // the replay is deterministic (the timings above are the real budget's)
    Walk wk = walk_route(r, rg, gx, gy, gz, max_frames, until_map, radius, follow);
    int fx, fy, fz;
    r.foot(fx, fy, fz);
    std::printf("    walked: %s in %d frames (%d jumps, %d doors used, %d routes, %d re-plans: %d strayed, %d timer), now map %u at %d,%d,%d; longest update %.2f ms\n",
                wk.reached ? "reached" : "NOT reached", wk.frames, wk.climbs, wk.doors, wk.replans, rg.stats.plans,
                rg.stats.replans_stray, rg.stats.replans_timer, r.map(), fx, fy, fz, wk.max_slice);
    CHECK(wk.reached);
    return wk.reached;
}

void settle(Run& r, int max) {
    for (int i = 0; i < max && (r.w.stasis || r.w.ask_menu); ++i) r.step();
}

void test_legs(const GameData& d, bool stitched) {
    std::printf("%s world\n", stitched ? "Stitched" : "Original");
    Run r(d, stitched);
    r.av().set(Actor::kInvincible, true);  // the monsters on the way are not the point here
    r.warp(14527, 5887, 8, 3);
    // The execution scene may start on the way; the crowd then stands round the end of the dock.
    test_leg(r, "camp -> dock", "execution", 12000, -1, 48, [&r] { return r.w.vm().get_global(89, 1) == 1; });
    // The execution plays at the dock's end (the gate opens after it).
    r.warp(11732, 5844, 48, 3);
    bool started = false;
    for (int f = 0; f < 9000; ++f) {
        r.step();
        if (r.w.stasis) started = true;
        if (started && !r.w.stasis && r.w.vm().get_global(89, 1)) break;
    }
    settle(r, 3000);
    CHECK_EQ(r.w.vm().get_global(89, 1), 1);
    test_leg(r, "dock -> gate -> Central Tenebrae", "enter_tenebrae", 6000, 40);
    settle(r, 3000);
    r.run(30);
    if (std::getenv("PG_ROUTE_PROBE")) {
        const int px = 13087, py = 8607;
        for (int cy = px / 512 - 1; cy <= py / 512 + 1; ++cy) (void)cy;
        for (int cy = py / 512 - 1; cy <= py / 512 + 1; ++cy)
            for (int cx = px / 512 - 1; cx <= px / 512 + 1; ++cx)
                for (uint16_t id : r.w.map().chunk(cx, cy)) {
                    const Item* it = r.w.item(id);
                    if (!it) continue;
                    Box b = r.w.box_of(*it);
                    if (b.x < px - 160 || b.x - b.xd > px + 160 || b.y < py - 160 || b.y - b.yd > py + 160) continue;
                    std::printf("      item %u shape %u frame %u box x %d..%d y %d..%d z %d..%d flags %04x%s\n", id, it->shape,
                                it->frame, b.x - b.xd, b.x, b.y - b.yd, b.y, b.z, b.z + b.zd,
                                r.w.data().typeflags.get(it->shape).flags(), it->as_actor() ? " actor" : "");
                }
    }
    test_leg(r, "to the dragon-statue waypoint (raised palace grounds)", "central_waypoint", 9000, -1, 48,
             [&r] { return r.w.vm().get_global(68, 1) == 1; });  // (stepping on it is the step)
    r.run(30);
    settle(r, 2000);
    std::printf("    waypoint flag G[68:1] = %d\n", r.w.vm().get_global(68, 1));
    CHECK_EQ(r.w.vm().get_global(68, 1), 1);
    // A door opened: the chunk it is in is rebuilt by the next plan (its items changed).
    if (!stitched) {
        // The doors of the house west of the palace stairs (DOOR_NS 13247..13279 x 10303..10431).
        r.warp(13230, 10400, 48, 40);
        uint16_t door = 0;
        for (int cy = 10400 / 512 - 1; cy <= 10400 / 512 + 1; ++cy)
            for (int cx = 13260 / 512 - 1; cx <= 13260 / 512 + 1; ++cx)
                for (uint16_t id : r.w.map().chunk(cx, cy))
                    if (const Item* it = r.w.item(id); it && r.w.is_door_shape(it->shape) && std::abs(it->x - 13279) < 8 && std::abs(it->y - 10431) < 8)
                        door = id;
        CHECK(door != 0);
        if (door) std::printf("  door %u shape %u frame %u q %u at %d,%d\n", door, r.w.item(door)->shape, r.w.item(door)->frame, r.w.item(door)->quality, r.w.item(door)->x, r.w.item(door)->y);
        bool was_open = door && r.w.door_is_open(*r.w.item(door));
        RouteGuide rg(r.w);
        rg.set_target(r.map(), 13400, 10368, 48);
        const Route& a = rg.plan_now(2.0);
        bool through = false;
        for (const RoutePoint& p : a.pts) through |= (p.flags & RoutePoint::kDoor) != 0;
        int rebuilt = rg.stats.chunk_rebuilds;
        std::string via = rg.via(0);
        if (door) r.w.use_item(door);
        r.run(60);
        bool now_open = door && r.w.door_is_open(*r.w.item(door));
        const Route& b = rg.plan_now(2.0);
        std::printf("  door: route %s the closed door (%s, via \"%s\"), the door %s -> %s, chunks rebuilt %d -> %d, route again %s (%d long)\n",
                    through ? "through" : "not through", a.usable() ? "found" : "none", via.c_str(), was_open ? "open" : "closed",
                    now_open ? "open" : "closed", rebuilt, rg.stats.chunk_rebuilds, b.usable() ? "found" : "none", b.length);
        CHECK(through);
        CHECK(via.find("door") != std::string::npos);
        CHECK(!was_open && now_open);
        CHECK(rg.stats.chunk_rebuilds > rebuilt);
        CHECK(b.usable());
    }
    // East Tenebrae and Bentic: over the 40/41 seam (a teleporter in Original mode).
    r.warp(14900, 14070, 48, 40);
    test_leg(r, "to East Tenebrae (40/41)", "ask_bentic", 3000, 41);
    for (int i = 0; i < 30; ++i) r.step();
    test_leg(r, "to Bentic", "ask_bentic", 6000, -1, 128);  // beside him (at night he is in bed)
}

// ---------------------------------------------------------------------------------------------
// The guide in the game mode: the route line drawn, the hint, the option, no stalls.

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, bool stitched) : gm(d) {
        gm.world().skip_opening = true;
        gm.world().ask_name = false;
        gm.world().auto_ui = true;
        if (stitched) gm.world().set_world_mode(true, g_data + "/world_layout.json");
        gm.debug().no_autosave_run = true;
        gm.debug().load_crit({g_data + "/critpath.json", g_data + "/map_links.json"});
        gm.start();
        frames(40);
        DebugTools& dt = gm.debug();
        dt.force(true);
        dt.show_crit = true;
        gm.world().avatar()->set(Actor::kInvincible, true);
    }
    World& w() { return gm.world(); }
    DebugTools& dt() { return gm.debug(); }
    double tick_ms_max = 0, route_ms_max = 0;
    void frames(int n, bool draw = true) {
        for (int i = 0; i < n; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            gm.tick();
            tick_ms_max = std::max(tick_ms_max, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            if (draw) gm.render(surf);
        }
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        InputEvent e;
        e.type = InputEvent::kMouseMove;
        e.x = 316; e.y = 196;
        gm.handle(e);
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 2);
        std::printf("  shot %s\n", path.c_str());
    }
    void warp(int x, int y, int z, unsigned m) {
        World& ww = w();
        ww.kernel().kill(1, kProcAny);
        ww.avatar()->set(Actor::kAnimLock, false);
        if (ww.camera_followed() != 1) ww.camera_follow(1);
        ww.teleport_actor(1, x, y, z, m);
        ww.stasis = false;
        frames(10);
    }
    // Runs until the route for the current target is there (the planning is sliced).
    bool wait_route(int max = 600) {
        frames(12);  // the tracker refreshes the guide three times a second
        for (int i = 0; i < max; ++i) {
            frames(1);
            if (dt().route->current() && !dt().route->busy()) return true;
        }
        return false;
    }
    std::string hint() {
        return dt().route_hint();
    }
};

void test_game_mode(const GameData& d) {
    std::printf("the route line in the game mode\n");
    Ui u(d, true);
    DebugTools& dt = u.dt();
    RouteGuide& rg = *dt.route;
    CHECK(dt.route_line());
    // Camp -> dock: the route leaves the screen; the arrow follows the route.
    dt.crit.jump_to = dt.crit.step_index("execution");
    u.warp(14527, 5887, 8, 3);
    CHECK(u.wait_route());
    std::printf("  camp: %s / %s / %s (route %zu points, %d long)\n", dt.guide.line1.c_str(), dt.guide.line2.c_str(), u.hint().c_str(),
                rg.route().pts.size(), rg.route().length);
    CHECK(rg.route().usable());
    CHECK(u.hint().find("Walk ") == 0);
    u.frames(4);
    u.shot("critpath_route_camp");
    // Near the dock's end: the route ends on screen (the diamond).
    u.warp(11900, 5800, 48, 3);
    CHECK(u.wait_route());
    u.frames(4);
    u.shot("critpath_route_dock");
    // Straight-line mode: the old line.
    dt.toggle("crit_route");
    CHECK(!dt.route_line());
    u.frames(3);
    CHECK(u.hint().empty());
    u.shot("critpath_route_straight");
    dt.toggle("crit_route");
    CHECK(dt.route_line());
    // Central Tenebrae: the dragon-statue waypoint up on the palace grounds (stairs, chevrons).
    dt.crit.jump_to = dt.crit.step_index("central_waypoint");
    u.warp(13904, 10320, 48, 40);
    CHECK(u.wait_route());
    std::printf("  palace (from %d,%d,%d): %s / %s\n", u.w().avatar()->x, u.w().avatar()->y, u.w().avatar()->z, dt.guide.line2.c_str(),
                u.hint().c_str());
    CHECK(rg.route().usable());
    CHECK(u.hint().find("up the stairs") != std::string::npos);
    u.frames(4);
    u.shot("critpath_route_stairs");
    // Re-plans: straying from the route, and every few seconds.
    {
        int strays = rg.stats.replans_stray;
        uint32_t serial = rg.route().serial;
        u.warp(13300, 10352, 48, 40);  // somewhere else on the floor below
        CHECK(u.wait_route());
        CHECK(rg.stats.replans_stray > strays || rg.stats.replans_target > 0);
        CHECK(rg.route().serial != serial);
        int timers = rg.stats.replans_timer;
        u.frames(RouteGuide::kReplanFrames + 30);
        CHECK(rg.stats.replans_timer > timers);
        std::printf("  re-plans: %d strayed, %d timer, %d target; chunk rebuilds %d\n", rg.stats.replans_stray, rg.stats.replans_timer,
                    rg.stats.replans_target, rg.stats.chunk_rebuilds);
    }
    // East Tenebrae over the seam: the hint names the walk on.
    dt.crit.jump_to = dt.crit.step_index("ask_bentic");
    u.warp(14900, 14070, 48, 40);
    CHECK(u.wait_route());
    std::printf("  to the seam: %s / %s\n", dt.guide.line2.c_str(), u.hint().c_str());
    CHECK(u.hint().find("East Tenebrae") != std::string::npos);
    u.frames(4);
    u.shot("critpath_route_seam");
    // No route: a target nobody can walk to (out in the void) falls back to the straight line.
    {
        int st = dt.crit.step_index("ask_bentic");
        CritTarget keep = dt.crit.steps[size_t(st)].target;
        CritTarget t;
        t.type = CritTarget::kPoint;
        t.map = 40;
        t.x = 14700; t.y = 13300; t.z = 48;
        dt.crit.steps[size_t(st)].target = t;
        dt.crit.steps[size_t(st)].alt_targets.clear();
        u.warp(14900, 14070, 48, 40);
        // A spot with no ground: find one near (the first cell without levels).
        bool found = false;
        for (int yy = 13000; yy > 9000 && !found; yy -= 64)
            for (int xx = 14000; xx < 17000 && !found; xx += 64)
                if ([&] {
                        for (int oy = -128; oy <= 128; oy += 32)
                            for (int ox = -128; ox <= 128; ox += 32)
                                if (!rg.levels_at(xx + ox, yy + oy).empty()) return false;
                        return true;
                    }()) {
                    dt.crit.steps[size_t(st)].target.x = xx;
                    dt.crit.steps[size_t(st)].target.y = yy;
                    found = true;
                }
        CHECK(found);
        CHECK(u.wait_route(2000));
        std::printf("  unreachable: %s (%d nodes)\n", u.hint().c_str(), rg.stats.last_expanded);
        CHECK(rg.route().status == Route::kFailed);
        CHECK(u.hint().find("No walking route found") == 0);
        u.frames(4);
        u.shot("critpath_route_none");
        dt.crit.steps[size_t(st)].target = keep;
    }
    // Drawing: the route line against the straight line (the same view, 200 frames each).
    {
        dt.crit.jump_to = dt.crit.step_index("central_waypoint");
        u.warp(13904, 10320, 48, 40);
        u.wait_route();
        auto draw_ms = [&]() {
            double worst = 0, sum = 0;
            for (int i = 0; i < 200; ++i) {
                auto t0 = std::chrono::steady_clock::now();
                u.gm.render(u.surf);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                worst = std::max(worst, ms);
                sum += ms;
            }
            return std::pair<double, double>{sum / 200, worst};
        };
        auto with = draw_ms();
        dt.set_route_line(false);
        auto without = draw_ms();
        dt.set_route_line(true);
        std::printf("  render: route line %.3f ms (worst %.3f), straight line %.3f ms (worst %.3f)\n", with.first, with.second,
                    without.first, without.second);
        CHECK(with.first < without.first + 0.5);
    }
    std::printf("  game mode: longest tick %.2f ms, longest route slice %.2f ms (budget %.1f ms)\n", u.tick_ms_max,
                rg.stats.slice_ms_max, 2.0);
    CHECK(rg.stats.slice_ms_max < 3.0);
    dt.crit.jump_to = -1;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Every step of the critical path, from where the step before leaves the Avatar: the guide's
// target (or the link toward it) is planned, the link is taken (the Avatar put at its arrival
// point), and so on until the target's map is reached and the route to the target is planned.
// The globals of the steps before are set where their done_when says how (so links with
// conditions open as in play). Prints each step's legs and the teleporters inside maps used.

// Sets globals so that `c` holds where that is a matter of globals (best effort).
void make_true(World& w, const CritCond& c) {
    switch (c.kind) {
    case CritCond::kAll: for (const CritCond& s : c.sub) make_true(w, s); break;
    case CritCond::kAny:
        for (const CritCond& s : c.sub)
            if (s.kind == CritCond::kGlobal || s.kind == CritCond::kHasItem || s.kind == CritCond::kAll) { make_true(w, s); return; }
        break;
    case CritCond::kGlobal: {
        int v = c.cmp == CritCond::kNe ? (c.value ? 0 : 1) : c.cmp == CritCond::kNonZero ? 1 : c.value;
        w.vm().set_global(unsigned(c.a), unsigned(std::max(1, c.b)), uint16_t(v));
        break;
    }
    case CritCond::kHasItem:
        // Given to the Avatar (story items open links: the Ethereal Travel book, the symbols).
        for (int k = 0; k < std::max(1, c.qty); ++k) {
            uint16_t id = w.create_item(unsigned(c.a), unsigned(std::max(0, c.frame)));
            if (!id) break;
            if (Item* it = w.item(id); it && c.quality >= 0) it->quality = uint16_t(c.quality);
            w.move_to_container(id, 1);
        }
        break;
    default: break;
    }
}

struct SweepResult { std::string id; bool ok = false, alt = false; int legs = 0, jumps = 0; std::string text; };

// Puts the Avatar at (x, y, z) of map m, on the ground below if that is in the air (an arrival
// point above a pit: in play he falls).
void warp_down(Run& r, int x, int y, int z, unsigned m) {
    r.warp(x, y, z, m);
    int fx, fy, fz;
    r.foot(fx, fy, fz);
    RouteGuide probe(r.w);
    int best = -1;
    for (int lz : probe.levels_at(fx, fy))
        if (lz <= fz + 8) best = lz;
    if (best >= 0 && fz - best > 8) r.warp(x, y, best, m);
}

std::vector<SweepResult> sweep(const GameData& d, bool verbose) {
    std::vector<SweepResult> out;
    Run r(d, false);
    r.av().set(Actor::kInvincible, true);
    const CritPath& cp = r.cp;
    unsigned m = 3;
    int x = 14527, y = 5887, z = 8;  // the start at Devon's camp
    for (size_t si = 0; si < cp.steps.size(); ++si) {
        const CritStep& st = cp.steps[si];
        SweepResult res;
        res.id = st.id;
        warp_down(r, x, y, z, m);
        r.ctx.observe();
        char buf[256];
        std::snprintf(buf, sizeof buf, "from map %u (%d,%d,%d)", r.map(), r.av().x, r.av().y, r.av().z);
        res.text = buf;
        bool reached_map = false;
        // The guide for this step alone (the globals set below may complete later steps too).
        CritPath one;
        one.links = cp.links;
        one.steps = {st};
        one.steps[0].done_when = CritCond{};
        auto guide = [&] { r.ctx.observe(); return one.guide(r.ctx); };
        for (int hop = 0; hop < 8; ++hop) {
            CritGuide g = guide();
            if (!g.have_point) {
                if (g.link >= 0 && !cp.links[size_t(g.link)].has_point) {  // an item or a spell
                    const MapLink& l = cp.links[size_t(g.link)];
                    res.text += " | use " + l.kind + " -> map " + std::to_string(l.to_map);
                    warp_down(r, l.to_x, l.to_y, l.to_z, l.to_map);
                    r.ctx.observe();
                    continue;
                }
                res.text += g.unreachable ? " | no known way to map " + std::to_string(g.target_map) : " | no target point";
                res.ok = !g.unreachable && g.target_map == 0;  // (a step without a place)
                break;
            }
            RouteGuide rg(r.w);
            if (!std::getenv("PG_ROUTE_NOLINKS")) rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
            rg.set_target(r.map(), g.x, g.y, g.z);
            const Route& rt = rg.plan_now(4.0);
            ++res.legs;
            res.jumps += rt.jumps;
            std::snprintf(buf, sizeof buf, " | map %u to %d,%d,%d: %s, %d long, %d teleporters%s%s", r.map(), g.x, g.y, g.z,
                          rt.status == Route::kFound ? "found" : rt.status == Route::kNear ? "near" : "FAILED", rt.length, rt.jumps,
                          rt.reason.empty() ? "" : ", ", rt.reason.c_str());
            res.text += buf;
            if (!rt.usable()) break;
            if (g.link < 0) { reached_map = true; break; }
            const MapLink& l = cp.links[size_t(g.link)];
            warp_down(r, l.to_x, l.to_y, l.to_z, l.to_map);
            r.ctx.observe();
        }
        if (reached_map) res.ok = true;
        // Not from there: from the places one arrives at on the target's map (the usual ways in).
        if (!res.ok && !std::getenv("PG_ROUTE_NOALT")) {
            const CritTarget* t = &st.target;
            for (const auto& alt : st.alt_targets)
                if (alt.first.eval(r.ctx)) { t = &alt.second; break; }
            unsigned tm = 0;
            int tx = 0, ty = 0, tz = 0;
            bool have = false;
            if (t->type == CritTarget::kNpc) have = r.ctx.npc_location(unsigned(t->npc), tm, tx, ty, tz);
            else if ((t->type == CritTarget::kPoint || t->type == CritTarget::kItem) && t->map >= 0) {
                tm = unsigned(t->map); tx = t->x; ty = t->y; tz = t->z;
                have = true;
            }
            int tried = 0;
            std::set<long long> seen;
            // Ways in from other maps first, then the teleporters inside the map, farthest first.
            std::vector<const MapLink*> ins;
            for (const MapLink& l : cp.links)
                if (have && l.to_map == tm && !l.dead) ins.push_back(&l);
            auto far = [&](const MapLink* l) { return std::hypot(double(l->to_x - tx), double(l->to_y - ty)); };
            std::stable_sort(ins.begin(), ins.end(), [&](const MapLink* a, const MapLink* b) {
                bool sa = !a->from_any && a->from_map == tm, sb = !b->from_any && b->from_map == tm;
                if (sa != sb) return !sa;
                return far(a) > far(b);
            });
            for (const MapLink* lp : ins) {
                const MapLink& l = *lp;
                if (!seen.insert((long long)l.to_x * 100000 + l.to_y).second) continue;
                ++tried;
                warp_down(r, l.to_x, l.to_y, l.to_z, l.to_map);
                r.ctx.observe();
                RouteGuide rg(r.w);
                if (!std::getenv("PG_ROUTE_NOLINKS")) rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
                rg.set_target(r.map(), tx, ty, tz);
                const Route& rt = rg.plan_now(4.0);
                if (rt.usable()) {
                    char b2[200];
                    std::snprintf(b2, sizeof b2, " || from the arrival %d,%d,%d (%s from map %d): %s, %d long, %d teleporters", l.to_x, l.to_y,
                                  l.to_z, l.kind.c_str(), l.from_any ? -1 : int(l.from_map), rt.status == Route::kFound ? "found" : "near",
                                  rt.length, rt.jumps);
                    res.text += b2;
                    res.jumps += rt.jumps;
                    res.ok = true;
                    res.alt = true;
                    break;
                }
            }
            if (!res.ok) res.text += " || from " + std::to_string(tried) + " arrivals on map " + std::to_string(tm) + ": none";
        }
        // The next step starts at this one's target (wherever the walk above got to).
        {
            const CritTarget* t = &st.target;
            for (const auto& alt : st.alt_targets)
                if (alt.first.eval(r.ctx)) { t = &alt.second; break; }
            unsigned tm = 0;
            int tx = 0, ty = 0, tz = 0;
            bool have = false;
            if (t->type == CritTarget::kNpc) have = r.ctx.npc_location(unsigned(t->npc), tm, tx, ty, tz);
            else if ((t->type == CritTarget::kPoint || t->type == CritTarget::kItem) && t->map >= 0) {
                tm = unsigned(t->map); tx = t->x; ty = t->y; tz = t->z;
                have = true;
            }
            if (have) { m = tm; x = tx; y = ty; z = tz; }
        }
        // The step is done: its globals set (links behind conditions open as in play).
        make_true(r.w, st.done_when);
        if (verbose) std::printf("  %-26s %s %s\n", st.id.c_str(), res.ok ? "ok  " : "FAIL", res.text.c_str());
        out.push_back(res);
    }
    return out;
}

void test_sweep(const GameData& d) {
    std::printf("every critical-path step from the step before\n");
    std::vector<SweepResult> rs = sweep(d, true);
    int ok = 0, through = 0;
    for (const SweepResult& s : rs) { ok += s.ok; through += s.jumps > 0; }
    std::printf("  %d of %zu steps reachable; %d through teleporters inside a map\n", ok, rs.size(), through);
    // Steps whose way needs teleporters inside a map (they had no walking route before).
    for (const char* id : {"third_test", "kill_malchir"}) {
        auto it = std::find_if(rs.begin(), rs.end(), [&](const SweepResult& s) { return s.id == id; });
        CHECK(it != rs.end() && it->ok && it->jumps > 0);
    }
    CHECK(ok >= 80);
}

// Mythran (NPC 33) stands in his house on the Plateau, which only teleporters lead into: the
// MYTHAUS egg at the house door (before the first meeting to the gated corridor, after it
// straight to his rooms), then the teleporter at the corridor's end. The route leads to the
// first teleporter, on from where it lands to the next, then to him; the Avatar walks it.
void test_mythran(const GameData& d) {
    std::printf("Mythran's house: through two teleporters\n");
    for (int met = 0; met <= 1; ++met) {
        Run r(d, false);
        r.av().set(Actor::kInvincible, true);
        r.w.vm().set_global(284, 1, uint16_t(met));
        r.warp(14590, 11290, 48, 8);  // where the cave from the lever room comes out on the Plateau
        CritGuide g = r.guide("meet_mythran");
        std::printf("  %s: %s; target %d,%d,%d\n", met ? "met before" : "first visit", g.line2.c_str(), g.x, g.y, g.z);
        CHECK(g.have_point && g.link < 0);
        RouteGuide rg(r.w);
        rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
        rg.set_target(r.map(), g.x, g.y, g.z);
        const Route& rt = rg.plan_now(2.0);
        print_route(rt);
        CHECK(rt.status == Route::kFound);
        CHECK_EQ(rt.jumps, met ? 1 : 2);
        CHECK(rg.stats.slice_ms_max < 3.0);
        // The first leg ends at the house door (the MYTHAUS egg, 13988,15084).
        int fx, fy, fz;
        r.foot(fx, fy, fz);
        RouteGuide::Where wh = rg.where(fx, fy, fz);
        CHECK(wh.jump);
        const RoutePoint& door = rt.pts[wh.leg_end];
        std::printf("    first leg: %d to %d,%d,%d, via \"%s\"\n", wh.left, door.x, door.y, door.z, rg.via(wh.seg).c_str());
        CHECK(std::hypot(door.x - (13988 - 32), door.y - (15084 - 32)) < 160);
        CHECK(rg.via(wh.seg).find("take the teleporter") != std::string::npos);
        // Without the teleporters there is no way.
        {
            RouteGuide none(r.w);
            none.set_target(r.map(), g.x, g.y, g.z);
            CHECK(!none.plan_now(2.0).usable() || none.route().status == Route::kNear);
        }
        // Walked: through the teleporters to him (the guide asked again as he goes).
        std::vector<std::pair<int, int>> landed;
        int lx = r.av().x, ly = r.av().y;
        int gx = g.x, gy = g.y, gz = g.z, frame0 = r.frames;
        auto each = [&] {
            if (std::abs(r.av().x - lx) + std::abs(r.av().y - ly) > 300) landed.push_back({r.av().x, r.av().y});
            lx = r.av().x; ly = r.av().y;
            if ((r.frames - frame0) % 30 == 0) {
                CritGuide g2 = r.guide("meet_mythran");
                if (g2.have_point) { gx = g2.x; gy = g2.y; gz = g2.z; }
            }
            return false;
        };
        rg.slice_nodes = 800;
        Walk wk = walk_route(r, rg, gx, gy, gz, 30 * 60 * 4, -1, 128, each);
        std::printf("    walked: %s in %d frames (%d doors, %d routes), teleported to", wk.reached ? "reached" : "NOT reached", wk.frames,
                    wk.doors, wk.replans);
        for (auto& p : landed) std::printf(" %d,%d", p.first, p.second);
        std::printf("; now %d,%d,%d\n", r.av().x, r.av().y, r.av().z);
        CHECK(wk.reached);
        CHECK_EQ(int(landed.size()), met ? 1 : 2);
    }
    // The stitched world: the same teleporters inside map 8.
    {
        Run r(d, true);
        r.warp(14590, 11290, 48, 8);
        CritGuide g = r.guide("meet_mythran");
        RouteGuide rg(r.w);
        rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
        rg.set_target(r.map(), g.x, g.y, g.z);
        const Route& rt = rg.plan_now(2.0);
        std::printf("  stitched world:");
        print_route(rt);
        CHECK(rt.status == Route::kFound && rt.jumps == 2);
    }
}

// The guide in the game mode at Mythran's: the line ends at the house door (the teleporter),
// then, from the arrival in the corridor, at the teleporter at its end, then at Mythran.
void test_mythran_ui(const GameData& d) {
    std::printf("the route line at Mythran's house\n");
    Ui u(d, false);
    DebugTools& dt = u.dt();
    dt.crit.jump_to = dt.crit.step_index("meet_mythran");
    struct Spot { int x, y, z; const char* shot; int left_max; };
    const Spot spots[] = {
        {14590, 11290, 48, "critpath_mythran_plateau", 0},     // outside: to the house door
        {14200, 15100, 48, "critpath_mythran_door", 400},      // at the door
        {6078, 1786, 0, "critpath_mythran_corridor", 0},       // after the first teleporter
        {2944, 1816, 0, "critpath_mythran_rooms", 0},          // after the second
    };
    for (const Spot& s : spots) {
        u.warp(s.x, s.y, s.z, 8);
        CHECK(u.wait_route(3000));
        const Route& rt = dt.route->route();
        Box b = u.w().box_of(*u.w().avatar());
        RouteGuide::Where wh = dt.route->where(b.x - b.xd / 2, b.y - b.yd / 2, b.z);
        std::printf("  %s: %s / %s (%d teleporters on the way, leg ends %d,%d)\n", s.shot, dt.guide.line2.c_str(), u.hint().c_str(), rt.jumps,
                    rt.pts.empty() ? 0 : rt.pts[wh.leg_end].x, rt.pts.empty() ? 0 : rt.pts[wh.leg_end].y);
        CHECK(rt.usable());
        if (s.x != 2944) CHECK(u.hint().find("take the teleporter") != std::string::npos);
        else CHECK(u.hint().find("teleporter") == std::string::npos);
        if (s.left_max) CHECK(wh.left <= s.left_max);
        u.frames(4);
        u.shot(s.shot);
    }
    dt.crit.jump_to = -1;
}

// The companions' homes (data/companions.json, NOTES.md "Companions: homes") can be walked to: the
// Avatar arrives on each home's map where the story brings him there and is driven along the walk
// route to the spot, kept alive whatever the monsters on the way do.
void test_home_walks(const GameData& d, bool stitched) {
    std::printf("walks to the companions' homes (%s)\n", stitched ? "stitched" : "original maps");
    struct From { const char* id; int x, y, z; const char* what; };
    const From from[] = {
        {"corvin", 14527, 5887, 8, "the Avatar's start at Devon's camp"},
        {"nerissa", 15464, 6912, 48, "the way in from West Tenebrae"},
        {"morvain", 5375, 29823, 8, "the way in from the East Road"},
        {"brannoc", 15071, 26239, 8, "the way in from Central Tenebrae"},
        {"wynna", 14590, 11210, 48, "the end of the lever-room caves"},
        {"ilvaran", 26776, 26912, 56, "the way in from the catacombs"},
        {"varn", 19263, 9631, 104, "the way in from the catacombs"},
        {"kalvax", 20275, 13904, 64, "the end of the lava tunnel"},
    };
    for (const From& f : from) {
        Run r(d, stitched);
        std::string err;
        CHECK(r.w.party->load_file(g_data + "/companions.json", err));
        int i = r.w.party->find_def(f.id);
        CHECK(i >= 0);
        if (i < 0) continue;
        const CompanionDef& def = r.w.party->defs[size_t(i)];
        CHECK(def.has_home);
        r.warp(f.x, f.y, f.z, def.home_map);
        RouteGuide rg(r.w);
        rg.set_target(def.home_map, def.home[0], def.home[1], def.home[2]);
        const Route& rt = rg.plan_now(2.0);
        std::printf("  %s, map %u, from %s:", f.id, def.home_map, f.what);
        print_route(rt);
        CHECK(rt.usable());
        const int tx = def.home[0], ty = def.home[1], tz = def.home[2];
        auto alive = [&] { r.av().hp = 200; return false; };
        Walk wk = walk_route(r, rg, tx, ty, tz, 30 * 60 * 6, -1, 128, alive);
        std::printf("    walked: %s after %d frames (%d climbs, %d doors), ends %d units away at z %d\n", wk.reached ? "reached" : "NOT reached",
                    wk.frames, wk.climbs, wk.doors, int(std::hypot(r.av().x - tx, r.av().y - ty)), r.av().z);
        CHECK(wk.reached);
    }
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    {
        std::string cfg = (g_shots.empty() ? std::string("/tmp") : g_shots) + "_route_test.cfg";
        Config::global().load(cfg);  // never the player's settings
        Config::global().set_bool("crit_route", true);
    }
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    if (std::getenv("PG_ROUTE_MYTHRAN")) {
        test_mythran(d);
        test_mythran_ui(d);
        std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (std::getenv("PG_ROUTE_SWEEP")) {
        test_sweep(d);
        std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (std::getenv("PG_ROUTE_HOMES")) {
        test_home_walks(d, false);
        std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (const char* from = std::getenv("PG_ROUTE_FROM")) {
        // Probe: PG_ROUTE_FROM=map,x,y,z PG_ROUTE_TO=x,y,z [PG_ROUTE_CLIMB=1]
        int m, x, y, z, tx, ty, tz;
        std::sscanf(from, "%d,%d,%d,%d", &m, &x, &y, &z);
        std::sscanf(std::getenv("PG_ROUTE_TO"), "%d,%d,%d", &tx, &ty, &tz);
        Run r(d, std::getenv("PG_ROUTE_STITCHED") != nullptr);
        if (const char* gl = std::getenv("PG_ROUTE_GLOBALS"))  // "pos:n=v;pos:n=v"
            for (const char* q = gl; *q;) {
                unsigned pos = 0, n = 0, v = 0;
                int used = 0;
                if (std::sscanf(q, "%u:%u=%u%n", &pos, &n, &v, &used) != 3) break;
                r.w.vm().set_global(pos, n, uint16_t(v));
                q += used;
                if (*q == ';') ++q;
            }
        r.warp(x, y, z, unsigned(m));
        int fx, fy, fz;
        r.foot(fx, fy, fz);
        std::printf("at %d,%d,%d (foot %d,%d,%d)\n", r.av().x, r.av().y, r.av().z, fx, fy, fz);
        if (std::getenv("PG_ROUTE_CLIMB")) {
            for (int dir = 0; dir < 8; ++dir)
                for (int k = 0; k < 8; ++k) {
                    AnimTracker t(r.w);
                    if (!t.init(1, 19 + unsigned(k), dir)) continue;
                    int res = t.run_all(true);
                    if (res == AnimTracker::kOk) std::printf("  climb %d dir %d ok -> %d,%d,%d\n", 16 + 8 * k, dir, t.x(), t.y(), t.z());
                }
        }
        if (const char* du = std::getenv("PG_ROUTE_USE")) {
            uint16_t id = uint16_t(std::atoi(du));
            const Item* it = r.w.item(id);
            std::printf("door %u before: frame %u q %u\n", id, it ? it->frame : 0, it ? it->quality : 0);
            r.w.use_item(id);
            r.run(90);
            it = r.w.item(id);
            std::printf("door %u after: frame %u q %u barks %zu\n", id, it ? it->frame : 0, it ? it->quality : 0, r.w.barks.size());
            for (const Bark& b : r.w.barks) for (const auto& pg : b.pages) for (const auto& l : pg.lines) std::printf("  bark: %s\n", l.c_str());
            if (const char* pd = std::getenv("PG_ROUTE_PUSH")) {
                r.w.input.pad_move = true;
                r.w.input.pad_dir = std::atoi(pd);
                r.w.input.pad_len = 1;
                for (int i = 0; i < 60; ++i) {
                    r.step();
                    if (i % 10 == 0) std::printf("  push: %d,%d,%d anim %u dir %u\n", r.av().x, r.av().y, r.av().z, r.av().last_anim, r.av().dir);
                }
                r.w.input.pad_move = false;
            }
        }
        if (std::getenv("PG_ROUTE_DOORS")) {
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx)
                    for (uint16_t id : r.w.map().chunk(cx, cy)) {
                        const Item* it = r.w.item(id);
                        if (!it || std::abs(it->x - tx) > 700 || std::abs(it->y - ty) > 700) continue;
                        std::string nm = r.w.data().usecode.class_name(it->shape);
                        if (!r.w.is_door_shape(it->shape) && nm.find("DOOR") == std::string::npos && nm.find("GATE") == std::string::npos) continue;
                        Box b = r.w.box_of(*it);
                        std::printf("      door %u shape %u (%s) frame %u q %u box x %d..%d y %d..%d z %d..%d\n", id, it->shape, nm.c_str(),
                                    it->frame, it->quality, b.x - b.xd, b.x, b.y - b.yd, b.y, b.z, b.z + b.zd);
                    }
        }
        if (std::getenv("PG_ROUTE_ITEMS")) {
            int R = std::atoi(std::getenv("PG_ROUTE_ITEMS"));
            for (int cy = ty / 512 - 1; cy <= ty / 512 + 1; ++cy)
                for (int cx = tx / 512 - 1; cx <= tx / 512 + 1; ++cx)
                    for (uint16_t id : r.w.map().chunk(cx, cy)) {
                        const Item* it = r.w.item(id);
                        if (!it) continue;
                        Box b = r.w.box_of(*it);
                        if (b.x < tx - R || b.x - b.xd > tx + R || b.y < ty - R || b.y - b.yd > ty + R) continue;
                        std::printf("      item %u shape %u frame %u box x %d..%d y %d..%d z %d..%d flags %04x%s%s\n", id,
                                    it->shape, it->frame, b.x - b.xd, b.x, b.y - b.yd, b.y, b.z, b.z + b.zd,
                                    r.w.data().typeflags.get(it->shape).flags(), it->as_actor() ? " actor" : "",
                                    (it->flags & kItemFastOnly) ? " glob" : "");
                    }
        }
        if (const char* wk = std::getenv("PG_ROUTE_WALK")) {
            // Black-box: push the stick in direction D for N frames, printing where he is.
            int dir = 0, n = 0;
            std::sscanf(wk, "%d,%d", &dir, &n);
            r.w.input.pad_move = true;
            r.w.input.pad_dir = dir;
            r.w.input.pad_len = 1;
            for (int i = 0; i < n; ++i) {
                unsigned m0 = r.map();
                int x0 = r.av().x, y0 = r.av().y;
                r.step();
                if (r.map() != m0 || std::abs(r.av().x - x0) + std::abs(r.av().y - y0) > 100)
                    std::printf("  jump at frame %d: map %u -> %u, %d,%d -> %d,%d,%d\n", i, m0, r.map(), x0, y0, r.av().x, r.av().y, r.av().z);
                if (i % 15 == 0) std::printf("  walk f %d: map %u at %d,%d,%d stasis %d\n", i, r.map(), r.av().x, r.av().y, r.av().z, r.w.stasis);
            }
            r.w.input.pad_move = false;
            r.run(60);
            std::printf("  after: map %u at %d,%d,%d\n", r.map(), r.av().x, r.av().y, r.av().z);
            return 0;
        }
        if (std::getenv("PG_ROUTE_EGGS")) {
            // The map's teleporter and usecode eggs (teleport id, trigger range, class).
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx)
                    for (uint16_t id : r.w.map().chunk(cx, cy)) {
                        const Item* it = r.w.item(id);
                        if (!it) continue;
                        unsigned fam = r.w.data().typeflags.get(it->shape).family();
                        if (fam != kFamTeleportEgg && fam != kFamUsecodeEgg) continue;
                        std::printf("      %s egg %u shape %u frame %u q %u (id %u) map %u at %d,%d,%d range %d,%d %s\n",
                                    fam == kFamTeleportEgg ? "tele" : "usecode", id, it->shape, it->frame, it->quality,
                                    it->quality & 0xFF, it->mapnum, it->x, it->y, it->z, it->npcnum >> 4, it->npcnum & 15,
                                    fam == kFamUsecodeEgg ? r.w.data().usecode.class_name(1151 + it->quality).c_str() : "");
                    }
        }
        RouteGuide rg(r.w);
        if (const char* gm = std::getenv("PG_ROUTE_GRID")) {
            int x0, y0, x1, y1;
            std::sscanf(gm, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
            for (int y = y0; y <= y1; y += 32) {
                std::printf("%6d ", y);
                for (int x = x0; x <= x1; x += 32) {
                    std::vector<int> l = rg.levels_at(x, y);
                    bool up = false, low = false, mid = false;
                    for (int z : l) { up |= z >= 90 && z <= 100; low |= z >= 40 && z <= 56; mid |= z > 56 && z < 90; }
                    char c = up && low ? '%' : up ? '#' : low ? '.' : mid ? 'm' : l.empty() ? ' ' : '^';
                    std::printf("%c", c);
                }
                std::printf("\n");
            }
        }
        if (const char* gm = std::getenv("PG_ROUTE_ZMAP")) {
            // Each cell's lowest standing height / 16 as a hex digit (space: none), step S.
            int x0, y0, x1, y1, st = 64;
            std::sscanf(gm, "%d,%d,%d,%d,%d", &x0, &y0, &x1, &y1, &st);
            for (int y = y0; y <= y1; y += st) {
                std::printf("%6d ", y);
                for (int x = x0; x <= x1; x += st) {
                    std::vector<int> l = rg.levels_at(x, y);
                    std::printf("%c", l.empty() ? ' ' : "0123456789abcdef"[std::clamp(l.back() / 16, 0, 15)]);
                }
                std::printf("\n");
            }
        }
        if (const char* gm = std::getenv("PG_ROUTE_CELLS")) {
            int x0, y0, x1, y1;
            std::sscanf(gm, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
            for (int y = y0; y <= y1; y += 32) {
                std::printf("%6d ", y);
                for (int x = x0; x <= x1; x += 32) {
                    std::string t;
                    for (int z : rg.levels_at(x, y)) t += (t.empty() ? "" : "/") + std::to_string(z);
                    std::printf("%-12s", t.c_str());
                }
                std::printf("\n");
            }
        }
        r.ctx.observe();
        rg.set_links(r.map(), r.cp.walk_links(r.ctx, r.map()));
        rg.set_target(r.map(), tx, ty, tz);
        const Route& rt = rg.plan_now(2.0);
        print_route(rt);
        std::printf("  %d nodes, closest %d,%d,%d, %d climbs tried (%d no)\n", rg.stats.last_expanded, rg.stats.closest[0],
                    rg.stats.closest[1], rg.stats.closest[2], rg.stats.climb_trials, rg.stats.climbs_rejected);
        for (const RoutePoint& p : rt.pts) std::printf("      %d,%d,%d flags %d\n", p.x, p.y, p.z, p.flags);
        if (std::getenv("PG_ROUTE_GO")) {
            // Walks the route; reports jumps (teleports on the same map) and map changes.
            int lx = r.av().x, ly = r.av().y;
            unsigned lm = r.map();
            bool jumped = false;
            auto watch = [&] {
                if (r.map() != lm || std::abs(r.av().x - lx) + std::abs(r.av().y - ly) > 200) {
                    std::printf("  jump: map %u %d,%d -> map %u %d,%d,%d\n", lm, lx, ly, r.map(), r.av().x, r.av().y, r.av().z);
                    jumped = true;
                }
                lx = r.av().x; ly = r.av().y; lm = r.map();
                return jumped;
            };
            Walk wk = walk_route(r, rg, tx, ty, tz, 3000, -1, 48, watch);
            r.run(60);
            std::printf("  walk %s, now map %u at %d,%d,%d\n", wk.reached ? "ended" : "NOT reached", r.map(), r.av().x, r.av().y, r.av().z);
        }
        return 0;
    }
    test_legs(d, false);
    test_legs(d, true);
    test_game_mode(d);
    test_mythran(d);
    test_mythran_ui(d);
    test_sweep(d);
    test_home_walks(d, false);
    test_home_walks(d, true);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
