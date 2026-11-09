// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The critical-path walking route (see route_guide.h): a lazily built walk grid of the current
// map, a sliced weighted A* over it, and the real pathfinder for the near stretch.
#include "game/route_guide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/pathfinder.h"
#include "game/world.h"

namespace pg {

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

constexpr int kCell = 32, kCells = 32768 / kCell;    // the walk grid: 1024 x 1024 cells
constexpr int kBucket = 64, kBuckets = 32768 / kBucket;  // collider buckets: 512 x 512
constexpr int kStepUp = 16, kClimbMax = 72, kDropMax = 64;
constexpr int kMaxExpand = 250000;
constexpr int kMaxLevels = 6;
constexpr int kGoalR = 56;  // arrived: this close to the target (xy), on its level
constexpr float kJumpCost = 64;  // taking a teleporter, as walking distance

// One thing that can block or hold the Avatar.
struct Col {
    int x, y, z, xd, yd, zd;  // max corner x / y, bottom z (physics.md notation)
    uint16_t f;               // shape flags
    bool door;
    bool tele;                // a teleporter egg's trigger range (items.md 8.1), not a thing
    int16_t chunk;            // the chunk list it came from
};

constexpr uint8_t kLvTele = 0x80;  // Level flag: standing here sets off a teleporter (or nearly)
constexpr int kTeleMargin = 8;     // how near a teleporter's trigger range counts as in it

struct Level {
    int16_t z;
    uint8_t flags;  // RoutePoint::kDoor: a (closed) door overlaps the Avatar's box here
};

bool overlap_xy(const Col& c, int x, int y, int xd, int yd) {
    return x - xd < c.x && x > c.x - c.xd && y - yd < c.y && y > c.y - c.yd;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The walk grid of one map

struct WalkGrid {
    const World& w;
    unsigned map = 0xFFFF;
    int xd = 64, yd = 64, zd = 40;  // the Avatar's box
    uint32_t serial = 1;            // plan number (chunks are re-checked once per plan)
    std::vector<uint8_t> built = std::vector<uint8_t>(64 * 64, 0);
    std::vector<uint32_t> sig = std::vector<uint32_t>(64 * 64, 0), checked = std::vector<uint32_t>(64 * 64, 0);
    std::unordered_map<int, std::vector<Col>> buckets;
    // Cells in blocks of 16 x 16 (one per map chunk), allocated when first touched: the levels
    // (n == 0xFF: not worked out yet) and, per plan, the search's node list head.
    struct CellLv { uint8_t n = 0xFF; Level lv[kMaxLevels]; };
    struct Block { CellLv cell[256]; uint32_t stamp[256] = {}; int32_t head[256] = {}; };
    std::vector<std::unique_ptr<Block>> blocks = std::vector<std::unique_ptr<Block>>(64 * 64);
    int cells_computed = 0;
    Block& block(int cx, int cy) {
        auto& b = blocks[size_t((cy >> 4) * 64 + (cx >> 4))];
        if (!b) b = std::make_unique<Block>();
        return *b;
    }
    static int slot(int cx, int cy) { return (cy & 15) * 16 + (cx & 15); }
    struct Levels {
        int n = 0;
        Level lv[kMaxLevels];
        struct Span { const Level *b, *e; const Level* begin() const { return b; } const Level* end() const { return e; } };
        Span lv_span() const { return Span{lv, lv + n}; }
    };
    int rebuilds = 0, chunks_built = 0;
    std::vector<const Col*> scratch;

    explicit WalkGrid(const World& world) : w(world) {}

    void reset(unsigned m) {
        map = m;
        std::fill(built.begin(), built.end(), 0);
        std::fill(checked.begin(), checked.end(), 0);
        buckets.clear();
        for (auto& b : blocks) b.reset();
        cells_computed = 0;
        chunks_built = 0;
    }

    static bool skip_item(const World& w, const Item& it) {
        if (it.as_actor() || (it.ext_flags & kExtSprite) || (it.flags & kItemFastOnly)) return true;
        return false;
    }

    uint32_t signature(int c) const {
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
        const CurrentMap& m = const_cast<World&>(w).map();
        for (uint16_t id : m.chunk(c % 64, c / 64)) {
            const Item* it = w.item(id);
            if (!it || skip_item(w, *it)) continue;
            mix(id); mix(it->shape); mix(it->frame); mix(uint32_t(it->x)); mix(uint32_t(it->y)); mix(uint32_t(it->z));
            mix(it->flags & kItemFlipped);
        }
        return h;
    }

    void add_col(int c, unsigned shape, int x, int y, int z, bool flipped) {
        const ShapeInfo& si = w.data().typeflags.get(shape);
        uint16_t f = si.flags();
        if (si.is(ShapeInfo::kEditor)) return;
        if (!(f & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof | ShapeInfo::kDamaging | ShapeInfo::kSea))) return;
        Col col;
        col.x = x; col.y = y; col.z = z;
        si.footprint(flipped, col.xd, col.yd, col.zd);
        col.f = f;
        col.door = w.is_door_shape(shape);
        col.tele = false;
        col.chunk = int16_t(c);
        int bx0 = std::clamp((x - col.xd + 1) / kBucket, 0, kBuckets - 1), bx1 = std::clamp(x / kBucket, 0, kBuckets - 1);
        int by0 = std::clamp((y - col.yd + 1) / kBucket, 0, kBuckets - 1), by1 = std::clamp(y / kBucket, 0, kBuckets - 1);
        for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx) buckets[by * kBuckets + bx].push_back(col);
    }

    void build_chunk(int c) {
        const CurrentMap& m = const_cast<World&>(w).map();
        for (uint16_t id : m.chunk(c % 64, c / 64)) {
            const Item* it = w.item(id);
            if (!it || skip_item(w, *it)) continue;
            const ShapeInfo& si = w.data().typeflags.get(it->shape);
            if (si.family() == kFamGlobEgg) {
                // The glob's contents from the data (the live copies exist only in the fast area).
                int bx = it->x & ~0x1FF, by = it->y & ~0x1FF;
                for (const GlobItem& g : w.data().globs.get(it->quality))
                    add_col(c, g.shape, bx + 2 * g.x + 1, by + 2 * g.y + 1, it->z + g.z, false);
                continue;
            }
            if (si.family() == kFamTeleportEgg && it->frame != 1) {  // (frame 1: an arrival point)
                // Its trigger range: x / y ranges of 32-unit steps round it, z within 48.
                int rx = it->npcnum >> 4, ry = it->npcnum & 15;
                Col col{it->x + 32 * rx, it->y + 32 * ry, it->z, 64 * rx + 1, 64 * ry + 1, 0, 0, false, true, int16_t(c)};
                int bx0 = std::clamp((col.x - col.xd + 1) / kBucket, 0, kBuckets - 1), bx1 = std::clamp(col.x / kBucket, 0, kBuckets - 1);
                int by0 = std::clamp((col.y - col.yd + 1) / kBucket, 0, kBuckets - 1), by1 = std::clamp(col.y / kBucket, 0, kBuckets - 1);
                for (int by = by0; by <= by1; ++by)
                    for (int bx = bx0; bx <= bx1; ++bx) buckets[by * kBuckets + bx].push_back(col);
                continue;
            }
            add_col(c, it->shape, it->x, it->y, it->z, it->flags & kItemFlipped);
        }
        built[size_t(c)] = 1;
        ++chunks_built;
    }

