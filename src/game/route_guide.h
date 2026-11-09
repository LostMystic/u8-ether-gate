// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The walking route for the critical-path guide (debug mode; app/debug_tools.h): the way the
// Avatar can really walk from where he stands to the guide's target on the current map (or to
// the link on the way to another map), so the guide line follows streets, stairs and doors
// instead of pointing through walls.
//
// Two levels, because the real pathfinder (game/pathfinder.h) is local (200 nodes):
// - a coarse search over a walk grid of the current map: 32-unit cells, each with the heights
//   the Avatar's box can stand at there (the placement and support rules of physics.md 2 on
//   the map's items, globs expanded from the data so chunks outside the fast area count too),
//   joined by walks (up / down 16), climbs (up to 72, the Avatar's climb actions), drops (down
//   to 64) and doors (closed doors are passable for routing and marked). The grid is built
//   lazily per chunk while the search touches it and cached per map; a chunk whose items
//   changed (a door opened or closed, something moved) is rebuilt when a later plan reads it;
// - the near part (about 200 units, up to the first climb / drop / door) is then replaced by a
//   plan of the real pathfinder from the Avatar's exact state.
// Teleporters inside the map (set_links: the same-map links of data/map_links.json whose
// conditions hold) are edges of the coarse search: stepping into one's trigger range goes on
// from its arrival point, so a target that can only be reached through teleporters (Mythran's
// house on the Plateau) gets a route that leads to the first teleporter, then on from where
// it lands. Other teleporters (to other maps, or not usable now) are kept out of the way.
// Planning runs in slices (update(budget_ms) each frame), so it never stalls a frame; the last
// route is kept while a new one is planned. A re-plan starts when the target moves, the map
// changes, the Avatar strays more than kStray units from the route, or every kReplanFrames.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pg {

class World;

struct RoutePoint {
    // How the point is reached from the previous one.
    // kTeleport: the point is a teleporter's arrival point; the point before it is in the
    // teleporter's trigger range (the stretch between them is not walked).
    enum : uint8_t { kDoor = 1, kClimb = 2, kDrop = 4, kStairs = 8, kExact = 16, kTeleport = 32 };
    int x = 0, y = 0, z = 0;  // the Avatar's foot centre (the middle of his footprint, his z)
    uint8_t flags = 0;
};

// A teleporter inside the current map: its source point (the egg), its trigger range in
// 32-unit steps (-1: the egg's own, looked up on the map; none there: 1) and where it lands
// (the arrival egg: the Avatar's x / y / z, the max corner of his box, as for eggs).
struct RouteLink {
    int x = 0, y = 0, z = 0;
    int rx = -1, ry = -1;
    int to_x = 0, to_y = 0, to_z = 0;
    bool operator==(const RouteLink& o) const {
        return x == o.x && y == o.y && z == o.z && rx == o.rx && ry == o.ry && to_x == o.to_x && to_y == o.to_y && to_z == o.to_z;
    }
};

struct Route {
    enum Status { kNone, kFound, kNear, kFailed };
    Status status = kNone;
    std::string reason;          // kFailed: why (for the hint)
    unsigned map = 0;
    int tx = 0, ty = 0, tz = 0;  // the target it was planned for
    std::vector<RoutePoint> pts; // start .. target
    int refined = 0;             // the first `refined` points come from the real pathfinder
    int length = 0;              // world units walked along the points (not the teleports)
    int jumps = 0;               // teleporters taken on the way
    uint32_t serial = 0;         // changes with every new route
    bool usable() const { return status == kFound || status == kNear; }
};

class RouteGuide {
public:
    explicit RouteGuide(World& w);
    ~RouteGuide();

    // The target on the current map (re-plans when it moves more than 32 units).
    void set_target(unsigned map, int x, int y, int z);
    void clear();
    // The teleporters inside `map` the route may take (re-plans when they change).
    void set_links(unsigned map, const std::vector<RouteLink>& links);
    // Works on the plan for up to `budget_ms`, then checks the re-plan rules. Call every frame.
    void update(double budget_ms);
    void replan();           // starts a new plan (the current route stays until it is done)
    void invalidate_grid();  // drops the cached walk grid (tests; a map change does it too)
    bool busy() const;
    bool has_target() const { return have_target_; }
    const Route& route() const { return route_; }
    // The route was planned for the current target.
    bool current() const;

    // Where the Avatar is on the route: the segment (pts[seg] -> pts[seg + 1]) nearest to the
    // foot point (x, y, z), the point on it, the distance off it and the walking distance left
    // to the end of this leg: the next teleporter (`leg_end`: the index of the point in its
    // trigger range, then `jump` is set) or the target.
    struct Where { size_t seg = 0; int px = 0, py = 0, pz = 0; int off = 0; int left = 0; size_t leg_end = 0; bool jump = false; };
    Where where(int x, int y, int z) const;
    // "up the stairs", "through the door (open it)", "take the teleporter"... for the stretch
    // from segment `seg` to the end of its leg.
    std::string via(size_t seg) const;

    // Plans to completion in slices of `slice_ms` (tests). Returns the route.
    const Route& plan_now(double slice_ms = 2.0, int max_slices = 200000);

    // The walk grid's standing heights in the cell holding (x, y) of the current map (tests).
    std::vector<int> levels_at(int x, int y);

    // Tests: a fixed amount of work per update (coarse nodes) instead of the time budget, so a
    // run is the same every time. 0 = the time budget.
    int slice_nodes = 0;

    static constexpr int kStray = 64;          // re-plan when this far off the route
    static constexpr int kReplanFrames = 90;   // and every 3 s of updates
    static constexpr int kRefineDist = 192;    // the real pathfinder's stretch

    struct Stats {
        int plans = 0, replans_stray = 0, replans_timer = 0, replans_target = 0;
        int slices = 0;               // update() calls that did planning work
        double slice_ms_max = 0;      // the longest planning slice
        double last_plan_ms = 0;      // CPU time of the last finished plan (all its slices)
        int last_plan_frames = 0;     // updates it took
        int last_expanded = 0;        // coarse nodes expanded
        int last_refine_nodes = 0;    // the real pathfinder's expansions (0 = not used)
        bool last_refined = false;
        int cells = 0, chunks = 0, chunk_rebuilds = 0;
        int climb_trials = 0, climbs_rejected = 0;
        int closest[3] = {0, 0, 0};   // the searched spot closest to the target
    } stats;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    World& w_;
    Route route_;
    bool have_target_ = false;
    unsigned links_map_ = 0xFFFF;
    std::vector<RouteLink> links_;
    unsigned tmap_ = 0;
    int tx_ = 0, ty_ = 0, tz_ = 0;
    int frames_since_plan_ = 0;
    void finish(Route&& r);
};

}  // namespace pg
