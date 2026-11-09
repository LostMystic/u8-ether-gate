// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The buried cave mouth on map 57 (NOTES.md "The buried cave mouth"; world/map_patches.*,
// data/patches/map_content.json, option "Underworld entrance"): the patch data, nothing overlapped,
// loaded items exact with the option Off, the option live and through saves, and that the mouth
// cannot be got into: a flood of the real collision tests, then the real controls (walking,
// running, jumps with and without Easy Ledges, the targeted jump, the pathfinder) from every side.
//
//   pagan_underworld_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [only]
//   only: data, exact, overlap, flood, play, option, skull, probe (prints what blocks the mouth)
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "app/game_mode.h"
#include "check.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "core/variants.h"
#include "data/gamedata.h"
#include "game/gumps.h"
#include "game/item_powers.h"
#include "game/ledge.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/variants.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "world/current_map.h"
#include "world/map_patches.h"

using namespace pg;
int g_fail = 0, g_pass = 0;

namespace {

std::string g_data, g_shots, g_saves;
constexpr unsigned kMap = 57;

// The mouth: its front (the plug's east face), the width between the rock on either side, the
// lintel. West of this front, between these y, is behind the rubble.
constexpr int kFrontX = 13568, kMouthY0 = 17024, kMouthY1 = 17279, kLintelZ = 40;
// A spot in the open in front of it, and a spot in the dead-end cave behind it.
constexpr int kOpen[3] = {13855, 17407, 0}, kBehind[3] = {13407, 17151, 0};

const map_patches::Group& group() { return *map_patches::find("underworld_entrance"); }

using Key = std::tuple<int, int, int, int, int>;
Key key(const ItemRecord& r) { return Key(r.shape, r.frame, r.x, r.y, r.z); }

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        gm.start();
        frames(30);
        w.controls.responsive = true;
        w.controls.assist = true;
        w.controls.easy_ledges = true;
    }
    World& w() { return gm.world(); }
    Actor& av() { return *w().avatar(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { w().avatar()->hp = 100; gm.tick(); } }  // (the Pit's monsters)
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.tick();
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        save_png(g_shots + "/" + name, rgba.data(), 320, 200, 2);
    }
    void ev(const InputEvent& e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b;
        e.x = gm.mouse_x();
        e.y = gm.mouse_y();
        ev(e);
    }
    // Stands him there, nothing running.
    bool place(int x, int y, int z) {
        World& wd = w();
        PlayerInput& in = wd.input;
        in.key_jump = in.jump_held = in.pad_jump = in.pad_move = false;
        in.key_forward = in.key_back = in.key_step = in.key_run = false;
        in.jump_buffer_ms = 0;
        in.no_grab_until_ms = 0;
        wd.kernel().kill(1, kProcActorAnim);
        wd.kernel().kill(1, kProcGravity);
        Actor& a = av();
        a.flags &= uint16_t(~kItemHanging);
        a.set(Actor::kAnimLock, false);
        a.last_anim = 2;
        if (wd.map_number_now() != kMap) { wd.teleport_actor(1, x, y, z, kMap); frames(6); }
        wd.move_to(1, x, y, z, false);
        frames(2);
        return a.x == x && a.y == y && a.z == z && wd.map_number_now() == kMap;
    }
};

// The harness has teeth only if it can get him in with nothing in the way. The original mouth is
// already shut by its own low rubble and the body under the 40-high lintel (NOTES.md), so besides
// the patch this takes those out, as a test only: what the Breach will have to remove too.
void open_the_mouth(World& w) {
    underworld_entrance() = false;
    underworld::sync_entrance(w);
    underworld_entrance() = true;  // (the flag only; the map stays as it is now)
    static const int gone[][5] = {{553, 24, 13535, 17119, 0}, {553, 1, 13503, 17183, 0}, {524, 0, 13503, 17119, 0},
                                  {524, 0, 13471, 17247, 0}, {359, 0, 13599, 17183, 0}, {218, 5, 13663, 17215, 0}};
    for (const auto& g : gone) {
        std::vector<uint16_t> here = w.map().chunk(g[2] / 512, g[3] / 512);
        for (uint16_t id : here) {
            const Item* it = w.item(id);
            if (it && int(it->shape) == g[0] && int(it->frame) == g[1] && it->x == g[2] && it->y == g[3] && it->z == g[4]) {
                w.destroy_now(id);
                break;
            }
        }
    }
}

