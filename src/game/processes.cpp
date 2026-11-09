// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Engine processes: camera, egg hatcher, scheduler, healing, startup, actor animation,
// a simple pathfinder, loiter, sprites, fades, teleports, item deletion, gravity.
#include "game/processes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/bugfix.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/ledge.h"
#include "game/leveling.h"
#include "game/world.h"

namespace pg {

// ---------------------------------------------------------------------------------------------
// Camera

void CameraProcess::start_scroll(int x, int y, int z, int frames) {
    std::copy(pos_, pos_ + 3, from_);
    to_[0] = x; to_[1] = y; to_[2] = z;
    scroll_left_ = scroll_total_ = std::max(1, frames);
}

void CameraProcess::point(int out[3]) const {
    if (follow_) {
        if (const Item* it = w_.item(follow_)) {
            int x, y, z;
            w_.location(*it, x, y, z);
            out[0] = x; out[1] = y; out[2] = z + 20;
            return;
        }
    }
    std::copy(pos_, pos_ + 3, out);
}

void CameraProcess::run() {
    if (w_.quake > 0) {
        int s = w_.quake;
        int ex = w_.rand_below(2 * s + 1) - s, ey = w_.rand_below(2 * s + 1) - s;
        w_.quake_offset = {2 * ex + 4 * ey, -2 * ex + 4 * ey};
    } else {
        w_.quake_offset = {0, 0};
    }
    if (follow_) {
        if (!w_.item(follow_)) follow_ = 1;  // the followed item went away
        return;
    }
    if (scroll_left_ > 0) {
        --scroll_left_;
        int done = scroll_total_ - scroll_left_;
        for (int i = 0; i < 3; ++i) pos_[i] = from_[i] + (to_[i] - from_[i]) * done / scroll_total_;
        if (scroll_left_ == 0) w_.camera_stay(to_[0], to_[1], to_[2]);  // ends this process
    }
}

// ---------------------------------------------------------------------------------------------
// Egg hatcher, scheduler, heal

void EggHatcherProcess::run() { w_.check_eggs(); }

void SchedulerProcess::run() {
    unsigned hour = w_.game_seconds() / 900;
    if (next_actor_ == 0) {
        if (hour <= last_hour) return;
        last_hour = hour;
        next_actor_ = 1;
    }
    while (next_actor_ > 0 && next_actor_ < 256) {
        uint16_t id = uint16_t(next_actor_++);
        Actor* a = w_.actor(id);
        if (!a || a->is(Actor::kDead)) continue;
        uint32_t minutes = w_.game_seconds() / 60;
        uint16_t pid = w_.call_event(id, kEvSchedule,
                                     {uint8_t(minutes), uint8_t(minutes >> 8), uint8_t(minutes >> 16), uint8_t(minutes >> 24)});
        if (pid) wait_for(pid);
        break;  // one actor per frame
    }
    if (next_actor_ >= 256) next_actor_ = 0;
}

void HealProcess::run() {
    Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead)) { terminate(); return; }
    ++counter_;
    if (counter_ == 900 && av->mana < 2 * av->intel) ++av->mana;
    if (counter_ >= 1800) {
        if (av->hp < unsigned(2 * av->str)) ++av->hp;
        counter_ = 0;
        hunger = std::min(200u, hunger + 1);
    }
}

// ---------------------------------------------------------------------------------------------
// Startup (startup.md 3)

void StartupProcess::run() {
    // 1. Intro movie (modal; the world waits behind it).
    if (w_.play_intro && !intro_done_ && !w_.skip_opening) {
        intro_done_ = true;
        if (uint16_t pid = w_.play_movie(World::kMovieIntro, false)) {
            wait_for(pid);
            return;
        }
    }
    if (w_.skip_opening) {  // debug: no opening scene, the Avatar stands up free
        if (const Item* music = w_.map().find_item(562, -1, 11551, 2079, 256)) w_.call_event(music->id, kEvCachein);
        w_.stasis = false;
        w_.do_anim(1, 2, -1);
        w_.startup_state = 1;
        terminate();
        return;
    }
    // 2. The FIRST egg.
    const Item* egg = w_.map().find_item(73, 36, 16188, 7500, 256);
    if (!egg) {
        PG_LOG("startup: FIRST egg not found");
        w_.startup_state = 99;
        terminate();
        return;
    }
    w_.camera_stay(egg->x, egg->y, egg->z);
    if (Item* e = w_.item(egg->id)) e->hatched = true;
    uint16_t first = w_.call_event(egg->id, kEvHatch);
    PG_LOG("startup: hatched FIRST egg %u (pid %u)", egg->id, first);
    // 3. The music egg.
    if (const Item* music = w_.map().find_item(562, -1, 11551, 2079, 256)) {
        uint16_t pid = w_.call_event(music->id, kEvCachein);
        PG_LOG("startup: MUSIC egg %u cachein (pid %u)", music->id, pid);
    }
    // 4. Name entry (modal: the world, with FIRST hatched, waits behind it).
    if (w_.ask_name) w_.open_menu(MenuGump::kName);
    w_.startup_state = 1;
    terminate();
}

