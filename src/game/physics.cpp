// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Shared physics (specs/world/physics.md): the static placement test, the swept-box test,
// moving with collision and its events, reach with line of sight, missile launching, and the
// stacking rules (grab, ascend).
#include <algorithm>
#include <cstdlib>

#include "core/bugfix.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/processes.h"
#include "game/world.h"

namespace pg {

namespace {

constexpr int kT = 0x4000;  // sweep time for "the whole move"

bool overlap3(const Box& a, const Box& b) {
    return a.x - a.xd < b.x && a.x > b.x - b.xd && a.y - a.yd < b.y && a.y > b.y - b.yd && a.z < b.z + b.zd &&
           a.z + a.zd > b.z;
}
bool overlap_xy(const Box& a, const Box& b) {
    return a.x - a.xd < b.x && a.x > b.x - b.xd && a.y - a.yd < b.y && a.y > b.y - b.yd;
}

// Chunk range covering a box (plus one chunk of margin).
void chunk_range(int x0, int y0, int x1, int y1, int& c0, int& c1, int& r0, int& r1) {
    c0 = std::clamp(std::min(x0, x1) / 512 - 1, 0, CurrentMap::kChunks - 1);
    c1 = std::clamp(std::max(x0, x1) / 512 + 1, 0, CurrentMap::kChunks - 1);
    r0 = std::clamp(std::min(y0, y1) / 512 - 1, 0, CurrentMap::kChunks - 1);
    r1 = std::clamp(std::max(y0, y1) / 512 + 1, 0, CurrentMap::kChunks - 1);
}

// One axis of the sweep (physics.md 3 step 2). Mover interval [m0, m1] at the start, the
// obstacle [o0, o1], velocity v over the whole move. False: no collision on this axis.
bool sweep_axis(int m0, int m1, int o0, int o1, int v, bool z_axis, int& entry, int& exit, bool& touch, bool& floor) {
    touch = floor = false;
    if (v == 0) {
        if (m1 < o0 || m0 > o1) return false;
        entry = -1;
        exit = kT;
        if (m1 == o0 || m0 == o1) touch = true;
        if (z_axis && m0 == o1) floor = true;
        return true;
    }
    if (v < 0) {
        entry = int((int64_t(o1) - m0) * kT / v);
        exit = int((int64_t(o0) - m1) * kT / v);
        if (m1 == o0) touch = true;
        if (m0 + v == o1) {
            touch = true;
            if (z_axis) floor = true;
        }
    } else {
        entry = int((int64_t(o0) - m1) * kT / v);
        exit = int((int64_t(o1) - m0) * kT / v);
        if (m0 == o1 || m1 + v == o0) touch = true;
    }
    if (exit < 0 || entry > kT) return false;
    return true;
}

}  // namespace

Box World::box_of(const Item& it) const {
    Box b;
    location(it, b.x, b.y, b.z);
    footprint(*root_container(it), b.xd, b.yd, b.zd);
    return b;
}

uint16_t World::shape_flags(const Item& it) const { return data_.typeflags.get(it.shape).flags(); }

// ---------------------------------------------------------------------------------------------
// Static placement (physics.md 2)

World::Placement World::can_exist(int x, int y, int z, int xd, int yd, int zd, uint16_t mflags, uint16_t ignore,
                                  const Box* start) const {
    Placement r;
    int land_top = -1000, support = -1000;
    int c0, c1, r0, r1;
    chunk_range(x - xd, y - yd, x, y, c0, c1, r0, r1);
    const Box t{x, y, z, xd, yd, zd};
    int bcx = x - xd / 2, bcy = y - yd / 2;
    for (int cy = r0; cy <= r1; ++cy)
        for (int cx = c0; cx <= c1; ++cx)
            for (uint16_t id : map_.chunk(cx, cy)) {
                if (id == ignore) continue;
                const Item* o = item(id);
                if (!o || (o->ext_flags & kExtSprite)) continue;
                if (passable_door(*o)) continue;  // milestone 6: open doors have no collision
                if (id < 256 && party_ghost(ignore, id)) continue;  // companions (game/party.h)
                const ShapeInfo& si = data_.typeflags.get(o->shape);
                uint16_t of = si.flags();
                if (!(of & (ShapeInfo::kSolid | ShapeInfo::kDamaging | ShapeInfo::kLand | ShapeInfo::kRoof))) continue;
                Box ob{o->x, o->y, o->z, 0, 0, 0};
                si.footprint(o->flags & kItemFlipped, ob.xd, ob.yd, ob.zd);
                if ((of & mflags & (ShapeInfo::kSolid | ShapeInfo::kDamaging)) && !r.blocker && overlap3(t, ob) &&
                    !(start && overlap3(*start, ob)) && !(debug_noclip && ignore == 1 && o->z + ob.zd > z)) {
                    r.valid = false;
                    r.blocker = id;
                }
                int top = o->z + ob.zd;
                if (top > z) continue;
                if ((of & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof)) && overlap_xy(t, ob))
                    support = std::max(support, top);
                if ((of & (ShapeInfo::kLand | ShapeInfo::kRoof)) && bcx > o->x - ob.xd && bcx <= o->x &&
                    bcy > o->y - ob.yd && bcy <= o->y && top > land_top) {
                    land_top = top;
                    r.land = id;
                }
            }
    r.support_z = support;
    r.supported = (support == z && land_top + 8 >= z) || z <= 0;
    return r;
}