// Behind the rubble: his box's west side past the front inside the mouth, or up on the rock above.
bool behind(World& w, const Actor& a, std::string* why = nullptr) {
    int xd, yd, zd;
    w.footprint(a, xd, yd, zd);
    if (a.x - xd + 1 < kFrontX && a.y - yd / 2 > kMouthY0 && a.y - yd / 2 < kMouthY1) { if (why) *why = "past the front"; return true; }
    // (Only standing there: the top of a jump off the heap goes this high.)
    if (a.z >= 48 && a.x > kFrontX - 64 - 128 && a.x < kFrontX + 128 + 64 && a.y > kMouthY0 - 128 && a.y < kMouthY1 + 128 &&
        !w.kernel().count_exact(1, kProcActorAnim) && !(a.gravity_pid && w.kernel().get(a.gravity_pid))) {
        World::Placement p = w.can_exist(a.x, a.y, a.z, xd, yd, zd, w.shape_flags(a), 1);
        if (p.valid && p.supported) {
            if (why) *why = "up on the rock";
            return true;
        }
    }
    return false;
}

// --- The data ----------------------------------------------------------------------------------

void test_data() {
    std::printf("the data file\n");
    std::vector<map_patches::Group> builtin = map_patches::groups();
    std::string err;
    CHECK(map_patches::load_file(g_data + "/patches/map_content.json", err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    const std::vector<map_patches::Group>& t = map_patches::groups();
    CHECK_EQ(t.size(), builtin.size());
    for (size_t g = 0; g < t.size() && g < builtin.size(); ++g) {
        CHECK(t[g].id == builtin[g].id && t[g].map == builtin[g].map);
        CHECK_EQ(t[g].items.size(), builtin[g].items.size());
        for (size_t i = 0; i < t[g].items.size() && i < builtin[g].items.size(); ++i)
            CHECK(key(t[g].items[i]) == key(builtin[g].items[i]));
        CHECK_EQ(t[g].moves.size(), builtin[g].moves.size());
        for (size_t i = 0; i < t[g].moves.size() && i < builtin[g].moves.size(); ++i) {
            const map_patches::Move& a = t[g].moves[i];
            const map_patches::Move& b = builtin[g].moves[i];
            CHECK(a.shape == b.shape && a.frame == b.frame && a.x == b.x && a.y == b.y && a.z == b.z && a.to_x == b.to_x &&
                  a.to_y == b.to_y && a.to_z == b.to_z);
        }
    }
    size_t n = group().items.size();
    CHECK(!map_patches::load_text("not json", err));
    CHECK(!map_patches::load_text(R"({"groups":[{"id":"x"}]})", err));
    CHECK_EQ(group().items.size(), n);  // a bad file changes nothing
    std::printf("  group underworld_entrance: map %u, %zu items, %zu moved\n", group().map, group().items.size(), group().moves.size());
}

// --- Loaded items exact ------------------------------------------------------------------------

std::multiset<Key> loaded(const GameData& d, unsigned map) {
    CurrentMap cm(d);
    cm.load(map);
    std::multiset<Key> out;
    for (int cy = 0; cy < 64; ++cy)
        for (int cx = 0; cx < 64; ++cx)
            for (uint16_t id : cm.chunk(cx, cy)) {
                const Item& it = cm.item(id);
                if (it.origin == Item::kFixed || it.origin == Item::kNonFixed) out.insert(Key(it.shape, it.frame, it.x, it.y, it.z));
            }
    return out;
}

// What the files give for a map: the top-level records, with the data fixes when asked for.
std::multiset<Key> from_files(const GameData& d, unsigned map) {
    std::multiset<Key> out;
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(map));
    apply_map_fixes(map, fixed);
    for (const MapEntry& e : build_item_tree(fixed, d.typeflags))
        if (e.parent < 0) out.insert(key(e.rec));
    for (const MapEntry& e : build_item_tree(parse_item_records(d.nonfixed.entry(map)), d.typeflags))
        if (e.parent < 0) out.insert(key(e.rec));
    return out;
}

void test_exact(const GameData& d) {
    std::printf("loaded items\n");
    underworld_entrance() = false;
    for (unsigned m : CurrentMap::populated_maps(d)) {
        std::vector<ItemRecord> raw = parse_item_records(d.fixed.entry(m)), with = raw;
        apply_map_fixes(m, with);
        CHECK(loaded(d, m) == from_files(d, m));
    }
    std::multiset<Key> off = loaded(d, kMap);
    underworld_entrance() = true;
    std::multiset<Key> on = loaded(d, kMap);
    std::multiset<Key> want = off;
    for (const map_patches::Move& m : group().moves) {
        auto it = want.find(Key(m.shape, m.frame, m.x, m.y, m.z));
        CHECK(it != want.end());
        if (it != want.end()) want.erase(it);
        want.insert(Key(m.shape, m.frame, m.to_x, m.to_y, m.to_z));
    }
    for (const ItemRecord& r : group().items) want.insert(key(r));
    CHECK(on == want);
    for (unsigned m : CurrentMap::populated_maps(d))
        if (m != kMap) {
            underworld_entrance() = false;
            std::multiset<Key> a = loaded(d, m);
            underworld_entrance() = true;
            CHECK(a == loaded(d, m));  // no other map changes
        }
    std::printf("  Off: every map = its files + the data fixes; On: map 57 + %zu items, %zu moved, other maps the same\n",
                group().items.size(), group().moves.size());
}

// --- Nothing overlapped ------------------------------------------------------------------------

struct Vol {
    int x0, x1, y0, y1, z0, z1;
    bool meets(const Vol& o) const {  // shares a volume, or a flat item inside the other's
        bool xy = x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
        bool z = (z1 > z0 && o.z1 > o.z0) ? (z0 < o.z1 && o.z0 < z1) : (z0 <= o.z1 && o.z0 <= z1);
        return xy && z;
    }
};

// All original items of the map as boxes, nothing patched; globs expanded.
struct Orig { Vol v; unsigned shape, frame; int x, y, z; bool nonfixed; };
std::vector<Orig> original_items(const GameData& d, unsigned map) {
    std::vector<Orig> out;
    auto add = [&](const ItemRecord& r, bool nf) {
        int xd, yd, zd;
        d.typeflags.get(r.shape).footprint(r.flags & kItemFlipped, xd, yd, zd);
        out.push_back({Vol{r.x - xd + 1, r.x, r.y - yd + 1, r.y, r.z, r.z + zd}, r.shape, r.frame, r.x, r.y, r.z, nf});
    };
    auto add_exp = [&](const ItemRecord& r, bool nf) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape;
                q.frame = g.frame;
                q.x = uint16_t((r.x & ~0x1FF) + 2 * g.x + 1);
                q.y = uint16_t((r.y & ~0x1FF) + 2 * g.y + 1);
                q.z = uint8_t(std::min(255, r.z + g.z));
                add(q, nf);
            }
        }
        add(r, nf);
    };
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(map));
    for (const ItemRecord& r : fixed) add_exp(r, false);
    for (const MapEntry& e : build_item_tree(parse_item_records(d.nonfixed.entry(map)), d.typeflags))
        if (e.parent < 0) add_exp(e.rec, true);
    return out;
}

