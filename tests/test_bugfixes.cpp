// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Original bug fixes (world, physics, audio; NOTES.md "Original bug fixes"): each bug of the
// original game reproduced with the "Original bug fixes" option Off and shown fixed with it On.
//
//   pagan_bugfix_tests GAME_DIR DATA_DIR [SHOT_DIR] [only]   (DATA_DIR: engine/data, for the patches)
//   only: one scenario (guard, summon, moss, lava, void, bounds, stitched, maps), or "scan"
//   (the map scanner: prints every walkable-floor gap and floating / sunk item it finds).
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "play.h"
#include "audio/mixer.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "game/processes.h"
#include "game/world.h"
#include "usecode/uc_machine.h"
#include "usecode/uc_patches.h"
#include "world/current_map.h"
#include "world/world_space.h"

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

std::string g_shots;

// --- 3. The Tenebrae gate guard -------------------------------------------------------------
// The first meeting ("Halt, stranger!"): the guard walks to the Avatar. specs/world/bugfixes.md
// 10: if he is turning on the spot at a patrol marker (a type 0x20A process) when he calls out,
// the turn goes on while he walks. Counts his facing changes while he walks to the Avatar.
struct GuardRun { int turns = 0, moves = 0, frames = 0, turning_after = 0, max_paths = 0; bool talked = false, caught = false; };

GuardRun guard_walk(const GameData& d, bool fixes, bool while_turning) {
    original_bug_fixes() = fixes;
    Play p(d);
    GuardRun r;
    p.warp(11640, 2900, 48, 3);  // out of the challenge egg's (GUARDEGG) reach
    Actor* g = p.w.actor(4);
    if (!g) return r;
    // Wait until his patrol is walking him to a marker (pathfind), or turning him at one (0x20A).
    for (int f = 0; f < 3000; ++f) {
        if (while_turning ? p.w.kernel().count_exact(4, 0x20A) > 0 : p.w.kernel().count_exact(4, kProcPathfind) > 0) {
            r.caught = true;
            break;
        }
        p.step();
    }
    // What GUARDEGG does when he comes near: G[424:1] = 1 ("first meeting pending"), then use NPC 4.
    p.w.vm().set_global(424, 1, 1);
    p.w.call_event(4, kEvUse);
    g = p.w.actor(4);
    int lx = g->x, ly = g->y, ld = g->dir;
    for (int f = 0; f < 900; ++f) {
        p.step();
        if (!(g = p.w.actor(4))) break;
        r.max_paths = std::max(r.max_paths, int(p.w.kernel().count_exact(4, kProcPathfind)));
        if (std::getenv("PAGAN_BUG_PROCS") && f < 120) {
            std::printf("   f%d guard %d,%d dir %d procs:", f, g->x, g->y, g->dir);
            p.w.kernel().for_each([&](Process* q) {
                if (q->item == 4 && q->live()) std::printf(" %s/%X", q->name(), q->type);
            });
            std::printf("\n");
        }
        // A turn process alive while he is walking to the Avatar (his pathfind is running).
        r.turning_after += p.w.kernel().count_exact(4, 0x20A) > 0 && p.w.kernel().count_exact(4, kProcPathfind) > 0;
        bool moved = g->x != lx || g->y != ly;
        if (moved) ++r.moves;
        if (g->dir != ld && p.w.stasis && !p.w.ask_menu) {
            ++r.turns;
            if (std::getenv("PAGAN_BUG_VERBOSE")) std::printf("   f%d guard %d,%d dir %d -> %d\n", f, g->x, g->y, ld, g->dir);
        }
        if (p.w.ask_menu) { r.talked = true; break; }
        lx = g->x; ly = g->y; ld = g->dir;
        r.frames = f;
    }
    return r;
}

void test_guard(const GameData& d) {
    std::printf("gate guard\n");
    for (bool turning : {false, true})
        for (bool fixes : {false, true}) {
            GuardRun r = guard_walk(d, fixes, turning);
            std::printf("  called out while %s, fixes %s: caught %d; turn process alive %d frames after; at most %d pathfind(s); "
                        "%d facing changes, %d moving frames, talked %d\n",
                        turning ? "turning" : "walking", fixes ? "on" : "off", int(r.caught), r.turning_after, r.max_paths,
                        r.turns, r.moves, int(r.talked));
            CHECK(r.caught);
            CHECK(r.talked);
            CHECK(r.moves > 0);
            CHECK(r.max_paths <= 1);  // a new pathfind replaces his old one (World::pathfind)
            if (fixes || !turning) CHECK_EQ(r.turning_after, 0);
            else CHECK(r.turning_after > 0);  // the original bug: the turn goes on
            if (fixes) CHECK(r.turns <= 4);
        }
    original_bug_fixes() = true;
}

// --- 5. The catacomb moss curtains ----------------------------------------------------------
// A free, supported spot for the Avatar near (x, y, z): rings out to 256 units, heights z-16..z+16.
bool find_spot(World& w, int x, int y, int z, int& ox, int& oy, int& oz) {
    int xd, yd, zd;
    w.footprint(*w.avatar(), xd, yd, zd);
    for (int r = 0; r <= 256; r += 16)
        for (int dy = -r; dy <= r; dy += 16)
            for (int dx = -r; dx <= r; dx += 16) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                for (int dz : {0, 8, -8, 16, -16}) {
                    World::Placement p = w.can_exist(x + dx, y + dy, z + dz, xd, yd, zd, w.shape_flags(*w.avatar()), 1);
                    if (p.valid && p.supported) { ox = x + dx; oy = y + dy; oz = z + dz; return true; }
                }
            }
    return false;
}

