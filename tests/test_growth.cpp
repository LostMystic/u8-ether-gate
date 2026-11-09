// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Leveling stage 4 (game/stat_growth.h, PLAN-LEVELING.md 4): trainers, tomes, elixirs, ordeals and
// quest stats through the real game mode, and the placements against the real maps (free spots,
// nothing stamped over, reachable by a flood of the real collision tests).
//   pagan_growth_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
//   ONLY: data, places, reach, train, dialog, tomes, elixirs, ordeals, quests, saves, off
//   or a probe: "probe MAP X Y Z R", "anchor MAP SHAPE", "climb MAP"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "game/critpath.h"
#include "game/game_log.h"
#include "game/ledge.h"
#include "game/leveling.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/stat_growth.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;
using nlohmann::json;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool leveling, bool companions = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;
        w.party->enabled = companions;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        if (!w.dialog->load_dir(g_data + "/dialog", err)) std::printf("  dialog: %s\n", err.c_str());
        w.dialog->enabled = false;  // the trainers are Leveling's: they need no "New Dialogue"
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Leveling& l() { return *gm.world().leveling; }
    StatGrowth& g() { return *gm.world().leveling->growth; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ticks(int n) { for (int i = 0; i < n; ++i) gm.tick(); }
    void ev(InputEvent e) { gm.handle(e); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    int idx(const std::string& id) { return p().find_def(id); }
    CompanionState& st(const std::string& id) { return p().state[size_t(idx(id))]; }
    Actor* a(const std::string& id) { return w().actor(st(id).id); }
    Actor* av() { return w().avatar(); }
    // The Avatar on that map at that spot, nothing running.
    bool place(unsigned map, int x, int y, int z) {
        World& wd = w();
        wd.kernel().kill(1, kProcActorAnim);
        wd.kernel().kill(1, kProcGravity);
        Actor& a = *av();
        a.flags &= uint16_t(~kItemHanging);
        a.set(Actor::kAnimLock, false);
        if (wd.map_number_now() != map) { wd.teleport_actor(1, x, y, z, map); ticks(6); }
        wd.move_to(1, x, y, z, false);
        ticks(2);
        return wd.map_number_now() == map;
    }
    std::vector<std::string> menu() {
        std::vector<std::string> out;
        if (w().ask_menu) for (uint16_t sid : w().ask_menu->answers) out.push_back(w().vm().string(sid));
        return out;
    }
    int index_of(const std::string& label) {
        std::vector<std::string> m = menu();
        for (size_t i = 0; i < m.size(); ++i)
            if (m[i] == label) return int(i);
        return -1;
    }
    int index_trimmed(const std::string& label) {
        std::vector<std::string> m = menu();
        for (size_t i = 0; i < m.size(); ++i) {
            std::string a = m[i];
            while (!a.empty() && a.back() == ' ') a.pop_back();
            if (a == label) return int(i);
        }
        return -1;
    }
    // Runs until a menu has been up `settle` frames (speech hurried along); false on timeout.
    bool wait_menu(int settle = 6, int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (w().ask_menu && w().ask_menu->frames_open >= settle) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
    bool wait_idle(int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (!w().ask_menu && !w().in_conversation() && w().barks.empty()) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
    void near_npc(uint16_t npc) {
        const Actor* g = w().actor(npc);
        w().teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
        frames(30);
    }
    void set(unsigned pos, unsigned n, unsigned v) { w().vm().set_global(pos, n, uint16_t(v)); }
    std::string joined() {
        std::string o;
        for (const std::string& m : menu()) o += (o.empty() ? "" : " | ") + m;
        return o;
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 2);
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

uint16_t give(World& w, uint16_t actor, unsigned shape, unsigned frame, uint16_t quality) {
    uint16_t id = w.create_item(shape, frame);
    w.item(id)->quality = quality;
    uint16_t pack = w.equipped(actor, 7);
    w.move_to_container(id, pack ? pack : actor);
    return id;
}

// --- The flood: the real collision tests (as tests/test_underworld.cpp) ---------------------------

using Cell = std::tuple<int, int, int>;

struct Flood {
    World& w;
    int rise;
    int xd, yd, zd;
    uint16_t flags;
    std::map<Cell, int> seen;  // cell -> climbs needed (0-1 search)
    std::deque<Cell> q;
    Flood(World& world, int r) : w(world), rise(r) {
        w.footprint(*w.avatar(), xd, yd, zd);
        flags = w.shape_flags(*w.avatar());
    }
    bool on_filler(uint16_t land) const {
        const Item* it = w.item(land);
        return !it || it->shape == 564;
    }
    bool stand(int x, int y, int z) const {
        World::Placement p = w.can_exist(x, y, z, xd, yd, zd, flags, 1);
        return p.valid && p.supported && !on_filler(p.land);
    }
    void push(int x, int y, int z, int cost, bool front) {
        auto it = seen.find(Cell(x, y, z));
        if (it != seen.end() && it->second <= cost) return;
        seen[Cell(x, y, z)] = cost;
        if (front) q.emplace_front(x, y, z);
        else q.emplace_back(x, y, z);
    }
    void step(int x, int y, int z, int nx, int ny, int cost) {
        World::Placement p = w.can_exist(nx, ny, z, xd, yd, zd, flags, 1);
        if (p.valid) {
            if (p.supported) { if (!on_filler(p.land)) push(nx, ny, z, cost, true); return; }
            if (p.support_z > -1000 && p.support_z < z && stand(nx, ny, p.support_z)) push(nx, ny, p.support_z, cost, true);
            return;
        }
        for (int r = 8; r <= rise; r += 8) {
            if (!w.can_exist(x, y, z + r, xd, yd, zd, flags, 1).valid) return;
            World::Placement up = w.can_exist(nx, ny, z + r, xd, yd, zd, flags, 1);
            if (!up.valid) continue;
            if (!up.supported || on_filler(up.land)) return;
            if (r > 8 && !ledge::ledge_spot(w, *w.avatar(), nx, ny, z + r)) return;
            push(nx, ny, z + r, r > 8 ? cost + 1 : cost, r <= 8);
            return;
        }
    }
    void run(int grid, size_t cap) {
        while (!q.empty() && seen.size() < cap) {
            auto [x, y, z] = q.front();
            q.pop_front();
            int c = seen[Cell(x, y, z)];
            for (auto [dx, dy] : {std::pair{grid, 0}, {-grid, 0}, {0, grid}, {0, -grid}}) step(x, y, z, x + dx, y + dy, c);
        }
    }
    // Onto the grid, on the floor nearest the point; false if there is no room.
    bool seed(int sx, int sy, int sz, int grid) {
        for (int r = 0; r <= 192; r += grid)
            for (int dy = -r; dy <= r; dy += grid)
                for (int dx = -r; dx <= r; dx += grid) {
                    int x = (sx | (grid - 1)) + dx, y = (sy | (grid - 1)) + dy;
                    World::Placement p = w.can_exist(x, y, sz + 40, xd, yd, zd, flags, 1);
                    if (p.support_z > -1000 && std::abs(p.support_z - sz) <= 48 && stand(x, y, p.support_z)) {
                        push(x, y, p.support_z, 0, false);
                        return true;
                    }
                }
        return false;
    }
    // The fewest climbs to stand within `r` of the point (-1 = never).
    int reached(int x, int y, int z, int r = 48) const {
        int best = -1;
        for (const auto& e : seen) {
            auto [cx, cy, cz] = e.first;
            if (std::abs(cx - x) <= r && std::abs(cy - y) <= r && std::abs(cz - z) <= 24 && (best < 0 || e.second < best)) best = e.second;
        }
        return best;
    }
};

void expand_all(Ui& u) {
    u.w().view_w = u.w().view_h = 65000;
    u.ticks(3);
}

// Where the Avatar arrives on a map: its teleporter destinations (frame 1) and usecode-egg spots.
std::vector<std::array<int, 3>> arrivals(World& w) {
    std::vector<std::array<int, 3>> out;
    for (size_t i = 512; i < 32768; ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0 || it->parent) continue;
        if (w.data().typeflags.get(it->shape).family() == kFamTeleportEgg && it->frame == 1) out.push_back({it->x, it->y, it->z});
    }
    return out;
}

// The footprint of a shape (a scratch item).
void shape_box(World& w, unsigned shape, unsigned frame, int& xd, int& yd, int& zd) {
    (void)frame;
    w.data().typeflags.get(shape).footprint(false, xd, yd, zd);
}

// Nothing else in the box an item of this shape would take at (x, y, z), it fits and stands on
// something: what "not stamped over anything" means here. `blocker` names the first item in the way.
bool clear_spot(World& w, unsigned shape, unsigned frame, int x, int y, int z, uint16_t ignore = 0, uint16_t* blocker = nullptr) {
    World::Placement p = w.can_exist_shape(shape, x, y, z, ignore);
    if (!p.valid || !p.supported || p.support_z != z) return false;
    int xd, yd, zd;
    shape_box(w, shape, frame, xd, yd, zd);
    const int z1 = z + std::max(zd, 1);
    for (size_t i = 256; i < 32768; ++i) {  // (not the actors: they walk about)
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0 || it->parent || i == ignore) continue;
        Box b = w.box_of(*it);
        if (b.x - b.xd >= x || x - xd >= b.x || b.y - b.yd >= y || y - yd >= b.y) continue;
        if (b.z + std::max(b.zd, 1) <= z || z1 <= b.z) continue;
        if (blocker) *blocker = uint16_t(i);
        return false;
    }
    return true;
}

// --- Probes ----------------------------------------------------------------------------------------

// A picture: the Avatar standing at (sx, sy, sz) and an item of the shape at (x, y, z).
void look_at(const GameData& d, unsigned map, int x, int y, int z, unsigned shape, unsigned frame, const std::string& file, int ax, int ay, int az) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, ax ? ax : x + 48, ax ? ay : y + 48, ax ? az : z);
    uint16_t id = u.w().create_item(shape, frame);
    u.w().move_to(id, x, y, z, false);
    u.frames(30);
    u.w().move_to(1, ax ? ax : x + 48, ax ? ay : y + 48, ax ? az : z, false);
    u.frames(2);
    g_shots = g_shots.empty() ? "/tmp" : g_shots;
    u.shot(file);
    std::printf("  avatar at %d,%d,%d\n", u.av()->x, u.av()->y, u.av()->z);
}

