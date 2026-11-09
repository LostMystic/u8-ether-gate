// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The Avatar controller (specs/world/avatar.md 1-6): mouse and keyboard movement, step /
// walk / run choice and validation, turning, jumping and climbing, hanging, combat
// controls and idle fidgets.
#include "game/avatar_mover.h"

#include "core/variants.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "core/variants.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/assist.h"
#include "game/combat.h"
#include "game/ledge.h"
#include "game/processes.h"
#include "game/leveling.h"
#include "game/world.h"

namespace pg {

namespace {
enum : unsigned {
    kWalk = 0, kRun = 1, kStand = 2, kJumpUp = 3, kStandUp = 4, kReady = 5, kUnready = 6, kAttack = 7,
    kAdvance = 8, kRetreat = 9, kRunJump = 10, kShakeHead = 11, kStep = 12, kDie = 14, kCombatStand = 15,
    kJump = 17, kAirJump = 18, kClimb16 = 19, kClimb40 = 22, kLookLeft = 32, kLookRight = 33, kBalance = 42,
    kFallBack = 44, kHang = 45, kKick = 58, kStartBlock = 59, kStopBlock = 60,
};
bool within_one(int a, int b) {
    int d = std::abs(a - b) % 8;
    return std::min(d, 8 - d) <= 1;
}
}  // namespace

bool AvatarMoverProcess::is_combat_action(unsigned a) {
    switch (a) {
    case kCombatStand: case kReady: case kAdvance: case kRetreat: case kAttack: case kKick: case kStartBlock:
    case kStopBlock: return true;
    default: return false;
    }
}

int AvatarMoverProcess::tri(unsigned action, int dir, bool end_support) const {
    AnimTracker t(w_);
    if (!t.init(1, action, dir)) return AnimTracker::kFail;
    return t.run_all(end_support);
}

bool AvatarMoverProcess::responsive() const { return w_.controls.responsive; }

uint16_t AvatarMoverProcess::turn(Actor& a, int dir, bool then_action) {
    if (!responsive()) return turn_actor(w_, 1, dir);
    if (then_action || dir < 0 || a.dir == (dir & 7)) return 0;
    bool combat = a.is(Actor::kInCombat) && !a.is(Actor::kCombatRun);
    play(combat ? kCombatStand : kStand, dir & 7);
    return 0;
}

bool AvatarMoverProcess::jump_wanted() const {
    const PlayerInput& in = w_.input;
    if (in.key_jump || in.pad_jump) return true;
    return responsive() && in.jump_buffer_ms && in.now_ms - in.jump_buffer_ms <= PlayerInput::kBufferMs;
}

// Responsive mode: walking, running, stepping, advancing and the idle poses can be cut short
// at any frame when the player asks for another direction, another gait, a stop or a jump.
bool AvatarMoverProcess::try_interrupt(Actor& a) {
    auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().find(1, kProcActorAnim));
    if (!p || !p->live() || (a.gravity_pid && w_.kernel().get(a.gravity_pid)) || w_.stasis) return false;
    if (a.flags & kItemHanging) return false;
    unsigned cur = p->action();
    bool moving = cur == kWalk || cur == kRun || cur == kStep || cur == kAdvance || cur == kRetreat;
    bool idle_pose = cur == kStand || cur == kLookLeft || cur == kLookRight || cur == kCombatStand;
    if (!moving && !idle_pose) return false;
    const PlayerInput& in = w_.input;
    bool combat = a.is(Actor::kInCombat) && !a.is(Actor::kCombatRun);
    int dir = -1;
    int len = 1;
    if (in.button[2].down) { dir = in.cursor_dir; len = in.cursor_len; }
    else if (in.pad_move) { dir = in.pad_dir; len = in.pad_len; }
    else if (in.key_forward || in.key_back) { dir = req_dir_ >= 0 ? req_dir_ : a.dir; len = 1; }
    if (in.key_run) len = 2;
    if (in.key_step) len = 0;
    bool attack = combat && (in.double_click(0) ||
                             (in.attack_buffer_ms && in.now_ms - in.attack_buffer_ms <= PlayerInput::kBufferMs));
    bool jump = !combat && jump_wanted();
    unsigned want = len == 0 ? kStep : len == 2 ? kRun : kWalk;
    if (combat) want = kAdvance;
    bool change = false;
    unsigned skip = 0;
    if (jump || attack) change = true;
    else if (idle_pose) change = dir >= 0;
    else if (dir < 0) change = true;  // stop now
    else if (dir != req_dir_ || (!combat && want != req_action_)) {
        change = true;
        skip = p->frames_played();
    }
    if (!change) return false;
    if (assist_.mode) cancel_assist();
    p->interrupt();
    resume_skip_ = skip;
    return true;
}

uint16_t AvatarMoverProcess::play(unsigned action, int dir, uint16_t after) {
    Actor* a = w_.avatar();
    if (!a) return 0;
    // Entering combat always draws first, leaving always sheathes first.
    bool was = is_combat_action(a->last_anim), now = is_combat_action(action);
    if (was != now && action != kReady && action != kUnready && action != kDie && action != kStandUp &&
        action != kFallBack && a->last_anim != kDie && a->last_anim != kFallBack && a->last_anim != kStandUp)
        action = now ? kReady : kUnready;
    uint16_t pid = w_.do_anim(1, action, dir);
    if (pid && after)
        if (Process* p = w_.kernel().get(pid)) p->wait_for(after);
    if (resume_skip_ && pid && (action == kWalk || action == kRun || action == kStep || action == kAdvance))
        if (auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().get(pid))) p->set_skip(resume_skip_);
    resume_skip_ = 0;
    return pid;
}

bool AvatarMoverProcess::cooldown_ok(const Actor& a) const {
    const int extra = w_.leveling ? w_.leveling->attack_delay_extra(a) : 0;  // Leveling: a heavy weapon below its STR
    return w_.kernel().frame_number() > w_.last_attack_frame + uint32_t(std::max(0, 25 - int(a.dex)) + extra);
}