// Map 50 (Upper Catacombs): a body (shape 218 at (2367, 3711, 64)) lies in an alcove behind moss
// curtains (shape 462 at (2367, 3743, 56) and its neighbours). Standing in front of it, can he
// reach it, and can a body be pushed into the alcove?
void test_moss(const GameData& d) {
    std::printf("moss curtains\n");
    Play p(d);
    p.warp(2367, 3800, 64, 50);
    int sx, sy, sz;
    CHECK(find_spot(p.w, 2367, 3790, 64, sx, sy, sz));
    p.w.move_to(1, sx, sy, sz);
    p.run(4);
    uint16_t bone = 0, curtain = 0;
    std::vector<uint16_t> near;
    p.w.area_search(2367, 3740, 128, false, near);
    for (uint16_t k : near)
        if (const Item* it = p.w.item(k)) {
            if (it->shape == 218 && it->x == 2367 && it->y == 3711) bone = k;
            if (it->shape == 462 && it->x == 2367 && it->y == 3743) curtain = k;
        }
    CHECK(bone && curtain);
    std::printf("  avatar at %d,%d,%d map %u; body %u, curtain %u\n", p.av().x, p.av().y, p.av().z,
                p.w.map_number_now(), bone, curtain);
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        bool solid = d.typeflags.get(462).is(ShapeInfo::kSolid);
        bool reach = bone && p.w.can_reach(1, bone, 256);
        // A body pushed into the alcove through the curtain (a swept move of a box the bones' size).
        const Item* b = p.w.item(bone);
        int bx = b ? b->x : 0, by = b ? b->y : 0, bz = b ? b->z : 0;
        uint16_t blocker = 0;
        int frac = 0;
        if (b) {
            p.w.move_to(bone, bx, by + 64, bz + 8);
            frac = p.w.collide_move(bone, bx, by, bz + 8, false, false, &blocker);
            p.w.move_to(bone, bx, by, bz);
        }
        std::printf("  fixes %s: curtain solid %d, body reachable %d, pushed in %d/16384 (blocker %u)\n", fixes ? "on" : "off",
                    int(solid), int(reach), frac, blocker);
        CHECK_EQ(solid, !fixes);
        CHECK_EQ(reach, fixes);
        const Item* bl = p.w.item(blocker);
        CHECK_EQ(bl && bl->shape >= 459 && bl->shape <= 464, !fixes);
        CHECK_EQ(frac == 0x4000, fixes);
        CHECK_EQ(d.typeflags.original(462).is(ShapeInfo::kSolid), true);
    }
    original_bug_fixes() = true;
}

// --- 2. Summon Creature (the Ether spell, ETHRSPEL class 1413 at 0x0225, "Kal Xen") --------------
// Cast at a spot in front of the Avatar (the target cursor answered with a floor tile there).
struct SummonRun { uint16_t id = 0; unsigned shape = 0; bool fast_only = false; int cls = -1; bool gone_after_map = false; };

SummonRun summon(const GameData& d, bool fixes) {
    original_bug_fixes() = fixes;
    Play p(d);
    SummonRun r;
    std::set<uint16_t> before;
    for (uint16_t i = 1; i < 256; ++i) if (p.w.actor(i)) before.insert(i);
    int tx = p.av().x + 96, ty = p.av().y, tz = p.av().z;
    std::vector<uint8_t> args = {uint8_t(tx), uint8_t(tx >> 8), uint8_t(ty), uint8_t(ty >> 8), uint8_t(tz)};
    p.w.auto_ui = false;
    if (std::getenv("PAGAN_BUG_TRACE")) p.w.vm().trace_intrinsics = true;
    // A small marker item (nothing solid) put down at the target point answers the target cursor.
    uint16_t floor = 0;
    for (unsigned sh = 100; sh < 800 && !floor; ++sh) {
        const ShapeInfo& si = d.typeflags.get(sh);
        if (si.flags() == 0 && si.family() == 0 && si.x() == 1 && si.y() == 1 && si.weight() > 0) {
            floor = p.w.create_item(sh, 0);
            if (floor) p.w.move_to(floor, tx, ty, tz);
        }
    }
    // The creature is picked at random and may not fit: cast again until one appears.
    uint16_t cast = 0;
    for (int f = 0; f < 6000 && !r.id; ++f) {
        if (!p.w.kernel().get(cast) || !p.w.kernel().get(cast)->live()) {
            cast = p.w.kernel().add(std::make_unique<UCProcess>(p.w.vm(), 1413, 0x0225, true, std::vector<uint8_t>{1, 0}, args));
        }
        p.step();
        if (p.w.target_pid)
            if (Process* t = p.w.kernel().get(p.w.target_pid)) { t->result = floor; t->terminate(); p.w.target_pid = 0; }
        for (uint16_t i = 1; i < 256; ++i)
            if (p.w.actor(i) && !before.count(i)) { r.id = i; break; }
    }
    p.run(60);
    if (const Actor* a = p.w.actor(r.id)) {
        r.shape = a->shape;
        r.fast_only = a->flags & kItemFastOnly;
        r.cls = p.w.usecode_class(*a);
        // Far away (out of the fast area) and back: a fast-only creature is gone.
        int ax = p.av().x, ay = p.av().y, az = p.av().z;
        unsigned m = p.w.map_number_now();
        p.w.teleport_actor(1, 11532, 1980, 48, 3);
        p.run(30);
        p.w.teleport_actor(1, ax, ay, az, m);
        p.run(30);
        r.gone_after_map = !p.w.actor(r.id) || p.w.actor(r.id)->shape != r.shape;
    }
    p.w.auto_ui = true;
    return r;
}

void test_summon(const GameData& d) {
    std::printf("summon creature\n");
    for (bool fixes : {false, true}) {
        SummonRun r = summon(d, fixes);
        std::printf("  fixes %s: creature %u (shape %u), fast-only %d, its usecode class %d, gone after leaving %d\n",
                    fixes ? "on" : "off", r.id, r.shape, int(r.fast_only), r.cls, int(r.gone_after_map));
        CHECK(r.id != 0);
        CHECK_EQ(r.fast_only, fixes);
        CHECK_EQ(r.cls == int(r.shape), fixes);  // without the flag: no class at all (events.md rule 2)
        CHECK_EQ(r.gone_after_map, fixes);
    }
    original_bug_fixes() = true;
}

// --- 4. Map holes and wrong heights: the scanner ----------------------------------------------
// Every top-level item of a map as loaded (FIXED with the data fixes and the globs expanded,
// then the top-level NONFIXED items), as boxes.
struct ScanBox {
    int x0, y0, x1, y1, z0, z1;  // inclusive x/y cell span in world units; z0..z1
    uint16_t sflags, iflags, shape;
    uint8_t frame;
    int x, y, z;
    bool nonfixed;
};

