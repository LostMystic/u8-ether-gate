// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6 movement assist and edge guard (see assist.h). Uses only the engine's own
// placement test, swept-box test and animation trials, so the assist never puts the Avatar
// anywhere he could not walk to himself.
#include "game/assist.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/world.h"

namespace pg {
namespace assist {

std::vector<std::array<int, 4>> last_probes;

bool surface_under(const World& w, int cx, int cy, int z, uint16_t self, int& top, uint16_t& flags, uint16_t& id) {
    top = -1000;
    flags = 0;
    id = 0;
    const CurrentMap& m = const_cast<World&>(w).map();
    int ccx = cx / 512, ccy = cy / 512;
    for (int yy = std::max(0, ccy - 1); yy <= std::min(CurrentMap::kChunks - 1, ccy + 1); ++yy)
        for (int xx = std::max(0, ccx - 1); xx <= std::min(CurrentMap::kChunks - 1, ccx + 1); ++xx)
            for (uint16_t k : m.chunk(xx, yy)) {
                if (k == self) continue;
                const Item* o = w.item(k);
                if (!o || (o->ext_flags & kExtSprite) || w.passable_door(*o)) continue;
                const ShapeInfo& si = w.data().typeflags.get(o->shape);
                uint16_t f = si.flags();
                if (!(f & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof | ShapeInfo::kSea | ShapeInfo::kDamaging)))
                    continue;
                int xd, yd, zd;
                si.footprint(o->flags & kItemFlipped, xd, yd, zd);
                if (cx <= o->x - xd || cx > o->x || cy <= o->y - yd || cy > o->y) continue;
                int t = o->z + zd;
                if (t > z + 8) continue;
                // The highest wins; on a tie, land is the safer reading.
                if (t > top || (t == top && (f & ShapeInfo::kLand) && !(flags & ShapeInfo::kLand))) {
                    top = t;
                    flags = f;
                    id = k;
                }
            }
    return id != 0;
}

bool dangerous(const World& w, const Actor& a, int x, int y, int z) {
    int xd, yd, zd;
    w.footprint(a, xd, yd, zd);
    int cx = x - xd / 2, cy = y - yd / 2;
    int top;
    uint16_t f, id;
    if (!surface_under(w, cx, cy, z, a.id, top, f, id)) return z > kSafeDrop;
    if ((f & ShapeInfo::kDamaging) || ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand))) return true;
    return z - top > kSafeDrop;
}

bool hazard(const World& w, const Actor& a, int x, int y, int z) {
    if (dangerous(w, a, x, y, z)) return true;
    int xd, yd, zd;
    w.footprint(a, xd, yd, zd);
    // The middle and the four corners (kLandInset inside). Lava (409 CLRLAVA, 483) is LAND to the
    // engine and hurts through its usecode (specs/world/creature_facts.md 2.1, 2.3), so it is named.
    const int ix[5] = {x - xd / 2, x - kLandInset, x - xd + kLandInset, x - kLandInset, x - xd + kLandInset};
    const int iy[5] = {y - yd / 2, y - kLandInset, y - kLandInset, y - yd + kLandInset, y - yd + kLandInset};
    for (int k = 0; k < 5; ++k) {
        int top;
        uint16_t f, id;
        if (!surface_under(w, ix[k], iy[k], z, a.id, top, f, id) || z - top > 8) continue;
        if ((f & ShapeInfo::kDamaging) || ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand))) return true;
        if (const Item* it = w.item(id); it && (it->shape == 409 || it->shape == 483)) return true;
    }
    return false;
}

bool safe_landing(const World& w, const Actor& a, int x, int y, int z) {
    int xd, yd, zd;
    w.footprint(a, xd, yd, zd);
    World::Placement pl = w.can_exist(x, y, z, xd, yd, zd, w.shape_flags(a), a.id);
    if (!pl.valid || !pl.supported || dangerous(w, a, x, y, z)) return false;
    const int ix[4] = {x - kLandInset, x - xd + kLandInset, x - kLandInset, x - xd + kLandInset};
    const int iy[4] = {y - kLandInset, y - kLandInset, y - yd + kLandInset, y - yd + kLandInset};
    for (int k = 0; k < 4; ++k) {
        int top;
        uint16_t f, id;
        if (!surface_under(w, ix[k], iy[k], z, a.id, top, f, id) || z - top > 8) return false;
        if ((f & ShapeInfo::kDamaging) || ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand))) return false;
    }
    return true;
}