void AvatarMoverProcess::run() {
    Actor* a = w_.avatar();
    if (!a || a->is(Actor::kDead)) return;
    {
        // Easy ledges: one hold of jump gets him up one ledge; the hint shows once per hold.
        PlayerInput& in = w_.input;
        if (in.jump_held && !was_held_) { hold_since_ms_ = in.now_ms; hold_hinted_ = false; retry_ = 0; }
        if (!in.jump_held) hold_used_ = false;
        was_held_ = in.jump_held;
        if (w_.ledge_hint.frames > 0) --w_.ledge_hint.frames;
        if (ledge_.mode == 2 && grab_tick(*a)) return;
        if (catch_in_jump(*a)) return;
    }
    // Busy, falling or out of the fast area: nothing this tick.
    if (a->is(Actor::kAnimLock) || w_.kernel().count_exact(1, kProcActorAnim)) {
        if (!(responsive() && try_interrupt(*a))) { idle_ = 0; return; }
    }
    if (a->gravity_pid && w_.kernel().get(a->gravity_pid)) { idle_ = 0; return; }
    if (w_.fast_ready() && !(a->flags & kItemFastArea)) return;
    if (pending_grab_dz_ > 0) {
        // The jump of a jump-and-grab has ended: up to the ledge, hanging.
        int dz = pending_grab_dz_;
        pending_grab_dz_ = 0;
        Box b = w_.box_of(*a);
        int fx = a->x + pending_grab_fwd_ * dir_x(pending_grab_dir_), fy = a->y + pending_grab_fwd_ * dir_y(pending_grab_dir_);
        if (w_.can_exist(fx, fy, a->z + dz, b.xd, b.yd, b.zd, w_.shape_flags(*a), 1).valid) {
            w_.move_to(1, fx, fy, a->z + dz);
            a->dir = uint8_t(pending_grab_dir_);
            play(kHang, pending_grab_dir_);
            return;
        }
    }
    if (a->last_anim == kHang && (a->flags & kItemHanging)) { hanging(*a); return; }
    if (ledge_.mode == 1 && continue_ledge(*a)) return;
    if (a->is(Actor::kInCombat) && !a->is(Actor::kCombatRun)) combat_mode(*a);
    else normal_mode(*a);
}

// ---------------------------------------------------------------------------------------------
// Normal mode (2-4)

void AvatarMoverProcess::normal_mode(Actor& a) {
    PlayerInput& in = w_.input;
    // The end of a combat run: back into combat, drawing the weapon (6.4). Even in stasis.
    if (a.is(Actor::kCombatRun) && !in.moving_input()) {
        a.set(Actor::kCombatRun, false);
        w_.set_in_combat(1);
        uint16_t p = 0;
        if (a.last_anim == kRun || a.last_anim == kRunJump) p = w_.do_anim(1, kWalk, a.dir);
        play(kReady, a.dir, p);
        return;
    }
    // Leaving combat sheathes first (6.1).
    if (!a.is(Actor::kCombatRun) && is_combat_action(a.last_anim)) {
        uint16_t p = w_.do_anim(1, kUnready, a.dir);
        uint16_t s = w_.do_anim(1, kStand, a.dir);
        if (p && s)
            if (Process* q = w_.kernel().get(s)) q->wait_for(p);
        return;
    }
    if (w_.stasis) {
        in.pad_jump = false;
        // Slowing from a run still happens.
        if (a.last_anim == kRun) { uint16_t p = w_.do_anim(1, kWalk, a.dir); play(kStand, a.dir, p); }
        was_moving_ = false;
        return;
    }
    ButtonState& L = in.button[0];
    ButtonState& R = in.button[2];
    if (!R.down || !R.handled) in.mouse_moving = false;  // a fresh press is not yet movement
    // 4.1 Jump triggers.
    bool jump_now = false;
    if (!L.handled && !R.handled && L.down && R.down &&
        uint32_t(std::abs(int(L.press_ms) - int(R.press_ms))) <= PlayerInput::kDoubleClickMs) {
        L.handled = R.handled = true;
        jump_now = true;
    } else if (!L.handled && L.down && ((R.down && in.mouse_moving) || in.key_step)) {
        L.handled = true;
        jump_now = true;
    } else if (jump_wanted()) {
        in.jump_buffer_ms = 0;
        // Easy ledges: after a hold got him up, holding on does not jump him off again.
        jump_now = !(easy_ledges() && hold_used_ && in.jump_held);
    }
    bool mouse_move = R.down || !R.handled;
    bool key_move = in.key_forward || in.key_back;
    if (jump_now) {
        if (in.pad_jump && !mouse_move) {
            in.pad_jump = false;
            jump(a, in.pad_move, false);
            return;
        }
        // "Moving" is the movement input flag (avatar.md 4.2): the right press consumed by
        // movement, or a direction / step key. Both buttons pressed together is therefore the
        // standing (targeted) jump.
        jump(a, (mouse_move && in.mouse_moving) || key_move || in.key_step, mouse_move);
        return;
    }
    // Easy ledges: while jump is held he keeps looking (a few times a second) for a ledge to
    // get up, e.g. after a hop that landed in front of one.
    if (easy_ledges() && in.jump_held && !hold_used_ && ++retry_ % 6 == 0) {
        // (The forward arc only: the full search ran on the press.)
        if (start_ledge(a, jump_dir(a, mouse_move), false, true)) return;
    }
    // Movement input.
    int dir = -1;
    int len = 1;
    if (mouse_move) {
        if (!R.handled && R.down && in.now_ms - R.press_ms < PlayerInput::kDoubleClickMs && !was_moving_ &&
            in.cursor_item == 1)
            return;  // maybe the first half of a double-click on the Avatar (combat toggle)
        R.handled = true;
        in.mouse_moving = true;
        dir = in.cursor_dir;
        len = in.cursor_len;
        back_active_ = false;
    } else if (key_move) {
        if (in.key_back) {
            if (!back_active_) { back_active_ = true; back_dir_ = (a.dir + 4) & 7; }
            dir = back_dir_;
        } else {
            back_active_ = false;
            dir = a.dir;
        }
        len = 1;
        if (in.key_turn_left) dir = (dir + 7) & 7;
        if (in.key_turn_right) dir = (dir + 1) & 7;
        if (in.key_back && back_active_) back_dir_ = dir;
    } else if (in.pad_move) {
        back_active_ = false;
        dir = in.pad_dir;
        len = in.pad_len;
    } else {
        back_active_ = false;
        if (in.key_turn_left || in.key_turn_right) {
            int d = (a.dir + (in.key_turn_left ? 7 : 1)) & 7;
            turn(a, d, false);
            idle_ = 0;
            return;
        }
    }
    if (in.key_step) len = 0;
    if (in.key_run) len = 2;
    if (dir >= 0) {
        idle_ = 0;
        unsigned action = kWalk;
        if (len == 0) action = kStep;
        else if (len == 2)
            action = (responsive() || a.last_anim == kWalk || a.last_anim == kRun || a.last_anim == kRunJump) ? kRun : kWalk;
        req_dir_ = dir;
        req_action_ = action;
        move(a, action, dir);
        was_moving_ = true;
        return;
    }
    // 2.7 Stopping and idling.
    was_moving_ = false;
    req_dir_ = -1;
    if (assist_.mode) cancel_assist();
    if (a.last_anim == kRun && !responsive()) {
        uint16_t p = w_.do_anim(1, kWalk, a.dir);
        play(kStand, a.dir, p);
        return;
    }
    if (a.last_anim != kStand && a.last_anim != kLookLeft && a.last_anim != kLookRight && (a.last_anim < 27 || a.last_anim > 31)) {
        play(kStand, a.dir);
        idle_ = 0;
        return;
    }
    idle(a);
}

