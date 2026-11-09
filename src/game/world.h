// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "data/palette.h"
#include "gfx/font.h"
#include "gfx/text_layout.h"
#include "game/game_log.h"
#include "game/gumps.h"
#include "game/sound.h"
#include "kernel/kernel.h"
#include "usecode/uc_machine.h"
#include "world/current_map.h"
#include "world/item.h"

namespace pg {

struct GameData;
class CameraProcess;
class SaveStore;
class SaveWriter;
class SaveReader;
class Party;
class Leveling;
class Creatures;
class Virtues;
class Spellbook;
class WorldStages;
class NpcDialog;
class TurnCombat;
class WorldSpace;
class NeighborStream;

// A world box (physics.md notation): x and y are the maximum corner, z the bottom, so it
// spans (x - xd, x] x (y - yd, y] x [z, z + zd).
struct Box {
    int x = 0, y = 0, z = 0, xd = 0, yd = 0, zd = 0;
};

// One contact found by the swept-box test (physics.md 3). Times are 0..0x4000.
struct SweepHit {
    uint16_t item = 0;
    int hit_time = 0, end_time = 0;
    bool touching = false, floor = false, blocking = false;
    unsigned dirs = 0;  // blocked axes: bit 0 x, 1 y, 2 z
};

// One mouse button as the Avatar controller sees it (avatar.md 1).
struct ButtonState {
    bool down = false;
    bool handled = true;            // the latest press has been used by some rule
    bool pair_used = false;         // the latest press completed a double-click
    uint32_t press_ms = 0, prev_press_ms = 0;  // prev 0: no earlier press to pair with
    uint32_t release_ms = 0;
    int press_x = 0, press_y = 0;   // screen point of the latest press
    // Records a press at time t (a press after a used double-click starts a new pair).
    void press(uint32_t t, int x, int y) {
        prev_press_ms = pair_used ? 0 : press_ms;
        press_ms = t;
        down = true;
        handled = false;
        pair_used = false;
        press_x = x;
        press_y = y;
    }
};

// Player input state, filled in by the game mode and read by the Avatar controller and the
// gravity process (avatar.md 1-6). Times are milliseconds of the game-mode clock.
struct PlayerInput {
    static constexpr uint32_t kDoubleClickMs = 400;
    uint32_t now_ms = 0;
    ButtonState button[3];         // 0 left, 1 middle, 2 right
    int mouse_x = 160, mouse_y = 100;
    int cursor_dir = 0;            // world direction 0..7 toward the cursor
    int cursor_len = 0;            // 0 step, 1 walk, 2 run
    bool key_forward = false, key_back = false, key_turn_left = false, key_turn_right = false;
    bool key_jump = false, key_run = false, key_step = false;
    // The world under the cursor (for the targeted jump): item and the point on its top.
    uint16_t cursor_item = 0;
    int cursor_point[3] = {0, 0, 0};
    uint32_t no_grab_until_ms = 0;  // just let go of a ledge: don't grab it again at once
    // Milestone 6 (engine design, not the original): the controller's left stick (a world
    // direction and 0 step / 1 walk / 2 run), its jump / attack / block, and commands buffered
    // while an animation that cannot be interrupted plays (Responsive mode; 0 = none).
    bool pad_move = false;
    int pad_dir = 0, pad_len = 1;
    bool pad_jump = false, pad_block = false;
    uint16_t pad_target = 0;        // the auto-target (attacks face it)
    uint32_t jump_buffer_ms = 0, attack_buffer_ms = 0;
    static constexpr uint32_t kBufferMs = 300;
    bool jump_held = false;         // Space, the pad's X or both mouse buttons are held (ledge grab)
    // The latest press came within the double-click time of the previous one and is unused.
    bool double_click(int b) const {
        const ButtonState& s = button[b];
        return !s.handled && s.prev_press_ms != 0 && s.press_ms - s.prev_press_ms <= kDoubleClickMs;
    }
    void use_double(int b) { button[b].handled = true; button[b].pair_used = true; }
    bool past_timeout(int b) const { return !button[b].handled && now_ms - button[b].press_ms > kDoubleClickMs; }
    // The mouse-movement flag (avatar.md 4.2): the right button is down and its press has been
    // consumed by movement (held past the double-click time). Set by the Avatar controller.
    bool mouse_moving = false;
    // Movement input is active (avatar.md 4.2, 5): mouse movement, a direction / step key or
    // the controller's stick.
    bool moving_input() const {
        return (button[2].down && button[2].handled && mouse_moving) || key_forward || key_back || key_step || pad_move;
    }
};

// Usecode event numbers (specs/usecode/events.md).
enum UcEvent : unsigned {
    kEvLook = 0x00, kEvUse = 0x01, kEvAnim = 0x02, kEvCachein = 0x04, kEvHit = 0x05, kEvGotHit = 0x06,
    kEvHatch = 0x07, kEvSchedule = 0x08, kEvRelease = 0x09, kEvEquip = 0x0A, kEvUnequip = 0x0B,
    kEvCombine = 0x0C, kEvCalledFromAnim = 0x0E, kEvEnterFast = 0x0F, kEvLeaveFast = 0x10, kEvCast = 0x11,
    kEvJustMoved = 0x12, kEvStole = 0x13, kEvGuardianBark = 0x15,
};

// Floating text above an item (specs/presentation/gumps.md 2.8).
struct Bark {
    uint16_t speaker = 0;
    uint16_t pid = 0;  // the notify process
    int font = 0;
    std::vector<TextPage> pages;
    unsigned page = 0;
    int ticks_left = 0;  // 60 Hz ticks for the current page
};

// A lightning bolt being drawn from the top of the screen onto a target (the Lightning spell).
// Rock Flesh on one actor (game/rock_flesh.h).
struct StoneSkin {
    int frames = 0;        // real-time frames left
    int turns = 0;         // turn-based rounds left
    int last_round = -1;   // the turn-based round last counted
    bool had_immortal = false;  // the actor was immortal before (left so at the end)
};

struct SkyBolt {
    uint16_t target = 0;
    int x = 0, y = 0, z = 0;  // the target's place, while it has one
    uint32_t seed = 0;
    int frames = 0;           // game frames left
};

// The answer menu (gumps.md 2.9).
struct AskMenu {
    uint16_t pid = 0;
    uint16_t list = 0;  // private copy of the answers
    std::vector<uint16_t> answers;
    struct Button { int x, y, w, h; };
    std::vector<Button> buttons;  // relative to the menu's top-left
    int w = 0, h = 0;
    int frames_open = 0;
};

// The game world: objects, the current map, the kernel and usecode, and the engine
// processes that drive them (specs/world/*.md).
class World : public UsecodeHost, public MapHooks {
public:
    explicit World(const GameData& data);
    ~World() override;