    // Removes chunk c's colliders and the cells they may have shaped.
    void unbuild_chunk(int c) {
        int cx = c % 64, cy = c / 64;
        // Items extend toward lower coordinates (at most 480 units), so into the chunks below.
        int bx0 = std::max(0, (cx - 1) * 8), bx1 = std::min(kBuckets - 1, cx * 8 + 7);
        int by0 = std::max(0, (cy - 1) * 8), by1 = std::min(kBuckets - 1, cy * 8 + 7);
        for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx) {
                auto it = buckets.find(by * kBuckets + bx);
                if (it == buckets.end()) continue;
                auto& v = it->second;
                v.erase(std::remove_if(v.begin(), v.end(), [c](const Col& k) { return k.chunk == c; }), v.end());
            }
        // Cells whose box (up to 64 units round the cell) touches that area.
        int x0 = std::max(0, (cx - 1) * 512 - 64) / kCell, x1 = std::min(32767, cx * 512 + 511 + 64) / kCell;
        int y0 = std::max(0, (cy - 1) * 512 - 64) / kCell, y1 = std::min(32767, cy * 512 + 511 + 64) / kCell;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (const auto& b = blocks[size_t((y >> 4) * 64 + (x >> 4))]) b->cell[slot(x, y)].n = 0xFF;
        built[size_t(c)] = 0;
        ++rebuilds;
    }

    void ensure_chunk(int c) {
        if (checked[size_t(c)] == serial) return;
        checked[size_t(c)] = serial;
        uint32_t s = signature(c);
        if (built[size_t(c)] && s == sig[size_t(c)]) return;
        if (built[size_t(c)]) unbuild_chunk(c);
        sig[size_t(c)] = s;
        build_chunk(c);
    }

    // The colliders that may overlap the xy range (x0, x1] x (y0, y1] (deduplicated).
    const std::vector<const Col*>& gather(int x0, int y0, int x1, int y1) {
        scratch.clear();
        int bx0 = std::clamp((x0 + 1) / kBucket, 0, kBuckets - 1), bx1 = std::clamp(x1 / kBucket, 0, kBuckets - 1);
        int by0 = std::clamp((y0 + 1) / kBucket, 0, kBuckets - 1), by1 = std::clamp(y1 / kBucket, 0, kBuckets - 1);
        // The chunks whose items can reach into these buckets: their own and the next ones up.
        int c0x = bx0 / 8, c1x = std::min(63, bx1 / 8 + 1), c0y = by0 / 8, c1y = std::min(63, by1 / 8 + 1);
        for (int cy = c0y; cy <= c1y; ++cy)
            for (int cx = c0x; cx <= c1x; ++cx) ensure_chunk(cy * 64 + cx);
        for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx) {
                auto it = buckets.find(by * kBuckets + bx);
                if (it == buckets.end()) continue;
                for (const Col& k : it->second) scratch.push_back(&k);
            }
        if (bx1 > bx0 || by1 > by0) {
            std::sort(scratch.begin(), scratch.end());
            scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
        }
        return scratch;
    }

    // The Avatar's box with its foot centre at (fx, fy) and bottom z is free of blockers
    // (closed doors do not block; `door` is set when one overlaps).
    bool box_free(int fx, int fy, int z, bool* door = nullptr) {
        int X = fx + xd / 2, Y = fy + yd / 2;
        for (const Col* c : gather(X - xd, Y - yd, X, Y)) {
            if (!(c->f & (ShapeInfo::kSolid | ShapeInfo::kDamaging))) continue;
            if (!overlap_xy(*c, X, Y, xd, yd) || !(z < c->z + c->zd && c->z < z + zd)) continue;
            if (c->door) { if (door) *door = true; continue; }
            return false;
        }
        return true;
    }

    // Something that burns (a campfire, lava) overlaps the Avatar's box there.
    bool box_hurts(int fx, int fy, int z) {
        int X = fx + xd / 2, Y = fy + yd / 2;
        for (const Col* c : gather(X - xd, Y - yd, X, Y))
            if ((c->f & ShapeInfo::kDamaging) && overlap_xy(*c, X, Y, xd, yd) && z <= c->z + c->zd + 8 && c->z < z + zd) return true;
        return false;
    }

    // The Avatar's box there is in a teleporter's trigger range (or within kTeleMargin of it).
    bool box_tele(int fx, int fy, int z) {
        int X = fx + xd / 2 + kTeleMargin, Y = fy + yd / 2 + kTeleMargin;
        for (const Col* c : gather(X - xd - 2 * kTeleMargin, Y - yd - 2 * kTeleMargin, X, Y))
            if (c->tele && overlap_xy(*c, X, Y, xd + 2 * kTeleMargin, yd + 2 * kTeleMargin) && c->z - 48 < z && z <= c->z + 48)
                return true;
        return false;
    }

    // The heights the Avatar can stand at in a cell (lowest first).
    Levels levels(int cx, int cy) {
        Levels out;
        if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells) return out;
        {
            CellLv& cl = block(cx, cy).cell[slot(cx, cy)];
            if (cl.n != 0xFF) {
                out.n = cl.n;
                std::copy(cl.lv, cl.lv + cl.n, out.lv);
                return out;
            }
        }
        {
            const int px = cx * kCell + kCell / 2, py = cy * kCell + kCell / 2;
            const int X = px + xd / 2, Y = py + yd / 2;
            std::vector<const Col*> cols = gather(X - xd, Y - yd, X, Y);
            int cand[48];
            int nc = 0;
            for (const Col* c : cols) {
                if (!(c->f & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof)) || c->door) continue;
                if (!overlap_xy(*c, X, Y, xd, yd)) continue;
                int top = c->z + c->zd;
                if (top < 0 || top > 255 + 64) continue;
                if (std::find(cand, cand + nc, top) == cand + nc && nc < 48) cand[nc++] = top;
            }
            std::sort(cand, cand + nc);
            for (int i = 0; i < nc && out.n < kMaxLevels; ++i) {
                const int z = cand[i];
                // Support (physics.md 2): land or roof under the foot centre within 8 below, and
                // the surface there is not water or something that burns.
                int land_top = -1000, surf_top = -1000;
                uint16_t surf_f = 0;
                bool blocked = false, door = false, tele = false;
                for (const Col* c : cols) {
                    if (c->tele) {
                        // (With a margin: a walk along the edge of the range must not drift into it.)
                        if (overlap_xy(*c, X + kTeleMargin, Y + kTeleMargin, xd + 2 * kTeleMargin, yd + 2 * kTeleMargin) &&
                            c->z - 48 < z && z <= c->z + 48)
                            tele = true;
                        continue;
                    }
                    bool under = px > c->x - c->xd && px <= c->x && py > c->y - c->yd && py <= c->y;
                    int top = c->z + c->zd;
                    if (under && (c->f & (ShapeInfo::kLand | ShapeInfo::kRoof)) && top <= z && top > land_top) land_top = top;
                    if (under && top <= z + 8 && !c->door &&
                        (top > surf_top || (top == surf_top && (c->f & ShapeInfo::kLand) && !(surf_f & ShapeInfo::kLand)))) {
                        surf_top = top;
                        surf_f = c->f;
                    }
                    if ((c->f & (ShapeInfo::kSolid | ShapeInfo::kDamaging)) && overlap_xy(*c, X, Y, xd, yd) &&
                        z < c->z + c->zd && c->z < z + zd) {
                        if (c->door) door = true;
                        else blocked = true;
                    }
                }
                if (blocked || land_top + 8 < z) continue;
                if ((surf_f & ShapeInfo::kDamaging) || ((surf_f & ShapeInfo::kSea) && !(surf_f & ShapeInfo::kLand))) continue;
                if (out.n > 0 && z - out.lv[out.n - 1].z < 4) { out.lv[out.n - 1].z = int16_t(z); continue; }
                out.lv[out.n++] = Level{int16_t(z), uint8_t((door ? RoutePoint::kDoor : 0) | (tele ? kLvTele : 0))};
            }
        }
        CellLv& cl = block(cx, cy).cell[slot(cx, cy)];
        cl.n = uint8_t(out.n);
        std::copy(out.lv, out.lv + out.n, cl.lv);
        ++cells_computed;
        return out;
    }
};