std::vector<ScanBox> map_boxes(const GameData& d, unsigned map) {
    std::vector<ScanBox> out;
    auto add = [&](const ItemRecord& r, bool nonfixed) {
        if (r.x >= 32768 || r.y >= 32768) return;
        const ShapeInfo& si = d.typeflags.get(r.shape);
        if (si.is(ShapeInfo::kEditor)) return;
        int xd, yd, zd;
        si.footprint(r.flags & kItemFlipped, xd, yd, zd);
        out.push_back({int(r.x) - xd + 1, int(r.y) - yd + 1, int(r.x), int(r.y), int(r.z), int(r.z) + zd, si.flags(), r.flags,
                       r.shape, r.frame, int(r.x), int(r.y), int(r.z), nonfixed});
    };
    auto add_or_expand = [&](const ItemRecord& r, bool nonfixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape; q.frame = g.frame;
                q.x = uint16_t(bx + 2 * g.x + 1); q.y = uint16_t(by + 2 * g.y + 1); q.z = uint8_t(std::min(255, r.z + g.z));
                add(q, nonfixed);
            }
        } else add(r, nonfixed);
    };
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(map));
    if (!std::getenv("PAGAN_SCAN_RAW")) apply_map_fixes(map, fixed);
    for (const ItemRecord& r : fixed) add_or_expand(r, false);
    for (const MapEntry& e : build_item_tree(parse_item_records(d.nonfixed.entry(map)), d.typeflags))
        if (e.parent < 0) add_or_expand(e.rec, true);
    return out;
}

struct Hole {
    unsigned map;
    int x0, y0, x1, y1, h;  // world-unit box of the gap, floor height around it
    int cells;
    int below;              // highest floor under the gap (-1: nothing)
    bool height;            // a patch of floor at the wrong height (not a gap)
    int patch_z;
    int rim = 0, rim_floor = 0, rim_shape = 0;
};

// Floor gaps: a small patch (up to 3x3 floor tiles) where the floor is missing at height h,
// surrounded on every side by floor whose top is exactly h, with nothing else filling it.
// Wrong heights: such a patch whose floor is there but 8..56 units off.
std::vector<Hole> scan_holes(const GameData& d, unsigned map) {
    constexpr int C = 32, N = 1024;
    std::vector<ScanBox> boxes = map_boxes(d, map);
    std::vector<std::vector<int16_t>> land(size_t(N) * N);  // LAND tops per 32-unit cell
    std::vector<std::vector<std::pair<int16_t, int16_t>>> fill(size_t(N) * N);  // z spans of anything else
    std::vector<int16_t> cap(size_t(N) * N, 999);  // lowest solid, non-roof thing above (a rock mass)
    int bx0 = N, by0 = N, bx1 = -1, by1 = -1;
    for (const ScanBox& b : boxes) {
        bool is_land = b.sflags & ShapeInfo::kLand;
        bool fills = b.sflags & (ShapeInfo::kSolid | ShapeInfo::kSea | ShapeInfo::kDamaging | ShapeInfo::kRoof);
        if (!is_land && !fills) continue;
        for (int cy = std::max(0, b.y0 / C); cy <= std::min(N - 1, b.y1 / C); ++cy)
            for (int cx = std::max(0, b.x0 / C); cx <= std::min(N - 1, b.x1 / C); ++cx) {
                size_t i = size_t(cy) * N + size_t(cx);
                if (is_land) land[i].push_back(int16_t(b.z1));
                if (!is_land || b.z1 > b.z0 + 8) fill[i].push_back({int16_t(b.z0), int16_t(b.z1)});
                if ((b.sflags & ShapeInfo::kSolid) && !(b.sflags & (ShapeInfo::kRoof | ShapeInfo::kLand)))
                    cap[i] = std::min(cap[i], int16_t(b.z0));
                bx0 = std::min(bx0, cx); by0 = std::min(by0, cy); bx1 = std::max(bx1, cx); by1 = std::max(by1, cy);
            }
    }
    auto has_land = [&](int cx, int cy, int h) {
        if (cx < 0 || cy < 0 || cx >= N || cy >= N) return false;
        for (int16_t t : land[size_t(cy) * N + size_t(cx)]) if (t == h) return true;
        return false;
    };
    auto covered = [&](int cx, int cy, int h) {  // standing at h is held or filled here
        size_t i = size_t(cy) * N + size_t(cx);
        for (int16_t t : land[i]) if (t >= h - 8 && t <= h + 24) return true;
        for (auto [z0, z1] : fill[i]) if (z1 >= h - 8 && z0 <= h + 24) return true;
        return false;
    };
    std::vector<Hole> out;
    std::set<std::tuple<int, int, int>> seen;
    std::vector<uint8_t> mark(size_t(N) * N, 0);
    for (int cy = std::max(1, by0); cy <= std::min(N - 2, by1); ++cy)
        for (int cx = std::max(1, bx0); cx <= std::min(N - 2, bx1); ++cx) {
            for (int16_t h : land[size_t(cy) * N + size_t(cx)]) {
                // Seeds: the four neighbours of a floor cell at h that are not covered at h.
                static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int sx = cx + dx4[k], sy = cy + dy4[k];
                    if (covered(sx, sy, h) || seen.count({sx, sy, h})) continue;
                    // Flood the uncovered patch (at most 12x12 cells = 3x3 tiles of 128).
                    std::vector<std::pair<int, int>> stack{{sx, sy}}, comp;
                    std::set<std::pair<int, int>> in{{sx, sy}};
                    bool open = false;
                    while (!stack.empty() && !open) {
                        auto [x, y] = stack.back();
                        stack.pop_back();
                        comp.push_back({x, y});
                        if (comp.size() > 144) { open = true; break; }
                        for (int j = 0; j < 4; ++j) {
                            int nx = x + dx4[j], ny = y + dy4[j];
                            if (nx < 0 || ny < 0 || nx >= N || ny >= N) { open = true; break; }
                            if (in.count({nx, ny})) continue;
                            if (covered(nx, ny, h)) {
                                if (!has_land(nx, ny, h)) {  // held by something else: not floor at h
                                    bool solid_wall = false;
                                    for (auto [z0, z1] : fill[size_t(ny) * N + size_t(nx)]) solid_wall |= z1 > h + 8;
                                    if (!solid_wall) { open = true; break; }
                                }
                                continue;
                            }
                            in.insert({nx, ny});
                            stack.push_back({nx, ny});
                        }
                    }
                    for (auto& c : in) seen.insert({c.first, c.second, h});
                    if (open) continue;
                    // Most of its rim must be walkable floor at h (not the walls round a rock mass).
                    int rim = 0, rim_floor = 0;
                    for (auto [x, y] : comp)
                        for (int j = 0; j < 4; ++j) {
                            int nx = x + dx4[j], ny = y + dy4[j];
                            if (in.count({nx, ny})) continue;
                            bool wall = false;
                            for (auto [z0, z1] : fill[size_t(ny) * N + size_t(nx)]) wall |= z1 > h + 8 && z0 <= h + 8;
                            if (wall) continue;
                            ++rim;
                            rim_floor += has_land(nx, ny, h);
                        }
                    if (rim < 4 || rim_floor * 4 < rim * 3) continue;
                    // Under a rock mass (a solid cap above, not a roof): the inside of a cliff.
                    int capped = 0;
                    for (auto [x, y] : comp) {
                        int16_t c0 = cap[size_t(y) * N + size_t(x)];
                        capped += c0 > h + 8 && c0 <= h + 160;
                    }
                    if (capped * 2 > int(comp.size())) continue;
                    Hole hole{map, N, N, -1, -1, h, int(comp.size()), -1, false, -1};
                    hole.rim = rim;
                    hole.rim_floor = rim_floor;
                    {
                        std::map<int, int> sh;
                        for (const ScanBox& b : boxes)
                            if ((b.sflags & ShapeInfo::kLand) && b.z1 == h)
                                for (auto [x, y] : comp)
                                    if (b.x1 >= x * C - C && b.x0 <= x * C + 2 * C - 1 && b.y1 >= y * C - C && b.y0 <= y * C + 2 * C - 1) {
                                        ++sh[b.shape];
                                        break;
                                    }
                        int best = 0;
                        for (auto [k, v] : sh) if (v > best) { best = v; hole.rim_shape = k; }
                    }
                    for (auto [x, y] : comp) {
                        hole.x0 = std::min(hole.x0, x); hole.y0 = std::min(hole.y0, y);
                        hole.x1 = std::max(hole.x1, x); hole.y1 = std::max(hole.y1, y);
                        for (int16_t t : land[size_t(y) * N + size_t(x)]) if (t < h) hole.below = std::max(hole.below, int(t));
                    }
                    // A patch of floor 8..56 off (all its cells): a wrong height, not a gap.
                    std::set<int> pz;
                    for (auto [x, y] : comp)
                        for (int16_t t : land[size_t(y) * N + size_t(x)]) if (std::abs(t - h) <= 56 && t != h) pz.insert(t);
                    hole.x0 *= C; hole.y0 *= C; hole.x1 = hole.x1 * C + C - 1; hole.y1 = hole.y1 * C + C - 1;
                    out.push_back(hole);
                    (void)pz;
                }
            }
        }
    return out;
}

