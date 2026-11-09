// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The new creatures (PLAN-CREATURES.md; the engine's own content, not in the original game;
// NOTES.md "New creatures"). Behind the option "New creatures" (default On); with it off nothing
// spawns and any that were out go away. They work with Leveling on or off.
//
// Building blocks:
// - An effect-bodied creature: a real actor (HP, collision, the monster table through
//   extra_monster_rows(), XP through Leveling's table) on an engine body shape (2060-2064,
//   data/shapes/shapes.json), drawn by the engine from the game's effect shapes or from an
//   existing sprite (WorldView's appearance hook). It moves and strikes by its own brain, in real
//   time and in turn-based combat (TurnCombat asks turn_act on its turn), and shows in the HUD's
//   roster and the combat log under its own name.
// - A rules list per creature (data/creatures.json): what a hit is (weapon, spell, artifact,
//   fire, water, ...) against an ordered list of "when these tags, damage x N", plus mana drain,
//   hittable only while surfaced, arcing strikes, drawn to metal armour, weakened by a named spell.
//
// The five: the Lurker's pet (water), the wraith, the lava wyrm, the storm eye, the Flame of Pyros.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "world/world_view.h"

namespace pg {

class World;
class Actor;
class SaveWriter;
class SaveReader;

struct CreatureRule {
    std::vector<std::string> when;  // all of these tags on the hit
    int pct = 100;                  // the damage becomes this percentage
    std::string say;                // a line in the combat log when it changes the damage
};

// One drawing: frames first..last of a shape, one step every `ticks` game frames.
struct CreatureDraw {
    unsigned shape = 0;
    int first = 0, last = 0, ticks = 4;
    bool own = false;  // the body's own frame (its stand frame by direction)
    bool set = false;
};

struct CreaturePlace {
    unsigned map = 0;
    int x = 0, y = 0, z = 0;
    std::string after;  // a critical-path step that must be done first ("" = none)
    std::string note;
};

struct CreatureDef {
    std::string id, name, look;
    std::string kind;  // "water" (lives in water 347/438), "lava" (in lava 409), "walker", "floater"
    unsigned body = 0;
    int level = 1, xp = 0;
    int hp0 = 1, hp1 = 1, dex0 = 10, dex1 = 10, str = 0, dmg0 = 1, dmg1 = 1, armour = 0;
    uint16_t dmg_type = 1;
    // Its strike: name (the log), reach (world units, ground plane), frames between strikes, sound.
    std::string attack = "strike";
    int reach = 128, every = 60, sound = -1;
    // A second, close strike (the lava wyrm's lash): within lash_reach it uses this name and damage
    // type instead, and fire protection does not help against it. lash_reach 0 = none.
    std::string lash = "lash";
    int lash_reach = 0;
    uint16_t lash_type = 4;
    int speed = 4;           // world units a frame it moves (real time); turn-based: 300 a turn
    int aggro = 640;         // notices the party within this
    // Surfacing (water and lava kinds): frames up and down, and frames up and calm when baited.
    int up_frames = 150, down_frames = 120, bait_frames = 0;
    int mana_drain = 0;      // mana its landed blows take
    int arc_range = 0, arc_pct = 0, arc_metal_range = 0;  // its strike jumps to others near the target
    bool metal_draws = false;  // picks the target wearing the most metal armour
    std::string weakened_by;   // a companion spell id
    int weak_frames = 0, weak_rounds = 0;
    std::string weak_say;
    bool lava_ok = false;      // a walker that crosses lava (the Flame of Pyros)
    bool fire_protect = false; // Endure Heat (703 near) and Fire Shield (569 near) protect from its strike
    std::vector<CreatureRule> rules;
    CreatureDraw draw_hidden, draw_rising, draw_up, draw_sinking, draw_strike, particle;
    int particle_every = 0;
    bool ghost = false;        // drawn with the 50% ghost blend
    std::string tint;          // "dark": a smoky dark recolour
    std::vector<CreaturePlace> places;
};

// What the brain keeps per creature actor (saved).
struct CreatureState {
    uint16_t actor = 0;
    int def = -1, place = -1;
    enum Phase : uint8_t { kHidden, kRising, kUp, kSinking } phase = kUp;
    int timer = 0;        // frames left in this phase
    int cooldown = 0;     // frames to its next strike
    int weak_frames = 0, weak_rounds = 0;
    int bait_frames = 0;  // eating: up and calm
    int bait_x = 0, bait_y = 0;
    int home_x = 0, home_y = 0;  // where it rose: it never strays far from it
    int idle_turns = 0;   // turn-based: its turns with nobody in reach (it withdraws after 3)
    bool withdrawn = false;
    bool trapped = false; // the Blue Vial hook: its lava turned to rock, it cannot dive
    bool calm = false;    // Virtues (PLAN-VIRTUES.md 3.19): fed by the Avatar, it leaves the party alone until hurt
    uint32_t said = 0;    // frame of its last rule line (not saved; the same line once per 2 s)
    std::string said_line;
    int dead_frames = 0;  // after death: fades, then the body goes
};

class Creatures {
public:
    explicit Creatures(World& w);

