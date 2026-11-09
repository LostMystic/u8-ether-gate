// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Levels and experience (PLAN-LEVELING.md section 2 and 3; the engine's own design, not in the
// original game; NOTES.md "Levels and XP"). Behind the "Leveling: On/Off" option: a bare World has
// it off (every older test runs the original rules), main() turns it on from the setting
// (default On, `--leveling on|off`). Off changes nothing: no XP, use-training as in the original.
//
// On: eight levels by total XP, no training by use (World::train_stat), no stat cap, a new game's
// Avatar at STR 11, DEX 10, INT 10. XP comes from kills (data/monster_levels.json) and critical-path
// steps (data/quest_xp.json), to the Avatar and every companion walking with him, in full.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/gumps.h"

namespace pg {

class World;
class Actor;
class CritPath;
class SaveWriter;
class SaveReader;
class StatGrowth;
class CompanionMagic;

constexpr int kMaxLevel = 8;
constexpr int kLevelXp[kMaxLevel] = {0, 100, 200, 400, 800, 1600, 3200, 6400};
constexpr int kRecruitLevelCap = 5;  // recruits never join above this level

// The Avatar's gains per level; the companions' come from "level_classes" in companions.json.
constexpr int kAvatarGainStr = 1, kAvatarGainDex = 1, kAvatarGainInt = 1;
constexpr int kAvatarStartStr = 11, kAvatarStartDex = 10, kAvatarStartInt = 10;
constexpr int kOriginalAvatarStr = 15;  // what the original's new Avatar has (NPCDATA)

struct LevelProgress {
    int level = 1;
    int xp = 0;
    // Stats from before levels (an old save, or spawned with the option off): kept as they are, so
    // a recruit's joining gains and the rebased base HP and mana do not apply. Later gains do.
    bool legacy = false;
};

// One level variant of a monster type (stage 3, data/monster_levels.json "variants"): its own XP
// and stats. A variant without stats (has_stats false) keeps the original monster table's.
struct MonsterVariant {
    int level = 1;
    int xp = 0;
    bool has_stats = false;
    int hp0 = 1, hp1 = 1, dex0 = 1, dex1 = 1, str = 0, intel = 0, dmg0 = 1, dmg1 = 1;
};

struct MonsterLevel {
    std::string name;
    int level = 1;  // the default variant's (maps not listed)
    int xp = 0;
    std::vector<MonsterVariant> variants;     // by level, lowest first; never empty once loaded
    std::map<unsigned, int> map_level;        // map number -> the level of the variant used there
    // One level per type (PLAN-CREATURES.md): a variant below the type's level is shown with this
    // before its name ("young", "lesser"): "young troll". Empty: no prefix.
    std::string weak_prefix;
    const MonsterVariant* variant(int level) const;  // that level's, else the nearest below, else the lowest
    int level_on_map(unsigned map) const;
};

// --- Stage 3: what stats do (PLAN-LEVELING.md 5; NOTES.md "Stat effects and monster tuning"). All
// of these only with Leveling on; with it off every rule is the original's.
// Heavy weapons: below this STR the weapon still works, but hits weaker and slower.
struct HeavyWeapon { uint16_t shape; int min_str; };
constexpr HeavyWeapon kHeavyWeapons[] = {{540, 15}, {419, 14}, {542, 13}};  // hammer, axe, sabre
constexpr int kHeavyDamageLossPct = 10;   // per STR point short; at most kHeavyDamageMaxLossPct
constexpr int kHeavyDamageMaxLossPct = 40;
constexpr int kHeavySlowPct = 6;          // the swing plays this much slower per point short, at most 30%
constexpr int kHeavySlowMaxPct = 30;
constexpr int kHeavyDelayFrames = 2;      // and waits this many frames longer per point short
// DEX past 25 (where the original's 25 - DEX attack delay reaches 0): attacks play faster.
constexpr int kDexFastFrom = 25, kDexFastPct = 2, kDexFastMaxPct = 30;
// STR makes the Avatar's climbing play faster (how far he climbs is the same).
constexpr int kClimbFastFrom = 10, kClimbFastPctX2 = 5, kClimbFastMaxPct = 50;  // +2.5% a point, at most +50%
// Dodge: DEX / 10 % up to DEX 30, then +1.5% a point, at most 20%.
constexpr int kDodgeKnee = 30, kDodgeMaxPermille = 200;
// Turn-based combat: a Move goes this much further per DEX point above 10, at most kMoveBonusMax.
constexpr int kMoveBonusPerDex = 4, kMoveBonusMax = 80;
// Chest traps (shape 756 in a chest, quality high byte 0): noticed by the Avatar within this
// distance, with (DEX - kTrapDexFloor) x kTrapPctPerDex % (one try per chest per map visit).
constexpr int kTrapNoticeRange = 192, kTrapDexFloor = 10, kTrapPctPerDex = 10;
// HP a level adds on top of 2 x STR, by class (the user may want casters to gain some; 0 for now).
constexpr int kCasterHpPerLevel = 0;
// Engine item bits (world/item.h ext_flags) holding a leveled monster's level, 0 = none.
constexpr uint16_t kExtLevelMask = 0x1E00;
constexpr int kExtLevelShift = 9;

class Leveling {
public:
    explicit Leveling(World& w);
    ~Leveling();