    // Starts a new game (specs/world/startup.md section 2-3).
    void new_game();
    // One rendered frame: two kernel passes, then the fast area, map-item animation and
    // the UI timers.
    void frame();

    const GameData& data() const { return data_; }
    Kernel& kernel() { return kernel_; }
    UCMachine& vm() { return vm_; }
    CurrentMap& map() { return map_; }
    ObjectTable& objects() { return objs_; }

    Item* item(uint16_t id) const { return objs_.get(id); }
    Actor* actor(uint16_t id) const { return objs_.actor(id); }
    Actor* avatar() const { return objs_.actor(1); }

    // --- Usecode events (events.md): returns the new pid, 0 if no handler.
    int usecode_class(const Item& it) const;
    uint16_t call_event(uint16_t id, unsigned event, const std::vector<uint8_t>& args = {});
    uint16_t call_event_w(uint16_t id, unsigned event, uint16_t a);
    uint16_t call_event_ww(uint16_t id, unsigned event, uint16_t a, uint16_t b);

    // --- Geometry
    void footprint(const Item& it, int& xd, int& yd, int& zd) const;
    // Absolute world location (the outermost container's for contained items).
    void location(const Item& it, int& x, int& y, int& z) const;
    const Item* root_container(const Item& it) const;
    bool is_container(const Item& it) const;

    // Static placement test (physics.md 2): would box (x - xd, x] x (y - yd, y] x [z, z + zd)
    // with flags `mover_flags` collide with something? `ignore` is skipped.
    // `start`: items overlapping this box are not blockers (an animation that began inside
    // something may finish). support_z is the highest solid/land/roof top under the box
    // (-1000 if none), land the land/roof item under the bottom centre.
    struct Placement {
        bool valid = true, supported = false;
        int support_z = 0;
        uint16_t blocker = 0, land = 0;
    };
    Placement can_exist(int x, int y, int z, int xd, int yd, int zd, uint16_t mover_flags, uint16_t ignore,
                        const Box* start = nullptr) const;
    Placement can_exist_shape(unsigned shape, int x, int y, int z, uint16_t ignore) const;
    Box box_of(const Item& it) const;
    uint16_t shape_flags(const Item& it) const;

