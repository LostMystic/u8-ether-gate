// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Companions: following the Avatar (formation, running to keep up, path finding round
// obstacles, catch-up teleports out of sight, travel through map changes), waiting, the combat AI (melee, casters keeping range, healers, retreating), engine-side
// spells, death and resurrection, lock picking and stealing. See party.h.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/creatures.h"
#include "game/leveling.h"
#include "game/companion_magic.h"
#include "game/anim_tracker.h"
#include "game/assist.h"
#include "game/combat.h"
#include "game/party.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/virtues.h"
#include "game/world.h"

namespace pg {

namespace {

long long dist2(const Item& a, const Item& b) {
    long long dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}
int dist(const Item& a, const Item& b) { return int(std::sqrt(double(dist2(a, b)))); }
int roll(World& w, int lo, int hi) { return hi <= lo ? lo : lo + w.rand_below(hi - lo + 1); }

// Formation offsets behind the Avatar: (back, side) in world units, by slot.
const int kSlots[Party::kMaxMembers][2] = {{72, -56}, {72, 56}, {136, -96}, {136, 0}, {136, 96}, {200, 0}};

}  // namespace

bool Party::busy(uint16_t id) const {
    const Actor* a = w_.actor(id);
    if (!a) return false;
    return a->is(Actor::kAnimLock) || w_.kernel().count_exact(id, kProcActorAnim) ||
           w_.kernel().count_exact(id, kProcPathfind) || w_.kernel().count_exact(id, kProcGravity);
}

bool Party::avatar_in_combat() const {
    const Actor* av = w_.avatar();
    return av && av->is(Actor::kInCombat);
}

void Party::formation_spot(int slot, int& x, int& y, int& z) const {
    const Actor* av = w_.avatar();
    int d = av->dir & 7;
    slot = std::clamp(slot, 0, kMaxMembers - 1);
    int back = kSlots[slot][0], side = kSlots[slot][1];
    // Diagonal direction steps are sqrt(2) long: scale them back to the same distance.
    int bx = -dir_x(d), by = -dir_y(d), sx = dir_x(d + 2), sy = dir_y(d + 2);
    int bs = (bx && by) ? 181 : 256, ss = (sx && sy) ? 181 : 256;
    x = av->x + (bx * back * bs + sx * side * ss) / 256;
    y = av->y + (by * back * bs + sy * side * ss) / 256;
    z = av->z;
}

bool Party::ground_between(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t self) const {
    double len = std::hypot(double(x1 - x0), double(y1 - y0));
    int n = std::max(1, int(len / 24));
    int prev = z0;
    for (int i = 1; i <= n; ++i) {
        int px = x0 + int(std::lround(double(x1 - x0) * i / n)), py = y0 + int(std::lround(double(y1 - y0) * i / n));
        int top;
        uint16_t f, id;
        if (!assist::surface_under(w_, px, py, prev + 24, self, top, f, id) || top < prev - 24) return false;
        if ((f & ShapeInfo::kDamaging) || ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand))) return false;
        if (const Item* it = w_.item(id); it && (it->shape == 409 || it->shape == 483)) return false;  // lava
        prev = top;
    }
    return std::abs(prev - z1) <= 16;
}

bool Party::avatar_settled() const {
    const Actor* av = w_.avatar();
    if (!av || av->chunk < 0 || av->is(Actor::kDead) || w_.kernel().count_exact(1, kProcGravity)) return false;
    int xd, yd, zd;
    w_.footprint(*av, xd, yd, zd);
    int top;
    uint16_t f, id;
    // Standing on something (not in the void), and not on water, lava or a damaging floor.
    return assist::surface_under(w_, av->x - xd / 2, av->y - yd / 2, av->z, av->id, top, f, id) && av->z - top <= 8 &&
           !assist::hazard(w_, *av, av->x, av->y, av->z);
}

bool Party::find_safe_spot(const Actor& a, int nx, int ny, int nz, int min_r, int max_r, bool prefer_hidden,
                           const std::vector<std::array<int, 3>>& taken, int& ox, int& oy, int& oz) const {
    const Actor* av = w_.avatar();
    if (!av) return false;
    int xd, yd, zd, axd, ayd, azd;
    w_.footprint(a, xd, yd, zd);
    w_.footprint(*av, axd, ayd, azd);
    // Others to keep clear of: the Avatar, the rest of the party, and spots already given out.
    std::vector<std::array<int, 3>> others = taken;
    others.push_back({av->x, av->y, av->z});
    for (int m : members)
        if (const Actor* o = w_.actor(state[size_t(m)].id); o && o != &a && o->chunk >= 0 && o->mapnum == a.mapnum)
            others.push_back({o->x, o->y, o->z});
    bool have_fallback = false;
    int fx = 0, fy = 0, fz = 0;
    for (int r = min_r; r <= max_r; r += 32) {
        int n = r == 0 ? 1 : std::max(8, r / 16);
        for (int k = 0; k < n; ++k) {
            double ang = 6.2831853 * k / n;
            int x = nx + int(std::lround(r * std::cos(ang))), y = ny + int(std::lround(r * std::sin(ang)));
            bool clear = true;
            for (const auto& o : others)
                if (std::abs(o[0] - x) < xd + 4 && std::abs(o[1] - y) < yd + 4 && std::abs(o[2] - nz) < zd) clear = false;
            if (!clear) continue;
            for (int dz : {0, 8, -8, 16, -16, 24}) {
                int z = nz + dz;
                if (z < 0) continue;
                if (!assist::safe_landing(w_, a, x, y, z) || assist::hazard(w_, a, x, y, z)) continue;
                World::Placement pl = w_.can_exist(x, y, z, xd, yd, zd, w_.shape_flags(a), a.id);
                if (const Item* land = w_.item(pl.land); !land || land->as_actor()) continue;
                // He could walk there from where he stands (not another stone across the water).
                if (!ground_between(av->x - axd / 2, av->y - ayd / 2, av->z, x - xd / 2, y - yd / 2, z, a.id)) continue;
                if (prefer_hidden && on_screen(x, y, z)) {
                    if (!have_fallback) { have_fallback = true; fx = x; fy = y; fz = z; }
                    break;
                }
                ox = x; oy = y; oz = z;
                return true;
            }
        }
    }
    if (have_fallback) { ox = fx; oy = fy; oz = fz; return true; }
    return false;
}

