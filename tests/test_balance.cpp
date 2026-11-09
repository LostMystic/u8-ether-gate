// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The headless fight simulator for Leveling's monster tuning (PLAN-LEVELING.md 6; NOTES.md "Stat
// effects and monster tuning"). Fights are played by the real game: the real game mode and world,
// a level-N Avatar (stage 2's gains, typical equipment for that point in the story) and level-N
// companions against one monster of a level-N variant from data/monster_levels.json, with the real
// combat code (animations, the AI, to-hit, armour, dodge, spells) in turn-based combat on Party
// Auto (the default style) or in real time (an attack-spamming Avatar). Nothing is a formula.
//
// The Avatar is given "withstand death" as a marker: when it is used up he would have died, and
// the fight counts as lost.
//
//   pagan_balance_tests GAME_DIR DATA_DIR [fights] [mode] [only]   (SIMTRACE=1: every fight's end)
//     fights: per row (default 24); mode: check (default), table, full (every variant, both
//     styles), tune; only: a monster name.
// check: the balance table for one variant per level and style, and fails if tuning drifts badly.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/companion_magic.h"
#include "game/creatures.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_data;

// ---------------------------------------------------------------------------------------------
// The characters of each level (stage 2's rules) and their typical equipment.

struct Stats { int str, dex, intel; };

// The Avatar: +1 each a level, and a chosen point a level, put alternately into STR and DEX (a
// fighting Avatar; the INT-heavy player is weaker in melee and stronger with spells).
Stats avatar_stats(int lv) {
    const int n = lv - 1;
    return {kAvatarStartStr + n + (n + 1) / 2, kAvatarStartDex + n + n / 2, kAvatarStartInt + n};
}

struct Kit {
    unsigned weapon;
    std::vector<std::pair<unsigned, unsigned>> armour;  // shape, frame
};

// What a player who buys from Darion and loots as he goes wears at each level (not the hidden magic
// armour). Armour class 1, 5, 9, 12, 16, 17, 18, 21 (the last with Deceiver's 2).
Kit avatar_kit(int lv) {
    switch (lv) {
    case 1: return {417, {{823, 0}}};                                         // mace, clothes
    case 2: return {417, {{823, 0}, {531, 2}, {532, 0}}};                     // + helmet, leggings
    case 3: return {420, {{531, 0}, {523, 4}, {532, 0}, {530, 4}}};           // sword
    case 4: return {420, {{531, 0}, {523, 2}, {532, 2}, {530, 0}}};
    case 5: return {419, {{531, 0}, {523, 0}, {532, 2}, {530, 0}, {539, 2}}};  // axe, a shield
    case 6: return {419, {{531, 0}, {523, 0}, {532, 2}, {530, 6}, {539, 2}}};
    case 7: return {817, {{843, 0}, {523, 0}, {532, 2}, {530, 6}, {539, 2}}};  // Flame Sting, magic helm
    default: return {822, {{843, 0}, {523, 0}, {532, 2}, {530, 6}, {539, 0}}};  // Deceiver
    }
}

int kit_ac(const Kit& k) {
    int ac = 0;
    for (auto& p : k.armour) {
        Item it;
        it.shape = uint16_t(p.first);
        it.frame = uint16_t(p.second);
        ac += item_armour(it);
    }
    if (const WeaponInfo* wi = weapon_info(k.weapon)) ac += wi->armour;
    return ac;
}

// A player swaps to a weapon that can hurt the foe at all: a blade against a torax or a golem's
// blade guard does nothing (combat.md 1), so a mace (or the axe's blunt half) is used instead.
Kit kit_against(Kit k, unsigned foe) {
    const MonsterInfo* m = monster_info(foe);
    const WeaponInfo* wi = weapon_info(k.weapon);
    if (!m || !wi) return k;
    auto left = [&](uint16_t t) { return uint16_t(t & ~(m->def_type & ~(kDmgUndead | kDmgMagic | kDmgPierce))); };
    if (left(wi->type) == 0) k.weapon = 417;
    return k;
}

// ---------------------------------------------------------------------------------------------

struct Result {
    int fights = 0, wins = 0, timeouts = 0;
    double hp_left = 0;     // the Avatar's HP left, as a share of his maximum, over the wins
    double party_left = 0;  // the party's HP left (the dead count 0), over the wins
    double rounds = 0;      // turn-based rounds (or real-time seconds), over all fights
    int dodges = 0;
    double win_rate() const { return fights ? double(wins) / fights : 0; }
    double hp() const { return wins ? hp_left / wins : 0; }
    double party() const { return wins ? party_left / wins : 0; }
};