    // --- Physics (physics.md 3-5, 7, 10; physics.cpp)
    // Swept-box test from `start` to the point (ex, ey, ez); contacts sorted by hit time.
    void sweep(const Box& start, int ex, int ey, int ez, uint16_t mover_flags, uint16_t self, bool blocking_only,
               std::vector<SweepHit>& out) const;
    // Where the swept box stops against contact h (blocked faces put flush with the obstacle).
    void contact_point(const Box& start, int ex, int ey, int ez, const SweepHit& h, int& x, int& y, int& z) const;
    // Moves an item with collision (physics.md 4). Returns the fraction moved (0x4000 = all).
    int collide_move(uint16_t id, int x, int y, int z, bool teleport, bool force, uint16_t* blocker = nullptr,
                     unsigned* dirs = nullptr);
    // Reach with line of sight (physics.md 5). The point form uses `box` placed at the point.
    bool can_reach(uint16_t from, uint16_t target, int range) const;
    bool can_reach_box(uint16_t from, const Box& target, uint16_t target_id, int range, uint16_t ignore = 0) const;
    // Throwing and falling (physics.md 7). hurl adds to an existing fall. Returns the pid.
    uint16_t hurl(uint16_t id, int vx, int vy, int vz, int gravity);
    uint16_t fall(uint16_t id);
    // Missile launch velocity (physics.md 10); false if the path check (when asked) fails.
    void missile_velocity(int fx, int fy, int fz, int tx, int ty, int tz, int speed, int gravity, int& vx, int& vy,
                          int& vz, int& frames) const;
    bool missile_path_clear(uint16_t id, int fx, int fy, int fz, int tx, int ty, int tz, int speed, int gravity,
                            uint16_t thrower) const;
    // Resting relations (physics.md 8): items lying on top of `id` (recursive) / directly under.
    void items_on_top(uint16_t id, std::vector<uint16_t>& out, bool recursive) const;
    // Pick-up side effects (physics.md 8): things on top fall, things below get release.
    void grab(uint16_t id);
    // Ascend (intrinsic 0x02B); true if the full move happened.
    bool ascend(uint16_t id, int dz);

    // --- Combat (combat.md; combat.cpp)
    void receive_hit(uint16_t victim, uint16_t attacker, int dir, int damage, uint16_t type);
    void kill_actor(uint16_t id, uint16_t damage_type);
    void explode(uint16_t id, bool destroy_item, bool damage);
    // Stat training (avatar.md 9): 0 STR, 1 INT, 2 DEX.
    void train_stat(int which, int points);
    std::array<int, 3> stat_accumulator{0, 0, 0};
    uint32_t last_attack_frame = 0;
    // The equipped item in an actor's slot 1..7 (7 = backpack), 0 if none.
    uint16_t equipped(uint16_t actor, unsigned slot) const;
    // Avatar death (avatar.md 11): set when the gravestone was closed (the game mode restarts).
    bool game_over = false;
    uint16_t death_pid = 0;
    // Fired the "avatar stole something" event at witnesses if `id` is not owned (avatar.md 8.6).
    void theft_check(uint16_t id);
    // Weight (items.md 4) and carrying limits (avatar.md 8.5).
    uint32_t item_weight(const Item& it) const;
    uint32_t total_weight(const Item& it) const;
    unsigned item_volume(const Item& it) const;
    // Puts an item into the Avatar's backpack if weight and volume allow.
    bool give_to_avatar(uint16_t id);