World::Placement World::can_exist_shape(unsigned shape, int x, int y, int z, uint16_t ignore) const {
    const ShapeInfo& si = data_.typeflags.get(shape);
    int xd, yd, zd;
    si.footprint(false, xd, yd, zd);
    return can_exist(x, y, z, xd, yd, zd, si.flags(), ignore);
}

// ---------------------------------------------------------------------------------------------
// The swept-box test (physics.md 3)

void World::sweep(const Box& a, int ex, int ey, int ez, uint16_t mflags, uint16_t self, bool blocking_only,
                  std::vector<SweepHit>& out) const {
    out.clear();
    const int v[3] = {ex - a.x, ey - a.y, ez - a.z};
    int c0, c1, r0, r1;
    chunk_range(std::min(a.x, ex) - a.xd, std::min(a.y, ey) - a.yd, std::max(a.x, ex), std::max(a.y, ey), c0, c1, r0, r1);
    const uint16_t block_mask = mflags & (ShapeInfo::kSolid | ShapeInfo::kDamaging | ShapeInfo::kLand);
    for (int cy = r0; cy <= r1; ++cy)
        for (int cx = c0; cx <= c1; ++cx)
            for (uint16_t id : map_.chunk(cx, cy)) {
                if (id == self) continue;
                const Item* o = item(id);
                if (!o || (o->ext_flags & kExtSprite)) continue;
                if (passable_door(*o)) continue;  // milestone 6: open doors have no collision
                if (id < 256 && party_ghost(self, id)) continue;  // companions (game/party.h)
                const ShapeInfo& si = data_.typeflags.get(o->shape);
                // Debug noclip: for the Avatar only what is below his feet counts (floors).
                if (debug_noclip && self == 1) {
                    int fxd, fyd, fzd;
                    si.footprint(o->flags & kItemFlipped, fxd, fyd, fzd);
                    if (o->z + fzd > std::min(a.z, ez)) continue;
                }
                bool blocking = (si.flags() & block_mask) != 0;
                if (blocking_only && !blocking) continue;
                Box ob{o->x, o->y, o->z, 0, 0, 0};
                si.footprint(o->flags & kItemFlipped, ob.xd, ob.yd, ob.zd);
                // 1. Ignore things overlapping the start by more than one unit on every axis.
                int ovx = std::min(a.x, ob.x) - std::max(a.x - a.xd, ob.x - ob.xd);
                int ovy = std::min(a.y, ob.y) - std::max(a.y - a.yd, ob.y - ob.yd);
                int ovz = std::min(a.z + a.zd, ob.z + ob.zd) - std::max(a.z, ob.z);
                if (ovx > 1 && ovy > 1 && ovz > 1) continue;
                // 2. Per axis.
                const int m0[3] = {a.x - a.xd, a.y - a.yd, a.z}, m1[3] = {a.x, a.y, a.z + a.zd};
                const int o0[3] = {ob.x - ob.xd, ob.y - ob.yd, ob.z}, o1[3] = {ob.x, ob.y, ob.z + ob.zd};
                int entry[3], exit[3];
                bool touch = false, floor = false, hit = true;
                for (int i = 0; i < 3 && hit; ++i) {
                    bool t, f;
                    hit = sweep_axis(m0[i], m1[i], o0[i], o1[i], v[i], i == 2, entry[i], exit[i], t, f);
                    touch |= t;
                    if (i == 2) floor = f;
                }
                if (!hit) continue;
                // 3-4. Hit time = latest entry, end = earliest exit.
                int ht = std::max({entry[0], entry[1], entry[2]}), et = std::min({exit[0], exit[1], exit[2]});
                if (ht > et) continue;
                SweepHit h;
                h.item = id;
                h.hit_time = std::max(ht, -1);
                h.end_time = std::min(et, kT);
                h.touching = touch;
                h.floor = floor;
                h.blocking = blocking;
                for (int i = 0; i < 3; ++i)
                    if (entry[i] == ht) h.dirs |= 1u << i;
                out.push_back(h);
            }
    // 5. Sorted by hit time, then end time.
    std::stable_sort(out.begin(), out.end(), [](const SweepHit& p, const SweepHit& q) {
        return p.hit_time != q.hit_time ? p.hit_time < q.hit_time : p.end_time < q.end_time;
    });
}

