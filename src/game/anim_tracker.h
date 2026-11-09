// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pg {

class World;
struct AnimSequence;
struct AnimFrame;

// What an action depends on and changes about an actor (actors.md 8.1 "state").
struct ActorState {
    int x = 0, y = 0, z = 0;
    uint8_t dir = 0;
    uint16_t last_anim = 2, last_anim_dir = 0;
    bool first_step = false, flipped = false, in_combat = false;
};

// Plays one ANIM.DAT action against the world without touching it (actors.md 7.3-7.8):
// works out the frames to play, moves and validates each frame (large-step sweep, placement,
// the step-up / side-slip scan, support), steers targeted jumps and finds melee hits.
// The animation process applies its results to the actor; the pathfinder and the Avatar
// controller use it for trial runs ("trying an action", 7.6).
class AnimTracker {
public:
    enum Result { kFail, kOffLand, kOk };

    explicit AnimTracker(const World& w) : w_(w) {}
    // Sets up `action` in direction `dir` (-1 = current) for the actor, from its current state
    // or from `from`. False if the actor or the action does not exist.
    bool init(uint16_t actor, unsigned action, int dir, const ActorState* from = nullptr, int steps_limit = 0);
    // Targeted mode (7.8): spread the gap to this point over the airborne frames.
    void set_target(int x, int y, int z);
    // Where the actor really is now (gravity may have moved it between frames).
    void sync(int x, int y, int z) { x_ = x; y_ = y; z_ = z; }

    // Drops the first n planned frames (at least one stays): Responsive mode continues an
    // interrupted walk at the matching frame index in the new direction.
    void skip(unsigned n) {
        if (frames_.size() > 1 && n > 0) frames_.erase(frames_.begin(), frames_.begin() + long(std::min<size_t>(n, frames_.size() - 1)));
    }
    // Advances to the next frame and moves/validates it. False when the action is over (or
    // stopped: blocked and not unstoppable, or unsupported in a trial).
    bool step();
    // Trial run of the whole action. With `end_support`, a final position that is not within 8
    // units above a surface also counts as "off land" (used for climbs, whose frames are all
    // airborne).
    Result run_all(bool end_support = false);

    int x() const { return x_; }
    int y() const { return y_; }
    int z() const { return z_; }
    int prev_x() const { return px_; }
    int prev_y() const { return py_; }
    int prev_z() const { return pz_; }
    bool blocked() const { return blocked_; }
    bool unsupported() const { return unsupported_; }
    uint16_t hit() const { return hit_; }
    // The frame being shown (after a successful step).
    const AnimFrame* frame() const { return frame_; }
    unsigned frame_index() const { return idx_ > 0 ? frames_[size_t(idx_ - 1)] : 0; }
    unsigned frames_played() const { return unsigned(idx_); }
    unsigned frames_total() const { return unsigned(frames_.size()); }
    bool done() const { return size_t(idx_) >= frames_.size(); }
    const AnimSequence* seq() const { return seq_; }
    unsigned action() const { return action_; }
    int dir() const { return dir_; }
    bool unstoppable() const;
    // The actor's state after the frames played so far.
    ActorState state() const;
    // Total planned travel of the action (forward and z movement summed).
    void travel(int& dx, int& dy, int& dz) const;
    // Savegames (the sequence pointers are found again from the shape and action).
    void save(class SaveWriter& out) const;
    void load(class SaveReader& in);

private:
    bool attack_check();
    const World& w_;
    const AnimSequence* seq_ = nullptr;
    uint16_t actor_ = 0;
    unsigned action_ = 0, shape_ = 0;
    int dir_ = 0;
    std::vector<unsigned> frames_;
    int idx_ = 0;
    int x_ = 0, y_ = 0, z_ = 0, px_ = 0, py_ = 0, pz_ = 0;
    int sx_ = 0, sy_ = 0, sz_ = 0, sxd_ = 0, syd_ = 0, szd_ = 0;  // start box
    uint16_t mflags_ = 0;
    ActorState start_;
    bool flipped_ = false;
    bool blocked_ = false, unsupported_ = false;
    uint16_t hit_ = 0;
    bool targeted_ = false;
    int gap_[3] = {0, 0, 0};
    int air_left_ = 0;
    const AnimFrame* frame_ = nullptr;
};

}  // namespace pg