    // --- Player input (avatar.md; read by the Avatar controller)
    PlayerInput input;
    // Control settings (milestone 6, the engine's own design; not saved with the game).
    // The World defaults are Classic (the faithful behaviour the tests check); the game
    // application loads the player's settings, where Responsive is the default.
    struct Controls {
        bool responsive = false;     // see PLAN-M6.md "Responsive and Classic control modes"
        bool smooth_scroll = false;  // render-rate camera interpolation
        bool click_text = true;      // a click / Space / Enter / pad A ends a bark page
        int zoom_step = 0;           // 0 = 1x; n = the world drawn n output pixels smaller
        bool assist = true;          // Responsive: movement assist, edge guard, passable open doors
        bool easy_ledges = true;     // Responsive: holding jump finds, lines up with and climbs any ledge in reach
    } controls;
    // The area the fast area must cover, in screen pixels (bigger when the view is zoomed out).
    int view_w = 320, view_h = 200;
    bool debug_assist = false;  // log the movement-assist probes (--debug-assist)
    // Debug menu (app/debug_tools.h): the Avatar walks through walls (floors still hold him).
    bool debug_noclip = false;
    // Maps the Avatar has been on (the critical path's visited_map; saved since version 3).
    std::set<unsigned> visited_maps;
    // The stored nonfixed items of a map that is not loaded (nullptr if none).
    const std::vector<MapEntry>* stored_items(unsigned map) const {
        return map < nonfixed_.size() ? &nonfixed_[map] : nullptr;
    }
    // Doors (milestone 6, Responsive mode): the door shapes are those of the usecode classes
    // named *DOOR* (DOOR_NS 68, DOOR_EW 69, TDOOR_NS 340, TDOOR_EW 342); quality bit 0x100 is
    // set while a door is open (observed in the data: every use toggles it while the shape
    // turns between the NS and EW variant). An open door has no collision in Responsive mode.
    bool is_door_shape(unsigned shape) const;
    bool door_is_open(const Item& it) const { return is_door_shape(it.shape) && (it.quality & 0x100); }
    bool passable_door(const Item& it) const { return passable_door_rules() && door_is_open(it); }
    bool passable_door_rules() const { return controls.responsive && controls.assist; }
    // Responsive mode: actors left inside a closed door's volume are moved out (no trapping).
    void free_actors_from_doors();
    // Screen point of a world point under the current view (zoom, smooth camera); set by the
    // game mode. Without it screen_point uses the plain 320 x 200 projection.
    std::function<void(int x, int y, int z, int& sx, int& sy)> screen_mapper;
    uint16_t mover_pid = 0;

    // --- Moving and creating items (items.md 5, 6, 10)
    // Plain move into the world, no collision (intrinsic 0x03E rules).
    void move_to(uint16_t id, int x, int y, int z, bool events = true);
    // Into a container (no weight/volume check unless `check`). False if refused.
    bool move_to_container(uint16_t id, uint16_t cont, bool check = false, bool at_end = true);
    void push_ether(uint16_t id);
    uint16_t ether_top() const { return ether_.empty() ? 0 : ether_.back(); }
    uint16_t ether_pop();
    // Takes an item out of its container / chunk / the ether (it stays an object).
    void detach(uint16_t id);
    uint16_t create_item(unsigned shape, unsigned frame);
    uint16_t create_monster(unsigned shape, unsigned frame);
    // Destroys an item (and its processes). deferred: through a destroy-item process.
    void destroy(uint16_t id, bool deferred = true);
    void destroy_now(uint16_t id);
    void destroy_contents(uint16_t id);

    // --- Map changes and teleporting (items.md 9)
    bool change_map(unsigned map);
    bool teleport_to_egg(unsigned map, unsigned egg_id);
    // Actor teleport (intrinsic 0x09E).
    void teleport_actor(uint16_t id, int x, int y, int z, unsigned map);

    // --- Camera (kernel.md 4, intrinsics/world.md)
    void camera_follow(uint16_t id);
    void camera_stay(int x, int y, int z);
    uint16_t camera_scroll(int x, int y, int z, int frames);
    void camera_point(int out[3]) const;
    uint16_t camera_followed() const;
    int quake = 0;  // earthquake strength (0 = none)
    int engine_quake = 0;  // frames left of a quake the engine started (game/variants.cpp); its quake is not saved
    std::array<int, 2> quake_offset{0, 0};
    // Item powers (game/item_powers.h): frames of cooldown left per ability, the power to fire
    // this frame, the frames and strength of the shake a power started.
    std::map<std::string, uint32_t> power_cooldown;
    std::string power_pending;
    int power_quake = 0, power_quake_strength = 0;

    // --- Time (time.md)
    uint32_t game_frames() const { return kernel_.frame_number() + time_offset_; }
    uint32_t game_seconds() const { return game_frames() / 30; }
    void set_game_hours(unsigned h);

    // --- Avatar state
    bool stasis = false;
    std::string avatar_name = "Avatar";
    bool cheats = false, cheated = false;

