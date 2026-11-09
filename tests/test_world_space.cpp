// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The stitched world (world/world_layout.h, world/world_space.h, game/world_stitch.cpp;
// PLAN-WORLD-EDITOR.md section 1): the layout's invariants and file round trip, seam
// crossings at every portal in both modes, walking across a seam, every kept teleport link,
// companions and a turn-based fight at a seam, saves in both modes, the neighbour streaming
// and its cost, and the critical path replayed headlessly in Stitched and Original mode with
// the story globals compared at each milestone. ctest `world`.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "app/atlas_view.h"
#include "app/game_mode.h"
#include "check.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "editor/area_model.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/savegame.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/platform.h"
#include "world/world_layout.h"
#include "world/world_space.h"

int g_fail = 0, g_pass = 0;

namespace {

using namespace pg;

std::string g_data, g_shots, g_saves;
std::string layout_path() { return g_data + "/world_layout.json"; }

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A World-only run (no game mode), the 30 fps input clock of tests/play.h.
struct Run {
    World w;
    int frames = 0;
    std::vector<std::string> notes;
    Run(const GameData& d, bool stitched, bool skip_opening) : w(d) {
        World::auto_ui_given().clear();  // every run answers the same questions in the same order
        w.auto_ui = true;
        w.ask_name = false;
        w.skip_opening = skip_opening;
        if (stitched) CHECK(w.set_world_mode(true, layout_path()));
        w.new_game();
        run(skip_opening ? 30 : 8400);  // the opening ends and stasis is released at about frame 8300
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
        w.kernel().kill(1, kProcAny);  // a fall or an animation in progress ends (the debug warp)
        w.avatar()->set(Actor::kAnimLock, false);
        if (w.camera_followed() != 1) w.camera_follow(1);  // a scene may have left the camera elsewhere
        w.teleport_actor(1, x, y, z, m);
        w.stasis = false;
        run(10);
    }
    void release() {
        w.input.button[2].down = false;
        w.input.button[2].handled = true;
        w.input.key_jump = false;
    }
    // Walks toward (x, y) like the player (right button held toward the point, jumps and side
    // steps when stuck, as tests/play.h). Stops when within 48, or when the map changes.
    bool walk_to(int x, int y, int max_frames, bool stop_on_map_change = true) {
        unsigned m0 = map();
        int stuck = 0, lx = -1, ly = -1, rel = 0, jumps = 0, side = 0, sides = 0;
        for (int f = 0; f < max_frames; ++f) {
            if (stop_on_map_change && map() != m0) { release(); return true; }
            int dx = x - av().x, dy = y - av().y;
            if (std::abs(dx) < 48 && std::abs(dy) < 48) { release(); return true; }
            PlayerInput& in = w.input;
            in.cursor_dir = direction_to(dx, dy);
            in.cursor_len = std::abs(dx) + std::abs(dy) > 400 ? 2 : 1;
            if (side > 0) {
                --side;
                in.cursor_dir = (in.cursor_dir + ((sides & 1) ? 2 : 6)) & 7;
                in.cursor_len = 1;
            }
            in.key_jump = false;
            if (rel > 0) {
                --rel;
                in.button[2].down = false;
                in.button[2].handled = true;
                if (rel == 10) in.key_jump = true;
            } else if (!in.button[2].down && !w.stasis) {
                in.button[2].press(in.now_ms, 0, 0);
            }
            if (av().x == lx && av().y == ly) ++stuck;
            else stuck = 0;
            lx = av().x;
            ly = av().y;
            if (stuck > 40 && !w.stasis) {
                stuck = 0;
                if (++jumps > 14) break;
                if (jumps % 2) rel = 20;
                else { side = 40; ++sides; }
            }
            step();
        }
        release();
        return false;
    }
    void world_point(int32_t out[3]) { w.avatar_world_point(out); }
};

// The game mode (rendering, the smooth view, companions, turn-based combat).
struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool stitched, bool companions = false, bool turn = false) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.auto_ui = true;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (companions && !w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = companions;
        w.turn->enabled = turn;
        if (stitched) CHECK(w.set_world_mode(true, layout_path()));
        gm.start();
    }
    World& w() { return gm.world(); }
    void frames(int n, bool draw = true) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            if (draw) gm.render(surf);
        }
    }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e);
        e.type = InputEvent::kKeyUp; gm.handle(e);
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 3);
        std::printf("  shot %s\n", path.c_str());
    }
    // The composed view at frame scale 4, zoomed out `step` steps (100 / 75 / 50 / 25%).
    void wide_shot(const std::string& name, int step) {
        if (g_shots.empty()) return;
        int keep = w().controls.zoom_step;
        w().controls.zoom_step = step;
        std::vector<uint32_t> px;
        int fw = 0, fh = 0;
        gm.render_frame(px, fw, fh, 4, 0.0, 1.0 / 30);  // sets the bigger view (the fast area and the streaming follow)
        frames(2, false);
        gm.render_frame(px, fw, fh, 4, 0.0, 1.0 / 30);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, px.data(), fw, fh, 1);
        std::printf("  shot %s (%dx%d)\n", path.c_str(), fw, fh);
        w().controls.zoom_step = keep;
    }
    void avatar_to(int x, int y, int z, unsigned m) {
        w().teleport_actor(1, x, y, z, m);
        w().stasis = false;
        frames(3);
    }
};

const Item* find_teleporter(World& w, unsigned to_map, int id, bool arrival) {
    for (size_t i = 512; i < w.objects().slots(); ++i) {
        const Item* e = w.item(uint16_t(i));
        if (!e || e->chunk < 0 || w.data().typeflags.get(e->shape).family() != kFamTeleportEgg) continue;
        if (arrival ? e->frame != 1 : e->frame == 1) continue;
        if (!arrival && e->mapnum != to_map) continue;
        if (id >= 0 && int(e->quality & 0xFF) != id) continue;
        return e;
    }
    return nullptr;
}

// --- 1. The layout -----------------------------------------------------------------------

