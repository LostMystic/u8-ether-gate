// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Turn-based combat (PLAN-TURNBASED.md; the engine's own design, not in the original game).
//
// When a fight starts (combat mode with a hostile in the encounter area, or being hit with
// "Turns When Hit" on) the world freezes: the kernel's hold filter stops every process that
// existed at the freeze, except the camera, windows, egg hatching, death and the processes of
// the character whose turn it is (and things that character's action creates: hit reactions,
// missiles, the die animation, usecode the action starts). Each round the fighters act in DEX
// order (highest first, ties at random): party members through the action menu (app/turn_ui.*)
// or on Auto, monsters through the AI below, which makes the same choices as the real-time
// combat AI (target, approach, attack). The fight ends in victory (no hostile left), defeat
// (the Avatar died: the normal death sequence and death menu), escape (every hostile is out of
// the encounter area of every party member) or a fall back to real time (scripted fights).
//
// Real time (the World default, so every older test is unchanged) never starts a fight and
// every hook below returns at once.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "game/anim_tracker.h"
#include "game/pathfinder.h"

namespace pg {

class World;
class Actor;
class Process;

class TurnCombat {
public:
    explicit TurnCombat(World& w);
    ~TurnCombat();

    // --- Settings (persistent, core/config.h keys combat_style / turn_on_hit / turn_speed; not
    // part of a savegame). `enabled` false = real time.
    bool enabled = false;
    bool start_on_hit = true;
    int speed = 0;  // 0 normal, 1 fast, 2 fastest (enemy and Auto turns)
    static void register_options();  // the options-page rows (once)
    void load_settings();            // from Config (players get turn-based by default)

    // --- Tunables
    // World units a Move may cover: 300 (about 37 feet; the user asked for 3-4x the first 80).
    static constexpr int kMoveRange = 300;
    static constexpr int kAreaMargin = 64;     // encounter area: the screen plus this (game pixels)
    // A turn that is not over after this long ends with "<name> missed its turn" (the user's
    // limit: 15 s, at the game's 30 frames a second; the speed-up does not shorten it, and the time
    // the player is asked something or the game is paused does not count).
    static constexpr int kTurnTimeoutFrames = 15 * 30;
    static constexpr int kNoticeFrames = 60;   // how long that message stays on screen
    // The turn ends by itself after a Move when nothing useful is left (has_followup): the
    // character then defends, the one thing left that costs nothing and helps.
    static constexpr bool kAutoEndDefends = true;
    bool auto_end = true;  // the rule is on (tests may switch it off)
    static constexpr unsigned kMaxNodes = 6000; // reachable-area search limit

    // --- Hooks from the shared engine code
    void pre_frame();  // World::frame, before the kernel: starting, the hold snapshot
    void frame();      // World::frame, after the kernel and the party: the turn machine
    // A hit landed (World::receive_hit, after the damage calculation): may start a fight,
    // Defend halves the damage. Returns the damage to apply.
    int on_hit(uint16_t victim, uint16_t attacker, int damage);
    bool hold(const Process& p) const;      // the kernel's hold filter
    bool holds_ai(uint16_t actor) const;    // companions' real-time AI is off for this actor
    bool active() const { return phase_ != kIdle; }

    // --- The fight
    struct Fighter {
        uint16_t id = 0;
        bool party = false;       // the Avatar, a companion, a summoned ally
        bool controllable = false;
        int dex = 0, tiebreak = 0;
        bool waited = false, defending = false, keep_auto = false;
    };
    const std::vector<Fighter>& order() const { return order_; }
    int current_index() const { return idx_; }
    uint16_t current() const;
    int round() const { return round_; }
    bool is_fighter(uint16_t id) const;
    bool is_party_fighter(uint16_t id) const;
    // The current fighter is a party member waiting for the player's command.
    bool awaiting_player() const { return phase_ == kTurn && sub_ == kChoose; }
    bool moved() const { return moved_; }
    bool party_auto = false;  // everyone in the party acts by themselves
    void toggle_party_auto();
    void toggle_keep_auto(uint16_t id);
    bool keep_auto(uint16_t id) const;
    // A short line about what happens now ("Brannoc attacks the ghoul", "Out of reach").
    std::string status;
    // A brief on-screen message that outlives the next turn's status ("Ghoul missed its turn").
    std::string notice;
    int notice_frames = 0;
    int timeouts = 0;  // turns ended by the time limit (tests)

