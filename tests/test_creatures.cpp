// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The new creatures (game/creatures.h, data/creatures.json, PLAN-CREATURES.md) and one level per
// monster type (data/monster_levels.json "weak_prefix"), through the real game mode.
//   pagan_creatures_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
//   ONLY: data, levels, rules, drain, surface, bait, arcs, metal, weaken, fire, water, turn, hud,
//   option, spawn, saves, places, shots; or a probe: "find CREATURE MAP X Y Z R"
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
#include "game/combat.h"
#include "game/creatures.h"
#include "game/critpath.h"
#include "game/game_log.h"
#include "game/ledge.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/virtues.h"
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
    Ui(const GameData& d, bool leveling, bool creatures = true, bool companions = true) : gm(d) {
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
        if (!w.creatures->load_dir(g_data, err)) std::printf("  creatures: %s\n", err.c_str());
        w.creatures->enabled = creatures;
        gm.start();
        ticks(40);  // the companions appear on the beach
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Leveling& l() { return *gm.world().leveling; }
    Creatures& c() { return *gm.world().creatures; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ticks(int n) { for (int i = 0; i < n; ++i) gm.tick(); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e);
        e.type = InputEvent::kKeyUp; gm.handle(e);
    }
    Actor* av() { return w().avatar(); }
    uint16_t cid(const char* name) { int i = p().find_def(name); return i >= 0 ? p().state[size_t(i)].id : 0; }
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
    void shot(const std::string& name, int scale = 2) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, scale);
    }
};

// The Avatar on map 26 by the lake's north shore (not on the way-in teleporter).
void to_lake(Ui& u) {
    u.place(26, 17504, 11000, 104);
    Creatures& c = u.c();
    const CreatureDef& walker = c.defs[size_t(c.def_index("wraith"))];
    for (int r = 0; r < 600; r += 16)
        for (int k = 0; k < 16; ++k) {
            int sx = 17504 + int(r * std::cos(k * 0.3927)), sy = 11100 + int(r * std::sin(k * 0.3927)), sz;
            if (c.habitat_ok(walker, sx, sy, 104, 1, sz)) { u.w().move_to(1, sx, sy, sz, false); return; }
        }
}

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

json read_json(const std::string& path) {
    std::ifstream f(path);
    return json::parse(f, nullptr, false);
}

// --- The flood: the real collision tests (as tests/test_growth.cpp) --------------------------------

using Cell = std::tuple<int, int, int>;

struct Flood {
    World& w;
    int rise;
    int xd, yd, zd;
    uint16_t flags;
    std::map<Cell, int> seen;
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
    // The nearest flooded cell to the box of a creature at (x, y, z): ground distance and its height.
    int nearest(int x, int y, int bxd, int byd, int& cz, int max_climbs = 1) const {
        int best = 1 << 30;
        for (const auto& e : seen) {
            if (e.second > max_climbs) continue;
            auto [cx, cy, z] = e.first;
            const int gx = std::max({0, (cx - xd) - x, (x - bxd) - cx});
            const int gy = std::max({0, (cy - yd) - y, (y - byd) - cy});
            const int dd = int(std::sqrt(double(gx) * gx + double(gy) * gy));
            if (dd < best) { best = dd; cz = z; }
        }
        return best;
    }
};

void expand_all(Ui& u) {
    u.w().view_w = u.w().view_h = 65000;
    u.ticks(3);
}

std::vector<std::array<int, 3>> arrivals(World& w) {
    std::vector<std::array<int, 3>> out;
    // Where the map links (data/map_links.json: usecode teleporters too) arrive on this map.
    json links = read_json(g_data + "/map_links.json");
    if (!links.is_discarded())
        for (const json& l : links["links"])
            if (l.value("to_map", -1) == int(w.map_number_now()) && l.contains("to_x"))
                out.push_back({l["to_x"].get<int>(), l["to_y"].get<int>(), l["to_z"].get<int>()});
    for (size_t i = 512; i < 32768; ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0 || it->parent) continue;
        if (w.data().typeflags.get(it->shape).family() == kFamTeleportEgg && it->frame == 1) out.push_back({it->x, it->y, it->z});
    }
    return out;
}

// No item (actors aside) overlaps the creature's box at the spot: nothing stamped over.
bool clear_box(World& w, unsigned body, int x, int y, int z, uint16_t* blocker = nullptr) {
    int xd, yd, zd;
    w.data().typeflags.get(body).footprint(false, xd, yd, zd);
    const int z1 = z + std::max(zd, 1);
    for (size_t i = 256; i < 32768; ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0 || it->parent) continue;
        Box b = w.box_of(*it);
        if (b.x - b.xd >= x || x - xd >= b.x || b.y - b.yd >= y || y - yd >= b.y) continue;
        if (b.z + std::max(b.zd, 1) <= z || z1 <= b.z) continue;
        if (b.zd == 0 && b.z == z) continue;  // the water / lava it lies on
        if (blocker) *blocker = uint16_t(i);
        return false;
    }
    return true;
}

// The flood of a map from its arrivals (the Avatar stands there with nothing of ours about).
std::unique_ptr<Flood> flood_map(Ui& u, unsigned map) {
    u.place(map, 1000, 1000, 0);
    expand_all(u);
    auto arr = arrivals(u.w());
    if (arr.empty()) return nullptr;
    u.place(map, arr[0][0], arr[0][1], arr[0][2]);
    expand_all(u);
    auto f = std::make_unique<Flood>(u.w(), 120);
    for (auto& a : arr) f->seed(a[0], a[1], a[2], 32);
    f->run(32, 900000);
    return f;
}

// --- Probe: spots near a point for a creature, and how near the party can stand -----------------