// ---------------------------------------------------------------------------------------------
// Actor animation (actors.md 7.3-7.6)

ActorAnimProcess::ActorAnimProcess(World& w, uint16_t actor, unsigned action, int dir, int steps)
    : w_(w), action_(action), dir_(dir), steps_limit_(steps), tr_(std::make_unique<AnimTracker>(w)) {
    item = actor;
    type = kProcActorAnim;
}

// Shows the tracker's next frame on the actor: movement (a forced collide move, so touch
// events fire), shape frame, flags, usecode bit, attack hit. False when the action is over.
bool ActorAnimProcess::apply_frame() {
    Actor* a = w_.actor(item);
    if (!a) return false;
    bool on_map = a->chunk >= 0;
    if (on_map) tr_->sync(a->x, a->y, a->z);
    uint16_t had_hit = tr_->hit();
    if (!tr_->step()) return false;
    const AnimFrame& f = *tr_->frame();
    played_ = true;
    a->frame = f.frame;
    if (f.flipped()) a->flags |= kItemFlipped;
    else a->flags &= uint16_t(~kItemFlipped);
    if (tr_->seq()->flags_a & AnimSequence::kHanging) a->flags |= kItemHanging;
    else a->flags &= uint16_t(~kItemHanging);
    if (tr_->x() != a->x || tr_->y() != a->y || tr_->z() != a->z) {
        if (on_map) w_.collide_move(item, tr_->x(), tr_->y(), tr_->z(), false, true);
        else { a->x = tr_->x(); a->y = tr_->y(); a->z = std::clamp(tr_->z(), 0, 255); }
    }
    if (!w_.actor(item)) return false;
    frame_sounds(f);
    if (f.usecode) w_.call_event(item, kEvCalledFromAnim);
    if (tr_->hit() && !had_hit) w_.receive_hit(tr_->hit(), item, (tr_->dir() + 4) & 7, 0, 0);
    return w_.actor(item) != nullptr;
}

// The frame's sound effect and the special-frame sounds (anim.md "Per-frame effects",
// actors.md 7.4 / 7.7). Ghost and fireskull specials are not sounds and are not done here.
void ActorAnimProcess::frame_sounds(const AnimFrame& f) {
    Actor* a = w_.actor(item);
    if (!a) return;
    SoundSystem& snd = w_.sound;
    if (f.sfx) snd.play_sfx(f.sfx, SoundSystem::kDefaultPriority, item);
    if (!f.special) return;
    if (item == 1 && (action_ == 5 || action_ == 6)) {
        if (w_.equipped(1, 6)) snd.play_sfx(((snd.random_pitch() & 1) ? 0x51 : 0x52), SoundSystem::kDefaultPriority, item);
        return;
    }
    if (a->shape == 411 || a->shape == 413) return;
    if (item == 1 && !w_.options.steps) return;
    // Footstep: by the land under the actor (actors.md 7.7).
    Box b = w_.box_of(*a);
    World::Placement p = w_.can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, w_.shape_flags(*a), item);
    unsigned land = 0;
    if (const Item* li = p.land ? w_.item(p.land) : nullptr) land = li->shape;
    bool running = action_ == 1;
    int sfx;
    switch (land) {
    case 0x03: case 0x04: case 0x09: case 0x0B: case 0x5C: case 0x5E: sfx = 0x2B; break;
    case 0x7E: case 0x80: sfx = 0xCD; break;
    case 0xA1: case 0xA2: case 0xA3: case 0xA4: sfx = running ? 0x99 : 0x91; break;
    default: sfx = running ? 0x97 : 0x90; break;
    }
    snd.play_sfx(sfx, SoundSystem::kDefaultPriority, item, 0, 0x80, snd.random_pitch());
}