    // --- Animation and AI helpers (actors.md 7)
    // Starts an animation process on an actor; returns its pid (0 if none).
    uint16_t do_anim(uint16_t actor, unsigned action, int dir, int steps = 0);
    uint16_t set_activity(uint16_t actor, unsigned activity);
    uint16_t pathfind(uint16_t actor, int x, int y, int z, uint16_t target, int range);
    void set_in_combat(uint16_t actor);
    void clear_in_combat(uint16_t actor);

    // --- UI state (drawn by the game mode)
    uint16_t bark(uint16_t speaker, const std::string& text);
    // owner / asker: the item and process of the usecode conversation asking (0 = an engine menu).
    uint16_t ask(uint16_t list, uint16_t owner = 0, uint16_t asker = 0);
    uint16_t open_reader(ReaderGump::Kind kind, uint16_t item, unsigned gump_shape, const std::string& text);
    uint16_t target_pick();
    std::vector<Bark> barks;
    std::unique_ptr<AskMenu> ask_menu;
    uint16_t target_pid = 0;  // targeting cursor up
    // The Lightning spell's picture (not saved): the bolts now in the air, and the flash when one
    // lands (the renderer shows it for exactly one frame, then clears it).
    std::vector<SkyBolt> sky_bolts;
    int sky_flash = 0;
    // Idle talk (the user's request, 2026-11-06): one line in this many is spoken, of the NPCs'
    // ambient scripts and of the companions' idle remarks. 1 = all of them, as in the original.
    static constexpr int kBanterOneIn = 5;
    int banter_one_in = kBanterOneIn;
    // Intrinsic item_bark: is this an idle line from an NPC's ambient script (never a conversation,
    // scene or quest line) that is to go unsaid? Then the script gets a silent wait as long as the
    // line would have lasted, so it runs on as if it had spoken: returns that process, else 0.
    uint16_t skip_banter(const UCProcess& proc, uint16_t speaker, const std::string& text);
    void close_bark(uint16_t speaker);
    void choose_answer(unsigned index);
    // The conversation in progress (NOTES.md "Combat ends a conversation"): the usecode process
    // that opened the latest answer menu and its item (the one talked to), and the process that
    // last turned stasis on. Not saved (a save is refused while the menu is up).
    uint16_t talk_pid = 0, talk_item = 0, stasis_pid = 0, stasis_cls = 0;
    bool in_conversation() const;  // that process still runs, an answer menu is up, or a companion talks
    // A fight starts (`cause`: the hostile, 0 = the player or the engine): a conversation in
    // progress ends at once, like walking away: the answer menu and the speech close, its usecode
    // process ends, stasis it set is lifted, a companion's talk ends. Not when the conversation's
    // own script starts the fight (it runs on, the scene's own fight). True if one was ended.
    bool combat_interrupts_conversation(uint16_t cause, const char* why);
    void end_conversation(const char* why);
    unsigned conversations_ended = 0;  // tests
    void click_bark(uint16_t speaker);
    // The open reader (book, scroll, plaque, grave), if any.
    ReaderGump* reader() const { return gumps.find<ReaderGump>(); }