// Floating and sunk items: top-level NONFIXED items that would fall (weight > 0, not HANGING)
// resting 8 or more above whatever is under them, or with their base inside a floor or wall.
struct Float { unsigned map; ScanBox b; int support; bool sunk; uint16_t in_shape; };
std::vector<Float> scan_floating(const GameData& d, unsigned map) {
    std::vector<ScanBox> boxes = map_boxes(d, map);
    std::vector<Float> out;
    for (const ScanBox& it : boxes) {
        if (!it.nonfixed || (it.iflags & 0x1000) || d.typeflags.get(it.shape).weight() == 0) continue;
        if (it.sflags & ShapeInfo::kFixed) continue;
        if (it.x > 32767 || it.y > 32767) continue;
        int support = -1;
        uint16_t in_shape = 0;
        bool sunk = false;
        for (const ScanBox& o : boxes) {
            if (&o == &it || !(o.sflags & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof))) continue;
            if (o.x1 < it.x0 || o.x0 > it.x1 || o.y1 < it.y0 || o.y0 > it.y1) continue;
            if (o.z1 <= it.z0) support = std::max(support, o.z1);
            else if (o.z0 < it.z0 && o.z1 > it.z0 && (o.sflags & ShapeInfo::kLand) && o.z1 - it.z0 >= 8 &&
                     o.x0 <= it.x0 && o.x1 >= it.x1 && o.y0 <= it.y0 && o.y1 >= it.y1) { sunk = true; in_shape = o.shape; }
        }
        if (sunk || (it.z0 > 0 && it.z0 - std::max(support, 0) >= 8))
            out.push_back({map, it, support, sunk, in_shape});
    }
    return out;
}

void scan_maps(const GameData& d) {
    if (std::getenv("PAGAN_SCAN_FLOAT")) {
        for (unsigned m : CurrentMap::populated_maps(d))
            for (const Float& f : scan_floating(d, m))
                std::printf("%s map %u shape %u:%u at %d,%d,%d  support top %d%s%u\n", f.sunk ? "sunk" : "float", m, f.b.shape,
                            f.b.frame, f.b.x, f.b.y, f.b.z, f.support, f.sunk ? "  inside shape " : "", f.sunk ? f.in_shape : 0);
        return;
    }
    size_t total = 0;
    for (unsigned m : CurrentMap::populated_maps(d)) {
        std::vector<Hole> hs = scan_holes(d, m);
        for (const Hole& h : hs)
            std::printf("hole map %u  x %d..%d y %d..%d  floor z %d  cells %d  below %d  rim %d/%d  shape %d\n", h.map, h.x0,
                        h.x1, h.y0, h.y1, h.h, h.cells, h.below, h.rim_floor, h.rim, h.rim_shape);
        total += hs.size();
    }
    std::printf("%zu holes\n", total);
}


// --- 1. Falling out of the world -------------------------------------------------------------
// specs/world/bugfixes.md 15. Three ways out: a hole in a high floor (a fall onto nothing, at
// z 0, from 104 or more kills him), walking off the edge of a map's scenery into empty space
// (stuck at the bottom: only Recall gets him out), and a step past the edge of the chunk grid
// (x or y past 32767: in no chunk at all). Stitched mode adds seams and segment edges.
struct VoidRun { int x = 0, y = 0, z = 0; bool alive = false, in_world = false; unsigned rescues = 0; int start[3] = {0, 0, 0}; };

// A floor tile with its top at 104 or more and nothing under it, on map m: (x, y, top) of its centre.
bool high_floor(const GameData& d, unsigned m, int& cx, int& cy, int& top) {
    std::vector<ScanBox> boxes = map_boxes(d, m);
    for (const ScanBox& b : boxes) {
        if (!(b.sflags & ShapeInfo::kLand) || b.z1 != b.z0 || b.z1 < 112 || b.x1 - b.x0 < 127 || b.y1 - b.y0 < 127) continue;
        bool under = false, over = false;
        for (const ScanBox& o : boxes) {
            if (&o == &b || o.x1 < b.x0 - 64 || o.x0 > b.x1 + 64 || o.y1 < b.y0 - 64 || o.y0 > b.y1 + 64) continue;
            if (o.z1 < b.z1 && o.x1 >= b.x0 && o.x0 <= b.x1 && o.y1 >= b.y0 && o.y0 <= b.y1) under = true;
            if (o.z0 >= b.z1 && o.z0 < b.z1 + 48 && o.x1 >= b.x0 && o.x0 <= b.x1 && o.y1 >= b.y0 && o.y0 <= b.y1) over = true;
        }
        if (under || over) continue;
        cx = (b.x0 + b.x1) / 2; cy = (b.y0 + b.y1) / 2; top = b.z1;
        return true;
    }
    return false;
}