void test_layout(const GameData& d) {
    std::printf("the stitched layout\n");
    WorldLayoutFile f;
    std::string err;
    CHECK(f.load(layout_path(), err));
    std::vector<MapFootprint> fps = compute_footprints(d);
    std::vector<std::string> problems = check_world_layout(f, fps);
    for (const std::string& p : problems) std::printf("  problem: %s\n", p.c_str());
    CHECK(problems.empty());
    std::printf("  %zu groups, %zu seams, %zu links, %zu variants\n", f.atlas.groups.size(), f.seams.size(), f.links.size(),
                f.variants.size());
    // The seams the atlas's votes confirm (NOTES.md "Zoom and the world atlas").
    // Plus the two declared seams (teleporters at the maps' edges whose offsets agree both ways):
    // Herdsman's Valley north of the gate, the Cemetery at the end of the East Road.
    std::set<std::pair<unsigned, unsigned>> want = {{3, 40}, {4, 39}, {5, 6}, {5, 41}, {7, 40}, {39, 40}, {40, 41}, {51, 57}};
    std::set<std::pair<unsigned, unsigned>> have;
    for (const WorldSeam& s : f.seams) {
        have.insert({s.a, s.b});
        CHECK(s.basis == "teleport edge" || s.votes >= 40);
        CHECK(!s.portals.empty());
        CHECK(!s.derived.empty());
        CHECK(s.strip[2] > s.strip[0] && s.strip[3] > s.strip[1]);
        int ga = f.atlas.group_of(s.a), gb = f.atlas.group_of(s.b);
        CHECK(ga >= 0 && gb >= 0);
        if (ga >= 0 && gb >= 0) {
            CHECK_EQ(f.atlas.groups[size_t(gb)].dx - f.atlas.groups[size_t(ga)].dx, s.dx);
            CHECK_EQ(f.atlas.groups[size_t(gb)].layer, f.atlas.groups[size_t(ga)].layer);
        }
    }
    CHECK(have == want);
    // Layers as map_index.md suggests: the Tenebrae cluster on the surface, the catacombs and
    // caves below it, the planes and Daemon's Crag above.
    auto layer = [&](unsigned m) { int g = f.atlas.group_of(m); return g >= 0 ? f.atlas.groups[size_t(g)].layer : 99; };
    // Revision 2: the whole island's outdoors on the surface (the fan map), underground below, the planes above.
    for (unsigned m : {3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u, 21u, 37u, 39u, 40u, 41u}) CHECK_EQ(layer(m), 0);
    for (unsigned m : {49u, 50u, 51u, 57u, 62u, 31u, 24u, 15u, 14u, 28u, 54u, 63u}) CHECK(layer(m) < 0);
    for (unsigned m : {43u, 44u, 45u, 46u, 47u}) CHECK(layer(m) > 0);
    CHECK_EQ(f.layout_revision, kLayoutRevision);
    // The maps nobody can reach are parked, each its own group, marked unplaced with a note.
    for (unsigned m : {20u, 29u, 33u, 56u, 223u, 255u}) {
        int g = f.atlas.group_of(m);
        CHECK(g >= 0 && f.atlas.groups[size_t(g)].extra.find("\"unplaced\":true") != std::string::npos);
    }
    CHECK_EQ(f.atlas.group_of(49), f.atlas.group_of(50));
    CHECK_EQ(f.atlas.group_of(16), f.atlas.group_of(26));
    CHECK_EQ(f.atlas.group_of(24), f.atlas.group_of(25));
    CHECK_EQ(f.variants.size(), 2);
    for (const AtlasGroup& g : f.atlas.groups) {
        CHECK(std::abs(g.dz - g.layer * kLayerZ) <= kMaxHeightTrim);
        CHECK(!g.derived.empty());
    }
    // Every link is a kept teleport between two placed maps; no seam pair among the egg links.
    int eggs = 0;
    for (const WorldLink& l : f.links) {
        CHECK(l.mode == "teleport");
        if (l.kind == "egg") {
            ++eggs;
            CHECK(!f.seam(l.from_map, l.to_map));
        }
    }
    std::printf("  %d kept teleporter links between maps, %zu usecode links\n", eggs, f.links.size() - size_t(eggs));
    CHECK(eggs > 20);
    // Regenerating gives the same layout (deterministic).
    WorldLayoutFile g = generate_world_layout(d, read_file(g_data + "/map_links.json"));
    CHECK_EQ(g.atlas.groups.size(), f.atlas.groups.size());
    bool same = g.atlas.groups.size() == f.atlas.groups.size();
    for (size_t i = 0; same && i < g.atlas.groups.size(); ++i)
        same = g.atlas.groups[i].maps == f.atlas.groups[i].maps && g.atlas.groups[i].dx == f.atlas.groups[i].dx &&
               g.atlas.groups[i].dy == f.atlas.groups[i].dy && g.atlas.groups[i].layer == f.atlas.groups[i].layer;
    CHECK(same);
    CHECK_EQ(g.seams.size(), f.seams.size());
    // The editor's document keeps the stitched fields and its own areas through a save.
    WorldLayout doc;
    CHECK(doc.from_json(read_file(layout_path()), err));
    LayoutArea a;
    a.id = "test_area"; a.name = "Test"; a.offset[0] = 4096; a.layer = -1;
    doc.areas.push_back(a);
    std::string text = doc.to_json();
    WorldLayoutFile back;
    CHECK(back.from_json(text, err));
    CHECK_EQ(back.seams.size(), f.seams.size());
    CHECK_EQ(back.links.size(), f.links.size());
    CHECK_EQ(back.variants.size(), f.variants.size());
    WorldLayout doc2;
    CHECK(doc2.from_json(text, err));
    CHECK_EQ(doc2.areas.size(), 1);
    CHECK_EQ(doc2.areas[0].layer, -1);
    // A group-level field someone adds by hand survives the atlas reader / writer.
    AtlasLayout al;
    CHECK(al.from_json("{\"groups\": [{\"name\": \"x\", \"maps\": [40], \"offset\": [0, 0], \"stairs\": [1, 2]}]}", err));
    CHECK(al.to_json().find("\"stairs\": [1,2]") != std::string::npos);
    // The run-time space: offsets and conversions.
    WorldSpace ws(d);
    CHECK(ws.load(f));
    int32_t w3[3];
    ws.to_world(41, 12767, 3327, 48, w3);
    CHECK_EQ(w3[0], 12767 + 2048);
    int l3[3];
    ws.to_local(40, w3, l3);
    CHECK_EQ(l3[0], 12767 + 2048);
    CHECK(ws.is_portal(40, 41, 1));
    CHECK(ws.is_portal(40, 7, 0));  // the north gate: a declared seam now
    CHECK(!ws.is_portal(43, 44, 0));
    int t7[3];
    {
        int32_t g7[3];
        ws.to_world(40, 13054, 2322, 48, g7);  // the gate egg, at the city's ground height
        ws.to_local(7, g7, t7);
        CHECK_EQ(t7[2], 8);                    // = map 7's ground there (the height trim)
    }
    unsigned m = 0;
    CHECK(ws.map_at(13038, 15042, 0, m));
    CHECK_EQ(m, 40);
    std::vector<unsigned> p40 = ws.seam_partners(40);
    std::sort(p40.begin(), p40.end());
    CHECK(p40 == std::vector<unsigned>({3, 7, 39, 41}));
}

