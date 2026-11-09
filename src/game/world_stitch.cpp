// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// The stitched world in the live game (world.h "The stitched world"; NOTES.md "The stitched
// world"): the World option, seam crossings and the neighbours' scenery.
#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "data/compat.h"
#include "core/config.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "world/world_space.h"

namespace pg {

// The default of the World option: Stitched, chosen after the critical-path replay compared
// both modes (NOTES.md "The stitched world": story globals identical at every milestone).
bool World::stitched_by_default() { return true; }

bool World::set_world_mode(bool on, const std::string& path) {
    pending_cross_ = pending_cross_egg_ = -1;
    if (!on) {
        if (stitch) PG_LOG("world: original maps");
        stitch.reset();
        neighbors.reset();
        return true;
    }
    if (!compat::world_known()) {
        compat::off("the stitched world", "this copy's maps differ from the ones its layout was made for");
        stitch.reset();
        neighbors.reset();
        return false;
    }
    std::string p = path.empty() ? world_layout_path : path;
    auto ws = std::make_unique<WorldSpace>(data_);
    std::string err;
    if (p.empty() || !ws->load_file(p, err)) {
        PG_LOG("world: no stitched layout (%s); original maps", p.empty() ? "no world_layout.json" : err.c_str());
        stitch.reset();
        neighbors.reset();
        return false;
    }
    world_layout_path = p;
    PG_LOG("world: stitched (%s: %zu groups, %zu seams, %zu teleport links)", p.c_str(), ws->layout().atlas.groups.size(),
           ws->layout().seams.size(), ws->layout().links.size());
    stitch = std::move(ws);
    neighbors = std::make_unique<NeighborStream>(data_);
    return true;
}

void World::register_world_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World& w) {
        MenuGump::OptionRow r{"World:", [&w] { return std::string(!compat::world_known() ? "Original" : w.stitched() ? "Stitched" : "Original maps"); },
                              [&w](int) {
                                  bool on = !w.stitched();
                                  w.set_world_mode(on, w.world_layout_path);
                                  Config& c = Config::global();
                                  c.set("world_mode", w.stitched() ? "stitched" : "original");
                                  if (!c.path().empty()) c.save();
                              }};
        r.group = "Game";
        r.kind = MenuGump::OptionRow::kChoice;
        return r;
    });
}

void World::avatar_world_point(int32_t out[3]) const {
    const Actor* av = avatar();
    out[0] = av ? av->x : 0;
    out[1] = av ? av->y : 0;
    out[2] = av ? av->z : 0;
    if (stitch && av) stitch->to_world(map_.map_number(), av->x, av->y, av->z, out);
}

bool World::stitch_portal(const Item& e) const {
    return stitch && stitch->is_portal(map_.map_number(), e.mapnum, e.quality & 0xFF);
}

