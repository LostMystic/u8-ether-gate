// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Item powers (NOTES.md "Item powers"; option "Item powers", Game group, Config "item_powers"):
// engine-side abilities of items, data-driven (data/items/abilities.json). So far the Skull of
// Quakes: using it anywhere but on its red slot shakes the screen and hurts the hostile creatures
// around the Avatar, then it is quiet for half an hour of play. The item's original usecode keeps
// working: see Ability::keeps_original_use.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

class Item;
class SaveReader;
class SaveWriter;
class UCProcess;
class World;

namespace item_powers {

struct Ability {
    std::string id, effect = "quake", name, log_name, log_text, quiet_text;
    unsigned shape = 0;
    int frame = -1;                   // -1 = any frame
    uint32_t cooldown_frames = 0;     // 30 frames = one second of play
    int radius = 512, height = 160;   // world units around the Avatar
    int damage_min = 10, damage_max = 25;
    int quake_strength = 5, quake_frames = 60;
    std::vector<int> sfx;
    int sfx_priority = 100;
    // The usecode still owns a use: at this quality it runs (with its target cursor), and only
    // picking target_shape is that use. -1 = no original use, the power always fires at once.
    int original_quality = -1, original_target_shape = -1;
    bool keeps_original_use() const { return original_quality >= 0; }
};

// The table: the built-in skull until a data file replaces it. Process-wide, like the options.
std::vector<Ability>& abilities();
bool load_file(const std::string& path, std::string& err);
bool load_text(const std::string& text, std::string& err);

// The option.
bool& enabled();
void register_options();                  // the "Item powers" row (app/main.cpp, the tests)
void apply_options();                     // reads the setting (World::load_options)

// The item's power, nullptr if it has none or the option is off.
const Ability* find(const Item& it);
bool is_power_shape(unsigned shape);      // for the turn menu's item list

// Frames of cooldown left (0 = ready), and the player's words for them.
uint32_t cooldown_left(const World& w, const Ability& ab);
std::string time_left_text(uint32_t frames);
// The item's name for the turn menu ("Skull of Quakes", "... (24 min)"); "" if it has no power.
std::string display_name(const World& w, const Item& it);
// A turn-based Use item would only meet the cooldown: true, with the words, so no turn is lost.
bool refuse_turn_use(const World& w, uint16_t item, std::string& status);

// --- Hooks (each one line in shared code)
// World::call_event, use: true = the power handled it, the usecode must not run.
bool on_use(World& w, uint16_t item);
// Intrinsic 0x000: an item's usecode asked for a target; picking something else fires its power.
void on_target_pick(World& w, const UCProcess& proc, uint16_t target_pid);
// Intrinsic 0x049: the item's look event names it; the cooldown is added to the words.
void decorate_look(const World& w, const UCProcess& proc, std::string& text);
// World::frame: pending power, cooldowns (only while the game runs), the end of the shake.
void frame(World& w);
void new_game(World& w);
// Savegames: an optional trailing section, written only while something is running; a save
// without it (every older one) has all powers ready.
void save(const World& w, SaveWriter& o);
void load(World& w, SaveReader& i);

// Fires the power now (false, with the Avatar's words, while it is on cooldown).
bool fire(World& w, const Ability& ab);

struct Stats { unsigned fired = 0, refused = 0, hits = 0; };
Stats& stats();

}  // namespace item_powers
}  // namespace pg