void AvatarMoverProcess::idle(Actor& a) {
    ++idle_;
    bool looking = a.last_anim == kLookLeft || a.last_anim == kLookRight;
    if (!looking) {
        if (w_.rand_below(3001) + 150 < idle_) {
            int side = last_look_;
            if (w_.rand_below(5) != 0) side = side == kLookLeft ? kLookRight : kLookLeft;
            last_look_ = side;
            w_.do_anim(1, unsigned(side), a.dir);
            idle_ = 1;
        }
    } else if (w_.rand_below(1501) + 30 < idle_) {
        w_.do_anim(1, kStand, a.dir);
        idle_ = 1;
    }
}

bool AvatarMoverProcess::assist_on() const { return w_.controls.responsive && w_.controls.assist; }

int AvatarMoverProcess::tri_move(unsigned action, int dir) {
    if (!assist_on()) return tri(action, dir);
    AnimTracker t(w_);
    if (!t.init(1, action, dir)) return AnimTracker::kFail;
    int r = t.run_all(false);
    if (r == AnimTracker::kFail) return r;
    // Edge guard: never walk into water, lava or a damaging drop by accident.
    const Actor* a = w_.avatar();
    if (a && assist::dangerous(w_, *a, t.x(), t.y(), t.z()) && !assist::dangerous(w_, *a, a->x, a->y, a->z)) {
        edge_stop_ = true;
        return AnimTracker::kFail;
    }
    return r;
}

void AvatarMoverProcess::cancel_assist() {
    assist_ = Assist{};
}

// Movement assist: the move straight ahead fails; look sideways for a spot from which the way
// ahead is clear and safe, and get him there (nudge, quick slide, or the pathfinder).
bool AvatarMoverProcess::start_assist(Actor& a, unsigned action, int dir) {
    assist::Candidate c;
    if (!assist::find(w_, a, action, dir, c)) return false;
    int off = std::abs(c.offset);
    if (off <= assist::kNudge) {
        w_.move_to(1, c.x, c.y, c.z);
        play(action, dir);
        return true;
    }
    assist_ = Assist{};
    assist_.dir = dir;
    assist_.action = action;
    assist_.x = c.x; assist_.y = c.y; assist_.z = c.z;
    // Further (up to kProbeMax): the same slide, a little longer (at most 5 frames). A walk
    // cycle sideways would overshoot (one cycle is ~100 units), so it is not used.
    assist_.mode = 1;
    return continue_assist(a, action, dir);
}

bool AvatarMoverProcess::continue_assist(Actor& a, unsigned action, int dir) {
    if (assist_.mode == 0) return false;
    if (dir != assist_.dir) { cancel_assist(); return false; }
    ++assist_.frames;
    if (assist_.mode == 1) {
        // A quick slide sideways, kSnapSpeed units a frame, then on in the asked direction.
        int dx = assist_.x - a.x, dy = assist_.y - a.y;
        int dist = std::max(std::abs(dx), std::abs(dy));
        if (dist == 0 || assist_.frames > 12) {
            assist_ = Assist{};
            play(action, dir);
            return true;
        }
        int stepn = std::min(dist, assist::kSnapSpeed);
        int nx = a.x + dx * stepn / dist, ny = a.y + dy * stepn / dist;
        if (w_.collide_move(1, nx, ny, a.z, false, false) < 0x4000) { assist_ = Assist{}; return false; }
        return true;
    }
    return false;
}

