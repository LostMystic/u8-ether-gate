// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Original bug fix: falling out of the world (specs/world/bugfixes.md 15, NOTES.md "Original bug
// fixes"). In the original a fall ends at z 0 even with nothing under the Avatar (physics.md 7),
// and in a few places he can walk or jump past the edge of a map's scenery, or drop through a
// hole in the floor, into empty space: there he is stuck (only Recall gets him out) or killed by
// the fall. With "Original bug fixes" On the engine remembers where he last stood on real ground
// and, when he comes to rest in nothing, puts him back there (no fall damage for such a fall).
// The same covers the stitched world: past a seam or a segment edge where no neighbour has ground.
#include <algorithm>
#include <cstdlib>

#include "core/bugfix.h"
#include "core/log.h"
#include "game/assist.h"
#include "game/processes.h"
#include "game/world.h"

namespace pg {

bool World::in_void(const Actor& a) const {
    if (a.parent || (a.flags & kItemEthereal)) return false;
    // Off the chunk grid altogether (x or y past 32767): in no chunk, invisible to everything.
    if (a.chunk < 0) return a.x > kMapMax || a.y > kMapMax;
    if (a.z > 0) return false;  // not at the bottom: falling, hanging or held up by something
    int xd, yd, zd;
    footprint(a, xd, yd, zd);
    Placement p = can_exist(a.x, a.y, a.z, xd, yd, zd, shape_flags(a), a.id);
    return p.support_z < -100 && !p.land;  // nothing at all under his feet: the bottom of the world
}

namespace {

// A standable spot on real ground near (x, y): rings of 16 units out to `r_max`, at heights
// around z first, then any height the ground has there.
bool find_ground(World& w, const Actor& a, int x, int y, int z, int r_max, int& ox, int& oy, int& oz) {
    int xd, yd, zd;
    w.footprint(a, xd, yd, zd);
    const uint16_t mf = w.shape_flags(a);
    for (int r = 0; r <= r_max; r += 16)
        for (int dy = -r; dy <= r; dy += 16)
            for (int dx = -r; dx <= r; dx += 16) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                int px = x + dx, py = y + dy;
                if (px < xd || py < yd || px > World::kMapMax || py > World::kMapMax) continue;
                for (int k = 0; k <= 64; ++k) {
                    int pz = k == 0 ? z : (k % 2 ? z + 8 * ((k + 1) / 2) : z - 8 * (k / 2));
                    if (pz < 0 || pz > 255) continue;
                    World::Placement p = w.can_exist(px, py, pz, xd, yd, zd, mf, a.id);
                    if (p.valid && p.supported && p.land && p.support_z == pz && !assist::dangerous(w, a, px, py, pz)) {
                        ox = px; oy = py; oz = pz;
                        return true;
                    }
                }
            }
    return false;
}

}  // namespace

void World::void_rescue_frame() {
    VoidRescue& v = void_rescue;
    Actor* a = avatar();
    if (!original_bug_fixes() || !a || a->is(Actor::kDead) || stasis || kernel_.paused()) {
        v.frames = 0;
        return;
    }
    const unsigned map = map_.map_number();
    const bool falling = a->gravity_pid != 0;
    // A teleport (a script, a teleporter, a seam crossing, the debug warp) is not a fall of his
    // own: the spot he stood on before it does not count any more.
    const bool jumped = map != v.last_map || std::abs(a->x - v.last[0]) > 128 || std::abs(a->y - v.last[1]) > 128;
    v.last[0] = a->x; v.last[1] = a->y; v.last[2] = a->z;
    v.last_map = map;
    if (jumped) {
        v.valid = false;
        v.frames = 0;
    }
    if (!falling && !a->parent && a->chunk >= 0 && !in_void(*a)) {
        v.frames = 0;
        // Real ground under his feet (not a ledge he hangs from, not lava): the spot to come back to.
        if (a->last_anim != 45) {
            int xd, yd, zd;
            footprint(*a, xd, yd, zd);
            Placement p = can_exist(a->x, a->y, a->z, xd, yd, zd, shape_flags(*a), 1);
            if (p.valid && p.supported && p.land && p.support_z == a->z && !assist::dangerous(*this, *a, a->x, a->y, a->z)) {
                v.valid = true;
                v.map = map;
                v.x = a->x; v.y = a->y; v.z = a->z;
            }
        }
        return;
    }
    if (falling || !in_void(*a)) {
        v.frames = 0;
        return;
    }
    // Only a fall or a walk of his own: he stood on this map's ground a little way from here, with
    // no teleport since. A script or a teleport that puts him on bare ground (usecode parks actors
    // at (100, 100, 0)) is left alone.
    if (!v.valid || v.map != map || std::abs(a->x - v.x) > 1024 || std::abs(a->y - v.y) > 1024) {
        v.frames = 0;
        return;
    }
    if (++v.frames < kVoidRescueFrames) return;
    v.frames = 0;
    int x = 0, y = 0, z = 0;
    bool ok = find_ground(*this, *a, v.x, v.y, v.z, 128, x, y, z);
    if (!ok) {
        PG_LOG("void rescue: the Avatar is in the void at %d,%d,%d (map %u), no ground found", a->x, a->y, a->z, map);
        return;
    }
    PG_LOG("void rescue: the Avatar fell out of the world at %d,%d,%d (map %u); back to %d,%d,%d", a->x, a->y, a->z, map, x, y,
           z);
    kernel_.kill(1, kProcActorAnim);
    kernel_.kill(1, kProcGravity);
    if ((a = avatar())) a->set(Actor::kAnimLock, false);
    move_to(1, x, y, z);
    v.last[0] = x; v.last[1] = y; v.last[2] = z;  // the rescue is no teleport: the spot stays good
    v.x = x; v.y = y; v.z = z;
    ++v.rescues;
}

}  // namespace pg