// Spots the flood from the map's arrivals reaches, far from every arrival and needing the most
// climbs, where an item of this shape fits without touching anything: candidates for a placement.
void cands(const GameData& d, unsigned map, unsigned shape, unsigned frame, int want_climbs) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, 1000, 1000, 0);
    expand_all(u);
    World& w = u.w();
    auto arr = arrivals(w);
    if (arr.empty()) { std::printf("  no arrivals\n"); return; }
    u.place(map, arr[0][0], arr[0][1], arr[0][2]);
    expand_all(u);
    Flood f(w, 120);
    for (auto& a : arr) f.seed(a[0], a[1], a[2], 32);
    f.run(32, 900000);
    std::printf("  %zu cells from %zu arrivals\n", f.seen.size(), arr.size());
    std::vector<std::tuple<long, int, int, int, int>> all;  // -score, x, y, z, climbs
    for (auto& e : f.seen) {
        auto [x, y, z] = e.first;
        long dmin = 1L << 40;
        for (auto& a : arr) dmin = std::min(dmin, long(x - a[0]) * (x - a[0]) + long(y - a[1]) * (y - a[1]));
        long score = e.second * 4000L + long(std::sqrt(double(dmin)));
        all.emplace_back(-score, x, y, z, e.second);
    }
    std::sort(all.begin(), all.end());
    std::vector<std::array<int, 3>> picked;
    for (auto& c : all) {
        auto [sc, x, y, z, cl] = c;
        bool near = false;
        for (auto& p : picked) near = near || (std::abs(p[0] - x) < 768 && std::abs(p[1] - y) < 768);
        if (near) continue;
        if (std::getenv("PAGAN_GROWTH_VERBOSE")) {
            uint16_t bl = 0;
            bool ok = clear_spot(w, shape, frame, x, y, z, 0, &bl);
            World::Placement pl = w.can_exist_shape(shape, x, y, z, 0);
            const Item* bi = w.item(bl);
            std::printf("    try %d,%d,%d: %d valid %d sup %d sz %d blocker %u shape %d\n", x, y, z, ok, pl.valid, pl.supported, pl.support_z, bl,
                        bi ? bi->shape : -1);
        }
        // An area, not a post or the top of a thin wall: most of the 5 x 5 cells around at this height.
        int area = 0;
        for (int gy = -64; gy <= 64; gy += 32)
            for (int gx = -64; gx <= 64; gx += 32) area += f.seen.count(Cell(x + gx, y + gy, z)) ? 1 : 0;
        if (area < 18 || x < 1600 || y < 1600) continue;
        // Real floor only (not the top of a wall or a roof): land under it, no roof.
        World::Placement pl = w.can_exist(x, y, z, f.xd, f.yd, f.zd, f.flags, 1);
        const Item* land = w.item(pl.land);
        if (!land) continue;
        const ShapeInfo& li = w.data().typeflags.get(land->shape);
        if (!li.is(ShapeInfo::kLand) || li.is(ShapeInfo::kRoof) || (want_climbs >= 0 && cl != want_climbs)) continue;
        // The item beside where he stands: the cell itself, or a step off it.
        for (auto [dx, dy] : {std::pair{0, 0}, {16, 0}, {0, 16}, {-16, 0}, {0, -16}}) {
            if (!clear_spot(w, shape, frame, x + dx, y + dy, z)) continue;
            picked.push_back({x + dx, y + dy, z});
            std::printf("  %d,%d,%d  climbs %d, %ld from an arrival, on shape %u\n", x + dx, y + dy, z, cl, (-sc) % 4000, land->shape);
            break;
        }
        if (picked.size() >= 15) break;
    }
}