// --- 2. Seam crossings at every portal ----------------------------------------------------

void test_portals(const GameData& d) {
    std::printf("seam portals: stitched crossings vs original teleports\n");
    WorldLayoutFile f;
    std::string err;
    f.load(layout_path(), err);
    WorldSpace ws(d);
    ws.load(f);
    Run st(d, true, true), og(d, false, true);
    int crossed = 0, kept_point = 0, total = 0;
    for (const WorldSeam& s : f.seams)
        for (const WorldPortal& p : s.portals) {
            ++total;
            // Stitched: onto the teleporter -> the partner map at the same world point.
            unsigned c0 = st.w.seam_stats.crossings, f0 = st.w.seam_stats.fallbacks;
            st.w.teleport_actor(1, p.x, p.y, p.z, p.map);
            st.w.stasis = false;
            int32_t before[3];
            ws.to_world(p.map, st.av().x, st.av().y, st.av().z, before);
            for (int k = 0; k < 30 && st.map() == p.map; ++k) st.step();
            int32_t after[3];
            st.world_point(after);
            bool ok = st.map() == p.to_map && st.w.seam_stats.crossings == c0 + 1;
            int dist = std::abs(after[0] - before[0]) + std::abs(after[1] - before[1]);
            crossed += ok;
            kept_point += ok && dist <= 192;
            // Original: the teleport to the arrival egg.
            og.warp(p.x, p.y, p.z, p.map);
            for (int k = 0; k < 30 && og.map() == p.map; ++k) og.step();
            const Item* arr = og.map() == p.to_map ? find_teleporter(og.w, p.to_map, int(p.id), true) : nullptr;
            bool at_arrival = arr && std::abs(og.av().x - arr->x) <= 64 && std::abs(og.av().y - arr->y) <= 64;
            std::printf("  %u -> %u id %u at %d,%d,%d: stitched %s, world point moved %d%s; original %s\n", p.map, p.to_map, p.id, p.x,
                        p.y, p.z, ok ? "crossed" : "FAILED", dist, st.w.seam_stats.fallbacks > f0 ? " (no room: the arrival egg)" : "",
                        at_arrival ? "teleported to the arrival egg" : "?");
            // Off the strip again so the next warp is a fresh start.
            st.w.teleport_actor(1, 100, 100, 0, 40);
            st.run(3);
            CHECK(ok);
            CHECK(at_arrival);
        }
    std::printf("  %d of %d portals crossed, %d kept the world point within 192 (fallbacks %u)\n", crossed, total, kept_point,
                st.w.seam_stats.fallbacks);
    // Walking off the current map's scenery onto the neighbour's ground (no teleporter there):
    // a point map 40 does not cover at all and map 41 covers with ground.
    const MapCoverage &c40 = ws.coverage(40), &c41 = ws.coverage(41);
    int ex = -1, ey = -1;
    for (int y = 2048; y < 16384 && ex < 0; y += 64)
        for (int x = 14000; x < 18000 && ex < 0; x += 64) {
            bool clear = true;
            for (int dy = -64; dy <= 64 && clear; dy += 64)
                for (int dx = -64; dx <= 64 && clear; dx += 64) clear = !c40.any(x + dx, y + dy) && c41.land(x + dx - 2048, y + dy);
            if (clear) { ex = x; ey = y; }
        }
    CHECK(ex >= 0);
    if (ex >= 0) {
        unsigned e0 = st.w.seam_stats.edge_crossings;
        st.w.kernel().kill(1, kProcAny);
        // First on map 40's own scenery a step away (an edge crossing needs him to walk off it:
        // a teleport onto bare ground never crosses), then the step off.
        int sx = ex, sy = ey;
        for (int r = 64; r <= 256 && sx == ex; r += 32)
            for (int a = 0; a < 8 && sx == ex; ++a) {
                static const int ux[8] = {-1, -1, 0, 1, 1, 1, 0, -1}, uy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
                if (c40.any(ex + ux[a] * r - 32, ey + uy[a] * r - 32)) { sx = ex + ux[a] * r; sy = ey + uy[a] * r; }
            }
        st.w.teleport_actor(1, 100, 100, 0, 40);  // (a bare corner: no crossing from there)
        st.w.stasis = false;
        st.run(3);
        CHECK_EQ(st.map(), 40);
        st.w.teleport_actor(1, sx, sy, 48, 40);
        st.run(2);
        st.w.teleport_actor(1, ex, ey, 48, 40);
        for (int k = 0; k < 10 && st.map() == 40; ++k) st.step();
        std::printf("  off the edge of map 40 at %d,%d: now map %u (edge crossings %u)\n", ex, ey, st.map(), st.w.seam_stats.edge_crossings - e0);
        CHECK(st.map() == 41 || st.map() == 7);  // the north edge: map 7's ground lies there too
        CHECK_EQ(st.w.seam_stats.edge_crossings, e0 + 1);
    }
    CHECK_EQ(crossed, total);
    CHECK(kept_point * 4 >= total * 3);
}