bool Party::place_near_avatar(int idx, bool hidden_only, bool lenient, bool puff) {
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    const Actor* av = w_.avatar();
    if (!a || !av || av->chunk < 0) return false;
    int slot = int(std::find(members.begin(), members.end(), idx) - members.begin());
    int gx, gy, gz;
    formation_spot(slot, gx, gy, gz);
    int x, y, z;
    const std::vector<std::array<int, 3>> none;
    // Safe ground he could walk to, clear of the others: by the formation slot, else round him.
    bool found = avatar_settled() && (find_safe_spot(*a, gx, gy, gz, 0, 128, hidden_only, none, x, y, z) ||
                                      find_safe_spot(*a, av->x, av->y, av->z, 64, 320, hidden_only, none, x, y, z));
    if (!found && lenient) {
        // Arriving with him (a map change): any dry, supported spot, as long as it is not a hazard.
        found = (find_spot(gx, gy, gz, a->id, x, y, z, 0, 192, hidden_only) && !assist::hazard(w_, *a, x, y, z)) ||
                (find_spot(av->x, av->y, av->z, a->id, x, y, z, 48, 320, hidden_only) && !assist::hazard(w_, *a, x, y, z));
    }
    if (!found) return false;
    w_.kernel().kill(st.id, kProcAny);
    a->set(Actor::kAnimLock, false);
    if (a->chunk >= 0) w_.detach(st.id);
    a->flags &= uint16_t(~kItemEthereal);
    a->mapnum = uint8_t(w_.map().map_number());
    a->dir = av->dir;
    w_.move_to(st.id, x, y, z, false);
    if (const AnimSequence* s = w_.data().anim.get(a->shape, 2)) a->frame = s->at(a->dir, 0).frame;
    if (puff) w_.kernel().add(std::make_unique<SpriteProcess>(w_, 578, 66, 80, 1, 1, x, y, z));
    st.stuck = 0;
    st.far_frames = 0;
    st.edge_at = 0;
    st.placing = false;
    st.last_pos[0] = x;
    st.last_pos[1] = y;
    log.push_back(defs[size_t(idx)].id + " placed");
    return true;
}

bool Party::regroup(int idx) {
    const Actor* av = w_.avatar();
    if (!av || !avatar_settled()) return false;
    uint32_t now = w_.kernel().frame_number();
    // Everyone waiting at an edge comes together (the one asking, and any other stuck there).
    std::vector<int> who;
    for (int m : members) {
        const CompanionState& s = state[size_t(m)];
        const Actor* a = w_.actor(s.id);
        if (!a || a->is(Actor::kDead) || s.mode != CompanionState::kFollow || a->chunk < 0 || a->mapnum != w_.map().map_number()) continue;
        bool edge = (s.edge_at && now - s.edge_at < kEdgeRecent) || std::abs(a->z - av->z) >= 32;
        if (m == idx || (s.stuck >= kEdgeStuck && edge)) who.push_back(m);
    }
    std::vector<std::array<int, 3>> spots;
    for (int m : who) {
        const Actor* a = w_.actor(state[size_t(m)].id);
        int slot = int(std::find(members.begin(), members.end(), m) - members.begin());
        int gx, gy, gz, x, y, z;
        formation_spot(slot, gx, gy, gz);
        if (!find_safe_spot(*a, gx, gy, gz, 0, 128, false, spots, x, y, z) &&
            !find_safe_spot(*a, av->x, av->y, av->z, 64, 256, false, spots, x, y, z))
            return false;  // no room for all of them yet: they keep waiting
        spots.push_back({x, y, z});
    }
    for (size_t k = 0; k < who.size(); ++k) {
        CompanionState& st = state[size_t(who[k])];
        Actor* a = w_.actor(st.id);
        w_.kernel().kill(st.id, kProcAny);
        a->set(Actor::kAnimLock, false);
        w_.move_to(st.id, spots[k][0], spots[k][1], spots[k][2], false);
        a->dir = av->dir;
        if (const AnimSequence* s = w_.data().anim.get(a->shape, 2)) a->frame = s->at(a->dir, 0).frame;
        w_.kernel().add(std::make_unique<SpriteProcess>(w_, 578, 66, 80, 1, 1, spots[k][0], spots[k][1], spots[k][2]));
        st.stuck = 0;
        st.far_frames = 0;
        st.edge_at = 0;
        st.last_pos[0] = spots[k][0];
        st.last_pos[1] = spots[k][1];
        log.push_back(defs[size_t(who[k])].id + " regrouped");
    }
    return true;
}

// Plays one walk (or run) cycle toward a point if a trial of it works and keeps out of harm's
// way (water, lava, damaging floors, drops: assist::hazard); tries the two neighbouring
// directions too. False if all are blocked; a step refused by a hazard or an edge marks
// `edge_at` (they wait there, see ai_follow).
bool Party::walk_toward(int idx, Actor& a, int x, int y, int z, bool run) {
    int d0 = direction_to(x - a.x, y - a.y);
    long long before = int64_t(x - a.x) * (x - a.x) + int64_t(y - a.y) * (y - a.y);
    unsigned action = a.is(Actor::kInCombat) ? 8u : run ? 1u : 0u;
    if (!w_.data().anim.get(a.shape, action)) action = 0;
    // Already standing in harm's way (put there by something else): any step out is fine.
    const bool careful = !assist::hazard(w_, a, a.x, a.y, a.z);
    for (int k : {0, 1, -1}) {
        int d = (d0 + k + 8) & 7;
        AnimTracker tr(w_);
        if (!tr.init(a.id, action, d)) return false;
        bool ok = true, edge = false;
        int steps = 0;
        while (tr.step()) {
            ++steps;
            if (tr.blocked() && !tr.unstoppable()) { ok = false; break; }
            if (tr.unsupported()) { ok = false; edge = true; break; }
            if (careful && (steps & 1) == 0 && assist::hazard(w_, a, tr.x(), tr.y(), tr.z())) { ok = false; edge = true; break; }
        }
        if (ok && ((tr.blocked() && !tr.unstoppable()) || tr.unsupported())) ok = false, edge = tr.unsupported();
        if (ok && careful && assist::hazard(w_, a, tr.x(), tr.y(), tr.z())) ok = false, edge = true;
        if (edge) state[size_t(idx)].edge_at = std::max(1u, w_.kernel().frame_number());
        if (!ok) continue;
        long long after = int64_t(x - tr.x()) * (x - tr.x()) + int64_t(y - tr.y()) * (y - tr.y());
        if (after >= before - 64) continue;
        return w_.do_anim(a.id, action, d) != 0;
    }
    (void)z;
    return false;
}