// Free spots for a shape near a point, with the climbs a flood from the map's arrivals needs to
// stand beside each (-1: the flood never gets there).
void spots(const GameData& d, unsigned map, int x, int y, int z, unsigned shape, unsigned frame) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, x, y, z + 8);
    expand_all(u);
    World& w = u.w();
    Flood f(w, 120);
    for (auto& a : arrivals(w)) f.seed(a[0], a[1], a[2], 32);
    f.run(32, 900000);
    std::printf("  flood %zu cells; the point itself: %d climbs\n", f.seen.size(), f.reached(x, y, z, 64));
    {
        long best = -1;
        Cell bc;
        for (auto& e : f.seen) {
            auto [cx, cy, cz] = e.first;
            long dd = long(cx - x) * (cx - x) + long(cy - y) * (cy - y) + 16L * (cz - z) * (cz - z);
            if (best < 0 || dd < best) { best = dd; bc = e.first; }
        }
        if (best >= 0) std::printf("  nearest flood cell %d,%d,%d (%d climbs)\n", std::get<0>(bc), std::get<1>(bc), std::get<2>(bc), f.seen[bc]);
    }
    Flood local(w, 120);
    local.seed(x, y, z, 16);
    local.run(16, 200000);
    std::vector<std::tuple<int, int, int, int, int, int>> found;
    for (int dz = -16; dz <= 16; dz += 8)
        for (int dy = -192; dy <= 192; dy += 16)
            for (int dx = -192; dx <= 192; dx += 16) {
                int cx = x + dx, cy = y + dy, cz = z + dz;
                if (cz < 0 || !clear_spot(w, shape, frame, cx, cy, cz)) continue;
                found.emplace_back(dx * dx + dy * dy, cx, cy, cz, f.reached(cx, cy, cz, 64), local.reached(cx, cy, cz, 40));
            }
    std::sort(found.begin(), found.end());
    for (size_t k = 0; k < found.size() && k < 12; ++k) {
        auto [dd, cx, cy, cz, c, lc] = found[k];
        std::printf("  spot %d,%d,%d (%d away): from arrivals %d climbs, from the point %d\n", cx, cy, cz, int(std::sqrt(double(dd))),
                    c, lc);
    }
}


void probe(const GameData& d, unsigned map, int x, int y, int z, int r) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, x, y, z);
    expand_all(u);
    World& w = u.w();
    for (size_t i = 512; i < 32768; ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0 || it->parent) continue;
        if (std::abs(it->x - x) > r || std::abs(it->y - y) > r) continue;
        int xd, yd, zd;
        w.footprint(*it, xd, yd, zd);
        std::printf("  %5zu shape %4u:%-3u at %5d,%5d,%3d  box %3dx%3dx%3d  fam %d q %u %s %s\n", i, it->shape, it->frame, it->x, it->y,
                    it->z, xd, yd, zd, w.data().typeflags.get(it->shape).family(), it->quality,
                    it->origin == Item::kFixed ? "fixed" : "nonfixed", w.data().usecode.class_name(it->shape).c_str());
    }
}

void anchor(const GameData& d, unsigned map, unsigned shape) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, 1000, 1000, 0);
    expand_all(u);
    World& w = u.w();
    for (size_t i = 512; i < 32768; ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->shape != shape) continue;
        int x, y, z;
        w.location(*it, x, y, z);
        const Item* r = w.root_container(*it);
        std::printf("  %zu frame %u q %u at %d,%d,%d%s (root shape %u)\n", i, it->frame, it->quality, x, y, z, it->parent ? " in a container" : "",
                    r ? r->shape : 0);
    }
    for (auto& a : arrivals(w)) std::printf("  arrival %d,%d,%d\n", a[0], a[1], a[2]);
}

// The climbing map: from every arrival, the spots that need the most climbs.
void climb(const GameData& d, unsigned map) {
    Ui u(d, true, false);
    u.frames(20);
    u.place(map, 1000, 1000, 0);
    expand_all(u);
    World& w = u.w();
    auto arr = arrivals(w);
    if (arr.empty()) { std::printf("  no arrivals\n"); return; }
    u.place(map, arr[0][0], arr[0][1], arr[0][2]);
    expand_all(u);
    Flood f(w, 120);
    for (auto& a : arr) f.seed(a[0], a[1], a[2], 32);
    f.run(32, 900000);
    std::map<int, int> hist;
    for (auto& e : f.seen) ++hist[e.second];
    std::printf("  %zu cells\n", f.seen.size());
    for (auto& h : hist) std::printf("  %d climbs: %d cells\n", h.first, h.second);
    int top = hist.rbegin()->first;
    int shown = 0;
    for (auto& e : f.seen)
        if (e.second >= std::max(2, top - 1) && shown < 60) {
            auto [x, y, z] = e.first;
            std::printf("    %d climbs at %d,%d,%d\n", e.second, x, y, z);
            ++shown;
        }
}

// --- Tests -----------------------------------------------------------------------------------------

json read_json(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str(), nullptr, false, true);
}

const GrowthItem* item_def(StatGrowth& g, const std::string& id) { return g.find_item(id); }

// Our item as the game makes it, into someone's pack.
uint16_t give_ours(World& w, const GrowthItem& g, uint16_t actor) { return give(w, actor, g.shape, g.frame, g.quality); }

int money(Ui& u, uint16_t actor) { return u.g().obsidian(actor); }