void test_overlap(const GameData& d) {
    std::printf("nothing overlapped\n");
    std::vector<Orig> items = original_items(d, kMap);
    const std::vector<map_patches::Move>& moves = group().moves;
    auto is_moved = [&](const Orig& o) {
        for (const map_patches::Move& m : moves)
            if (o.shape == m.shape && o.frame == m.frame && o.x == m.x && o.y == m.y && o.z == m.z) return true;
        return false;
    };
    std::vector<Vol> mine;
    for (const ItemRecord& r : group().items) {
        int xd, yd, zd;
        d.typeflags.get(r.shape).footprint(false, xd, yd, zd);
        mine.push_back({r.x - xd + 1, r.x, r.y - yd + 1, r.y, r.z, r.z + zd});
    }
    for (const map_patches::Move& m : moves) {
        for (const Orig& o : items)  // the same box at its new place
            if (o.shape == m.shape && o.frame == m.frame && o.x == m.x && o.y == m.y && o.z == m.z) {
                int dx = m.to_x - m.x, dy = m.to_y - m.y, dz = m.to_z - m.z;
                mine.push_back({o.v.x0 + dx, o.v.x1 + dx, o.v.y0 + dy, o.v.y1 + dy, o.v.z0 + dz, o.v.z1 + dz});
            }
    }
    int nearby = 0, hit = 0;
    for (const Orig& o : items) {
        const ShapeInfo& si = d.typeflags.get(o.shape);
        bool is_near = false;
        for (const Vol& v : mine)
            if (o.v.x0 <= v.x1 + 256 && v.x0 - 256 <= o.v.x1 && o.v.y0 <= v.y1 + 256 && v.y0 - 256 <= o.v.y1) is_near = true;
        if (is_near) ++nearby;
        // Eggs and invisible markers take no room; the floor under it and the moved items are meant.
        if (si.is(ShapeInfo::kEditor) || si.family() == kFamGlobEgg || is_moved(o)) continue;
        for (const Vol& v : mine)
            if (o.v.meets(v) && !(o.v.z1 == o.v.z0 && o.v.z1 <= v.z0)) {  // (a floor tile under it, at its feet)
                ++hit;
                std::printf("  overlaps original %s %u:%u at %d,%d,%d\n", o.nonfixed ? "N" : "F", o.shape, o.frame, o.x, o.y, o.z);
                break;
            }
    }
    CHECK_EQ(hit, 0);
    // None of the patch's items overlap each other either.
    for (size_t a = 0; a < mine.size(); ++a)
        for (size_t b = a + 1; b < mine.size(); ++b) {
            if (mine[a].meets(mine[b])) std::printf("  items %zu and %zu of the patch overlap\n", a, b);
            CHECK(!mine[a].meets(mine[b]));
        }
    std::printf("  %zu original items within 256, none overlapped; %zu items of the patch apart\n", size_t(nearby), mine.size());
}