bool World::cross_seam(unsigned to, int egg_id) {
    auto t0 = std::chrono::steady_clock::now();
    Actor* av = avatar();
    const unsigned from = map_.map_number();
    if (!stitch || !av || !stitch->placed(from) || !stitch->placed(to) || !stitch->seam(from, to)) return false;
    int32_t w[3];
    stitch->to_world(from, av->x, av->y, av->z, w);
    int lp[3];
    stitch->to_local(to, w, lp);
    if (lp[0] < 0 || lp[1] < 0 || lp[0] >= 32768 || lp[1] >= 32768 || lp[2] < 0 || lp[2] > 255) return false;
    int fdx, fdy, fdz, tdx, tdy, tdz;
    stitch->offset(from, fdx, fdy, fdz);
    stitch->offset(to, tdx, tdy, tdz);
    // The ids of this map's teleporters into `to` (their arrival eggs there, for hatch_arrival_eggs).
    std::vector<unsigned> arrival_ids;
    for (size_t i = 512; i < objs_.slots(); ++i) {
        const Item* it = objs_.get(uint16_t(i));
        if (it && it->chunk >= 0 && data_.typeflags.get(it->shape).family() == kFamTeleportEgg && it->frame != 1 && it->mapnum == to)
            arrival_ids.push_back(it->quality & 0xFF);
    }
    // A fight does not cross a seam: the foes stay on their map, the party has got away.
    if (turn && turn->active()) turn->end_fight(TurnCombat::kEscape);
    const int ox = av->x, oy = av->y, oz = av->z;
    // The map change itself is the original's (items.md 9): the old map's nonfixed items stored,
    // processes killed, the new map's items cached in, every NPC's schedule event, a new egg
    // hatcher; followers travel along (Party::before_map_change).
    av->mapnum = uint8_t(to);
    if (!change_map(to)) {
        av->mapnum = uint8_t(from);
        return false;
    }
    int xd, yd, zd;
    footprint(*av, xd, yd, zd);
    const uint16_t mf = shape_flags(*av);
    auto fits = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || x >= 32768 || y >= 32768 || z < 0 || z > 255) return false;
        Placement p = can_exist(x, y, z, xd, yd, zd, mf, 1);
        return p.valid && (p.supported || p.support_z >= z - 24);
    };
    // He is at the same world point at once, and the fast area is brought up around him now
    // (what the end of this frame would do): the ground of a glob only exists once its chunk is
    // fast, and the placement test needs it.
    push_ether(1);
    ether_pop();
    move_to(1, lp[0], lp[1], lp[2]);
    {
        int cam[3];
        camera_point(cam);
        map_.update_fast_area(cam, cam, view_w, view_h);
        std::copy(cam, cam + 3, prev_cam_);
        fast_init_ = true;
    }
    int px = lp[0], py = lp[1], pz = lp[2];
    bool ok = fits(px, py, pz);
    // Nearby (the two copies of the strip can differ by a tile): rings out to 128 units.
    for (int r = 8; r <= 128 && !ok; r += 8)
        for (int k = 0; k < 16 && !ok; ++k) {
            static const int ux[16] = {0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2, -2, -2, -2, -1};
            static const int uy[16] = {-2, -2, -2, -1, 0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2};
            for (int dz : {0, 8, -8, 16})
                if (!ok && fits(lp[0] + ux[k] * r / 2, lp[1] + uy[k] * r / 2, lp[2] + dz)) {
                    px = lp[0] + ux[k] * r / 2; py = lp[1] + uy[k] * r / 2; pz = lp[2] + dz; ok = true;
                }
        }
    // Walking off the edge where the partner's ground lies lower (map 3's shore at z 8 below
    // Tenebrae's z 48 west of the city): he goes over and falls onto it, as off any edge, instead
    // of falling down into the void of the map he is leaving (NOTES.md "Original bug fixes").
    if (!ok && egg_id < 0) {
        Placement q = can_exist(lp[0], lp[1], lp[2], xd, yd, zd, mf, 1);
        if (q.valid && q.support_z >= 0 && q.support_z >= lp[2] - 96) {
            ok = true;
            PG_LOG("world: seam crossing %u -> %u: the ground is %d below; he drops onto it", from, to, lp[2] - q.support_z);
        }
    }
    if (!ok) {
        Placement q = can_exist(lp[0], lp[1], lp[2], xd, yd, zd, mf, 1);
        const Item* b = item(q.blocker);
        PG_LOG("world: seam crossing %u -> %u: the same point %d,%d,%d is %s (blocker shape %u, support z %d)", from, to, lp[0],
               lp[1], lp[2], q.valid ? "unsupported" : "blocked", b ? b->shape : 0, q.support_z);
    }
    if (!ok && egg_id >= 0) {
        // The original arrival point (the teleporter's frame-1 egg with the same id).
        for (size_t i = 512; i < objs_.slots() && !ok; ++i) {
            const Item* it = objs_.get(uint16_t(i));
            if (it && it->chunk >= 0 && data_.typeflags.get(it->shape).family() == kFamTeleportEgg && it->frame == 1 &&
                int(it->quality & 0xFF) == egg_id) {
                px = it->x; py = it->y; pz = it->z; ok = true;
            }
        }
        if (ok) ++seam_stats.fallbacks;
    }
    if (!ok) {
        PG_LOG("world: seam crossing %u -> %u: no room at %d,%d,%d; back", from, to, lp[0], lp[1], lp[2]);
        av->mapnum = uint8_t(from);
        change_map(from);
        move_to(1, ox, oy, oz);
        return false;
    }
    if (px != lp[0] || py != lp[1] || pz != lp[2]) move_to(1, px, py, pz);
    hatch_arrival_eggs(arrival_ids, egg_id);
    just_teleported = true;  // standing in the partner's teleporter does not send him back
    region_shift.dx = fdx - tdx;
    region_shift.dy = fdy - tdy;
    region_shift.dz = fdz - tdz;
    region_shift.from = from;
    region_shift.to = to;
    ++region_shift.serial;
    ++seam_stats.crossings;
    if (neighbors) neighbors->clear();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    seam_stats.last_ms = ms;
    seam_stats.max_ms = std::max(seam_stats.max_ms, ms);
    PG_LOG("world: seam crossing map %u -> %u at world %d,%d,%d (map %u %d,%d,%d -> map %u %d,%d,%d), %.2f ms", from, to,
           w[0], w[1], w[2], from, ox, oy, oz, to, px, py, pz, ms);
    return true;
}

