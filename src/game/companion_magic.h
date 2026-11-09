// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Leveling stage 5 (PLAN-LEVELING.md 7; NOTES.md "Companion spells and reagents"): companion spells
// unlock by level or by the Avatar's learning, and the reagent schools count uses per spell, which
// are bought from Mythran. The engine's own design. Data: data/companion_magic.json. Everything
// needs Leveling::active(); with it off, Party::cast and the AI behave exactly as before.
//
// - Unlocks: damaging and Tempest spells by the companion's level; heals and summons once the
//   Avatar has learned a matching spell (a global or an item he carries; sticky once seen, saved);
//   essential spells (Ilvaran's raise, Nerissa's rite) never lock.
// - Uses: Morvain, Kalvax, Wynna: a count per spell, full at the start (and for old saves). A cast
//   takes one use and the mana. The others: mana only.
// - Banter: once when a spell runs low, once when one is out (again after a restock).
// - Restock: when a conversation with Mythran ends, each reagent user in the party on his map buys
//   uses with the obsidian in their own pack.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pg {

class World;
class Leveling;
class SaveWriter;
class SaveReader;

struct MagicSpellRule {
    std::string spell;            // companions.json spell id
    int level = 1;                // unlock level (when avatar is empty and !always)
    bool always = false;          // essential: never locked
    std::vector<std::string> avatar;  // unlock: any one of these Avatar spells learned
    int stock = 0;                // full uses (0 = not counted: mana only)
    std::string model;            // the matching Avatar spell the price is modelled on
    int reagents = 0;
    int price = 0;                // obsidian per use (worked out at load)
};

struct MagicCompanion {
    std::string id, school;
    std::vector<MagicSpellRule> spells;
    bool counts_uses() const;     // a reagent school
};

struct AvatarSpell {
    std::string id, name, why;
    int global_pos = -1, global_width = 1, global_value = 1;
    std::vector<std::pair<unsigned, unsigned>> items;  // shape, frame
};

class CompanionMagic {
public:
    CompanionMagic(World& w, Leveling& l);

    // --- Data
    bool load_text(const std::string& text, std::string& err);
    bool loaded = false;
    int doses = 10;
    unsigned mythran = 33;
    std::map<std::string, int> reagent_price;
    std::map<std::string, AvatarSpell> avatar_spells;
    std::map<std::string, MagicCompanion> companions;  // by companion id
    static int price_of(int reagents, int reagent_price, int doses);

    // --- State (saved: Leveling's optional section "LVL5")
    std::map<std::string, std::map<std::string, int>> uses;  // companion id -> spell id -> left (missing = full)
    std::set<std::string> learned;                           // Avatar spell ids seen learned
    std::set<std::string> said_low, said_out;                // companion ids: banter said this cycle

    // --- Queries (all with Leveling active; callers check)
    struct Gate {
        bool locked = false;     // not unlocked yet (why says by what)
        bool empty = false;      // a counted spell with no uses left
        int uses = -1;           // uses left, -1 = not counted
        int stock = 0;
        std::string why;         // "level 3", "needs Healing Touch", "out of reagents"
        bool ok() const { return !locked && !empty; }
    };
    Gate gate(int idx, const std::string& spell) const;  // idx: Party::defs index
    const MagicSpellRule* rule(const std::string& companion, const std::string& spell) const;
    const MagicCompanion* companion(const std::string& id) const;
    int left(const std::string& companion, const std::string& spell) const;  // -1 = not counted
    bool learned_now(const std::string& avatar_spell) const;                 // from the game state
    // "out": a counted spell of theirs that is unlocked has no uses; "low": one is at or below its
    // low mark; "ok" otherwise ("" = not a reagent user).
    std::string reagent_state(int idx) const;
    bool is_low(const MagicSpellRule& r, int left) const;
    // The AI's rule: with few uses left a spell is not spent on a trivial target (a monster well
    // below the caster's level, or one a blow would finish).
    bool worth_casting(int idx, const std::string& spell, uint16_t target) const;
    // One line for the HUD: each spell with its uses, or why it is locked.
    std::string summary(int idx) const;

    // --- Events
    void on_cast(int idx, const std::string& spell);  // a use spent
    void frame();      // learning, banter, the Mythran restock (from Leveling::frame while active)
    void restock();    // the party buys (tests call it directly too)
    void reset();
    void save(SaveWriter& o) const;
    bool load(SaveReader& in);
    // A full restock's price for this companion, minus nothing (what they would spend).
    int restock_cost(int idx) const;
    std::vector<std::string> log;  // what happened (tests)

private:
    World& w_;
    Leveling& l_;
    bool talking_mythran_ = false;
    uint32_t next_learn_check_ = 0;
    void check_learned();
    void banter();
};

}  // namespace pg