void ActorAnimProcess::run() {
    if (!started_) {
        started_ = true;
        Actor* a = w_.actor(item);
        if (!a || a->is(Actor::kAnimLock)) { terminate(); return; }  // one animation at a time
        if (!tr_->init(item, action_, dir_, nullptr, steps_limit_)) { terminate(); return; }
        if (skip_) tr_->skip(skip_);
        if (targeted_) tr_->set_target(target_[0], target_[1], target_[2]);
        a->set(Actor::kAnimLock, true);
        locked_ = true;
        a->dir = uint8_t(tr_->dir());
        a->last_anim = uint16_t(action_);
        a->last_anim_dir = uint16_t(tr_->dir());
        if (!apply_frame()) { terminate(); return; }
        if (tr_->blocked() && !tr_->unstoppable()) { after_block(); terminate(); return; }
        hold_ = int(tr_->seq()->repeat);
        return;
    }
    Actor* a = w_.actor(item);
    if (!a) { terminate(); return; }
    // Leveling (stage 3): some animations play faster or slower (DEX past 25 and heavy weapons for
    // attacks, STR for the Avatar's climbing): 0, 1 or 2 steps this run. Off: always one.
    int steps = 1;
    if (w_.leveling && w_.leveling->active()) {
        const AnimSequence* sq = tr_->seq();
        const int pct = w_.leveling->anim_speed(*a, action_, sq && (sq->flags_a & AnimSequence::kAttack));
        if (pct != 100) {
            speed_acc_ += pct;
            steps = speed_acc_ / 100;
            speed_acc_ %= 100;
        }
    }
    for (int k = 0; k < steps; ++k) {
        if (!step_once()) return;
        if (k + 1 < steps && !(a = w_.actor(item))) { terminate(); return; }
    }
}

bool ActorAnimProcess::step_once() {
    Actor* a = w_.actor(item);
    if (!a) { terminate(); return false; }
    if (w_.fast_ready() && a->chunk >= 0 && !(a->flags & kItemFastArea)) { terminate(); return false; }  // 6.3
    if (hold_ > 0) { --hold_; return true; }
    // The end of this frame's display time: losing support makes the actor fall (7.6).
    if (tr_->unsupported() && !fell_ && a->z > 0 && a->chunk >= 0) {
        fell_ = true;
        w_.hurl(item, tr_->x() - tr_->prev_x(), tr_->y() - tr_->prev_y(), tr_->z() - tr_->prev_z(), 2);
        if (!tr_->unstoppable()) { terminate(); return false; }
    }
    if (tr_->done() || !apply_frame()) { terminate(); return false; }
    if (tr_->blocked() && !tr_->unstoppable()) { after_block(); terminate(); return false; }
    hold_ = int(tr_->seq()->repeat);
    return true;
}

// Blocked while unsupported (and above the ground): fall straight down (7.6).
void ActorAnimProcess::after_block() {
    Actor* a = w_.actor(item);
    if (!a || a->chunk < 0 || a->z <= 0) return;
    Box b = w_.box_of(*a);
    if (!w_.can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, w_.shape_flags(*a), item).supported) w_.hurl(item, 0, 0, 0, 4);
}

void ActorAnimProcess::on_terminate() {
    Actor* a = w_.actor(item);
    if (!a || !locked_) return;
    a->set(Actor::kAnimLock, false);
    const AnimSequence* seq = tr_->seq();
    if (!seq) return;
    if ((seq->flags_a & AnimSequence::kTwoStep) && played_) a->set(Actor::kFirstStep, !a->is(Actor::kFirstStep));
    if (seq->flags_b & 0x80) w_.destroy(item);  // "destroy actor" sequences
}

// ---------------------------------------------------------------------------------------------
// Loiter (actors.md 9.4)