struct Sim {
    GameMode gm;
    bool turn;
    int fight_no = 0;
    bool broken = false;
    Sim(const GameData& d, bool turn_based) : gm(d), turn(turn_based) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = true;
        w.party->beach_always = true;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = true;
        // The new creatures (game/creatures.h): loaded for the creature fights; none of their places
        // fill (the simulator spawns its own).
        if (!w.creatures->load_dir(g_data, err)) std::printf("  creatures: %s\n", err.c_str());
        w.creatures->enabled = true;
        w.turn->enabled = turn_based;
        w.turn->speed = 2;
        w.turn->start_on_hit = true;
        gm.start();
        for (const CreatureDef& cd : w.creatures->defs)
            for (size_t i = 0; i < cd.places.size(); ++i) w.creatures->spawned.insert(cd.id + ":" + std::to_string(i));
        frames(60);
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Leveling& l() { return *gm.world().leveling; }
    void frames(int n) { for (int i = 0; i < n; ++i) gm.tick(); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e);
        e.type = InputEvent::kKeyUp; gm.handle(e);
    }
    uint16_t cid(const char* name) { int i = p().find_def(name); return i >= 0 ? p().state[size_t(i)].id : 0; }

    void equip_kit(uint16_t who, const Kit& k) {
        World& wd = w();
        uint16_t pack = wd.equipped(who, 7);
        // Off with the old (destroyed: the simulator's own items).
        if (Actor* a = wd.actor(who)) {
            std::vector<uint16_t> old;
            for (uint16_t c : a->contents)
                if (const Item* it = wd.item(c); it && (it->flags & kItemEquipped) && c != pack) old.push_back(c);
            for (uint16_t c : old) wd.destroy_now(c);
        }
        auto put = [&](unsigned shape, unsigned frame) {
            uint16_t id = wd.create_item(shape, frame);
            if (pack) wd.move_to_container(id, pack);
            if (!wd.equip(who, id)) wd.destroy_now(id);
        };
        put(k.weapon, 0);
        for (auto& a : k.armour) put(a.first, a.second);
    }

    // Sets up the party of `size` (the Avatar, Brannoc, Kalvax) at level `lv`, healed.
    std::vector<uint16_t> party_at(int lv, int size, std::vector<std::string> names = {"brannoc", "kalvax"}, const Kit* kit = nullptr) {
        World& wd = w();
        Actor* av = wd.avatar();
        Stats s = avatar_stats(lv);
        av->str = int16_t(s.str); av->dex = int16_t(s.dex); av->intel = int16_t(s.intel);
        av->hp = uint16_t(2 * av->str);
        av->mana = int16_t(2 * av->intel);
        av->set(Actor::kWithstandDeath, true);
        av->set(Actor::kPoisoned, false);
        l().progress[1].level = lv;
        l().magic->uses.clear();  // stage 5: every fight starts with the casters' full stock of uses
        equip_kit(1, kit ? *kit : kit_against(avatar_kit(lv), foe_));
        std::vector<uint16_t> ids{1};
        // Anyone else from an earlier fight leaves.
        for (int m : std::vector<int>(p().members))
            if (std::find(names.begin(), names.end(), p().defs[size_t(m)].id) == names.end()) {
                p().part_ways(m);
                if (Actor* a = wd.actor(p().state[size_t(m)].id)) {
                    a->set(Actor::kInCombat, false);
                    wd.move_to(a->id, 15200, 6900, 0, false);
                }
            }
        for (int k = 0; k < int(names.size()); ++k) {
            int i = p().find_def(names[size_t(k)]);
            if (i < 0) continue;
            const bool want = k + 2 <= size;
            if (!want) {
                if (p().in_party(i)) p().part_ways(i);
                if (Actor* a = wd.actor(p().state[size_t(i)].id)) {
                    a->set(Actor::kInCombat, false);
                    wd.move_to(a->id, 15200, 6900, 0, false);  // out of the way
                }
                continue;
            }
            if (!p().in_party(i)) p().join(i);
            if (p().state[size_t(i)].mode == CompanionState::kDead || (wd.actor(p().state[size_t(i)].id) &&
                                                                     wd.actor(p().state[size_t(i)].id)->is(Actor::kDead)))
                p().revive(i, 100);
            Actor* a = wd.actor(p().state[size_t(i)].id);
            if (!a) continue;
            const CompanionDef& d = p().defs[size_t(i)];
            a->str = int16_t(d.lv_str + d.gain_str * (lv - 1));
            a->dex = int16_t(d.lv_dex + d.gain_dex * (lv - 1));
            a->intel = int16_t(d.lv_int + d.gain_int * (lv - 1));
            l().progress[a->id].level = lv;
            a->hp = uint16_t(p().max_hp(*a));
            a->mana = int16_t(p().max_mana(*a));
            a->set(Actor::kPoisoned, false);
            if (k == 0) {  // Brannoc: his axe, the Avatar's armour
                Kit kk = kit ? *kit : avatar_kit(lv);
                kk.weapon = 419;
                equip_kit(a->id, kit_against(kk, foe_));
            }
            ids.push_back(a->id);
        }
        return ids;
    }

    void place_party(const std::vector<uint16_t>& ids) {
        World& wd = w();
        wd.kernel().kill(1, kProcAny);
        wd.move_to(1, 14300, 6150, 0, false);
        if (Actor* av = wd.avatar()) { av->set(Actor::kAnimLock, false); av->dir = 2; }
        for (size_t k = 1; k < ids.size(); ++k) {
            wd.kernel().kill(ids[k], kProcAny);
            if (Actor* a = wd.actor(ids[k])) a->set(Actor::kAnimLock, false);
            wd.move_to(ids[k], 14300 - 64, 6150 + (k == 1 ? -48 : 48), 0, false);
        }
    }

    // Everything hostile near the spot goes (the last fight's monster, its summons, bodies).
    void clear_hostiles() {
        World& wd = w();
        std::vector<uint16_t> near;
        wd.area_search(14300, 6150, 2600, false, near);
        for (uint16_t k : near) {
            const Actor* a = wd.actor(k);
            if (!a || k == 1 || p().is_companion(k) || (a->ext_flags & kExtPermanentNpc)) continue;
            wd.kernel().kill(k, kProcAny);
            wd.destroy_now(k);
        }
        for (auto& s : p().summons) if (wd.actor(s.id)) { wd.kernel().kill(s.id, kProcAny); wd.destroy_now(s.id); }
        p().summons.clear();
    }

    void leave_combat() {
        if (w().turn->active()) w().turn->end_fight(TurnCombat::kAborted);
        frames(2);
        if (w().avatar()->is(Actor::kInCombat)) { key('c'); frames(20); }
        w().avatar()->set(Actor::kInCombat, false);
        for (int m : p().members)
            if (Actor* a = w().actor(p().state[size_t(m)].id)) a->set(Actor::kInCombat, false);
        frames(5);
    }

    // One fight. Returns 1 won, 0 lost, -1 timed out.
    unsigned foe_ = 0;
    int fight(unsigned shape, int mlevel, int lv, int size, Result& r) {
        World& wd = w();
        foe_ = shape;
        leave_combat();
        clear_hostiles();
        wd.rng.seed(1000u + unsigned(fight_no++) * 7919u);
        std::vector<uint16_t> ids = party_at(lv, size);
        place_party(ids);
        frames(4);
        // The monster, 220 to 300 away.
        uint16_t m = wd.create_monster(shape, 0);
        Actor* ma = wd.actor(m);
        if (!ma) return -1;
        l().apply_monster(*ma, mlevel);
        ma->flags |= kItemFastOnly | kItemDisposable;
        bool placed = false;
        for (int rr = 220; rr <= 640 && !placed; rr += 20)
            for (int k = 0; k < 24 && !placed; ++k)
                for (int z : {0, 8, 16, 24}) {
                    double ang = 6.2831853 * k / 24 + 0.3 * (fight_no % 5);
                    int x = 14300 + int(rr * std::cos(ang)), y = 6150 + int(rr * std::sin(ang));
                    World::Placement pl = wd.can_exist_shape(shape, x, y, z, m);
                    if (pl.valid && pl.supported) { wd.move_to(m, x, y, z, false); placed = true; break; }
                }
        if (!placed) {
            std::printf("    %u: no room for it\n", shape);
            wd.destroy_now(m);
            ++r.fights;
            ++r.timeouts;
            return -1;
        }
        frames(2);
        if (turn) {
            key('c');
            frames(4);
            if (!wd.turn->active()) wd.turn->start_fight("simulator");
            wd.turn->party_auto = true;
            if (!wd.turn->active()) {
                // The world got into a state where no fight starts (the Avatar ran far off on a
                // previous fight): the caller starts a fresh game.
                broken = true;
                clear_hostiles();
                return -1;
            }
        } else {
            key('c');
            frames(4);
            wd.set_in_combat(m);
            if ((ma = wd.actor(m))) { ma->target = 1; ma->fixed_target = 1; }
        }
        int out = -1, ticks = 0;
        const int limit = turn ? 9000 : 5400;  // ticks (turn-based runs 4 world frames a tick)
        uint32_t last_swing = 0;
        for (; ticks < limit; ++ticks) {
            frames(1);
            Actor* av = wd.avatar();
            ma = wd.actor(m);
            if (!ma || ma->is(Actor::kDead)) { out = 1; break; }
            if (!av->is(Actor::kWithstandDeath) || av->is(Actor::kDead)) { out = 0; break; }
            if (!turn) {
                // The player spamming attack: as soon as the rules allow a swing that would land.
                const uint32_t now = wd.kernel().frame_number();
                const int delay = std::max(0, 25 - int(av->dex)) + l().attack_delay_extra(*av);
                if (!av->is(Actor::kAnimLock) && !wd.kernel().count_exact(1, kProcActorAnim) && now > last_swing + uint32_t(delay)) {
                    int dir = direction_to(ma->x - av->x, ma->y - av->y);
                    ActorState st;
                    st.x = av->x; st.y = av->y; st.z = av->z; st.dir = dir;
                    st.last_anim = av->last_anim; st.last_anim_dir = av->last_anim_dir;
                    st.first_step = av->is(Actor::kFirstStep); st.in_combat = true;
                    if (wd.turn->can_hit_from(1, st, m)) {
                        wd.do_anim(1, 7, dir);
                        last_swing = now;
                    } else if (!wd.kernel().count_exact(1, kProcPathfind)) {
                        wd.pathfind(1, 0, 0, 0, m, 0);  // walks up to it, as a player would
                    }
                }
            } else if (!wd.turn->active()) {
                // The fight ended by itself: the party ran out of the area (Auto backs off when badly
                // hurt), a loss; anything else (a fall back to real time) is no result.
                if (wd.turn->last_end == TurnCombat::kEscape) out = 0;
                break;
            }
        }
        Actor* av = wd.avatar();
        if (std::getenv("SIMTRACE"))
            std::printf("    fight %d: %s after %d ticks, rounds %d, avatar %d/%d at %d,%d, foe hp %d, turn %d end %d\n", fight_no,
                        out == 1 ? "won" : out == 0 ? "lost" : "timeout", ticks, wd.turn->round(), av->hp, 2 * av->str, av->x, av->y,
                        wd.actor(m) ? int(wd.actor(m)->hp) : -1, int(wd.turn->active()), int(wd.turn->last_end));
        if (std::getenv("SIMTRACE"))
            for (const auto& fb : wd.turn->fallbacks) std::printf("      fallback %u (%u): %s\n", fb.id, fb.shape, fb.why.c_str());
        ++r.fights;
        r.rounds += turn ? wd.turn->round() : ticks / 30.0;
        if (out == 1) {
            ++r.wins;
            r.hp_left += double(av->hp) / std::max(1, 2 * int(av->str));
            double tot = 0;
            for (uint16_t id : ids) {
                const Actor* a = wd.actor(id);
                if (!a || a->is(Actor::kDead)) continue;
                tot += double(a->hp) / std::max(1, id == 1 ? 2 * int(a->str) : p().max_hp(*a));
            }
            r.party_left += tot / double(ids.size());
        } else if (out < 0) {
            ++r.timeouts;
        }
        av->set(Actor::kWithstandDeath, false);
        leave_combat();
        clear_hostiles();
        return out;
    }

    // --- The new creatures (game/creatures.h, PLAN-CREATURES.md): one at its level against the
    // level-N Avatar and two companions, with the right tactic or the wrong one.
    Creatures& cr() { return *gm.world().creatures; }
    struct Spot { int x = 0, y = 0, z = 0; bool ok = false; };
    // Where a creature can be near (x, y) on this map, and a spot the party can stand on near it.
    Spot creature_spot(const CreatureDef& d, int x, int y, int z) {
        Spot sp;
        for (int r = 0; r <= 900 && !sp.ok; r += 32)
            for (int k = 0; k < 16 && !sp.ok; ++k) {
                int nx = x + int(r * std::cos(k * 0.3927)), ny = y + int(r * std::sin(k * 0.3927)), nz;
                if (cr().habitat_ok(d, nx, ny, z, 0, nz)) sp = {nx, ny, nz, true};
            }
        return sp;
    }
    Spot stand_spot(int x, int y, int z, int rmin, int rmax, bool ground_only) {
        Spot sp;
        World& wd = w();
        const CreatureDef& walker = cr().defs[size_t(cr().def_index("wraith"))];
        for (int r = rmin; r <= rmax && !sp.ok; r += 16)
            for (int k = 0; k < 24 && !sp.ok; ++k)
                for (int dz : {0, 8, 16, 24, -8}) {
                    int nx = x + int(r * std::cos(k * 0.2618)), ny = y + int(r * std::sin(k * 0.2618)), nz = z + dz;
                    World::Placement pl = wd.can_exist_shape(1, nx, ny, nz, 1);
                    if (!pl.valid || !pl.supported) continue;
                    if (const Item* land = wd.item(pl.land); land && land->shape == 409 && land->frame == 33) continue;  // deadly lava
                    int gz;
                    if (ground_only && !cr().habitat_ok(walker, nx, ny, nz, 1, gz)) continue;
                    sp = {nx, ny, nz, true};
                    break;
                }
        return sp;
    }
    int creature_fight(const std::string& id, bool right, Result& r) {
        World& wd = w();
        const int di = cr().def_index(id);
        if (di < 0) return -1;
        const CreatureDef& d = cr().defs[size_t(di)];
        const int lv = d.level;
        leave_combat();
        clear_hostiles();
        for (auto& e : std::map<uint16_t, CreatureState>(cr().live)) { wd.kernel().kill(e.first, kProcAny); wd.destroy_now(e.first); }
        cr().live.clear();
        wd.rng.seed(5000u + unsigned(fight_no++) * 7919u);
        // The party for each creature and tactic (names: the two companions).
        std::vector<std::string> names{"brannoc", "kalvax"};
        Kit kit = avatar_kit(lv);
        if (id == "wraith") {
            if (right) names = {"brannoc", "ilvaran"};  // an artifact blade (Flame Sting at 7) and the Light
            else kit.weapon = 419;                      // an axe, and fire spells
        } else if (id == "lava_wyrm") {
            if (right) names = {"brannoc", "nerissa"};  // Endure Heat on everyone, ice
        } else if (id == "pyros_flame") {
            if (right) names = {"brannoc", "nerissa"};  // Deceiver (an artifact) and the Tempest
            else kit.weapon = 420;                      // a sword, and fire
        } else if (id == "storm_eye") {
            if (right) kit.armour = {{823, 0}, {523, 4}, {530, 4}, {532, 0}};  // leather, no metal
        }
        foe_ = d.body;
        std::vector<uint16_t> ids = party_at(lv, 3, names, &kit);
        // The arena.
        unsigned map = 3;
        int ax = 14300, ay = 6150, az = 0;
        if (id == "lava_wyrm") { map = 24; ax = d.places[1].x; ay = d.places[1].y; az = d.places[1].z; }
        if (wd.map_number_now() != map) {
            // In by the tunnel's north entry (safe ground), then to the arena.
            wd.kernel().kill(1, kProcAny);
            wd.teleport_actor(1, 20287, 24191, 64, map);
            frames(10);
            for (size_t k = 1; k < ids.size(); ++k) wd.teleport_actor(ids[k], 20287, 24191, 64, map);
            frames(4);
        }
        Spot cs, ps;
        if (d.kind == "water") {
            cs = creature_spot(d, 14800, 6500, 0);
            ps = stand_spot(cs.x, cs.y, 0, 64, 400, true);
        } else if (d.kind == "lava") {
            // (the search needs the arena in the fast area: all of the map for a moment)
            const int vw = wd.view_w, vh = wd.view_h;
            wd.view_w = wd.view_h = 65000;
            frames(2);
            // The party on firm ground by the lava (the AI would walk them into deep lava otherwise),
            // the wyrm in the nearest lava.
            ps = stand_spot(ax, ay, az, 0, 1600, true);
            cs = creature_spot(d, ps.x, ps.y, ps.z);
            if (std::getenv("SIMTRACE")) std::printf("    arena: %d,%d,%d (%d) party %d,%d,%d (%d)\n", cs.x, cs.y, cs.z, cs.ok, ps.x, ps.y, ps.z, ps.ok);
            wd.view_w = vw;
            wd.view_h = vh;
        } else {
            ps = {14300, 6150, 0, true};
            cs = stand_spot(14300, 6150, 0, 240, 320, true);
        }
        if (!cs.ok || !ps.ok) { std::printf("    %s: no arena\n", id.c_str()); ++r.fights; ++r.timeouts; return -1; }
        wd.kernel().kill(1, kProcAny);
        wd.move_to(1, ps.x, ps.y, ps.z, false);
        if (Actor* av = wd.avatar()) { av->set(Actor::kAnimLock, false); av->set(Actor::kInCombat, false); }
        const bool spread = id == "storm_eye" && right;
        for (size_t k = 1; k < ids.size(); ++k) {
            wd.kernel().kill(ids[k], kProcAny);
            if (Actor* a = wd.actor(ids[k])) a->set(Actor::kAnimLock, false);
            Spot s2 = stand_spot(ps.x, ps.y, ps.z, spread ? 300 : 48, spread ? 500 : 160, d.kind == "water");
            if (s2.ok) wd.move_to(ids[k], s2.x + (spread ? 0 : int(k) * 8), s2.y + (spread && k == 2 ? 300 : 0), s2.z, false);
            if (spread && k == 2) { Spot s3 = stand_spot(ps.x - 300, ps.y + 300, ps.z, 0, 300, true); if (s3.ok) wd.move_to(ids[k], s3.x, s3.y, s3.z, false); }
        }
        frames(4);
        const uint16_t m = cr().spawn_at(di, cs.x, cs.y, cs.z);
        if (!m) { ++r.fights; ++r.timeouts; return -1; }
        if (Actor* ma = wd.actor(m)) ma->flags |= kItemFastOnly | kItemDisposable;
        // Endure Heat: an aura (703) kept on each of the party (as the spell's, creature_facts.md 2.3).
        std::vector<uint16_t> auras;
        if (id == "lava_wyrm" && right)
            for (uint16_t pid : ids) {
                uint16_t a = wd.create_item(703, 0);
                if (Item* ai = wd.item(a)) ai->flags |= kItemFastOnly | kItemDisposable;
                auras.push_back(a);
                (void)pid;
            }
        auto keep_auras = [&]() {
            for (size_t k = 0; k < auras.size() && k < ids.size(); ++k)
                if (const Actor* pa = wd.actor(ids[k]); pa && wd.item(auras[k])) wd.move_to(auras[k], pa->x, pa->y, pa->z, false);
        };
        keep_auras();
        // Bait: torax ribs thrown into the water by it.
        if (id == "lurker_pet" && right) {
            // Into open water a little way from it, toward the party.
            Spot bs;
            for (int rr = 96; rr <= 320 && !bs.ok; rr += 32)
                for (int k = 0; k < 16 && !bs.ok; ++k) {
                    int nx = cs.x + int(rr * std::cos(k * 0.3927)), ny = cs.y + int(rr * std::sin(k * 0.3927)), nz;
                    if (cr().habitat_ok(d, nx, ny, cs.z, m, nz)) bs = {nx, ny, nz, true};
                }
            uint16_t meat = wd.create_item(537, 2);
            wd.move_to(meat, bs.ok ? bs.x - 48 : cs.x, bs.ok ? bs.y - 48 : cs.y, cs.z + 40, false);
            wd.fall(meat);
            const int b0 = cr().baits;
            for (int f = 0; f < 90 && cr().baits == b0; ++f) frames(1);
        }
        frames(2);
        key('c');
        frames(4);
        if (!wd.turn->active()) wd.turn->start_fight("simulator");
        wd.turn->party_auto = true;
        int out = -1, ticks = 0;
        for (; ticks < 9000; ++ticks) {
            frames(1);
            keep_auras();
            Actor* av = wd.avatar();
            const Actor* ma = wd.actor(m);
            if (!ma || ma->is(Actor::kDead)) { out = 1; break; }
            if (!av->is(Actor::kWithstandDeath) || av->is(Actor::kDead)) { out = 0; break; }
            if (!wd.turn->active()) {
                if (wd.turn->last_end == TurnCombat::kEscape) out = 0;
                break;  // (a creature that lost interest: no result)
            }
        }
        Actor* av = wd.avatar();
        if (std::getenv("SIMTRACE"))
            std::printf("    %s %s fight %d: %s after %d ticks, rounds %d, avatar %d/%d, foe hp %d, strikes %d arcs %d\n", id.c_str(),
                        right ? "right" : "wrong", fight_no, out == 1 ? "won" : out == 0 ? "lost" : "timeout", ticks, wd.turn->round(), av->hp,
                        2 * av->str, wd.actor(m) ? int(wd.actor(m)->hp) : -1, cr().strikes, cr().arcs);
        if (std::getenv("SIMTRACE")) {
            std::printf("      baits %d, last:", cr().baits);
            const auto& lg = cr().log;
            for (size_t k = lg.size() > 14 ? lg.size() - 14 : 0; k < lg.size(); ++k) std::printf(" [%s]", lg[k].c_str());
            std::printf("\n");
        }
        ++r.fights;
        r.rounds += wd.turn->round();
        if (out == 1) {
            ++r.wins;
            r.hp_left += double(av->hp) / std::max(1, 2 * int(av->str));
            double tot = 0;
            for (uint16_t pid : ids) {
                const Actor* a = wd.actor(pid);
                if (!a || a->is(Actor::kDead)) continue;
                tot += double(a->hp) / std::max(1, pid == 1 ? 2 * int(a->str) : p().max_hp(*a));
            }
            r.party_left += tot / double(ids.size());
        } else if (out < 0) {
            ++r.timeouts;
        }
        av->set(Actor::kWithstandDeath, false);
        if (av->is(Actor::kDead) || wd.game_over) broken = true;  // (a fresh game for the next)
        for (uint16_t a : auras) if (wd.item(a)) wd.destroy_now(a);
        leave_combat();
        if (wd.actor(m)) { wd.kernel().kill(m, kProcAny); wd.destroy_now(m); }
        cr().live.clear();
        // Back to the start beach for the next fight.
        if (wd.map_number_now() != 3) {
            wd.kernel().kill(1, kProcAny);
            wd.teleport_actor(1, 14300, 6150, 8, 3);
            frames(10);
            for (size_t k = 1; k < ids.size(); ++k) wd.teleport_actor(ids[k], 14300 - 64, 6150, 8, 3);
            frames(4);
        }
        return out;
    }
};