// ---------------------------------------------------------------------------------------------
// One plan in progress

struct RouteGuide::Impl {
    World& w;
    WalkGrid grid;
    explicit Impl(World& world) : w(world), grid(world) {}

    enum Phase { kIdle, kStart, kCoarse, kRefine } phase = kIdle;
    int slice_nodes = 0, slice_used = 0;  // a fixed amount of work per slice instead of time (tests)
    bool out_of_time(Clock::time_point deadline) const {
        return slice_nodes > 0 ? slice_used >= slice_nodes : Clock::now() >= deadline;
    }
    Route out;
    double cpu_ms = 0;
    int frames = 0;

    // A* nodes. A node reached by a teleporter (flags kTeleport) keeps the cell where its
    // trigger range was entered (jcx, jcy, jz; jflags: how that cell was reached).
    struct Node {
        int cx, cy; int16_t z; uint8_t flags; int parent; float g; int next;
        int jcx = 0, jcy = 0; int16_t jz = 0; uint8_t jflags = 0;
    };
    std::vector<Node> nodes;
    std::vector<std::pair<float, int>> open;  // min-heap on f
    int expanded = 0, best = -1;
    float best_h = 1e30f;
    int fallback = -1, fallback_at = 0, climbs_rejected = 0, climb_trials = 0;
    std::unordered_map<uint64_t, bool> climb_cache;  // climbs tried on the live world (per map)
    unsigned climb_map = 0xFFFF;
    int goal = -1;
    int sx = 0, sy = 0, sz = 0;
    float wt = 1.3f;
    int goal_z = 0;   // the height to arrive at: the walkable level at the target nearest its z

    // Refinement.
    std::unique_ptr<Pathfinder> pf;
    int refine_to = 0;
    ActorState refine_start;
    int refine_nodes = 0;


    // Teleporters inside the map (RouteGuide::set_links): the trigger range (as a Col in the
    // Avatar's max-corner coordinates, like the grid's teleporter boxes), the arrival foot
    // point, and for the heuristic the source's foot point, its reach and the estimated cost
    // from its arrival on to the target (through further teleporters if that is shorter).
    struct Jump { Col box; int fx, fy, fz; float sx, sy, reach; float H; };
    std::vector<RouteLink> link_src;
    std::vector<Jump> jumps;