// --- The flood: the real collision tests over the whole map ----------------------------------------

using Cell = std::tuple<int, int, int>;

struct Flood {
    World& w;
    int rise;                      // the highest climb allowed in one move
    int xd, yd, zd;
    uint16_t flags;
    std::set<Cell> seen;
    std::deque<Cell> q;
    Flood(World& world, int r) : w(world), rise(r) {
        w.footprint(*w.avatar(), xd, yd, zd);
        flags = w.shape_flags(*w.avatar());
    }
    // (Shape 564 is the black filler under and around the map: never a place he can be.)
    bool on_filler(uint16_t land) const {
        const Item* it = w.item(land);
        return !it || it->shape == 564;  // (or nothing: the floor of the void)
    }
    bool stand(int x, int y, int z) const {
        World::Placement p = w.can_exist(x, y, z, xd, yd, zd, flags, 1);
        return p.valid && p.supported && !on_filler(p.land);
    }
    void push(int x, int y, int z) {
        if (seen.insert(Cell(x, y, z)).second) q.emplace_back(x, y, z);
    }
    // From (x, y, z) one grid step to (nx, ny): level, a step or climb up, or down to what is there.
    void step(int x, int y, int z, int nx, int ny) {
        World::Placement p = w.can_exist(nx, ny, z, xd, yd, zd, flags, 1);
        if (p.valid) {
            if (p.supported) { if (!on_filler(p.land)) push(nx, ny, z); return; }
            if (p.support_z > -1000 && p.support_z < z) {
                if (stand(nx, ny, p.support_z)) push(nx, ny, p.support_z);
            }
            return;
        }
        for (int r = 8; r <= rise; r += 8) {
            if (!w.can_exist(x, y, z + r, xd, yd, zd, flags, 1).valid) return;  // the head hits something
            World::Placement up = w.can_exist(nx, ny, z + r, xd, yd, zd, flags, 1);
            if (!up.valid) continue;
            if (!up.supported || on_filler(up.land)) return;
            // More than a step needs a climb, and a climb only ends on a real ledge.
            if (r > 8 && !ledge::ledge_spot(w, *w.avatar(), nx, ny, z + r)) return;
            push(nx, ny, z + r);
            return;
        }
    }
    void run(int grid, size_t cap) {
        while (!q.empty() && seen.size() < cap) {
            auto [x, y, z] = q.front();
            q.pop_front();
            for (auto [dx, dy] : {std::pair{grid, 0}, {-grid, 0}, {0, grid}, {0, -grid}}) step(x, y, z, x + dx, y + dy);
        }
    }
    bool any_behind() const {
        for (auto& c : seen) {
            auto [x, y, z] = c;
            if (x - xd + 1 < kFrontX && x > 13184 && y - yd / 2 > kMouthY0 && y - yd / 2 < kMouthY1 && z < kLintelZ + 8) {
                if (std::getenv("PAGAN_UW_VERBOSE")) std::printf("    behind: %d,%d,%d (box %d)\n", x, y, z, xd);
                return true;
            }
        }
        return false;
    }
};

// A world on map 57 with every glob expanded (the walls) and no time passing.
void expand_all(Ui& u) {
    World& w = u.w();
    w.view_w = w.view_h = 65000;
    u.place(kOpen[0], kOpen[1], kOpen[2]);
    u.frames(4);
}

constexpr int kGrid = 32;
const int kStarts[][3] = {
    {30495, 28127, 48},   // from map 21
    {9280, 12024, 0},     // from map 51
    {14303, 16287, 16},   // beside the body with the Key of the Conventicler
    {kOpen[0], kOpen[1], kOpen[2]},
    {25279, 29159, 0},    // the Heart's mud
};

std::set<Cell> flood_map(Ui& u, int rise, Flood** keep = nullptr) {
    static Flood* last = nullptr;
    delete last;
    last = new Flood(u.w(), rise);
    for (const int* s : kStarts) {
        // Onto the grid, on the floor nearest there.
        bool done = false;
        for (int r = 0; r <= 192 && !done; r += 32)
            for (int dy = -r; dy <= r && !done; dy += 32)
                for (int dx = -r; dx <= r && !done; dx += 32) {
                    int x = (s[0] | 31) + dx, y = (s[1] | 31) + dy;
                    World::Placement p = u.w().can_exist(x, y, s[2] + 40, last->xd, last->yd, last->zd, last->flags, 1);
                    if (p.support_z > -1000 && last->stand(x, y, p.support_z)) {
                        last->push(x, y, p.support_z);
                        done = true;
                    }
                }
        if (!done) std::printf("    start %d,%d,%d: no room\n", s[0], s[1], s[2]);
    }
    last->run(kGrid, 800000);
    if (keep) *keep = last;
    return last->seen;
}


