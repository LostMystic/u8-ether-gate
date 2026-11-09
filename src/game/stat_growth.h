// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Leveling stage 4 (PLAN-LEVELING.md 4; NOTES.md "Trainers, tomes, elixirs and ordeals"): the only
// ways besides levels that raise a stat, all the engine's own design and all only with Leveling on
// (Leveling::active()). Data: data/stat_growth.json (trainers' prices, the tomes, elixirs and
// ordeals with their places) and data/quest_stats.json (major story steps that give companions a
// point), next to quest_xp.json.
//
// - Paid trainers: engine-side topics in Darion's and Mythran's menus (data/dialog/darion.json,
//   mythran.json, "feature": "leveling"); a session raises one stat by 1, up to the cap, paid in
//   obsidian (shape 143) from the trainee's pack, then the Avatar's.
// - Tomes (shape 288) and elixirs (shape 766) told apart from the original's by a quality in
//   kQualityBase.. (none in the data uses it, checked by ctest `growth`). They are created on their
//   map the first time it is entered with Leveling on (`spawned`, saved), then are ordinary map
//   items: they are saved with the map, carried and dropped. Use and look are the engine's.
//   A companion reads or drinks one dropped on their portrait (or on them), or used from their pack.
// - Ordeals: places that are hard to get to; standing there the first time gives the Avatar points.
// - Quest stats: each companion walking with the Avatar gets a point in their main stat.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/npc_dialog.h"

namespace pg {

class World;
class Leveling;
class SaveWriter;
class SaveReader;
class CritPath;
class Item;
class Actor;

enum GrowthStat { kGrowStr = 0, kGrowDex = 1, kGrowInt = 2 };

// A tome or an elixir and where it is placed.
struct GrowthItem {
    std::string id, kind;          // kind "tome" or "elixir"
    std::string name;              // what a look says
    int stat = kGrowInt, gain = 1;
    unsigned map = 0;
    int x = 0, y = 0, z = 0;       // world position (items: the high x, high y, low z corner)
    uint16_t shape = 0, frame = 0, quality = 0;
    std::string where;             // for people: the place in words
    std::vector<std::string> text; // a tome's pages
};

struct GrowthOrdeal {
    std::string id, name;
    int stat = kGrowStr, gain = 2;
    unsigned map = 0;
    int x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;  // the Avatar's position in this box
    std::string where, line;       // line: the log's words when done
};

struct QuestStat {
    std::string step;
    int companions = 1;  // each companion walking with the Avatar: this many in their main stat
    int avatar = 0;      // the Avatar: this many in the stat below (0 = nothing)
    int avatar_stat = kGrowStr;
};

struct Trainer {
    unsigned npc = 0;
    std::string name;
    std::vector<int> stats;
    std::map<std::string, std::vector<std::string>> lines;  // by moment: intro, pick, paid, poor, capped, done...
};

class StatGrowth {
public:
    static constexpr uint16_t kQualityBase = 0x5A00;  // our items' qualities: 0x5A00 + n (n < 256)
    static constexpr uint16_t kTomeShape = 288, kElixirShape = 766, kMoneyShape = 143;

    StatGrowth(World& w, Leveling& l);
    ~StatGrowth();

    // --- Data
    bool load_text(const std::string& text, std::string& err);         // stat_growth.json
    bool load_quests_text(const std::string& text, std::string& err);  // quest_stats.json
    bool loaded = false;
    int price_base = 50, price_from = 10, price_growth_permille = 115, price_round = 5;
    int train_cap = 30;
    int tome_min_int = 30;
    std::vector<Trainer> trainers;
    std::vector<GrowthItem> items;
    std::vector<GrowthOrdeal> ordeals;
    std::vector<QuestStat> quest_stats;
    std::map<std::string, int> main_stat;  // companion id -> stat (else by class)
    std::map<std::string, std::vector<std::string>> words;  // shared lines (tome/elixir, companions)

    // --- State (saved: Leveling's optional section "LVL4")
    std::set<std::string> spawned;     // items created on their map
    std::set<std::string> used;        // tomes read, elixirs drunk
    std::set<std::string> ordeals_done;
    std::set<std::string> steps_paid;  // quest_stats steps paid (or already done when adopted)
    bool steps_adopted = false;        // false: the steps done so far are marked paid without a reward
    std::map<uint16_t, std::array<int, 3>> trained;  // sessions bought, by actor and stat

    // --- Queries
    int price(int stat_value) const;   // a session at this stat value; 0 = at the cap
    const GrowthItem* item_of(const Item& it) const;  // ours by shape and quality, else nullptr
    const GrowthItem* find_item(const std::string& id) const;
    const Trainer* trainer(unsigned npc) const;
    int obsidian(uint16_t actor) const;  // carried, in all containers
    int main_stat_of(uint16_t companion) const;
    static int stat_value(const Actor& a, int stat);
    static const char* stat_name(int stat);       // "STR"
    static const char* stat_word(int stat);       // "Strength"
    static int stat_from(const std::string& s);   // "str" -> 0, -1 if unknown

    // --- Hooks
    // World::call_event: use and look of our items. True = handled, no usecode.
    bool on_event(uint16_t item, unsigned event);
    // An item dropped into a companion's pack by the player (the portrait, or onto them).
    void on_given(uint16_t item, uint16_t companion);
    // Engine-side dialogue: what an action topic says and offers (npc_dialog.h DialogAction).
    DialogAction dialog_action(uint16_t npc, const std::string& action);
    void frame();      // from Leveling::frame while active
    void reset();      // no state (a load of a save without the section, before load())
    void new_game();
    void adopt();      // an old game: the steps already done never pay
    void save(SaveWriter& o) const;
    bool load(SaveReader& in);
    // Tests and the debug menu.
    void spawn_here();  // creates this map's items that are not yet in the world
    bool pay(uint16_t trainee, int amount);  // takes obsidian: the trainee's own first, then the Avatar's
    bool pay_own(uint16_t who, int amount);  // takes obsidian from their own inventory only (false: not enough)
    bool train(uint16_t npc, uint16_t trainee, int stat, std::string& say);
    // Reads or drinks: `who` is the Avatar or a companion. False if nothing happened.
    bool consume(uint16_t item, uint16_t who);
    std::vector<std::string> log;  // what happened (tests)

private:
    World& w_;
    Leveling& l_;
    unsigned spawn_map_ = 0xFFFF;
    std::unique_ptr<CritPath> crit_;
    bool crit_tried_ = false;
    CritPath* crit();
    uint32_t next_check_ = 0;
    void check_ordeals();
    void check_quests();
    std::vector<uint16_t> trainees() const;  // the Avatar and the companions walking with him
    std::string line(const Trainer& t, const std::string& moment, const std::string& who = "", int amount = 0,
                     int stat = -1) const;
    std::string word(const std::string& key, uint16_t who = 0, const std::string& thing = "", int stat = -1, int n = 0) const;
    void announce(uint16_t who, int stat, int gain, const std::string& why);
    uint16_t holder(uint16_t item) const;  // the actor carrying it (outermost), 0 if none
};

}  // namespace pg