// Stands him on the tile at (cx, cy, top), takes the floor away under him, waits.
VoidRun fall_through(const GameData& d, bool fixes, bool stitched, unsigned m, int cx, int cy, int top) {
    original_bug_fixes() = fixes;
    Play p(d);
    if (stitched) CHECK(p.w.set_world_mode(true, std::string(PAGAN_SOURCE_DIR) + "/data/world_layout.json"));
    VoidRun r;
    p.warp(cx + 32, cy + 32, top, m);
    p.w.kernel().kill(1, kProcAny);
    p.av().set(Actor::kAnimLock, false);
    p.w.move_to(1, cx + 16, cy + 16, top);
    p.run(20);
    r.start[0] = p.av().x; r.start[1] = p.av().y; r.start[2] = p.av().z;
    // The floor goes (as a missing tile): every floor item under his footprint.
    int xd, yd, zd;
    p.w.footprint(p.av(), xd, yd, zd);
    std::vector<uint16_t> near;
    p.w.area_search(p.av().x, p.av().y, 256, false, near);
    for (uint16_t k : near)
        if (const Item* it = p.w.item(k); it && k >= 256) {
            const ShapeInfo& si = d.typeflags.get(it->shape);
            int ixd, iyd, izd;
            si.footprint(it->flags & kItemFlipped, ixd, iyd, izd);
            if ((si.flags() & (ShapeInfo::kLand | ShapeInfo::kSolid | ShapeInfo::kRoof)) && it->z + izd <= top &&
                it->x - ixd < p.av().x && it->x > p.av().x - xd && it->y - iyd < p.av().y && it->y > p.av().y - yd)
                p.w.destroy_now(k);
        }
    p.w.fall(1);
    p.run(240);
    const Actor* a = p.w.avatar();
    r.x = a->x; r.y = a->y; r.z = a->z;
    r.alive = !a->is(Actor::kDead);
    r.in_world = a->chunk >= 0;
    r.rescues = p.w.void_rescue.rescues;
    return r;
}

// Walks him off the edge of map m's scenery in direction `dir` (from a floor cell at the edge).
// want_partner: (stitched) a seam partner has ground beyond the edge instead of nothing.
bool scenery_edge(World& w, const GameData& d, unsigned m, int dir, int& ex, int& ey, int& ez, bool want_partner = false) {
    MapCoverage c = compute_coverage(d, m);
    WorldSpace* ws = w.stitch.get();
    for (int y = 1024; y < 31744; y += 64)
        for (int x = 1024; x < 31744; x += 64) {
            if (!c.land(x, y)) continue;
            bool clear = true;
            int partner_land = 0;
            for (int k = 1; k <= 6 && clear; ++k) {
                int qx = x + dir_x(dir) * 64 * k, qy = y + dir_y(dir) * 64 * k;
                for (int s = -1; s <= 1 && clear; ++s) {
                    int sx = qx + dir_y(dir) * 64 * s, sy = qy - dir_x(dir) * 64 * s;
                    clear = !c.any(sx, sy);
                    if (clear && ws)
                        for (unsigned pm : ws->seam_partners(m)) {
                            int32_t wp[3];
                            ws->to_world(m, sx, sy, 48, wp);
                            int lp[3];
                            ws->to_local(pm, wp, lp);
                            if (ws->coverage(pm).any(lp[0], lp[1])) {
                                if (!want_partner) clear = false;
                                else if (ws->coverage(pm).land(lp[0], lp[1])) ++partner_land;
                            }
                        }
                }
            }
            if (!clear || (want_partner && partner_land < 18)) continue;
            int sx, sy, sz;
            if (find_spot(w, x + 31, y + 31, 48, sx, sy, sz) && std::abs(sx - x) < 128 && std::abs(sy - y) < 128) {
                ex = sx; ey = sy; ez = sz;
                return true;
            }
        }
    return false;
}

VoidRun walk_off(const GameData& d, bool fixes, bool stitched, unsigned m, int dir, bool to_partner = false,
                 unsigned* end_map = nullptr) {
    original_bug_fixes() = fixes;
    Play p(d);
    if (stitched) CHECK(p.w.set_world_mode(true, std::string(PAGAN_SOURCE_DIR) + "/data/world_layout.json"));
    VoidRun r;
    p.warp(100, 100, 0, m);
    int ex, ey, ez;
    bool found = false;
    for (int k = 0; k < 8 && !found; k += 2)
        if (scenery_edge(p.w, d, m, (dir + k) & 7, ex, ey, ez, to_partner)) { found = true; dir = (dir + k) & 7; }
    if (!found) { std::printf("  no scenery edge on map %u\n", m); return r; }
    p.w.kernel().kill(1, kProcAny);
    p.av().set(Actor::kAnimLock, false);
    p.w.move_to(1, ex, ey, ez);
    p.run(10);
    r.start[0] = p.av().x; r.start[1] = p.av().y; r.start[2] = p.av().z;
    std::printf("  map %u: the edge of its scenery at %d,%d,%d, walking in direction %d\n", m, ex, ey, ez, dir);
    // Step out over the edge: walk animations toward the empty side (no ledge grab: no input).
    for (int k = 0; k < 16 && p.w.map_number_now() == m; ++k) p.act(0, dir);
    p.run(120);
    if (end_map) *end_map = p.w.map_number_now();
    const Actor* a = p.w.avatar();
    r.x = a->x; r.y = a->y; r.z = a->z;
    r.alive = !a->is(Actor::kDead);
    r.in_world = a->chunk >= 0;
    r.rescues = p.w.void_rescue.rescues;
    return r;
}

bool near_start(const VoidRun& r) { return std::abs(r.x - r.start[0]) <= 160 && std::abs(r.y - r.start[1]) <= 160 && r.z > 0; }

void print_run(const char* what, bool fixes, const VoidRun& r) {
    std::printf("  %s, fixes %s: from %d,%d,%d to %d,%d,%d; alive %d, in the world %d, rescues %u\n", what, fixes ? "on" : "off",
                r.start[0], r.start[1], r.start[2], r.x, r.y, r.z, int(r.alive), int(r.in_world), r.rescues);
}