// 2.5-2.6: validate a step / walk / run, slide along walls, creep up to edges, turn.
void AvatarMoverProcess::move(Actor& a, unsigned action, int dir) {
    bool adjusted = false;
    edge_stop_ = false;
    if (assist_on()) {
        if (assist_.mode && continue_assist(a, action, dir)) return;
        if (tri_move(action, dir) == AnimTracker::kFail && start_assist(a, action, dir)) return;
        edge_stop_ = false;
    }
    for (int round = 0; round < 4; ++round) {
        auto works = [&](int r) { return r == AnimTracker::kOk || (r == AnimTracker::kOffLand && action != kStep); };
        int d = dir;
        int res = tri_move(action, d);
        if (!works(res) && !(action == kStep && res == AnimTracker::kOffLand && round > 0)) {
            int r2 = tri_move(action, (dir + 1) & 7);
            if (works(r2)) { d = (dir + 1) & 7; res = r2; }
            else {
                int r3 = tri_move(action, (dir + 7) & 7);
                if (works(r3)) { d = (dir + 7) & 7; res = r3; }
            }
        }
        if (works(res) || (action == kStep && res == AnimTracker::kOffLand)) {
            if (res == AnimTracker::kOffLand) {
                if (action == kWalk) { action = kStep; continue; }
                if (action == kRun) { action = kWalk; continue; }
                if (action == kStep && !adjusted && a.z > 0 && a.last_anim != kBalance) {
                    if (a.dir != d) { turn(a, d, false); return; }
                    play(kBalance, d);
                    return;
                }
            }
            // 2.6 Turning.
            if (d != a.dir && !responsive()) {
                bool moving = a.last_anim == kWalk || a.last_anim == kRun || a.last_anim == kCombatStand;
                if (!(moving && within_one(d, a.dir))) {
                    if (a.last_anim == kRun) { play(kWalk, a.dir); return; }  // slow down first
                    turn_actor(w_, 1, d);
                    return;
                }
            }
            play(action, d);
            return;
        }
        // All three directions failed: walk -> step, run -> walk, step -> stand.
        if (action == kWalk) { action = kStep; adjusted = true; continue; }
        if (action == kRun) { action = kWalk; adjusted = true; continue; }
        if (edge_stop_ && a.last_anim != kBalance && a.dir == dir) { play(kBalance, dir); return; }  // teeter at the edge
        if (a.dir != dir) turn(a, dir, false);
        else if (a.last_anim != kStand) play(kStand, dir);
        return;
    }
}

bool AvatarMoverProcess::try_climb(Actor& a, int dir) {
    for (int k = 0; k < 8; ++k)
        if (tri(kClimb16 + unsigned(k), dir, true) == AnimTracker::kOk) {
            play(kClimb16 + unsigned(k), dir);
            w_.train_stat(0, 2 + k);
            w_.train_stat(2, 2 * (2 + k));
            return true;
        }
    (void)a;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Easy ledges (Responsive + "Easy Ledges"; game/ledge.h)

bool AvatarMoverProcess::easy_ledges() const { return w_.controls.responsive && w_.controls.easy_ledges; }

// The way the player wants to go: the pointer, the stick, the arrow keys, else his facing.
int AvatarMoverProcess::jump_dir(const Actor& a, bool mouse) const {
    const PlayerInput& in = w_.input;
    if (mouse) return in.cursor_dir;
    if (in.pad_move) return in.pad_dir;
    int dir = a.dir;
    if (in.key_forward || in.key_back) {
        dir = in.key_back ? (a.dir + 4) & 7 : a.dir;
        if (in.key_turn_left) dir = (dir + 7) & 7;
        if (in.key_turn_right) dir = (dir + 1) & 7;
    }
    return dir;
}

bool AvatarMoverProcess::start_ledge(Actor& a, int pref_dir, bool around, bool hint) {
    PlayerInput& in = w_.input;
    if (a.is(Actor::kInCombat) || (a.flags & kItemHanging) || ledge_.mode) return false;
    ledge::Plan p;
    // Walking or running: straight ahead only (a running jump is not turned into a climb of
    // something off to the side).
    bool narrow = w_.input.moving_input() && (a.last_anim == kWalk || a.last_anim == kRun);
    if (!ledge::find(w_, a, pref_dir, around, p, narrow)) {
        // Nothing in reach: say so once per hold (and not more than every 20 s), once the
        // hold has lasted a moment (a quick tap is just a jump).
        if (hint && !hold_hinted_ && in.now_ms - hold_since_ms_ >= 400 &&
            (!last_hint_ms_ || in.now_ms - last_hint_ms_ >= 20000)) {
            hold_hinted_ = true;
            last_hint_ms_ = in.now_ms;
            if (w_.hint_ui) w_.hint_ui("No ledge in reach");
        }
        return false;
    }
    PG_LOG("ledge: %s %d up (action %u%s), dir %d, %d away (spot %d,%d)", p.grab ? "jump, hang and pull up" : "climb",
           p.rise, p.move.action, p.move.targeted ? ", steered" : "", p.dir, p.travel, p.sx, p.sy);
    w_.ledge_hint.x = p.ex; w_.ledge_hint.y = p.ey; w_.ledge_hint.z = p.ez;
    w_.ledge_hint.dir = p.dir;
    w_.ledge_hint.frames = ledge::kHintFrames;
    hold_used_ = true;
    cancel_assist();
    ledge_ = LedgeRun{};
    ledge_.mode = 1;
    ledge_.plan = p;
    a.dir = uint8_t(p.dir);
    idle_ = 0;
    return continue_ledge(a);
}

// Walks (glides) him to the spot: sideways to the lane first, then forward, the path the
// search checked. Movement input away from it cancels.
bool AvatarMoverProcess::continue_ledge(Actor& a) {
    const PlayerInput& in = w_.input;
    ledge::Plan& p = ledge_.plan;
    int push = in.button[2].down && in.mouse_moving ? in.cursor_dir : in.pad_move ? in.pad_dir : -1;
    auto cancel = [&] {
        ledge_ = LedgeRun{};
        hold_used_ = false;  // the hold keeps trying
        return false;
    };
    if (++ledge_.frames > 30 || w_.stasis || a.z != p.sz || (push >= 0 && !within_one(push, p.dir) && !within_one(push, a.dir)))
        return cancel();
    int dx = dir_x(p.dir), dy = dir_y(p.dir);
    int along = (p.sx - a.x) * dx + (p.sy - a.y) * dy;  // forward still to go (diagonals count twice)
    if (dx && dy) along /= 2;
    int lx = p.sx - along * dx, ly = p.sy - along * dy;  // the lane point beside the spot
    if (ledge_.leg == 0 && a.x == lx && a.y == ly) ledge_.leg = 1;
    int tx = ledge_.leg == 0 ? lx : p.sx, ty = ledge_.leg == 0 ? ly : p.sy;
    int ddx = tx - a.x, ddy = ty - a.y;
    int dist = std::max(std::abs(ddx), std::abs(ddy));
    if (dist == 0 && ledge_.leg == 1) { do_ledge(a); return true; }
    if (dist == 0) return true;
    // The path was checked in 4-unit steps; move along it the same way (placement test).
    Box b = w_.box_of(a);
    int nx = a.x, ny = a.y;
    for (int moved = 0; moved < ledge::kApproachSpeed && (nx != tx || ny != ty); moved += 4) {
        int sx = nx + std::clamp(tx - nx, -4, 4), sy = ny + std::clamp(ty - ny, -4, 4);
        World::Placement pl = w_.can_exist(sx, sy, a.z, b.xd, b.yd, b.zd, w_.shape_flags(a), 1);
        if (!pl.valid || !pl.supported) break;
        nx = sx; ny = sy;
    }
    if (nx == a.x && ny == a.y) return cancel();
    w_.move_to(1, nx, ny, a.z);
    if (ledge_.leg == 1 && a.x == p.sx && a.y == p.sy) do_ledge(a);
    return true;
}

// A climb (or pull-up), steered onto its landing spot if the move says so; climbs train.
void AvatarMoverProcess::play_climb(const ledge::Move& m, int dir) {
    if (tri(m.action, dir, true) != AnimTracker::kOk && !m.targeted) return;
    uint16_t pid = play(m.action, dir);
    if (m.targeted)
        if (auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().get(pid))) p->set_target(m.tx, m.ty, m.tz);
    int k = int(m.action) - int(kClimb16);
    w_.train_stat(0, 2 + k);
    w_.train_stat(2, 2 * (2 + k));
}