// --- 3. Walking across a seam (the view glides on) ----------------------------------------

void test_walk_across(const GameData& d) {
    std::printf("walking across the 40/41 seam\n");
    Ui u(d, true);
    u.frames(20);
    u.avatar_to(14700, 3298, 48, 40);
    u.frames(10);
    World& w = u.w();
    CHECK_EQ(w.map().map_number(), 40);
    u.shot("world_seam_before");
    // Walk east (+x) in world space: the pointer right-down of the Avatar, the right button held.
    auto mouse = [&](int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; u.gm.handle(e); };
    auto rbutton = [&](bool down) {
        InputEvent e; e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp; e.button = 3;
        e.x = u.gm.mouse_x(); e.y = u.gm.mouse_y(); u.gm.handle(e);
    };
    mouse(262, 150);
    rbutton(true);
    int32_t start[3], last[3];
    w.avatar_world_point(start);
    std::copy(start, start + 3, last);
    int max_jump = 0, crossing_frame = -1;
    unsigned c0 = w.seam_stats.crossings;
    for (int f = 0; f < 240; ++f) {
        u.frames(1);
        int32_t p[3];
        w.avatar_world_point(p);
        max_jump = std::max(max_jump, std::abs(p[0] - last[0]) + std::abs(p[1] - last[1]));
        std::copy(p, p + 3, last);
        if (crossing_frame < 0 && w.seam_stats.crossings > c0) {
            crossing_frame = f;
            u.shot("world_seam_crossing");
            u.wide_shot("world_seam_crossing_wide", 3);
            // The neighbour's scenery (map 40 seen from 41) is streamed in around the view.
            CHECK(w.neighbors && w.neighbors->items().size() > 100);
            std::printf("  after the crossing: %zu neighbour items drawn from %u chunks\n", w.neighbors->items().size(),
                        w.neighbors->chunks_live);
        }
    }
    rbutton(false);
    u.frames(5);
    u.shot("world_seam_after");
    std::printf("  walked %d world units east; crossing at frame %d onto map %u; largest step %d; crossing %.2f ms\n",
                last[0] - start[0], crossing_frame, w.map().map_number(), max_jump, w.seam_stats.last_ms);
    CHECK(crossing_frame >= 0);
    CHECK_EQ(w.map().map_number(), 41);
    CHECK(last[0] - start[0] > 700);
    CHECK(max_jump < 96);  // no jump in world space (an original teleport moves ~550)
    u.wide_shot("world_seam_wide", 3);
    // Back west across the seam: the reverse crossing.
    c0 = w.seam_stats.crossings;
    mouse(58, 50);  // left-up of him: -x
    rbutton(true);
    for (int f = 0; f < 300 && w.map().map_number() == 41; ++f) u.frames(1);
    rbutton(false);
    u.frames(5);
    std::printf("  back west: map %u after %u crossing(s)\n", w.map().map_number(), w.seam_stats.crossings - c0);
    CHECK_EQ(w.map().map_number(), 40);
}

// --- 4. Every kept teleport link ----------------------------------------------------------

void test_links(const GameData& d) {
    std::printf("kept teleport links (stitched)\n");
    WorldLayoutFile f;
    std::string err;
    f.load(layout_path(), err);
    WorldSpace ws(d);
    ws.load(f);
    Run r(d, true, true);
    int tried = 0, ok = 0, layer_changes = 0;
    for (const WorldLink& l : f.links) {
        if (l.kind != "egg" || !l.has_from) continue;
        ++tried;
        r.warp(l.from[0], l.from[1], l.from[2], l.from_map);
        for (int k = 0; k < 40 && r.map() == l.from_map; ++k) r.step();
        bool good = r.map() == l.to_map && std::abs(r.av().x - l.to[0]) <= 96 && std::abs(r.av().y - l.to[1]) <= 96;
        if (good) {
            ++ok;
            layer_changes += ws.layer(l.from_map) != ws.layer(l.to_map);
            // The world point after the teleport is the arrival egg's world point.
            int32_t wp[3], want[3];
            r.world_point(wp);
            ws.to_world(l.to_map, l.to[0], l.to[1], l.to[2], want);
            CHECK(std::abs(wp[0] - want[0]) <= 96 && std::abs(wp[1] - want[1]) <= 96);
        } else {
            std::printf("  link %u -> %u (id %d at %d,%d,%d): ended on map %u at %d,%d,%d\n", l.from_map, l.to_map, l.id, l.from[0],
                        l.from[1], l.from[2], r.map(), r.av().x, r.av().y, r.av().z);
        }
    }
    std::printf("  %d of %d teleporter links work as teleports (%d between layers); no seam crossing among them: %s\n", ok, tried,
                layer_changes, r.w.seam_stats.crossings == 0 ? "yes" : "no");
    CHECK(tried > 30);
    CHECK(ok * 10 >= tried * 9);
    CHECK_EQ(r.w.seam_stats.crossings, 0);
}

// --- 5. Companions and a turn-based fight at a seam ----------------------------------------