void test_flood(const GameData& d) {
    std::printf("the flood: every spot he can walk to on map 57 (real collision tests, %d grid)\n", kGrid);
    Ui u(d);
    expand_all(u);
    World& w = u.w();
    // Rubble gone: the original (with the Off moves undone).
    underworld_entrance() = false;
    underworld::sync_entrance(w);
    u.frames(2);
    Flood* f = nullptr;
    // (No frames between the floods: the monsters the view hatched stay where they are.)
    std::set<Cell> off_walk = flood_map(u, 8, &f);
    bool off_behind = f->any_behind();
    std::set<Cell> off_climb = flood_map(u, 120, &f);
    bool off_climb_behind = f->any_behind();
    // The original mouth (above) was shut already. And with the low rubble and body gone as well:
    underworld_entrance() = true;
    underworld::sync_entrance(w);
    std::set<Cell> on_walk = flood_map(u, 8, &f);
    bool on_behind = f->any_behind();
    std::set<Cell> on_climb = flood_map(u, 120, &f);
    bool on_climb_behind = f->any_behind();
    std::printf("  option Off: %zu spots walking (behind the mouth: %s), %zu with climbs up to 120 (behind: %s)\n", off_walk.size(),
                off_behind ? "yes" : "no", off_climb.size(), off_climb_behind ? "yes" : "no");
    std::printf("  option On: %zu spots walking (behind the mouth: %s), %zu with climbs up to 120 (behind: %s)\n", on_walk.size(),
                on_behind ? "yes" : "no", on_climb.size(), on_climb_behind ? "yes" : "no");
    CHECK(!on_behind);
    CHECK(!on_climb_behind);
    // Everything else is the same: what he loses or gains is only at the heap.
    auto far = [](const Cell& c) {
        auto [x, y, z] = c;
        return x < 13400 || x > 13950 || y < 16950 || y > 17400;
    };
    int lost = 0, gained = 0;
    for (const Cell& c : off_walk)
        if (!on_walk.count(c) && far(c)) { if (++lost < 6) std::printf("    lost %d,%d,%d\n", std::get<0>(c), std::get<1>(c), std::get<2>(c)); }
    for (const Cell& c : on_walk)
        if (!off_walk.count(c) && far(c)) { if (++gained < 6) std::printf("    gained %d,%d,%d\n", std::get<0>(c), std::get<1>(c), std::get<2>(c)); }
    CHECK_EQ(lost, 0);
    CHECK_EQ(gained, 0);
    std::printf("  away from the heap: %d spots lost, %d gained\n", lost, gained);
    // The flood has teeth: with the mouth's old rubble and body gone too, it walks straight in.
    {
        Ui v(d);
        expand_all(v);
        open_the_mouth(v.w());
        Flood* g = nullptr;
        flood_map(v, 8, &g);
        std::printf("  (a test only) patch, old rubble and body all gone: behind the mouth: %s\n", g->any_behind() ? "yes" : "no");
        CHECK(g->any_behind());
        v.w().view_w = v.w().view_h = 400;
        underworld_entrance() = true;
    }
    // The cave behind it is a dead end: a flood started behind never comes out (nor does it matter).
    Flood back(w, 120);
    if (back.stand(kBehind[0], kBehind[1], kBehind[2])) {
        back.push(kBehind[0], kBehind[1], kBehind[2]);
        back.run(kGrid, 100000);
        bool out = false;
        for (const Cell& c : back.seen)
            if (std::get<0>(c) > kFrontX + 64) out = true;
        std::printf("  from behind: %zu spots, %s to the front\n", back.seen.size(), out ? "a way" : "no way");
        CHECK(!out);
    }
    // The pathfinder.
    CHECK(u.place(13823, 17119, 0));
    std::vector<PathStep> path;
    {
        Pathfinder pf(w);
        pf.set_actor(1);
        pf.set_target_point(kBehind[0], kBehind[1], kBehind[2]);
        CHECK(!pf.plan(path));
        std::printf("  pathfinder to behind: %s (%u expansions)\n", path.empty() ? "no plan" : "PLANNED", pf.expanded());
    }
    {
        Pathfinder pf(w);
        pf.set_actor(1);
        pf.set_target_point(13951, 17279, 0);
        path.clear();
        CHECK(pf.plan(path));  // and it still finds its way around in front of it
    }
    w.view_w = w.view_h = 400;
}

// --- The real controls -------------------------------------------------------------------------

struct PlayStats { int starts = 0, tried = 0, got_behind = 0, bad = 0; };