void LoiterProcess::run() {
    Actor* a = w_.actor(item);
    if (!a || a->is(Actor::kDead) || count_ == 0) { terminate(); return; }
    if (stage_ == 0) {
        int x = a->x + 32 * (w_.rand_below(21) - 10), y = a->y + 32 * (w_.rand_below(21) - 10);
        uint16_t pid = w_.pathfind(item, x, y, a->z, 0, 0);
        stage_ = 1;
        if (pid) wait_for(pid);
        return;
    }
    if (stage_ == 1) {
        stage_ = 2;
        bool idle = w_.data().anim.get(a->shape, 47) != nullptr;
        if (idle && w_.rand_below(3) == 0) {
            if (uint16_t pid = w_.do_anim(item, 47 + unsigned(w_.rand_below(2)), -1)) wait_for(pid);
        } else {
            wait_for(kernel()->add(std::make_unique<DelayProcess>(30 * (4 + w_.rand_below(4)))));
        }
        return;
    }
    stage_ = 0;
    if (count_ > 0) --count_;
}

// ---------------------------------------------------------------------------------------------
// Sprites

SpriteProcess::SpriteProcess(World& w, unsigned shape, int first, int last, int repeats, int delay, int x, int y, int z)
    : w_(w), first_(first), last_(last), repeats_(std::max(1, repeats)), delay_(std::max(1, delay)), frame_(first) {
    pos_[0] = x; pos_[1] = y; pos_[2] = z;
    sprite_ = w_.create_item(shape, unsigned(first));
    if (Item* it = w_.item(sprite_)) {
        it->flags |= kItemDisposable;
        it->ext_flags |= kExtSprite;
    }
}

void SpriteProcess::run() {
    if (!created_) {
        created_ = true;
        w_.move_to(sprite_, pos_[0], pos_[1], pos_[2], false);
        wait_ = 2 * delay_;
        return;
    }
    if (--wait_ > 0) return;
    wait_ = 2 * delay_;
    Item* it = w_.item(sprite_);
    if (!it) { terminate(); return; }
    if (frame_ == last_) {
        if (--repeats_ <= 0) { terminate(); return; }
        frame_ = first_;
    } else {
        frame_ += first_ <= last_ ? 1 : -1;
    }
    it->frame = uint16_t(frame_);
}

void SpriteProcess::on_terminate() {
    if (sprite_ && w_.item(sprite_)) w_.destroy_now(sprite_);
    sprite_ = 0;
}

// ---------------------------------------------------------------------------------------------
// Palette fades and the screen inverter

void FadeProcess::run() {
    int done = total_ - left_;
    for (int i = 0; i < 12; ++i)
        w_.palette.m[size_t(i)] = from_.m[size_t(i)] + (to_.m[size_t(i)] - from_.m[size_t(i)]) * done / std::max(1, total_);
    if (--left_ < 0) { w_.palette = to_; terminate(); }
}

void FadeProcess::on_terminate() {
    if (w_.fade_pid == pid()) { w_.fade_pid = 0; w_.fade_priority = 0; }
}

void InvertProcess::run() {
    static const int steps[] = {0, 8, 63, 211, 493, 945, 1594, 2459, 3552, 4870, 6406, 8139, 10042, 12078, 14207,
                                16384, 18561, 20690, 22726, 24629, 26362, 27898, 29216, 30308, 31174, 31823,
                                32274, 32556, 32704, 32760, 32768};
    ++step_;
    if (step_ >= 30) { w_.invert_state = target_; terminate(); return; }
    int from = target_ ? 0 : 0x8000;
    w_.invert_state = target_ ? from + steps[step_] : from - steps[step_];
}

// ---------------------------------------------------------------------------------------------
// Teleport, item deletion, gravity, feign death, combat

void TeleportProcess::run() {
    w_.teleport_to_egg(map_, egg_);
    terminate();
}

void DestroyItemProcess::run() {
    uint16_t id = item;
    item = 0;  // so the kill inside destroy_now does not hit us
    w_.destroy_now(id);
    terminate();
}

GravityProcess::GravityProcess(World& w, uint16_t id, int vx, int vy, int vz, int gravity)
    : w_(w), vx_(vx), vy_(vy), vz_(vz), g_(gravity) {
    item = id;
    type = kProcGravity;
}