    // --- Gumps (specs/presentation/gumps.md)
    Gumps gumps{*this};
    // Opens / closes an item's container window (intrinsics 0x04C / 0x04D, dead bodies).
    void open_gump(uint16_t item, unsigned gump_shape);
    void close_gump(uint16_t item);
    // Use with the reach rule (avatar.md 7): non-actors need reach 128, else the red cross;
    // dead bodies toggle their gump.
    void use_item(uint16_t id);
    int red_cross_frames = 0;  // the out-of-reach cursor is shown while > 0
    // Container rules without moving (items.md 5, avatar.md 8.5): nesting, volume, weight.
    bool can_put_in(uint16_t id, uint16_t cont) const { return put_in_refusal(id, cont).empty(); }
    // Why an item cannot go into a container ("" = it can): "Too heavy to carry (452 of 440)",
    // "Not enough room in there", ... (the engine's own wording; shown beside the red cross).
    std::string put_in_refusal(uint16_t id, uint16_t cont) const;
    // The red cross with a reason the game mode shows beside the pointer for a moment.
    void refuse(const std::string& why) { red_cross_frames = 8; if (!why.empty()) { refuse_text = why; refuse_frames = 75; } }
    std::string refuse_text;
    int refuse_frames = 0;
    // Paperdoll equipping (avatar.md 8.5): one item per equip type; a taken slot is swapped
    // into the backpack.
    bool can_equip(uint16_t actor, uint16_t id) const;
    bool equip(uint16_t actor, uint16_t id);
    // Screen point of an item's (outermost container's) position under the current camera.
    void screen_point(const Item& it, int& sx, int& sy) const;
    // The main menu / name entry / diary book.
    MenuGump* open_menu(MenuGump::Mode mode);
    // Things the gumps ask the game mode to do between frames (new game, load, save, quit).
    struct Request {
        enum Kind { kNone, kNewGame, kLoad, kSave, kQuit } kind = kNone;
        int slot = 0;               // kNewGame: 1 = diary entry 1 (no intro), kNewGameIntro = "New Game"
        std::string description;
        static constexpr int kNewGameIntro = -1;
    } request;
    SaveStore* saves = nullptr;  // owned by the game mode
    // Saving is allowed: no modal gump other than `except`, no process that prevents it.
    bool can_save(const Gump* except = nullptr) const;
    PaletteTransform menu_saved_palette;
    // Options page values (gumps.md 2.3). Text speed 0..9 maps to talkSpeed 10 + 10 * value.
    struct Options {
        bool music = true, sfx = true, steps = true, animations = true;
        int text_speed = 5;
        bool frame_skip = false, speed_limit = true;
        int music_volume = 255, sfx_volume = 255;  // 0..255 (not in the original's page)
    } options;
    // Reads / writes the options from / to the persistent settings (core/config.h).
    void load_options();
    void store_options();
    // Sound effects, speech and music (game/sound.h).
    SoundSystem sound{*this};
    int talk_speed() const { return 10 + 10 * options.text_speed; }
    // New game: show the name entry book after the opening is set up (startup.md 3 step 4).
    bool ask_name = false;
    // New game: the startup process plays the intro movie first (startup.md 3 step 1).
    bool play_intro = false;
    // The intro is wanted for a new game chosen from the start menu ("New Game"): off in
    // headless runs and with --no-intro (app/main.cpp). Diary entry 1 never replays it.
    bool intro_on_new_game = false;
    // Movies (game/movie.h): from_menu wraps them in the fade transition; the new-game intro
    // keeps its music for the game, the endgame opens the main menu when it ends.
    enum MovieKind { kMovieIntro, kMovieEndgame };
    uint16_t play_movie(MovieKind kind, bool from_menu);  // the notify pid, 0 if no movie
    // Extra options-page rows other features add (each builds its row for this world).
    using OptionRowFactory = std::function<MenuGump::OptionRow(World&)>;
    static std::vector<OptionRowFactory>& extra_option_rows();
    // Main-menu entries the game mode provides (milestone 6): the world map and the controls
    // help page. Empty when there is no game mode (tests).
    std::function<void()> open_map_ui, open_help_ui;
    // Takes an item from someone else's container into the Avatar's backpack (the game mode: weight
    // check, stacks merged, theft noticed; the red cross with the reason if it cannot). Set by GameMode.
    std::function<bool(uint16_t item)> take_item_ui;
    // The options page's Zoom row (the game mode's zoom levels; empty = no row).
    std::function<std::string()> zoom_value_ui;
    std::function<void(int dir)> zoom_change_ui;
    // Easy ledges (game/ledge.h): a short hint for the player ("No ledge in reach"), shown by
    // the game mode; and the ledge the held jump is going for, highlighted for a few frames.
    std::function<void(const std::string&)> hint_ui;
    struct LedgeHint {
        int x = 0, y = 0, z = 0, dir = 0;
        int frames = 0;
    } ledge_hint;

    // Companions (game/party.h; the engine's own design). Always present; does nothing unless
    // party->enabled (off by default: the original game).
    std::unique_ptr<Party> party;
    std::unique_ptr<Leveling> leveling;  // levels and XP (game/leveling.h); always there, off in a bare World
    std::unique_ptr<Creatures> creatures;  // the new creatures (game/creatures.h); always there, off in a bare World
    std::map<uint16_t, StoneSkin> stone_skin;  // Rock Flesh effects by actor (game/rock_flesh.h)
    std::unique_ptr<Spellbook> spellbook;  // the Avatar's spellbook (game/spellbook.h); always there
    std::unique_ptr<Virtues> virtues;  // the Avatar's virtue scores (game/virtues.h); always there, off in a bare World
    std::unique_ptr<WorldStages> stages;  // Pagan transformed as the virtues grow (game/world_stages.h)
    // Engine-side dialogue added to original NPCs (game/npc_dialog.h; data/dialog/*.json). Always
    // present; does nothing unless dialog->enabled (off by default: the original game).
    std::unique_ptr<NpcDialog> dialog;
    // Turn-based combat (game/turn_combat.h; the engine's own design). Always present; off
    // (real time) unless `turn->enabled`, which the game application sets from the settings.
    std::unique_ptr<TurnCombat> turn;
    // The HUD's message and combat logs (game/game_log.h, PLAN-HUD.md). Not saved.
    GameLog game_log;
    // Both actors are on the party's side (the Avatar, companions, their summons).
    bool same_party(uint16_t a, uint16_t b) const;
    // The mover passes through actor `other` (party members, companions during scenes).
    bool party_ghost(uint16_t mover, uint16_t other) const;

