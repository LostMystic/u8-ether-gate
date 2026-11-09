// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Easy ledges (PLAN-M6.md "Ledges"; the engine's own design, Responsive mode with the "Easy
// Ledges" option). Holding jump looks for a ledge the Avatar can get up from the world's
// geometry, no painting needed: the surfaces just ahead of him in a forward arc (then all
// round, closer), within a body length and a half, 16 to kMaxRise units up. Every candidate is
// checked with the engine's own animation trials (the climb, or the hang and pull-up, must
// work and end supported), so he never ends inside or on top of nothing. Roofs (the ROOF
// pieces, which carry LAND too) and spots right by one (eaves), the tops of wall pieces,
// water and damaging surfaces are never ledges. All the tunables are here.
#pragma once
#include <cstdint>

namespace pg {

class World;
class Actor;

namespace ledge {

// --- Tunables (world units; the Avatar's footprint is 64 x 64, his height 40).
constexpr int kReach = 96;         // how far ahead he walks to a ledge (1.5 body lengths)
constexpr int kAroundReach = 40;   // ... in the other directions (the all-round fallback)
constexpr int kSide = 32;          // sideways line-up (the movement-assist nudge), 8-unit steps
constexpr int kMaxRise = 72;       // the highest ledge top above where he stands (one storey, a full climb)
constexpr int kMaxDirect = 72;     // up to here a climb animation (16..72) does it in one go
constexpr int kMaxHang = 88;       // the hang point at most this far above his feet
constexpr int kHangForward = 16;   // the hang up to this much nearer the face (4-unit steps)
constexpr int kCatchBelow = 8;     // falling: a hang point up to this far below him still catches
constexpr int kCatchAbove = 12;    // ... and above him, plus this frame's fall
constexpr int kApproachSpeed = 16; // walking him to the spot: units per frame
constexpr int kHintFrames = 24;    // the ledge highlight shows this long (30 a second)
constexpr int kRoofGap = 32;       // a roof this near a landing spot (beside it, up to 48 above): a building
constexpr int kSearchTrials = 400; // at most this many animation trials per search (time)
constexpr int kCatchTrials = 40;   // ... per falling frame

enum : unsigned { kClimb16 = 19, kClimb40 = 22, kHang = 45, kJumpUp = 3 };

// The pull-ups tried from a hang, best first (climbs of 40, 32, 48, 24 and 56 units).
extern const unsigned kPulls[5];

// A climb animation, steered (the animation's targeted mode, as for targeted jumps) onto a
// standable spot on the top when its own travel would not end on one: a shallow step, a
// top that is not a multiple of 8 up.
struct Move {
    unsigned action = 0;
    bool targeted = false;
    int tx = 0, ty = 0, tz = 0;
};

struct Plan {
    int dir = 0;             // the direction he faces for it
    int sx = 0, sy = 0, sz = 0;  // where he stands to do it (box position)
    bool grab = false;       // false: a climb from the spot; true: jump, hang, pull up
    Move move;               // the climb (direct), or the pull-up from the hang
    int hx = 0, hy = 0, hz = 0;  // grab: the hang point
    int ex = 0, ey = 0, ez = 0;  // the ledge's edge in front of him (for the highlight)
    int rise = 0;            // the ledge top above his feet
    int travel = 0;          // units to the spot
};

// Looks for a ledge to get up, preferring `pref_dir` (the stick / facing): the forward arc
// (pref, then 45 degrees either side) up to kReach away with a sideways line-up up to kSide;
// if `around` and nothing is there, the other directions up to kAroundReach. Within a
// direction the nearest spot wins. `narrow` (he is walking or running): straight ahead only,
// lined up at most 16 to the side. False if nothing is in reach.
bool find(const World& w, const Actor& a, int pref_dir, bool around, Plan& out, bool narrow = false);

// Whether the top of `item` (the surface under a spot) is a ledge at all: not a roof piece
// (ROOF), not water or anything damaging, and not the top of a wall piece (at most 32
// thick and at least 32 high: standing on the buildings' walls is not the idea).
bool ledge_surface(const World& w, uint16_t item);

// Whether the actor can stand at box position (x, y, z) as on a ledge's top: fits, supported
// level at z, on a ledge surface, no roof beside or just above.
bool ledge_spot(const World& w, const Actor& a, int x, int y, int z);

// The best pull-up from a hang at (x, y, z) facing dir. False if none works.
bool pull_from(const World& w, const Actor& a, int x, int y, int z, int dir, Move& out);

// Falling (or coming down in a jump) with jump held: a hang point within the catch window
// (kCatchBelow below to kCatchAbove + fall above him), facing dir or 45 degrees off, nudged
// up to 16 sideways or forward, from which a pull-up works. Fills x, y, z, dir and the
// edge (for the highlight).
bool catch_point(const World& w, const Actor& a, int fall, int& x, int& y, int& z, int& dir, int edge[3]);

}  // namespace ledge
}  // namespace pg