// One attempt's frames: fails if he ever gets behind.
void watch(Ui& u, int n, PlayStats& st, const char* what) {
    bool bad = false;
    std::string why;
    for (int f = 0; f < n; ++f) {
        u.frames(1);
        if (!bad && behind(u.w(), u.av(), &why)) {
            bad = true;
            ++st.got_behind;
            if (st.got_behind < 8)
                std::printf("    %s: behind (%s) at %d,%d,%d\n", what, why.c_str(), u.av().x, u.av().y, u.av().z);
        }
    }
}

void release_all(Ui& u) {
    u.button(1, false);
    u.button(3, false);
    u.w().input.key_jump = u.w().input.jump_held = false;
}

// Tries everything at the mouth from around it, aiming the pointer past him at (tx, ty).
PlayStats run_play(const GameData& d, bool easy_ledges, bool classic, int stride, bool opened = false) {
    PlayStats st;
    Ui u(d);
    World& w = u.w();
    w.controls.easy_ledges = easy_ledges;
    w.controls.responsive = !classic;
    w.controls.assist = !classic;
    w.view_w = w.view_h = 1200;
    int xd, yd, zd;
    w.footprint(u.av(), xd, yd, zd);
    w.teleport_actor(1, kOpen[0], kOpen[1], kOpen[2], kMap);
    u.frames(10);
    if (opened) open_the_mouth(w);
    const uint16_t fl = w.shape_flags(u.av());
    // Screen offsets of the pointer: all round, near and far.
    static const int aim[][2] = {{-70, -35}, {-70, 0}, {-35, -35}, {0, -50}, {35, -35}, {70, 0}, {-90, -60}, {-30, -20}};
    int idx = 0;
    for (int y = kMouthY0 - 160; y <= kMouthY1 + 160; y += 48)
        for (int x = kFrontX + 32; x <= kFrontX + 320; x += 48) {
            // On the floor, or on the heap, whichever is there.
            for (int z : {40, 32, 24, 16, 8, 0}) {
                World::Placement p = w.can_exist(x, y, z, xd, yd, zd, fl, 1);
                if (!(p.valid && p.supported)) continue;
                if (++idx % stride) break;
                ++st.starts;
                for (const auto& a : aim) {
                    if (!u.place(x, y, z)) break;
                    // 1. Held right button: walks, runs at it.
                    u.move(160 + a[0], 114 + a[1]);
                    u.frames(2);
                    u.button(3, true);
                    ++st.tried;
                    watch(u, 45, st, "run");
                    release_all(u);
                    u.frames(8);
                    // 2. Both buttons: the targeted jump (Classic) or the forward jump.
                    if (!u.place(x, y, z)) break;
                    u.move(160 + a[0], 114 + a[1]);
                    u.frames(2);
                    u.button(1, true);
                    u.button(3, true);
                    u.frames(2);
                    release_all(u);
                    ++st.tried;
                    watch(u, 40, st, "both buttons");
                    // 3. Space held, the way Easy Ledges climbs.
                    if (!u.place(x, y, z)) break;
                    PlayerInput& in = w.input;
                    in.key_jump = in.jump_held = true;
                    in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
                    ++st.tried;
                    watch(u, 60, st, "held jump");
                    release_all(u);
                    // Let him come to rest, then he must be somewhere valid.
                    for (int f = 0, still = 0, lx = -1, ly = -1, lz = -1; f < 240 && still < 4; ++f) {
                        u.frames(1);
                        const Actor& av = u.av();
                        still = (av.x == lx && av.y == ly && av.z == lz) ? still + 1 : 0;
                        lx = av.x; ly = av.y; lz = av.z;
                        if (w.kernel().count_exact(1, kProcActorAnim) || (av.gravity_pid && w.kernel().get(av.gravity_pid)) || (av.flags & kItemHanging))
                            still = 0;
                    }
                    World::Placement end = w.can_exist(u.av().x, u.av().y, u.av().z, xd, yd, zd, fl, 1);
                    const Item* bl = w.item(end.blocker);
                    if (!end.valid && !(end.blocker < 256)) {  // (not a monster he was set down on)
                        ++st.bad;
                        if (st.bad < 6) std::printf("    ended inside %d at %d,%d,%d (from %d,%d,%d)\n", bl ? int(bl->shape) : -1, u.av().x, u.av().y, u.av().z, x, y, z);
                    }
                }
                break;
            }
        }
    return st;
}