void test_companions_and_fight(const GameData& d) {
    std::printf("companions across a seam, a turn-based fight at a seam\n");
    Ui u(d, true, true, true);
    u.frames(60);
    Party& p = *u.w().party;
    int joined = 0;
    for (const char* n : {"brannoc", "varn"}) {
        int i = p.find_def(n);
        if (i >= 0 && p.join(i)) ++joined;
    }
    CHECK_EQ(joined, 2);
    u.frames(30);
    // To Central Tenebrae by a teleport (the party travels along), then walk over the seam.
    u.avatar_to(14700, 3298, 48, 40);
    u.frames(90);
    World& w = u.w();
    {
        InputEvent e; e.type = InputEvent::kMouseMove; e.x = 262; e.y = 150; u.gm.handle(e);
        e.type = InputEvent::kMouseDown; e.button = 3; u.gm.handle(e);
        for (int f = 0; f < 300 && w.map().map_number() == 40; ++f) u.frames(1);
        e.type = InputEvent::kMouseUp; u.gm.handle(e);
    }
    u.frames(120);
    CHECK_EQ(w.map().map_number(), 41);
    int near = 0;
    for (int m : p.members) {
        const Actor* a = w.actor(p.state[size_t(m)].id);
        if (!a) continue;
        int dd = std::abs(a->x - w.avatar()->x) + std::abs(a->y - w.avatar()->y);
        std::printf("  %s on map %u, %d units from the Avatar\n", p.defs[size_t(m)].name.c_str(), unsigned(a->mapnum), dd);
        near += a->mapnum == 41 && a->chunk >= 0 && dd < 500;
    }
    CHECK_EQ(near, 2);
    u.shot("world_party_after_seam");
    // A fight beside the seam (turn-based): a ghoul, the C key; then the Avatar steps onto the
    // portal back to map 40 in the middle of it: the fight ends as an escape.
    Actor* av = w.avatar();
    uint16_t g = w.create_monster(142, 0);
    bool placed = false;
    for (int r = 260; r <= 420 && !placed; r += 16)
        for (int k = 0; k < 16 && !placed; ++k) {
            int x = av->x + int(r * std::cos(k * 0.3927)), y = av->y + int(r * std::sin(k * 0.3927));
            World::Placement pl = w.can_exist_shape(142, x, y, av->z, g);
            if (pl.valid && pl.supported && pl.land) { w.move_to(g, x, y, av->z, false); placed = true; }
        }
    CHECK(placed);
    u.frames(2);
    u.key('c');
    u.frames(30);
    CHECK(w.turn->active());
    u.frames(200);
    u.shot("world_fight_at_seam");
    CHECK(w.neighbors && !w.neighbors->items().empty());
    const Item* portal = find_teleporter(w, 40, -1, false);
    CHECK(portal != nullptr);
    if (portal) {
        w.kernel().kill(1, kProcAny);
        w.move_to(1, portal->x, portal->y, portal->z, false);
        for (int f = 0; f < 60 && w.map().map_number() == 41; ++f) u.frames(1);
    }
    std::printf("  fight %s; now on map %u (last end %d)\n", w.turn->active() ? "still on" : "over", w.map().map_number(),
                int(w.turn->last_end));
    CHECK(!w.turn->active());
    CHECK_EQ(w.map().map_number(), 40);
    CHECK(w.turn->last_end == TurnCombat::kEscape);
    u.frames(60);
}

// --- 6. Saves in both modes -----------------------------------------------------------------

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

void test_saves(const GameData& d) {
    std::printf("saves: stitched and original\n");
    Run a(d, !std::getenv("PG_SAVES_ORIGINAL"), true);
    a.warp(14700, 3298, 48, 40);
    a.walk_to(15700, 3298, 300);
    CHECK_EQ(a.map(), 41);
    std::vector<uint8_t> s1 = save_bytes(a.w);
    // The same world loads in a stitched world and gives the same bytes.
    World b(d);
    b.skip_opening = true;
    CHECK(b.set_world_mode(true, layout_path()));
    b.new_game();
    {
        SaveReader in(s1.data(), s1.size());
        CHECK(b.load_game(in));
    }
    std::vector<uint8_t> sb = save_bytes(b);
    size_t first = 0;
    while (first < std::min(sb.size(), s1.size()) && sb[first] == s1[first]) ++first;
    if (sb != s1) {
        std::printf("  note: a load + save is not byte-identical here in either mode (from byte %zu, sizes %zu / %zu):", first,
                    s1.size(), sb.size());
        for (size_t k = first > 24 ? first - 24 : 0; k < first + 24 && k < s1.size(); ++k) std::printf(" %02x", s1[k]);
        std::printf(" /");
        for (size_t k = first > 24 ? first - 24 : 0; k < first + 24 && k < sb.size(); ++k) std::printf(" %02x", sb[k]);
        std::printf("\n");
    }
    // And in an original world (no conversion: one map in its own coordinates either way);
    // its save differs only in the WRLD block.
    World c(d);
    c.skip_opening = true;
    c.new_game();
    {
        SaveReader in(s1.data(), s1.size());
        CHECK(c.load_game(in));
    }
    std::vector<uint8_t> s2 = save_bytes(c);
    CHECK_EQ(s2.size(), sb.size());
    size_t diff = 0;
    for (size_t i = 0; i < std::min(sb.size(), s2.size()); ++i) diff += sb[i] != s2[i];
    std::printf("  the save loaded in a stitched and in an original world, saved again: %zu bytes differ (the mode flag and hash)\n", diff);
    CHECK(diff <= 5);
    CHECK_EQ(c.map().map_number(), 41);
    // Both run on identically for 300 frames (the mode changes nothing while no seam is crossed).
    for (int i = 0; i < 300; ++i) {
        b.input.now_ms = c.input.now_ms = uint32_t(i * 33);
        b.frame();
        c.frame();
    }
    std::vector<uint8_t> e1 = save_bytes(b), e2 = save_bytes(c);
    size_t d2 = 0;
    for (size_t i = 0; i < std::min(e1.size(), e2.size()); ++i) d2 += e1[i] != e2[i];
    CHECK(e1.size() == e2.size() && d2 <= 5);
}

// --- 7. Streaming cost -----------------------------------------------------------------------