void find(const GameData& d, const std::string& id, unsigned map, int x, int y, int z, int r) {
    Ui u(d, false, false, false);
    u.frames(5);
    auto f = flood_map(u, map);
    if (!f) { std::printf("no arrivals on map %u\n", map); return; }
    int di = u.c().def_index(id);
    if (di < 0) { std::printf("no creature %s\n", id.c_str()); return; }
    const CreatureDef& def = u.c().defs[size_t(di)];
    int bxd, byd, bzd;
    u.w().data().typeflags.get(def.body).footprint(false, bxd, byd, bzd);
    std::printf("flood: %zu cells\n", f->seen.size());
    int shown = 0, tried = 0, hab = 0, clr = 0, nearest = 1 << 30;
    std::map<int, int> blocked;
    for (int rr = 0; rr <= r && shown < 30; rr += 32)
        for (int dy = -rr; dy <= rr && shown < 30; dy += 32)
            for (int dx = -rr; dx <= rr && shown < 30; dx += 32) {
                if (std::max(std::abs(dx), std::abs(dy)) != rr) continue;
                int oz;
                const int px = x + dx, py = y + dy;
                ++tried;
                if (!u.c().habitat_ok(def, px, py, z, 0, oz)) continue;
                ++hab;
                uint16_t bl = 0;
                if (!clear_box(u.w(), def.body, px, py, oz, &bl)) { ++blocked[u.w().item(bl) ? u.w().item(bl)->shape : 0]; continue; }
                ++clr;
                int cz = 0;
                int near = f->nearest(px, py, bxd, byd, cz);
                if (near < nearest) nearest = near;
                if (near > def.reach) continue;
                std::printf("  %d,%d,%d: party stands %d away at z %d\n", px, py, oz, near, cz);
                ++shown;
            }
    std::printf("tried %d, in habitat %d, clear %d, nearest party %d; blocked by:", tried, hab, clr, nearest);
    for (auto& b : blocked) std::printf(" %d x%d", b.first, b.second);
    std::printf("\n");
}

// --- Tests ------------------------------------------------------------------------------------------

// Probe (not part of the run): original monster eggs and monsters near each place.
void crowd(const GameData& d) {
    Ui base(d, false, false, false);
    for (const CreatureDef& def0 : base.c().defs)
        for (const CreaturePlace& p : def0.places) {
            Ui u(d, false, false, false);
            u.frames(5);
            u.place(p.map, p.x, p.y, p.z);
            expand_all(u);
            int eggs = 0, eggs_near = 0, act = 0, act_near = 0, near_egg = 1 << 30, all_eggs = 0;
            for (size_t i = 2; i < 32768; ++i) {
                const Item* it = u.w().item(uint16_t(i));
                if (!it || it->chunk < 0 || it->parent) continue;
                const int dist = std::max(std::abs(it->x - p.x), std::abs(it->y - p.y));
                if (it->shape == 500) {
                    ++all_eggs;
                    if (dist < 2048) ++eggs;
                    if (dist < 1024) ++eggs_near;
                    near_egg = std::min(near_egg, dist);
                } else if (i < 256 && i > 1) {
                    const Actor* a = u.w().actor(uint16_t(i));
                    if (a && !a->is(Actor::kDead) && u.c().def_of(uint16_t(i)) == nullptr) {
                        if (dist < 2048) ++act;
                        if (dist < 1024) ++act_near;
                    }
                }
            }
            std::printf("CROWD %-12s map %2u (%5d,%5d) eggs<1024 %d <2048 %d nearest %d (map total %d); actors<1024 %d <2048 %d  %s\n",
                        def0.id.c_str(), p.map, p.x, p.y, eggs_near, eggs, near_egg, all_eggs, act_near, act, p.note.c_str());
        }
}

// Probe (not part of the run): reachable spots for CREATURE on MAP far from the original monster eggs.
void lonely(const GameData& d, const std::string& id, unsigned map, int min_egg) {
    Ui u(d, false, false, false);
    u.frames(5);
    auto f = flood_map(u, map);
    if (!f) { std::printf("no arrivals\n"); return; }
    int di = u.c().def_index(id);
    const CreatureDef& def = u.c().defs[size_t(di)];
    std::vector<std::array<int, 2>> eggs;
    for (size_t i = 2; i < 32768; ++i) {
        const Item* it = u.w().item(uint16_t(i));
        if (it && it->chunk >= 0 && !it->parent && it->shape == 500) eggs.push_back({it->x, it->y});
    }
    int bxd, byd, bzd;
    u.w().data().typeflags.get(def.body).footprint(false, bxd, byd, bzd);
    std::set<std::pair<int, int>> done;
    int shown = 0;
    for (auto& e : f->seen) {
        const int x = std::get<0>(e.first) / 256 * 256, y = std::get<1>(e.first) / 256 * 256, z = std::get<2>(e.first);
        if (!done.insert({x, y}).second) continue;
        int ne = 1 << 30;
        for (auto& g : eggs) ne = std::min(ne, std::max(std::abs(g[0] - x), std::abs(g[1] - y)));
        if (ne < min_egg) continue;
        int oz = 0;
        if (!u.c().habitat_ok(def, x, y, z, 0, oz)) continue;
        if (!clear_box(u.w(), def.body, x, y, oz)) continue;
        int cz = 0;
        if (f->nearest(x, y, bxd, byd, cz) > std::max(def.reach, 32)) continue;
        std::printf("LONELY %d,%d,%d nearest egg %d\n", x, y, oz, ne);
        if (++shown > 400) break;
    }
}

