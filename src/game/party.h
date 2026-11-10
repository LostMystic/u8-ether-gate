// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Companions (PLAN-COMPANIONS.md; the engine's own design, not in the original game): up to six
// recruitable followers who talk through the game's bark and answer-menu UI, follow the Avatar
// through teleports and map changes, fight beside him, carry their own inventory and paperdoll,
// and are saved with the world. All content (stats, art, spells, dialogue) is data:
// engine/data/companions.json.
//
// Companions are ordinary Actors with object ids from the actor range (248..255 preferred, any
// free actor id otherwise), no usecode class (so the original scripts never run on them) and
// the engine flag kExtCompanion. With the option off (`enabled` false) the party never spawns
// anything and every hook below is a no-op, so the original game is unchanged.
//
//   party.cpp           data loading, state, spawning, save / load, recolour tables, hooks
//   companion_ai.cpp    following, travel, catch-up, combat AI, spells, death, lock picking,
//                       stealing
//   companion_talk.cpp  conversations (barks + the answer menu) and barks by trigger
//   party_gumps.cpp     the party bar
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pg {

class World;
class Actor;
class Surface8;
class SaveWriter;
class SaveReader;
class CritPath;
class Banter;
struct ShapeFrame;

// The engine-only item flag marking a companion actor (world/item.h kExt* values).
constexpr uint16_t kExtCompanion = 0x0040;
// A whole-figure companion sheet (art.paperdoll_body): the figure's centre column in gump 1, left
// of the equipment slot column (gumps.cpp kIconCol*).
constexpr int kDollFigCx = 35;

// A dialogue / bark condition: every field that is set must hold.
struct PartyCond {
    int in_party = -1, party_full = -1, waiting = -1, combat = -1, joined_before = -1, dead_ally = -1;
    int map = -1;
    std::vector<std::string> present, absent;        // other companions in / not in the party
    std::vector<std::string> flags, not_flags;       // this companion's own flags
    std::vector<std::string> step_done, step_not_done;  // critical-path step ids (data/critpath.json)
    std::vector<std::string> asked, not_asked;       // topic ids asked before
    int leveling = -1;                               // Leveling active (game/leveling.h)
    std::string reagents, not_reagents;              // reagent state "out" / "low" / "ok" (game/companion_magic.h)
    bool empty = true;
};

// One answer in a conversation.
struct PartyTopic {
    std::string id, label;
    PartyCond when;
    std::vector<std::string> say;
    std::vector<std::pair<PartyCond, std::vector<std::string>>> alt;  // the first that holds replaces say
    std::string action;  // join wait follow leave trade bye picklock steal cast:<spell> raise:<who>
    std::vector<std::string> set_flags, clear_flags;
    std::vector<PartyTopic> topics;  // a sub-menu ("Back" is added)
};

struct RecolourRule {
    float hue0 = 0, hue1 = 360;  // source hue range (degrees, wraps if hue0 > hue1)
    float min_sat = 0.15f;       // only coloured pixels
    float max_sat = 1.01f, min_val = 0, max_val = 1.01f;
    float add_sat = 0;           // added to the saturation (tints greys and whites)
    float hue = -1;              // new hue (-1 keep), or a shift when `shift` is set
    bool shift = false;
    float sat = 1, val = 1;      // multipliers
};

struct ItemSpec {
    unsigned shape = 0, frame = 0, qty = 0;
};

// Spell reach (the user's request, 2026-11-06): companions.json keeps each spell's original range
// and radius; the game uses them times this, except the spells spell_range_scale() leaves alone.
constexpr int kSpellRangeScale = 2;
// How long the Lightning spell's bolt is seen before its hit lands (and the flash), in game
// frames (30 a second).
constexpr int kLightningFrames = 3;

// An engine-side spell (data: "spells" in companions.json).
struct SpellDef {
    std::string id, name, kind;  // kind: bolt heal field knock slow calm drain summon raise cure lightning
    int mana = 0, dmg0 = 0, dmg1 = 0, range = 448, radius = 0;
    int base_range = 448, base_radius = 0;  // the numbers in the data file, before kSpellRangeScale
    uint16_t dmg_type = 0x20;
    unsigned proj_shape = 0, proj_frame = 0;  // 0 = no travelling missile
    unsigned fx_shape = 0, fx_first = 0, fx_last = 0;
    int sfx = -1, cast_sfx = -1;
    bool combat = true, outside = false;  // castable in / out of combat
    bool flash = false;                   // one frame of pale screen when it lands
    std::string only;                     // raise: only this companion id
};

struct CompanionDef {
    std::string id, name, sex, element, school, role, title;
    int str = 15, dex = 15, intel = 10, hp = 30, mana = 0;
    // With Leveling on (data: "leveling"; game/leveling.h): level 1 stats and the gains per level
    // from the class in "level_classes". The numbers above stay for Leveling off.
    std::string lv_class;
    int lv_str = 15, lv_dex = 15, lv_int = 10, lv_hp = 0, lv_mana = 0;
    int gain_str = 1, gain_dex = 1, gain_int = 1;
    unsigned shape = 1;
    std::vector<RecolourRule> recolour, doll_recolour;
    bool own_doll = false;  // art.paperdoll "own": the paperdoll shows the companion's own figure
    // Engine-made pictures (data/portraits/, data/picture.h): art.portrait = the party-bar picture
    // (24 x 25, drawn instead of the cropped standing frame); art.paperdoll_head = a painted head
    // laid over the own figure's head in the paperdoll ({file, dx, dy}: offset from the fit).
    std::string portrait, doll_head;
    int doll_head_dx = 0, doll_head_dy = 0;
    // art.paperdoll_body: a whole standing figure, painted from the portrait (tools/doll_body.py).
    // With one the companion's sheet shows the figure and the equipment as icons in a column of
    // slots beside it (gumps.cpp, NOTES.md "Companion sheets") instead of on the Avatar's body.
    std::string doll_body;
    // What they may wear and wield (data: "gear"; data/items/equipment.json): "caster", "rogue"
    // or "warrior". Default from the leveling class: caster -> caster, anything else -> warrior.
    std::string gear;
    std::vector<ItemSpec> equip, pack;
    std::vector<std::string> spells;
    int spawn[3] = {0, 0, 0};  // the beach spot (map 3): debug mode, and the fallback for a home
    int spawn_dir = 4;
    // Where they wait for the Avatar to find them (data: "home"; NOTES.md "Companions: homes").
    bool has_home = false;
    unsigned home_map = 3;
    int home[3] = {0, 0, 0};
    int home_dir = 4;
    std::string home_place;  // a name for the debug menu
    bool caster = false, healer = false, rogue = false;
    std::vector<PartyTopic> topics;
    std::map<std::string, std::vector<std::string>> barks;
    struct AreaBark { std::vector<int> maps; std::vector<std::string> lines; };
    std::vector<AreaBark> area_barks;
};

// The live state of one companion (saved).
struct CompanionState {
    enum Mode : uint8_t { kUnspawned, kIdle, kFollow, kWait, kReturn, kDead };  // kReturn: old saves only
    uint16_t id = 0;
    Mode mode = kUnspawned;
    Mode before_death = kIdle;
    bool joined_before = false;
    bool hidden = false;  // the option was turned off: taken off the map until it is on again
    std::set<std::string> flags, asked;
    int wait_pos[3] = {0, 0, 0};
    uint8_t wait_map = 3;
    uint16_t target = 0;
    uint32_t next_bark = 0, next_act = 0, next_cast = 0, next_regen = 0, last_seen_map = 0;
    int stuck = 0, last_pos[2] = {0, 0}, far_frames = 0;
    bool placing = false;  // arrived on a new map with the Avatar: put beside him next frame
    // Following across something they cannot walk (not saved): the frame a step toward him was
    // last refused because of water, lava, a damaging floor or a drop (companion_ai.cpp).
    uint32_t edge_at = 0;
    // A melee blow resolved by the AI (shapes whose attack animation has no hit frame).
    uint16_t pending_hit = 0;
    uint32_t pending_hit_at = 0;
};

// A travelling spell missile (saved).
struct PartyBolt {
    uint16_t item = 0, caster = 0, target = 0;
    int spell = -1;
    int from[3] = {0, 0, 0};
    int frames = 0, total = 1;
};

class Party {
public:
    explicit Party(World& w);
    ~Party();

    // --- Data
    bool load_file(const std::string& path, std::string& err);
    bool load_text(const std::string& text, std::string& err);
    std::vector<CompanionDef> defs;
    std::vector<SpellDef> spells;
    const SpellDef* spell(const std::string& id) const;
    int spell_index(const std::string& id) const;
    std::string data_dir;  // where companions.json was found (critpath.json is looked for there)

    // --- Settings (not part of the save: persistent options, see party.cpp)
    bool enabled = false;   // "Companions: On/Off" (World default off: the original game)
    bool show_bar = true;   // "Party bar: On/Off"
    static constexpr int kMaxMembers = 6;
    // Following across hazards (companion_ai.cpp): a follower held up this long (the `stuck`
    // count, ~frames) within kEdgeRecent frames of a step refused at water / lava / an edge waits
    // there, and comes to the Avatar once he stands on safe ground with room for the party.
    static constexpr int kEdgeStuck = 64;
    static constexpr uint32_t kEdgeRecent = 180;
    static void register_options();  // the options-page rows (once)
    void set_enabled(bool on);

    // --- State
    std::vector<CompanionState> state;  // parallel to defs
    std::vector<int> members;           // indices into defs, in joining order (formation order)
    bool spawned_once = false;
    std::vector<PartyBolt> bolts;
    uint16_t avatar_target = 0;         // the last actor the Avatar hit
    struct Summon { uint16_t id = 0; uint32_t until = 0; };
    std::vector<Summon> summons;        // Morvain's skeletons

    // Travel banter between pairs of companions at banter points (game/banter.h). Always there;
    // does nothing until its data is loaded (main: data/banter.json, data/banter_points.json).
    std::unique_ptr<Banter> banter;

    void reset();         // new game: nothing spawned
    void pre_frame();     // before the kernel runs (collects answers / target clicks)
    void frame();         // after the kernel ran

    // --- Queries (all false / -1 when nothing is spawned)
    int index_of(uint16_t actor) const;  // def index of a companion actor, -1 if not one
    bool is_companion(uint16_t actor) const { return index_of(actor) >= 0; }
    bool in_party(int idx) const;
    bool party_member(uint16_t actor) const;  // the Avatar or a following / waiting companion
    // A companion never recruited: nobody can hurt them, monsters ignore them (PLAN-LEVELING.md 1;
    // not behind the Leveling option).
    bool untouchable(uint16_t actor) const;
    bool party_full() const { return int(members.size()) >= kMaxMembers; }
    int find_def(const std::string& id) const;
    int max_hp(const Actor& a) const;
    int max_mana(const Actor& a) const;

    // --- Commands
    bool spawn_all();                    // at the beach (or `here`: around the Avatar)
    void spawn_here();
    // Puts one companion beside the Avatar on his map (the debug menu's story jump). False: no spot.
    bool bring_to_avatar(int idx) { return idx >= 0 && size_t(idx) < state.size() && place_near_avatar(idx, false, true); }
    bool join(int idx);
    void wait(int idx);
    void follow(int idx);
    void part_ways(int idx);
    void open_inventory(int idx);
    // Milestone 6 controller binding: the party bar's slots (0 = the Avatar, then the members
    // in order), a highlighted slot (-1 none; drawn by the bar) and opening a slot's windows
    // (the Avatar's paperdoll, a companion's paperdoll and backpack).
    std::vector<uint16_t> bar_ids() const;
    int pad_highlight = -1;
    bool hud_panel = false;  // the HUD side panel shows the portraits: no party bar overlay
    void open_bar_slot(int slot);
    bool revive(int idx, int hp_percent = 50);
    void despawn_all();                  // option turned off
    void respawn_hidden();               // option turned on again

    // --- Homes (NOTES.md "Companions: homes"). Whoever has never joined rests at their home, or
    // on the beach while debug mode is on; anyone who has joined is left where they are.
    void set_debug_beach(bool on);       // from the Debug Mode option (live; not saved)
    bool beach_always = false;           // tests that use the beach: as if debug mode were on
    // own_home: their own home even while debug mode gathers the never-recruited on the beach.
    bool resting_place(int idx, unsigned& map, int pos[3], int& dir, bool own_home = false) const;  // false: nowhere valid
    void sync_homes();                   // puts the never-recruited at their resting places
    // Anyone dead on a map the Avatar is not on goes home alive, with their things, to be found and
    // recruited again; they say their "found_again" line when found (NOTES.md "Companions: safety").
    void send_dead_home();
    // Debug menu: every companion (in the party or not, alive or dead) leaves the party and is put at
    // their own home at once, alive and well. Returns how many went.
    int send_all_home();
    std::string where_text(int idx) const;  // for the debug menu

    // --- Hooks called from the shared engine code (all no-ops when disabled)
    // A usecode event aimed at a companion (look, use): handled here instead. Returns true if so.
    bool handle_event(uint16_t id, unsigned event);
    // Party members never collide with each other (the Avatar walks through companions) and,
    // during scenes, nobody collides with companions.
    bool passes_through(uint16_t mover, uint16_t other) const;
    // Two actors on the same side: no damage between them, no melee hits.
    bool same_side(uint16_t a, uint16_t b) const;
    // A monster's combat AI re-picks its target among party members (0 = keep its target).
    uint16_t monster_retarget(uint16_t monster, uint16_t current);
    void on_hit(uint16_t victim, uint16_t attacker, int damage);
    void on_avatar_theft();  // the Avatar took something that is not his (witnessed or not)
    void before_map_change(unsigned new_map);
    // Recolour table for a companion's world shape / paperdoll picture (nullptr = none).
    const uint8_t* remap(uint16_t actor) const;
    const uint8_t* doll_remap(uint16_t actor) const;
    // A companion with its own figure (art.paperdoll "own"): the whole paperdoll picture, the
    // original gump with its body panel cleared and the companion's standing frame drawn in
    // (party_gumps.cpp). nullptr = the usual recoloured body.
    const ShapeFrame* doll_picture(uint16_t actor) const;
    // A companion on the Avatar's paperdoll body: the painted head and where it lies in the
    // doll picture, drawn again over the equipment so a helmet's sprite does not hide the face
    // (the helmet stays clickable: the paperdoll picks items by their own sprites). Null if none.
    const ShapeFrame* doll_head_over(uint16_t actor, int& x, int& y) const;
    // A companion whose sheet shows equipment as icons (art.paperdoll_body).
    bool icon_sheet(uint16_t actor) const;
    // Why this companion may not wear or wield the item ("" = they may, or it is not equipment,
    // or the actor is not a companion): their gear class against the item's grade
    // (data/items/equipment.json). Items not in that table are allowed.
    std::string equip_refusal(uint16_t actor, uint16_t item) const;
    // Whether anything in data/items/equipment.json for that equip slot suits their gear class
    // (a caster has no helmet, arm guard, leggings or shield slot). True for unknown gear classes.
    bool slot_usable(uint16_t actor, unsigned slot) const;
    // The item's name from data/items/equipment.json ("" if it is not there).
    std::string item_label(unsigned shape, unsigned frame) const;
    // The party-bar picture of a companion with art.portrait (nullptr = the cropped frame).
    const ShapeFrame* bar_portrait(uint16_t actor) const;
    // An engine-made picture by its path under the data folder (cached; nullptr if it fails).
    const ShapeFrame* picture(const std::string& file) const;
    std::string display_name(uint16_t actor) const;

    // --- Conversation (companion_talk.cpp)
    void start_talk(int idx);
    bool talking() const { return talk_.idx >= 0; }
    int talk_companion() const { return talk_.idx; }
    void bark_trigger(int idx, const std::string& trigger, bool force = false);
    std::vector<const PartyTopic*> visible_topics(int idx) const;  // the current menu (tests)
    bool eval(int idx, const PartyCond& c) const;
    void end_talk();

    // --- Rogue and casters (companion_ai.cpp)
    bool pick_lock(int idx, uint16_t door, std::string& msg);
    bool steal_from(int idx, uint16_t victim, std::string& msg);
    bool cast(int idx, int spell, uint16_t target);  // starts the cast (animation, missile)
    bool spell_known(int idx, const std::string& id) const;
    // Leveling stage 5 (game/companion_magic.h): whether the spell may be cast now (unlocked, a use
    // left); why says why not. Always true with Leveling off. auto_target: an AI cast at this
    // target (0 = none): with few uses left, not at a trivial one.
    bool spell_ready(int idx, int spell, std::string* why = nullptr, uint16_t auto_target = 0) const;

    // --- Saving (savegame version 4)
    void save(SaveWriter& out) const;
    bool load(SaveReader& in);

    // --- The party bar (party_gumps.cpp)
    void update_bar();
    bool bar_wanted() const;

    World& world() { return w_; }
    const World& world() const { return w_; }
    std::vector<std::string> log;  // what happened (tests)

private:
    friend class PartyBarGump;
    friend class Banter;
    // party.cpp
    bool create_actor(int idx, int x, int y, int z, unsigned map);
    void relocate(int idx, unsigned map, const int pos[3], int dir);
    void build_remaps();
    bool find_spot(int near_x, int near_y, int near_z, uint16_t self, int& ox, int& oy, int& oz, int min_r = 32,
                   int max_r = 256, bool prefer_hidden = false) const;
    bool on_screen(int x, int y, int z, int margin = 16) const;
    // companion_ai.cpp
    void ai(int idx);
    void ai_idle(int idx, Actor& a);
    void ai_follow(int idx, Actor& a);
    bool ai_combat(int idx, Actor& a);
    // Puts a companion beside the Avatar: only on safe ground he could walk to from where he
    // stands, clear of the party (find_safe_spot); with `lenient` (arriving with him on a new map)
    // any dry, supported spot if there is no such one. `puff`: a puff of smoke there. False if no spot.
    bool place_near_avatar(int idx, bool hidden_only, bool lenient = false, bool puff = false);
    bool find_safe_spot(const Actor& a, int nx, int ny, int nz, int min_r, int max_r, bool prefer_hidden,
                        const std::vector<std::array<int, 3>>& taken, int& ox, int& oy, int& oz) const;
    // Walkable ground all the way from one point to another (no water, lava, gap or wall-high step).
    bool ground_between(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t self) const;
    // The Avatar stands still on safe ground (not falling, not over water / lava / an edge).
    bool avatar_settled() const;
    // Followers waiting at the edge of something they cannot cross come to him together, once
    // there is room for all of them beside him. True if they came.
    bool regroup(int idx);
    void formation_spot(int slot, int& x, int& y, int& z) const;
    bool walk_toward(int idx, Actor& a, int x, int y, int z, bool run);
    bool busy(uint16_t id) const;
    uint16_t pick_threat(int idx) const;
    bool in_melee_range(uint16_t id, uint16_t target) const;
    bool attack_has_hit_frame(unsigned shape) const;  // ANIM.DAT action 7 reaches (attack range > 0)
    bool adjacent(uint16_t id, uint16_t target, int gap) const;
    void step_bolts();
    void land_spell(int spell, uint16_t caster, uint16_t target, int x, int y, int z);
    void heal_actor(uint16_t caster, uint16_t target, int amount, const SpellDef& s);
    void check_deaths();
    void regen(int idx, Actor& a);
    void tick_summons();
    bool avatar_in_combat() const;
    // companion_talk.cpp
    void talk_frame();
    void talk_menu();
    void talk_say(const std::vector<std::string>& lines);
    void talk_choose(const PartyTopic& t);
    bool do_action(int idx, const std::string& action);
    const std::vector<std::string>* bark_lines(int idx, const std::string& trigger) const;
    void ambient_barks();
    struct Talk {
        int idx = -1;
        enum Stage { kNone, kSaying, kMenu, kTarget } stage = kNone;
        std::vector<std::string> queue;  // lines still to say
        uint16_t bark_pid = 0, ask_pid = 0, target_pid = 0;
        std::vector<uint16_t> answer_sids;
        std::vector<const PartyTopic*> answers;  // nullptr = Back / Bye
        std::vector<const std::vector<PartyTopic>*> menu_stack;
        std::string pending_action;   // after the lines: run this action
        std::string target_action;    // picklock / steal waiting for a target click
        bool end_after = false;
        int frames = 0;
    } talk_;
    uint32_t next_ambient_ = 0;
    uint16_t innocent_ = 0;
    uint16_t place_pack_ = 0, place_doll_ = 0;  // a companion's backpack window to put beside the paperdoll
    int place_tries_ = 0;
    void place_pack();  // a non-hostile the Avatar struck (a bark if it dies)
    bool critpath_tried_ = false;
    std::unique_ptr<CritPath> crit_;
    mutable std::vector<bool> crit_done_;
    mutable uint32_t crit_frame_ = 0xFFFFFFFF;
    bool step_done(const std::string& id) const;
    std::vector<std::array<uint8_t, 256>> remap_, doll_remap_;
    mutable std::map<int, std::unique_ptr<ShapeFrame>> doll_pictures_;
    mutable std::map<int, std::pair<int, int>> doll_head_at_;
    // data/items/equipment.json, read on first use.
    struct EquipInfo { std::string name, grade; };
    mutable bool equip_loaded_ = false;
    mutable std::map<uint32_t, EquipInfo> equip_info_;            // shape << 8 | frame
    mutable std::map<std::string, std::vector<std::string>> gear_armour_, gear_weapon_;
    void load_equipment() const;
    mutable std::map<std::string, std::unique_ptr<ShapeFrame>> pictures_;  // by file; null = failed
    World& w_;
    uint32_t start_frame_ = 0;
    bool debug_beach_ = false;  // Debug Mode is on (a setting, never saved)
    bool homes_dirty_ = false;  // sync_homes wanted (toggled, loaded): done at the next frame
    unsigned last_map_ = 0;
};

}  // namespace pg