void GravityProcess::run() {
    Item* it = w_.item(item);
    if (!it || it->parent || (it->flags & kItemEthereal) || it->chunk < 0) { terminate(); return; }
    it->gravity_pid = pid();
    Actor* a = it->as_actor();
    // Fall height is measured from the peak.
    if (a && (fall_start_ < 0 || a->z > fall_start_)) fall_start_ = a->z;
    if (++frames_ > 1800) { terminate(); return; }  // safety net
    // The Avatar's variant: holding a movement input next to a ledge grabs it (physics.md 7).
    // Milestone 6, Responsive: holding jump grabs too, without movement input.
    bool want_grab = w_.input.moving_input() || (w_.controls.responsive && w_.input.jump_held);
    if (item == 1 && a && !w_.stasis && want_grab && vz_ < 0 && !a->is(Actor::kDead) &&
        w_.input.now_ms >= w_.input.no_grab_until_ms) {
        AnimTracker t(w_);
        if (t.init(1, 22, a->dir) && t.run_all(true) == AnimTracker::kOk) {
            terminate();
            w_.kernel().kill(1, kProcActorAnim);
            if (Actor* av = w_.actor(1)) av->set(Actor::kAnimLock, false);
            w_.do_anim(1, 45, -1);
            return;
        }
        // Easy ledges (game/ledge.h): with jump held, a wider catch: hang points from a little
        // below him to this frame's fall above him, 45 degrees either side, nudged sideways or
        // toward the face, with any of the pull-ups.
        if (w_.controls.responsive && w_.controls.easy_ledges && w_.input.jump_held) {
            int hx, hy, hz, hd, edge[3];
            if (ledge::catch_point(w_, *a, -vz_, hx, hy, hz, hd, edge)) {
                terminate();
                w_.kernel().kill(1, kProcActorAnim);
                w_.move_to(1, hx, hy, hz);
                if (Actor* av = w_.actor(1)) { av->set(Actor::kAnimLock, false); av->dir = uint8_t(hd); }
                w_.ledge_hint.x = edge[0]; w_.ledge_hint.y = edge[1]; w_.ledge_hint.z = edge[2];
                w_.ledge_hint.dir = hd;
                w_.ledge_hint.frames = ledge::kHintFrames;
                w_.do_anim(1, 45, hd);
                return;
            }
        }
        // Milestone 6, Responsive: a fast fall can skip the few units where the climb fits;
        // also try the spots up to this frame's fall above him (he catches the edge on the way).
        if (w_.controls.responsive) {
            int span = std::min(24, std::max(0, -vz_) + 4);
            Box b = w_.box_of(*a);
            for (int d = 2; d <= span; d += 2) {
                if (!w_.can_exist(a->x, a->y, a->z + d, b.xd, b.yd, b.zd, w_.shape_flags(*a), 1).valid) break;
                ActorState st;
                st.x = a->x; st.y = a->y; st.z = a->z + d;
                st.dir = a->dir;
                st.last_anim = 45;
                st.last_anim_dir = a->dir;
                AnimTracker t2(w_);
                if (t2.init(1, 22, a->dir, &st) && t2.run_all(true) == AnimTracker::kOk) {
                    terminate();
                    w_.kernel().kill(1, kProcActorAnim);
                    w_.move_to(1, a->x, a->y, a->z + d);
                    if (Actor* av = w_.actor(1)) av->set(Actor::kAnimLock, false);
                    w_.do_anim(1, 45, -1);
                    return;
                }
            }
        }
    }
    // 1. On the ground and moving down: the fall ends.
    if (it->z <= 0 && vz_ <= 0) { land(); return; }
    // 2. Sweep to the next point.
    int tx = it->x + vx_, ty = it->y + vy_, tz = std::max(0, it->z + vz_);
    uint16_t blocker = 0;
    unsigned dirs = 0;
    int frac = w_.collide_move(item, tx, ty, tz, false, false, &blocker, &dirs);
    if (!(it = w_.item(item))) { terminate(); return; }
    // 3. Free flight.
    if (frac == 0x4000 && !blocker) {
        if (it->z <= 0 && vz_ < 0) { land(); return; }
        vz_ -= g_;
        return;
    }
    const Item* hit = w_.item(blocker);
    // 4. Landed on something (blocked only in z, moving down).
    if (dirs == 4 && vz_ < 0) {
        Box b = w_.box_of(*it);
        World::Placement pl = w_.can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, w_.shape_flags(*it), item);
        if (!pl.supported && hit) {
            // Slide off towards the nearer edge of what it hit.
            Box hb = w_.box_of(*hit);
            int cx = b.x - b.xd / 2, cy = b.y - b.yd / 2;
            vz_ = 0;
            bool pushed = false;
            if (std::abs(vx_) < 16) {
                if (hb.x - cx <= 16) { vx_ = 16; pushed = true; }
                else if (cx - (hb.x - hb.xd) <= 16) { vx_ = -16; pushed = true; }
            } else pushed = true;
            if (std::abs(vy_) < 16) {
                if (hb.y - cy <= 16) { vy_ = 16; pushed = true; }
                else if (cy - (hb.y - hb.yd) <= 16) { vy_ = -16; pushed = true; }
            } else pushed = true;
            if (!pushed) { land(); return; }  // perched in the middle: nowhere to slide
            return;
        }
        bool hit_land = hit && w_.data().typeflags.get(hit->shape).is(ShapeInfo::kLand);
        if (vz_ < -2 && !a && (!hit_land || vz_ < -2 * g_)) {
            // Bounce (U8: non-actors only).
            vz_ = -vz_ / 3;
            int amount = std::abs(vx_) + std::abs(vy_) + vz_;
            double heading = (vx_ || vy_) ? std::atan2(double(vy_), double(vx_)) : 0.0;
            heading += (w_.rand_below(181) - 90) * 3.14159265358979 / 180.0;
            vx_ += int(amount * std::cos(heading));
            vy_ += int(amount * std::sin(heading));
            if (hit_land) {
                vx_ /= 4; vy_ /= 4; vz_ /= 2;
                if (vz_ == 0) { land(); return; }
            } else {
                if (std::abs(vx_) > 2) vx_ /= 2;
                if (std::abs(vy_) > 2) vy_ /= 2;
            }
            it->flags |= kItemBouncing;
            return;
        }
        land();
        return;
    }
    // 5. Walls, ceilings, diagonal contacts: reverse and halve each blocked axis.
    if (dirs & 1) vx_ = -vx_ / 2;
    if (dirs & 2) vy_ = -vy_ / 2;
    if (dirs & 4) vz_ = -vz_ / 2;
    if (!dirs && frac == 0) { land(); return; }
    it->flags |= kItemBouncing;
}