void test_streaming(const GameData& d) {
    std::printf("neighbour streaming\n");
    WorldLayoutFile f;
    std::string err;
    f.load(layout_path(), err);
    WorldSpace ws(d);
    ws.load(f);
    NeighborStream ns(d);
    // A walk along the 40/41 seam in map 40 at the camera's pace (8 units a frame).
    double worst = 0, total = 0;
    int frames = 0;
    double t0 = now_ms();
    ws.statics(41);
    ws.statics(39);
    ws.statics(3);
    ws.coverage(40);
    double prep = now_ms() - t0;
    for (int y = 2000; y < 16000; y += 8, ++frames) {
        int cam[3] = {15000, y, 68};
        double a = now_ms();
        ns.update(ws, 40, cam, 320, 200, nullptr);
        double b = now_ms() - a;
        worst = std::max(worst, b);
        total += b;
    }
    std::printf("  first use of three neighbours: %.1f ms (their statics and the coverage, once per map)\n", prep);
    std::printf("  %d frames along the seam: %u chunk builds (%.3f ms each on average, longest %.3f ms), update %.3f ms average, "
                "%.3f ms worst; %u chunks live\n",
                frames, ns.chunk_builds, ns.build_ms_total / std::max(1u, ns.chunk_builds), ns.build_ms_max, total / frames,
                worst, ns.chunks_live);
    CHECK(ns.chunk_builds > 20);
    CHECK(worst < 8.0);  // well inside a 33 ms frame (native)
    CHECK(!ns.items().empty());
    // Nothing from the duplicated strip: no neighbour item sits on the active map's ground.
    const MapCoverage& c40 = ws.coverage(40);
    int on_ground = 0;
    for (const Item* it : ns.items()) on_ground += c40.land(it->x, it->y);
    CHECK_EQ(on_ground, 0);
    // A seam crossing's own cost (a full map change) in a running game.
    Run r(d, true, true);
    r.warp(14700, 3298, 48, 40);
    r.walk_to(15700, 3298, 300);
    std::printf("  a seam crossing (map change 40 -> 41 with cachein and schedules): %.2f ms\n", r.w.seam_stats.last_ms);
    CHECK(r.w.seam_stats.crossings >= 1);
    CHECK(r.w.seam_stats.max_ms < 50);
}

// --- 8. The critical path, Stitched vs Original --------------------------------------------

std::vector<std::pair<unsigned, unsigned>> story_globals() {
    std::set<std::pair<unsigned, unsigned>> out;
    nlohmann::json j = nlohmann::json::parse(read_file(g_data + "/critpath.json"), nullptr, false);
    std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& v) {
        if (v.is_object()) {
            if (v.contains("global") && v["global"].is_array() && v["global"].size() == 2)
                out.insert({v["global"][0].get<unsigned>(), v["global"][1].get<unsigned>()});
            for (auto& [k, x] : v.items()) walk(x);
        } else if (v.is_array()) {
            for (auto& x : v) walk(x);
        }
    };
    walk(j);
    return {out.begin(), out.end()};
}

struct Milestone {
    std::string name;
    unsigned map = 0;
    bool reached = false;
    std::vector<uint16_t> story;
    std::vector<uint8_t> globals;
    int frame = 0;
    std::string how;
};