void test_data(const GameData& d) {
    std::printf("data: the files, the prices, the obsidian there is, our qualities unused\n");
    Ui u(d, true);
    StatGrowth& g = u.g();
    CHECK(g.loaded);
    CHECK_EQ(g.trainers.size(), size_t(2));
    CHECK(g.trainer(5) && g.trainer(33));
    int tomes = 0, elixirs = 0;
    for (const GrowthItem& it : g.items) (it.kind == "tome" ? tomes : elixirs)++;
    std::printf("  %d tomes, %d elixirs, %zu ordeals, %zu quest steps\n", tomes, elixirs, g.ordeals.size(), g.quest_stats.size());
    CHECK(tomes >= 4 && tomes <= 6);
    CHECK(elixirs >= 6 && elixirs <= 8);
    CHECK(g.ordeals.size() >= 2 && g.ordeals.size() <= 4);
    CHECK(g.quest_stats.size() >= 5 && g.quest_stats.size() <= 8);
    for (const GrowthOrdeal& o : g.ordeals) CHECK(o.gain == 2 && (o.stat == kGrowStr || o.stat == kGrowDex));
    for (const GrowthItem& it : g.items) {
        if (it.kind == "tome") CHECK(it.stat == kGrowInt && (it.gain == 1 || it.gain == 2));
        else CHECK(it.gain == 1 && (it.stat == kGrowStr || it.stat == kGrowDex));
    }
    // The quest steps exist; ids are unique.
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_data + "/critpath.json", err));
    for (const QuestStat& q : g.quest_stats) {
        CHECK(cp.step_index(q.step) >= 0);
        CHECK_EQ(q.avatar, 0);  // primarily the companions: the Avatar gets nothing here
    }
    std::set<std::string> ids;
    for (const GrowthItem& it : g.items) CHECK(ids.insert(it.id).second);
    for (const GrowthOrdeal& o : g.ordeals) CHECK(ids.insert(o.id).second);
    // Prices: 50 at 10, climbing, about 400-600 near 30, nothing at 30.
    std::printf("  prices:");
    for (int v = 10; v <= 30; v += 2) std::printf(" %d:%d", v, g.price(v));
    std::printf(" 29:%d\n", g.price(29));
    CHECK_EQ(g.price(10), 50);
    CHECK_EQ(g.price(8), 50);
    for (int v = 10; v < 29; ++v) CHECK(g.price(v + 1) > g.price(v));
    CHECK(g.price(29) >= 400 && g.price(29) <= 600);
    CHECK_EQ(g.price(30), 0);
    CHECK_EQ(g.price(35), 0);
    int low = 0, high = 0;
    for (int v = 10; v < 20; ++v) low += g.price(v);
    for (int v = 20; v < 30; ++v) high += g.price(v);
    // What the game holds: placed obsidian (NONFIXED, all maps), and one pass of the monster eggs'
    // expected drops (combat.md 7: chance 0.5, the counts by type; data/monster_counts.json).
    long placed = 0;
    for (unsigned m = 0; m < d.nonfixed.count(); ++m)
        for (const ItemRecord& r : parse_item_records(d.nonfixed.entry(m)))
            if (r.shape == StatGrowth::kMoneyShape) placed += r.quality;
    json mc = read_json(g_data + "/monster_counts.json");
    const std::map<std::string, double> per = {{"troll", 26.25}, {"golem", 26.25}, {"ghost", 15}, {"skeleton", 5.5},
                                               {"changeling", 5.5}, {"guard", 7.5}, {"theurgist", 3.75}};
    double drops = 0;
    for (auto& m : mc["maps"].items())
        for (auto& e : m.value()["eggs"].items()) {
            std::string name = mc["monster_types"].contains(e.key()) ? mc["monster_types"][e.key()].value("name", std::string()) : "";
            auto it = per.find(name);
            if (it != per.end()) drops += it->second * e.value().value("hatchable", 0);
        }
    std::printf("  one stat 10 to 20: %d obsidian, 20 to 30: %d; the game holds %ld placed + about %.0f from one pass of its monsters\n", low,
                high, placed, drops);
    CHECK(placed > 1000);
    // Possible but costly: one stat from 20 to 30 takes most of what the whole game has.
    CHECK(high > (placed + long(drops)) * 3 / 4 && high < placed + long(drops));
    // Our qualities are not the original's: no tome or potion in the data has one.
    for (unsigned m = 0; m < d.nonfixed.count(); ++m)
        for (const ItemRecord& r : parse_item_records(d.nonfixed.entry(m)))
            if (r.shape == StatGrowth::kTomeShape || r.shape == StatGrowth::kElixirShape) CHECK(r.quality < StatGrowth::kQualityBase);
    for (unsigned m = 0; m < d.fixed.count(); ++m)
        for (const ItemRecord& r : parse_item_records(d.fixed.entry(m)))
            if (r.shape == StatGrowth::kTomeShape || r.shape == StatGrowth::kElixirShape) CHECK(r.quality < StatGrowth::kQualityBase);
    // The trainers' dialogue files: no problems, the entries are Leveling's.
    NpcDialog& nd = *u.w().dialog;
    CHECK(nd.find_def(5) >= 0 && nd.find_def(33) >= 0);
    for (const std::string& pr : nd.problems()) {
        std::printf("  problem: %s\n", pr.c_str());
        CHECK(pr.find("darion") != 0 && pr.find("mythran") != 0);
    }
}

