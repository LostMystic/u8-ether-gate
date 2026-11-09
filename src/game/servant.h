// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The fighting Aerial Servant (NOTES.md "The Aerial Servant"): an engine-side addition to the
// original spell (specs/world/magic.md 5), behind the "Item powers" option. The original servant
// fetches items and refuses NPCs. With the option On, an Aerial Servant cast on a hostile creature
// summons a fighting servant instead: a short-lived actor (the whirlwind, shape 481) on the
// Avatar's side that flies to the creature and strikes it for small damage a few times. The
// creature turns on it, and a blow or two dispels it in a puff. Friendly NPCs are still refused.
// Tunables: data/items/servant.json (built-in defaults until it is loaded).
#pragma once
#include <cstdint>
#include <string>

namespace pg {

class UCProcess;
class World;

namespace servant {

struct Params {
    bool fights = true;          // the extension itself (with the "Item powers" option)
    unsigned shape = 481;        // the servant's look (MINIAIR, the small whirlwind)
    int hp = 2;                  // a blow or two dispels it
    int dex = 20;                // its defence against blows (it dodges some)
    int strikes = 4;             // blows it deals at most
    int damage_min = 2, damage_max = 4;
    int strike_interval = 45;    // frames between its blows (room for the creature to swing back)
    int first_strike = 15;       // frames from its arrival to the first blow
    int duration = 600;          // frames it lasts at most once it has arrived (20 s)
    int speed = 10;              // flight, world units a frame (the original servant's)
    int hover = 8;               // height above the creature's feet while fighting
    int sfx_strike = 57, sfx_appear = 54, sfx_vanish = 116;
    std::string name = "Aerial Servant";
};
Params& params();
bool load_file(const std::string& path, std::string& err);
bool load_text(const std::string& text, std::string& err);

// The hook (intrinsic 0x02C, item_get_weight): AIRSPEL's Aerial Servant (1430:1093) weighs its
// first target right after the casting animations. A hostile creature there starts the fight and
// ends the spell's process (the original would refuse it). True: handled.
bool on_weight_check(World& w, UCProcess& proc, uint16_t target);

// A hostile creature the fighting servant may be sent at (not the Avatar, a companion, the dead,
// nor a friendly NPC).
bool is_foe(const World& w, uint16_t target);

// Starts a fighting servant from the Avatar at `target` (also for tests). The process id, 0 if not.
uint16_t summon(World& w, uint16_t target);

struct Stats { unsigned summoned = 0, strikes = 0, damage = 0, hits_taken = 0, dispelled = 0; };
Stats& stats();

}  // namespace servant
}  // namespace pg