void test_void(const GameData& d) {
    std::printf("falling out of the world\n");
    // A. A hole in a high floor.
    unsigned hm = 0;
    int hx = 0, hy = 0, ht = 0;
    for (unsigned m : {40u, 3u, 7u, 8u, 49u, 50u})
        if (high_floor(d, m, hx, hy, ht)) { hm = m; break; }
    CHECK(hm != 0);
    std::printf("  a high floor: map %u at %d,%d, top %d\n", hm, hx, hy, ht);
    for (bool stitched : {false, true})
        for (bool fixes : {false, true}) {
            VoidRun r = fall_through(d, fixes, stitched, hm, hx, hy, ht);
            print_run(stitched ? "through a hole (stitched)" : "through a hole", fixes, r);
            if (fixes) {
                CHECK(r.alive);
                CHECK_EQ(r.rescues, 1);
                CHECK(near_start(r));
            } else {
                CHECK_EQ(r.z, 0);  // the original: at the bottom of the world (killed by the fall from 104+)
                CHECK_EQ(r.alive, ht < 104);
            }
        }
    // B. Off the edge of a map's scenery (Tenebrae's city map, 40, and Herdsman's Valley, 7), in
    // both world modes: in stitched mode where no seam partner has ground beyond (a segment
    // edge; map 40 has partners all round, so only map 7 there).
    for (unsigned m : {40u, 7u})
        for (bool stitched : {false, true})
            for (bool fixes : {false, true}) {
                if (stitched && m == 40) continue;
                VoidRun r = walk_off(d, fixes, stitched, m, m == 40 ? 6 : 2);
                char what[64];
                std::snprintf(what, sizeof what, "off map %u's edge%s", m, stitched ? " (stitched)" : "");
                print_run(what, fixes, r);
                CHECK(r.alive);
                if (fixes) {
                    CHECK(r.rescues >= 1);
                    CHECK(near_start(r));
                } else {
                    CHECK_EQ(r.z, 0);  // stuck down in the void
                    CHECK_EQ(r.rescues, 0);
                }
            }
    // At a seam (stitched): walking off map 40 onto a partner's ground crosses over, no rescue.
    for (bool fixes : {false, true}) {
        unsigned end = 0;
        VoidRun r = walk_off(d, fixes, true, 40, 6, true, &end);
        print_run("over a seam off map 40 (stitched)", fixes, r);
        std::printf("    now on map %u\n", end);
        CHECK(end != 40 && end != 0);
        CHECK_EQ(r.rescues, 0);
        CHECK(r.alive);
        CHECK(r.z > 0);
    }
    // C. The edge of the chunk grid: a walk step that would take him past x 32767.
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Play p(d);
        p.warp(100, 100, 0, 40);
        p.w.kernel().kill(1, kProcAny);
        p.w.move_to(1, 32740, 20000, 0);
        p.run(2);
        p.w.collide_move(1, 32820, 20000, 0, false, false);
        const Actor& a = p.av();
        std::printf("  past x 32767, fixes %s: at %d,%d, in a chunk %d\n", fixes ? "on" : "off", a.x, a.y, int(a.chunk >= 0));
        CHECK_EQ(a.chunk >= 0, fixes);
        CHECK_EQ(a.x <= 32767, fixes);
    }
    original_bug_fixes() = true;
}


// --- 4. Map holes: the engine's extra data fixes (world/current_map.cpp apply_map_fixes) ---------
// On each tile the "Original bug fixes" add, the Avatar is put down in the middle: with the option
// Off (the original data) there is nothing there and he falls; On he stands on it.
void test_maps(const GameData& d) {
    std::printf("map holes\n");
    struct Spot { unsigned map; int x, y, z; const char* what; };
    static const Spot spots[] = {
        {62, 16127, 6143, 48, "map 62 ground"}, {62, 16767, 6143, 48, "map 62 ground"}, {62, 16895, 6143, 48, "map 62 ground"},
        {62, 19071, 6143, 48, "map 62 ground"}, {62, 20095, 7167, 48, "map 62 ground"}, {62, 14975, 6527, 0, "map 62 lava"},
        {7, 9983, 21023, 8, "map 7 ground (the gap of the spec's tile at y 21157)"}};
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Play p(d);
        int xd, yd, zd;
        p.w.footprint(p.av(), xd, yd, zd);
        for (const Spot& s : spots) {
            if (p.w.map_number_now() != s.map) p.warp(100, 100, 0, s.map);
            // His centre over the middle of the tile (the 128 x 128 tile ends at (x, y)); the
            // map-7 spot is the 32-unit strip the spec's tile leaves open.
            int ax = s.x - 64 + xd / 2, ay = (s.map == 7 ? s.y - 16 : s.y - 64) + yd / 2;
            World::Placement pl = p.w.can_exist(ax, ay, s.z, xd, yd, zd, p.w.shape_flags(p.av()), 1);
            bool lava = s.z == 0;
            // Lava at z 0: what matters is that the lava tile (a DAMAGING floor) is there.
            bool held = lava ? pl.land != 0 : (pl.supported && pl.support_z == s.z);
            std::printf("  fixes %s: %s at %d,%d,%d: %s (support z %d)\n", fixes ? "on" : "off", s.what, ax, ay, s.z,
                        held ? "floor" : "nothing", pl.support_z);
            CHECK_EQ(held, fixes);
            if (!lava) {
                p.w.kernel().kill(1, kProcAny);
                p.av().set(Actor::kAnimLock, false);
                p.w.move_to(1, ax, ay, s.z);
                p.w.fall(1);  // as when he steps there
                p.run(60);
                std::printf("    put there: now at %d,%d,%d\n", p.av().x, p.av().y, p.av().z);
                CHECK_EQ(p.av().z == s.z && std::abs(p.av().x - ax) < 16 && std::abs(p.av().y - ay) < 16, fixes);
            }
        }
    }
    original_bug_fixes() = true;
}

// --- 6. The lava sound ------------------------------------------------------------------------
// Lava bubbles (shape 603, LAVABUB) loop sound 87 (LAVABUBL) from their enterFastArea event and
// stop it in leaveFastArea. Map 12 (Daemon's Crag) has 29 of them.
// A standable spot at least 2048 from (x, y) with no lava bubble within 2048 of it.
int far_spot(World& w, int x, int y, int z, int& ox, int& oy, int& oz) {
    std::vector<std::pair<int, int>> bubbles;  // the map's bubbles, those in globs too
    for (const ScanBox& b : map_boxes(w.data(), w.map_number_now()))
        if (b.shape == 603) bubbles.push_back({b.x, b.y});
    for (int r = 2048; r <= 12288; r += 512)
        for (int k = 0; k < 8; ++k) {
            static const int ux[8] = {1, 1, 0, -1, -1, -1, 0, 1}, uy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            int cx = x + ux[k] * r, cy = y + uy[k] * r;
            bool clear = cx > 0 && cy > 0 && cx < 32768 && cy < 32768;
            for (auto [bx, by] : bubbles) clear = clear && (std::abs(bx - cx) > 2048 || std::abs(by - cy) > 2048);
            if (clear && find_spot(w, cx, cy, z, ox, oy, oz)) return r;
        }
    return 0;
}