    // --- The stitched world (game/world_stitch.cpp, world/world_space.h; PLAN-WORLD-EDITOR.md
    // section 1; the engine's own design). The live game always runs one map in its own 16-bit
    // coordinates, as the original does, so usecode sees exactly the original state; the
    // stitched world adds: a world point for every (map, point) from world_layout.json, the
    // neighbouring maps across a seam drawn around the current one, and seam crossings (an
    // original teleporter between two seam maps, or walking off the edge of the current map's
    // data onto its neighbour's) that keep the Avatar's world point instead of jumping to the
    // arrival egg. A crossing is still a full map change (items.md 9: stored nonfixed items,
    // killed processes, cachein and schedule events), so every script sees what it expects.
    // Null = Original maps (byte-identical to the game without this feature).
    std::unique_ptr<WorldSpace> stitch;
    std::unique_ptr<NeighborStream> neighbors;
    bool stitched() const { return stitch != nullptr; }
    unsigned map_number_now() const { return map_.map_number(); }
    // Turns the stitched world on (loads the layout file; false and Original if it cannot) or off.
    bool set_world_mode(bool stitched_on, const std::string& layout_path);
    std::string world_layout_path;  // set by the application (engine/data/world_layout.json)
    // The options-page row "World: Stitched / Original maps" (setting "world_mode").
    static void register_world_options();
    // The options-page row "Original bug fixes: On / Off" (setting "bug_fixes", default On;
    // the flag is pg::original_bug_fixes() in core/bugfix.h, shared by every bug fix).
    static void register_bugfix_option();
    static bool stitched_by_default();
    // The Avatar's world point (stitched) or his map point (original).
    void avatar_world_point(int32_t out[3]) const;
    // A seam crossing moved the current map's coordinates: a point of the old map + (dx, dy, dz)
    // is the same world point in the new one (the view shifts its smoothing state by it).
    struct RegionShift { int dx = 0, dy = 0, dz = 0; unsigned from = 0, to = 0; uint32_t serial = 0; } region_shift;
    // Crosses into seam partner `to` keeping the Avatar's world point (egg_id: the teleporter's
    // arrival egg to fall back on if the point is blocked there, -1 none). False if not possible.
    bool cross_seam(unsigned to, int egg_id);
    // After a crossing: the usecode eggs round the arrival egg the original teleport would have put
    // him on (ids: the teleporters from the old map into this one) hatch as if he stood there.
    void hatch_arrival_eggs(const std::vector<unsigned>& ids, int egg_id);
    struct SeamStats { unsigned crossings = 0, edge_crossings = 0, fallbacks = 0; double last_ms = 0, max_ms = 0; } seam_stats;

    // --- Falling out of the world (Original bug fixes; game/void_rescue.cpp)
    // Where the Avatar last stood on real ground (a LAND / ROOF / SOLID item under his feet,
    // inside the map's scenery) and how many frames he has now been "in the void": come to rest
    // where the map has nothing under him (at z 0 below a hole, or off the edge of the map's
    // scenery, or in stitched mode past a seam or a segment edge with no ground beyond).
    struct VoidRescue {
        bool valid = false;
        unsigned map = 0;
        int x = 0, y = 0, z = 0;
        int frames = 0;          // consecutive frames in the void
        unsigned rescues = 0;    // for tests / the log
        int last[3] = {0, 0, 0}; // where he was last frame (a jump of more than 128 is a teleport)
        unsigned last_map = 0;
    } void_rescue;
    static constexpr int kVoidRescueFrames = 15;  // half a second of standing in nothing
    // True if the actor stands (or would land) on nothing the map has: no item at all under
    // his footprint at or below his feet and z 0, or outside the map's scenery.
    bool in_void(const Actor& a) const;
    void void_rescue_frame();
    // Clamp for physical moves: the chunk grid is 0..32767 (items outside it are in no chunk).
    static constexpr int kMapMax = 32767;

