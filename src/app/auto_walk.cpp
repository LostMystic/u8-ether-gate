// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Walk to act (an extra, not original; NOTES.md "Walk to act"). A double-click use, or a drag
// of a world item into a pack (onto the Avatar, a companion, a party portrait or a party
// inventory window), on something just out of reach makes the Avatar walk there first, when it
// is within auto_walk_range and the pathfinder finds a way that keeps out of water, lava and
// deadly drops (assist::hazard, as the companions do). On arrival in reach the action happens
// exactly as if he had been there: use_item, or the drop (slider, perform_drop, theft check).
// Any new press, movement, combat, a window or a conversation cancels it; a target that moved
// is re-pathed a few times; otherwise (too far, no safe way) the red cross as before.
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "core/log.h"
#include "game/gumps.h"
#include "game/party.h"
#include "game/pathfinder.h"
#include "game/world.h"

namespace pg {

namespace {
constexpr int kReach = 128;  // World::use_item and the drag's reach
}

bool GameMode::auto_walk_eligible(uint16_t id) const {
    const World& w = *world_;
    if (auto_walk_range <= 0 || w.stasis || atlas_on_ || help_page_ >= 0) return false;
    const Actor* av = w.avatar();
    if (!av || av->is(Actor::kDead) || av->is(Actor::kInCombat) || av->is(Actor::kCombatRun)) return false;
    if (w.gumps.top_modal() || w.ask_menu) return false;
    const Item* it = w.item(id);
    if (!it || id == 1) return false;
    // Only things lying in the world (or in a container lying there): nothing carried by him,
    // by another NPC, or an actor itself (talking has no reach to walk for).
    const Item* r = w.root_container(*it);
    if (!r || r->id == 1 || r->as_actor() || r->chunk < 0) return false;
    Box ab = w.box_of(*av), tb = w.box_of(*r);
    int gx = std::max({0, (ab.x - ab.xd) - tb.x, (tb.x - tb.xd) - ab.x});
    int gy = std::max({0, (ab.y - ab.yd) - tb.y, (tb.y - tb.yd) - ab.y});
    if (std::max(gx, gy) > auto_walk_range) return false;
    return std::abs(tb.z - ab.z) <= 64;
}

bool GameMode::auto_walk_path(uint16_t id) const {
    Pathfinder pf(*world_);
    pf.set_actor(1);
    pf.set_avoid_hazards(true);
    pf.set_target_item(id, false);
    std::vector<PathStep> path;
    return pf.plan(path);
}

bool GameMode::auto_walk_begin(AutoWalk::Kind kind, uint16_t id, const DropAt* at) {
    World& w = *world_;
    const Item* it = w.item(id);
    if (!it) return false;
    if (kind == AutoWalk::kUse && (it->as_actor() || w.can_reach(1, id, kReach))) return false;  // as before
    if (!auto_walk_eligible(id) || !auto_walk_path(id)) {
        PG_LOG("walk to act: %u out of reach and %s", id, auto_walk_eligible(id) ? "no safe way" : "too far");
        return false;
    }
    AutoWalk aw;
    aw.kind = kind;
    aw.target = id;
    if (at) aw.at = *at;
    auto p = std::make_unique<PathfindProcess>(w, 1, 0, 0, 0, id);
    p->set_avoid_hazards(true);
    aw.pid = w.kernel().add(std::move(p));
    if (!aw.pid) return false;
    walk_ = aw;
    PG_LOG("walk to act: walking to %u to %s", id, kind == AutoWalk::kUse ? "use it" : "pick it up");
    return true;
}

void GameMode::auto_walk_cancel(const char* why) {
    if (walk_.kind == AutoWalk::kNone) return;
    World& w = *world_;
    Process* p = w.kernel().get(walk_.pid);
    if (p && p->live() && p->type == kProcPathfind && p->item == 1) w.kernel().kill(1, kProcPathfind);
    PG_LOG("walk to act: %u cancelled (%s)", walk_.target, why);
    walk_ = AutoWalk{};
}

void GameMode::auto_walk_input(const InputEvent& ev) {
    if (walk_.kind == AutoWalk::kNone) return;
    if (ev.type == InputEvent::kMouseDown || ev.type == InputEvent::kPadDown || (ev.type == InputEvent::kKeyDown && !ev.repeat))
        auto_walk_cancel("input");
}

void GameMode::auto_walk_tick() {
    if (walk_.kind == AutoWalk::kNone) return;
    World& w = *world_;
    const Actor* av = w.avatar();
    const PlayerInput& in = w.input;
    const char* why = nullptr;
    if (!av || av->is(Actor::kDead)) why = "no Avatar";
    else if (av->is(Actor::kInCombat) || av->is(Actor::kCombatRun)) why = "combat";
    else if (in.moving_input() || in.button[2].down) why = "movement";
    else if (w.stasis) why = "stasis";
    else if (w.gumps.top_modal() || w.ask_menu) why = "a window or a conversation";
    else if (drag_ != kDragNone || atlas_on_ || help_page_ >= 0) why = "a drag or a menu";
    else if (!w.item(walk_.target)) why = "target gone";
    else if (++walk_.frames > kAutoWalkMaxFrames) why = "took too long";
    if (why) {
        bool gone = std::string(why) == "took too long" || std::string(why) == "target gone";
        auto_walk_cancel(why);
        if (gone) w.refuse("Couldn't get there");
        return;
    }
    Process* p = w.kernel().get(walk_.pid);
    if (p && p->live() && p->type == kProcPathfind && p->item == 1) return;  // still walking
    if (w.can_reach(1, walk_.target, kReach)) {
        AutoWalk done = walk_;
        walk_ = AutoWalk{};
        PG_LOG("walk to act: arrived at %u after %d frames", done.target, done.frames);
        if (done.kind == AutoWalk::kUse) {
            w.use_item(done.target);
        } else if (done.at.gump && !gump_alive(done.at.gump)) {
            w.refuse("The window was closed");
        } else {
            drop_now(done.target, done.at);
        }
        return;
    }
    // Stopped short (the target moved, or the way was blocked): try again a few times.
    if (++walk_.tries <= kAutoWalkRepaths && auto_walk_eligible(walk_.target) && auto_walk_path(walk_.target)) {
        auto np = std::make_unique<PathfindProcess>(w, 1, 0, 0, 0, walk_.target);
        np->set_avoid_hazards(true);
        walk_.pid = w.kernel().add(std::move(np));
        if (walk_.pid) return;
    }
    PG_LOG("walk to act: gave up on %u", walk_.target);
    walk_ = AutoWalk{};
    w.refuse("Couldn't get there");
}

// A drag of an item out of reach: only a pack takes it (the Avatar, a companion, a party
// portrait on the HUD panel, a window of a party member's inventory); elsewhere the red cross.
void GameMode::update_far_drop() {
    World& w = *world_;
    drop_ = kDropNone;
    gump_drop_ = Gump::kDropNo;
    drop_why_ = "Too far away: only your pack or a companion's can take it from here";
    auto party_owned = [&](uint16_t id) {
        const Item* o = w.item(id);
        if (!o) return false;
        const Item* r = w.root_container(*o);
        return r && (r->id == 1 || (w.party && w.party->party_member(r->id)));
    };
    auto into_pack = [&](uint16_t actor) {
        uint16_t pack = w.equipped(actor, 7);
        drop_why_ = pack ? w.put_in_refusal(drag_item_, pack) : "No backpack";
        if (drop_why_.empty()) { drop_ = kDropAvatar; drop_target_ = actor; }
    };
    Gump* g = nullptr;
    bool on_panel = panel_w_ > 0 && mouse_x_ >= 320;
    if (!on_panel) {
        g = w.gumps.at(mouse_x_, mouse_y_);
        if (g && g->modal) g = nullptr;
    }
    bool over_gump = g != nullptr;
    if (g && !party_owned(g->owner)) g = nullptr;
    if (g != drop_gump_ && gump_alive(drop_gump_)) drop_gump_->drag_leave();
    drop_gump_ = g;
    if (on_panel) {
        if (uint16_t id = hud_panel_->portrait_actor(mouse_x_ - 320, mouse_y_)) into_pack(id);
        return;
    }
    if (g) {
        gump_drop_ = g->drag_over(drag_item_, mouse_x_ - g->x, mouse_y_ - g->y, grab_x_, grab_y_);
        return;
    }
    if (over_gump) return;  // someone else's window: the red cross
    uint16_t tid;
    int pt[3];
    if (!world_point(mouse_x_, mouse_y_, tid, pt)) return;
    if (tid == 1 || (w.party && tid != drag_item_ && w.party->party_member(tid))) into_pack(tid);
}

}  // namespace pg