// ---------------------------------------------------------------------------------------------
// Targets (PLAN-LEVELING.md 6): level 1 a fair fight alone, 2 harder alone, 3 tough but winnable
// alone, 4 and up about even for 2 to 3 party members. As numbers, for one monster of level N
// against characters of level N: the win rate band, and the band of the Avatar's HP left when won.
struct Target { int size; double win_lo, win_hi; const char* words; };
Target target_for(int lv) {
    switch (lv) {
    case 1: return {1, 0.80, 1.00, "fair fight, solo"};
    case 2: return {1, 0.65, 0.95, "harder, solo"};
    case 3: return {1, 0.45, 0.85, "tough but winnable, solo"};
    default: return {2, 0.35, 0.75, "about even for 2 (3 win easily)"};
    }
}

struct Row {
    std::string name;
    unsigned shape;
    int level;
};

// Not duels: a ghost or an air servant cannot be hurt without magic (it harms through what it
// summons), a fire skull blows itself up, vermin are level 1 nuisances.
bool no_duel(unsigned shape) { return shape == 411 || shape == 377 || shape == 413 || shape == 806 || shape == 807; }

std::vector<Row> variants(const Leveling& l, const std::string& only) {
    std::vector<Row> out;
    for (const auto& m : l.monsters)
        for (const MonsterVariant& v : m.second.variants)
            if (v.has_stats && v.xp > 0 && !no_duel(m.first) && m.first < GameData::kFirstCustomShape && (only.empty() || only == m.second.name))
                out.push_back({m.second.name, m.first, v.level});
    std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.level != b.level ? a.level < b.level : a.name < b.name; });
    return out;
}