// The original sends the Avatar over a seam by a teleport onto the arrival egg, and scripts rely on
// the usecode eggs there: CEMHAT2 at the map 5 -> 6 arrival (5343, 29759, range 0) is what notes the
// ceremonial dagger (G[195]), without which CEMHAT never moves Vividos to the altar and the Ceremony
// of Eternity plays outside the tomb (specs/world/scenes.md 1.2, 1.6). A crossing at the same world
// point lands him elsewhere (365 units off there), so those eggs hatch now, once, as if he stood on
// the arrival egg; eggs whose range takes in where he really is are left to the egg hatcher.
void World::hatch_arrival_eggs(const std::vector<unsigned>& ids, int egg_id) {
    const Actor* av = avatar();
    if (!av) return;
    int axd, ayd, azd;
    footprint(*av, axd, ayd, azd);
    // The arrival egg: the teleporter's own, else (walked over the edge) the nearest of the old
    // map's teleporters into this one.
    const Item* dest = nullptr;
    long best = 1L << 40;
    for (size_t i = 512; i < objs_.slots(); ++i) {
        const Item* it = objs_.get(uint16_t(i));
        if (!it || it->chunk < 0 || data_.typeflags.get(it->shape).family() != kFamTeleportEgg || it->frame != 1) continue;
        unsigned id = it->quality & 0xFF;
        if (egg_id >= 0 ? int(id) != egg_id : std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
        long d = std::labs(long(it->x) - av->x) + std::labs(long(it->y) - av->y);
        if (d < best) { best = d; dest = it; }
    }
    if (!dest) return;
    const int dx = dest->x, dy = dest->y, dz = dest->z;
    // The egg hatcher's test (the Avatar's footprint in the egg's range), with the footprint grown
    // by `m` on every side: the first step off the arrival egg (CEMHAT2, range 0, touches the edge
    // of a footprint on its arrival egg: the player steps onto it walking away). One step only: the
    // GHOULEGG there (q 45) starts 32 units further and is left for him to walk into.
    auto covers = [&](const Item& e, int x, int y, int z, int m) {
        int rx = (e.npcnum >> 4) * 32, ry = (e.npcnum & 15) * 32;
        return e.x - rx <= x + m && x - axd - m < e.x + rx && e.y - ry <= y + m && y - ayd - m < e.y + ry && e.z - 48 < z &&
               z <= e.z + 48;
    };
    constexpr int kArrivalSteps = 32;
    std::vector<uint16_t> hatch;
    for (size_t i = 512; i < objs_.slots(); ++i) {
        const Item* e = objs_.get(uint16_t(i));
        if (!e || e->chunk < 0 || e->hatched || data_.typeflags.get(e->shape).family() != kFamUsecodeEgg) continue;
        if (covers(*e, dx, dy, dz, kArrivalSteps) && !covers(*e, av->x, av->y, av->z, 0)) hatch.push_back(uint16_t(i));
    }
    for (uint16_t id : hatch) {
        if (Item* e = item(id)) {
            e->hatched = true;
            PG_LOG("world: seam crossing: arrival egg %u (shape %u q %u at %d,%d,%d) hatched", id, e->shape, e->quality, e->x, e->y, e->z);
            call_event(id, kEvHatch);
        }
    }
}

void World::stitch_frame() {
    if (pending_cross_ >= 0) {
        unsigned to = unsigned(pending_cross_);
        int egg = pending_cross_egg_;
        pending_cross_ = pending_cross_egg_ = -1;
        // Not possible (the strip blocked on the other side): the original teleport.
        if (!cross_seam(to, egg)) kernel_.add(std::make_unique<TeleportProcess>(*this, to, unsigned(std::max(egg, 0))));
    }
    Actor* av = avatar();
    // Walking off the current map's scenery onto the neighbour's ground across a seam.
    if (av && av->chunk >= 0 && !stasis && !av->is(Actor::kDead) && !kernel_.paused()) {
        const unsigned cur = map_.map_number();
        int xd, yd, zd;
        footprint(*av, xd, yd, zd);
        int cx = av->x - xd / 2, cy = av->y - yd / 2;
        const bool on_own = stitch->coverage(cur).any(cx, cy);
        // Walked off: he stood on this map's scenery a moment ago, within a few steps of here.
        const bool walked = edge_last_map_ == int(cur) && std::abs(av->x - edge_last_[0]) <= 256 &&
                            std::abs(av->y - edge_last_[1]) <= 256 && std::abs(av->z - edge_last_[2]) <= 64;
        if (on_own) {
            edge_last_[0] = av->x; edge_last_[1] = av->y; edge_last_[2] = av->z;
            edge_last_map_ = int(cur);
        } else if (walked) {
            int32_t w[3];
            stitch->to_world(cur, cx, cy, av->z, w);
            for (unsigned p : stitch->seam_partners(cur)) {
                int lp[3];
                stitch->to_local(p, w, lp);
                if (stitch->coverage(p).land(lp[0], lp[1])) {
                    ++seam_stats.edge_crossings;
                    cross_seam(p, -1);
                    break;
                }
            }
        }
    }
    // The neighbours' scenery around the camera (drawn by the game view).
    if (neighbors) {
        int cam[3];
        camera_point(cam);
        neighbors->update(*stitch, map_.map_number(), cam, view_w, view_h,
                          [this](unsigned m) { return stored_items(m); });
    }
}

}  // namespace pg