void AvatarMoverProcess::do_ledge(Actor& a) {
    ledge::Plan p = ledge_.plan;
    ledge_ = LedgeRun{};
    a.dir = uint8_t(p.dir);
    if (!p.grab) {
        play_climb(p.move, p.dir);
        return;
    }
    ledge::Move m;
    if (!ledge::pull_from(w_, a, p.hx, p.hy, p.hz, p.dir, m)) return;  // the world changed
    ledge_.mode = 2;
    ledge_.plan = p;
    ledge_.last_z = a.z;
    play(kJumpUp, p.dir);
}

// The jump of a grab: let the jump lift him, and at its top (or once it reaches the hang
// height) carry him on up to the hang point (16 units a frame up, then 8 toward the face) and
// hang there. The search checked the column above the spot, so this is the path he takes.
bool AvatarMoverProcess::grab_tick(Actor& a) {
    ledge::Plan& p = ledge_.plan;
    ++ledge_.frames;
    if (!ledge_.rising) {
        auto* anim = dynamic_cast<ActorAnimProcess*>(w_.kernel().find(1, kProcActorAnim));
        bool jumping = anim && anim->live();
        // (The jump holds each height for a frame or two: the top is when he starts down.)
        bool apex = a.z < ledge_.last_z;
        ledge_.last_z = std::max(ledge_.last_z, a.z);
        if (jumping && a.z < p.hz && !apex && ledge_.frames < 45) return true;
        if (jumping) w_.kernel().kill(1, kProcActorAnim);
        w_.kernel().kill(1, kProcGravity);
        a.set(Actor::kAnimLock, false);
        ledge_.rising = true;
    }
    Box b = w_.box_of(a);
    int nx = a.x, ny = a.y, nz = a.z;
    if (a.z != p.hz) nz = a.z + std::clamp(p.hz - a.z, -16, 16);
    else {
        nx = a.x + std::clamp(p.hx - a.x, -8, 8);
        ny = a.y + std::clamp(p.hy - a.y, -8, 8);
    }
    if (!w_.can_exist(nx, ny, nz, b.xd, b.yd, b.zd, w_.shape_flags(a), 1).valid || ledge_.frames > 90) {
        ledge_ = LedgeRun{};
        w_.fall(1);
        return true;
    }
    w_.move_to(1, nx, ny, nz);
    if (nx == p.hx && ny == p.hy && nz == p.hz) {
        int dir = p.dir;
        ledge_ = LedgeRun{};
        a.dir = uint8_t(dir);
        auto_pull_ = true;
        play(kHang, dir);
    }
    return true;
}

// Coming down in a jump (the animation's own arc, not a fall) with jump held: catch a ledge
// in front within the catch window, as a fall does.
bool AvatarMoverProcess::catch_in_jump(Actor& a) {
    PlayerInput& in = w_.input;
    int prev = air_z_;
    air_z_ = a.z;
    if (!easy_ledges() || !in.jump_held || ledge_.mode || w_.stasis || in.now_ms < in.no_grab_until_ms) return false;
    unsigned la = a.last_anim;
    if (la != kJumpUp && la != kJump && la != kRunJump && la != kAirJump) return false;
    auto* anim = dynamic_cast<ActorAnimProcess*>(w_.kernel().find(1, kProcActorAnim));
    if (!anim || !anim->live() || prev < 0 || a.z >= prev) return false;
    int x, y, z, dir, edge[3];
    if (!ledge::catch_point(w_, a, prev - a.z, x, y, z, dir, edge)) return false;
    w_.kernel().kill(1, kProcActorAnim);
    a.set(Actor::kAnimLock, false);
    w_.move_to(1, x, y, z);
    a.dir = uint8_t(dir);
    w_.ledge_hint.x = edge[0]; w_.ledge_hint.y = edge[1]; w_.ledge_hint.z = edge[2];
    w_.ledge_hint.dir = dir;
    w_.ledge_hint.frames = ledge::kHintFrames;
    auto_pull_ = true;
    hold_used_ = true;
    play(kHang, dir);
    PG_LOG("ledge: caught in a jump at %d,%d,%d", x, y, z);
    return true;
}

