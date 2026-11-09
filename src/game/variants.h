// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>

#include "core/variants.h"

namespace pg {

class World;

// Game variants (NOTES.md "v2.12 features" and "Restored content: the undead invasion"):
// the options-page rows "v2.12 changes", "Undead invasion" and "Underworld entrance" (all Game),
// their settings, and the invasion's per-frame glue.

// Registers the option rows once (app/main.cpp, the tests).
void register_variant_options();
// Reads the settings into the flags (called by World::load_options, so also after a load); a
// save made with the invasion on, loaded with it off, has G[58:1] cleared here.
void apply_variant_options(World& w);

// The undead invasion (specs/world/cut_undead_invasion.md). G[58:1] ("hasHeart") is the whole
// switch: every reader is in the original usecode, nothing there ever writes it.
namespace invasion {
constexpr unsigned kFlag = 58;              // G[58:1]
constexpr unsigned kPlacedFlag = 569;       // G[569:1]: the Heart is on its pentagram point
constexpr unsigned kHeartShape = 442, kHeartFrame = 1;  // BROCK frame 1, the Heart of Earth
constexpr unsigned kAbsorbedShape = 793;    // the Heart once Lithos is absorbed (frame 1)
constexpr int kCheckFrames = 15;            // the inventory is looked at twice a second
// Earthquakes (new glue, section 4): in Tenebrae (maps 39-41) while the flag is set, a quake of
// strength kQuakeStrength for kQuakeFrames every kQuakePeriod frames of game time.
constexpr int kQuakePeriod = 2700, kQuakeFrames = 75, kQuakeStrength = 2;

// The item is somewhere in the Avatar's inventory (nested containers too).
bool avatar_has(const World& w, unsigned shape, unsigned frame);
// One world frame: sets the flag the first time the Heart (442:1) is in his inventory while
// it is not yet placed (G[569:1] = 0) and not absorbed (no 793:1 carried); clears it once it is
// absorbed or placed; runs the quakes. Does nothing while the option is Off.
void frame(World& w);
// The option was switched: Off clears the flag (and stops an engine quake).
void option_changed(World& w);
// For tests and the log.
struct Stats { unsigned sets = 0, clears = 0, quakes = 0; };
Stats& stats();
}  // namespace invasion

// The buried cave mouth on map 57 (NOTES.md "The buried cave mouth"; world/map_patches.*).
namespace underworld {
// Makes the map now loaded agree with the patch groups' On / Off state, at once: adds what is
// missing, removes what should be gone. Other maps pick it up when they load. Called when the
// option changes and after a game is loaded; call it when the Breach opens.
void sync_entrance(World& w);
}  // namespace underworld

}  // namespace pg
