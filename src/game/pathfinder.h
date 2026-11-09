// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "game/anim_tracker.h"
#include "kernel/kernel.h"

namespace pg {

class World;

// One planned action: play `action` toward `dir`, stopping after `steps` frames (0 = all).
struct PathStep {
    unsigned action = 0;
    int dir = 0;
    int steps = 0;
};

// Best-first search over actor states, each move one walk (or advance, in combat) action
// simulated against the world (specs/world/actors.md 8.1-8.4).
class Pathfinder {
public:
    explicit Pathfinder(const World& w) : w_(w) {}
    void set_actor(uint16_t actor, const ActorState* from = nullptr);
    void set_target_point(int x, int y, int z);
    // An item target; with `hit` the goal is being able to hit it with an attack (8.3).
    void set_target_item(uint16_t item, bool hit);
    // After set_actor: keep out of water, lava and deadly drops as companions do (the Avatar's
    // walk-to-act, NOTES.md "Walk to act"). Ignored if he already stands in harm's way.
    void set_avoid_hazards(bool on);
    // Plans a path (without the final stand). False if none was found within the limits.
    bool plan(std::vector<PathStep>& out);
    // The same search in slices (the critical-path route guide spreads it over frames):
    // plan_begin() then plan_step(n) expands up to n nodes and returns 1 found (the path in
    // `out`), -1 none, 0 not finished yet. plan() is plan_begin() + plan_step(unlimited).
    void plan_begin();
    int plan_step(unsigned max_expansions, std::vector<PathStep>& out);
    unsigned expanded() const { return expanded_; }
    static constexpr unsigned kMaxExpanded = 200;

    // Goal test for a state (also used by the pathfind process to stop early).
    bool at_goal(const ActorState& s) const;

private:
    struct Node {
        ActorState st;
        double cost = 0, rank = 0;
        int parent = -1;
        PathStep step;
        int depth = 0;
    };
    double remaining(const ActorState& s) const;
    bool visited(int x, int y, int z) const;
    void target_point(int& x, int& y, int& z) const;
    const World& w_;
    uint16_t actor_ = 0;
    ActorState start_;
    int tx_ = 0, ty_ = 0, tz_ = 0;
    uint16_t target_ = 0;
    bool hit_ = false;
    bool avoid_ = false;  // a companion: never through water, lava or over a deadly drop (assist::hazard)
    int xd_ = 0, yd_ = 0, zd_ = 0;
    std::vector<std::array<int, 3>> visited_;
    unsigned expanded_ = 0;
    // The search state between plan_step() calls.
    std::vector<Node> nodes_;
    std::vector<int> open_;  // a heap of node indices (lowest rank first)
    int tgx_ = 0, tgy_ = 0, tgz_ = 0;
    int state_ = -1;          // 0 running, 1 found, -1 none / not begun
};

// Drives an actor along a planned path, replanning when a step fails (actors.md 8.5).
// Result 1 = arrived, 0 = failed.
class PathfindProcess : public Process {
public:
    PathfindProcess(World& w, uint16_t actor, int x, int y, int z, uint16_t target, bool hit_mode = false);
    void run() override;
    void on_terminate() override;
    const char* name() const override { return "pathfind"; }
    PG_SAVED_PROCESS("pathfind")
    // Where it is heading (the target item's position or the goal point), for the debug view.
    void goal(int& x, int& y, int& z) const;
    // Plan and replan with Pathfinder::set_avoid_hazards (not saved: the walk-to-act that sets
    // it is not saved either).
    void set_avoid_hazards(bool on) { avoid_ = on; }

private:
    bool replan();
    World& w_;
    int tx_, ty_, tz_;
    uint16_t target_;
    bool hit_;
    bool planned_ = false;
    std::vector<PathStep> path_;
    size_t next_ = 0;
    int target_pos_[3] = {0, 0, 0};
    int replans_ = 0;
    bool avoid_ = false;
};

}  // namespace pg