    // --- Movement: the reachable area of the current fighter (Dijkstra over walk steps).
    struct Node {
        ActorState st;
        int parent = -1;
        PathStep step;
        int cost = 0;
    };
    const std::vector<Node>& reach();
    // The nodes from the start to `node` (inclusive), start first.
    void chain(int node, std::vector<int>& out) const;
    // A node's centre at the feet (world units).
    void node_point(int node, int& x, int& y, int& z) const;

    // --- What the menu offers
    std::vector<uint16_t> enemies_of(uint16_t id) const;  // living opponents in the fight, nearest first
    bool can_hit_now(uint16_t id, uint16_t target) const;
    bool can_hit_from(uint16_t id, const ActorState& s, uint16_t target) const;
    bool attack_reachable(uint16_t target);  // now, or after a move this turn
    // Could the current fighter still do something useful in this turn without moving? An attack
    // on a foe in reach, a spell it can pay for that has a target in range (or needs none), an
    // item worth using (potion or food only when hurt, a power or flask always). Defend, Wait and
    // End turn do not count.
    bool has_followup(uint16_t id);
    struct CastOption {
        std::string name;
        int spell = -1;       // companion spell (Party::spells index)
        uint16_t item = 0;    // (unused since the spellbook: the Avatar's list is the book's, `book`)
        int book = -1;        // the Avatar's spellbook spell (Spellbook::Spell::key), cast through the book
        int mana = 0;
        bool friendly = false;  // targets the party (heals)
        bool self = false;      // no target needed
        int range = 448;
        // Leveling stage 5 (game/companion_magic.h): a spell shown but not castable now (greyed,
        // with the reason), and the uses left (-1 = not counted).
        bool locked = false;
        std::string why;
        int uses = -1;
    };
    std::vector<CastOption> cast_options(uint16_t id) const;
    bool cast_reaches(uint16_t id, const CastOption& o, uint16_t target) const;  // range and sight line
    std::vector<uint16_t> use_items(uint16_t id) const;

    // --- Commands for the current fighter (from the UI or tests); false if refused (status says why)
    bool cmd_move(int node);
    bool cmd_attack(uint16_t target);
    bool cmd_cast(const CastOption& o, uint16_t target);
    bool cmd_use(uint16_t item);
    bool cmd_defend();
    bool cmd_wait();
    bool cmd_auto(bool keep);
    bool cmd_end_turn();

    // --- Pacing: world frames to run this game frame (enemy / Auto turns run faster).
    int world_frames() const;
    bool speed_held = false;  // the UI's speed-up button is held