// Every placement on its real map: free, nothing stamped over, and reached by the flood.
void test_places(const GameData& d) {
    std::printf("places: free spots, nothing stamped over, reached from the map's arrivals (real collision tests)\n");
    json j = read_json(g_data + "/stat_growth.json");
    std::map<std::string, int> min_climbs;
    for (const json& o : j["ordeals"]) min_climbs[o.value("id", std::string())] = o.value("min_climbs", 1);
    Ui base(d, true, false);
    std::map<unsigned, std::vector<const GrowthItem*>> items;
    std::map<unsigned, std::vector<const GrowthOrdeal*>> ordeals;
    for (const GrowthItem& it : base.g().items) items[it.map].push_back(&it);
    for (const GrowthOrdeal& o : base.g().ordeals) ordeals[o.map].push_back(&o);
    std::set<unsigned> maps;
    for (auto& e : items) maps.insert(e.first);
    for (auto& e : ordeals) maps.insert(e.first);
    for (unsigned map : maps) {
        Ui u(d, false, false);  // off: nothing of ours in the way
        u.frames(5);
        u.place(map, 1000, 1000, 0);
        expand_all(u);
        World& w = u.w();
        auto arr = arrivals(w);
        CHECK(!arr.empty());
        if (arr.empty()) continue;
        u.place(map, arr[0][0], arr[0][1], arr[0][2]);
        expand_all(u);
        for (const GrowthItem* it : items[map]) {
            uint16_t bl = 0;
            bool free = clear_spot(w, it->shape, it->frame, it->x, it->y, it->z, 0, &bl);
            if (!free) std::printf("  %s: not free (item %u shape %d)\n", it->id.c_str(), bl, w.item(bl) ? w.item(bl)->shape : -1);
            CHECK(free);
        }
        Flood f(w, 120);
        for (auto& a : arr) f.seed(a[0], a[1], a[2], 32);
        f.run(32, 900000);
        for (const GrowthItem* it : items[map]) {
            int c = f.reached(it->x, it->y, it->z, 48);
            std::printf("  map %2u %-26s (%5d,%5d,%3d): free, stood beside after %d climb(s) from an arrival\n", map, it->id.c_str(), it->x,
                        it->y, it->z, c);
            CHECK(c >= 0 && c <= 1);
        }
        for (const GrowthOrdeal* o : ordeals[map]) {
            int lo = -1;
            size_t in = 0;
            for (auto& e : f.seen) {
                auto [x, y, z] = e.first;
                if (x < o->x0 || x > o->x1 || y < o->y0 || y > o->y1 || z < o->z0 || z > o->z1) continue;
                ++in;
                if (lo < 0 || e.second < lo) lo = e.second;
            }
            std::printf("  map %2u %-26s: %zu spots inside, the easiest needs %d climbs (wanted %d)\n", map, o->id.c_str(), in, lo, min_climbs[o->id]);
            CHECK(in > 0);
            CHECK(lo >= min_climbs[o->id]);
        }
    }
}

void test_spawn(const GameData& d) {
    std::printf("spawn: the items appear on their map once, with Leveling on only\n");
    Ui u(d, true, false);
    u.frames(30);
    StatGrowth& g = u.g();
    const GrowthItem* e = item_def(g, "elixir_ghost_house");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(!g.spawned.count(e->id));
    u.place(e->map, e->x + 64, e->y + 64, e->z);
    u.frames(3);
    CHECK(g.spawned.count(e->id));
    auto count = [&](World& w) {
        int n = 0;
        for (size_t i = 256; i < 32768; ++i)
            if (const Item* it = w.item(uint16_t(i)); it && it->shape == e->shape && it->quality == e->quality) {
                ++n;
                CHECK(it->x == e->x && it->y == e->y && it->z == e->z && it->chunk >= 0);
            }
        return n;
    };
    CHECK_EQ(count(u.w()), 1);
    // Away and back: still one (it was saved with the map).
    u.place(3, 13000, 13000, 0);
    u.frames(3);
    u.place(e->map, e->x + 64, e->y + 64, e->z);
    u.frames(3);
    CHECK_EQ(count(u.w()), 1);
    // Leveling off: nothing appears.
    Ui off(d, false, false);
    off.frames(30);
    off.place(e->map, e->x + 64, e->y + 64, e->z);
    off.frames(3);
    CHECK_EQ(count(off.w()), 0);
    CHECK(off.g().spawned.empty());
}

bool talk_to(Ui& u, uint16_t npc, bool training_hours = false) {
    if (npc == 5 && u.w().map_number_now() == 3) {
        // After the execution his schedule takes him into town (maps 39, 41): go there first.
        u.w().teleport_actor(1, 15583, 13700, 48, 41);
        u.frames(30);
    }
    u.near_npc(npc);
    if (const Actor* n = u.w().actor(npc))
        std::printf("  npc %u on map %u at %d,%d,%d (the Avatar on %u)\n", npc, n->mapnum, n->x, n->y, n->z, u.w().map_number_now());
    if (training_hours) u.set(473, 4, 1);  // his schedule's training hours: his own answer is in the menu
    u.w().call_event(npc, kEvUse);
    return u.wait_menu();
}

void test_train(const GameData& d) {
    std::printf("train: Darion's training through his real menu, for the Avatar and a companion, paid in obsidian\n");
    Ui u(d, true);
    u.frames(40);
    StatGrowth& g = u.g();
    u.set(89, 1, 1);  // the execution: his schedule runs
    u.p().join(u.idx("brannoc"));
    u.frames(5);
    Actor* br = u.a("brannoc");
    give(u.w(), 1, StatGrowth::kMoneyShape, 0, 900);
    give(u.w(), br->id, StatGrowth::kMoneyShape, 0, 30);
    CHECK_EQ(money(u, 1), 900);
    CHECK_EQ(money(u, br->id), 30);
    u.frames(2);
    CHECK(talk_to(u, 5, true));
    std::printf("  his menu: %s\n", u.joined().c_str());
    const uint16_t own = u.w().ask_menu ? u.w().ask_menu->pid : 0;
    int e = u.index_of("I need training.");
    CHECK(e >= 0);
    int originals = 0;
    for (const std::string& m : u.menu()) originals += m == "I need training. " ? 1 : 0;
    CHECK_EQ(originals, 0);  // the sparring session is not offered with Leveling on
    bool hid = false;
    for (const std::string& l : u.w().dialog->log) hid = hid || l.find("darion hid") == 0;
    CHECK(hid);
    if (e < 0) return;
    u.w().choose_answer(unsigned(e));
    CHECK(u.wait_menu());
    std::printf("  who: %s\n", u.joined().c_str());
    int b = u.index_of("Train Brannoc.");
    CHECK(u.index_of("Train me.") >= 0 && b >= 0);
    if (b < 0) return;
    u.w().choose_answer(unsigned(b));
    CHECK(u.wait_menu());
    std::printf("  what: %s\n", u.joined().c_str());
    const int s0 = br->str, p = g.price(s0);
    int st = u.index_of("Strength, " + std::to_string(p) + " obsidian.");
    CHECK(st >= 0 && u.index_of("Dexterity, " + std::to_string(g.price(br->dex)) + " obsidian.") >= 0 && u.index_of("Back.") >= 0);
    if (st < 0) return;
    u.w().choose_answer(unsigned(st));
    CHECK(u.wait_menu());
    CHECK(u.w().ask_menu && u.w().ask_menu->pid == own);  // back in his own menu
    CHECK_EQ(br->str, s0 + 1);
    CHECK_EQ(money(u, br->id), 0);            // his own coins first
    CHECK_EQ(money(u, 1), 900 - (p - 30));     // then the Avatar's
    CHECK_EQ(g.trained[br->id][kGrowStr], 1);
    // The Avatar's turn; then too poor; then at the cap.
    const int ad = u.av()->dex, pd = g.price(ad);
    CHECK(g.dialog_action(5, "session:1:1").say.size() == 1);
    CHECK_EQ(u.av()->dex, ad + 1);
    CHECK_EQ(money(u, 1), 900 - (p - 30) - pd);
    const int left = money(u, 1);
    give(u.w(), 1, StatGrowth::kMoneyShape, 0, 0);
    u.av()->str = 29;
    DialogAction poor = g.dialog_action(5, "session:1:0");
    if (left >= g.price(29)) {
        CHECK_EQ(u.av()->str, 30);
    } else {
        CHECK_EQ(u.av()->str, 29);
        CHECK(!poor.say.empty() && poor.say[0].find(std::to_string(g.price(29))) != std::string::npos);
        CHECK_EQ(money(u, 1), left);
        u.av()->str = 30;
    }
    DialogAction capped = g.dialog_action(5, "trainee:1");
    CHECK_EQ(capped.topics.size(), size_t(1));  // only Dexterity is left
    // Someone not walking with him cannot be trained.
    u.p().part_ways(u.idx("brannoc"));
    u.frames(3);
    const int bs = br->str;
    give(u.w(), 1, StatGrowth::kMoneyShape, 0, 900);
    g.dialog_action(5, "session:" + std::to_string(br->id) + ":0");
    CHECK_EQ(br->str, bs);
    u.wait_idle(200);
    u.shot("growth_darion_menu");
}