// 4.2-4.3 Jumping.
void AvatarMoverProcess::jump(Actor& a, bool moving, bool mouse) {
    PlayerInput& in = w_.input;
    int dir = mouse ? in.cursor_dir : a.dir;
    idle_ = 0;
    if (easy_ledges() && in.jump_held && start_ledge(a, jump_dir(a, mouse), !moving, true)) return;
    // Responsive: every jump goes forward (toward the pointer for a mouse jump), with landing
    // magnetism; the targeted standing jump below belongs to the Classic controls (NOTES.md
    // "v2.12 features").
    if (responsive()) { responsive_jump(a, moving, mouse); return; }
    // The controller: a jump in the stick's direction, or a hop / climb when standing.
    bool pad = !mouse && !in.key_jump && !in.button[0].down;
    if (pad && in.pad_move) dir = in.pad_dir;
    if (pad && !moving) {
        if (try_climb(a, dir)) return;
        play(kJumpUp, dir);
        return;
    }
    if (moving) {
        unsigned action = kJump;
        if (a.last_anim == kRun || a.last_anim == kRunJump) action = kRunJump;
        else if (a.is(Actor::kAirWalk)) action = kAirJump;
        else if ((mouse && in.cursor_len == 0) || in.key_step) action = kJumpUp;
        else if (!mouse && try_climb(a, dir)) return;
        play(action, dir);
        return;
    }
    // Standing still: face the cursor first.
    uint16_t turn = mouse || in.button[0].down ? this->turn(a, in.cursor_dir, true) : 0;
    if (mouse || in.button[0].down) dir = in.cursor_dir;
    bool short_jump = in.cursor_len == 0 || in.key_step;
    if (!turn && try_climb(a, dir)) return;
    if (short_jump) { play(kJumpUp, dir, turn); return; }
    // v2.10 (the "v2.12 changes" option Off): no targeted jump; the plain jump toward the
    // pointer, its distance set by the animation (specs/world/v212_features.md 1.2-1.3).
    if (!v212_changes()) { play(kJump, dir, turn); return; }
    // Jump to target (v2.12): only onto land within STR * 32 on both axes;
    // anything else: he shakes his head (specs/world/avatar.md 4.3).
    const Item* t = w_.item(in.cursor_item);
    Box b = w_.box_of(a);
    int cx = b.x - b.xd / 2, cy = b.y - b.yd / 2;
    int range = 32 * std::max<int>(1, w_.leveling->reach_str(a));  // (Leveling: at least the original's reach)
    bool ok = t && t->id != 1 && w_.data().typeflags.get(t->shape).is(ShapeInfo::kLand) &&
              std::abs(in.cursor_point[0] - cx) < range && std::abs(in.cursor_point[1] - cy) < range;
    if (!ok) { play(kShakeHead, dir, turn); return; }
    uint16_t pid = play(kJumpUp, dir, turn);
    if (auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().get(pid))) {
        p->set_target(in.cursor_point[0] + b.xd / 2, in.cursor_point[1] + b.yd / 2, in.cursor_point[2]);
        PG_LOG("jump to %d,%d,%d (from %d,%d,%d)", in.cursor_point[0] + b.xd / 2, in.cursor_point[1] + b.yd / 2,
               in.cursor_point[2], a.x, a.y, a.z);
    }
}