    // --- Settings (not in the save)
    bool enabled = false;  // "Leveling: On/Off"
    static void register_options();
    void set_enabled(bool on);

    // --- Data
    std::string data_dir;
    bool load_dir(const std::string& dir, std::string& err);
    bool load_monsters_text(const std::string& text, std::string& err);
    bool load_quests_text(const std::string& text, std::string& err);
    std::map<unsigned, MonsterLevel> monsters;       // by shape
    std::map<std::string, int> quest_xp;             // by critical-path step id
    std::vector<std::pair<std::string, int>> old_save_levels;  // step id, level
    std::vector<int> diff_percent{100, 100, 50, 25, 10};       // by (earner level - monster level)

    // --- State (saved)
    bool started = false;   // this game has levels (a new game with the option on, or adopted)
    int pending_points = 0;  // the Avatar's chosen points not spent yet
    std::map<uint16_t, LevelProgress> progress;  // by actor id: the Avatar and the companions
    std::set<std::string> paid_steps;

    // --- Queries
    bool active() const { return enabled && started; }
    int level_of(uint16_t actor) const;
    int xp_of(uint16_t actor) const;
    // The XP the next level needs (0 at the top).
    static int next_level_xp(int level) { return level >= 1 && level < kMaxLevel ? kLevelXp[level] : 0; }
    static int level_for_xp(int xp);
    // Whether a companion has the rebased level-1 stats (data: "leveling" in companions.json).
    bool rebased(uint16_t actor) const;
    // What a kill pays this earner. The rule that stops farming: data "level_difference_percent".
    int kill_xp(int base_xp, int monster_level, int earner_level) const;
    // The level the story so far suggests (old saves; quest_xp.json "old_save_levels").
    int level_from_story();
    // THE place for a future level limit (shrines, PLAN-LEVELING.md): nobody passes it, their XP
    // waits and the level comes at the next XP after it is raised. No limit for now.
    int level_limit(uint16_t actor) const;

    // The STR that reach counts by (the targeted jump, throwing): with Leveling on, never less than
    // the original's starting STR, so a low-STR Avatar cannot be stopped by a jump the story needs.
    int reach_str(const Actor& a) const;

    // --- Stage 3: stat effects (all return the original's value / do nothing when not active())
    // Monsters: the variant a monster fights with (its stored level, else its map's), nullptr if
    // none or not active.
    const MonsterVariant* monster_variant(const Actor& a) const;
    int monster_level(const Actor& a) const;  // 0 if not a leveled monster type
    // "young " / "lesser " when this monster is a weaker variant of its type (active() only).
    std::string weak_prefix(const Actor& a) const;
    void on_monster_created(uint16_t actor);   // World::create_monster: the map's variant
    void apply_monster(Actor& a, int level);   // that variant's stats (new HP, DEX, STR, INT)
    void on_monster_revived(Actor& a);         // a risen skeleton: its own variant's HP and DEX again
    // Spell damage and timed summons by the caster's INT: x (1 + (INT - 10) / 20), never below x 1.
    static int int_scaled(int value, int intel);
    // The caster of the usecode process running intrinsic 0x053, when it is 1411:233D (Flame Bolt,
    // Explosion, Conflagration, NPC casters; leveling_facts.md 2.1); 0 for traps and anything else.
    static uint16_t usecode_spell_caster(const World& w, uint16_t start_cls, uint16_t start_addr,
                                         const uint8_t* stack, unsigned stack_size);
    // Heavy weapons: the STR an actor is short of its weapon's minimum (0 = fine).
    int heavy_short(const Actor& a) const;
    static int heavy_min_str(unsigned weapon_shape);
    int weapon_damage(const Actor& a, int damage) const;  // a weapon blow, after the heavy rule
    int attack_delay_extra(const Actor& a) const;          // frames added between attacks
    // Animation speed in percent (100 = the original): DEX past 25 for attacks, STR for the Avatar's
    // climbing, heavy weapons slow a swing (game/processes.cpp ActorAnimProcess).
    int anim_speed(const Actor& a, unsigned action, bool attack_seq) const;
    // Dodge chance in permille for a defender's DEX (0 when not active).
    int dodge_permille(const Actor& defender) const;
    bool roll_dodge(const Actor& defender);  // and shows "Dodged"
    int move_bonus(const Actor& a) const;    // turn-based Move range added
    int hp_bonus(uint16_t actor) const;      // extra max HP from levels (kCasterHpPerLevel)
    void on_equip(uint16_t actor, uint16_t item);  // a heavy weapon: the player is told
    void float_text(uint16_t actor, const std::string& text);
    void check_traps();                      // chest traps near the Avatar (frame)
    bool last_dodged = false;                // the last calculate_damage ended in a dodge (the log)
    int dodges = 0;                          // dodges so far (tests, the simulator; not saved)
    std::set<uint16_t> trap_checked;         // chests tried on this map visit (not saved)
    unsigned trap_map = 0xFFFF;