void test_mythran(const GameData& d) {
    std::printf("mythran: his lessons for INT in his real menu\n");
    Ui u(d, true, false);
    u.frames(40);
    u.set(89, 1, 1);
    u.set(284, 1, 1);  // met him before: his later-visits menu
    give(u.w(), 1, StatGrowth::kMoneyShape, 0, 500);
    CHECK(talk_to(u, 33));
    std::printf("  his menu: %s\n", u.joined().c_str());
    int e = u.index_of("Sharpen my mind.");
    CHECK(e >= 0);
    if (e < 0) return;
    u.w().choose_answer(unsigned(e));
    CHECK(u.wait_menu());
    std::printf("  then: %s\n", u.joined().c_str());
    const int i0 = u.av()->intel, p = u.g().price(i0);
    int s = u.index_of("Intelligence, " + std::to_string(p) + " obsidian.");
    CHECK(s >= 0 && u.index_of("Strength, " + std::to_string(u.g().price(u.av()->str)) + " obsidian.") < 0);
    if (s < 0) return;
    u.w().choose_answer(unsigned(s));
    CHECK(u.wait_menu());
    CHECK_EQ(u.av()->intel, i0 + 1);
    CHECK_EQ(u.av()->mana, 2 * (i0 + 1));  // max mana rose, and the mana with it (he was full)
    CHECK_EQ(money(u, 1), 500 - p);
}

void test_off(const GameData& d) {
    std::printf("off: Darion's original menu, no trainers, no gains\n");
    Ui u(d, false);
    u.frames(40);
    u.set(89, 1, 1);
    u.frames(2);
    CHECK(talk_to(u, 5, true));
    std::printf("  his menu: %s\n", u.joined().c_str());
    CHECK(u.index_of("I need training. ") >= 0);  // his own answer
    CHECK(u.index_of("I need training.") < 0);    // not ours
    StatGrowth& g = u.g();
    CHECK(g.dialog_action(5, "session:1:0").say.empty());
    // A tome or an elixir (from a game played with it on) does nothing.
    const GrowthItem* t = item_def(g, "tome_fire");
    const GrowthItem* e = item_def(g, "elixir_pit");
    u.w().end_conversation("test");
    u.wait_idle(300);
    u.av()->intel = 30;
    const int s = u.av()->dex;
    uint16_t ti = give_ours(u.w(), *t, 1), ei = give_ours(u.w(), *e, 1);
    u.w().call_event(ti, kEvUse);
    u.frames(3);
    if (u.w().reader()) u.w().reader()->close();
    u.w().call_event(ei, kEvUse);
    u.frames(3);
    CHECK_EQ(u.av()->intel, 30);
    CHECK_EQ(u.av()->dex, s);
    CHECK(u.w().item(ei) != nullptr);
    CHECK(g.used.empty());
    // An ordeal's spot gives nothing.
    const GrowthOrdeal& o = g.ordeals[0];
    u.place(o.map, (o.x0 + o.x1) / 2 | 31, (o.y0 + o.y1) / 2 | 31, o.z0);
    u.frames(12);
    CHECK(g.ordeals_done.empty());
}

void test_tomes(const GameData& d) {
    std::printf("tomes: beyond the Avatar below INT 30 and kept; +%d once at 30; a companion reads one given to them\n", 2);
    Ui u(d, true);
    u.frames(40);
    StatGrowth& g = u.g();
    const GrowthItem& t = *item_def(g, "tome_cove");
    uint16_t id = give_ours(u.w(), t, 1);
    // Look: its own name.
    u.w().call_event(id, kEvLook);
    u.frames(2);
    bool named = false;
    for (const GameLog::Line& l : u.w().game_log.lines(GameLog::kMessages)) named = named || l.text.find(t.name) != std::string::npos;
    CHECK(!u.w().barks.empty());
    CHECK(named);
    u.w().barks.clear();
    // INT 10: beyond him, kept.
    CHECK_EQ(u.av()->intel, 10);
    u.w().call_event(id, kEvUse);
    u.frames(3);
    CHECK(u.w().reader() != nullptr);
    CHECK_EQ(u.av()->intel, 10);
    CHECK(u.w().item(id) != nullptr);
    CHECK(!g.used.count(t.id));
    u.shot("growth_tome_beyond");
    if (u.w().reader()) u.w().reader()->close();
    u.frames(2);
    // INT 30: +2, once.
    u.av()->intel = 30;
    u.w().call_event(id, kEvUse);
    u.frames(3);
    CHECK_EQ(u.av()->intel, 30 + t.gain);
    CHECK(g.used.count(t.id));
    CHECK(u.w().item(id) != nullptr);  // the book stays, spent
    bool logged = false;
    for (const GameLog::Line& l : u.w().game_log.lines(GameLog::kMessages)) logged = logged || l.text.find("INT rises to 32") != std::string::npos;
    CHECK(logged);
    u.shot("growth_tome_read");
    if (u.w().reader()) u.w().reader()->close();
    u.frames(2);
    u.w().call_event(id, kEvUse);
    u.frames(3);
    CHECK_EQ(u.av()->intel, 30 + t.gain);
    if (u.w().reader()) u.w().reader()->close();
    // A companion: given one below 30, they say so and keep it; at 30 they read it.
    u.p().join(u.idx("kalvax"));
    u.frames(5);
    Actor* k = u.a("kalvax");
    const GrowthItem& t2 = *item_def(g, "tome_fortress");
    uint16_t id2 = give_ours(u.w(), t2, k->id);
    const int ki = k->intel;
    CHECK(ki < 30);
    g.on_given(id2, k->id);
    CHECK_EQ(k->intel, ki);
    CHECK(u.w().item(id2) && u.w().item(id2)->parent);
    k->intel = 30;
    u.w().call_event(id2, kEvUse);  // used from his pack: he reads it
    u.frames(3);
    CHECK_EQ(k->intel, 30 + t2.gain);
    CHECK_EQ(u.av()->intel, 30 + t.gain);
    CHECK(g.used.count(t2.id));
}