// ---------------------------------------------------------------------------------------------

void Party::ai(int idx) {
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    if (!a || a->is(Actor::kDead) || st.mode == CompanionState::kDead) return;
    if (st.placing) {
        if (w_.avatar() && w_.avatar()->chunk >= 0) place_near_avatar(idx, false, true);
        return;
    }
    if (a->mapnum != w_.map().map_number() || a->chunk < 0) {
        // Not here: followers catch up (a usecode teleport or a map change we did not see).
        if (st.mode == CompanionState::kFollow) st.placing = true;
        return;
    }
    regen(idx, *a);
    // A swing of a shape without a hit frame lands half way through its animation.
    if (st.pending_hit && w_.kernel().frame_number() >= st.pending_hit_at) {
        uint16_t t = st.pending_hit;
        st.pending_hit = 0;
        const Actor* ta = w_.actor(t);
        if (ta && !ta->is(Actor::kDead) && adjacent(st.id, t, 48)) {
            w_.receive_hit(t, st.id, (direction_to(ta->x - a->x, ta->y - a->y) + 4) & 7, 0, 0);
            if (!(a = w_.actor(st.id))) return;
        }
    }
    if (w_.stasis) {
        // Scenes: hold still and never path through the scripted actors.
        w_.kernel().kill(st.id, kProcPathfind);
        return;
    }
    if (talk_.idx == idx) {
        if (!busy(st.id) && w_.avatar()) {
            int d = direction_to(w_.avatar()->x - a->x, w_.avatar()->y - a->y);
            if (d != a->dir) turn_actor(w_, st.id, d);
        }
        return;
    }
    switch (st.mode) {
    case CompanionState::kIdle: ai_idle(idx, *a); break;
    case CompanionState::kFollow:
        if (!ai_combat(idx, *a)) ai_follow(idx, *a);
        break;
    case CompanionState::kWait:
        if (!ai_combat(idx, *a) && !busy(a->id) && a->is(Actor::kInCombat)) {
            a->set(Actor::kInCombat, false);
            w_.do_anim(a->id, 2, a->dir);
        }
        break;
    default: break;
    }
}

void Party::ai_idle(int idx, Actor& a) {
    CompanionState& st = state[size_t(idx)];
    uint32_t now = w_.kernel().frame_number();
    // Back home after dying, and found: their line.
    if (st.flags.count("came_home") && !talking() && !w_.ask_menu)
        if (const Actor* v = w_.avatar(); v && v->mapnum == a.mapnum && dist(a, *v) < 384) {
            st.flags.erase("came_home");
            bark_trigger(idx, "found_again", true);
        }
    // Attacked while waiting to be found: defend.
    if (st.target && ai_combat(idx, a)) return;
    if (busy(a.id) || now < st.next_act) return;
    st.next_act = now + 180 + unsigned(w_.rand_below(240));
    const Actor* av = w_.avatar();
    int d = a.dir;
    if (av && av->mapnum == a.mapnum && dist(a, *av) < 384) d = direction_to(av->x - a.x, av->y - a.y);
    else if (w_.rand_below(3) == 0) d = w_.rand_below(8);
    if (d != a.dir) turn_actor(w_, a.id, d);
}

void Party::ai_follow(int idx, Actor& a) {
    CompanionState& st = state[size_t(idx)];
    const Actor* av = w_.avatar();
    if (!av || av->chunk < 0) return;
    uint32_t now = w_.kernel().frame_number();
    if (a.is(Actor::kInCombat) && !busy(a.id)) {
        a.set(Actor::kInCombat, false);
        w_.do_anim(a.id, 2, a.dir);
        return;
    }
    int da = dist(a, *av);
    // Far away (left behind, stuck, a usecode teleport): catch up out of sight, onto safe ground
    // beside him (not while he is in the air or on a stepping stone with no room: then they wait).
    if (da > 1200 || std::abs(a.z - av->z) > 160) {
        ++st.far_frames;
        if ((!on_screen(a.x, a.y, a.z) || st.far_frames > 120) && place_near_avatar(idx, true, false, on_screen(a.x, a.y, a.z)))
            return;
    } else {
        st.far_frames = 0;
    }
    int slot = int(std::find(members.begin(), members.end(), idx) - members.begin());
    int gx, gy, gz;
    formation_spot(slot, gx, gy, gz);
    long long dg2 = int64_t(gx - a.x) * (gx - a.x) + int64_t(gy - a.y) * (gy - a.y);
    bool avatar_moving = av->last_anim == 0 || av->last_anim == 1 || av->last_anim == 8;
    // Not on his level (he jumped down a drop or climbed a ledge): never "close enough".
    bool level = std::abs(a.z - av->z) < 32;
    bool resting = level && (dg2 < 48LL * 48 || (da < 120 && !avatar_moving));
    // Held up at water, lava, a drop or a ledge he climbed: wait at the edge (no step into it,
    // see walk_toward), then come to him once he stands on safe ground with room for the party.
    bool at_edge = (st.edge_at && now - st.edge_at < kEdgeRecent) || !level;
    // Stuck: no progress for a while while away from him.
    if ((now & 31) == 0) {
        bool away = da > 256 || (at_edge && (da > 128 || !level));
        // At an edge, pacing along it is no progress either: only getting nearer him counts.
        int was = int(std::hypot(double(st.last_pos[0] - av->x), double(st.last_pos[1] - av->y)));
        bool none = std::abs(a.x - st.last_pos[0]) + std::abs(a.y - st.last_pos[1]) < 16 || (at_edge && was - da < 16);
        if (!resting && none && away) st.stuck += 32;
        else st.stuck = 0;
        st.last_pos[0] = a.x;
        st.last_pos[1] = a.y;
        if (at_edge && st.stuck >= kEdgeStuck && regroup(idx)) return;
        if (st.stuck >= 240 && !on_screen(a.x, a.y, a.z) && place_near_avatar(idx, true)) return;
        if (st.stuck >= 600 && place_near_avatar(idx, true, false, true)) return;
    }
    if (busy(a.id)) {
        // A path walk that is no longer needed (he came back) is dropped.
        if (da < 160 && level && w_.kernel().count_exact(a.id, kProcPathfind)) w_.kernel().kill(a.id, kProcPathfind);
        return;
    }
    if (resting) {
        if (!avatar_moving && now >= st.next_act) {
            st.next_act = now + 90;
            int d = direction_to(av->x - a.x, av->y - a.y);
            if (da > 64 && d != a.dir) turn_actor(w_, a.id, d);
        }
        return;
    }
    bool run = da > 280 || av->last_anim == 1;
    if (walk_toward(idx, a, gx, gy, gz, run)) return;
    if (walk_toward(idx, a, av->x, av->y, av->z, run)) return;
    if (now < st.next_act) return;
    st.next_act = now + 30;
    // Blocked: plan a path to the Avatar (the planner walks round obstacles).
    w_.kernel().add(std::make_unique<PathfindProcess>(w_, a.id, 0, 0, 0, 1));
}