void World::contact_point(const Box& a, int ex, int ey, int ez, const SweepHit& h, int& x, int& y, int& z) const {
    int t = std::clamp(h.hit_time, 0, kT);
    x = a.x + int(int64_t(ex - a.x) * t / kT);
    y = a.y + int(int64_t(ey - a.y) * t / kT);
    z = a.z + int(int64_t(ez - a.z) * t / kT);
    // Integer times round the stop point: put the blocked faces exactly against the obstacle.
    const Item* o = item(h.item);
    if (!o || h.hit_time < 0) return;
    Box ob = box_of(*o);
    if (h.dirs & 1) x = ex > a.x ? ob.x - ob.xd : ob.x + a.xd;
    if (h.dirs & 2) y = ey > a.y ? ob.y - ob.yd : ob.y + a.yd;
    if (h.dirs & 4) z = ez > a.z ? ob.z - a.zd : ob.z + ob.zd;
    // Never past the start in the direction of travel.
    if ((ex > a.x && x < a.x) || (ex < a.x && x > a.x)) x = a.x;
    if ((ey > a.y && y < a.y) || (ey < a.y && y > a.y)) y = a.y;
    if ((ez > a.z && z < a.z) || (ez < a.z && z > a.z)) z = a.z;
}

// ---------------------------------------------------------------------------------------------
// Moving with collision (physics.md 4)

int World::collide_move(uint16_t id, int x, int y, int z, bool teleport, bool force, uint16_t* blocker, unsigned* dirs) {
    Item* it = item(id);
    if (!it) return 0;
    if (blocker) *blocker = 0;
    if (dirs) *dirs = 0;
    bool in_world = it->chunk >= 0 && !it->parent && !(it->flags & kItemEthereal);
    // Original bug fixes: a walk, fall or throw stops at the edge of the chunk grid (0..32767)
    // instead of carrying the item out of every chunk (where nothing can see or reach it).
    const int xy_max = original_bug_fixes() && in_world && !teleport ? kMapMax : 0xFFFF;
    x = std::clamp(x, 0, xy_max);
    y = std::clamp(y, 0, xy_max);
    z = std::clamp(z, 0, 255);
    if (!in_world) teleport = true;  // only the destination is tested
    Box start = box_of(*it);
    if (!in_world) { start.x = x; start.y = y; start.z = z; }
    int dx = std::abs(x - start.x), dy = std::abs(y - start.y), dz = std::abs(z - start.z);
    int force_val = (dx / 4 + dy / 4 + dz + std::max(dy / 4, dz)) / 2;
    uint16_t mf = shape_flags(*it);
    std::vector<SweepHit> hits;
    sweep(start, x, y, z, mf, id, false, hits);

    struct Ev { uint16_t item; bool hit, release; };
    std::vector<Ev> evs;
    int stop = kT;
    const SweepHit* stopper = nullptr;
    if (teleport) {
        if (!force)
            for (const SweepHit& h : hits)
                if (h.blocking && !h.touching && h.end_time == kT) {
                    if (blocker) *blocker = h.item;
                    if (dirs) *dirs = h.dirs;
                    return 0;
                }
        // Events: overlapping at the end -> gotHit/hit; only at the start -> release.
        for (const SweepHit& h : hits) {
            bool at_start = h.hit_time < 0, at_end = h.end_time == kT;
            if (h.touching && !h.floor) continue;
            if (at_start && at_end) continue;
            if (at_end) evs.push_back({h.item, true, false});
            else if (at_start) evs.push_back({h.item, false, true});
        }
    } else {
        if (!force)
            for (const SweepHit& h : hits)
                if (h.blocking && !h.touching) {
                    stop = std::max(0, h.hit_time);
                    stopper = &h;
                    if (blocker) *blocker = h.item;
                    if (dirs) *dirs = h.dirs;
                    break;
                }
        for (const SweepHit& h : hits) {
            if (h.hit_time > stop) break;
            bool hit = (!h.touching || h.floor) && h.hit_time >= 0;
            bool release = h.end_time < stop;
            if (hit || release) evs.push_back({h.item, hit, release});
        }
        if (stop < kT && stopper) {
            int cx, cy, cz;
            contact_point(start, x, y, z, *stopper, cx, cy, cz);
            x = cx; y = cy; z = cz;
        }
    }
    move_to(id, x, y, z, false);
    bool any_release = false;
    for (const Ev& e : evs) {
        if (!item(e.item) || !item(id)) break;
        uint16_t gp = 0;
        if (e.hit) {
            gp = call_event_ww(e.item, kEvGotHit, id, uint16_t(force_val));
            call_event_ww(id, kEvHit, e.item, uint16_t(force_val));
        }
        if (e.release && item(e.item)) {
            any_release = true;
            uint16_t rp = call_event(e.item, kEvRelease);
            if (gp && rp)
                if (Process* p = kernel_.get(rp)) p->wait_for(gp);
        }
    }
    if (any_release && item(id)) call_event(id, kEvRelease);
    return stop;
}