    // --- Results (tests, the log)
    enum End { kNone, kVictory, kDefeat, kEscape, kFallback, kAborted };
    End last_end = kNone;
    int fights = 0;
    std::vector<uint16_t> turn_history;    // fighter ids in the order their turns began
    std::vector<int> turn_rounds;          // the round of each entry
    std::vector<std::string> log;
    // Scripted fights that stayed in real time (the actor and why), for the report and tests.
    struct FallbackNote { uint16_t id; unsigned shape; std::string why; };
    std::vector<FallbackNote> fallbacks;
    bool is_scripted(uint16_t id, std::string* why = nullptr) const;
    // Ends the fight now (tests, the option turned off): the world runs on in real time.
    void end_fight(End why);
    // Starts a fight now if anything hostile is in the area (false if not or scripted).
    bool start_fight(const char* reason);
    // Hostiles in the encounter area around the party (not yet in the fight).
    std::vector<uint16_t> hostiles_in_area() const;
    bool is_hostile(uint16_t id) const;
    // Real time only: combat mode ends once the foes it was entered for are gone.
    static constexpr int kRealtimeCalmFrames = 60;
    void world_replaced();  // a load or a new game (World::reset_state / new_game)
    bool party_side(uint16_t id) const;  // the Avatar or on his side
    // Followers off screen are put at the edge of the view on the Avatar's side (combat start).
    int pull_companions(const std::vector<uint16_t>& hostiles);

private:
    enum Phase { kIdle, kStart, kTurn };
    enum Sub { kBegin, kChoose, kThink, kMoving, kActing, kPause };
    void new_round();
    void begin_turn();
    void next_turn();
    void finish_action();
    void cmd_defend_now(uint16_t id);
    void turn_timed_out();
    bool asking_player() const;   // a target cursor, an answer menu or a window waits for the player
    void leave_combat();          // the end of a fight: weapons away, peaceful stance and music
    void realtime_calm();         // real time: all foes gone, combat mode ends
    bool check_end();
    void ai_think();
    bool ai_heal(Actor& a);
    bool ai_cast(Actor& a, uint16_t target);
    bool busy(uint16_t id) const;
    bool settled() const;
    bool valid_fighter(uint16_t id) const;
    bool in_area(const Actor& from, const Actor& o, int margin) const;
    std::vector<uint16_t> party_actors() const;
    void compute_reach(uint16_t id);
    void start_route(int node);
    void step_route();
    int best_node_toward(uint16_t id, uint16_t target, bool want_hit, int keep_away = 0);
    int node_away_from(uint16_t id, uint16_t threat);
    void do_attack(uint16_t id, uint16_t target);
    bool do_cast(uint16_t id, const CastOption& o, uint16_t target);
    void say(const std::string& s, bool to_log = true);  // to_log: also a line in the HUD combat log
    std::string name_of(uint16_t id) const;
    int max_hp(const Actor& a) const;
    Fighter* fighter(uint16_t id);
    const Fighter* fighter(uint16_t id) const;
    void reset_silently();

    World& w_;
    Phase phase_ = kIdle;
    Sub sub_ = kBegin;
    std::vector<Fighter> order_;
    std::vector<Fighter> joiners_;
    int idx_ = -1, round_ = 0;
    uint16_t acting_ = 0;
    uint64_t snap_ = 0;        // processes with a serial <= snap_ are frozen
    int sub_frames_ = 0, phase_frames_ = 0;
    bool moved_ = false;
    bool avatar_was_combat_ = true;
    bool pending_start_ = false;
    bool prev_combat_ = false;
    uint16_t pending_attack_ = 0;  // a melee attack to make once the move is done
    uint16_t wait_pid_ = 0;        // usecode started by the action (Use / Cast with items)
    uint16_t direct_hit_ = 0;      // a blow to resolve without a reaching animation frame
    int direct_dir_ = 0, direct_in_ = 0;
    int hp_at_move_ = 0;
    std::vector<PathStep> route_;
    std::vector<Node> reach_;
    uint16_t reach_for_ = 0;
    bool reach_dirty_ = true;
    const Actor* avatar_seen_ = nullptr;  // a load / new game replaced the world
    uint32_t last_frame_ = 0;
    int pause_left_ = 0;
    double turn_age_ = 0;          // game frames this turn has been running (kTurnTimeoutFrames)
    std::set<uint16_t> let_run_;   // pids that go on after a timed-out turn (hit reactions, falls)
    bool pending_unready_ = false; // the Avatar puts his weapon away once he is free
    bool rt_foes_seen_ = false;    // real time: a foe was about since combat mode started
    int rt_calm_ = 0;
    std::set<uint16_t> fallback_ids_;     // hostiles of a scripted encounter (real time)
};

}  // namespace pg