void test_play(const GameData& d) {
    std::printf("the real controls: run, jump, targeted jump, held jump, from all round\n");
    PlayStats a = run_play(d, true, false, 2);
    std::printf("  Responsive, Easy Ledges On: %d starts, %d attempts: %d got behind, %d ended inside something\n", a.starts, a.tried, a.got_behind, a.bad);
    PlayStats b = run_play(d, false, false, 2);
    std::printf("  Responsive, Easy Ledges Off: %d starts, %d attempts: %d got behind, %d ended inside something\n", b.starts, b.tried, b.got_behind, b.bad);
    PlayStats c = run_play(d, false, true, 2);
    std::printf("  Classic with the targeted jump: %d starts, %d attempts: %d got behind, %d ended inside something\n", c.starts, c.tried, c.got_behind, c.bad);
    CHECK(a.starts >= 10 && a.tried > 200);  // (fewer than the grid: spots taken by the monsters that hatch)
    CHECK_EQ(a.got_behind + b.got_behind + c.got_behind, 0);
    CHECK_EQ(a.bad + b.bad + c.bad, 0);
    // The harness has teeth: with the patch, the old rubble and the body gone it does get him in.
    PlayStats e = run_play(d, true, false, 2, true);
    std::printf("  (a test only, the mouth emptied: %d of %d attempts got behind)\n", e.got_behind, e.tried);
    CHECK(e.got_behind > 0);
    // From the other side: put behind it, every control, he never comes out at the front.
    {
        Ui u(d);
        World& w = u.w();
        w.view_w = w.view_h = 1200;
        w.teleport_actor(1, kBehind[0], kBehind[1], kBehind[2], kMap);
        u.frames(10);
        int out = 0, tried = 0;
        static const int aim[][2] = {{70, 35}, {70, 0}, {35, 35}, {0, 50}, {90, 60}};
        for (const auto& a : aim) {
            CHECK(u.place(kBehind[0], kBehind[1], kBehind[2]));
            u.move(160 + a[0], 114 + a[1]);
            u.frames(2);
            u.button(3, true);
            ++tried;
            for (int f = 0; f < 60; ++f) { u.frames(1); if (u.av().x - 64 >= kFrontX) ++out; }
            release_all(u);
            CHECK(u.place(kBehind[0], kBehind[1], kBehind[2]));
            PlayerInput& in = w.input;
            in.key_jump = in.jump_held = true;
            in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
            ++tried;
            for (int f = 0; f < 60; ++f) { u.frames(1); if (u.av().x - 64 >= kFrontX) ++out; }
            release_all(u);
        }
        CHECK_EQ(out, 0);
        std::printf("  from behind: %d attempts, he never came out at the front\n", tried);
    }
    // The pathfinder's walk: told to go behind it, he stays out.
    {
        Ui u(d);
        World& w = u.w();
        w.teleport_actor(1, kOpen[0], kOpen[1], kOpen[2], kMap);
        u.frames(10);
        uint16_t pid = w.pathfind(1, kBehind[0], kBehind[1], kBehind[2], 0, 0);
        PlayStats st;
        for (int i = 0; i < 600; ++i) {
            u.frames(1);
            if (behind(w, u.av())) ++st.got_behind;
            if (pid && !w.kernel().get(pid)) break;
        }
        CHECK_EQ(st.got_behind, 0);
        std::printf("  middle-click walk to behind it: ended at %d,%d,%d\n", u.av().x, u.av().y, u.av().z);
    }
}

// --- The option, saves, the Breach -------------------------------------------------------------

int rubble_on_map(World& w) {
    int n = 0;
    for (const ItemRecord& r : group().items)
        for (uint16_t id : w.map().chunk(r.x / 512, r.y / 512)) {
            const Item* it = w.item(id);
            if (it && it->origin == Item::kFixed && key(ItemRecord(r)) == Key(it->shape, it->frame, it->x, it->y, it->z)) { ++n; break; }
        }
    return n;
}

bool moved_items_at(World& w, bool moved) {
    for (const map_patches::Move& m : group().moves) {
        const int x = moved ? m.to_x : m.x, y = moved ? m.to_y : m.y;
        bool found = false;
        for (uint16_t id : w.map().chunk(x / 512, y / 512)) {
            const Item* it = w.item(id);
            if (it && it->shape == m.shape && it->frame == m.frame && it->x == x && it->y == y) found = true;
        }
        if (!found) return false;
    }
    return true;
}