void print_header(bool turn) {
    std::string acs;
    for (int lv = 1; lv <= kMaxLevel; ++lv) acs += (lv > 1 ? "," : "") + std::to_string(kit_ac(avatar_kit(lv)));
    std::printf("\n  %s: one level-N monster against level-N characters (the Avatar's AC by level %s)\n",
                turn ? "turn-based (Party Auto)" : "real time", acs.c_str());
    std::printf("  %-12s %2s %-5s %6s %8s %8s %7s %4s  %s\n", "monster", "N", "party", "won", "AvHP", "partyHP", turn ? "rounds" : "secs",
                "t/o", "target");
}

const GameData* g_gd = nullptr;
std::unique_ptr<Sim> new_sim(bool turn, int fight_no) {
    auto s = std::make_unique<Sim>(*g_gd, turn);
    s->fight_no = fight_no;
    return s;
}

// A creature fight; a fresh game now and then (a long run uses up the actor numbers) or when the
// world broke, with the creatures' numbers carried over (the tuner edits them).
void creature_run(std::unique_ptr<Sim>& s, const std::string& id, bool right, Result& r) {
    s->creature_fight(id, right, r);
    if (s->broken || s->fight_no % 30 == 0) {
        std::vector<CreatureDef> keep = s->cr().defs;
        const int no = s->fight_no;
        s.reset();
        s = new_sim(true, no);
        s->cr().defs = keep;
        s->cr().register_rows();
    }
}