// Sample channels of the mixer still playing (0..15 normal, 16..19 ambient loops).
int busy_channels(const AudioMixer& m) {  // the ambient-loop channels (16..19) still playing
    int n = 0;
    for (int c = 16; c < AudioMixer::kChannels; ++c) n += m.channel_busy(c);
    return n;
}

// specs/world/bugfixes.md 14: near lava the bubbling grows louder and never
// stops, because a looping sample whose channel was given to another sound went on playing,
// unreferenced. Our engine never had it: a channel is stopped before it is given a new sample
// (SoundSystem::start_voice), the ambient loops have their own channels, and a loop stops when
// its item leaves the fast area or is gone. Checked here through the real mixer.
void test_lava(const GameData& d) {
    std::printf("lava sound\n");
    Play p(d);
    AudioMixer mix;
    mix.set_rate(22050);
    p.w.sound.set_mixer(&mix);
    std::vector<int16_t> pcm(2 * 22050 / 30 + 64);
    auto run = [&](int n) {
        for (int i = 0; i < n; ++i) {
            p.step();
            std::fill(pcm.begin(), pcm.end(), 0);
            mix.render(pcm.data(), 22050 / 30);
        }
    };
    p.warp(10175, 14700, 16, 12);
    int sx, sy, sz;
    bool near_ok = find_spot(p.w, 10175, 14700, 16, sx, sy, sz);
    CHECK(near_ok);
    p.w.move_to(1, sx, sy, sz);
    run(30);
    bool near = p.w.sound.is_playing(87);
    int near_busy = busy_channels(mix);
    run(300);  // the loops keep their channels (more bubbles than ambient channels)
    int fx, fy, fz;
    int r = far_spot(p.w, sx, sy, sz, fx, fy, fz);
    p.w.move_to(1, fx, fy, fz);
    run(30);
    bool far = p.w.sound.is_playing(87);
    int far_busy = busy_channels(mix);
    std::printf("  far spot %d,%d,%d\n", fx, fy, fz);
    std::printf("  near the bubbles (%d,%d,%d): playing %d (%d mixer channels busy); %d units away: playing %d (%d busy)\n",
                sx, sy, sz, int(near), near_busy, r, int(far), far_busy);
    if (far || far_busy) std::printf("  still playing: %s\n", p.w.sound.describe().c_str());
    CHECK(near);
    CHECK(near_busy > 0);
    CHECK(!far);
    CHECK_EQ(far_busy, 0);
    // Back, a save and a load next to them, then away again.
    p.w.move_to(1, sx, sy, sz);
    p.run(30);
    SaveWriter out;
    p.w.save_game(out, "lava");
    {
        World w2(d);
        w2.auto_ui = true;
        w2.new_game();
        SaveReader in(out.data().data(), out.data().size());
        CHECK(w2.load_game(in));
        for (int f = 0; f < 30; ++f) w2.frame();
        bool loaded_near = w2.sound.is_playing(87);
        w2.move_to(1, fx, fy, fz);
        for (int f = 0; f < 30; ++f) w2.frame();
        bool loaded_far = w2.sound.is_playing(87);
        if (loaded_far) {
            std::printf("  after the load, away: %s\n", w2.sound.describe().c_str());
            for (uint32_t k = 256; k < 32767; ++k)
                if (const Item* it = w2.item(uint16_t(k)); it && w2.sound.describe().find("@" + std::to_string(k) + "(") != std::string::npos)
                    std::printf("   item %u: shape %u at %d,%d,%d flags %04x\n", k, it->shape, it->x, it->y, it->z, it->flags);
        }
        std::printf("  after a load: near %d, away %d\n", int(loaded_near), int(loaded_far));
        CHECK(loaded_near);
        CHECK(!loaded_far);
    }
    // A bubble destroyed while its loop plays (lava that a script removes).
    p.run(10);
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        p.w.move_to(1, fx, fy, fz);
        p.run(10);
        p.w.move_to(1, sx, sy, sz);
        p.run(30);
        bool before = p.w.sound.is_playing(87);
        std::vector<uint16_t> bubbles;
        for (uint32_t k = 256; k < 32767; ++k)
            if (const Item* it = p.w.item(uint16_t(k)); it && it->shape == 603 && it->chunk >= 0) bubbles.push_back(uint16_t(k));
        SaveWriter keep;
        p.w.save_game(keep, "bubbles");
        for (uint16_t k : bubbles) p.w.destroy_now(k);
        p.run(10);
        bool after_destroy = p.w.sound.is_playing(87);
        std::printf("  fixes %s: all %zu bubbles destroyed while playing %d: playing %d\n", fixes ? "on" : "off", bubbles.size(),
                    int(before), int(after_destroy));
        CHECK(before);
        CHECK(!after_destroy);  // never had it: a loop whose item is gone stops (SoundSystem::frame)
        SaveReader in(keep.data().data(), keep.data().size());
        CHECK(p.w.load_game(in));
    }
    p.w.sound.set_mixer(nullptr);
    original_bug_fixes() = true;
}

// The ambient sound items (shape 712, class SFX): the one on the Tenebrae dock (map 3, quality 0:
// WIND3 / WIND4) starts a persistent loop script from enterFastArea. Does it end when he leaves
// the map?
int sfx_loops(World& w) {
    int n = 0;
    w.kernel().for_each([&](Process* p) {
        if (auto* u = dynamic_cast<UCProcess*>(p); u && u->live() && u->cls == 712) ++n;
    });
    return n;
}