// ---------------------------------------------------------------------------------------------
// Reach and line of sight (physics.md 5, avatar.md 8.1)

bool World::can_reach_box(uint16_t from, const Box& tb, uint16_t target, int range, uint16_t ignore) const {
    const Item* me = item(from);
    if (!me) return false;
    Box mb = box_of(*me);
    // 1. Distance on x and y only.
    if ((mb.x - mb.xd) - tb.x > range || (tb.x - tb.xd) - mb.x > range || (mb.y - mb.yd) - tb.y > range ||
        (tb.y - tb.yd) - mb.y > range)
        return false;
    // 2. Up to three sight lines, a 2x2x2 box through solid things.
    auto line = [&](int fx, int fy, int fz, int tx, int ty, int tz) {
        Box s{fx, fy, fz, 2, 2, 2};
        std::vector<SweepHit> hits;
        sweep(s, tx, ty, tz, ShapeInfo::kSolid, from, true, hits);
        int target_time = kT + 1;
        if (target)
            for (const SweepHit& h : hits)
                if (h.item == target) { target_time = h.hit_time; break; }
        for (const SweepHit& h : hits) {
            if (h.item == target || h.item == ignore || h.touching || !h.blocking) continue;
            if (!target || h.hit_time < target_time) return false;
        }
        return true;
    };
    int fz = mb.z;
    int tz = tb.z;
    if (tb.z > mb.z && tb.z < mb.z + mb.zd) fz = tb.z;
    if (line(mb.x, mb.y, fz, tb.x, tb.y, tz)) return true;
    int ex = mb.x - mb.xd / 2, ey = mb.y - mb.yd / 2, ez = mb.zd > 16 ? mb.z + mb.zd - 8 : mb.z;
    int cx = tb.x - tb.xd / 2 + 1, cy = tb.y - tb.yd / 2 + 1;
    if (line(ex, ey, ez, cx, cy, tb.z + tb.zd / 2)) return true;
    return line(ex, ey, ez, cx, cy, tb.z + tb.zd);
}

bool World::can_reach(uint16_t from, uint16_t target, int range) const {
    const Item* t = item(target);
    if (!t || !item(from)) return false;
    const Item* rt = root_container(*t);
    if (rt->id == from) return true;  // carried by me
    return can_reach_box(from, box_of(*rt), rt->id, range);
}

// ---------------------------------------------------------------------------------------------
// Falling and throwing (physics.md 7, 10)

uint16_t World::hurl(uint16_t id, int vx, int vy, int vz, int g) {
    Item* it = item(id);
    if (!it || it->parent || (it->flags & kItemEthereal)) return 0;
    if (it->chunk < 0) return 0;
    if (auto* gp = dynamic_cast<GravityProcess*>(kernel_.get(it->gravity_pid)); gp && gp->live()) {
        gp->add_velocity(vx, vy, vz, g);
        return it->gravity_pid;
    }
    it->gravity_pid = kernel_.add(std::make_unique<GravityProcess>(*this, id, vx, vy, vz, g));
    return it->gravity_pid;
}

uint16_t World::fall(uint16_t id) {
    Item* it = item(id);
    if (!it) return 0;
    const ShapeInfo& si = data_.typeflags.get(it->shape);
    if ((it->flags & kItemHanging) || si.is(ShapeInfo::kFixed) || si.weight() == 0) return 0;
    return hurl(id, 0, 0, 0, 4);
}