    // --- Savegames (formats/savegame.md; the engine's own format, savegame.cpp)
    void save_game(SaveWriter& out, const std::string& description) const;
    // Replaces the whole world state; false (and a fresh new game) if the data is bad.
    bool load_game(SaveReader& in);
    // Headless convenience: answer menus and readers close by themselves after a while.
    bool auto_ui = false;
    // The answers auto_ui has given (shared by every World of the process, as it always was;
    // a test comparing two runs clears it before each).
    static std::vector<std::string>& auto_ui_given();
    // Debug: start without the opening scene (no FIRST egg; the Avatar is free at once).
    bool skip_opening = false;
    const ShapeFont& font(int n) const;

    // --- Palette (intrinsics/ui.md palette effects)
    PaletteTransform palette;           // what is drawn now
    PaletteTransform palette_target;    // the "current transform" fades return to
    unsigned palette_xform = 0;
    uint16_t fade_pid = 0;
    int fade_priority = 0;
    uint16_t start_fade(const PaletteTransform& to, int frames, int priority, const PaletteTransform* from = nullptr);
    int invert_state = 0;  // screen inversion 0..0x8000

    // --- Randomness
    int rand_below(int n);
    std::mt19937 rng{12345};

    // --- Searches (loops.md, items.md 11)
    void loop_search(int type, uint16_t a, uint16_t b, const uint8_t* script, unsigned len,
                     std::vector<uint16_t>& out) override;
    bool object_exists(uint16_t id) const override { return objs_.get(id) != nullptr; }
    bool eval_criteria(const Item& it, const uint8_t* script, unsigned len) const;
    // The box is cx - cxd - range .. cx + range (and likewise y): (cxd, cyd) is the centre item's
    // footprint for a usecode loop (loops.md 1.1), 0 for a plain point.
    void area_search(int cx, int cy, int range, bool recurse, std::vector<uint16_t>& out, int cxd = 0, int cyd = 0) const;
    void surface_search(uint16_t ref, bool above, std::vector<uint16_t>& out) const;

    // MapHooks
    void item_enter_fast(Item& it) override;
    void item_leave_fast(Item& it) override;
    void destroy_item(uint16_t id) override { destroy_now(id); }

    // Egg hatching (items.md 8.1), run by the egg hatcher process.
    void check_eggs();
    bool just_teleported = false;
    // The fast area has been computed at least once since the last map load.
    bool fast_ready() const { return fast_init_; }

    bool trace_events = false;  // log every usecode event started
    // Statistics for tests / logs.
    uint64_t events_started = 0;
    unsigned startup_state = 0;
    std::vector<std::string> log_lines;

private:
    void load_map_items(unsigned map, bool cachein);
    void stitch_frame();  // world_stitch.cpp: pending / edge crossings, the neighbours' scenery
    bool stitch_portal(const Item& egg) const;
    int pending_cross_ = -1, pending_cross_egg_ = -1;
    // Where the Avatar last stood on the current map's own scenery (an edge crossing needs him to
    // have walked off it: a teleport or a script putting him on bare ground never crosses).
    int edge_last_[3] = {0, 0, 0};
    int edge_last_map_ = -1;
    void reset_state();
    void save_map_items();
    void animate_items();
    void update_ui();
    void create_npcs();
    std::unique_ptr<Actor> make_npc(const struct NpcRecord& n) const;

    const GameData& data_;
    Kernel kernel_;
    UCMachine vm_;
    ObjectTable objs_;
    CurrentMap map_;
    std::vector<std::vector<MapEntry>> nonfixed_;  // stored nonfixed items of every map
    std::vector<uint16_t> ether_;
    uint16_t camera_pid_ = 0;
    uint32_t time_offset_ = 0;
    int prev_cam_[3] = {0, 0, 0};
    bool fast_init_ = false;
    std::vector<ShapeFont> fonts_;
    mutable std::vector<int8_t> door_shapes_;  // per shape: -1 unknown, 0 no, 1 door
    friend class CameraProcess;
    friend class StartupProcess;
};

void register_intrinsics(World& w);

}  // namespace pg
