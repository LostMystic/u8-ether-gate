// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Rock Flesh, remade (the engine's design, user's request 2026-11-08; NOTES.md "Rock Flesh"; with
// Original bug fixes on). The original spell (ERTHSPEL 1414:02BF, leveling_facts.md 1.1) makes the
// Avatar immortal for 100 s with no sign at all, so it looked as if it did nothing. Now the token's
// use runs this instead: the Avatar casts (animations 27, 29, 28, "Rel Sanct Ylem"), a target cursor
// picks the Avatar or an ally (a foe or nothing: refused, nothing spent), the target takes no HP
// damage (the original's immortal flag) and is drawn in grey stone for 2 minutes, or 12 rounds of
// turn-based combat. Mana 2 and INT practice U[5,10], as the original. Saved (section RKFL).
#include <cstdint>

namespace pg {

class World;
class SaveWriter;
class SaveReader;

namespace rock_flesh {

constexpr int kFrames = 3600;  // 2 minutes at 30 frames a second
constexpr int kTurns = 12;     // rounds of turn-based combat
constexpr int kMana = 2;

// A use event on a Rock Flesh token (746 frame 2): handled here. Returns the casting process's
// pid (0: not handled, the original script runs).
uint16_t on_use(World& w, uint16_t token);
// Starts the effect on an actor (the cast's end; tests). False for a foe, the dead, a non-actor.
bool apply(World& w, uint16_t target);
bool active(const World& w, uint16_t actor);
void frame(World& w);
// The stone look (a grey copy of the palette) while the effect lasts, else nullptr.
const uint8_t* remap(World& w, uint16_t actor);
void save(const World& w, SaveWriter& o);  // only when an effect is on
void load(World& w, SaveReader& in);       // reads RKFL if it is next

}  // namespace rock_flesh
}  // namespace pg