void test_option(const GameData& d) {
    std::printf("the option, saves, the Breach\n");
    register_variant_options();
    Ui u(d);
    World& w = u.w();
    const int all = int(group().items.size());
    w.teleport_actor(1, kOpen[0], kOpen[1], kOpen[2], kMap);
    u.frames(10);
    CHECK_EQ(rubble_on_map(w), all);
    CHECK(moved_items_at(w, true));
    // The row, live.
    bool saw = false;
    for (auto& make : World::extra_option_rows()) {
        MenuGump::OptionRow r = make(w);
        if (r.label != "Underworld entrance:") continue;
        saw = true;
        CHECK(r.group == "Game");
        CHECK(r.value() == "On");
        r.change(1);
        CHECK(r.value() == "Off");
        CHECK_EQ(rubble_on_map(w), 0);
        CHECK(moved_items_at(w, false));
        u.shot("rubble_option_off.png");
        CHECK(!Config::global().get_bool("underworld_entrance", true));
        r.change(1);
        CHECK(r.value() == "On");
        CHECK_EQ(rubble_on_map(w), all);
        CHECK(moved_items_at(w, true));
    }
    CHECK(saw);
    // A save made with it On, loaded with it Off, and the other way: the map follows the option.
    {
        SaveWriter out;
        w.save_game(out, "rubble");
        std::vector<uint8_t> on_save = out.data();
        Config::global().set_bool("underworld_entrance", false);
        SaveReader in(on_save.data(), on_save.size());
        CHECK(w.load_game(in));
        CHECK(!underworld_entrance());
        CHECK_EQ(rubble_on_map(w), 0);
        CHECK(moved_items_at(w, false));
        SaveWriter out2;
        w.save_game(out2, "rubble");
        std::vector<uint8_t> off_save = out2.data();
        Config::global().set_bool("underworld_entrance", true);
        SaveReader in2(off_save.data(), off_save.size());
        CHECK(w.load_game(in2));
        CHECK(underworld_entrance());
        CHECK_EQ(rubble_on_map(w), all);
        CHECK(moved_items_at(w, true));
        // On a map the group is not on: the next visit has it.
        w.teleport_actor(1, 4342, 2138, 0, 21);
        u.frames(5);
        underworld_entrance() = false;
        w.teleport_actor(1, kOpen[0], kOpen[1], kOpen[2], kMap);
        u.frames(5);
        CHECK_EQ(rubble_on_map(w), 0);
        CHECK(moved_items_at(w, false));
        underworld_entrance() = true;
        underworld::sync_entrance(w);
        Config::global().set_bool("underworld_entrance", true);
    }
    // What the Skull will do later: the Breach open takes the rubble away, the moved stay moved, and
    // the way is free.
    underworld_breach_open() = true;
    underworld::sync_entrance(w);
    CHECK_EQ(rubble_on_map(w), 0);
    CHECK(moved_items_at(w, true));
    {
        int xd, yd, zd;
        w.footprint(u.av(), xd, yd, zd);
        CHECK(w.can_exist(13567, 17215, 0, xd, yd, zd, w.shape_flags(u.av()), 1).valid);
    }
    underworld_breach_open() = false;
    underworld::sync_entrance(w);
    CHECK_EQ(rubble_on_map(w), all);
    std::printf("  row, live; saves either way; other maps; the Breach flag: %d items each time\n", all);
}

// --- The Skull does not open it yet ------------------------------------------------------------

void test_skull(const GameData& d) {
    std::printf("the Skull's power\n");
    Ui u(d);
    World& w = u.w();
    w.teleport_actor(1, kOpen[0], kOpen[1], kOpen[2], kMap);
    u.frames(10);
    CHECK(u.place(kFrontX + 192, 17151, 0));
    const int all = int(group().items.size());
    CHECK(item_powers::fire(w, item_powers::abilities().front()));
    u.frames(120);
    CHECK_EQ(rubble_on_map(w), all);
    CHECK(!underworld_breach_open());
    CHECK(moved_items_at(w, true));
    std::printf("  fired beside the mouth: all %d items still there\n", all);
    // Fixed items: can't be picked up.
    for (const ItemRecord& r : group().items) CHECK(d.typeflags.get(r.shape).is(ShapeInfo::kFixed));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_underworld_saves";
    if (!g_shots.empty()) mkdir(g_shots.c_str(), 0755);
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (only == "probe") {  // what is in the way, spot by spot, along the mouth
        Ui u(d);
        expand_all(u);
        World& w = u.w();
        underworld_entrance() = false;
        underworld::sync_entrance(w);
        int xd, yd, zd;
        w.footprint(u.av(), xd, yd, zd);
        std::printf("box %d %d %d\n", xd, yd, zd);
        for (int y : {17119, 17151, 17183, 17215})
            for (int z : {0, 8})
                for (int x = 13727; x >= 13343; x -= 32) {
                    World::Placement p = w.can_exist(x, y, z, xd, yd, zd, w.shape_flags(u.av()), 1);
                    const Item* b = w.item(p.blocker);
                    std::printf("y %d z %d x %d: valid %d sup %d %d", y, z, x, p.valid, p.supported, p.support_z);
                    if (b) std::printf(" blocker %u:%u at %d,%d,%d", b->shape, b->frame, b->x, b->y, b->z);
                    std::printf("\n");
                }
        return 0;
    }
    if (want("data")) test_data();
    if (want("exact")) test_exact(d);
    if (want("overlap")) test_overlap(d);
    if (want("flood")) test_flood(d);
    if (want("option")) test_option(d);
    if (want("skull")) test_skull(d);
    if (want("play")) test_play(d);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