// A pixel of an item (or actor) on screen, false if none shows.
bool pixel_of(Ui& u, uint16_t id, int& px, int& py) {
    const Item* it = u.w().item(id);
    if (!it) return false;
    int sx, sy;
    u.w().screen_point(*it, sx, sy);
    for (int dy = -40; dy <= 4; ++dy)
        for (int dx = -16; dx <= 16; ++dx)
            if (u.gm.pick(sx + dx, sy + dy) == id) { px = sx + dx; py = sy + dy; return true; }
    return false;
}

// The player drags an item lying in the world onto someone (the real mouse path).
bool drag_onto(Ui& u, uint16_t item, uint16_t target) {
    int ix, iy, tx, ty;
    u.frames(2);
    if (!pixel_of(u, item, ix, iy) || !pixel_of(u, target, tx, ty)) return false;
    auto mv = [&](int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; u.ev(e); };
    auto bt = [&](bool down) {
        InputEvent e; e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp; e.button = 1;
        e.x = u.gm.mouse_x(); e.y = u.gm.mouse_y(); u.ev(e);
    };
    mv(ix, iy); bt(true); u.frames(1);
    mv(ix + 10, iy + 2); u.frames(1);
    mv(tx, ty); u.frames(1);
    bt(false); u.frames(2);
    return true;
}

void test_give(const GameData& d) {
    std::printf("give: an elixir dragged from the ground onto a companion: they drink it\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("brannoc"));
    u.frames(30);
    StatGrowth& g = u.g();
    Actor* b = u.a("brannoc");
    Actor* av = u.av();
    // Brannoc beside him, the bottle on the ground between.
    u.w().move_to(b->id, av->x + 96, av->y, av->z, false);
    u.p().state[size_t(u.idx("brannoc"))].mode = CompanionState::kWait;
    const GrowthItem& e = *item_def(g, "elixir_catacombs_walk");  // STR
    uint16_t id = u.w().create_item(e.shape, e.frame);
    u.w().item(id)->quality = e.quality;
    int x = av->x + 40, y = av->y + 40;
    World::Placement pl = u.w().can_exist_shape(e.shape, x, y, av->z, id);
    CHECK(pl.valid);
    u.w().move_to(id, x, y, pl.supported ? av->z : pl.support_z, false);
    const int s = b->str;
    CHECK(drag_onto(u, id, b->id));
    CHECK_EQ(b->str, s + 1);
    CHECK(u.w().item(id) == nullptr);
    CHECK(g.used.count(e.id));
}

void test_elixirs(const GameData& d) {
    std::printf("elixirs: +1 once, the bottle gone; a companion drinks one given to them\n");
    Ui u(d, true);
    u.frames(40);
    StatGrowth& g = u.g();
    const GrowthItem& e = *item_def(g, "elixir_cove");      // STR
    const GrowthItem& e2 = *item_def(g, "elixir_ancients"); // DEX
    uint16_t id = give_ours(u.w(), e, 1);
    const int s = u.av()->str, hp = u.av()->hp;
    u.w().call_event(id, kEvUse);
    u.frames(2);
    CHECK_EQ(u.av()->str, s + 1);
    CHECK_EQ(u.av()->hp, hp + 2);  // max HP is 2 x STR
    CHECK(u.w().item(id) == nullptr);
    CHECK(g.used.count(e.id));
    bool floated = false;
    for (const auto& f : u.l().floats) floated = floated || (f.actor == 1 && f.text == "+1 STR");
    CHECK(floated);
    u.p().join(u.idx("corvin"));
    u.frames(5);
    Actor* c = u.a("corvin");
    const int cd = c->dex;
    uint16_t id2 = give_ours(u.w(), e2, c->id);
    g.on_given(id2, c->id);
    CHECK_EQ(c->dex, cd + 1);
    CHECK(u.w().item(id2) == nullptr);
}

void test_ordeals(const GameData& d) {
    std::printf("ordeals: standing there the first time: +2, a log line and floating text; once\n");
    Ui u(d, true, false);
    u.l().quest_xp.clear();  // no level-ups in the way of the counts
    u.frames(30);
    StatGrowth& g = u.g();
    for (const GrowthOrdeal& o : g.ordeals) {
        Actor* av = u.av();
        const int before = StatGrowth::stat_value(*av, o.stat);
        // Somewhere inside the box he can stand.
        bool stood = false;
        for (int y = o.y0 | 31; y <= o.y1 && !stood; y += 32)
            for (int x = o.x0 | 31; x <= o.x1 && !stood; x += 32) {
                u.place(o.map, x, y, o.z1);
                World::Placement p = u.w().can_exist(x, y, o.z1, 32, 32, 40, u.w().shape_flags(*av), 1);
                if (p.support_z >= o.z0 && p.support_z <= o.z1) {
                    u.w().move_to(1, x, y, p.support_z, false);
                    stood = true;
                }
            }
        CHECK(stood);
        u.frames(12);
        CHECK(g.ordeals_done.count(o.id));
        CHECK_EQ(StatGrowth::stat_value(*av, o.stat), before + o.gain);
        bool logged = false;
        for (const GameLog::Line& l : u.w().game_log.lines(GameLog::kMessages)) logged = logged || l.text == o.line;
        CHECK(logged);
        u.shot("growth_" + o.id);
        u.frames(12);
        CHECK_EQ(StatGrowth::stat_value(*av, o.stat), before + o.gain);
    }
}

bool do_step(World& w, const CritPath& cp, const std::string& id) {
    int k = cp.step_index(id);
    if (k < 0) return false;
    const CritCond* c = &cp.steps[size_t(k)].done_when;
    while (c->kind == CritCond::kAll && !c->sub.empty()) c = &c->sub[0];
    if (c->kind != CritCond::kGlobal) return false;
    w.vm().set_global(unsigned(c->a), unsigned(c->b), c->cmp == CritCond::kEq || c->cmp == CritCond::kGe ? uint16_t(c->value) : 1);
    return true;
}

void test_quests(const GameData& d) {
    std::printf("quests: companions walking with him get a point in their main stat; the Avatar nothing; once\n");
    Ui u(d, true);
    u.l().quest_xp.clear();  // no level-ups in the way of the counts
    u.frames(40);
    StatGrowth& g = u.g();
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_data + "/critpath.json", err));
    u.p().join(u.idx("brannoc"));
    u.p().join(u.idx("kalvax"));
    u.p().join(u.idx("corvin"));
    u.frames(5);
    u.p().state[size_t(u.idx("corvin"))].mode = CompanionState::kWait;  // told to wait: nothing
    Actor *b = u.a("brannoc"), *k = u.a("kalvax"), *c = u.a("corvin"), *av = u.av();
    const int bs = b->str, ki = k->intel, cd = c->dex, as = av->str, ad = av->dex, ai = av->intel;
    CHECK(do_step(u.w(), cp, "meet_lithos"));
    u.frames(35);
    CHECK(g.steps_paid.count("meet_lithos"));
    CHECK(g.steps_paid.count("necromancer_6"));  // the earlier milestone counts as done, and pays too
    CHECK_EQ(b->str, bs + 2);
    CHECK_EQ(k->intel, ki + 2);
    CHECK_EQ(c->dex, cd);
    CHECK(av->str == as && av->dex == ad && av->intel == ai);
    u.frames(65);
    CHECK_EQ(b->str, bs + 2);
    // A save from 0.15.0 (no LVL4 section): what is done never pays, what comes after does.
    std::vector<uint8_t> bytes = save_bytes(u.w());
    static const char t4[4] = {'L', 'V', 'L', '4'}, te[4] = {'E', 'N', 'D', '.'};
    auto s4 = std::find_end(bytes.begin(), bytes.end(), t4, t4 + 4), se = std::find_end(bytes.begin(), bytes.end(), te, te + 4);
    CHECK(s4 != bytes.end() && s4 < se);
    bytes.erase(s4, se);
    Ui v(d, true);
    v.l().quest_xp.clear();
    v.frames(5);
    SaveReader r(bytes.data(), bytes.size());
    CHECK(v.w().load_game(r));
    CHECK(!v.g().steps_adopted);
    Actor* vb = v.a("brannoc");
    const int vbs = vb->str;
    CHECK(do_step(v.w(), cp, "trial"));  // done before the first check: it counts as old
    v.frames(35);
    CHECK(v.g().steps_adopted);
    CHECK_EQ(vb->str, vbs);
    CHECK(do_step(v.w(), cp, "ancient_ones"));
    v.frames(35);
    CHECK_EQ(vb->str, vbs + 1);
}