void Party::regen(int idx, Actor& a) {
    CompanionState& st = state[size_t(idx)];
    uint32_t now = w_.kernel().frame_number();
    if (now < st.next_regen) return;
    st.next_regen = now + 150;
    if (a.is(Actor::kInCombat)) return;
    if (int(a.mana) < max_mana(a)) ++a.mana;
    if ((now / 150) % 2 == 0 && int(a.hp) < max_hp(a)) ++a.hp;
}

// ---------------------------------------------------------------------------------------------
// Combat

uint16_t Party::pick_threat(int idx) const {
    const CompanionState& st = state[size_t(idx)];
    const Actor* me = w_.actor(st.id);
    const Actor* av = w_.avatar();
    if (!me || !av) return 0;
    auto valid = [&](uint16_t t) {
        const Actor* o = w_.actor(t);
        if (!o || o->is(Actor::kDead) || same_side(st.id, t) || o->mapnum != me->mapnum) return false;
        if (w_.creatures && w_.creatures->untargetable(t)) return false;  // a new creature under the water
        if (w_.fast_ready() && !(o->flags & kItemFastArea)) return false;
        return dist(*o, *av) < 900;
    };
    if (st.target && valid(st.target)) return st.target;
    if (avatar_target && valid(avatar_target) && avatar_in_combat()) return avatar_target;
    std::vector<uint16_t> near;
    w_.area_search(av->x, av->y, 640, false, near);
    uint16_t best = 0;
    long long bestd = 1LL << 40;
    for (uint16_t k : near) {
        const Actor* o = w_.actor(k);
        if (!o || !valid(k)) continue;
        bool attacking_us = o->is(Actor::kInCombat) && party_member(o->target);
        bool hostile = avatar_in_combat() && is_enemy(*o, *av) && o->is(Actor::kInCombat);
        bool monster = avatar_in_combat() && is_enemy(*o, *av) && monster_info(o->shape) && !(o->ext_flags & kExtPermanentNpc);
        if (!attacking_us && !hostile && !monster) continue;
        long long d = dist2(*o, *me);
        if (d < bestd) { bestd = d; best = k; }
    }
    return best;
}

bool Party::attack_has_hit_frame(unsigned shape) const {
    const AnimSequence* s = w_.data().anim.get(shape, 7);
    if (!s) return false;
    for (const AnimFrame& f : s->frames)
        if (f.attack_range() > 0) return true;
    return false;
}

// Box to box: at most `gap` units apart on the ground plane, overlapping in height.
bool Party::adjacent(uint16_t id, uint16_t target, int gap) const {
    const Actor* a = w_.actor(id);
    const Actor* t = w_.actor(target);
    if (!a || !t || a->mapnum != t->mapnum) return false;
    Box ab = w_.box_of(*a), tb = w_.box_of(*t);
    int gx = std::max({0, (tb.x - tb.xd) - ab.x, (ab.x - ab.xd) - tb.x});
    int gy = std::max({0, (tb.y - tb.yd) - ab.y, (ab.y - ab.yd) - tb.y});
    bool z = ab.z < tb.z + tb.zd + 8 && tb.z < ab.z + ab.zd + 8;
    return gx <= gap && gy <= gap && z;
}

bool Party::in_melee_range(uint16_t id, uint16_t target) const {
    const Actor* a = w_.actor(id);
    const Actor* t = w_.actor(target);
    if (!a || !t) return false;
    if (dist(*a, *t) > 200) return false;
    Box tb = w_.box_of(*t), ab = w_.box_of(*a);
    int dir = direction_to((tb.x - tb.xd / 2) - (ab.x - ab.xd / 2), (tb.y - tb.yd / 2) - (ab.y - ab.yd / 2));
    AnimTracker tr(w_);
    if (!tr.init(id, 7, dir)) return false;
    while (tr.step())
        if (tr.hit()) return tr.hit() == target;
    return false;
}