// Responsive jumps (PLAN-M6.md): the key / pad jump goes forward in the direction he faces or
// is pushed: standing a short hop (kHop), walking the jump, running the running jump. Straight
// up only with a wall right in front. A landing that would be unsafe (water, lava, a deep drop,
// the very edge) moves to the nearest safe platform in a 30 degree arc within reach; with none
// the jump stays as it is (a deliberate jump into water works).
void AvatarMoverProcess::responsive_jump(Actor& a, bool moving, bool mouse) {
    PlayerInput& in = w_.input;
    int dir = mouse ? in.cursor_dir : in.pad_move ? in.pad_dir : a.dir;
    if (!mouse && !in.pad_move && (in.key_forward || in.key_back)) {
        dir = in.key_back ? (a.dir + 4) & 7 : a.dir;
        if (in.key_turn_left) dir = (dir + 7) & 7;
        if (in.key_turn_right) dir = (dir + 1) & 7;
    }
    bool running = a.last_anim == kRun || a.last_anim == kRunJump;
    if (!moving && !mouse && try_climb(a, dir)) return;
    // A ledge too high for a direct climb but within a jump and a grab (up to kGrabReach above
    // the highest climb): jump, catch it and hang; holding jump pulls him up (hanging()).
    // (With Easy Ledges the search in start_ledge() does this, with its own rules.)
    if (!moving && !mouse && (in.jump_held || in.key_jump || in.pad_jump) && !easy_ledges()) {
        Box b = w_.box_of(a);
        for (int dz = 8; dz <= kGrabReach; dz += 8)
            for (int fwd = 0; fwd <= 16; fwd += 4) {
                // The hang: up dz and flush against the wall (up to 16 forward).
                int hx = a.x + fwd * dir_x(dir), hy = a.y + fwd * dir_y(dir), hz = a.z + dz;
                if (!w_.can_exist(hx, hy, hz, b.xd, b.yd, b.zd, w_.shape_flags(a), 1).valid) continue;
                ActorState st;
                st.x = hx; st.y = hy; st.z = hz;
                st.dir = uint8_t(dir);
                st.last_anim = kHang;
                st.last_anim_dir = uint16_t(dir);
                AnimTracker t(w_);
                int res = t.init(1, kClimb40, dir, &st) ? t.run_all(true) : -1;
                if (res == AnimTracker::kOk) {
                    pending_grab_dz_ = dz;
                    pending_grab_dir_ = dir;
                    pending_grab_fwd_ = fwd;
                    play(kJumpUp, dir);
                    PG_LOG("ledge: jump and grab %d up, %d forward", dz, fwd);
                    return;
                }
            }
    }
    Box b = w_.box_of(a);
    // A wall right in front: straight up.
    int fx = b.x + 32 * dir_x(dir), fy = b.y + 32 * dir_y(dir);
    if (!w_.can_exist(fx, fy, b.z + 8, b.xd, b.yd, b.zd, w_.shape_flags(a), 1).valid &&
        !w_.can_exist(fx, fy, b.z, b.xd, b.yd, b.zd, w_.shape_flags(a), 1).valid) {
        play(kJumpUp, dir);
        return;
    }
    unsigned action = !moving ? kJumpUp : running ? kRunJump : a.is(Actor::kAirWalk) ? kAirJump : kJump;
    // The natural landing.
    int nx, ny, nz, dist;
    if (action == kJumpUp) {
        dist = assist::kHop;
        nx = b.x + int(std::lround(dist * dir_x(dir) / std::hypot(double(dir_x(dir)), double(dir_y(dir)))));
        ny = b.y + int(std::lround(dist * dir_y(dir) / std::hypot(double(dir_x(dir)), double(dir_y(dir)))));
        nz = b.z;
    } else {
        AnimTracker t(w_);
        if (!t.init(1, action, dir)) { play(action, dir); return; }
        int dx, dy, dz;
        t.travel(dx, dy, dz);
        dist = int(std::hypot(double(dx), double(dy)));
        nx = b.x + dx; ny = b.y + dy; nz = b.z + dz;
    }
    // Where he would come down: the surface under the natural point.
    int top;
    uint16_t f, id;
    if (assist::surface_under(w_, nx - b.xd / 2, ny - b.yd / 2, nz + assist::kJumpRise, 1, top, f, id) && top >= nz - 96) nz = top;
    int land[3] = {nx, ny, nz};
    bool target = action == kJumpUp;  // the hop always flies to its point
    if (w_.controls.assist && !assist::safe_landing(w_, a, nx, ny, nz)) {
        if (assist::find_landing(w_, a, dir, dist, nx, ny, nz, land)) target = true;
    }
    uint16_t pid = play(action, dir);
    if (target)
        if (auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().get(pid))) p->set_target(land[0], land[1], land[2]);
    idle_ = 0;
}

// 5. Hanging from a ledge.
void AvatarMoverProcess::hanging(Actor& a) {
    PlayerInput& in = w_.input;
    if (w_.stasis) return;
    ButtonState& L = in.button[0];
    bool climb = (!L.handled && !L.down) || in.past_timeout(0) || in.key_jump || in.pad_jump;
    bool asked = false;  // Responsive: jump held or pressed, pushing toward the ledge, a grab
    if (responsive()) {
        // Responsive: jump held or tapped, or pushing toward the ledge, climbs up; pushing away
        // lets go; with nothing held he drops.
        int push = in.button[2].down ? in.cursor_dir : in.pad_move ? in.pad_dir : in.key_forward ? a.dir : in.key_back ? (a.dir + 4) & 7 : -1;
        bool toward = push >= 0 && within_one(push, a.dir);
        bool away = push >= 0 && !toward && within_one(push, (a.dir + 4) & 7);
        if (in.jump_held || jump_wanted() || toward || auto_pull_) climb = asked = true;
        if (away || in.key_step) {
            auto_pull_ = false;
            a.flags &= uint16_t(~kItemHanging);
            a.last_anim = kStand;
            in.no_grab_until_ms = in.now_ms + 600;
            w_.fall(1);
            return;
        }
        if (!climb && push < 0 && !in.jump_held) {
            a.flags &= uint16_t(~kItemHanging);
            a.last_anim = kStand;
            in.no_grab_until_ms = in.now_ms + 600;
            w_.fall(1);
            return;
        }
        if (!climb) return;
    }
    if (in.pad_jump) in.pad_jump = false;
    if (climb) {
        L.handled = true;
        ledge::Move m;
        bool ok = tri(kClimb40, a.dir, true) == AnimTracker::kOk;
        if (ok) m.action = kClimb40;
        // Easy ledges: the other pull-ups (32, 48, 24, 56), steered onto the top if need be.
        if (!ok && easy_ledges() && asked) ok = ledge::pull_from(w_, a, a.x, a.y, a.z, a.dir, m);
        if (ok) {
            a.flags &= uint16_t(~kItemHanging);
            play_climb(m, a.dir);
            auto_pull_ = false;
            hold_used_ = in.jump_held;
        } else if (auto_pull_) {
            auto_pull_ = false;  // the ledge is gone: let go
            a.flags &= uint16_t(~kItemHanging);
            a.last_anim = kStand;
            in.no_grab_until_ms = in.now_ms + 600;
            w_.fall(1);
        }
        return;
    }
    // Letting go (avatar.md 5): the hang lasts only while movement input is held and the
    // 40-unit climb would still work; releasing all movement input drops him.
    if (!in.moving_input() || tri(kClimb40, a.dir, true) != AnimTracker::kOk) {
        a.flags &= uint16_t(~kItemHanging);
        a.last_anim = kStand;
        in.no_grab_until_ms = in.now_ms + 600;
        w_.fall(1);
    }
}

// ---------------------------------------------------------------------------------------------
// Combat mode (6.2)