std::vector<Milestone> replay(const GameData& d, bool stitched) {
    std::printf("critical path replay (%s)\n", stitched ? "stitched" : "original");
    std::vector<std::pair<unsigned, unsigned>> sg = story_globals();
    Run r(d, stitched, false);
    std::vector<Milestone> out;
    auto mark = [&](const std::string& name, bool reached, const std::string& how) {
        Milestone m;
        m.name = name;
        m.map = r.map();
        m.reached = reached;
        m.frame = r.frames;
        m.how = how;
        for (auto [pos, n] : sg) m.story.push_back(r.w.vm().get_global(pos, n));
        for (unsigned b = 0; b < 4096; b += 8) m.globals.push_back(uint8_t(r.w.vm().get_global(b, 8)));
        int32_t wp[3];
        r.world_point(wp);
        std::printf("  %-34s %s  map %u at %d,%d,%d (world %d,%d,%d), frame %d  %s\n", name.c_str(), reached ? "ok " : "NO ", m.map,
                    r.av().x, r.av().y, r.av().z, wp[0], wp[1], wp[2], r.frames, how.c_str());
        out.push_back(m);
    };
    // Waits out a scene (stasis on, then off) and conversations (auto-answered).
    auto settle = [&](int max) {
        for (int i = 0; i < max && (r.w.stasis || r.w.ask_menu); ++i) r.step();
    };
    mark("the opening (Devon)", r.w.vm().get_global(248, 1) || r.w.vm().get_global(251, 1), "new game, 8400 frames");

    // The execution at the end of the dock.
    r.warp(11732, 5844, 48, 3);
    bool started = false;
    for (int f = 0; f < 9000; ++f) {
        r.step();
        if (r.w.stasis) started = true;
        if (started && !r.w.stasis && r.w.vm().get_global(89, 1)) break;
    }
    settle(3000);
    mark("the execution", r.w.vm().get_global(89, 1) == 1, "debug warp to the dock's end, the scene plays");

    // Walk up the road through the open gate into Central Tenebrae.
    std::string how = "walked from the dock to the gate";
    for (auto [x, y] : {std::pair{11800, 4600}, {11650, 3200}, {11560, 2300}, {11516, 1660}}) {
        if (r.map() != 3) break;
        r.walk_to(x, y, 2500);
        settle(3000);
    }
    if (r.map() == 3) { how += " (stuck at " + std::to_string(r.av().x) + "," + std::to_string(r.av().y) + "; warped onto the gate)"; r.warp(11516, 1700, 48, 3); r.run(60); }
    for (int i = 0; i < 60 && r.map() == 3; ++i) r.step();
    how += stitched ? std::string(", seam crossing ") + (r.w.seam_stats.crossings ? "(walked through)" : "(none)") : ", teleporter";
    mark("Central Tenebrae", r.map() == 40 && r.w.visited_maps.count(40), how);

    // The waypoint between the dragon statues (TELPFLAG at 13087,8607,104).
    r.warp(13087, 8800, 104, 40);
    r.walk_to(13087, 8607, 600);
    r.run(30);
    settle(2000);
    mark("the Central Tenebrae waypoint", r.w.vm().get_global(68, 1) == 1, "debug warp to the palace grounds, walked onto it");

    // East Tenebrae (over the 40/41 seam) and Bentic.
    r.warp(14900, 14070, 48, 40);
    r.walk_to(15800, 14070, 600);
    for (int i = 0; i < 60 && r.map() == 40; ++i) r.step();
    std::string east = stitched ? "walked over the 40/41 seam" : "walked into the teleporter";
    Actor* bentic = r.w.actor(12);
    bool talked = false;
    // The player's way through his talk (critpath.json's hint): neighbours, Mythran, the
    // Plateau first; otherwise anything not asked yet; "bye" last.
    std::set<std::string> asked;
    auto answer = [&]() {
        if (!r.w.ask_menu || r.w.ask_menu->frames_open < 10) return;
        int pick = -1, best = -1;
        for (size_t i = 0; i < r.w.ask_menu->answers.size(); ++i) {
            std::string a = r.w.vm().string(r.w.ask_menu->answers[i]);
            int score = asked.count(a) ? 0 : 10;
            for (const char* k : {"Mythran", "Plateau", "scholar", "neighbor", "leav", "Devon", "people"})
                if (a.find(k) != std::string::npos) score += 20;
            if (a.find("bye") != std::string::npos) score = 1;
            if (score > best) { best = score; pick = int(i); }
        }
        if (pick < 0) return;
        asked.insert(r.w.vm().string(r.w.ask_menu->answers[size_t(pick)]));
        r.w.choose_answer(unsigned(pick));
    };
    for (int tries = 0; tries < 6 && bentic && !r.w.vm().get_global(295, 1); ++tries) {
        if (bentic->mapnum != r.map()) break;
        r.warp(bentic->x + 64, bentic->y, bentic->z, r.map());
        r.w.use_item(12);
        uint16_t seen = 0;
        bool in_talk = false;
        for (int i = 0; i < 6000; ++i) {
            answer();
            r.step();
            if (r.w.stasis || r.w.ask_menu) in_talk = true;
            if (in_talk && !r.w.stasis && !r.w.ask_menu) break;
            if (std::getenv("PG_TRACE_TALK") && r.w.ask_menu && r.w.ask_menu->pid != seen) {
                seen = r.w.ask_menu->pid;
                std::printf("    menu:");
                for (uint16_t a : r.w.ask_menu->answers) std::printf(" [%s]", r.w.vm().string(a).c_str());
                std::printf("\n");
            }
        }
        talked |= in_talk;
    }
    mark("East Tenebrae: Bentic about Mythran", r.w.vm().get_global(295, 1) == 1,
         east + (talked ? ", talked to Bentic (answers picked like a player: neighbours, Mythran, the Plateau)" : ", Bentic not found"));

    // Back to Central Tenebrae (the reverse seam), then the north exit to Herdsman's Valley.
    if (r.map() == 41) {
        r.warp(14332, 3298, 48, 41);
        r.run(30);
        r.walk_to(12200, 3298, 900);
        for (int i = 0; i < 60 && r.map() == 41; ++i) r.step();
    }
    std::string back = r.map() == 40 ? (stitched ? "back over the 41/40 seam" : "back through the teleporter into 40")
                                     : "not back in 40 (stasis " + std::to_string(r.w.stasis) + " paused " + std::to_string(r.w.kernel().paused()) +
                                           " gumps " + std::to_string(r.w.gumps.list.size()) + " barks " + std::to_string(r.w.barks.size()) + " at " + std::to_string(r.av().x) + "," + std::to_string(r.av().y) + "," +
                                           std::to_string(r.av().z) + " on map " + std::to_string(r.map()) + ")";
    r.warp(13054, 2700, 48, 40);
    r.walk_to(13054, 2322, 900);
    for (int i = 0; i < 60 && r.map() == 40; ++i) r.step();
    settle(2000);
    mark("Herdsman's Valley (north exit)", r.map() == 7, back + "; debug warp near the north exit, walked into it");

    // The Plateau cave entrance.
    r.warp(9498, 20000, 8, 7);
    r.walk_to(9498, 19702, 900);
    for (int i = 0; i < 60 && r.map() == 7; ++i) r.step();
    settle(2000);
    mark("the Plateau", r.map() == 8 && r.w.visited_maps.count(8), "debug warp near the cave, walked into its teleporter");

    // Mythran (NPC 33): the first meeting ("May I ask your name?").
    Actor* myth = r.w.actor(33);
    bool met = false;
    for (int tries = 0; tries < 4 && myth && !r.w.vm().get_global(284, 1); ++tries) {
        if (myth->mapnum != r.map()) break;
        r.warp(myth->x + 64, myth->y, myth->z, r.map());
        r.w.use_item(33);
        bool in_talk = false;
        for (int i = 0; i < 6000; ++i) {
            if (r.w.ask_menu && r.w.ask_menu->frames_open >= 10) {
                int pick = -1, best = -1;
                for (size_t k = 0; k < r.w.ask_menu->answers.size(); ++k) {
                    std::string a = r.w.vm().string(r.w.ask_menu->answers[k]);
                    int score = asked.count(a) ? 0 : 10;
                    if (a.find("name") != std::string::npos) score += 20;
                    if (a.find("bye") != std::string::npos) score = 1;
                    if (score > best) { best = score; pick = int(k); }
                }
                if (pick >= 0) { asked.insert(r.w.vm().string(r.w.ask_menu->answers[size_t(pick)])); r.w.choose_answer(unsigned(pick)); }
            }
            r.step();
            if (r.w.stasis || r.w.ask_menu) in_talk = true;
            if (in_talk && !r.w.stasis && !r.w.ask_menu) break;
        }
        met |= in_talk;
    }
    mark("Mythran", r.w.vm().get_global(284, 1) == 1, met ? "debug warp beside him, talked (\"May I ask your name?\")" : "Mythran not found");

    // The Cemetery by the East Road: East Tenebrae over the 41/5 seam, then the road's
    // teleporter to the Cemetery (map 6).
    r.warp(22700, 11007, 48, 41);
    r.walk_to(23500, 11007, 600);
    for (int i = 0; i < 60 && r.map() == 41; ++i) r.step();
    std::string road = r.map() == 5 ? (stitched ? "walked over the 41/5 seam onto the East Road" : "walked into the teleporter to the East Road")
                                    : "not on the East Road";
    r.warp(7423, 11300, 48, 5);
    r.walk_to(7423, 10943, 900);
    for (int i = 0; i < 60 && r.map() == 5; ++i) r.step();
    settle(2000);
    mark("the Cemetery (East Road)", r.map() == 6 && r.w.visited_maps.count(6) && r.w.visited_maps.count(5),
         road + "; debug warp near the cemetery gate, walked into it");
    std::printf("  seam crossings %u (edge %u, fallbacks %u), longest %.2f ms\n", r.w.seam_stats.crossings, r.w.seam_stats.edge_crossings,
                r.w.seam_stats.fallbacks, r.w.seam_stats.max_ms);
    return out;
}