bool Party::ai_combat(int idx, Actor& a) {
    CompanionState& st = state[size_t(idx)];
    const CompanionDef& d = defs[size_t(idx)];
    uint32_t now = w_.kernel().frame_number();
    bool engaged = avatar_in_combat() || st.target;
    if (st.target) {
        const Actor* t = w_.actor(st.target);
        if (!t || t->is(Actor::kDead) || t->mapnum != a.mapnum) {
            if (t && t->is(Actor::kDead) && in_party(idx) && w_.rand_below(3) == 0) bark_trigger(idx, "kill");
            st.target = 0;
        }
    }
    if (!engaged) return false;
    uint16_t tid = pick_threat(idx);
    // Healers look after the party first, in and out of a fight.
    if (d.healer && !busy(a.id) && now >= st.next_cast) {
        uint16_t worst = 0;
        int worst_pct = 50;
        auto consider = [&](uint16_t id) {
            const Actor* m = w_.actor(id);
            if (!m || m->is(Actor::kDead) || m->mapnum != a.mapnum || dist(*m, a) > 384 * kSpellRangeScale) return;
            int pct = int(m->hp) * 100 / std::max(1, id == 1 ? 2 * int(m->str) : max_hp(*m));
            if (pct < worst_pct) { worst_pct = pct; worst = id; }
        };
        consider(1);
        for (int m : members) consider(state[size_t(m)].id);
        if (worst)
            for (const std::string& sid : d.spells) {
                int si = spell_index(sid);
                if (si >= 0 && spells[size_t(si)].kind == "heal" && a.mana >= spells[size_t(si)].mana && spell_ready(idx, si, nullptr, worst)) {
                    if (cast(idx, si, worst)) return true;
                }
            }
    }
    if (!tid) {
        if (!avatar_in_combat() && a.is(Actor::kInCombat) && !busy(a.id)) {
            a.set(Actor::kInCombat, false);
            w_.do_anim(a.id, 2, a.dir);
        }
        return false;
    }
    if (!a.is(Actor::kInCombat)) {
        a.set(Actor::kInCombat, true);
        if (in_party(idx)) bark_trigger(idx, "combat");
    }
    st.target = tid;
    if (busy(a.id) || now < st.next_act) return true;
    const Actor* t = w_.actor(tid);
    int dt = dist(a, *t);
    int dir = direction_to(t->x - a.x, t->y - a.y);
    // Badly hurt: back off toward the Avatar, away from the enemy.
    if (int(a.hp) * 4 < max_hp(a)) {
        const Actor* av = w_.avatar();
        if (dt < 320 && av) {
            int rx = a.x + (a.x - t->x) + (av->x - a.x) / 2, ry = a.y + (a.y - t->y) + (av->y - a.y) / 2;
            if (walk_toward(idx, a, rx, ry, a.z, true)) return true;
        }
    }
    // Casters keep their distance and cast.
    if (d.caster && now >= st.next_cast) {
        std::vector<int> usable;
        for (const std::string& sid : d.spells) {
            int si = spell_index(sid);
            if (si < 0) continue;
            const SpellDef& s = spells[size_t(si)];
            if (!s.combat || a.mana < s.mana || s.kind == "heal" || s.kind == "raise" || s.kind == "cure") continue;
            if (!spell_ready(idx, si, nullptr, tid)) continue;  // Leveling: locked, no uses, or not worth the last ones
            if (s.kind == "summon") {
                bool have = false;
                for (const Summon& sm : summons) have |= w_.actor(sm.id) != nullptr;
                if (have) continue;
            }
            usable.push_back(si);
        }
        if (!usable.empty()) {
            if (dt < 128 && !d.rogue && w_.rand_below(2) == 0) {
                int rx = a.x + (a.x - t->x) * 2, ry = a.y + (a.y - t->y) * 2;
                if (walk_toward(idx, a, rx, ry, a.z, false)) return true;
            }
            int si = usable[size_t(w_.rand_below(int(usable.size())))];
            if (dt <= spells[size_t(si)].range && w_.can_reach(a.id, tid, spells[size_t(si)].range + 64)) {
                if (cast(idx, si, tid)) return true;
            } else if (walk_toward(idx, a, t->x, t->y, t->z, true)) {
                return true;
            }
        }
    }
    // Melee. Shapes whose attack animation never reaches (no hit frame in ANIM.DAT, e.g. 403)
    // swing when adjacent and the blow is resolved here, half way through the swing.
    bool reaches = attack_has_hit_frame(a.shape);
    if (reaches ? in_melee_range(a.id, tid) : adjacent(a.id, tid, 32)) {
        w_.do_anim(a.id, 7, dir);
        if (!reaches) {
            unsigned len = 4;
            if (const AnimSequence* s = w_.data().anim.get(a.shape, 7)) len = std::max(2u, s->length * (s->repeat + 1));
            st.pending_hit = tid;
            st.pending_hit_at = now + len / 2 + 1;
        }
        st.next_act = now + unsigned(std::max(4, 2 * (25 - int(a.dex))));
        if (w_.leveling) st.next_act += unsigned(w_.leveling->attack_delay_extra(a));  // a heavy weapon below its STR
        return true;
    }
    // Casters waiting for their next spell keep back; with Leveling, only while they have an attack
    // spell they can still cast (out of reagents, they close in and fight).
    bool keeps_back = d.caster && a.mana >= 2 && dt < 448;
    if (keeps_back && w_.leveling && w_.leveling->active()) {
        keeps_back = false;
        for (const std::string& sid : d.spells) {
            int si = spell_index(sid);
            if (si < 0) continue;
            const SpellDef& s = spells[size_t(si)];
            if (s.combat && s.kind != "heal" && s.kind != "raise" && s.kind != "cure" && spell_ready(idx, si, nullptr, tid))
                keeps_back = true;
        }
    }
    if (keeps_back) {
        st.next_act = now + 10;
        return true;
    }
    if (walk_toward(idx, a, t->x, t->y, t->z, dt > 200)) return true;
    st.next_act = now + 20;
    w_.kernel().add(std::make_unique<PathfindProcess>(w_, a.id, 0, 0, 0, tid, reaches));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Spells

bool Party::spell_known(int idx, const std::string& id) const {
    const auto& s = defs[size_t(idx)].spells;
    return std::find(s.begin(), s.end(), id) != s.end();
}

bool Party::spell_ready(int idx, int si, std::string* why, uint16_t auto_target) const {
    if (si < 0 || size_t(si) >= spells.size() || idx < 0 || size_t(idx) >= defs.size()) return false;
    if (!w_.leveling || !w_.leveling->active() || !w_.leveling->magic->loaded) return true;
    const CompanionMagic& m = *w_.leveling->magic;
    CompanionMagic::Gate g = m.gate(idx, spells[size_t(si)].id);
    if (!g.ok()) {
        if (why) *why = g.why;
        return false;
    }
    if (auto_target && !m.worth_casting(idx, spells[size_t(si)].id, auto_target)) {
        if (why) *why = "saving the last uses";
        return false;
    }
    return true;
}

bool Party::cast(int idx, int si, uint16_t target) {
    if (si < 0 || size_t(si) >= spells.size()) return false;
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    const Item* t = w_.item(target);
    if (!a || !t || a->is(Actor::kDead)) return false;
    const SpellDef& s = spells[size_t(si)];
    if (a->mana < s.mana) return false;
    if (!spell_ready(idx, si)) return false;  // Leveling stage 5: locked or no uses left
    a->mana = int16_t(a->mana - s.mana);
    if (w_.leveling && w_.leveling->active() && w_.leveling->magic->loaded) w_.leveling->magic->on_cast(idx, s.id);
    uint32_t now = w_.kernel().frame_number();
    st.next_cast = now + 60 + unsigned(w_.rand_below(60));
    st.next_act = now + 20;
    int tx, ty, tz;
    w_.location(*t, tx, ty, tz);
    int dir = target == a->id ? a->dir : direction_to(tx - a->x, ty - a->y);
    unsigned anim = 7;
    for (unsigned k : {27u, 28u, 29u, 30u, 31u})
        if (w_.data().anim.get(a->shape, k)) { anim = k; break; }
    w_.kernel().kill(a->id, kProcPathfind);
    w_.do_anim(a->id, anim, dir);
    if (s.cast_sfx >= 0) w_.sound.play_sfx(s.cast_sfx, SoundSystem::kDefaultPriority, a->id);
    // Stats grow by use, but not with Leveling on (PLAN-LEVELING.md 4: training by use is removed).
    if (!(w_.leveling && w_.leveling->enabled) && w_.rand_below(30) == 0 && a->intel < 30) ++a->intel;
    log.push_back(defs[size_t(idx)].id + " casts " + s.id);
    if (s.kind == "lightning" && target != a->id) {
        // Lightning: a bolt from the sky for a few frames, then the hit and the flash (step_bolts).
        PartyBolt pb;
        pb.caster = a->id;
        pb.target = target;
        pb.spell = si;
        pb.from[0] = tx; pb.from[1] = ty; pb.from[2] = tz + 24;
        pb.total = kLightningFrames;
        bolts.push_back(pb);
        w_.sky_bolts.push_back({target, tx, ty, tz, uint32_t(w_.rand_below(1 << 20)), kLightningFrames});
        return true;
    }
    if (s.proj_shape && target != a->id) {
        uint16_t b = w_.create_item(s.proj_shape, s.proj_frame);
        if (Item* bi = w_.item(b)) {
            bi->flags |= kItemFastOnly | kItemDisposable;
            bi->ext_flags |= kExtSprite;
            PartyBolt pb;
            pb.item = b;
            pb.caster = a->id;
            pb.target = target;
            pb.spell = si;
            pb.from[0] = a->x; pb.from[1] = a->y; pb.from[2] = a->z + 32;
            int dd = int(std::sqrt(double(int64_t(tx - a->x) * (tx - a->x) + int64_t(ty - a->y) * (ty - a->y))));
            pb.total = std::max(4, dd / 28);
            w_.move_to(b, pb.from[0], pb.from[1], pb.from[2], false);
            bolts.push_back(pb);
            return true;
        }
    }
    land_spell(si, a->id, target, tx, ty, tz);
    return true;
}

void Party::step_bolts() {
    for (size_t i = 0; i < w_.sky_bolts.size();)
        if (--w_.sky_bolts[i].frames <= 0) w_.sky_bolts.erase(w_.sky_bolts.begin() + int64_t(i));
        else ++i;
    for (size_t i = 0; i < bolts.size();) {
        PartyBolt& b = bolts[i];
        ++b.frames;
        int tx = b.from[0], ty = b.from[1], tz = b.from[2];
        if (const Item* t = w_.item(b.target)) {
            w_.location(*t, tx, ty, tz);
            tz += 24;
        }
        bool done = b.frames >= b.total || (b.item && !w_.item(b.item));
        if (!done) {
            int k = b.frames, n = b.total;
            if (b.item) w_.move_to(b.item, b.from[0] + (tx - b.from[0]) * k / n, b.from[1] + (ty - b.from[1]) * k / n,
                       b.from[2] + (tz - b.from[2]) * k / n, false);
            ++i;
            continue;
        }
        PartyBolt copy = b;
        bolts.erase(bolts.begin() + int64_t(i));
        if (copy.item && w_.item(copy.item)) w_.destroy_now(copy.item);
        land_spell(copy.spell, copy.caster, copy.target, tx, ty, tz - 24);
    }
}

void Party::heal_actor(uint16_t caster, uint16_t target, int amount, const SpellDef& s) {
    Actor* t = w_.actor(target);
    if (!t || t->is(Actor::kDead)) return;
    int mx = target == 1 ? 2 * int(t->str) : max_hp(*t);
    int before = t->hp;
    t->hp = uint16_t(std::min(mx, int(t->hp) + amount));
    w_.game_log.on_heal(w_, caster, target, int(t->hp) - before, s.name);  // the HUD's combat log
}

void Party::land_spell(int si, uint16_t caster, uint16_t target, int x, int y, int z) {
    if (si < 0 || size_t(si) >= spells.size()) return;
    const SpellDef& s = spells[size_t(si)];
    // The HUD's combat log names the spell in the hits it causes ("Kalvax's Fire Bolt hits ...").
    struct SpellContext {
        GameLog& g;
        ~SpellContext() { g.spell_context.clear(); }
    } spell_context{w_.game_log};
    w_.game_log.spell_context = s.name;
    if (s.fx_shape) w_.kernel().add(std::make_unique<SpriteProcess>(w_, s.fx_shape, int(s.fx_first), int(s.fx_last), 1, 1, x, y, z));
    if (s.sfx >= 0) w_.sound.play_sfx(s.sfx, SoundSystem::kDefaultPriority, target);
    if (s.flash) w_.sky_flash = 1;  // shown for one frame, over the game picture only (GameMode)
    Actor* c = w_.actor(caster);
    Actor* t = w_.actor(target);
    int dir = c ? direction_to(c->x - x, c->y - y) : 0;  // the hit comes from the caster
    const bool lv = w_.leveling && w_.leveling->active();
    auto hurt = [&](uint16_t id, int dmg) {
        if (id == caster || same_side(caster, id)) return;  // no friendly fire (an area spell spares its caster too)
        const Actor* v = w_.actor(id);
        if (!v || v->is(Actor::kDead)) return;
        if (lv && c) dmg = Leveling::int_scaled(dmg, c->intel);  // Leveling: spell damage by the caster's INT
        w_.receive_hit(id, caster, dir, std::max(1, dmg), s.dmg_type);
    };
    if (s.kind == "heal") {
        heal_actor(caster, target, roll(w_, s.dmg0, s.dmg1), s);
    } else if (s.kind == "cure") {
        if (t) { t->set(Actor::kPoisoned, false); heal_actor(caster, target, roll(w_, s.dmg0, s.dmg1), s); }
    } else if (s.kind == "raise") {
        int i = index_of(target);
        if (i >= 0) revive(i, 50);
    } else if (s.kind == "field") {
        std::vector<uint16_t> near;
        w_.area_search(x, y, std::max(64, s.radius), false, near);
        for (uint16_t k : near)
            if (const Actor* o = w_.actor(k); o && std::abs(o->z - z) < 64) hurt(k, roll(w_, s.dmg0, s.dmg1));
        if (s.fx_shape)
            for (int k = 0; k < std::max(4, s.radius / 24); ++k)  // as thick over the wider area
                w_.kernel().add(std::make_unique<SpriteProcess>(w_, s.fx_shape, int(s.fx_first), int(s.fx_last), 1, 1,
                                                                 x + roll(w_, -s.radius / 2, s.radius / 2),
                                                                 y + roll(w_, -s.radius / 2, s.radius / 2), z));
    } else if (s.kind == "summon") {
        uint16_t sk = w_.create_monster(119, 0);
        Actor* sa = w_.actor(sk);
        const Actor* av = w_.avatar();
        int px, py, pz;
        if (sa && c && av && find_spot(c->x, c->y, c->z, sk, px, py, pz, 48, 160)) {
            sa->alignment = av->alignment;
            sa->enemy_alignment = av->enemy_alignment;
            sa->flags |= kItemDisposable;
            w_.move_to(sk, px, py, pz, false);
            // Leveling: the skeleton fights at its summoner's level, and lasts longer with INT.
            int secs = std::max(10, s.dmg1);
            if (lv) {
                w_.leveling->apply_monster(*sa, w_.leveling->level_of(caster));
                secs = Leveling::int_scaled(secs, c->intel);
            }
            summons.push_back({sk, w_.kernel().frame_number() + 30u * unsigned(secs)});
            w_.do_anim(sk, 4, c->dir);
            if (t && !same_side(caster, target)) {
                w_.set_in_combat(sk);
                if ((sa = w_.actor(sk))) { sa->target = target; sa->fixed_target = target; }
            }
        } else if (sk) {
            w_.destroy_now(sk);
        }
    } else if (t) {
        int dmg = roll(w_, s.dmg0, s.dmg1);
        if (s.kind != "calm") hurt(target, dmg);
        if (!(t = w_.actor(target))) return;
        if (s.kind == "drain" && c) heal_actor(caster, caster, std::max(1, dmg / 2), s);
        if (s.kind == "knock" && !t->is(Actor::kDead) && !same_side(caster, target)) {
            int d = c ? direction_to(t->x - c->x, t->y - c->y) : 0;
            w_.hurl(target, 6 * dir_x(d), 6 * dir_y(d), 8, 4);
        }
        if (s.kind == "slow" && !t->is(Actor::kDead) && !same_side(caster, target)) {
            w_.kernel().kill(target, kProcActorAnim);
            t->set(Actor::kAnimLock, false);
            w_.do_anim(target, 13, t->dir);
        }
        if (s.kind == "calm" && !t->is(Actor::kDead) && !same_side(caster, target)) {
            const MonsterInfo* m = monster_info(t->shape);
            bool undead = m && (m->def_type & kDmgUndead);
            int chance = c ? 30 + 2 * int(c->intel) : 40;
            if (!undead && w_.rand_below(100) < chance) {
                w_.clear_in_combat(target);
                if ((t = w_.actor(target))) { t->target = 0; t->fixed_target = 0; }
                for (CompanionState& cs : state)
                    if (cs.target == target) cs.target = 0;
                if (avatar_target == target) avatar_target = 0;
                log.push_back("calmed " + std::to_string(target));
            }
        }
    }
}

void Party::tick_summons() {
    uint32_t now = w_.kernel().frame_number();
    for (size_t i = 0; i < summons.size();) {
        Actor* s = w_.actor(summons[i].id);
        if (!s || now >= summons[i].until || (s->is(Actor::kDead) && now + 60 >= summons[i].until)) {
            if (s) {
                w_.kernel().add(std::make_unique<SpriteProcess>(w_, 578, 66, 80, 1, 1, s->x, s->y, s->z));
                w_.destroy(summons[i].id);
            }
            summons.erase(summons.begin() + int64_t(i));
            continue;
        }
        if (s->is(Actor::kDead)) summons[i].until = std::min(summons[i].until, now + 90);
        // A summoned skeleton with nothing to fight follows its master's target or rests.
        if (!s->is(Actor::kDead) && !s->is(Actor::kInCombat))
            for (int m : members)
                if (state[size_t(m)].target) {
                    w_.set_in_combat(summons[i].id);
                    if ((s = w_.actor(summons[i].id))) { s->target = state[size_t(m)].target; s->fixed_target = s->target; }
                    break;
                }
        ++i;
    }
}

// ---------------------------------------------------------------------------------------------
// Death and resurrection

void Party::check_deaths() {
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        if (st.mode == CompanionState::kUnspawned || st.mode == CompanionState::kDead || st.hidden) continue;
        const Actor* a = w_.actor(st.id);
        if (!a || !a->is(Actor::kDead)) continue;
        st.before_death = st.mode;
        st.mode = CompanionState::kDead;
        st.target = 0;
        bool was_member = in_party(int(i));
        members.erase(std::remove(members.begin(), members.end(), int(i)), members.end());
        if (talk_.idx == int(i)) end_talk();
        w_.close_gump(st.id);
        log.push_back(defs[i].id + " died");
        PG_LOG("party: %s has fallen", defs[i].name.c_str());
        if (was_member)
            for (int m : members)
                if (w_.rand_below(2) == 0 || m == members.front()) { bark_trigger(m, "ally_down", true); break; }
    }
    if (innocent_) {
        const Actor* v = w_.actor(innocent_);
        if (!v) innocent_ = 0;
        else if (v->is(Actor::kDead)) {
            innocent_ = 0;
            if (!members.empty()) bark_trigger(members[size_t(w_.rand_below(int(members.size())))], "innocent", true);
        }
    }
    if (!members.empty() || avatar_target) {
        if (const Actor* t = w_.actor(avatar_target); !t || t->is(Actor::kDead)) avatar_target = 0;
    }
}