void test_ambient(const GameData& d) {
    std::printf("ambient sound loops\n");
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Play p(d);
        p.warp(11679, 6300, 48, 3);
        int near = 0;
        for (int f = 0; f < 300 && !near; ++f) { p.step(); near = sfx_loops(p.w); }
        if (std::getenv("PAGAN_BUG_VERBOSE"))
            p.w.kernel().for_each([&](Process* q) {
                if (auto* u = dynamic_cast<UCProcess*>(q); u && u->live() && (u->cls == 712 || u->item == 2496))
                    std::printf("   proc %u cls %u item %u type %u\n", q->pid(), u->cls, q->item, q->type);
            });
        p.w.teleport_actor(1, 11532, 1980, 48, 40);  // into the city (another map)
        p.run(10);
        p.run(600);
        int after = sfx_loops(p.w);
        bool wind = false;
        for (int f = 0; f < 1200 && !wind; ++f) { p.step(); wind = p.w.sound.is_playing(115) || p.w.sound.is_playing(116); }
        p.warp(11679, 6300, 48, 3);  // and back
        p.run(300);
        int back = sfx_loops(p.w);
        std::printf("  fixes %s: loops on the dock %d; on map 40 %d (wind heard there %d); back on the dock %d\n",
                    fixes ? "on" : "off", near, after, int(wind), back);
        // Our engine never had the endless loop: a map change ends every process of the old map's
        // items (World::save_map_items), so the SFX item's loop ends with its map either way.
        CHECK_EQ(near, 1);
        CHECK_EQ(after, 0);
        CHECK(!wind);
        CHECK_EQ(back, 1);
    }
    original_bug_fixes() = true;
}


}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    std::string data_dir = argc > 2 ? argv[2] : "";
    g_shots = argc > 3 ? argv[3] : "";
    std::string only = argc > 4 ? argv[4] : "";
    if (!g_shots.empty()) mkdir(g_shots.c_str(), 0755);
    if (!g_shots.empty()) Config::global().load(g_shots + "/pagan_bugfix_test.cfg");  // never the player's settings
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    {
        std::string err;
        if (!data_dir.empty() && !load_story_patches(d.usecode, data_dir + "/patches", err)) std::printf("patches: %s\n", err.c_str());
    }
    if (only == "scan") { scan_maps(d); return 0; }
    if (only == "diff") {  // PAGAN_SCAN_DIFF=a,b: floor tiles of map a with no floor near their height in map b
        unsigned a, b;
        if (const char* e = std::getenv("PAGAN_SCAN_DIFF"); e && std::sscanf(e, "%u,%u", &a, &b) == 2) {
            std::map<std::pair<int, int>, std::vector<int>> cov;
            for (const ScanBox& q : map_boxes(d, b))
                if (q.sflags & ShapeInfo::kLand)
                    for (int cy = q.y0 / 32; cy <= q.y1 / 32; ++cy)
                        for (int cx = q.x0 / 32; cx <= q.x1 / 32; ++cx) cov[{cx, cy}].push_back(q.z1);
            for (const ScanBox& q : map_boxes(d, a)) {
                if (!(q.sflags & ShapeInfo::kLand) || q.z1 - q.z0 > 8) continue;
                int cells = 0, miss = 0;
                for (int cy = q.y0 / 32; cy <= q.y1 / 32; ++cy)
                    for (int cx = q.x0 / 32; cx <= q.x1 / 32; ++cx) {
                        ++cells;
                        bool ok = false;
                        for (int t : cov[{cx, cy}]) ok |= t >= q.z1 - 8 && t <= q.z1 + 72;
                        miss += !ok;
                    }
                if (miss * 2 >= cells)
                    std::printf("map %u floor %u:%u at %d,%d,%d (box x %d..%d y %d..%d): %d/%d cells with no floor in map %u\n", a,
                                q.shape, q.frame, q.x, q.y, q.z, q.x0, q.x1, q.y0, q.y1, miss, cells, b);
            }
        }
        return 0;
    }
    if (only == "topdown") {  // PAGAN_SCAN_MAP=map,file.pgm: the highest floor top per 32-unit cell, top-down
        unsigned m;
        char file[512];
        if (const char* e = std::getenv("PAGAN_SCAN_MAP"); e && std::sscanf(e, "%u,%511s", &m, file) == 2) {
            std::vector<int> top(1024 * 1024, -1);
            std::vector<uint8_t> kind(1024 * 1024, 0);
            for (const ScanBox& q : map_boxes(d, m)) {
                bool land = q.sflags & ShapeInfo::kLand, sea = q.sflags & ShapeInfo::kSea, dmg = q.sflags & ShapeInfo::kDamaging;
                if (!land && !sea && !dmg) continue;
                for (int cy = q.y0 / 32; cy <= q.y1 / 32; ++cy)
                    for (int cx = q.x0 / 32; cx <= q.x1 / 32; ++cx) {
                        size_t i = size_t(cy) * 1024 + size_t(cx);
                        if (q.z1 > top[i] && q.z1 - q.z0 <= 16) { top[i] = q.z1; kind[i] = uint8_t(sea ? 2 : dmg ? 3 : 1); }
                    }
            }
            if (FILE* f = std::fopen(file, "wb")) {
                std::fprintf(f, "P6 1024 1024 255\n");
                for (size_t i = 0; i < top.size(); ++i) {
                    uint8_t v = top[i] < 0 ? 0 : uint8_t(std::min(255, 60 + top[i]));
                    uint8_t rgb[3] = {v, v, v};
                    if (kind[i] == 2) { rgb[0] = 0; rgb[1] = 0; }
                    if (kind[i] == 3) { rgb[2] = 0; }
                    if (top[i] < 0) rgb[0] = rgb[1] = rgb[2] = 0;
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
        return 0;
    }
    if (only == "items") {  // PAGAN_SCAN_BOX=map,x0,y0,x1,y1: every box touching it
        unsigned m; int x0, y0, x1, y1;
        if (const char* b = std::getenv("PAGAN_SCAN_BOX"); b && std::sscanf(b, "%u,%d,%d,%d,%d", &m, &x0, &y0, &x1, &y1) == 5)
            for (const ScanBox& q : map_boxes(d, m))
                if (q.x1 >= x0 && q.x0 <= x1 && q.y1 >= y0 && q.y0 <= y1)
                    std::printf("%s shape %u:%u at %d,%d,%d box x %d..%d y %d..%d z %d..%d flags %04x itemflags %04x\n",
                                q.nonfixed ? "N" : "F", q.shape, q.frame, q.x, q.y, q.z, q.x0, q.x1, q.y0, q.y1, q.z0, q.z1,
                                q.sflags, q.iflags);
        return 0;
    }
    auto want = [&](const char* s) { return only.empty() || only == s; };
    if (want("void")) test_void(d);
    if (want("maps")) test_maps(d);
    if (want("guard")) test_guard(d);
    if (want("moss")) test_moss(d);
    if (want("summon")) test_summon(d);
    if (want("lava")) test_lava(d);
    if (want("lava") || want("ambient")) test_ambient(d);
    original_bug_fixes() = true;
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