    float h(int cx, int cy, int z) const {
        const float px = float(cx * kCell + kCell / 2), py = float(cy * kCell + kCell / 2);
        float dx = px - float(out.tx), dy = py - float(out.ty);
        float best = std::sqrt(dx * dx + dy * dy) + 0.25f * float(std::abs(z - goal_z));
        for (const Jump& j : jumps) {
            float d = std::max(0.0f, std::hypot(px - j.sx, py - j.sy) - j.reach) + j.H;
            if (d < best) best = d;
        }
        return best;
    }
    // The teleporter whose trigger range the Avatar's box is in at that cell and height (-1 none).
    int jump_at(int cx, int cy, int z) const {
        const int X = cx * kCell + kCell / 2 + grid.xd / 2, Y = cy * kCell + kCell / 2 + grid.yd / 2;
        for (size_t i = 0; i < jumps.size(); ++i) {
            const Col& c = jumps[i].box;
            if (overlap_xy(c, X, Y, grid.xd, grid.yd) && c.z - 48 < z && z <= c.z + 48) return int(i);
        }
        return -1;
    }
    void setup_jumps() {
        jumps.clear();
        for (const RouteLink& l : link_src) {
            int rx = l.rx, ry = l.ry;
            if (rx < 0 || ry < 0) {
                // The egg's own range (items.md 8.1: npcnum, x range high nibble, y range low).
                rx = ry = 1;
                const CurrentMap& m = w.map();
                const int ccx = std::clamp(l.x / 512, 0, 63), ccy = std::clamp(l.y / 512, 0, 63);
                for (uint16_t id : m.chunk(ccx, ccy)) {
                    const Item* it = w.item(id);
                    if (!it || std::abs(it->x - l.x) > 1 || std::abs(it->y - l.y) > 1 || std::abs(it->z - l.z) > 1) continue;
                    unsigned fam = w.data().typeflags.get(it->shape).family();
                    if (fam != kFamTeleportEgg && fam != kFamUsecodeEgg) continue;
                    rx = it->npcnum >> 4;
                    ry = it->npcnum & 15;
                    break;
                }
            }
            Jump j;
            j.box = Col{l.x + 32 * rx, l.y + 32 * ry, l.z, 64 * rx + 1, 64 * ry + 1, 0, 0, false, true, -1};
            j.fx = l.to_x - grid.xd / 2;
            j.fy = l.to_y - grid.yd / 2;
            j.fz = l.to_z;
            j.sx = float(l.x - grid.xd / 2);
            j.sy = float(l.y - grid.yd / 2);
            j.reach = float(32 * std::max(rx, ry) + 32);
            j.H = kJumpCost + std::hypot(float(j.fx - out.tx), float(j.fy - out.ty)) + 0.25f * float(std::abs(j.fz - goal_z));
            jumps.push_back(j);
        }
        // The cost on from each arrival: straight to the target or through other teleporters.
        for (size_t pass = 0; pass < jumps.size(); ++pass)
            for (Jump& a : jumps)
                for (const Jump& b : jumps) {
                    float d = kJumpCost + std::max(0.0f, std::hypot(float(a.fx) - b.sx, float(a.fy) - b.sy) - b.reach) + b.H;
                    if (d < a.H) a.H = d;
                }
    }
    // Goes through teleporter `j` entered at cell (jcx, jcy) height jz: a node at its arrival.
    void take_jump(int j, int jcx, int jcy, int jz, uint8_t jflags, int parent, float g) {
        const Jump& J = jumps[size_t(j)];
        const int acx = std::clamp(J.fx / kCell, 0, kCells - 1), acy = std::clamp(J.fy / kCell, 0, kCells - 1);
        int bx = -1, by = -1, bz = 0, bc = 1 << 30;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto lvs = grid.levels(acx + dx, acy + dy);  // (a temporary: keep it alive through the loop)
                for (const Level& l : lvs.lv_span()) {
                    int dz = std::abs(l.z - J.fz);
                    if (dz > 24) continue;
                    int c = dz + 16 * (std::abs(dx) + std::abs(dy));
                    if (c < bc) { bc = c; bx = acx + dx; by = acy + dy; bz = l.z; }
                }
            }
        if (bx < 0) return;
        int k = add(bx, by, bz, RoutePoint::kTeleport, parent, g + kJumpCost);
        if (k >= 0) {
            Node& n = nodes[size_t(k)];
            n.jcx = jcx; n.jcy = jcy; n.jz = int16_t(jz); n.jflags = jflags;
        }
    }
    bool at_goal(int cx, int cy, int z) const {
        int dx = cx * kCell + kCell / 2 - out.tx, dy = cy * kCell + kCell / 2 - out.ty;
        return (dx * dx + dy * dy <= kGoalR * kGoalR && std::abs(z - goal_z) <= 16) || in_goal_egg(cx, cy, z);
    }
    // The target is a teleporter (a link): anywhere in its trigger range is there.
    std::vector<Col> goal_eggs;
    bool in_goal_egg(int cx, int cy, int z) const {
        const int X = cx * kCell + kCell / 2 + grid.xd / 2, Y = cy * kCell + kCell / 2 + grid.yd / 2;
        for (const Col& c : goal_eggs)
            if (overlap_xy(c, X, Y, grid.xd, grid.yd) && c.z - 48 < z && z <= c.z + 48) return true;
        return false;
    }
    // Beside the target but on another level (a waypoint egg above the floor, an NPC upstairs
    // seen from below): used when the right level cannot be reached.
    bool near_goal(int cx, int cy, int z) const {
        int dx = cx * kCell + kCell / 2 - out.tx, dy = cy * kCell + kCell / 2 - out.ty;
        return dx * dx + dy * dy <= kGoalR * kGoalR && std::abs(z - out.tz) <= 64;
    }

    // A climb the Avatar can really do (his climb actions tried against the world, after
    // walking flush to the ledge). Only where the world is live (the fast area: glob contents
    // exist there); elsewhere the grid's rule stands until a later plan gets closer.
    bool climb_ok(int pxa, int pya, int za, int pxb, int pyb, int zb) {
        CurrentMap& m = w.map();
        if (!m.is_fast(std::clamp(pxa / 512, 0, 63), std::clamp(pya / 512, 0, 63)) ||
            !m.is_fast(std::clamp(pxb / 512, 0, 63), std::clamp(pyb / 512, 0, 63)))
            return true;
        const Actor* a = w.avatar();
        if (!a) return true;
        const int dir = direction_to(pxb - pxa, pyb - pya);
        const uint64_t ck = (uint64_t(uint32_t((pya / kCell) * kCells + pxa / kCell)) << 24) | (uint64_t(dir) << 20) |
                            (uint64_t(uint16_t(za + 512)) << 10) | uint64_t(uint16_t(zb + 512) & 0x3FF);
        if (climb_map != grid.map) { climb_cache.clear(); climb_map = grid.map; }
        auto cached = climb_cache.find(ck);
        if (cached != climb_cache.end()) return cached->second;
        const uint16_t mf = w.shape_flags(*a);
        int X = pxa + grid.xd / 2, Y = pya + grid.yd / 2;
        for (int k = 4; k <= 48; k += 4) {
            int nx = pxa + grid.xd / 2 + dir_x(dir) * k, ny = pya + grid.yd / 2 + dir_y(dir) * k;
            if (!w.can_exist(nx, ny, za, grid.xd, grid.yd, grid.zd, mf, 1).valid) break;
            X = nx;
            Y = ny;
        }
        ActorState st;
        st.x = X; st.y = Y; st.z = za;
        st.dir = uint8_t(dir);
        st.last_anim_dir = uint16_t(dir);
        ++climb_trials;
        bool ok = false;
        for (unsigned k = 0; k < 8 && !ok; ++k) {
            if (std::abs(16 + 8 * int(k) - (zb - za)) > 8) continue;  // the climbs of about this height
            AnimTracker t(w);
            ok = t.init(1, 19 + k, dir, &st) && t.run_all(true) == AnimTracker::kOk && std::abs(t.z() - zb) <= 8;
        }
        if (!ok) ++climbs_rejected;
        climb_cache[ck] = ok;
        return ok;
    }

    int add(int cx, int cy, int z, uint8_t flags, int parent, float g) {
        WalkGrid::Block& b = grid.block(cx, cy);
        const int sl = WalkGrid::slot(cx, cy);
        if (b.stamp[sl] != grid.serial) { b.stamp[sl] = grid.serial; b.head[sl] = -1; }
        for (int k = b.head[sl]; k >= 0; k = nodes[size_t(k)].next) {
            Node& n = nodes[size_t(k)];
            if (n.z != z) continue;
            if (g >= n.g) return -1;
            n.g = g; n.parent = parent; n.flags = flags;
            n.jcx = n.jcy = 0; n.jz = 0; n.jflags = 0;
            open.push_back({g + wt * h(cx, cy, z), k});
            std::push_heap(open.begin(), open.end(), std::greater<>());
            return k;
        }
        int i = int(nodes.size());
        nodes.push_back(Node{cx, cy, int16_t(z), flags, parent, g, b.head[sl], 0, 0, 0, 0});
        b.head[sl] = i;
        open.push_back({g + wt * h(cx, cy, z), i});
        std::push_heap(open.begin(), open.end(), std::greater<>());
        return i;
    }

    // Starts the coarse search from the Avatar's foot point.
    bool begin(unsigned map, int ax, int ay, int az, int tx, int ty, int tz) {
        out = Route{};
        out.map = map; out.tx = tx; out.ty = ty; out.tz = tz;
        nodes.clear(); open.clear();
        if (nodes.capacity() < 65536) { nodes.reserve(65536); open.reserve(131072); }
        expanded = 0; best = -1; best_h = 1e30f; goal = -1;
        fallback = -1; fallback_at = 0; climbs_rejected = 0;
        cpu_ms = 0; frames = 0;
        pf.reset();
        refine_nodes = 0;
        sx = ax; sy = ay; sz = az;
        jumps.clear();
        wt = 1.3f;  // (set by start() from the heuristic)
        if (grid.map != map) grid.reset(map);
        ++grid.serial;
        if (const Actor* a = w.avatar()) w.footprint(*a, grid.xd, grid.yd, grid.zd);
        phase = kStart;
        return true;
    }

    // The first slice: the level to arrive at and the start node.
    bool start() {
        const int tx = out.tx, ty = out.ty, tz = out.tz, ax = sx, ay = sy, az = sz;
        goal_eggs.clear();
        for (const Col* c : grid.gather(tx - 1, ty - 1, tx, ty))
            if (c->tele && overlap_xy(*c, tx, ty, 1, 1) && std::abs(tz - c->z) <= 48) goal_eggs.push_back(*c);
        // The level to arrive at: a target point may float (an egg) or stand in something (an
        // NPC on a chair): the walkable level round it nearest its z, one below preferred.
        goal_z = tz;
        {
            int tcx = std::clamp(tx / kCell, 0, kCells - 1), tcy = std::clamp(ty / kCell, 0, kCells - 1);
            int bestc = 1 << 30;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto lvs = grid.levels(tcx + dx, tcy + dy);  // (a temporary: keep it alive through the loop)
                    for (const Level& l : lvs.lv_span()) {
                        int c = l.z <= tz + 8 ? tz - l.z : 2 * (l.z - tz);
                        c = std::abs(c) + 4 * (std::abs(dx) + std::abs(dy));
                        if (c < bestc) { bestc = c; goal_z = l.z; }
                    }
                }
        }
        // The teleporters inside the map, and the heuristic's weight: more for long searches
        // (fewer nodes, a little less direct).
        setup_jumps();
        {
            int scx = std::clamp(ax / kCell, 0, kCells - 1), scy = std::clamp(ay / kCell, 0, kCells - 1);
            wt = 1.3f + std::clamp((h(scx, scy, az) - 1500.0f) / 5000.0f, 0.0f, 1.2f);
        }
        // The start cell: the level nearest his feet, here or close by.
        int ccx = std::clamp(ax / kCell, 0, kCells - 1), ccy = std::clamp(ay / kCell, 0, kCells - 1);
        // (Then, standing on something the grid does not count, a level he can step down to.)
        for (int pass = 0; pass < 2 && nodes.empty(); ++pass)
            for (int r = 0; r <= 3 && nodes.empty(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                        const auto lvs = grid.levels(ccx + dx, ccy + dy);  // (a temporary: keep it alive through the loop)
                        for (const Level& l : lvs.lv_span())
                            if (pass == 0 ? std::abs(l.z - az) <= (r == 0 ? 8 : 16) : (l.z <= az && az - l.z <= kDropMax)) {
                                add(ccx + dx, ccy + dy, l.z, 0, -1, float(r * kCell));
                                break;
                            }
                    }
        if (nodes.empty()) {
            out.status = Route::kFailed;
            out.reason = "the Avatar is not on walkable ground";
            return false;
        }
        phase = kCoarse;
        return true;
    }

    // Expands up to the deadline. True when the coarse search is over.
    bool coarse(Clock::time_point deadline) {
        static const int D[8][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
        int n = 0;
        while (!open.empty()) {
            if (n++ > 0 && out_of_time(deadline)) return false;
            ++slice_used;
            std::pop_heap(open.begin(), open.end(), std::greater<>());
            auto [f, ni] = open.back();
            open.pop_back();
            const Node cur = nodes[size_t(ni)];
            if (f > cur.g + wt * h(cur.cx, cur.cy, cur.z) + 0.01f) continue;  // a stale entry
            float hh = h(cur.cx, cur.cy, cur.z);
            if (hh < best_h) { best_h = hh; best = ni; }
            if (at_goal(cur.cx, cur.cy, cur.z)) { goal = ni; return true; }
            if (fallback < 0 && near_goal(cur.cx, cur.cy, cur.z)) { fallback = ni; fallback_at = expanded; }
            if (++expanded > kMaxExpand) return true;
            if (fallback >= 0 && expanded > 4 * fallback_at + 60000) return true;  // the right level is not near
            const int pxa = cur.cx * kCell + kCell / 2, pya = cur.cy * kCell + kCell / 2;
            const int cur_jump = jumps.empty() ? -1 : jump_at(cur.cx, cur.cy, cur.z);
            for (int d = 0; d < 8; ++d) {
                int nx = cur.cx + D[d][0], ny = cur.cy + D[d][1];
                if (nx < 0 || ny < 0 || nx >= kCells || ny >= kCells) continue;
                const WalkGrid::Levels lvs = grid.levels(nx, ny);  // a copy (box tests may rebuild cells)
                const auto lv = lvs.lv_span();
                const bool diag = d >= 4;
                const float step = diag ? 45.25f : 32.0f;
                for (const Level& l : lv) {
                    // A teleporter inside the map is a way on (not the one he stands in: he has
                    // just arrived there and it does not fire); others are kept out of the way.
                    int jn = jumps.empty() ? -1 : jump_at(nx, ny, l.z);
                    if (jn >= 0 && (jn == cur_jump || at_goal(nx, ny, l.z))) jn = -1;
                    if ((l.flags & kLvTele) && jn < 0 && !at_goal(nx, ny, l.z)) continue;  // not into a teleporter on the way
                    int dz = l.z - cur.z;
                    if (dz > kClimbMax || dz < -kDropMax) continue;
                    const int pxb = nx * kCell + kCell / 2, pyb = ny * kCell + kCell / 2;
                    const int zmax = std::max<int>(l.z, cur.z);
                    bool door = (l.flags & RoutePoint::kDoor) != 0;  // (kLvTele is not a RoutePoint flag)
                    uint8_t fl = 0;
                    float cost = step;
                    if (dz > kStepUp) {
                        // A climb: room above him at the new height, where he stands.
                        // Room all the way up where he stands (not through a ceiling).
                        bool room = true;
                        for (int zz = cur.z + 16; zz < l.z && room; zz += 16) room = grid.box_free(pxa, pya, zz);
                        if (!room || !grid.box_free(pxa, pya, l.z, &door)) continue;
                        if (!climb_ok(pxa, pya, cur.z, pxb, pyb, l.z)) continue;
                        fl |= RoutePoint::kClimb;
                        cost += 256 + float(dz);
                    } else if (dz < -kStepUp) {
                        // A drop: he walks out over the edge at his height, then falls.
                        // ... and nothing to land on on the way down (not through a floor).
                        bool room = grid.box_free(pxb, pyb, cur.z, &door);
                        for (int zz = cur.z - 16; zz > l.z && room; zz -= 16) room = grid.box_free(pxb, pyb, zz);
                        if (!room) continue;
                        fl |= RoutePoint::kDrop;
                        cost += 24 + float(-dz) / 2;
                    } else if (dz != 0) {
                        fl |= RoutePoint::kStairs;
                        cost += float(std::abs(dz)) / 2;
                    }
                    // The way between the two cells (thin walls), and the corners of a diagonal.
                    if (!grid.box_free((pxa + pxb) / 2, (pya + pyb) / 2, zmax, &door)) continue;
                    if (diag && (!grid.box_free(pxb, pya, zmax + 8, &door) || !grid.box_free(pxa, pyb, zmax + 8, &door)))
                        continue;
                    if (door) { fl |= RoutePoint::kDoor; cost += 48; }
                    if (jn >= 0) take_jump(jn, nx, ny, l.z, fl, ni, cur.g + cost);
                    else add(nx, ny, l.z, fl, ni, cur.g + cost);
                }
            }
        }
        return true;
    }

    // Turns the coarse result into route points.
    void finish_coarse() {
        int end = goal;
        if (end < 0 && fallback >= 0) {
            end = fallback;
            int dz = out.tz - nodes[size_t(end)].z;
            out.reason = std::string("the spot is ") + std::to_string(std::abs(dz)) + (dz > 0 ? " above" : " below");
        }
        if (end < 0 && best >= 0) {
            const Node& b = nodes[size_t(best)];
            int dx = b.cx * kCell + kCell / 2 - out.tx, dy = b.cy * kCell + kCell / 2 - out.ty;
            // Close enough to count (the target stands in something, on a table...).
            if (dx * dx + dy * dy <= 112 * 112 && std::abs(b.z - goal_z) <= 40) end = best;
        }
        if (end < 0) {
            out.status = Route::kFailed;
            out.reason = expanded > kMaxExpand ? "too far to search" : "blocked (a locked door, a key or a jump?)";
            out.pts.clear();
            return;
        }
        out.status = goal >= 0 ? Route::kFound : Route::kNear;
        std::vector<RoutePoint> pts;
        for (int i = end; i >= 0; i = nodes[size_t(i)].parent) {
            const Node& n = nodes[size_t(i)];
            pts.push_back(RoutePoint{n.cx * kCell + kCell / 2, n.cy * kCell + kCell / 2, n.z, n.flags});
            // Through a teleporter: the cell in its trigger range comes before the arrival.
            if (n.flags & RoutePoint::kTeleport)
                pts.push_back(RoutePoint{n.jcx * kCell + kCell / 2, n.jcy * kCell + kCell / 2, n.jz, n.jflags});
        }
        std::reverse(pts.begin(), pts.end());
        // Straight runs on one level need only their ends.
        auto sgn = [](int v) { return (v > 0) - (v < 0); };
        std::vector<RoutePoint> s;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i > 0 && i + 1 < pts.size()) {
                const RoutePoint &a = pts[i - 1], &m = pts[i], &c = pts[i + 1];
                bool same = !m.flags && !c.flags && a.z == m.z && m.z == c.z && sgn(m.x - a.x) == sgn(c.x - m.x) &&
                            sgn(m.y - a.y) == sgn(c.y - m.y);
                if (same) continue;
            }
            s.push_back(pts[i]);
        }
        if (!s.empty()) { s.front().x = sx; s.front().y = sy; s.front().z = sz; s.front().flags = 0; }
        if (out.status == Route::kFound) {
            RoutePoint t{out.tx, out.ty, s.empty() ? out.tz : s.back().z, RoutePoint::kExact};
            if (s.empty() || std::abs(s.back().x - t.x) + std::abs(s.back().y - t.y) > 4) s.push_back(t);
        }
        out.pts = std::move(s);
    }

    // The near stretch with the real pathfinder: up to kRefineDist along, stopping before the
    // first climb, drop or door (which the walk planner cannot do).
    bool begin_refine() {
        const Actor* a = w.avatar();
        if (!a || out.pts.size() < 2 || a->is(Actor::kInCombat)) return false;
        int dist = 0;
        int to = 0;
        for (size_t i = 1; i < out.pts.size(); ++i) {
            if (out.pts[i].flags & (RoutePoint::kClimb | RoutePoint::kDrop | RoutePoint::kDoor | RoutePoint::kTeleport)) break;
            dist += int(std::hypot(out.pts[i].x - out.pts[i - 1].x, out.pts[i].y - out.pts[i - 1].y));
            to = int(i);
            if (dist >= kRefineDist) break;
        }
        if (to <= 0) return false;
        // Cut a long last segment at kRefineDist (the planner is local).
        if (dist > kRefineDist + 32) {
            RoutePoint& p = out.pts[size_t(to)];
            const RoutePoint& q = out.pts[size_t(to - 1)];
            double seg = std::hypot(p.x - q.x, p.y - q.y), back = dist - kRefineDist;
            if (seg > back + 16) {
                RoutePoint m = p;
                m.x = p.x - int((p.x - q.x) * back / seg);
                m.y = p.y - int((p.y - q.y) * back / seg);
                m.flags = 0;
                out.pts.insert(out.pts.begin() + to, m);
            }
        }
        refine_to = to;
        pf = std::make_unique<Pathfinder>(w);
        pf->set_actor(1);
        const RoutePoint& g = out.pts[size_t(to)];
        pf->set_target_point(g.x + grid.xd / 2, g.y + grid.yd / 2, g.z);
        refine_start.x = a->x; refine_start.y = a->y; refine_start.z = a->z;
        refine_start.dir = a->dir;
        refine_start.last_anim = a->last_anim;
        refine_start.last_anim_dir = a->last_anim_dir;
        refine_start.first_step = a->is(Actor::kFirstStep);
        refine_start.flipped = a->flags & kItemFlipped;
        refine_start.in_combat = false;
        pf->plan_begin();
        phase = kRefine;
        return true;
    }

    // True when the refinement is over (used or not).
    bool refine(Clock::time_point deadline) {
        std::vector<PathStep> steps;
        int r = 0;
        do {
            r = pf->plan_step(1, steps);
            slice_used += 4;  // (a pathfinder node tries eight walks)
        } while (r == 0 && !out_of_time(deadline));
        if (r == 0) return false;
        refine_nodes = int(pf->expanded());
        if (r < 0) { pf.reset(); return true; }
        // Play the steps from the start state to get the points.
        std::vector<RoutePoint> pts;
        pts.push_back(RoutePoint{refine_start.x - grid.xd / 2, refine_start.y - grid.yd / 2, refine_start.z, RoutePoint::kExact});
        ActorState st = refine_start;
        AnimTracker tr(w);
        for (const PathStep& s : steps) {
            if (!tr.init(1, s.action, s.dir, &st, s.steps)) break;
            while (tr.step()) {}
            st = tr.state();
            RoutePoint p{st.x - grid.xd / 2, st.y - grid.yd / 2, st.z, RoutePoint::kExact};
            if (std::abs(p.z - pts.back().z) >= 8) p.flags |= RoutePoint::kStairs;
            pts.push_back(p);
        }
        // The walk planner does not mind fire; the guide does (the coarse way goes round it).
        // Nor does it mind teleporters; a walk into one (not the goal's) is not used either.
        // (Its end may be in the teleporter the route takes next: the last steps into it.)
        const bool into_jump = size_t(refine_to) + 1 < out.pts.size() && (out.pts[size_t(refine_to) + 1].flags & RoutePoint::kTeleport);
        for (size_t i = 0; i < pts.size(); ++i) {
            const RoutePoint& q = pts[i];
            if (grid.box_hurts(q.x, q.y, q.z)) { pf.reset(); return true; }
            if (i > 0 && !(into_jump && i + 3 >= pts.size()) && grid.box_tele(q.x, q.y, q.z) && !grid.box_tele(pts[0].x, pts[0].y, pts[0].z) &&
                !in_goal_egg(std::clamp(q.x / kCell, 0, kCells - 1), std::clamp(q.y / kCell, 0, kCells - 1), q.z)) {
                pf.reset();
                return true;
            }
        }
        int n = int(pts.size());
        pts.insert(pts.end(), out.pts.begin() + refine_to + 1, out.pts.end());
        out.pts = std::move(pts);
        out.refined = n;
        pf.reset();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------

RouteGuide::RouteGuide(World& w) : p_(std::make_unique<Impl>(w)), w_(w) {}
RouteGuide::~RouteGuide() = default;

void RouteGuide::set_target(unsigned map, int x, int y, int z) {
    bool moved = !have_target_ || map != tmap_ || std::abs(x - tx_) > 32 || std::abs(y - ty_) > 32 || std::abs(z - tz_) > 16;
    have_target_ = true;
    tmap_ = map; tx_ = x; ty_ = y; tz_ = z;
    if (moved) { ++stats.replans_target; replan(); }
}

void RouteGuide::set_links(unsigned map, const std::vector<RouteLink>& links) {
    if (map == links_map_ && links == links_) return;
    links_map_ = map;
    links_ = links;
    if (have_target_) replan();
}

void RouteGuide::clear() {
    have_target_ = false;
    p_->phase = Impl::kIdle;
    p_->pf.reset();
    route_ = Route{};
}

bool RouteGuide::busy() const { return p_->phase != Impl::kIdle; }

bool RouteGuide::current() const {
    return have_target_ && route_.status != Route::kNone && route_.map == tmap_ && std::abs(route_.tx - tx_) <= 32 &&
           std::abs(route_.ty - ty_) <= 32 && route_.map == w_.map_number_now();
}

void RouteGuide::invalidate_grid() { p_->grid.reset(0xFFFF); }

void RouteGuide::replan() {
    const Actor* a = w_.avatar();
    if (!have_target_ || !a) return;
    frames_since_plan_ = 0;
    ++stats.plans;
    int xd, yd, zd;
    w_.footprint(*a, xd, yd, zd);
    p_->link_src = links_map_ == w_.map_number_now() ? links_ : std::vector<RouteLink>{};
    if (!p_->begin(w_.map_number_now(), a->x - xd / 2, a->y - yd / 2, a->z, tx_, ty_, tz_)) {
        finish(std::move(p_->out));
        return;
    }
}

void RouteGuide::finish(Route&& r) {
    Impl& p = *p_;
    p.phase = Impl::kIdle;
    r.length = 0;
    r.jumps = 0;
    for (size_t i = 1; i < r.pts.size(); ++i) {
        if (r.pts[i].flags & RoutePoint::kTeleport) { ++r.jumps; continue; }
        r.length += int(std::hypot(r.pts[i].x - r.pts[i - 1].x, r.pts[i].y - r.pts[i - 1].y));
    }
    r.serial = route_.serial + 1;
    // Logged when the target or the outcome changes (not for every re-plan).
    bool news = r.status != route_.status || r.map != route_.map || std::abs(r.tx - route_.tx) > 32 || std::abs(r.ty - route_.ty) > 32;
    route_ = std::move(r);
    stats.last_plan_ms = p.cpu_ms;
    stats.last_plan_frames = p.frames;
    stats.last_expanded = p.expanded;
    stats.last_refine_nodes = p.refine_nodes;
    stats.last_refined = route_.refined > 0;
    stats.cells = p.grid.cells_computed;
    stats.chunks = p.grid.chunks_built;
    stats.chunk_rebuilds = p.grid.rebuilds;
    if (news)
        PG_LOG("route: map %u to %d,%d,%d: %s, %zu points, %d long, %d teleporters, %d nodes, %.1f ms in %d slices (longest so far %.2f ms)%s%s",
               route_.map, route_.tx, route_.ty, route_.tz,
               route_.status == Route::kFound ? "found" : route_.status == Route::kNear ? "near" : "none", route_.pts.size(),
               route_.length, route_.jumps, p.expanded, p.cpu_ms, p.frames, stats.slice_ms_max, route_.reason.empty() ? "" : ": ",
               route_.reason.c_str());
    stats.climb_trials = p.climb_trials;
    stats.climbs_rejected = p.climbs_rejected;
    if (p.best >= 0 && size_t(p.best) < p.nodes.size()) {
        const Impl::Node& b = p.nodes[size_t(p.best)];
        stats.closest[0] = b.cx * kCell + kCell / 2;
        stats.closest[1] = b.cy * kCell + kCell / 2;
        stats.closest[2] = b.z;
    }
}

void RouteGuide::update(double budget_ms) {
    Impl& p = *p_;
    const Actor* a = w_.avatar();
    if (!have_target_ || !a) return;
    if (w_.map_number_now() != tmap_) return;  // the guide has not caught up with a map change
    if (p.phase != Impl::kIdle && p.out.map != w_.map_number_now()) p.phase = Impl::kIdle;
    ++frames_since_plan_;
    auto t0 = Clock::now();
    auto deadline = t0 + std::chrono::microseconds(int64_t(budget_ms * 1000));
    p.slice_nodes = slice_nodes;
    p.slice_used = 0;
    if (p.phase == Impl::kIdle && !w_.stasis) {
        // The re-plan rules (not while a scene holds him).
        int xd, yd, zd;
        w_.footprint(*a, xd, yd, zd);
        if (route_.map != w_.map_number_now() || route_.status == Route::kNone) {
            replan();
        } else if (route_.usable() && !a->gravity_pid) {
            Where wh = where(a->x - xd / 2, a->y - yd / 2, a->z);
            if (wh.off > kStray) { ++stats.replans_stray; replan(); }
            else if (frames_since_plan_ >= kReplanFrames && wh.left > 48) { ++stats.replans_timer; replan(); }
        } else if (frames_since_plan_ >= kReplanFrames) {
            ++stats.replans_timer;
            replan();
        }
    }
    if (p.phase == Impl::kIdle) return;
    if (p.phase == Impl::kStart && !p.start()) {
        p.cpu_ms += ms_since(t0);
        ++p.frames;
        ++stats.slices;
        stats.slice_ms_max = std::max(stats.slice_ms_max, ms_since(t0));
        finish(std::move(p.out));
        return;
    }
    if (p.phase == Impl::kCoarse && !p.out_of_time(deadline)) {
        if (p.coarse(deadline)) {
            p.finish_coarse();
            if (!(p.out.usable() && p.begin_refine())) {
                p.cpu_ms += ms_since(t0);
                ++p.frames;
                ++stats.slices;
                stats.slice_ms_max = std::max(stats.slice_ms_max, ms_since(t0));
                finish(std::move(p.out));
                return;
            }
        }
    }
    if (p.phase == Impl::kRefine && !p.out_of_time(deadline)) {
        if (p.refine(deadline)) {
            p.cpu_ms += ms_since(t0);
            ++p.frames;
            ++stats.slices;
            stats.slice_ms_max = std::max(stats.slice_ms_max, ms_since(t0));
            finish(std::move(p.out));
            return;
        }
    }
    double ms = ms_since(t0);
    p.cpu_ms += ms;
    ++p.frames;
    ++stats.slices;
    stats.slice_ms_max = std::max(stats.slice_ms_max, ms);
}

const Route& RouteGuide::plan_now(double slice_ms, int max_slices) {
    replan();
    for (int i = 0; i < max_slices && busy(); ++i) {
        --frames_since_plan_;  // these calls are not frames
        update(slice_ms);
    }
    return route_;
}

std::vector<int> RouteGuide::levels_at(int x, int y) {
    WalkGrid& g = p_->grid;
    if (g.map != w_.map_number_now()) g.reset(w_.map_number_now());
    if (const Actor* a = w_.avatar()) w_.footprint(*a, g.xd, g.yd, g.zd);
    std::vector<int> out;
    const auto lvs = g.levels(x / kCell, y / kCell);
    for (const Level& l : lvs.lv_span()) out.push_back(l.z);
    return out;
}

RouteGuide::Where RouteGuide::where(int x, int y, int z) const {
    Where best;
    const auto& pts = route_.pts;
    if (pts.empty()) return best;
    if (pts.size() == 1) {
        best.px = pts[0].x; best.py = pts[0].y; best.pz = pts[0].z;
        best.off = int(std::hypot(x - pts[0].x, y - pts[0].y));
        return best;
    }
    double bd = 1e18, boff = 0;
    int leg = 0;  // teleporters before the segment: a later leg counts as a little further away
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const RoutePoint &a = pts[i], &b = pts[i + 1];
        if (b.flags & RoutePoint::kTeleport) { ++leg; continue; }  // (not walked)
        double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
        double t = l2 > 0 ? std::clamp(((x - a.x) * vx + (y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
        double qx = a.x + vx * t, qy = a.y + vy * t, qz = a.z + (b.z - a.z) * t;
        // Height counts too (a route on the floor above is not where he is).
        double dz = std::max(0.0, std::abs(z - qz) - 24);
        double off = std::sqrt((qx - x) * (qx - x) + (qy - y) * (qy - y) + 4 * dz * dz);
        double d = off + 48.0 * leg;
        if (d < bd - 1e-6) {
            bd = d;
            boff = off;
            best.seg = i;
            best.px = int(qx); best.py = int(qy); best.pz = int(qz);
        }
    }
    if (bd > 1e17) {  // (only teleports: no walked segment)
        best.px = pts[0].x; best.py = pts[0].y; best.pz = pts[0].z;
        best.off = int(std::hypot(x - pts[0].x, y - pts[0].y));
        return best;
    }
    best.off = int(boff);
    // The walk to the end of this leg: the next teleporter's trigger cell, or the target.
    best.leg_end = pts.size() - 1;
    double left = std::hypot(pts[best.seg + 1].x - best.px, pts[best.seg + 1].y - best.py);
    for (size_t i = best.seg + 1; i < pts.size(); ++i) {
        if (i + 1 < pts.size() && (pts[i + 1].flags & RoutePoint::kTeleport)) { best.leg_end = i; best.jump = true; break; }
        if (i + 1 < pts.size()) left += std::hypot(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
    }
    best.left = int(left) + int(std::hypot(x - best.px, y - best.py));
    return best;
}

std::string RouteGuide::via(size_t seg) const {
    const auto& pts = route_.pts;
    std::vector<std::string> parts;
    auto add = [&](const std::string& s) {
        if (parts.size() < 2 && (parts.empty() || parts.back() != s)) parts.push_back(s);
    };
    int run_up = 0, run_down = 0;  // height walked up / down a stretch of steps
    for (size_t i = seg + 1; i < pts.size(); ++i) {
        const RoutePoint& p = pts[i];
        if (p.flags & RoutePoint::kTeleport) {  // the end of this leg
            if (parts.size() >= 2) parts.pop_back();
            parts.push_back("take the teleporter");
            break;
        }
        if (parts.size() >= 2) continue;
        int dz = p.z - pts[i - 1].z;
        if (p.flags & RoutePoint::kDoor) add("through the door (open it)");
        if (p.flags & RoutePoint::kClimb) { add("climb up the ledge (jump)"); run_up = run_down = 0; continue; }
        if (p.flags & RoutePoint::kDrop) { add("jump down"); run_up = run_down = 0; continue; }
        if (dz > 0) { run_up += dz; run_down = 0; }
        else if (dz < 0) { run_down -= dz; run_up = 0; }
        if (run_up >= 24) { add("up the stairs"); run_up = -10000; }
        if (run_down >= 24) { add("down the stairs"); run_down = -10000; }
    }
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : ", then ") + p;
    return s;
}

}  // namespace pg
