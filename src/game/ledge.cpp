// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Easy ledges (see ledge.h). Uses only the engine's own placement test, the surface probe of
// the movement assist and animation trials, so every ledge it offers is one the climb or
// hang animations can really do from that spot, ending supported.
#include "game/ledge.h"

#include <algorithm>
#include <cstdlib>

#include "game/anim_tracker.h"
#include "data/gamedata.h"
#include "game/assist.h"
#include "game/world.h"

namespace pg {
namespace ledge {

const unsigned kPulls[5] = {22, 21, 23, 20, 24};

namespace {

int climb_rise(unsigned action) { return 16 + 8 * (int(action) - int(kClimb16)); }

// Standable spots on a top (box positions), where a steered climb may end.
struct Spots {
    int n = 0;
    int x[12], y[12];
};

struct Ctx {
    const World& w;
    const Actor& a;
    int xd = 0, yd = 0, zd = 0;
    uint16_t flags = 0;
    mutable int trials = 0, budget = 1 << 30;  // animation trials run / allowed
    Ctx(const World& world, const Actor& actor) : w(world), a(actor) {
        w.footprint(a, xd, yd, zd);
        flags = w.shape_flags(a);
    }
    bool valid(int x, int y, int z) const { return w.can_exist(x, y, z, xd, yd, zd, flags, a.id).valid; }
    // A roof piece (ROOF) beside the box at (x, y) on a top at z, or just above it: the
    // spot is on a building (an eave, a wall under the roof's edge), not a ledge.
    bool roof_near(int x, int y, int z) const {
        const CurrentMap& m = const_cast<World&>(w).map();
        int x0 = x - xd - kRoofGap, x1 = x + kRoofGap, y0 = y - yd - kRoofGap, y1 = y + kRoofGap;
        for (int cy = std::max(0, y0 / 512); cy <= std::min(CurrentMap::kChunks - 1, y1 / 512); ++cy)
            for (int cx = std::max(0, x0 / 512); cx <= std::min(CurrentMap::kChunks - 1, x1 / 512); ++cx)
                for (uint16_t k : m.chunk(cx, cy)) {
                    const Item* o = w.item(k);
                    if (!o || k == a.id) continue;
                    uint16_t f = w.data().typeflags.get(o->shape).flags();
                    if (!(f & ShapeInfo::kRoof)) continue;
                    int oxd, oyd, ozd;
                    w.footprint(*o, oxd, oyd, ozd);
                    if (o->x <= x0 || o->x - oxd >= x1 || o->y <= y0 || o->y - oyd >= y1) continue;
                    if (o->z + ozd < z - 16 || o->z > z + 48) continue;
                    return true;
                }
        return false;
    }
    // Standing on a ledge surface at z (for the landing spots).
    bool stand_top(int x, int y, int z) const {
        World::Placement p = w.can_exist(x, y, z, xd, yd, zd, flags, a.id);
        return p.valid && p.supported && p.support_z == z && ledge_surface(w, p.land) && !roof_near(x, y, z);
    }
    // Standing on level ground at z (no step, no drop).
    bool stand(int x, int y, int z) const {
        World::Placement p = w.can_exist(x, y, z, xd, yd, zd, flags, a.id);
        return p.valid && p.supported && p.support_z == z;
    }
    // A trial of a climb from (x, y, z) (standing, or hanging), steered to the target if any.
    bool trial(const Move& m, int dir, int x, int y, int z, unsigned from_anim) const {
        if (++trials > budget) return false;
        ActorState st;
        st.x = x; st.y = y; st.z = z;
        st.dir = uint8_t(dir);
        st.last_anim = uint16_t(from_anim);
        st.last_anim_dir = uint16_t(dir);
        AnimTracker t(w);
        if (!t.init(a.id, m.action, dir, &st)) return false;
        if (m.targeted) t.set_target(m.tx, m.ty, m.tz);
        if (t.run_all(true) != AnimTracker::kOk) return false;
        // Where it really ends must be a ledge (not a wall's top, a roof ...).
        return ledge_surface(w, w.can_exist(t.x(), t.y(), t.z(), xd, yd, zd, flags, a.id).land) && !roof_near(t.x(), t.y(), t.z());
    }
    // The tops of what stands just ahead of the box at (x, y) toward dir, above zlo and at
    // most zhi, lowest first (up to 6). Probes 4..36 units beyond his front, across his width.
    int tops(int x, int y, int dir, int zlo, int zhi, int out[6]) const {
        int n = 0;
        int dx = dir_x(dir), dy = dir_y(dir);
        int cx = x - xd / 2, cy = y - yd / 2;
        int px = -dy, py = dx;
        const int ahead[4] = {4, 12, 24, 36};
        const int side[3] = {0, xd / 2 - 8, -(xd / 2 - 8)};
        for (int e : ahead)
            for (int s : side) {
                int qx = cx + dx * (xd / 2 + e) + px * s, qy = cy + dy * (yd / 2 + e) + py * s;
                int top;
                uint16_t f, id;
                if (!assist::surface_under(w, qx, qy, zhi - 8, a.id, top, f, id)) continue;
                if (top <= zlo || top > zhi || !ledge_surface(w, id)) continue;
                bool dup = false;
                for (int k = 0; k < n; ++k) dup |= out[k] == top;
                if (!dup && n < 6) out[n++] = top;
            }
        std::sort(out, out + n);
        return n;
    }
    void edge(int x, int y, int dir, int top, int e[3]) const {
        e[0] = x - xd / 2 + dir_x(dir) * (xd / 2 + 4);
        e[1] = y - yd / 2 + dir_y(dir) * (yd / 2 + 4);
        e[2] = top;
    }
    // The standable spots on the top ahead of (x, y): 8..72 ahead, up to 16 to either side,
    // nearest first (up to 12). None: nothing to climb onto there (a parapet, a wall on the
    // edge, a top too small to stand on).
    void spots_on(int x, int y, int dir, int top, Spots& out) const {
        out.n = 0;
        int dx = dir_x(dir), dy = dir_y(dir), px = -dy, py = dx;
        for (int e = 8; e <= 72 && out.n < 6; e += 8)
            for (int s : {0, 8, -8, 16, -16}) {
                int qx = x + e * dx + s * px, qy = y + e * dy + s * py;
                if (out.n < 6 && stand_top(qx, qy, top)) { out.x[out.n] = qx; out.y[out.n] = qy; ++out.n; }
            }
    }
    // A climb `action` from (x, y, z) onto the top: as it is, or steered onto one of the two
    // standable spots nearest the animation's own end.
    bool climb_onto(unsigned action, int dir, int x, int y, int z, unsigned from_anim, int top, const Spots& sp, Move& out) const {
        if (sp.n == 0) return false;
        Move m;
        m.action = action;
        int end = z + climb_rise(action);
        if (end >= top && end - top <= 8 && trial(m, dir, x, y, z, from_anim)) { out = m; return true; }
        if (std::abs(end - top) > 12) return false;  // beyond the steering's reach in z (16)
        ActorState st;
        st.x = x; st.y = y; st.z = z;
        st.dir = uint8_t(dir);
        st.last_anim = uint16_t(from_anim);
        st.last_anim_dir = uint16_t(dir);
        AnimTracker t(w);
        if (!t.init(a.id, action, dir, &st)) return false;
        int tdx, tdy, tdz;
        t.travel(tdx, tdy, tdz);
        int nx = x + tdx, ny = y + tdy;
        int best[2] = {-1, -1}, bd[2] = {1 << 30, 1 << 30};
        for (int i = 0; i < sp.n; ++i) {
            int d = std::abs(sp.x[i] - nx) + std::abs(sp.y[i] - ny);
            if (d < bd[0]) { bd[1] = bd[0]; best[1] = best[0]; bd[0] = d; best[0] = i; }
            else if (d < bd[1]) { bd[1] = d; best[1] = i; }
        }
        for (int i : best) {
            if (i < 0) continue;
            m.targeted = true;
            m.tx = sp.x[i]; m.ty = sp.y[i]; m.tz = top;
            if (trial(m, dir, x, y, z, from_anim)) { out = m; return true; }
        }
        return false;
    }
    // A pull-up from the hang at (x, y, z) that reaches one of the tops ahead.
    bool pull(int x, int y, int z, int dir, Move& out) const {
        int t[6];
        int n = tops(x, y, dir, z + 8, z + 72, t);
        for (int k = 0; k < n; ++k) {
            Spots sp;
            spots_on(x, y, dir, t[k], sp);
            for (unsigned c : kPulls)
                if (climb_onto(c, dir, x, y, z, kHang, t[k], sp, out)) return true;
        }
        return false;
    }
};

void fill(const Ctx& c, Plan& p, int x, int y, int z, int dir, int top, const Move& m) {
    p = Plan{};
    p.dir = dir; p.sx = x; p.sy = y; p.sz = z;
    p.move = m;
    p.rise = top - z;
    int e[3];
    c.edge(x, y, dir, top, e);
    p.ex = e[0]; p.ey = e[1]; p.ez = e[2];
}

// At the spot (x, y, z) facing dir: a direct climb onto a top, or a jump to a hang point
// from which a pull-up works. The column above the spot must be clear up to the hang.
bool try_spot(const Ctx& c, int x, int y, int z, int dir, Plan& p) {
    int t[6];
    int n = c.tops(x, y, dir, z + 12, z + kMaxRise, t);
    int dx = dir_x(dir), dy = dir_y(dir);
    for (int i = 0; i < n; ++i) {
        int rise = t[i] - z;
        Spots sp;
        c.spots_on(x, y, dir, t[i], sp);
        if (sp.n == 0) continue;
        Move m;
        if (rise <= kMaxDirect + 8) {
            int k0 = std::clamp((rise - 16) / 8, 0, 7);
            for (int k = k0; k <= std::min(7, k0 + 1); ++k)
                if (c.climb_onto(kClimb16 + unsigned(k), dir, x, y, z, 2, t[i], sp, m)) {
                    fill(c, p, x, y, z, dir, t[i], m);
                    return true;
                }
        }
        // Jump, hang, pull up: hang points for the pull-ups that reach this top (the steering
        // takes up a top that is not a multiple of 8 above the hang).
        for (unsigned pull : kPulls) {
            int hz = t[i] - climb_rise(pull);
            if (hz < z || hz - z > kMaxHang) continue;
            bool clear = true;
            for (int zz = z + 8; zz < hz && clear; zz += 8) clear = c.valid(x, y, zz);
            if (!clear || !c.valid(x, y, hz)) continue;
            for (int hf = 0; hf <= kHangForward; hf += 8) {
                int hx = x + hf * dx, hy = y + hf * dy;
                if (hf && !c.valid(hx, hy, hz)) break;
                if (!c.climb_onto(pull, dir, hx, hy, hz, kHang, t[i], sp, m)) continue;
                fill(c, p, x, y, z, dir, t[i], m);
                p.grab = true;
                p.hx = hx; p.hy = hy; p.hz = hz;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool ledge_surface(const World& w, uint16_t item) {
    const Item* it = w.item(item);
    if (!it) return false;
    const ShapeInfo& si = w.data().typeflags.get(it->shape);
    uint16_t f = si.flags();
    if (f & ShapeInfo::kRoof) return false;  // (the roofs' pieces carry LAND too)
    if ((f & ShapeInfo::kDamaging) || ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand))) return false;
    int xd, yd, zd;
    w.footprint(*it, xd, yd, zd);
    return !(std::min(xd, yd) <= 32 && zd >= 32);
}

bool ledge_spot(const World& w, const Actor& a, int x, int y, int z) {
    Ctx c(w, a);
    return c.stand_top(x, y, z);
}

bool pull_from(const World& w, const Actor& a, int x, int y, int z, int dir, Move& out) {
    Ctx c(w, a);
    return c.pull(x, y, z, dir, out);
}

bool find(const World& w, const Actor& a, int pref_dir, bool around, Plan& out, bool narrow) {
    Ctx c(w, a);
    c.budget = kSearchTrials;
    const int order[8] = {0, 1, -1, 2, -2, 3, -3, 4};
    const int lats[9] = {0, 8, -8, 16, -16, 24, -24, 32, -32};
    const int lats_around[3] = {0, 16, -16};
    for (int oi = 0; oi < (narrow ? 1 : around ? 8 : 3); ++oi) {
        int dir = (pref_dir + order[oi] + 8) & 7;
        int reach = oi < 3 ? kReach : kAroundReach;
        int dx = dir_x(dir), dy = dir_y(dir);
        int px = -dy, py = dx;
        bool found = false;
        Plan best;
        // All round (beyond the arc) only three lanes: straight, and 16 to either side.
        const int* lanes = oi < 3 ? lats : lats_around;
        for (int li = 0; li < (narrow ? 5 : oi < 3 ? 9 : 3); ++li) {
            int lat = lanes[li];
            int alat = std::abs(lat), sg = lat < 0 ? -1 : 1;
            if (found && alat >= best.travel) break;  // the lanes are nearest first
            // Sideways to the lane (level, supported all the way).
            bool ok = true;
            for (int s = 4; s <= alat && ok; s += 4) ok = c.stand(a.x + sg * s * px, a.y + sg * s * py, a.z);
            if (!ok) continue;
            int lx = a.x + lat * px, ly = a.y + lat * py;
            // Forward to the face.
            int last = -1;
            bool wall = false;
            for (int t = 0; t <= reach; t += 4) {
                int x = lx + t * dx, y = ly + t * dy;
                if (!c.valid(x, y, a.z)) { wall = true; break; }
                if (t > 0 && !c.stand(x, y, a.z)) break;  // (where he stands now may be an edge)
                last = t;
            }
            if (last < 0) continue;
            // The spot flush with the face, then a little back; where nothing blocks the lane
            // (a low ledge the box passes over, the end of his reach) only its end.
            const int backs[3] = {0, 8, 16};
            for (int b : backs) {
                int t = last - b;
                if (t < 0 || (!wall && b > 0)) break;
                int travel = alat + t;
                if (found && travel >= best.travel) break;
                Plan p;
                if (try_spot(c, lx + t * dx, ly + t * dy, a.z, dir, p)) {
                    p.travel = travel;
                    best = p;
                    found = true;
                    break;
                }
            }
        }
        if (found) { out = best; return true; }
    }
    return false;
}

bool catch_point(const World& w, const Actor& a, int fall, int& x, int& y, int& z, int& dir, int edge[3]) {
    Ctx c(w, a);
    c.budget = kCatchTrials;
    const int order[3] = {0, 1, -1};
    const int lats[5] = {0, 8, -8, 16, -16};
    int lo = a.z - kCatchBelow, hi = a.z + kCatchAbove + std::max(0, fall);
    for (int oi : order) {
        int d = (a.dir + oi + 8) & 7;
        int dx = dir_x(d), dy = dir_y(d);
        int px = -dy, py = dx;
        for (int lat : lats) {
            int alat = std::abs(lat), sg = lat < 0 ? -1 : 1;
            bool ok = true;
            for (int s = 4; s <= alat && ok; s += 4) ok = c.valid(a.x + sg * s * px, a.y + sg * s * py, a.z);
            if (!ok) continue;
            int bx = a.x + lat * px, by = a.y + lat * py;
            int t[6];
            int n = c.tops(bx, by, d, lo + 16, hi + 56, t);
            for (int i = 0; i < n; ++i) {
                // A jump gains no storeys: the top may be no higher above the ground he last
                // stood on than a ledge he could grab from there. A fall from above stays free.
                if (w.void_rescue.valid && t[i] > w.void_rescue.z + kMaxRise) continue;
                Spots sp;
                c.spots_on(bx, by, d, t[i], sp);
                if (sp.n == 0) continue;
                for (unsigned pull : kPulls)
                    for (int adj = 0; adj <= 4; adj += 4) {
                        int hz = t[i] - climb_rise(pull) + adj;
                        if (hz < lo || hz > hi || hz < 0) continue;
                        bool clear = true;
                        for (int zz = std::min(a.z, hz) + 4; zz < std::max(a.z, hz) && clear; zz += 4) clear = c.valid(bx, by, zz);
                        if (!clear || !c.valid(bx, by, hz)) continue;
                        for (int hf = 0; hf <= kHangForward; hf += 8) {
                            int hx = bx + hf * dx, hy = by + hf * dy;
                            if (hf && !c.valid(hx, hy, hz)) break;
                            Move m;
                            if (!c.climb_onto(pull, d, hx, hy, hz, kHang, t[i], sp, m)) continue;
                            x = hx; y = hy; z = hz; dir = d;
                            c.edge(hx, hy, d, t[i], edge);
                            return true;
                        }
                    }
            }
        }
    }
    return false;
}

}  // namespace ledge
}  // namespace pg