// The fall ends: fall damage and the land animation for living actors (physics.md 7).
void GravityProcess::land() {
    terminate();
    Actor* a = w_.actor(item);
    if (!a || a->is(Actor::kDead)) return;
    // Original bug fixes: a fall out of the world (onto nothing at the bottom) does no harm; the
    // void rescue puts him back where he last stood (game/void_rescue.cpp).
    if (item == 1 && original_bug_fixes() && w_.in_void(*a)) return;
    int height = fall_start_ - a->z;
    if (height >= 80) {
        int dmg = height < 104 ? (height - 72) / 4 : int(a->hp);
        w_.sound.play_sfx(51, 250, item);  // "oof" (physics.md 7)
        w_.receive_hit(item, 0, (a->dir + 4) & 7, dmg, 0x0180);
        if (!(a = w_.actor(item)) || a->is(Actor::kDead)) return;
    }
    if (a->is(Actor::kStunned) || a->last_anim == 14) return;
    // Unstoppable actions (jumps) carry on through the landing.
    if (auto* p = dynamic_cast<ActorAnimProcess*>(w_.kernel().find(item, kProcActorAnim)); p && p->live()) {
        const AnimSequence* s = w_.data().anim.get(a->shape, p->action());
        if (s && (s->flags_a & AnimSequence::kUnstoppable)) return;
    }
    w_.kernel().kill(item, kProcActorAnim);
    if ((a = w_.actor(item))) a->set(Actor::kAnimLock, false);
    uint16_t pid = w_.do_anim(item, 16, -1);
    if (pid && a && a->is(Actor::kInCombat))
        if (uint16_t p2 = w_.do_anim(item, 15, -1))
            if (Process* p = w_.kernel().get(p2)) p->wait_for(pid);
}

void GravityProcess::on_terminate() {
    if (Item* it = w_.item(item))
        if (it->gravity_pid == pid()) {
            it->gravity_pid = 0;
            it->flags &= uint16_t(~kItemBouncing);
        }
}

void ClearFeignProcess::run() {
    Actor* a = w_.actor(item);
    if (!a) { terminate(); return; }
    switch (stage_++) {
    case 0: if (uint16_t p = w_.do_anim(item, 14, -1)) wait_for(p); break;
    case 1: if (uint16_t p = w_.do_anim(item, 4, -1)) wait_for(p); break;
    case 2: wait_for(kernel()->add(std::make_unique<DelayProcess>(900))); break;
    default: a->set(Actor::kFeignDeath, false); terminate(); break;
    }
}

}  // namespace pg