void World::missile_velocity(int fx, int fy, int fz, int tx, int ty, int tz, int s, int g, int& vx, int& vy, int& vz,
                             int& n) const {
    s = std::max(1, s);
    int dx = tx - fx, dy = ty - fy, dz = tz - fz;
    n = (std::abs(dx) + std::abs(dy) + s / 2) / s;
    if (n > 0) {
        vz = (g * n * (n - 1) / 2 + dz) / n;
        if (vz > s / 4) {
            if (g == 0 || s / (4 * g) <= n) {
                if (s >= 4 && dz / (s / 4) > n) n = dz / (s / 4);
            } else {
                n = s / (4 * g);
            }
            n = std::max(1, n);
            vz = (g * n * (n - 1) / 2 + dz) / n;
        }
        vx = (dx + n / 2) / n;
        vy = (dy + n / 2) / n;
    } else {
        vz = dz > 0 ? s / 4 : -s / 4;
        vx = vy = 0;
    }
}

bool World::missile_path_clear(uint16_t id, int fx, int fy, int fz, int tx, int ty, int tz, int s, int g,
                               uint16_t thrower) const {
    const Item* it = item(id);
    if (!it) return false;
    int vx, vy, vz, n;
    missile_velocity(fx, fy, fz, tx, ty, tz, s, g, vx, vy, vz, n);
    Box b = box_of(*it);
    b.x = fx; b.y = fy; b.z = fz;
    uint16_t mf = shape_flags(*it);
    std::vector<SweepHit> hits;
    for (int i = 0; i < std::max(1, n); ++i) {
        int nx = b.x + vx, ny = b.y + vy, nz = std::max(0, b.z + vz);
        sweep(b, nx, ny, nz, mf, id, true, hits);
        bool last = i + 1 >= std::max(1, n);
        for (const SweepHit& h : hits) {
            if (!h.blocking || h.touching || h.item == thrower) continue;
            // Integer velocities can dip a few units into the landing surface on the last
            // frame: landing on top of something there is not an obstruction.
            if (last && h.dirs == 4 && vz < 0) continue;
            return false;
        }
        b.x = nx; b.y = ny; b.z = nz;
        vz -= g;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Stacking (physics.md 8)

void World::items_on_top(uint16_t id, std::vector<uint16_t>& out, bool recursive) const {
    std::vector<uint16_t> s;
    surface_search(id, true, s);
    for (uint16_t k : s) {
        if (std::find(out.begin(), out.end(), k) != out.end() || k == id) continue;
        out.push_back(k);
        if (recursive && out.size() < 256) items_on_top(k, out, true);
    }
}

void World::grab(uint16_t id) {
    std::vector<uint16_t> on, under;
    items_on_top(id, on, true);
    surface_search(id, false, under);
    for (uint16_t k : on) fall(k);
    for (uint16_t k : under) call_event(k, kEvRelease);
}

bool World::ascend(uint16_t id, int dz) {
    Item* it = item(id);
    if (!it) return false;
    if (dz == 0) return true;
    std::vector<uint16_t> on;
    items_on_top(id, on, true);
    struct Old { uint16_t id; int x, y, z; };
    std::vector<Old> carried;
    for (uint16_t k : on) {
        const Item* ki = item(k);
        if (!ki || data_.typeflags.get(ki->shape).is(ShapeInfo::kFixed)) continue;
        carried.push_back({k, ki->x, ki->y, ki->z});
    }
    for (const Old& o : carried) push_ether(o.id);
    int start_z = it->z;
    int frac = collide_move(id, it->x, it->y, std::clamp(it->z + dz, 0, 255), false, false);
    int moved = (it = item(id)) ? it->z - start_z : 0;
    (void)frac;
    for (size_t i = carried.size(); i-- > 0;) {
        const Old& o = carried[i];
        Item* ki = item(o.id);
        if (!ki) continue;
        int xd, yd, zd;
        footprint(*ki, xd, yd, zd);
        int nz = std::clamp(o.z + moved, 0, 255);
        if (can_exist(o.x, o.y, nz, xd, yd, zd, shape_flags(*ki), o.id).valid) {
            move_to(o.id, o.x, o.y, nz, false);
        } else {
            move_to(o.id, o.x, o.y, o.z, false);
            if (moved < 0) fall(o.id);
        }
    }
    return moved == dz;
}

}  // namespace pg
