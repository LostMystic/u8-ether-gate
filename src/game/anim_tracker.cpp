// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Trial and real playback of one actor action (specs/world/actors.md 7.3-7.8,
// specs/world/physics.md 6).
#include "game/anim_tracker.h"

#include <algorithm>
#include <cstdlib>

#include "data/gamedata.h"
#include "game/party.h"
#include "game/world.h"

namespace pg {

bool AnimTracker::init(uint16_t actor, unsigned action, int dir, const ActorState* from, int steps_limit) {
    const Actor* a = w_.actor(actor);
    seq_ = nullptr;
    frames_.clear();
    idx_ = 0;
    blocked_ = unsupported_ = false;
    hit_ = 0;
    targeted_ = false;
    frame_ = nullptr;
    if (!a) return false;
    actor_ = actor;
    action_ = action;
    shape_ = a->shape;
    seq_ = w_.data().anim.get(a->shape, action);
    if (!seq_ || seq_->length == 0) { seq_ = nullptr; return false; }
    if (from) {
        start_ = *from;
    } else {
        int x, y, z;
        w_.location(*a, x, y, z);
        start_.x = x; start_.y = y; start_.z = z;
        start_.dir = a->dir;
        start_.last_anim = a->last_anim;
        start_.last_anim_dir = a->last_anim_dir;
        start_.first_step = a->is(Actor::kFirstStep);
        start_.flipped = a->flags & kItemFlipped;
        start_.in_combat = a->is(Actor::kInCombat);
    }
    dir_ = (dir < 0 || dir > 7) ? start_.dir : dir;
    x_ = px_ = start_.x;
    y_ = py_ = start_.y;
    z_ = pz_ = start_.z;
    flipped_ = start_.flipped;
    mflags_ = w_.data().typeflags.get(a->shape).flags();
    w_.data().typeflags.get(a->shape).footprint(flipped_, sxd_, syd_, szd_);
    sx_ = x_; sy_ = y_; sz_ = z_;

    // 7.3: which frames play.
    const int n = int(seq_->length);
    const bool looping = seq_->flags_a & (AnimSequence::kLooping | AnimSequence::kLooping2);
    int s, e;
    if (seq_->flags_a & AnimSequence::kTwoStep) {
        if (start_.first_step) { s = looping ? n - 1 : 0; e = n / 2; }
        else { s = n / 2; e = looping ? n - 1 : n; }
    } else {
        s = 0;
        e = n;
        if (start_.last_anim == action && start_.last_anim_dir == dir_ && n > 1) s = 1;
    }
    int cur = s;
    frames_.push_back(unsigned(cur));
    for (int guard = 0; guard < 256; ++guard) {
        int next = cur + 1;
        if (next == e) break;
        if (next >= n) next = looping ? 1 : 0;
        if (next == e) break;
        frames_.push_back(unsigned(next));
        cur = next;
    }
    if (steps_limit > 0 && frames_.size() > size_t(steps_limit)) frames_.resize(size_t(steps_limit));
    return true;
}

bool AnimTracker::unstoppable() const { return seq_ && (seq_->flags_a & AnimSequence::kUnstoppable); }

void AnimTracker::travel(int& dx, int& dy, int& dz) const {
    dx = dy = dz = 0;
    if (!seq_) return;
    for (unsigned f : frames_) {
        const AnimFrame& fr = seq_->at(unsigned(dir_), f);
        dx += 4 * dir_x(dir_) * fr.distance;
        dy += 4 * dir_y(dir_) * fr.distance;
        dz += fr.dz;
    }
}

void AnimTracker::set_target(int tx, int ty, int tz) {
    if (!seq_) return;
    int dx, dy, dz;
    travel(dx, dy, dz);
    targeted_ = true;
    gap_[0] = tx - (x_ + dx);
    gap_[1] = ty - (y_ + dy);
    gap_[2] = std::clamp(tz - (z_ + dz), -16, 16);
    air_left_ = 0;
    for (unsigned f : frames_)
        if (!seq_->at(unsigned(dir_), f).on_ground()) ++air_left_;
}

ActorState AnimTracker::state() const {
    ActorState s = start_;
    s.x = x_; s.y = y_; s.z = z_;
    s.dir = uint8_t(dir_);
    s.last_anim = uint16_t(action_);
    s.last_anim_dir = uint16_t(dir_);
    s.flipped = flipped_;
    if (seq_ && (seq_->flags_a & AnimSequence::kTwoStep) && done()) s.first_step = !start_.first_step;
    return s;
}

bool AnimTracker::step() {
    if (!seq_ || done()) return false;
    if (blocked_ && !unstoppable()) return false;
    const AnimFrame& f = seq_->at(unsigned(dir_), frames_[size_t(idx_)]);
    ++idx_;
    frame_ = &f;
    flipped_ = f.flipped();
    px_ = x_; py_ = y_; pz_ = z_;
    int xd, yd, zd;
    w_.data().typeflags.get(shape_).footprint(flipped_, xd, yd, zd);
    const Box start{sx_, sy_, sz_, sxd_, syd_, szd_};

    // 1. Movement (plus the targeted-mode share on airborne frames).
    int dx = 4 * dir_x(dir_) * f.distance, dy = 4 * dir_y(dir_) * f.distance, dz = f.dz;
    if (targeted_ && !f.on_ground() && air_left_ > 0) {
        int ex = gap_[0] / air_left_, ey = gap_[1] / air_left_, ez = gap_[2] / air_left_;
        dx += ex; dy += ey; dz += ez;
        gap_[0] -= ex; gap_[1] -= ey; gap_[2] -= ez;
        --air_left_;
    }
    if (dx || dy || dz) {
        int tx = x_ + dx, ty = y_ + dy, tz = std::max(0, z_ + dz);
        // 3. Large steps are swept first.
        if (std::abs(dx) >= xd - 8 || std::abs(dy) >= yd - 8 || std::abs(dz) >= zd - 8) {
            std::vector<SweepHit> hits;
            w_.sweep(Box{x_, y_, z_, xd, yd, zd}, tx, ty, tz, mflags_, actor_, true, hits);
            for (const SweepHit& h : hits) {
                if (!h.blocking || h.touching || h.hit_time < 0) continue;
                // Stop at the point of impact.
                w_.contact_point(Box{x_, y_, z_, xd, yd, zd}, tx, ty, tz, h, x_, y_, z_);
                z_ = std::max(0, z_);
                blocked_ = true;
                break;
            }
        }
        if (!blocked_) {
            // 4. Placement at the target; 5. the step scan for on-ground frames.
            World::Placement pl = w_.can_exist(tx, ty, tz, xd, yd, zd, mflags_, actor_, &start);
            bool ok = pl.valid && (!f.on_ground() || pl.supported);
            if (!ok) {
                if (f.on_ground() && zd > 8) {
                    int hx = 0, hy = 0;
                    switch (dir_ & 7) {
                    case 0: case 4: hx = 1; break;
                    case 2: case 6: hy = 1; break;
                    case 1: case 5: hx = 1; hy = 1; break;
                    default: hx = 1; hy = -1; break;
                    }
                    static const int side[3] = {0, -4, 4}, up[5] = {0, -4, 4, -8, 8};
                    bool found = false, first = false;
                    int fx = 0, fy = 0, fz = 0;
                    for (int s : side) {
                        for (int u : up) {
                            int cx = tx + hx * s, cy = ty + hy * s, cz = tz + u;
                            if (cz < 0) continue;
                            World::Placement c = w_.can_exist(cx, cy, cz, xd, yd, zd, mflags_, actor_, &start);
                            if (!c.valid) continue;
                            if (c.support_z == cz || cz == 0) { fx = cx; fy = cy; fz = cz; found = true; break; }
                            if (!first) { first = true; fx = cx; fy = cy; fz = cz; }
                        }
                        if (found) break;
                    }
                    if (found || first) { tx = fx; ty = fy; tz = fz; ok = true; }
                    else blocked_ = true;
                } else if (!pl.valid) {
                    blocked_ = true;
                } else {
                    ok = true;  // airborne or short: unsupported is allowed
                }
            }
            if (ok) { x_ = tx; y_ = ty; z_ = tz; }
        }
    }
    // 6. Attack frames look for a victim (one hit per action).
    if (!hit_ && (seq_->flags_a & AnimSequence::kAttack) && f.attack_range() > 0) attack_check();
    // 7. On-ground frames need support at the final position.
    if (f.on_ground() && z_ > 0 && !w_.can_exist(x_, y_, z_, xd, yd, zd, mflags_, actor_, &start).supported)
        unsupported_ = true;
    return true;
}

bool AnimTracker::attack_check() {
    const AnimFrame& f = *frame_;
    int xd, yd, zd;
    w_.data().typeflags.get(shape_).footprint(flipped_, xd, yd, zd);
    int r = 32 * int(f.attack_range());
    Box ab{x_ + r * dir_x(dir_), y_ + r * dir_y(dir_), z_, xd, yd, zd};
    std::vector<uint16_t> near;
    w_.area_search(x_, y_, 320, false, near);
    for (uint16_t id : near) {
        if (id == actor_) continue;
        const Actor* o = w_.actor(id);
        if (!o || o->is(Actor::kDead)) continue;
        if (w_.same_party(actor_, id)) continue;  // companions: party members are not hit
        if (w_.party && w_.party->untouchable(id)) continue;  // nor those not yet recruited
        Box ob = w_.box_of(*o);
        if (ab.x - ab.xd < ob.x && ab.x > ob.x - ob.xd && ab.y - ab.yd < ob.y && ab.y > ob.y - ob.yd &&
            ab.z < ob.z + ob.zd && ab.z + ab.zd > ob.z) {
            hit_ = id;
            return true;
        }
    }
    return false;
}

AnimTracker::Result AnimTracker::run_all(bool end_support) {
    if (!seq_) return kFail;
    while (step()) {
        if (blocked_ && !unstoppable()) return kFail;
        if (unsupported_) return kOffLand;
    }
    if (blocked_ && !unstoppable()) return kFail;
    if (unsupported_) return kOffLand;
    if (end_support && z_ > 0) {
        int xd, yd, zd;
        w_.data().typeflags.get(shape_).footprint(flipped_, xd, yd, zd);
        World::Placement pl = w_.can_exist(x_, y_, z_, xd, yd, zd, mflags_, actor_);
        if (!pl.valid || pl.support_z < z_ - 8) return kOffLand;
        // Where he would settle must be properly supported (land under the centre), so he
        // cannot climb onto a head or a winch.
        if (pl.support_z != z_ && !w_.can_exist(x_, y_, pl.support_z, xd, yd, zd, mflags_, actor_).supported) return kOffLand;
        if (pl.support_z == z_ && !pl.supported) return kOffLand;
    }
    return kOk;
}

}  // namespace pg