Result run_row(std::unique_ptr<Sim>& s, const Row& r, int size, int n, bool print, bool check) {
    Result res;
    for (int k = 0, tries = 0; k < n && tries < 3 * n; ++tries) {
        int before = res.fights;
        s->fight(r.shape, r.level, r.level, size, res);
        if (s->broken) {
            // A fresh game, with the variant numbers carried over (the tuner edits them).
            std::map<unsigned, MonsterLevel> keep = s->l().monsters;
            const bool turn = s->turn;
            const int no = s->fight_no;
            s.reset();
            s = new_sim(turn, no);
            s->l().monsters = keep;
        }
        if (res.fights > before) ++k;
    }
    Target t = target_for(r.level);
    bool in = res.win_rate() >= t.win_lo - 1e-9 && res.win_rate() <= t.win_hi + 1e-9;
    if (print)
        std::printf("  %-12s %2d %-5d %5.0f%% %7.0f%% %7.0f%% %7.1f %4d  %s%s\n", r.name.c_str(), r.level, size, 100 * res.win_rate(),
                    100 * res.hp(), 100 * res.party(), res.fights ? res.rounds / res.fights : 0, res.timeouts,
                    size == t.size ? t.words : "", size == t.size ? (in ? "" : "  <- off target") : "");
    (void)check;
    if (std::getenv("SIMTRACE")) std::printf("    (%d dodges so far)\n", s->l().dodges);
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    const int n = argc > 3 ? std::max(1, std::atoi(argv[3])) : 24;
    const std::string mode = argc > 4 ? argv[4] : "check";
    const std::string only = argc > 5 ? argv[5] : "";
    Config::global().load("/tmp/pagan_balance_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    g_gd = &d;
    auto t0 = std::chrono::steady_clock::now();
    if (mode == "tune") {
        // For each variant: one factor k on its HP and damage, searched until the win rate is at the
        // middle of its target band. Prints the tuned numbers (data/monster_levels.json is edited
        // from these by hand or by a script).
        std::unique_ptr<Sim> s = new_sim(true, 0);
        const char* levels = std::getenv("TUNE_LEVELS");  // e.g. "2,6": only those levels
        for (const Row& r : variants(s->l(), only)) {
            if (levels && !std::strstr((std::string(",") + levels + ",").c_str(), ("," + std::to_string(r.level) + ",").c_str())) continue;
            MonsterVariant base;
            for (MonsterVariant& x : s->l().monsters[r.shape].variants) if (x.level == r.level) base = x;
            MonsterVariant* v = nullptr;
            auto var = [&]() -> MonsterVariant* {
                for (MonsterVariant& x : s->l().monsters[r.shape].variants) if (x.level == r.level) return &x;
                return nullptr;
            };
            Target t = target_for(r.level);
            const double want = (t.win_lo + t.win_hi) / 2;
            // TUNE_NARROW: a second pass around the data's numbers (0.6x to 1.6x).
            const bool narrow = std::getenv("TUNE_NARROW") != nullptr;
            double lo = narrow ? 0.6 : 0.08, hi = narrow ? 1.6 : 6.0, k = 1.0;
            Result last;
            auto apply = [&](double f) {
                v = var();
                v->hp0 = std::max(1, int(std::lround(base.hp0 * f)));
                v->hp1 = std::max(v->hp0, int(std::lround(base.hp1 * f)));
                v->dmg0 = std::max(0, int(std::lround(base.dmg0 * f)));
                v->dmg1 = std::max(std::max(1, v->dmg0), int(std::lround(base.dmg1 * f)));
            };
            for (int it = 0; it < (narrow ? 5 : 8); ++it) {
                k = std::sqrt(lo * hi);
                apply(k);
                last = run_row(s, r, t.size, n, false, false);
                if (last.win_rate() > want) lo = k; else hi = k;
            }
            k = std::sqrt(lo * hi);
            apply(k);
            last = run_row(s, r, t.size, n, false, false);
            v = var();
            std::printf("TUNE {\"shape\": %u, \"level\": %d, \"k\": %.3f, \"hp\": [%d, %d], \"damage\": [%d, %d], \"win\": %.2f, \"hp_left\": %.2f}\n",
                        r.shape, r.level, k, v->hp0, v->hp1, v->dmg0, v->dmg1, last.win_rate(), last.hp());
        }
        return 0;
    }
    if (mode == "ctune") {
        // The new creatures: one factor k on HP and damage, searched until the right tactic wins
        // about 55% (the middle of 35-75%). Prints the numbers for data/creatures.json.
        std::unique_ptr<Sim> s = new_sim(true, 0);
        const std::vector<CreatureDef> all = s->cr().defs;  // (s is replaced now and then)
        for (const CreatureDef& cd : all) {
            if (!only.empty() && only != cd.id) continue;
            const CreatureDef base = cd;
            double lo = 0.3, hi = 6.0, k = 1;
            Result last;
            auto apply = [&](double f) {
                for (CreatureDef& x : s->cr().defs) {
                    if (x.id != cd.id) continue;
                    x.hp0 = std::clamp(int(std::lround(base.hp0 * f)), 1, 999);
                    x.hp1 = std::clamp(int(std::lround(base.hp1 * f)), x.hp0, 999);
                    x.dmg0 = std::max(1, int(std::lround(base.dmg0 * f)));
                    x.dmg1 = std::max(x.dmg0, int(std::lround(base.dmg1 * f)));
                }
                s->cr().register_rows();
            };
            for (int it = 0; it < 7; ++it) {
                k = std::sqrt(lo * hi);
                apply(k);
                last = Result{};
                for (int f = 0; f < n; ++f) creature_run(s, cd.id, true, last);
                if (last.win_rate() > 0.55) lo = k; else hi = k;
            }
            k = std::sqrt(lo * hi);
            apply(k);
            Result r1, r0;
            for (int f = 0; f < n; ++f) creature_run(s, cd.id, true, r1);
            for (int f = 0; f < n; ++f) creature_run(s, cd.id, false, r0);
            for (const CreatureDef& x : s->cr().defs)
                if (x.id == cd.id)
                    std::printf("CTUNE {\"id\": \"%s\", \"k\": %.3f, \"hp\": [%d, %d], \"damage\": [%d, %d], \"right\": %.2f, \"wrong\": %.2f}\n",
                                x.id.c_str(), k, x.hp0, x.hp1, x.dmg0, x.dmg1, r1.win_rate(), r0.win_rate());
        }
        return 0;
    }
    if (mode == "creatures" || (mode == "check" && only.empty())) {
        // The new creatures: the right tactic should be a fair fight for three of their level
        // (35-75% won, as level 4 and up), the wrong one clearly worse.
        std::unique_ptr<Sim> s = new_sim(true, 0);
        const int cn = mode == "check" ? std::max(8, n / 2) : n;
        std::printf("\n  new creatures, turn-based (Party Auto): the Avatar and two companions of its level\n");
        std::printf("  %-12s %2s %-6s %6s %8s %8s %7s %4s\n", "creature", "N", "tactic", "won", "AvHP", "partyHP", "rounds", "t/o");
        const std::vector<CreatureDef> all = s->cr().defs;  // (s is replaced now and then)
        for (const CreatureDef& cd : all) {
            if (!only.empty() && only != cd.id) continue;
            Result res[2];
            for (int right = 1; right >= 0; --right) {
                for (int k = 0; k < cn; ++k) creature_run(s, cd.id, right == 1, res[right]);
                const Result& q = res[right];
                std::printf("  %-12s %2d %-6s %5.0f%% %7.0f%% %7.0f%% %7.1f %4d\n", cd.id.c_str(), cd.level, right ? "right" : "wrong",
                            100 * q.win_rate(), 100 * q.hp(), 100 * q.party(), q.fights ? q.rounds / q.fights : 0, q.timeouts);
            }
            if (mode == "check") {
                CHECK(res[1].win_rate() >= 0.35 - 0.20 && res[1].win_rate() <= 0.75 + 0.20);
                CHECK(res[1].win_rate() >= res[0].win_rate() + 0.20);  // the wrong tactic is clearly worse
            }
        }
        if (mode == "creatures") {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%.0f s\n%d checks passed, %d failed\n", secs, g_pass, g_fail);
            return g_fail ? 1 : 0;
        }
    }
    for (bool turn : {true, false}) {
        if (mode == "check" && !turn) continue;
        std::unique_ptr<Sim> s = new_sim(turn, 0);
        std::vector<Row> rows = variants(s->l(), only);
        if (mode == "check" && only.empty()) {
            // One common type per level (the drift check; "full" runs every variant).
            // One level per type (PLAN-CREATURES.md): each type at its own level. Level 7 has no
            // duelable original type (the air servant is immune without magic); the new creatures'
            // fights below cover 6 to 8.
            const std::pair<const char*, int> pick[] = {{"torax", 1}, {"skeleton", 2}, {"ghoul", 3}, {"kith", 4},
                                                        {"golem", 5}, {"troll", 6}, {"demon", 8}};
            std::vector<Row> some;
            for (auto& pk : pick)
                for (const Row& r : rows)
                    if (r.name == pk.first && r.level == pk.second) some.push_back(r);
            CHECK_EQ(some.size(), 7);
            rows = some;
        }
        print_header(turn);
        for (const Row& r : rows) {
            Target t = target_for(r.level);
            Result main = run_row(s, r, t.size, n, true, true);
            // Drift: well outside the band (allowing for the noise of n fights) fails.
            if (mode == "check") {
                const double slack = 0.20;
                CHECK(main.win_rate() >= t.win_lo - slack && main.win_rate() <= t.win_hi + slack);
                CHECK(main.timeouts * 4 <= main.fights);
            }
            if (t.size > 1) {
                Result three = run_row(s, r, 3, n, true, true);
                if (mode == "check") CHECK(three.win_rate() >= std::max(0.5, main.win_rate() - 0.15));
            }
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%.0f s\n%d checks passed, %d failed\n", secs, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
