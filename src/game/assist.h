// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6 movement assist and edge guard (PLAN-M6.md; the engine's own design, Responsive
// mode only). All the tunable constants are here.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace pg {

class World;
class Actor;

namespace assist {

// --- Tunables (world units; a game pixel is 2-4 units, the Avatar's footprint is 64 x 64).
constexpr int kProbeStep = 4;      // sideways probes at 4, 8, 12 ... units, both sides, nearest first
constexpr int kProbeMax = 56;      // furthest sideways probe (inside a +-30 degree cone of kAhead)
constexpr int kAhead = 96;         // the way on must be clear and safe at least this far (1.5 bodies)
constexpr int kNudge = 12;         // up to this: moved there at once, the step plays on
constexpr int kSnap = 32;          // up to this: a quick slide of kSnapSpeed a frame
constexpr int kSnapSpeed = 12;    // units per frame
// Beyond kSnap (to kProbeMax): the same slide, up to 5 frames (a sideways walk cycle is ~100
// units and overshoots; the pathfinder is not needed at these distances). All of it only
// while the player keeps pushing the same way.
constexpr int kSafeDrop = 72;      // a drop above this hurts (physics.md 7: damage from 73 up)

// --- Jumps (Responsive): key / pad jumps go forward; a landing that would be unsafe snaps to
// a safe platform within the arc.
constexpr int kHop = 96;           // a standing jump: a short hop this far forward
constexpr int kJumpArcDeg = 30;    // platforms searched up to this far either side, 5 degree steps
constexpr int kJumpMinPct = 40;    // ... from this share of the natural distance
constexpr int kJumpMaxPct = 130;   // ... to this share, plus kJumpSlack units
constexpr int kJumpSlack = 32;
constexpr int kJumpRise = 40;      // a landing at most this much higher than the take-off
constexpr int kLandInset = 12;     // the body's corners (this far inside) must all be on it

// True if he can land and stand at (x, y, z): fits, supported, not dangerous, and every corner
// of the body (kLandInset inside) has a surface within 8 of z (not the very edge).
bool safe_landing(const World& w, const Actor& a, int x, int y, int z);
// The landing for a jump from the actor's box position toward `dir` whose natural landing is
// (nx, ny, nz) after `dist` units: true with the chosen point (it may be the natural one).
// False if nothing safe is in the arc (the jump stays as it is).
bool find_landing(const World& w, const Actor& a, int dir, int dist, int nx, int ny, int nz, int out[3]);

// The surface under a point: the highest top at or below z of anything standing there
// (solid, land, roof, water, damaging), its shape flags and item. False if there is none.
bool surface_under(const World& w, int cx, int cy, int z, uint16_t self, int& top, uint16_t& flags, uint16_t& id);

// True if the actor standing (or arriving in the air) at box position (x, y, z) would be over
// water, lava / a damaging surface, or above a drop of more than kSafeDrop.
bool dangerous(const World& w, const Actor& a, int x, int y, int z);

// True if an actor standing at box position (x, y, z) would be in harm's way: dangerous() at
// the centre of its body, or water, lava (409, 483) or a damaging surface right under (within 8
// of) its middle or any corner kLandInset inside the body. The companions' movement and
// catch-ups refuse such spots.
bool hazard(const World& w, const Actor& a, int x, int y, int z);

struct Candidate {
    int x = 0, y = 0, z = 0;
    int offset = 0;  // signed sideways distance
};

// The nearest sideways spot (perpendicular to `dir`) from which `action` in `dir` works and
// stays safe for kAhead units. False if none within kProbeMax.
bool find(World& w, const Actor& a, unsigned action, int dir, Candidate& out);

// Debug drawing (--debug-assist): the probes of the last search (x, y, z, result 0/1/2).
extern std::vector<std::array<int, 4>> last_probes;

}  // namespace assist
}  // namespace pg