    // --- Events
    void reset();                     // no levels (a load without any: an old save)
    void new_game();                  // the Avatar's starting stats, when on
    void frame();                     // after the party: adoption, quest XP, the prompt, floating text
    void on_kill(uint16_t victim, uint16_t attacker);  // from World::receive_hit, before the death
    void on_spawned(uint16_t actor);  // a companion was created
    void on_recruit(uint16_t actor);  // first join: one level below the Avatar, at most 5
    void add_xp(uint16_t actor, int xp);
    void award_party(int xp);         // quest XP: the Avatar and those walking with him
    // Spends one chosen point: 0 STR, 1 DEX, 2 INT. False when there is none.
    bool spend_point(int stat);
    std::vector<std::string> log;     // what happened (tests)

    // --- Floating text above someone's head (drawn with the barks, not saved)
    struct FloatText { uint16_t actor = 0; std::string text; int frames = 0; };
    std::vector<FloatText> floats;

    // --- Saving (savegame version 6)
    void save(SaveWriter& o) const;
    bool load(SaveReader& in);

    bool calm() const;  // no fight, conversation or scene: the prompt may show

    // Stage 4: trainers, tomes, elixirs, ordeals and quest stats (game/stat_growth.h). Always there.
    std::unique_ptr<StatGrowth> growth;
    // Stage 5: companion spells and reagents (game/companion_magic.h). Always there.
    std::unique_ptr<CompanionMagic> magic;
    // The one place stats go up (with HP and mana raised by what their maximum rose).
    void raise(Actor& a, int str, int dex, int intel);

    // --- Levels set directly (the debug menu's levels page and story jump; app/debug_tools.cpp).
    // The stats a character of this level has by the level-up rules alone: the Avatar's starting
    // stats, his gains and his chosen points spread STR, DEX, INT in turn (the point of level k goes
    // to stat (k - 2) % 3); a companion's level-1 stats and class gains. False: neither.
    bool base_stats(uint16_t actor, int level, int& str, int& dex, int& intel) const;
    // Sets a level with its XP (-1: the level's threshold) and no unspent points (no prompt).
    // rebuild: the stats become base_stats(); else the gains (and the Avatar's points) between the
    // two levels are added or taken away, so trainers, tomes and elixirs stay. HP and mana full.
    // Starts levels in this game if they had not started (Leveling on).
    void set_level(uint16_t actor, int level, bool rebuild, int xp = -1);
    // A game put at a point of the story (a story checkpoint, saved without levels): levels on,
    // the steps already done paid (no quest XP or quest stats for them), the Avatar at this level
    // and XP with rebuilt stats. Nothing when the option is off.
    void start_at_story(int level, int xp);

private:
    void adopt_old_game();
    void level_up(uint16_t actor);
    void check_quests();
    void update_prompt();
    World& w_;
    std::unique_ptr<CritPath> crit_;
    bool crit_tried_ = false;
    bool crit_load();
    uint32_t next_quest_check_ = 0;
    uint32_t next_calm_check_ = 0;
    bool calm_cached_ = false;
};

// The small prompt for the Avatar's chosen point: not modal, play goes on under it, and it waits.
// Mouse: the three buttons. Keys: 1, 2, 3 (GameMode::key_down). Controller: L3 takes the focus,
// Left / Right pick, A chooses, B or L3 gives the focus back (GameMode::pad_event).
class LevelPromptGump : public Gump {
public:
    static constexpr int kW = 170, kH = 28;
    explicit LevelPromptGump(World& w);
    const char* kind() const override { return "levelprompt"; }
    void paint(Surface8& s) override;
    bool hit(int lx, int ly) const override { return lx >= 0 && ly >= 0 && lx < kW && ly < kH; }
    bool press(int button, int lx, int ly) override { return true; }
    void click(int button, int lx, int ly) override { pick(button, lx, ly); }
    void double_click(int button, int lx, int ly) override { pick(button, lx, ly); }
    bool button_point(int stat, int& sx, int& sy) const;  // the middle of a button (screen)
    bool pad_focus = false;
    bool pad_hint = false;  // a controller is connected: say how to reach it
    int selected = 0;

private:
    void pick(int button, int lx, int ly);
    int button_at(int lx, int ly) const;
    uint8_t bg_ = 0, edge_ = 0, hl_ = 0;
    bool cols_ = false;
};

}  // namespace pg