void AvatarMoverProcess::combat_mode(Actor& a) {
    PlayerInput& in = w_.input;
    ButtonState& L = in.button[0];
    ButtonState& R = in.button[2];
    idle_ = 0;
    was_moving_ = false;
    // 1. Getting up after being knocked down.
    if (a.last_anim == kDie || a.last_anim == kFallBack) {
        uint16_t p = play(kStandUp, a.dir);
        if (a.is(Actor::kStunned)) {
            a.set(Actor::kStunned, false);
            uint16_t s1 = w_.do_anim(1, kShakeHead, a.dir);
            if (s1 && p) if (Process* q = w_.kernel().get(s1)) q->wait_for(p);
            uint16_t s2 = w_.do_anim(1, kShakeHead, a.dir);
            if (s2 && s1) if (Process* q = w_.kernel().get(s2)) q->wait_for(s1);
        }
        return;
    }
    // 2. Releasing a block.
    if (a.last_anim == kStartBlock) {
        if (!L.down && !in.pad_block) play(kStopBlock, a.dir);
        return;  // holding the pose
    }
    if (w_.stasis) return;
    // 4. Block: the left button held past the double-click time.
    if (L.down && in.past_timeout(0)) {
        L.handled = true;
        uint16_t t = turn(a, in.cursor_dir, true);
        play(kStartBlock, in.cursor_dir, t);
        return;
    }
    if (in.pad_block) {  // the controller's left trigger: block facing the way he looks
        play(kStartBlock, a.dir);
        return;
    }
    bool dir_input = R.down || in.key_forward || in.key_back || in.pad_move;
    bool pad_attack = in.attack_buffer_ms && in.now_ms - in.attack_buffer_ms <= PlayerInput::kBufferMs;
    if (in.attack_buffer_ms && !pad_attack) in.attack_buffer_ms = 0;
    // 5. Attack.
    if ((in.double_click(0) || pad_attack) && cooldown_ok(a)) {
        int d = dir_input ? a.dir : in.cursor_dir;
        if (pad_attack) {
            in.attack_buffer_ms = 0;
            d = in.pad_move ? in.pad_dir : a.dir;
            if (const Item* t = w_.item(in.pad_target); t && t->id != 1 && !in.pad_move)
                d = direction_to(t->x - a.x, t->y - a.y);
        } else {
            in.use_double(0);
        }
        uint16_t t = (dir_input && !pad_attack) ? 0 : turn(a, d, true);
        play(kAttack, d, t);
        w_.last_attack_frame = w_.kernel().frame_number();
        w_.train_stat(0, 1 + w_.rand_below(2));
        w_.train_stat(2, 2 + w_.rand_below(2));
        return;
    }
    // 6. Kick (a right double-click away from the Avatar; on him the game mode toggles combat).
    if (in.double_click(2) && in.cursor_item != 1 && cooldown_ok(a)) {
        in.use_double(2);
        int d = dir_input && !R.down ? a.dir : in.cursor_dir;
        uint16_t t = turn(a, d, true);
        play(kKick, d, t);
        w_.last_attack_frame = w_.kernel().frame_number();
        w_.train_stat(0, 1 + w_.rand_below(2));
        w_.train_stat(2, 2 + w_.rand_below(2));
        return;
    }
    // 7. Moving with the right button.
    int dir = -1;
    bool run = false;
    if (R.down || (!R.handled && in.now_ms - R.press_ms > PlayerInput::kDoubleClickMs)) {
        if (!R.down) R.handled = true;
        dir = in.cursor_dir;
        run = in.cursor_len == 2 || in.key_run;
    } else if (in.key_forward || in.key_back) {
        dir = in.key_forward ? a.dir : (a.dir + 4) & 7;
        if (in.key_turn_left) dir = (dir + 7) & 7;
        if (in.key_turn_right) dir = (dir + 1) & 7;
        run = in.key_forward && in.key_run;
    } else if (in.pad_move) {
        dir = in.pad_dir;
        run = in.pad_len == 2;
    } else if (in.key_turn_left || in.key_turn_right) {
        turn(a, (a.dir + (in.key_turn_left ? 7 : 1)) & 7, false);
        return;
    }
    if (dir >= 0) {
        if (run) {
            // Start a combat run: one walk cycle, then normal-mode rules.
            a.set(Actor::kCombatRun, true);
            a.set(Actor::kInCombat, false);
            w_.sound.combat_music(110);
            play(responsive() ? kRun : kWalk, dir);
            return;
        }
        if (a.last_anim == kRun) { play(kReady, a.dir); return; }
        if (dir == ((a.dir + 4) & 7)) {
            if (tri(kRetreat, a.dir) != AnimTracker::kFail) play(kRetreat, a.dir);
            else if (a.last_anim != kCombatStand) play(kCombatStand, a.dir);
            return;
        }
        req_dir_ = dir;
        req_action_ = kAdvance;
        if (dir != a.dir && !within_one(dir, a.dir) && !responsive()) { turn(a, dir, false); return; }
        int d = dir;
        if (tri(kAdvance, d) == AnimTracker::kFail) {
            if (tri(kAdvance, (dir + 1) & 7) != AnimTracker::kFail) d = (dir + 1) & 7;
            else if (tri(kAdvance, (dir + 7) & 7) != AnimTracker::kFail) d = (dir + 7) & 7;
            else d = -1;
        }
        if (d >= 0) play(kAdvance, d);
        else if (a.dir != dir) turn(a, dir, false);
        else if (a.last_anim != kCombatStand) play(kCombatStand, a.dir);
        return;
    }
    req_dir_ = -1;
    // 8. A click whose time passed unused turns him to face the cursor.
    for (int b : {0, 2})
        if ((in.past_timeout(b) || (responsive() && !in.button[b].handled)) && !in.button[b].down) {
            in.button[b].handled = true;
            if (in.cursor_dir != a.dir) { turn(a, in.cursor_dir, false); return; }
        }
    // 10. Stand ready.
    if (a.last_anim != kCombatStand) play(kCombatStand, a.dir);
}

}  // namespace pg