void test_saves(const GameData& d) {
    std::printf("saves: the state round-trips; an older save loads\n");
    Ui u(d, true, false);
    u.frames(30);
    StatGrowth& g = u.g();
    const GrowthItem* e = item_def(g, "elixir_ghost_house");
    u.place(e->map, e->x + 64, e->y + 64, e->z);
    u.frames(3);
    g.used.insert("tome_cove");
    g.ordeals_done.insert("ordeal_stone_cove");
    g.trained[1] = {2, 1, 0};
    std::vector<uint8_t> b = save_bytes(u.w());
    Ui v(d, true, false);
    v.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(v.w().load_game(r));
    StatGrowth& h = v.g();
    CHECK(h.spawned == g.spawned && h.used == g.used && h.ordeals_done == g.ordeals_done && h.steps_paid == g.steps_paid);
    CHECK(h.trained == g.trained && h.steps_adopted);
    v.frames(3);
    // The spawned item came back with the map, and is not made again.
    int n = 0;
    for (size_t i = 256; i < 32768; ++i)
        if (const Item* it = v.w().item(uint16_t(i)); it && it->shape == e->shape && it->quality == e->quality) ++n;
    CHECK_EQ(n, 1);
    std::vector<uint8_t> b2 = save_bytes(v.w());
    static const char t[4] = {'L', 'V', 'L', '1'};
    auto tail = [&](const std::vector<uint8_t>& x) { return std::vector<uint8_t>(std::find_end(x.begin(), x.end(), t, t + 4), x.end()); };
    CHECK(tail(b) == tail(b2));
    // A version-5 save (before levels) still loads.
    std::vector<uint8_t> old = b;
    static const char te[4] = {'E', 'N', 'D', '.'};
    auto l1 = std::find_end(old.begin(), old.end(), t, t + 4), en = std::find_end(old.begin(), old.end(), te, te + 4);
    old.erase(l1, en);
    old[8] = 5;
    Ui o(d, true, false);
    o.frames(5);
    SaveReader ro(old.data(), old.size());
    CHECK(o.w().load_game(ro));
    CHECK(o.g().spawned.empty());
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_growth_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto arg = [&](int k) { return argc > 5 + k ? std::atoi(argv[5 + k]) : 0; };
    if (only == "probe") { probe(d, unsigned(arg(1)), arg(2), arg(3), arg(4), arg(5) ? arg(5) : 160); return 0; }
    if (only == "anchor") { anchor(d, unsigned(arg(1)), unsigned(arg(2))); return 0; }
    if (only == "cands") { cands(d, unsigned(arg(1)), unsigned(arg(2)), unsigned(arg(3)), argc > 9 ? arg(4) : -1); return 0; }
    if (only == "look" && argc > 12) { look_at(d, unsigned(arg(1)), arg(2), arg(3), arg(4), unsigned(arg(5)), unsigned(arg(6)), argv[12], arg(8), arg(9), arg(10)); return 0; }
    if (only == "climb") { climb(d, unsigned(arg(1))); return 0; }
    if (only == "spots") { spots(d, unsigned(arg(1)), arg(2), arg(3), arg(4), unsigned(arg(5)), unsigned(arg(6))); return 0; }
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("data")) test_data(d);
    if (want("places")) test_places(d);
    if (want("spawn")) test_spawn(d);
    if (want("train")) test_train(d);
    if (want("mythran")) test_mythran(d);
    if (want("off")) test_off(d);
    if (want("tomes")) test_tomes(d);
    if (want("elixirs")) test_elixirs(d);
    if (want("give")) test_give(d);
    if (want("ordeals")) test_ordeals(d);
    if (want("quests")) test_quests(d);
    if (want("saves")) test_saves(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