void test_critical_path(const GameData& d) {
    std::vector<Milestone> o = replay(d, false), s = replay(d, true);
    CHECK_EQ(o.size(), s.size());
    std::vector<std::pair<unsigned, unsigned>> sg = story_globals();
    std::printf("critical path: story globals (%zu from critpath.json) at each milestone, original vs stitched\n", sg.size());
    int same_all = 0;
    for (size_t i = 0; i < std::min(o.size(), s.size()); ++i) {
        std::vector<std::string> diffs;
        for (size_t k = 0; k < sg.size(); ++k)
            if (o[i].story[k] != s[i].story[k])
                diffs.push_back("G[" + std::to_string(sg[k].first) + ":" + std::to_string(sg[k].second) + "] " +
                                std::to_string(o[i].story[k]) + "/" + std::to_string(s[i].story[k]));
        int bits = 0;
        for (size_t k = 0; k < o[i].globals.size(); ++k) bits += __builtin_popcount(unsigned(o[i].globals[k] ^ s[i].globals[k]));
        std::printf("  %-34s original %s map %u, stitched %s map %u; story globals %s; all globals: %d bits differ\n",
                    o[i].name.c_str(), o[i].reached ? "ok" : "NO", o[i].map, s[i].reached ? "ok" : "NO", s[i].map,
                    diffs.empty() ? "identical" : "differ", bits);
        for (const std::string& x : diffs) std::printf("      %s\n", x.c_str());
        CHECK(o[i].reached);
        CHECK(s[i].reached);
        CHECK_EQ(o[i].map, s[i].map);
        CHECK(diffs.empty());
        same_all += diffs.empty() && o[i].reached && s[i].reached;
    }
    std::printf("  %d of %zu milestones reached in both modes with identical story globals\n", same_all, o.size());
}

// --- 9. The atlas with the stitched layout -----------------------------------------------------

void test_atlas(const GameData& d) {
    std::printf("the atlas with the stitched layout\n");
    std::vector<ShapeFont> fonts;
    for (unsigned i = 0; i < d.fonts.count(); ++i) fonts.emplace_back(d.fonts.shape(i), int(i));
    AtlasView a(d, fonts);
    a.load_layout(layout_path());
    CHECK(a.layered());
    a.show(40, 13038, 15042, 48, 4);
    CHECK_EQ(a.layer(), 0);
    a.show(50, 21311, 10527, 0, 4);
    CHECK(a.layer() < 0);
    InputEvent e;
    e.type = InputEvent::kKeyDown;
    e.key = kKeyPageUp;
    int before = a.layer();
    a.handle(e);
    CHECK(a.layer() > before);
    if (!g_shots.empty()) {
        for (int L : {1, 0, -1, -2}) {
            a.set_layer(L);
            a.show_all();
            std::vector<uint32_t> px;
            a.render(px, -1);
            std::string ln = L < 0 ? "m" + std::to_string(-L) : std::to_string(L);
            for (const char* stem : {"/world_atlas_layer", "/layout_atlas_layer"}) {
                std::string path = g_shots + stem + ln + ".png";
                save_png(path, px.data(), AtlasView::kW, AtlasView::kH, 2);
                std::printf("  shot %s\n", path.c_str());
            }
        }
    }
}

// The opening in both modes from a new game: same state (nothing differs before a seam).
void test_determinism(const GameData& d) {
    std::printf("the opening: original twice, stitched twice\n");
    auto globals = [](Run& r) { std::vector<uint8_t> g; for (unsigned b = 0; b < 4096; b += 8) g.push_back(uint8_t(r.w.vm().get_global(b, 8))); return g; };
    Run o1(d, false, false), o2(d, false, false), s1(d, true, false), s2(d, true, false);
    std::printf("  G[248:1]: %u %u %u %u\n", o1.w.vm().get_global(248, 1), o2.w.vm().get_global(248, 1), s1.w.vm().get_global(248, 1),
                s2.w.vm().get_global(248, 1));
    CHECK(globals(o1) == globals(o2));
    CHECK(globals(s1) == globals(s2));
    CHECK(globals(o1) == globals(s1));
    CHECK(save_bytes(o1.w).size() == save_bytes(s1.w).size());
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_world_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");  // Nerissa's own figure (data/shapes/)
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, void (*f)(const GameData&)) {
        if (!only.empty() && only != n) return;
        double t = now_ms();
        f(d);
        std::printf("  (%s: %.1f s)\n", n, (now_ms() - t) / 1000);
    };
    run("layout", test_layout);
    run("portals", test_portals);
    run("walk", test_walk_across);
    run("links", test_links);
    run("party", test_companions_and_fight);
    run("saves", test_saves);
    run("stream", test_streaming);
    run("atlas", test_atlas);
    run("determinism", test_determinism);
    run("critpath", test_critical_path);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