bool find_landing(const World& w, const Actor& a, int dir, int dist, int nx, int ny, int nz, int out[3]) {
    if (safe_landing(w, a, nx, ny, nz)) { out[0] = nx; out[1] = ny; out[2] = nz; return true; }
    const double base = std::atan2(double(dir_y(dir)), double(dir_x(dir)));
    // The surface he takes off from does not count as a landing (that is no jump).
    int xd0, yd0, zd0;
    w.footprint(a, xd0, yd0, zd0);
    int top0;
    uint16_t f0, from = 0;
    surface_under(w, a.x - xd0 / 2, a.y - yd0 / 2, a.z, a.id, top0, f0, from);
    const int dmin = std::max(32, dist * kJumpMinPct / 100), dmax = dist * kJumpMaxPct / 100 + kJumpSlack;
    for (int deg = 0; deg <= kJumpArcDeg; deg += 5)
        for (int side : {1, -1}) {
            if (deg == 0 && side < 0) continue;
            double ang = base + side * deg * M_PI / 180.0;
            // Nearest to the natural distance first: alternate shorter and longer.
            for (int k = 0; k <= (dmax - dmin) / 8 + 1; ++k) {
                int off = (k + 1) / 2 * 8 * ((k & 1) ? -1 : 1);
                int d = std::clamp(dist, dmin, dmax) + off;
                if (d < dmin || d > dmax) continue;
                int x = a.x + int(std::lround(std::cos(ang) * d)), y = a.y + int(std::lround(std::sin(ang) * d));
                int xd, yd, zd;
                w.footprint(a, xd, yd, zd);
                int top;
                uint16_t f, id;
                if (!surface_under(w, x - xd / 2, y - yd / 2, a.z + kJumpRise, a.id, top, f, id)) continue;
                if (top < a.z - kSafeDrop || top > a.z + kJumpRise) continue;
                if (id == from || !safe_landing(w, a, x, y, top)) continue;
                out[0] = x; out[1] = y; out[2] = top;
                if (w.debug_assist) PG_LOG("jump: landing moved %d degrees, %d units (natural %d) -> %d,%d,%d", side * deg, d, dist, x, y, top);
                return true;
            }
        }
    return false;
}

namespace {
// A trial of `action` in `dir` from the actor's state moved to (x, y, z); the end point.
bool trial_from(World& w, const Actor& a, unsigned action, int dir, int x, int y, int z, int& ex, int& ey, int& ez) {
    ActorState st;
    st.x = x; st.y = y; st.z = z;
    st.dir = uint8_t(dir);
    st.last_anim = a.last_anim;
    st.last_anim_dir = a.last_anim_dir;
    st.first_step = a.is(Actor::kFirstStep);
    st.flipped = a.flags & kItemFlipped;
    st.in_combat = a.is(Actor::kInCombat);
    AnimTracker t(w);
    if (!t.init(a.id, action, dir, &st)) return false;
    if (t.run_all(false) != AnimTracker::kOk) return false;
    ex = t.x(); ey = t.y(); ez = t.z();
    return true;
}
}  // namespace

bool find(World& w, const Actor& a, unsigned action, int dir, Candidate& out) {
    last_probes.clear();
    Box b = w.box_of(a);
    double fx = dir_x(dir), fy = dir_y(dir);
    double n = std::sqrt(fx * fx + fy * fy);
    fx /= n;
    fy /= n;
    const double px = -fy, py = fx;  // perpendicular
    const uint16_t mf = w.shape_flags(a);
    std::vector<SweepHit> hits;
    for (int off = kProbeStep; off <= kProbeMax; off += kProbeStep)
        for (int side : {1, -1}) {
            int o = off * side;
            int tx = b.x + int(std::lround(px * o)), ty = b.y + int(std::lround(py * o)), tz = b.z;
            // 1. He can stand there, safely.
            World::Placement pl = w.can_exist(tx, ty, tz, b.xd, b.yd, b.zd, mf, a.id);
            if (!pl.valid || !pl.supported || dangerous(w, a, tx, ty, tz)) { last_probes.push_back({tx, ty, tz, 0}); continue; }
            // 2. Short shifts slide straight there: nothing in between.
            if (off <= kSnap) {
                w.sweep(b, tx, ty, tz, mf, a.id, true, hits);
                bool blocked = false;
                for (const SweepHit& h : hits)
                    if (h.blocking && h.hit_time < 0x4000 && !h.touching) blocked = true;
                if (blocked) { last_probes.push_back({tx, ty, tz, 0}); continue; }
            }
            // 3. From there the way on is clear and safe for at least kAhead units.
            int x = tx, y = ty, z = tz, gone = 0;
            bool ok = true;
            for (int cycle = 0; cycle < 4 && gone < kAhead; ++cycle) {
                int ex, ey, ez;
                if (!trial_from(w, a, action, dir, x, y, z, ex, ey, ez) || dangerous(w, a, ex, ey, ez)) { ok = false; break; }
                int d = std::max(std::abs(ex - x), std::abs(ey - y));
                if (d == 0) { ok = false; break; }
                gone += d;
                x = ex; y = ey; z = ez;
            }
            last_probes.push_back({tx, ty, tz, ok ? 2 : 1});
            if (!ok) continue;
            out.x = tx; out.y = ty; out.z = tz;
            out.offset = o;
            if (w.debug_assist)
                PG_LOG("assist: dir %d action %u: sideways %d -> %d,%d (%zu probes)", dir, action, o, tx, ty, last_probes.size());
            return true;
        }
    if (w.debug_assist) PG_LOG("assist: dir %d action %u: no spot within %d (%zu probes)", dir, action, kProbeMax, last_probes.size());
    return false;
}

}  // namespace assist
}  // namespace pg