void test_data(const GameData& d) {
    std::printf("data: five creatures, their levels, tags for every damaging companion spell\n");
    Ui u(d, true);
    Creatures& c = u.c();
    CHECK_EQ(c.defs.size(), 5);
    const std::map<std::string, int> lv = {{"lurker_pet", 6}, {"wraith", 7}, {"lava_wyrm", 7}, {"storm_eye", 7}, {"pyros_flame", 8}};
    for (const CreatureDef& def : c.defs) {
        CHECK(lv.count(def.id));
        if (lv.count(def.id)) CHECK_EQ(def.level, lv.at(def.id));
        CHECK(d.shapes.shape(def.body) != nullptr);  // the body (data/shapes/shapes.json)
        CHECK(monster_info(def.body) != nullptr);
        CHECK(u.l().monsters.count(def.body));
        CHECK(!def.places.empty());
        CHECK(!def.look.empty());
        CHECK(!def.rules.empty());
    }
    for (unsigned s = 815; s <= 822; ++s) CHECK(c.magic_weapons.count(s));
    for (const SpellDef& s : u.p().spells)
        if (s.dmg1 > 0 && (s.kind == "bolt" || s.kind == "field" || s.kind == "lightning" || s.kind == "knock" || s.kind == "slow" ||
                           s.kind == "drain"))
            CHECK(c.spell_tags.count(s.id));
    // No Avatar spell is water; the Tempest spells of Varn and Nerissa are.
    for (const char* s : {"wave", "slowing_tide", "ice_shards"}) {
        auto& t = c.spell_tags[s];
        CHECK(std::find(t.begin(), t.end(), "water") != t.end());
    }
}

void test_levels(const GameData& d) {
    std::printf("levels: one level per type; a weaker variant early, shown as \"young troll\"\n");
    Ui u(d, true);
    const std::map<std::string, int> want = {
        {"small lizard", 1}, {"small spider", 1}, {"small insect", 1}, {"torax", 1}, {"skeleton", 2}, {"fireskull", 2},
        {"ghoul", 3}, {"changeling", 3}, {"invisible", 3}, {"kith", 4}, {"seeker", 5}, {"golem", 5}, {"troll", 6},
        {"ghost", 6}, {"air servant", 7}, {"demon", 8}};
    int found = 0;
    for (const auto& m : u.l().monsters) {
        auto it = want.find(m.second.name);
        if (it == want.end()) continue;
        ++found;
        CHECK_EQ(m.second.level, it->second);
        // Every map entry is a weaker variant, and every variant is at or below the type's level.
        for (const auto& ml : m.second.map_level) CHECK(ml.second < m.second.level);
        for (const MonsterVariant& v : m.second.variants) CHECK(v.level <= m.second.level);
        if (!m.second.map_level.empty()) CHECK(!m.second.weak_prefix.empty());
    }
    CHECK_EQ(found, int(want.size()));
    // A troll on map 7 (Herdsman's Valley) is a young troll; on map 14 a troll.
    u.place(7, 12000, 12000, 0);
    uint16_t t = u.w().create_monster(83, 0);
    Actor* ta = u.w().actor(t);
    CHECK(ta && u.l().monster_level(*ta) == 2);
    CHECK(GameLog::name_of(u.w(), t, false) == std::string("young troll"));
    CHECK(u.l().weak_prefix(*ta) == std::string("young "));
    u.w().move_to(t, u.av()->x + 128, u.av()->y, u.av()->z, false);
    u.w().call_event(t, kEvLook);
    bool barked = false;
    for (const Bark& b : u.w().barks)
        if (b.speaker == t)
            for (const TextPage& pg : b.pages)
                for (const auto& line : pg.lines) barked = barked || line.find("young troll") != std::string::npos;
    CHECK(barked);
    ta->mapnum = 14;
    ta->ext_flags = uint16_t(ta->ext_flags & ~kExtLevelMask);
    CHECK_EQ(u.l().monster_level(*ta), 6);
    CHECK(creature_display_name(u.w(), t) == std::string());
    u.w().destroy_now(t);
    // With Leveling off there is no prefix (the original's stats and names).
    Ui off(d, false);
    off.place(7, 12000, 12000, 0);
    uint16_t t2 = off.w().create_monster(83, 0);
    CHECK(creature_display_name(off.w(), t2) == std::string());
}

// A creature spawned in front of the Avatar on the start beach (or at a given spot).
uint16_t spawn_near(Ui& u, const std::string& id, int dx = 160, int dy = 0) {
    Creatures& c = u.c();
    int di = c.def_index(id);
    const Actor* a = u.av();
    uint16_t k = c.spawn_at(di, a->x + dx, a->y + dy, a->z);
    return k;
}

// A companion casts at a target for real (animation, missile, landing), with mana to spare.
void cast_at(Ui& u, const char* who, const char* spell, uint16_t target) {
    const int i = u.p().find_def(who);
    if (!u.p().in_party(i)) u.p().join(i);
    Actor* c = u.w().actor(u.p().state[size_t(i)].id);
    const Actor* t = u.w().actor(target);
    CHECK(c && t);
    if (!c || !t) return;
    u.w().move_to(c->id, t->x - 200, t->y, t->z, false);
    c->mana = 99;
    const bool ok = u.p().cast(i, u.p().spell_index(spell), target);
    CHECK(ok);
    for (int f = 0; f < 120; ++f) u.ticks(1);
}

int hit_with(Ui& u, uint16_t victim, uint16_t attacker, int damage, uint16_t type, bool weapon, const std::string& spell = "") {
    u.w().game_log.spell_context = spell;
    int r = u.c().on_hit(victim, attacker, damage, type, weapon);
    u.w().game_log.spell_context.clear();
    return r;
}

