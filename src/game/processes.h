// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "data/palette.h"
#include "game/anim_tracker.h"
#include "kernel/kernel.h"

namespace pg {

class World;
struct AnimSequence;

// The view centre (kernel.md 4 "Camera"): follow an item, sit still, or scroll.
class CameraProcess : public Process {
public:
    CameraProcess(World& w, uint16_t follow) : w_(w), follow_(follow) {}
    CameraProcess(World& w, int x, int y, int z) : w_(w), follow_(0) { pos_[0] = x; pos_[1] = y; pos_[2] = z; }
    void start_scroll(int x, int y, int z, int frames);
    void run() override;
    const char* name() const override { return "camera"; }
    PG_SAVED_PROCESS("camera")
    void point(int out[3]) const;
    uint16_t following() const { return follow_; }

private:
    World& w_;
    uint16_t follow_;
    int pos_[3] = {0, 0, 0};
    int from_[3] = {0, 0, 0}, to_[3] = {0, 0, 0};
    int scroll_left_ = 0, scroll_total_ = 0;
};

// Each frame, checks the eggs against the Avatar (items.md 8.1).
class EggHatcherProcess : public Process {
public:
    explicit EggHatcherProcess(World& w) : w_(w) {}
    void run() override;
    const char* name() const override { return "egg hatcher"; }
    PG_SAVED_PROCESS("egghatch")

private:
    World& w_;
};

// Hourly schedule sweep (time.md 3).
class SchedulerProcess : public Process {
public:
    explicit SchedulerProcess(World& w) : w_(w) { type = kProcScheduler; }
    void run() override;
    const char* name() const override { return "scheduler"; }
    PG_SAVED_PROCESS("scheduler")
    unsigned last_hour = 0;

private:
    World& w_;
    int next_actor_ = 0;  // 0 = idle
};

// Avatar regeneration and hunger (time.md 4).
class HealProcess : public Process {
public:
    explicit HealProcess(World& w) : w_(w) { type = kProcHeal; }
    void run() override;
    const char* name() const override { return "heal"; }
    PG_SAVED_PROCESS("heal")
    unsigned hunger = 0;

private:
    World& w_;
    unsigned counter_ = 0;
};

// The new-game startup (startup.md 3).
class StartupProcess : public Process {
public:
    explicit StartupProcess(World& w) : w_(w) { prevents_saving = true; }
    void run() override;
    const char* name() const override { return "startup"; }
    PG_SAVED_PROCESS("startup")

private:
    World& w_;
    bool intro_done_ = false;
};

// Plays one ANIM.DAT action on an actor (actors.md 7): an AnimTracker works out and
// validates each frame, and the process applies it with a forced collide move (so touch
// events fire), handles blocking, losing support (falling) and attack hits.
class ActorAnimProcess : public Process {
public:
    ActorAnimProcess(World& w, uint16_t actor, unsigned action, int dir, int steps);
    void run() override;
    void on_terminate() override;
    const char* name() const override { return "actor anim"; }
    PG_SAVED_PROCESS("anim")
    unsigned action() const { return action_; }
    // Targeted mode (actors.md 7.8), set before the first run.
    void set_target(int x, int y, int z) { targeted_ = true; target_[0] = x; target_[1] = y; target_[2] = z; }
    // Milestone 6 (Responsive mode): start `n` frames into the action, and end it early
    // without counting it as a played half of a two-step cycle.
    void set_skip(unsigned n) { skip_ = n; }
    void interrupt() { played_ = false; terminate(); }
    unsigned frames_played() const { return tr_ ? tr_->frames_played() : 0; }
    int direction() const { return tr_ ? tr_->dir() : dir_; }

private:
    bool apply_frame();
    bool step_once();  // one frame's worth (false: the process ended)
    void after_block();
    void frame_sounds(const struct AnimFrame& f);
    World& w_;
    unsigned action_;
    int dir_;
    int steps_limit_;
    std::unique_ptr<AnimTracker> tr_;
    bool started_ = false, played_ = false, locked_ = false, fell_ = false;
    int hold_ = 0;
    bool targeted_ = false;
    int target_[3] = {0, 0, 0};
    unsigned skip_ = 0;
    int speed_acc_ = 0;  // Leveling's animation speed (not saved: at most one step's rounding)
};

// Wanders around (actors.md 9.4).
class LoiterProcess : public Process {
public:
    LoiterProcess(World& w, uint16_t actor, int count) : w_(w), count_(count) { item = actor; type = kProcLoiter; }
    void run() override;
    const char* name() const override { return "loiter"; }
    PG_SAVED_PROCESS("loiter")

private:
    World& w_;
    int count_;
    int stage_ = 0;
};

// A short-lived animated effect item (intrinsics/world.md "Sprites").
class SpriteProcess : public Process {
public:
    SpriteProcess(World& w, unsigned shape, int first, int last, int repeats, int delay, int x, int y, int z);
    explicit SpriteProcess(World& w) : w_(w), first_(0), last_(0), repeats_(1), delay_(1), frame_(0), pos_{0, 0, 0} {}
    void run() override;
    void on_terminate() override;
    const char* name() const override { return "sprite"; }
    PG_SAVED_PROCESS("sprite")

private:
    World& w_;
    uint16_t sprite_ = 0;
    int first_, last_, repeats_, delay_;
    int frame_, wait_ = 0;
    int pos_[3];
    bool created_ = false;
};

// A window's notify process: ends when the window closes (ui.md).
class NotifyProcess : public Process {
public:
    explicit NotifyProcess(uint16_t owner) { item = owner; type = kProcGumpNotify; }
    void run() override {}
    void on_terminate() override { if (on_end) on_end(result); }
    std::function<void(uint32_t)> on_end;  // item powers: the target cursor's pick (not saved)
    const char* name() const override { return "gump notify"; }
    PG_SAVED_PROCESS("notify")
};

// Palette fade from one matrix to another over N frames (ui.md "Palette effects").
class FadeProcess : public Process {
public:
    FadeProcess(World& w, const PaletteTransform& from, const PaletteTransform& to, int frames)
        : w_(w), from_(from), to_(to), total_(frames), left_(frames) {}
    void run() override;
    void on_terminate() override;
    const char* name() const override { return "palette fade"; }
    PG_SAVED_PROCESS("fade")

private:
    World& w_;
    PaletteTransform from_, to_;
    int total_, left_;
};

// Screen inversion (ui.md 0x0C8).
class InvertProcess : public Process {
public:
    InvertProcess(World& w, int target) : w_(w), target_(target) {}
    void run() override;
    const char* name() const override { return "inverter"; }
    PG_SAVED_PROCESS("invert")

private:
    World& w_;
    int target_;
    int step_ = 0;
};

// Finishes a teleport after the map change (items.md 9). Type 1: survives the map change.
class TeleportProcess : public Process {
public:
    TeleportProcess(World& w, unsigned map, unsigned egg) : w_(w), map_(map), egg_(egg) { type = kProcPersistent; }
    void run() override;
    const char* name() const override { return "teleport"; }
    PG_SAVED_PROCESS("teleport")

private:
    World& w_;
    unsigned map_, egg_;
};

// Deferred item deletion (items.md 10).
class DestroyItemProcess : public Process {
public:
    DestroyItemProcess(World& w, uint16_t id) : w_(w) { item = id; type = kProcDestroyItem; }
    void run() override;
    const char* name() const override { return "destroy item"; }
    PG_SAVED_PROCESS("destroy")

private:
    World& w_;
};

// Falling, thrown and bouncing items (physics.md 7), with the Avatar's ledge grab and the
// fall damage / landing rules for actors.
class GravityProcess : public Process {
public:
    GravityProcess(World& w, uint16_t id, int vx, int vy, int vz, int gravity);
    void run() override;
    void on_terminate() override;
    const char* name() const override { return "gravity"; }
    PG_SAVED_PROCESS("gravity")
    void add_velocity(int vx, int vy, int vz, int g) { vx_ += vx; vy_ += vy; vz_ += vz; g_ = std::max(g_, g); }

private:
    void land();
    World& w_;
    int vx_, vy_, vz_, g_;
    int frames_ = 0;
    int fall_start_ = -1;
};

// Runs a sequence of animations then a delay, then clears feign death (actor intrinsic 0x096).
class ClearFeignProcess : public Process {
public:
    ClearFeignProcess(World& w, uint16_t actor) : w_(w) { item = actor; type = kProcClearFeign; }
    void run() override;
    const char* name() const override { return "clear feign death"; }
    PG_SAVED_PROCESS("clearfeign")

private:
    World& w_;
    int stage_ = 0;
};

// Intrinsic 0x0C8: the screen flip, or (Original bug fixes on) the sparkles that replace it (intrinsics.cpp).
uint16_t screen_invert_effect(World& w, bool invert);

}  // namespace pg