bool Party::revive(int idx, int hp_percent) {
    if (idx < 0 || size_t(idx) >= state.size()) return false;
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    if (!a || !a->is(Actor::kDead)) return false;
    w_.kernel().kill(st.id, kProcAny);
    w_.close_gump(st.id);
    a->flags &= uint16_t(~(kItemGumpOpen | 0x8000));
    a->set(Actor::kDead, false);
    a->set(Actor::kAnimLock, false);
    a->set(Actor::kInCombat, false);
    a->hp = uint16_t(std::max(1, max_hp(*a) * hp_percent / 100));
    w_.do_anim(st.id, 4, -1);
    st.mode = CompanionState::kIdle;
    if (!party_full() && st.before_death != CompanionState::kIdle && st.before_death != CompanionState::kReturn) {
        members.push_back(idx);
        st.mode = CompanionState::kFollow;
    }
    st.wait_pos[0] = a->x; st.wait_pos[1] = a->y; st.wait_pos[2] = a->z;
    st.wait_map = a->mapnum;
    // Not in the party: where they fell; one who never joined goes back to their resting place.
    if (st.mode == CompanionState::kIdle && !st.joined_before) homes_dirty_ = true;
    log.push_back(defs[size_t(idx)].id + " revived");
    bark_trigger(idx, "revived", true);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The rogue's trades

bool Party::pick_lock(int idx, uint16_t door, std::string& msg) {
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    Item* it = w_.item(door);
    if (!a || !it) { msg = "nothing"; return false; }
    if (!w_.is_door_shape(it->shape)) { msg = "not_a_lock"; return false; }
    if (!(it->quality & 0x400)) { msg = "not_locked"; return false; }
    const Actor* av = w_.avatar();
    int dx = it->x - a->x, dy = it->y - a->y;
    int dxa = av ? it->x - av->x : 9999, dya = av ? it->y - av->y : 9999;
    if (dx * dx + dy * dy > 320 * 320 && dxa * dxa + dya * dya > 256 * 256) { msg = "too_far"; return false; }
    // No lockpick item ships with the game: the rogue's own skill (DEX) decides. Doors with
    // quality bit 0x200 as well are harder (their meaning is not known; treated as stout locks).
    int chance = std::clamp(3 * int(a->dex) + 10 - ((it->quality & 0x200) ? 25 : 0), 5, 92);
    w_.do_anim(a->id, 7, direction_to(it->x - a->x, it->y - a->y));
    if (w_.rand_below(100) < chance) {
        it->quality = uint16_t(it->quality & ~0x400);
        w_.sound.play_sfx(0x2E, SoundSystem::kDefaultPriority, door);
        if (w_.rand_below(8) == 0 && a->dex < 30) ++a->dex;
        msg = "picked";
        log.push_back("picked " + std::to_string(door));
        return true;
    }
    msg = "failed";
    return false;
}

bool Party::steal_from(int idx, uint16_t victim, std::string& msg) {
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    Actor* v = w_.actor(victim);
    if (!a || !v || victim == 1 || same_side(st.id, victim)) { msg = "no_victim"; return false; }
    if (v->is(Actor::kDead)) { msg = "dead"; return false; }
    if (dist(*a, *v) > 256) { msg = "too_far"; return false; }
    std::vector<uint16_t> loot;
    for (uint16_t k : v->contents)
        if (const Item* c = w_.item(k); c && !(c->flags & kItemEquipped)) loot.push_back(k);
    int chance = std::clamp(3 * int(a->dex) + 5, 5, 85);
    bool ok = w_.rand_below(100) < chance;
    uint16_t taken = 0;
    if (ok) {
        uint16_t pack = w_.equipped(st.id, 7);
        if (!loot.empty()) taken = loot[size_t(w_.rand_below(int(loot.size())))];
        else {
            taken = w_.create_item(143, 0);  // a few coins from the purse
            if (Item* c = w_.item(taken)) c->quality = uint16_t(3 + w_.rand_below(12));
            if (taken && !w_.move_to_container(taken, victim)) { w_.destroy_now(taken); taken = 0; }
        }
        if (taken && pack && w_.move_to_container(taken, pack)) {
            if (Item* c = w_.item(taken)) c->flags &= uint16_t(~kItemOwned);
            if (taken) w_.call_event(taken, kEvCombine);
        } else {
            ok = false;
        }
    }
    // Seen? Anyone (the victim too) within 512 with a line of sight to the thief.
    bool seen = !ok;
    std::vector<uint16_t> near;
    w_.area_search(a->x, a->y, 512, false, near);
    for (uint16_t k : near) {
        const Actor* o = w_.actor(k);
        if (!o || k == st.id || same_side(st.id, k) || o->is(Actor::kDead)) continue;
        int p = k == victim ? 25 : 60;
        if (w_.can_reach(k, st.id, 512) && w_.rand_below(100) < p) seen = true;
    }
    if (seen) {
        // The game's own reaction: the "stole" event at everyone around, as for the Avatar.
        uint16_t evitem = taken ? taken : victim;
        for (uint16_t k : near) {
            const Actor* o = w_.actor(k);
            if (!o || k == st.id || same_side(st.id, k) || o->is(Actor::kDead)) continue;
            w_.call_event_w(k, kEvStole, evitem);
        }
        log.push_back("theft seen");
    }
    if (ok && taken && w_.virtues) w_.virtues->on_theft(taken);  // infamy (game/virtues.h)
    if (ok && w_.rand_below(8) == 0 && a->dex < 30) ++a->dex;
    msg = ok ? (seen ? "stolen_seen" : "stolen") : "caught";
    log.push_back(std::string("steal ") + msg);
    return ok;
}

}  // namespace pg