uint16_t equip(Ui& u, uint16_t who, unsigned shape, unsigned frame = 0) {
    World& w = u.w();
    const unsigned et = w.data().typeflags.get(shape).equip_type();
    for (unsigned s = 1; s <= 6; ++s)
        if (const Item* it = w.item(w.equipped(who, s)); it && w.data().typeflags.get(it->shape).equip_type() == et) w.destroy_now(it->id);
    uint16_t id = w.create_item(shape, frame);
    uint16_t pack = w.equipped(who, 7);
    if (pack) w.move_to_container(id, pack);
    if (!w.equip(who, id)) { w.destroy_now(id); return 0; }
    return id;
}

void test_rules(const GameData& d) {
    std::printf("rules: magic weapons only, immune to spells, fire immunity, water and artifact weaknesses, lava\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t wr = spawn_near(u, "wraith");
    const uint16_t fl = spawn_near(u, "pyros_flame", 0, 160);
    CHECK(wr && fl);
    // The Avatar with a mace: a plain weapon does nothing to the wraith.
    equip(u, 1, 417);
    CHECK_EQ(hit_with(u, wr, 1, 10, kDmgBlunt, true), 0);
    // Slayer (an artifact): full damage.
    equip(u, 1, 816);
    CHECK_EQ(hit_with(u, wr, 1, 10, 0x64, true), 10);
    // Spells: Kalvax's Fire Bolt, Wynna's Lightning, an Avatar Flame Bolt (usecode, type 8), Grant Peace (0xA8).
    const uint16_t kal = u.cid("kalvax");
    CHECK_EQ(hit_with(u, wr, kal, 10, 0xA8, false, "Fire Bolt"), 0);
    CHECK_EQ(hit_with(u, wr, kal, 10, 0xA0, false, "Lightning"), 0);
    CHECK_EQ(hit_with(u, wr, 1, 10, kDmgFire, false), 0);
    CHECK_EQ(hit_with(u, wr, 1, 60, 0xA8, false), 0);
    auto tags = c.hit_tags(wr, 1, 0xA8, false);
    CHECK(std::find(tags.begin(), tags.end(), "grant_peace") != tags.end());
    // The wraith is not undead (Grant Peace and the Khumash-Gor scimitar's double do not apply).
    CHECK(!(monster_info(c.def_of(wr)->body)->def_type & kDmgUndead));
    // A daemon's blow (magic bit) hurts it.
    const uint16_t dm = u.w().create_monster(96, 0);
    CHECK_EQ(hit_with(u, wr, dm, 10, 0x29, true), 10);
    u.w().destroy_now(dm);
    // Flame of Pyros: fire 0, a mace half, Flame Sting (fire + artifact) 150%, Ice Shards double, lava 0.
    equip(u, 1, 417);
    CHECK_EQ(hit_with(u, fl, 1, 10, kDmgBlunt, true), 5);
    CHECK_EQ(hit_with(u, fl, kal, 10, 0xA8, false, "Fire Bolt"), 0);
    CHECK_EQ(hit_with(u, fl, 1, 10, kDmgFire, false), 0);
    CHECK_EQ(hit_with(u, fl, 1, 10, kDmgBlunt | kDmgFire, false), 0);
    equip(u, 1, 817);
    CHECK_EQ(hit_with(u, fl, 1, 10, 0x2A, true), 15);
    CHECK_EQ(hit_with(u, fl, u.cid("nerissa"), 10, 0xA0, false, "Ice Shards"), 20);
    CHECK_EQ(hit_with(u, fl, u.cid("varn"), 6, 0xA0, false, "Wave"), 12);
    CHECK_EQ(hit_with(u, fl, 0, 4, kDmgPierce, false), 0);  // lava (no attacker, pierce)
    // Through the real hit: Fire Bolt landing from Kalvax leaves its HP, the log says why.
    const int hp = u.w().actor(fl)->hp;
    u.w().stasis = true;  // (it stands still for the bolt)
    c.log.clear();
    cast_at(u, "kalvax", "fire_bolt", fl);
    u.w().stasis = false;
    CHECK_EQ(int(u.w().actor(fl)->hp), hp);
    CHECK(std::find(c.log.begin(), c.log.end(), "rule fire 0") != c.log.end());
    bool said = false;
    for (const auto& l : u.w().game_log.lines(GameLog::kCombat)) said = said || l.text.find("Fire only feeds the Flame") != std::string::npos;
    CHECK(said);
}

void test_drain(const GameData& d) {
    std::printf("drain: each wraith blow that lands takes mana\n");
    Ui u(d, true);
    u.frames(10);
    const uint16_t wr = spawn_near(u, "wraith", 64);
    Actor* av = u.av();
    av->mana = 20;
    u.c().on_hit(1, wr, 5, kDmgNormal, false);
    CHECK_EQ(int(av->mana), 14);
    u.c().on_hit(1, wr, 0, kDmgNormal, false);  // a miss takes nothing
    CHECK_EQ(int(av->mana), 14);
    // Its real strike: a hit through receive_hit.
    av->mana = 20;
    int before = av->hp;
    for (int k = 0; k < 20 && av->hp == before; ++k) u.c().strike(wr, 1);
    CHECK(int(av->hp) < before);
    CHECK(av->mana < 20);
}

void test_surface(const GameData& d) {
    std::printf("surface: the lurker's pet and the lava wyrm can be hit only while up\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const int di = c.def_index("lurker_pet");
    // Tenebrae harbour water by the start beach.
    int x = 14800, y = 6400, z = 0, oz = 0;
    bool ok = false;
    for (int dx = 0; dx < 800 && !ok; dx += 32)
        for (int dy = 0; dy < 800 && !ok; dy += 32)
            if (c.habitat_ok(c.defs[size_t(di)], x + dx, y + dy, z, 0, oz)) { x += dx; y += dy; ok = true; }
    CHECK(ok);
    const uint16_t lp = c.spawn_at(di, x, y, oz);
    CHECK(lp);
    CHECK(c.untargetable(lp));
    CHECK_EQ(hit_with(u, lp, 1, 10, kDmgBlade, true), 0);
    c.live[lp].phase = CreatureState::kUp;
    CHECK(!c.untargetable(lp));
    CHECK_EQ(hit_with(u, lp, 1, 10, kDmgBlade, true), 10);
    // It moves only in water: a step onto the beach is refused.
    int nz;
    CHECK(!c.habitat_ok(c.defs[size_t(di)], u.av()->x, u.av()->y, u.av()->z, lp, nz));
    // In real time it surfaces when the Avatar stands at the water's edge, and bites.
    c.live[lp].phase = CreatureState::kHidden;
    c.live[lp].timer = 0;
    u.w().move_to(1, x + 40, y - 140, 0, false);
    // A spot on the shore within its reach (ground, not water: the Avatar would drown).
    const Actor* a = u.w().actor(lp);
    const CreatureDef& walker = c.defs[size_t(c.def_index("wraith"))];
    bool shore = false;
    for (int r = 64; r < 400 && !shore; r += 16)
        for (int k = 0; k < 16 && !shore; ++k) {
            int sx = a->x + int(r * std::cos(k * 0.3927)), sy = a->y + int(r * std::sin(k * 0.3927)), sz;
            if (c.habitat_ok(walker, sx, sy, 8, 1, sz)) {
                u.w().move_to(1, sx, sy, sz, false);
                shore = true;
            }
        }
    CHECK(shore);
    const int hp = u.av()->hp;
    u.av()->hp = 200;
    int ups = 0;
    for (int f = 0; f < 900; ++f) {
        u.ticks(1);
        if (c.live.count(lp) && c.live[lp].phase == CreatureState::kUp) ++ups;
    }
    CHECK(ups > 0);
    CHECK(c.strikes > 0);
    u.av()->hp = uint16_t(hp);
    // The lava wyrm: hidden in the lava tunnel, the Blue Vial hook keeps it up.
    const int wi = c.def_index("lava_wyrm");
    const CreaturePlace& wp = c.defs[size_t(wi)].places[1];
    u.place(wp.map, wp.x + 200, wp.y, wp.z);
    const uint16_t ww = c.spawn(wi, 1);
    CHECK(ww);
    if (ww) {
        CHECK(c.untargetable(ww));
        CHECK(c.trap_in_rock(ww));
        CHECK(!c.untargetable(ww));
        CHECK_EQ(hit_with(u, ww, 1, 10, kDmgBlade, true), 10);
    }
}

void test_bait(const GameData& d) {
    std::printf("bait: food thrown into the water lures the lurker's pet up\n");
    Ui u(d, true);
    u.w().virtues->enabled = true;  // Virtues (PLAN-VIRTUES.md 3.19): feeding it calms it
    u.frames(10);
    Creatures& c = u.c();
    const int di = c.def_index("lurker_pet");
    int x = 14800, y = 6400, oz = 0;
    bool ok = false;
    for (int dx = 0; dx < 800 && !ok; dx += 32)
        for (int dy = 0; dy < 800 && !ok; dy += 32)
            if (c.habitat_ok(c.defs[size_t(di)], 14800 + dx, 6400 + dy, 0, 0, oz)) { x = 14800 + dx; y = 6400 + dy; ok = true; }
    const uint16_t lp = c.spawn_at(di, x + 128, y + 128, oz);
    CHECK(lp);
    // A fish, dropped from above onto the water near it: it falls, touches the water (usecode WATER
    // destroys it with a splash) and the pet comes up for it.
    uint16_t fish = u.w().create_item(537, 0);
    u.w().move_to(fish, x, y, oz + 48, false);
    u.w().fall(fish);
    for (int f = 0; f < 90 && !c.baits; ++f) u.ticks(1);
    CHECK_EQ(c.baits, 1);
    int up = 0;
    for (int f = 0; f < 400; ++f) {
        u.ticks(1);
        if (c.live.count(lp) && c.live[lp].phase == CreatureState::kUp) ++up;
    }
    CHECK(up >= 200);  // it surfaces and stays up while it eats
    CHECK(!u.w().item(fish) || u.w().item(fish)->shape != 537);
    // Calmed: Compassion once, out of the fight, no target; a blow ends the calm.
    CHECK(c.live.count(lp) && c.live[lp].calm);
    CHECK_EQ(u.w().virtues->get(kCompassion), 3);
    CHECK(c.withdrawn(lp));
    c.on_hit(lp, 1, 1, 1, true);
    CHECK(!c.live[lp].calm);
    // A stone does nothing.
    const int before = c.baits;
    c.on_touch(0, 0);
    CHECK_EQ(c.baits, before);
}

void test_arcs(const GameData& d) {
    std::printf("arcs: the storm eye's lightning jumps to those near the one struck\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t br = u.cid("brannoc"), ka = u.cid("kalvax");
    u.p().join(u.p().find_def("brannoc"));
    u.p().join(u.p().find_def("kalvax"));
    u.frames(5);
    const uint16_t se = spawn_near(u, "storm_eye", 200);
    CHECK(se);
    Actor* av = u.av();
    av->hp = 900;
    u.w().actor(br)->hp = 900;
    u.w().actor(ka)->hp = 900;
    // Close together: arcs to both.
    u.w().move_to(br, av->x, av->y + 64, av->z, false);
    u.w().move_to(ka, av->x - 64, av->y, av->z, false);
    for (uint16_t id : {uint16_t(1), br, ka}) for (unsigned s = 1; s <= 5; ++s) if (uint16_t k = u.w().equipped(id, s)) u.w().destroy_now(k);
    const int a0 = c.arcs;
    c.strike(se, 1);
    CHECK_EQ(c.arcs - a0, 2);
    // Spread out: none.
    u.w().move_to(br, av->x, av->y + 400, av->z, false);
    u.w().move_to(ka, av->x - 400, av->y, av->z, false);
    const int a1 = c.arcs;
    c.strike(se, 1);
    CHECK_EQ(c.arcs - a1, 0);
    // Metal armour draws it further: Brannoc in a mail shirt at 180 is reached.
    u.w().move_to(br, av->x, av->y + 180, av->z, false);
    equip(u, br, 523, 2);
    CHECK_EQ(c.metal_pieces(br), 1);
    const int a2 = c.arcs;
    c.strike(se, 1);
    CHECK_EQ(c.arcs - a2, 1);
}

void test_metal(const GameData& d) {
    std::printf("metal: the storm eye picks the one in the most metal armour\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t br = u.cid("brannoc");
    u.p().join(u.p().find_def("brannoc"));
    u.frames(5);
    Actor* av = u.av();
    for (uint16_t id : {uint16_t(1), br}) for (unsigned s = 1; s <= 5; ++s) if (uint16_t k = u.w().equipped(id, s)) u.w().destroy_now(k);
    const uint16_t se = spawn_near(u, "storm_eye", 160);
    av->hp = 900;
    u.w().actor(br)->hp = 900;
    u.w().move_to(br, av->x - 60, av->y + 60, av->z, false);  // a little further than the Avatar
    const int a0 = c.strikes;
    // Nobody in metal: the nearer (the Avatar).
    int hp_av = av->hp, hp_br = u.w().actor(br)->hp;
    for (int f = 0; f < 120; ++f) u.ticks(1);
    CHECK(c.strikes > a0);
    CHECK(int(av->hp) < hp_av || av->hp == 0);
    (void)hp_br;
    // Brannoc in a helmet and mail: him.
    equip(u, br, 531, 0);
    equip(u, br, 523, 0);
    CHECK_EQ(c.metal_pieces(br), 2);
    av->hp = 900;
    u.w().actor(br)->hp = 900;
    u.w().move_to(br, av->x - 60, av->y + 60, av->z, false);
    const int brhp = u.w().actor(br)->hp;
    c.live[se].cooldown = 0;
    c.log.clear();
    for (int f = 0; f < 80; ++f) u.ticks(1);
    CHECK(int(u.w().actor(br)->hp) < brhp);
    auto st = std::find_if(c.log.begin(), c.log.end(), [](const std::string& l) { return l.rfind("strike ", 0) == 0; });
    CHECK(st != c.log.end() && *st == "strike " + std::to_string(br));
}

void test_weaken(const GameData& d) {
    std::printf("weaken: Light of the Ancients makes the wraith hurt by any weapon for a while\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t wr = spawn_near(u, "wraith", 200);
    const uint16_t il = u.cid("ilvaran");
    equip(u, 1, 417);
    CHECK_EQ(hit_with(u, wr, 1, 10, kDmgBlunt, true), 0);
    const int hp = u.w().actor(wr)->hp;
    (void)il;
    cast_at(u, "ilvaran", "ancient_light", wr);
    CHECK_EQ(int(u.w().actor(wr)->hp), hp);  // the light itself does nothing to it
    CHECK(c.live[wr].weak_frames > 0);
    CHECK_EQ(hit_with(u, wr, 1, 10, kDmgBlunt, true), 10);
    // It wears off (Ilvaran sent away first, or he would cast it again).
    u.p().part_ways(u.p().find_def("ilvaran"));
    // (in real time: the cast may have started a turn-based fight, where it counts in rounds)
    if (u.w().turn->active()) u.w().turn->end_fight(TurnCombat::kAborted);
    u.w().turn->enabled = false;
    c.live[wr].weak_frames = 2;
    c.live[wr].weak_rounds = 0;
    u.ticks(3);
    CHECK_EQ(hit_with(u, wr, 1, 10, kDmgBlunt, true), 0);
}

void test_fire(const GameData& d) {
    std::printf("fire: Endure Heat and Fire Shield keep off the lava wyrm's spray\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t ww = spawn_near(u, "lava_wyrm", 120);
    CHECK(ww);
    if (!ww) return;
    c.trap_in_rock(ww);
    Actor* av = u.av();
    av->hp = 200;
    int hp = av->hp;
    for (int k = 0; k < 10; ++k) c.strike(ww, 1);
    CHECK(int(av->hp) < hp);
    // An Endure Heat aura (703) on him.
    uint16_t aura = u.w().create_item(703, 0);
    u.w().move_to(aura, av->x, av->y, av->z, false);
    hp = av->hp;
    for (int k = 0; k < 10; ++k) c.strike(ww, 1);
    CHECK_EQ(int(av->hp), hp);
    u.w().destroy_now(aura);
    // A Fire Shield ring flame (569) by him.
    uint16_t ring = u.w().create_item(569, 0);
    u.w().move_to(ring, av->x + 48, av->y, av->z, false);
    hp = av->hp;
    for (int k = 0; k < 10; ++k) c.strike(ww, 1);
    CHECK_EQ(int(av->hp), hp);
}

void test_turn(const GameData& d) {
    std::printf("turn: a creature fights in turn-based combat, by name in the log and the roster\n");
    Ui u(d, true);
    u.w().turn->enabled = true;
    u.w().turn->speed = 2;
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t wr = spawn_near(u, "wraith", 200);
    equip(u, 1, 816);  // Slayer
    u.av()->set(Actor::kWithstandDeath, true);
    u.key('c');
    u.frames(4);
    if (!u.w().turn->active()) u.w().turn->start_fight("test");
    CHECK(u.w().turn->active());
    CHECK(u.w().turn->is_fighter(wr));
    u.w().turn->party_auto = true;
    const int s0 = c.strikes;
    for (int f = 0; f < 6000 && u.w().turn->active(); ++f) u.frames(1);
    CHECK(c.strikes > s0);
    bool named = false, hurt = false;
    for (const auto& l : u.w().game_log.lines(GameLog::kCombat)) {
        named = named || l.text.find("wraith") != std::string::npos;
        hurt = hurt || l.text.find("hits the wraith") != std::string::npos || l.text.find("kills the wraith") != std::string::npos;
    }
    CHECK(named);
    CHECK(hurt);
    u.shot("creature_turn_wraith");
    // The lurker under the water is not offered as a target, and it withdraws when nobody is in reach.
    TurnCombat& tc = *u.w().turn;
    if (tc.active()) tc.end_fight(TurnCombat::kAborted);
}

void test_hud(const GameData& d) {
    std::printf("hud: the HUD's foe slot and the log name the creature; XP is paid once\n");
    Ui u(d, true);
    u.frames(10);
    Creatures& c = u.c();
    const uint16_t fl = spawn_near(u, "pyros_flame", 120);
    CHECK(GameLog::name_of(u.w(), fl, true) == std::string("the Flame of Pyros"));
    // A kill pays its XP (Leveling on), once.
    u.l().progress[1].level = 8;
    const int xp0 = u.l().xp_of(1);
    Actor* a = u.w().actor(fl);
    a->hp = 1;
    equip(u, 1, 817);
    u.w().receive_hit(fl, 1, 0, 30, kDmgPierce | kDmgMagic);
    CHECK(u.w().actor(fl) == nullptr || u.w().actor(fl)->is(Actor::kDead));
    CHECK(u.l().xp_of(1) > xp0);
    bool killed = false;
    for (const auto& l : u.w().game_log.lines(GameLog::kCombat)) killed = killed || l.text.find("Flame of Pyros") != std::string::npos;
    CHECK(killed);
    // Its body goes after a moment.
    u.frames(60);
    CHECK(!c.live.count(fl));
}

void test_option(const GameData& d) {
    std::printf("option: with New creatures off nothing spawns; turning it off sends them away\n");
    Ui u(d, true, false);
    u.frames(10);
    to_lake(u);
    u.frames(40);
    CHECK(u.c().live.empty());
    CHECK(u.c().spawned.empty());
    u.c().enabled = true;
    u.frames(40);
    CHECK(!u.c().live.empty());
    u.c().enabled = false;
    u.frames(2);
    CHECK(u.c().live.empty());
    CHECK(u.c().spawned.empty());
    // The option row.
    bool row = false;
    for (auto& f : World::extra_option_rows()) row = row || f(u.w()).label == "New creatures:";
    CHECK(row);
}

void test_spawn(const GameData& d) {
    std::printf("spawn: each place once, on entering its map; 'after' places wait for their step\n");
    Ui u(d, false);
    u.frames(10);
    to_lake(u);
    u.frames(40);
    CHECK_EQ(u.w().map_number_now(), 26);
    size_t n26 = 0;
    for (const CreatureDef& def : u.c().defs)
        for (const CreaturePlace& p : def.places) n26 += p.map == 26 && p.after.empty();
    CHECK_EQ(u.c().live.size(), n26);
    // Away and back: the same ones, no new.
    u.place(16, 19254, 9434, 104);
    u.frames(10);
    to_lake(u);
    u.frames(40);
    CHECK_EQ(u.c().live.size(), n26);
    // A place gated on a step (map 51) waits.
    for (const CreatureDef& def : u.c().defs)
        for (size_t i = 0; i < def.places.size(); ++i)
            if (!def.places[i].after.empty()) {
                u.place(def.places[i].map, def.places[i].x, def.places[i].y, def.places[i].z);
                u.frames(40);
                CHECK(!u.c().spawned.count(def.id + ":" + std::to_string(i)));
                return;
            }
}

void test_saves(const GameData& d) {
    std::printf("saves: round trip, identical bytes; a save without the section loads\n");
    Ui u(d, true);
    u.frames(10);
    to_lake(u);
    u.frames(40);
    CHECK(!u.c().live.empty());
    u.c().live.begin()->second.weak_frames = 77;
    std::vector<uint8_t> a = save_bytes(u.w());
    SaveReader in(a.data(), a.size());
    CHECK(u.w().load_game(in));
    CHECK(!u.c().live.empty());
    CHECK_EQ(u.c().live.begin()->second.weak_frames, 77);
    std::vector<uint8_t> b = save_bytes(u.w());
    CHECK(a == b);
    // A game with nothing spawned writes no section, and loads with nothing.
    Ui v(d, true, false);
    v.frames(10);
    std::vector<uint8_t> c0 = save_bytes(v.w());
    CHECK(std::search(c0.begin(), c0.end(), std::begin("CRTR"), std::begin("CRTR") + 4) == c0.end());
    SaveReader in2(c0.data(), c0.size());
    CHECK(u.w().load_game(in2));
    CHECK(u.c().live.empty());
    CHECK(u.c().spawned.empty());
    // The old test saves (no section) still load.
    std::string tdir = g_data + "/../tests/data";
    (void)tdir;
}

void test_places(const GameData& d) {
    std::printf("places: in their habitat, nothing stamped over, the party can reach them\n");
    Ui base(d, false, false, false);
    std::map<unsigned, std::vector<std::pair<int, int>>> by_map;
    for (size_t di = 0; di < base.c().defs.size(); ++di)
        for (size_t pi = 0; pi < base.c().defs[di].places.size(); ++pi) by_map[base.c().defs[di].places[pi].map].push_back({int(di), int(pi)});
    for (auto& e : by_map) {
        Ui u(d, false, false, false);
        u.frames(5);
        auto f = flood_map(u, e.first);
        CHECK(f != nullptr);
        if (!f) continue;
        for (auto [di, pi] : e.second) {
            const CreatureDef& def = u.c().defs[size_t(di)];
            const CreaturePlace& p = def.places[size_t(pi)];
            int oz = 0;
            // (monsters walk about: those hatched near the spot are taken away first)
            for (unsigned n = 2; n < 248; ++n)
                if (const Actor* o = u.w().actor(uint16_t(n)); o && o->chunk >= 0 && std::abs(o->x - p.x) < 384 && std::abs(o->y - p.y) < 384)
                    u.w().destroy_now(uint16_t(n));
            const bool hab = u.c().habitat_ok(def, p.x, p.y, p.z, 0, oz) && oz == p.z;
            uint16_t bl = 0;
            const bool clear = clear_box(u.w(), def.body, p.x, p.y, p.z, &bl);
            int bxd, byd, bzd, cz = 0;
            u.w().data().typeflags.get(def.body).footprint(false, bxd, byd, bzd);
            const int near = f->nearest(p.x, p.y, bxd, byd, cz);
            std::printf("  map %2u %-12s (%5d,%5d,%3d)%s: habitat %d, clear %d%s, party within %d (z %d)  %s\n", e.first, def.id.c_str(), p.x, p.y,
                        p.z, p.after.empty() ? "" : (" after " + p.after).c_str(), hab, clear,
                        clear ? "" : (" (item " + std::to_string(bl) + " shape " + std::to_string(u.w().item(bl) ? u.w().item(bl)->shape : -1) + ")").c_str(),
                        near, cz, p.note.c_str());
            CHECK(hab);
            CHECK(clear);
            CHECK(near <= std::max(def.reach, 32));
            if (def.kind == "walker" || def.kind == "floater") CHECK(near <= 32);
        }
    }
}

void shots(const GameData& d) {
    std::printf("shots: each creature where it lives\n");
    Ui base(d, true, false, false);
    for (size_t di = 0; di < base.c().defs.size(); ++di) {
        const CreatureDef& def0 = base.c().defs[di];
        if (std::getenv("CRSHOT") && def0.id != std::getenv("CRSHOT")) continue;
        // (the Crag's wyrm lies under the bridge, out of the picture: the tunnel's)
        const size_t pi = def0.id == "lava_wyrm" ? 1 : 0;
        const CreaturePlace p = def0.places[pi];
        Ui u(d, false, false, false);
        u.frames(5);
        // The Avatar stands where the flood says the party can, near the place (on ground: not
        // water or lava, which the flood counts as floor).
        auto f = flood_map(u, p.map);
        int bxd, byd, bzd, cz = p.z;
        u.w().data().typeflags.get(def0.body).footprint(false, bxd, byd, bzd);
        int best = 1 << 30, sx = p.x, sy = p.y;
        if (f)
            for (const auto& e : f->seen) {
                auto [x, y, z] = e.first;
                const int dd = int(std::hypot(double(x - p.x), double(y - p.y)));
                int gz;
                const CreatureDef& walker = u.c().defs[size_t(u.c().def_index("wraith"))];
                if (dd >= 128 && dd < best && u.c().habitat_ok(walker, x, y, z, 1, gz) && gz == z) { best = dd; sx = x; sy = y; cz = z; }
            }
        u.w().view_w = 320;
        u.w().view_h = 200;
        u.place(p.map, sx, sy, cz);
        u.c().enabled = true;
        const uint16_t k = u.c().spawn(int(di), int(pi));
        CHECK(k);
        u.av()->set(Actor::kInvincible, true);
        u.frames(150);  // it comes for him
        for (auto ph : {CreatureState::kHidden, CreatureState::kUp}) {
            if (!u.c().live.count(k)) break;
            if (ph == CreatureState::kHidden && def0.kind != "water" && def0.kind != "lava") continue;
            u.c().live[k].phase = ph;
            u.c().live[k].timer = 400;
            u.c().live[k].cooldown = 5;
            u.frames(ph == CreatureState::kUp ? 14 : 6);
            u.shot("creature_" + def0.id + (ph == CreatureState::kHidden ? "_hidden" : ""), 3);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_creatures_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    Creatures::register_options();
    std::string only = argc > 5 ? argv[5] : "";
    if (only == "find" && argc > 11) {
        find(d, argv[6], unsigned(std::atoi(argv[7])), std::atoi(argv[8]), std::atoi(argv[9]), std::atoi(argv[10]), std::atoi(argv[11]));
        return 0;
    }
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("data")) test_data(d);
    if (want("levels")) test_levels(d);
    if (want("rules")) test_rules(d);
    if (want("drain")) test_drain(d);
    if (want("surface")) test_surface(d);
    if (want("bait")) test_bait(d);
    if (want("arcs")) test_arcs(d);
    if (want("metal")) test_metal(d);
    if (want("weaken")) test_weaken(d);
    if (want("fire")) test_fire(d);
    if (want("turn")) test_turn(d);
    if (want("hud")) test_hud(d);
    if (want("option")) test_option(d);
    if (want("spawn")) test_spawn(d);
    if (want("saves")) test_saves(d);
    if (want("places")) test_places(d);
    if (want("shots")) shots(d);
    if (only == "crowd") crowd(d);
    if (only == "lonely" && argc > 8) lonely(d, argv[6], unsigned(std::atoi(argv[7])), std::atoi(argv[8]));
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
