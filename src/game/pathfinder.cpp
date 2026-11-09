// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The pathfinder (specs/world/actors.md 8): a best-first search over actor states whose
// moves are walk/advance actions tried against the world, and the process that follows the
// path, replanning when a step no longer works.
#include "game/pathfinder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <queue>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/assist.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"

namespace pg {

void Pathfinder::set_actor(uint16_t actor, const ActorState* from) {
    actor_ = actor;
    const Actor* a = w_.actor(actor);
    if (!a) return;
    if (from) {
        start_ = *from;
    } else {
        start_.x = a->x; start_.y = a->y; start_.z = a->z;
        start_.dir = a->dir;
        start_.last_anim = a->last_anim;
        start_.last_anim_dir = a->last_anim_dir;
        start_.first_step = a->is(Actor::kFirstStep);
        start_.flipped = a->flags & kItemFlipped;
        start_.in_combat = a->is(Actor::kInCombat);
    }
    w_.footprint(*a, xd_, yd_, zd_);
    // Companions keep out of harm's way (unless they already stand in it: then any way out).
    avoid_ = (a->ext_flags & kExtCompanion) != 0 && !assist::hazard(w_, *a, start_.x, start_.y, start_.z);
}

void Pathfinder::set_avoid_hazards(bool on) {
    const Actor* a = w_.actor(actor_);
    if (on && a) avoid_ = !assist::hazard(w_, *a, start_.x, start_.y, start_.z);
}

void Pathfinder::set_target_point(int x, int y, int z) {
    tx_ = x; ty_ = y; tz_ = z;
    target_ = 0;
    hit_ = false;
}

void Pathfinder::set_target_item(uint16_t item, bool hit) {
    const Item* it = w_.item(item);
    target_ = it ? w_.root_container(*it)->id : 0;
    hit_ = hit;
}

void Pathfinder::target_point(int& x, int& y, int& z) const {
    if (target_) {
        if (const Item* it = w_.item(target_)) {
            Box b = w_.box_of(*it);
            x = b.x - b.xd / 2; y = b.y - b.yd / 2; z = b.z;
            return;
        }
    }
    x = tx_; y = ty_; z = tz_;
}

// 8.2: the 8-direction walking distance from the actor's centre to the target.
double Pathfinder::remaining(const ActorState& s) const {
    int x, y, z;
    target_point(x, y, z);
    double dx = std::abs(x - (s.x - xd_ / 2)), dy = std::abs(y - (s.y - yd_ / 2));
    return std::abs(dx - dy) + 1.41421356 * std::min(dx, dy);
}

bool Pathfinder::at_goal(const ActorState& s) const {
    if (!target_) {
        double dx = s.x - tx_, dy = s.y - ty_, dz = s.z - tz_;
        return dx * dx + dy * dy + dz * dz < 2304;
    }
    const Item* t = w_.item(target_);
    if (!t) return false;
    Box tb = w_.box_of(*t);
    if (hit_) {
        // Would an attack from here, facing the target's centre, hit it?
        int dir = direction_to((tb.x - tb.xd / 2) - (s.x - xd_ / 2), (tb.y - tb.yd / 2) - (s.y - yd_ / 2));
        AnimTracker tr(w_);
        ActorState st = s;
        if (!tr.init(actor_, 7, dir, &st)) return false;
        while (tr.step())
            if (tr.hit()) return tr.hit() == target_;
        return false;
    }
    // Item: the gap between the footprints on the larger of x and y (actors.md 8.3).
    int gx = std::max({0, (tb.x - tb.xd) - s.x, (s.x - xd_) - tb.x});
    int gy = std::max({0, (tb.y - tb.yd) - s.y, (s.y - yd_) - tb.y});
    return std::max(gx, gy) <= 32;
}

bool Pathfinder::visited(int x, int y, int z) const {
    for (const auto& v : visited_) {
        int dx = v[0] - x, dy = v[1] - y, dz = v[2] - z;
        if (dx * dx + dy * dy + dz * dz < 64) return true;
    }
    return false;
}

bool Pathfinder::plan(std::vector<PathStep>& out) {
    plan_begin();
    return plan_step(~0u, out) == 1;
}

void Pathfinder::plan_begin() {
    visited_.clear();
    nodes_.clear();
    open_.clear();
    expanded_ = 0;
    state_ = 0;
    Node s0;
    s0.st = start_;
    s0.rank = 3 * remaining(start_);
    nodes_.push_back(s0);
    open_.push_back(0);
    visited_.push_back({start_.x, start_.y, start_.z});
    target_point(tgx_, tgy_, tgz_);
}

int Pathfinder::plan_step(unsigned max_expansions, std::vector<PathStep>& out) {
    out.clear();
    if (state_ != 0) return state_ == 1 ? 1 : -1;
    if (!w_.actor(actor_)) return state_ = -1;
    if (nodes_.size() == 1 && expanded_ == 0 && at_goal(start_)) return state_ = 1;
    std::vector<Node>& nodes = nodes_;
    // A min-heap on rank; equal ranks keep the order std::priority_queue would give them.
    auto cmp = [&nodes](int a, int b) { return nodes[size_t(a)].rank > nodes[size_t(b)].rank; };
    const int tgx = tgx_, tgy = tgy_;
    AnimTracker tr(w_);
    int goal = -1;
    unsigned budget = max_expansions;
    while (!open_.empty()) {
        std::pop_heap(open_.begin(), open_.end(), cmp);
        int ni = open_.back();
        open_.pop_back();
        if (nodes[size_t(ni)].rank == 0 && ni != 0) { goal = ni; break; }
        if (++expanded_ > kMaxExpanded) { open_.clear(); break; }
        const Node cur = nodes[size_t(ni)];
        const unsigned action = cur.st.in_combat ? 8 : 0;
        for (int dir = 0; dir < 8; ++dir) {
            ActorState st = cur.st;
            if (!tr.init(actor_, action, dir, &st)) return state_ = -1;
            // 1-2. Skip directions whose end point (or a point 10 units along) is known.
            int dx, dy, dz;
            tr.travel(dx, dy, dz);
            if (visited(st.x + dx, st.y + dy, st.z + dz)) continue;
            double len = std::sqrt(double(dx) * dx + double(dy) * dy);
            if (len > 20) {
                int px = st.x + int(dx * 10 / len), py = st.y + int(dy * 10 / len);
                if (visited(px, py, st.z)) continue;
            }
            // 3. Step it, remembering the step closest to the target.
            double best = 1e18;
            int best_step = -1;
            ActorState best_state;
            bool stopped = false;
            int steps = 0;
            while (tr.step()) {
                ++steps;
                if ((tr.blocked() && !tr.unstoppable()) || tr.unsupported()) { stopped = true; break; }
                // Companions: every other frame (and the end, below) must keep out of harm's way.
                if (avoid_ && (steps & 1) == 0 && assist::hazard(w_, *w_.actor(actor_), tr.x(), tr.y(), tr.z())) {
                    stopped = true;
                    break;
                }
                double cx = (tr.x() - xd_ / 2) - tgx, cy = (tr.y() - yd_ / 2) - tgy;
                double d = cx * cx + cy * cy;
                if (d < best) { best = d; best_step = steps; best_state = tr.state(); }
            }
            if (!stopped && avoid_ && assist::hazard(w_, *w_.actor(actor_), tr.x(), tr.y(), tr.z())) stopped = true;
            auto add = [&](const ActorState& ns, int nsteps) {
                Node n;
                n.st = ns;
                double mx = ns.x - cur.st.x, my = ns.y - cur.st.y, mz = ns.z - cur.st.z;
                double turn = 0;
                if (ni != 0) {  // the first move from the start pays no turning cost
                    int diff = std::abs(dir - cur.step.dir) % 8;
                    turn = 64.0 * std::min(diff, 8 - diff);
                }
                n.cost = cur.cost + std::sqrt(mx * mx + my * my + mz * mz) + turn;
                n.parent = ni;
                n.step = PathStep{action, dir, nsteps};
                n.depth = cur.depth + 1;
                n.rank = at_goal(ns) ? 0 : 2 * n.cost + 3 * remaining(ns);
                nodes.push_back(n);
                open_.push_back(int(nodes.size() - 1));
                std::push_heap(open_.begin(), open_.end(), cmp);
                visited_.push_back({ns.x, ns.y, ns.z});
            };
            if (!stopped) {
                ActorState ns = tr.state();
                if (!visited(ns.x, ns.y, ns.z)) add(ns, 0);
                else visited_.push_back({ns.x, ns.y, ns.z});
            } else {
                visited_.push_back({tr.x(), tr.y(), tr.z()});
            }
            // 6. A partial move to the closest step.
            if (best_step > 1 && (best_step < steps || (stopped && target_)))
                if (!visited(best_state.x, best_state.y, best_state.z) &&
                    !(avoid_ && assist::hazard(w_, *w_.actor(actor_), best_state.x, best_state.y, best_state.z)))
                    add(best_state, best_step);
        }
        if (--budget == 0) return 0;
    }
    if (goal < 0) return state_ = -1;
    for (int n = goal; n > 0; n = nodes[size_t(n)].parent) out.push_back(nodes[size_t(n)].step);
    std::reverse(out.begin(), out.end());
    return state_ = 1;
}

// ---------------------------------------------------------------------------------------------
// The pathfind process (8.5)

PathfindProcess::PathfindProcess(World& w, uint16_t actor, int x, int y, int z, uint16_t target, bool hit_mode)
    : w_(w), tx_(x), ty_(y), tz_(z), target_(target), hit_(hit_mode) {
    item = actor;
    type = kProcPathfind;
}

void PathfindProcess::goal(int& x, int& y, int& z) const {
    x = tx_; y = ty_; z = tz_;
    if (const Item* t = target_ ? w_.item(target_) : nullptr) w_.location(*t, x, y, z);
}

bool PathfindProcess::replan() {
    Actor* a = w_.actor(item);
    if (!a) return false;
    if (hit_ && !a->is(Actor::kInCombat)) hit_ = false;  // left combat: an ordinary item target
    Pathfinder pf(w_);
    pf.set_actor(item);
    if (avoid_) pf.set_avoid_hazards(true);
    if (target_) {
        const Item* t = w_.item(target_);
        if (!t) return false;
        pf.set_target_item(target_, hit_);
        int z;
        w_.location(*t, target_pos_[0], target_pos_[1], z);
        target_pos_[2] = z;
    } else {
        pf.set_target_point(tx_, ty_, tz_);
    }
    std::vector<PathStep> p;
    if (!pf.plan(p)) return false;
    if (!p.empty()) p.push_back(PathStep{a->is(Actor::kInCombat) ? 15u : 2u, p.back().dir, 0});
    path_ = std::move(p);
    next_ = 0;
    return true;
}

void PathfindProcess::run() {
    Actor* a = w_.actor(item);
    if (!a || a->is(Actor::kDead)) { result = 0; terminate(); return; }
    if (!planned_) {
        planned_ = true;
        if (!replan()) { result = 0; terminate(); return; }
        a->set(Actor::kPathfinding, true);
    }
    if (w_.fast_ready() && !(a->flags & kItemFastArea)) return;  // 2. only in the fast area
    if (target_) {
        const Item* t = w_.item(target_);
        if (!t) { result = 0; terminate(); return; }
        int x, y, z;
        w_.location(*t, x, y, z);
        if (std::abs(x - target_pos_[0]) >= 32 || std::abs(y - target_pos_[1]) >= 32 || std::abs(z - target_pos_[2]) >= 8) {
            if (!replan()) { result = 0; terminate(); return; }
        }
    }
    if (next_ >= path_.size()) { result = 1; terminate(); return; }
    if (a->is(Actor::kAnimLock)) return;
    const PathStep& st = path_[next_];
    bool last_stand = next_ + 1 == path_.size() && (st.action == 2 || st.action == 15);
    if (!last_stand) {
        AnimTracker tr(w_);
        if (!tr.init(item, st.action, st.dir, nullptr, st.steps) || tr.run_all() != AnimTracker::kOk) {
            if (++replans_ > 20 || !replan()) { result = 0; terminate(); return; }
            if (next_ >= path_.size()) { result = 1; terminate(); return; }
        }
    }
    const PathStep& s2 = path_[next_++];
    uint16_t pid = w_.do_anim(item, s2.action, s2.dir, s2.steps);
    if (pid) wait_for(pid);
}

void PathfindProcess::on_terminate() {
    if (Actor* a = w_.actor(item)) a->set(Actor::kPathfinding, false);
}

}  // namespace pg
