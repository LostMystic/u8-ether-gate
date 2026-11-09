// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>

#include "game/ledge.h"
#include "kernel/kernel.h"

namespace pg {

class World;
class Actor;

// The Avatar controller (specs/world/avatar.md 1-6): turns the player input in
// World::input into step / walk / run / jump / climb / combat actions. One persistent
// process (type 1, not attached to the Avatar, so it survives map changes and death).
class AvatarMoverProcess : public Process {
public:
    explicit AvatarMoverProcess(World& w) : w_(w) { type = kProcPersistent; }
    void run() override;
    const char* name() const override { return "avatar mover"; }
    PG_SAVED_PROCESS("mover")

    static bool is_combat_action(unsigned action);

private:
    void normal_mode(Actor& a);
    void combat_mode(Actor& a);
    void hanging(Actor& a);
    void jump(Actor& a, bool moving, bool mouse);
    void responsive_jump(Actor& a, bool moving, bool mouse);
    bool try_climb(Actor& a, int dir);
    // Validates and plays a step / walk / run toward dir (2.5), with turning (2.6).
    void move(Actor& a, unsigned action, int dir);
    // Plays an action, swapping in ready/unready weapon when the combat class changes.
    uint16_t play(unsigned action, int dir, uint16_t after = 0);
    int tri(unsigned action, int dir, bool end_support = false) const;
    void idle(Actor& a);
    bool cooldown_ok(const Actor& a) const;
    // Milestone 6, Responsive mode: turning without the turn animation (`then_action`: an
    // action in the new direction follows at once, so nothing is played here), and ending a
    // walk / run / idle animation early when the player asks for something else.
    uint16_t turn(Actor& a, int dir, bool then_action);
    bool try_interrupt(Actor& a);
    bool responsive() const;
    bool jump_wanted() const;
    // Milestone 6, Responsive mode with movement assist (see game/assist.h): the trial of a
    // move that also fails when it would end over a hazard or a damaging drop (edge guard),
    // and the sideways search that lines him up with a doorway or a narrow path.
    int tri_move(unsigned action, int dir);
    bool assist_on() const;
    bool start_assist(Actor& a, unsigned action, int dir);
    bool continue_assist(Actor& a, unsigned action, int dir);
    void cancel_assist();
    struct Assist {
        int mode = 0;  // 0 none, 1 snap (sliding), 2 path
        int dir = 0;
        unsigned action = 0;
        int x = 0, y = 0, z = 0;
        uint16_t pid = 0;  // mode 2: the sideways walking direction
        int frames = 0;
    } assist_;
    bool edge_stop_ = false;
    int pending_grab_dz_ = 0, pending_grab_dir_ = 0, pending_grab_fwd_ = 0;  // a jump that ends hanging from a ledge
    static constexpr int kGrabReach = 72;             // a ledge this much above a direct climb
    // Easy ledges (Responsive + the "Easy Ledges" option; game/ledge.h): holding jump finds a
    // ledge in reach, walks him to the spot, faces it and climbs, or jumps, hangs and pulls up.
    bool easy_ledges() const;
    int jump_dir(const Actor& a, bool mouse) const;
    bool start_ledge(Actor& a, int pref_dir, bool around, bool hint);
    bool continue_ledge(Actor& a);
    void do_ledge(Actor& a);
    void play_climb(const ledge::Move& m, int dir);
    bool grab_tick(Actor& a);       // the jump of a grab: up to the hang point
    bool catch_in_jump(Actor& a);   // coming down in a jump with jump held: catch a ledge
    struct LedgeRun {
        int mode = 0;               // 0 none, 1 walking to the spot, 2 jumping up to the hang
        int leg = 0;                // mode 1: 0 sideways to the lane, 1 forward to the spot
        ledge::Plan plan;
        int frames = 0, last_z = 0;
        bool rising = false;
    } ledge_;
    bool auto_pull_ = false;        // the hang came from a grab: pull up without waiting
    bool hold_used_ = false;        // this hold of jump got him up: no more jumps until released
    bool hold_hinted_ = false, was_held_ = false;
    uint32_t hold_since_ms_ = 0, last_hint_ms_ = 0;
    int retry_ = 0, air_z_ = -1;

    World& w_;
    int idle_ = 0;
    int last_look_ = 33;
    bool back_active_ = false;
    int back_dir_ = 0;
    bool was_moving_ = false;
    unsigned resume_skip_ = 0;           // frames to skip in the next movement action
    int req_dir_ = -1;                   // the movement asked for when the current one started
    unsigned req_action_ = 0;
};

}  // namespace pg