    // --- Settings (not in the save)
    bool enabled = false;  // "New creatures" (main turns it on from the setting, default On)
    static void register_options();

    // --- Data (data/creatures.json)
    bool load_dir(const std::string& dir, std::string& err);
    bool load_text(const std::string& text, std::string& err);
    bool loaded() const { return !defs.empty(); }
    std::vector<CreatureDef> defs;
    std::map<std::string, std::vector<std::string>> spell_tags;  // companion spell id -> tags
    std::set<unsigned> magic_weapons;
    std::vector<std::pair<unsigned, int>> metal_armour;           // shape, frame (-1 any; odd = its even)
    // Puts the creatures' monster rows (combat.h) and level rows (Leveling) in place.
    void register_rows();
    const CreatureDef* def_of(uint16_t actor) const;
    int def_index(const std::string& id) const;

    // --- State (saved)
    std::map<uint16_t, CreatureState> live;      // by actor id
    std::set<std::string> spawned;               // "id:place"

    // --- Hooks
    void frame();                                // World::frame: spawning and the brains
    void reset();                                // a new game / a load without the section
    bool appearance(uint16_t id, WorldView::Options::Look& look) const;
    bool look_event(uint16_t id);                // World::call_event look: its look text
    // World::receive_hit after the damage calculation: the rules, mana drain. `weapon`: a blow
    // whose damage came from the attacker (melee), not a spell or an effect.
    int on_hit(uint16_t victim, uint16_t attacker, int damage, uint16_t type, bool weapon);
    void on_touch(uint16_t floor, uint16_t toucher);  // gotHit of water: bait
    bool untargetable(uint16_t id) const;        // under the water / lava now
    bool withdrawn(uint16_t id) const;           // has left the fight (turn-based)
    bool is_creature(uint16_t id) const { return live.count(id) != 0; }
    // Turn-based: the creature's whole turn (move, surface or dive, strike). Returns false if it
    // is not a creature.
    bool turn_act(uint16_t id);
    // The Blue Vial hook (PLAN-CREATURES.md: "the Blue Vial's lava-to-rock traps it"): a lava
    // creature can no longer dive and stays hittable. Nothing calls it yet.
    bool trap_in_rock(uint16_t id);

    // Spawns one creature now at a place (tests, the debug menu); returns the actor.
    uint16_t spawn(int def, int place);
    uint16_t spawn_at(int def, int x, int y, int z);
    // Its strike on a target now (both combat styles).
    void strike(uint16_t id, uint16_t target);
    int metal_pieces(uint16_t actor) const;
    std::vector<std::string> hit_tags(uint16_t victim, uint16_t attacker, uint16_t type, bool weapon) const;
    // Whether the creature fits at (x, y, about z) in its habitat (water, lava, or standable ground);
    // out_z is the height it would be at.
    bool habitat_ok(const CreatureDef& d, int x, int y, int z, uint16_t self, int& out_z) const;
    std::vector<std::string> log;  // what happened (tests)
    int strikes = 0, arcs = 0, baits = 0;

    void save(SaveWriter& o) const;
    bool load(SaveReader& in);
    bool has_state() const { return !live.empty() || !spawned.empty(); }

private:
    void brain(CreatureState& st, Actor& a, const CreatureDef& d);
    uint16_t pick_target(const Actor& a, const CreatureDef& d, int range) const;
    bool step_toward(Actor& a, const CreatureDef& d, int tx, int ty, int dist_max, int stop_at);
    int dist_to(const Actor& a, const Actor& t) const;
    bool fire_protected(const Actor& v) const;
    void say(CreatureState& st, const std::string& line);
    void effects(CreatureState& st, const Actor& a, const CreatureDef& d);
    void set_phase(CreatureState& st, const CreatureDef& d, CreatureState::Phase p);
    const CreatureDraw& drawing(const CreatureState& st, const CreatureDef& d) const;
    void spawn_here();
    void build_tint();
    World& w_;
    std::vector<uint8_t> dark_;
    unsigned last_map_ = 0xFFFF;
};

// The name an actor is shown under (combat log, HUD, turn-based lines): a new creature's own
// name, or a leveled monster's weaker variant ("young troll"). Empty: the usual name.
std::string creature_display_name(const World& w, uint16_t id);

}  // namespace pg
