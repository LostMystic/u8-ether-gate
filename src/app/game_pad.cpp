// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6 (PLAN-M6.md; the engine's own design): controller input in the game mode
// (the left stick moves, buttons jump / use / look / open windows, the D-pad zooms and cycles
// the auto-target, the right stick drives a virtual pointer), the auto-target, the answer
// menu on the pad, movie skip prompts, and click-through for barks with fast-forward.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "app/atlas_view.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/ask_nav.h"
#include "game/container_sort.h"
#include "game/credits.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/movie.h"
#include "game/processes.h"
#include "gfx/surface.h"

namespace pg {

namespace {
constexpr int kStickDead = 8000;       // radial dead zone of the sticks (of 32767)
constexpr int kStickRun = 27000;       // beyond this the left stick runs
constexpr int kTriggerOn = 12000;
constexpr int kPointerFrames = 150;    // the virtual pointer stays active 5 s after use
constexpr int kTargetRange = 448;      // auto-target search radius (world units)
constexpr int kBackHoldFrames = 30;    // holding Back 1 s opens the help page
constexpr int kMinPageFrames = 5;      // fast-forward shows every bark page at least ~150 ms

// World direction 0..7 of a stick / screen vector (dx right, dy up), in even 45-degree
// sectors (the pointer's rule without its 2:1 correction).
int stick_dir(int dx, int dy) {
    int sdir;
    if (dx == 0) sdir = dy > 0 ? 0 : 4;
    else {
        long t = (1024L * dy) / dx;
        if (dx < 0) t = -t;
        int right = t > 2472 ? 0 : t > 424 ? 1 : t >= -424 ? 2 : t >= -2472 ? 3 : 4;
        sdir = dx > 0 ? right : (8 - right) & 7;
    }
    return (sdir + 7) & 7;
}
}  // namespace

bool GameMode::pointer_active() const {
    const World& w = *world_;
    return pads_ > 0 && (pointer_frames_ < kPointerFrames || w.gumps.top_modal() || drag_ != kDragNone);
}

void GameMode::synth_mouse(int button, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
    e.button = button;
    e.x = mouse_x_;
    e.y = mouse_y_;
    e.fx = mouse_fx_;
    e.fy = mouse_fy_;
    handle(e);
}

bool GameMode::scene_running() const {
    const World& w = *world_;
    return w.stasis && !w.gumps.top_modal();
}

bool GameMode::fast_forward() const {
    const World& w = *world_;
    return text_hold_ && w.controls.click_text && w.stasis && !w.ask_menu && !w.gumps.top_modal() &&
           text_hold_frames_ >= kTextGuardFrames && drag_ == kDragNone;
}

// Ends the current page of every bark on screen (anywhere) or of the bark under the pointer.
// The bark then shows its next page or finishes as if its time had run out, so the waiting
// usecode carries on normally.
bool GameMode::bark_click(bool anywhere) {
    World& w = *world_;
    // One advance per press, and each page is immune for kTextGuardFrames after it appeared
    // (a press meant for the previous page, its release, the second click of a double-click
    // or a key repeat cannot skip the new one).
    auto hit = [&](const Bark& b) {
        if (anywhere) return true;
        for (const BarkRect& r : bark_rects_)
            if (r.speaker == b.speaker && mouse_x_ >= r.x - 2 && mouse_x_ < r.x + r.w + 2 && mouse_y_ >= r.y - 2 &&
                mouse_y_ < r.y + r.h + 2)
                return true;
        return false;
    };
    auto old_enough = [&](const Bark& b) {
        auto it = bark_seen_.find(b.speaker);
        return it != bark_seen_.end() && it->second.first == b.page && frames_ - it->second.second >= kTextGuardFrames;
    };
    bool any = false, guarded = false;
    for (const Bark& b : w.barks)
        if (hit(b) && b.ticks_left > 0) {
            any = true;
            if (!old_enough(b)) guarded = true;
        }
    if (!any) return false;
    for (Bark& b : w.barks)
        if (hit(b) && b.ticks_left > 0 && old_enough(b)) {
            b.ticks_left = 0;
            ++pages_skipped_;
        }
    (void)guarded;
    return true;  // the press was on the text: it does nothing else, even when guarded
}

// Per tick: when each bark page appeared; while a click-through button is held, pages end
// as soon as they have been up for the minimum time.
void GameMode::bark_tick() {
    World& w = *world_;
    if (text_hold_) ++text_hold_frames_;
    else text_hold_frames_ = 0;
    for (auto it = bark_seen_.begin(); it != bark_seen_.end();) {
        bool live = false;
        for (const Bark& b : w.barks) live |= b.speaker == it->first;
        it = live ? std::next(it) : bark_seen_.erase(it);
    }
    for (Bark& b : w.barks) {
        auto it = bark_seen_.find(b.speaker);
        if (it == bark_seen_.end() || it->second.first != b.page) {
            bark_seen_[b.speaker] = {b.page, frames_};
            continue;
        }
        if (text_hold_ && w.controls.click_text && scene_running() && text_hold_frames_ >= kTextGuardFrames &&
            frames_ - it->second.second >= kMinPageFrames && b.ticks_left > 0) {
            b.ticks_left = 0;
            ++pages_skipped_;
        }
    }
}

// A new answer menu: with a controller its first answer is highlighted at once, otherwise
// nothing is until an arrow key is pressed.
void GameMode::ask_sync() {
    World& w = *world_;
    if (!w.ask_menu) { ask_sel_ = -1; ask_keys_ = false; return; }
    if (w.ask_menu->pid != ask_pid_) {
        ask_pid_ = w.ask_menu->pid;
        ask_sel_ = pads_ > 0 && !w.ask_menu->answers.empty() ? 0 : -1;
        ask_keys_ = false;
        // A direction still held from before must not move the new menu's highlight at once.
        menu_stick_dir_ = -2;  // wait until the stick is back in the middle
        menu_dpad_dir_ = -1;
    }
}

void GameMode::ask_nav(int d) {
    World& w = *world_;
    if (!w.ask_menu) return;
    ask_sel_ = ask_move(w.ask_menu->buttons, ask_sel_, AskDir(d));
    pointer_frames_ = kPointerFrames;  // the highlight, not the pointer, is in use now
}

int GameMode::ask_exit() const {
    World& w = *world_;
    if (!w.ask_menu) return -1;
    std::vector<std::string> labels;
    for (uint16_t sid : w.ask_menu->answers) labels.push_back(w.vm().string(sid));
    return ask_exit_index(labels);
}

void GameMode::choose_ask_selection() {
    World& w = *world_;
    if (w.ask_menu && ask_sel_ >= 0 && size_t(ask_sel_) < w.ask_menu->answers.size()) {
        unsigned i = unsigned(ask_sel_);
        ask_sel_ = -1;
        last_pad_answer_ = int(i);
        w.choose_answer(i);
    }
}

// ---------------------------------------------------------------------------------------------
// Auto-target

void GameMode::update_targets() {
    World& w = *world_;
    targets_.clear();
    const Actor* av = w.avatar();
    if (!av || av->chunk < 0) { target_ = 0; return; }
    bool combat = av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    std::vector<uint16_t> near;
    w.area_search(av->x, av->y, kTargetRange, false, near);
    struct Cand { uint16_t id; double score, angle; };
    std::vector<Cand> c;
    const double face = std::atan2(double(dir_y(av->dir)), double(dir_x(av->dir)));
    const UsecodeFile& uc = data_.usecode;
    for (uint16_t id : near) {
        if (id == 1) continue;
        const Item* it = w.item(id);
        if (!it || it->parent || it->chunk < 0 || !(it->flags & kItemFastArea) || (it->flags & kItemInvisible)) continue;
        const ShapeInfo& si = data_.typeflags.get(it->shape);
        if (si.is(ShapeInfo::kEditor)) continue;
        const Actor* a = it->as_actor();
        bool usable;
        if (a) usable = combat ? !a->is(Actor::kDead) : true;
        else {
            if (combat) continue;
            int cls = w.usecode_class(*it);
            usable = cls >= 0 && uc.event_entry(unsigned(cls), kEvUse) != 0;
        }
        if (!usable) continue;
        int sx, sy;
        w.screen_point(*it, sx, sy);
        if (sx < 0 || sx >= 320 || sy < 0 || sy >= 220) continue;
        double dx = it->x - av->x, dy = it->y - av->y;
        double dist = std::sqrt(dx * dx + dy * dy);
        double ang = std::atan2(dy, dx);
        double off = std::abs(std::remainder(ang - face, 2 * M_PI));
        // Nearest in front: distance, with items behind him counting up to three times as far.
        c.push_back({id, dist * (1.0 + 2.0 * off / M_PI) + (a ? 0 : 16), ang});
    }
    if (c.empty()) { target_ = 0; return; }
    // Cycling order: clockwise round the Avatar (screen-wise), starting in front of him.
    std::sort(c.begin(), c.end(), [&](const Cand& a, const Cand& b) {
        double ra = std::remainder(a.angle - face, 2 * M_PI), rb = std::remainder(b.angle - face, 2 * M_PI);
        return ra < rb;
    });
    for (const Cand& k : c) targets_.push_back(k.id);
    bool keep = target_ && frames_ - target_pick_frame_ < 300 &&
                std::find(targets_.begin(), targets_.end(), target_) != targets_.end();
    if (!keep) {
        const Cand* best = &c[0];
        for (const Cand& k : c)
            if (k.score < best->score) best = &k;
        target_ = best->id;
    }
}

// ---------------------------------------------------------------------------------------------
// Controller events

void GameMode::pad_event(const InputEvent& ev) {
    World& w = *world_;
    PlayerInput& in = w.input;
    if (ev.type == InputEvent::kPadAdded) {
        ++pads_;
        pad_waiting_.insert(ev.pad);  // until its first button press
        pad_name_ = ev.name[0] ? ev.name : "controller";
        toast("Controller connected: " + pad_name_ + "  (press a button; hold Back: help)", 120);
        return;
    }
    if (ev.type == InputEvent::kPadRemoved) {
        pads_ = std::max(0, pads_ - 1);
        pad_waiting_.erase(ev.pad);
        std::memset(pad_btn_, 0, sizeof pad_btn_);
        pad_axes_by_id_.erase(ev.pad);
        std::memset(pad_axis_, 0, sizeof pad_axis_);
        for (const auto& kv : pad_axes_by_id_)
            for (int i = 0; i < kPadAxes; ++i)
                if (std::abs(kv.second[size_t(i)]) > std::abs(pad_axis_[i])) pad_axis_[i] = kv.second[size_t(i)];
        in.pad_move = in.pad_block = in.pad_jump = false;
        for (int i = 0; i < 2; ++i)
            if (pointer_buttons_[i]) { pointer_buttons_[i] = false; synth_mouse(i == 0 ? 1 : 3, false); }
        text_hold_ = false;
        toast(pads_ ? "Controller disconnected" : "Controller disconnected: keyboard and mouse", 120);
        return;
    }
    if (pads_ == 0) pads_ = 1;  // events without an "added" (a pad that was there at start)
    // A controller counts only after a real button press (a phantom or idle pad, such as a
    // browser's virtual one, must not act): that first press only wakes it.
    if (pad_waiting_.count(ev.pad)) {
        if (ev.type == InputEvent::kPadDown) {
            pad_waiting_.erase(ev.pad);
            PG_LOG("controller %d: first button press, now active", ev.pad);
        }
        return;
    }
    // --- The HUD side panel (app/hud_panel.h): R3 (the right stick's press) gives it the focus;
    // while it has it, it takes the controller (B or R3 give the focus back).
    if (panel_w_ > 0 && hud_panel_ && !atlas_on_ && help_page_ < 0) {
        if (hud_panel_->focused() && !w.gumps.top_modal() && hud_panel_->pad(ev)) return;
        if (ev.type == InputEvent::kPadDown && ev.button == kPadRStick && !w.gumps.top_modal()) {
            hud_panel_->set_focus(true);
            return;
        }
    }
    // --- End of the side panel block ---------------------------------------------------------
    // The level-up prompt (game/leveling.h): L3 gives it the focus; only its own buttons are taken,
    // so the Avatar keeps walking under it.
    if (auto* lp = w.gumps.find<LevelPromptGump>(); lp && ev.type == InputEvent::kPadDown && !w.gumps.top_modal()) {
        if (ev.button == kPadLStick) { lp->pad_focus = !lp->pad_focus; return; }
        if (lp->pad_focus) {
            if (ev.button == kPadLeft) { lp->selected = (lp->selected + 2) % 3; return; }
            if (ev.button == kPadRight) { lp->selected = (lp->selected + 1) % 3; return; }
            if (ev.button == kPadA) { w.leveling->spend_point(lp->selected); lp->pad_focus = false; return; }
            if (ev.button == kPadB) { lp->pad_focus = false; return; }
        }
    }
    // --- Turn-based combat (app/turn_ui.h; owned by the turn-based combat feature) -----------
    // During a turn-based fight the action menu, picking, Party Auto (RB) and the speed-up (A
    // held) take the controller first; whatever it does not use falls through to the code below.
    if (turn_ui_ && turn_ui_->pad(ev)) return;
    // --- End of the turn-based combat block -----------------------------------------------
    if (ev.type == InputEvent::kPadAxis) {
        int a = std::clamp(ev.axis, 0, kPadAxes - 1);
        int before = pad_axis_[a];
        // Several controllers: each keeps its own sticks; the strongest deflection counts (so
        // a resting pad does not cancel the one in use).
        auto& mine = pad_axes_by_id_[ev.pad];
        mine[size_t(a)] = ev.value;
        int best = 0;
        for (const auto& kv : pad_axes_by_id_)
            if (std::abs(kv.second[size_t(a)]) > std::abs(best)) best = kv.second[size_t(a)];
        pad_axis_[a] = best;
        // The right trigger: attack (combat), or fast-forward a scene while held.
        if (a == kAxisRT) {
            bool on = ev.value > kTriggerOn, was = before > kTriggerOn;
            if (on && !was && !atlas_on_ && help_page_ < 0) {
                const Actor* av = w.avatar();
                if (scene_running() && !w.barks.empty()) { bark_click(true); text_hold_ = true; }
                else if (av && av->is(Actor::kInCombat)) in.attack_buffer_ms = std::max<uint32_t>(1, in.now_ms);
            }
            if (!on && was) text_hold_ = false;
            if (atlas_on_ && atlas_ && on && !was) atlas_->zoom_step(-1);
        }
        if (a == kAxisLT && atlas_on_ && atlas_ && ev.value > kTriggerOn && before <= kTriggerOn) atlas_->zoom_step(1);
        return;
    }
    const int b = std::clamp(ev.button, 0, kPadButtons - 1);
    const bool down = ev.type == InputEvent::kPadDown;
    pad_btn_[b] = down;
    if (down) pad_press_frame_[b] = frames_;

    // Releases.
    if (!down) {
        if (b == kPadA || b == kPadB) {
            int mb = b == kPadA ? 0 : 1;
            if (pointer_buttons_[mb]) { pointer_buttons_[mb] = false; synth_mouse(mb == 0 ? 1 : 3, false); }
        }
        if (b == kPadA) text_hold_ = false;
        if (b == kPadLB && lb_down_) {
            // A tap of LB: the paperdoll (LB held with the D-pad picked a party member).
            if (!lb_used_ && !w.gumps.top_modal() && !atlas_on_ && help_page_ < 0 && !w.stasis) w.call_event(1, kEvUse);
            lb_down_ = false;
        }
        if (b == kPadStart && start_down_frame_ >= 0) {
            // A short press of Start: the main menu (a long one opened the debug menu).
            if (!start_long_ && !w.gumps.top_modal() && !atlas_on_ && help_page_ < 0) w.open_menu(MenuGump::kMenu);
            start_down_frame_ = -1;
            start_long_ = false;
        }
        if (b == kPadBack) {
            if (!back_help_ && back_down_frame_ >= 0) {
                if (atlas_on_) close_atlas();
                else if (help_page_ >= 0) help_page_ = -1;
                else if (!w.gumps.top_modal()) open_atlas();
            }
            back_down_frame_ = -1;
            back_help_ = false;
        }
        return;
    }
    if (b == kPadBack) { back_down_frame_ = frames_; back_help_ = false; return; }

    // The atlas.
    if (atlas_on_) {
        if (b == kPadB) close_atlas();
        else if (b == kPadUp && atlas_) atlas_->zoom_step(-1);
        else if (b == kPadDown && atlas_) atlas_->zoom_step(1);
        return;
    }
    // The help page.
    if (help_page_ >= 0) {
        if (b == kPadRight || b == kPadA || b == kPadRB) help_page_ = (help_page_ + 1) % help_pages();
        else if (b == kPadLeft || b == kPadLB) help_page_ = (help_page_ + help_pages() - 1) % help_pages();
        else help_page_ = -1;
        return;
    }
    auto key = [&](int k) {
        InputEvent e;
        e.type = InputEvent::kKeyDown;
        e.key = k;
        key_down(e);
        e.type = InputEvent::kKeyUp;
        handle(e);
    };
    Gump* modal = w.gumps.top_modal();
    // Movies and the credits: Start, A or B open the skip prompt; then A = Yes, B = No.
    if (modal) {
        bool prompt = false, skippable = false;
        if (auto* m = dynamic_cast<MovieGump*>(modal)) { prompt = m->prompt_open(); skippable = true; }
        if (auto* c = dynamic_cast<CreditsGump*>(modal)) { prompt = c->prompt_open(); skippable = true; }
        if (skippable) {
            if (prompt && b == kPadA) key('y');
            else if (prompt && b == kPadB) key('n');
            else if (!prompt && (b == kPadStart || b == kPadA || b == kPadB)) key(kKeyEscape);
            return;
        }
        // Other windows (the menu book, readers, the quit box, the slider): the pointer with
        // A as the left button, B / Start = Esc, the D-pad = arrow keys.
        // Every book with pages (the options page, Read / Write Diary, books and scrolls in the
        // world): LB the previous page, RB the next (PgUp / PgDn).
        if (b == kPadLB || b == kPadRB) {
            auto* book = dynamic_cast<MenuGump*>(modal);
            auto* reader = dynamic_cast<ReaderGump*>(modal);
            if ((book && book->paged()) || (reader && reader->paged())) key(b == kPadLB ? kKeyPageUp : kKeyPageDown);
            return;
        }
        // The options page: A changes the selected row.
        if (auto* book = dynamic_cast<MenuGump*>(modal); book && book->mode() == MenuGump::kOptions) {
            if (b == kPadA) { key(kKeyReturn); return; }
        }
        // Elsewhere A activates what is highlighted (Enter; a reader turns the page, the quit
        // box says yes), or clicks where the pointer is while the right stick is in use.
        if (b == kPadA) {
            if (pointer_frames_ < kPointerFrames && !pointer_on_target_) { pointer_buttons_[0] = true; synth_mouse(1, true); return; }
            if (dynamic_cast<QuitGump*>(modal)) key('y');
            else if (dynamic_cast<ReaderGump*>(modal)) key(kKeySpace);
            else key(kKeyReturn);
            return;
        }
        if (b == kPadB || b == kPadStart) { key(kKeyEscape); return; }
        if (b == kPadLeft) key(kKeyLeft);
        if (b == kPadRight) key(kKeyRight);
        if (b == kPadUp) key(kKeyUp);
        if (b == kPadDown) key(kKeyDown);
        if (b == kPadY) key(kKeyReturn);
        return;
    }
    // The answer menu takes priority over everything else: the D-pad moves the highlight by
    // where the answers are drawn (the first answer is highlighted from the start), A answers
    // the highlighted one (or the one under the pointer while the right stick is in use), B
    // the menu's back / bye answer (game/ask_nav.h). A press in the first moment after the
    // menu appeared is ignored (one meant for the line before); B waits the full 0.5 s.
    if (w.ask_menu && !w.ask_menu->answers.empty()) {
        ask_sync();
        int n = int(w.ask_menu->answers.size());
        if (ask_sel_ < 0 || ask_sel_ >= n) ask_sel_ = 0;
        if (b == kPadUp || b == kPadDown || b == kPadLeft || b == kPadRight) {
            int d = b == kPadUp ? kAskUp : b == kPadDown ? kAskDown : b == kPadLeft ? kAskLeft : kAskRight;
            ask_nav(d);
            menu_dpad_dir_ = d;
            menu_dpad_next_ = now_ms() + kMenuRepeatFirstMs;
            return;
        }
        if (b == kPadA) {
            unsigned idx;
            if (pointer_frames_ < kPointerFrames && !pointer_on_target_ && ask_hit(mouse_x_, mouse_y_, idx)) ask_sel_ = int(idx);
            if (w.ask_menu->frames_open >= kMenuGuardFrames) choose_ask_selection();
            return;
        }
        if (b == kPadB) {
            int e = ask_exit();
            if (e >= 0) {
                if (w.ask_menu->frames_open >= kTextGuardFrames) { ask_sel_ = e; choose_ask_selection(); }
                return;
            }
            // No back / bye answer: B picks nothing (it still closes a window, if one is open).
        }
    }
    // v2.12 hotkeys on the controller (LB held, like the party pick): D-pad Up quick-save, Down
    // quick-load; B bedroll, Y recall item, X keyring. (Not while a party member is picked: then
    // Y / B belong to the party bar.)
    if (lb_down_ && (b == kPadUp || b == kPadDown || ((b == kPadB || b == kPadY || b == kPadX) &&
                                                      !(w.party && w.party->pad_highlight >= 0)))) {
        lb_used_ = true;
        if (b == kPadUp) quick_save();
        else if (b == kPadDown) quick_load();
        else use_first_item(b == kPadB ? kBedroll : b == kPadY ? kRecall : kKeyring);
        return;
    }
    // [container sort binding] X with the virtual pointer over a container window sorts it.
    if (pointer_active() && b == kPadX && drag_ == kDragNone)
        if (auto* g = dynamic_cast<ContainerGump*>(w.gumps.at(mouse_x_, mouse_y_)); g && g->can_sort()) {
            sort_container(w, *g);
            return;
        }
    const Actor* av = w.avatar();
    bool combat = av && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    // The party bar: LB held + D-pad Left / Right picks a member; A or Y opens his windows.
    Party* party = w.party.get();
    if (party && lb_down_ && (b == kPadLeft || b == kPadRight)) {
        int n = int(party->bar_ids().size());
        int cur = party->pad_highlight;
        party->pad_highlight = cur < 0 ? (b == kPadRight ? std::min(1, n - 1) : n - 1) : (cur + (b == kPadRight ? 1 : n - 1)) % n;
        party_pick_frame_ = frames_;
        lb_used_ = true;
        return;
    }
    if (party && party->pad_highlight >= 0 && (b == kPadA || b == kPadY)) {
        party->open_bar_slot(party->pad_highlight);
        party->pad_highlight = -1;
        return;
    }
    if (party && party->pad_highlight >= 0 && b == kPadB) { party->pad_highlight = -1; return; }
    (void)combat;
    switch (b) {
    case kPadA:
        // A is the left mouse button on the highlight: the auto-target (the pointer jumps onto
        // it) or whatever is under the pointer while the right stick is aiming it. It goes
        // through the mouse's own press / release, so a tap is a click (look: an NPC says his
        // name) and two taps within the double-click time are a double-click (use: an NPC
        // talks; in combat: attack); hold and move the pointer = drag. In a scene: the next line.
        if (w.controls.click_text && !w.barks.empty() && scene_running()) {
            bark_click(true);
            text_hold_ = true;
            break;
        }
        // Each press snaps onto the highlight again (it may have walked off, or been cycled),
        // unless the right stick has moved the pointer since the last snap.
        if (target_ && (!pointer_active() || pointer_on_target_)) {
            pointer_to_target();
            pointer_on_target_ = true;
        }
        pointer_frames_ = 0;
        pointer_buttons_[0] = true;
        synth_mouse(1, true);
        break;
    case kPadX:
        in.pad_jump = true;
        in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
        break;
    case kPadB:
        // B closes windows: the one under the active pointer, else the newest; with none open
        // it cancels a drag or the target cursor.
        if (drag_ != kDragNone) { cancel_drag(); break; }
        {
            Gump* g = pointer_active() ? w.gumps.at(mouse_x_, mouse_y_) : nullptr;
            if (g && (g->modal || std::string(g->kind()) == "partybar")) g = nullptr;
            if (!g) g = w.gumps.newest_window();
            if (g) {
                g->close();
                w.gumps.reap();
                break;
            }
        }
        if (w.target_pid)
            if (Process* p = w.kernel().get(w.target_pid)) { p->result = 0; p->terminate(); }
        break;
    case kPadY:
        if (!w.stasis) if (uint16_t p = w.equipped(1, 7)) w.call_event(p, kEvUse);
        break;
    case kPadLB: lb_down_ = true; lb_used_ = false; break;  // acts on release (or with the D-pad)
    case kPadRB: toggle_combat(true); break;
    case kPadStart: start_down_frame_ = frames_; start_long_ = false; break;  // acts on release / hold
    case kPadUp: zoom_by(-1); break;
    case kPadDown: zoom_by(1); break;
    case kPadLeft:
    case kPadRight:
        if (!targets_.empty()) {
            auto it = std::find(targets_.begin(), targets_.end(), target_);
            int i = it == targets_.end() ? 0 : int(it - targets_.begin());
            int n = int(targets_.size());
            i = (i + (b == kPadRight ? 1 : n - 1)) % n;
            target_ = targets_[size_t(i)];
            target_pick_frame_ = frames_;
        }
        break;
    default: break;
    }
}

// Puts the virtual pointer on the auto-target (a point on its drawn shape), so a press of A
// acts on it like a mouse click there.
void GameMode::pointer_to_target() {
    World& w = *world_;
    const Item* it = w.item(target_);
    if (!it) return;
    int sx, sy;
    w.screen_point(*w.root_container(*it), sx, sy);
    // Search up from the feet for a pixel that picks the target.
    for (int dy = 0; dy <= 40; dy += 2)
        for (int dx : {0, -2, 2, -4, 4}) {
            float fx = sx + dx + 0.5f, fy = sy - dy + 0.5f;
            if (fx < 0 || fy < 0 || fx >= 320 || fy >= 200) continue;
            if (pick_f(fx, fy) == target_) {
                InputEvent e;
                e.type = InputEvent::kMouseMove;
                e.x = int(fx); e.y = int(fy); e.fx = fx; e.fy = fy;
                handle(e);
                return;
            }
        }
    InputEvent e;
    e.type = InputEvent::kMouseMove;
    e.x = std::clamp(sx, 0, 319); e.y = std::clamp(sy - 8, 0, 199);
    handle(e);
}

// Per tick: the sticks and the held buttons.
void GameMode::pad_tick() {
    World& w = *world_;
    PlayerInput& in = w.input;
    if (pads_ == 0) { in.pad_move = false; in.pad_block = false; return; }
    if (in.pad_jump && in.now_ms - in.jump_buffer_ms > PlayerInput::kBufferMs) in.pad_jump = false;
    // The party highlight fades after 5 s.
    if (Party* party = w.party.get(); party && party->pad_highlight >= 0 && frames_ - party_pick_frame_ > 150)
        party->pad_highlight = -1;
    // Holding Start 1 s: the debug menu (when the debug mode provides one).
    if (start_down_frame_ >= 0 && !start_long_ && pad_btn_[kPadStart] && frames_ - start_down_frame_ >= kBackHoldFrames &&
        debug().enabled()) {
        start_long_ = true;
        open_debug_menu();
    }
    // Holding Back: the help page.
    if (back_down_frame_ >= 0 && !back_help_ && pad_btn_[kPadBack] && frames_ - back_down_frame_ >= kBackHoldFrames) {
        back_help_ = true;
        if (atlas_on_) close_atlas();
        open_help();
    }
    if (back_down_frame_ >= 0 && pad_btn_[kPadBack] && atlas_on_) {
        // Real time passes in the atlas without game frames: count ticks there too.
    }
    // The right stick: the virtual pointer (speed grows with the deflection).
    int rx = pad_axis_[kAxisRX], ry = pad_axis_[kAxisRY];
    double rm = std::sqrt(double(rx) * rx + double(ry) * ry);
    if (rm > kStickDead) {
        double s = (rm - kStickDead) / (32767.0 - kStickDead);
        double speed = 2.0 + 10.0 * s * s;  // game pixels per tick
        float fx = mouse_fx_ >= 0 ? mouse_fx_ : mouse_x_ + 0.5f, fy = mouse_fy_ >= 0 ? mouse_fy_ : mouse_y_ + 0.5f;
        fx = std::clamp(float(fx + speed * rx / rm), 0.0f, 319.5f + float(panel_w_));  // onto the HUD side panel too
        fy = std::clamp(float(fy + speed * ry / rm), 0.0f, 199.5f);
        InputEvent e;
        e.type = InputEvent::kMouseMove;
        e.x = int(fx);
        e.y = int(fy);
        e.fx = fx;
        e.fy = fy;
        handle(e);
        pointer_frames_ = 0;
        pointer_on_target_ = false;  // the stick aims the pointer now
    } else if (pointer_frames_ < 100000) {
        ++pointer_frames_;
    }
    // The left stick: movement (8 directions; walk, run when pushed further), or the answer
    // menu, or panning the atlas.
    int lx = pad_axis_[kAxisLX], ly = pad_axis_[kAxisLY];
    double lm = std::sqrt(double(lx) * lx + double(ly) * ly);
    int ldir = lm > kStickDead ? stick_dir(lx, -ly) : -1;
    if (atlas_on_ && atlas_) {
        // World direction -> screen arrow keys held in the atlas.
        bool want[4] = {false, false, false, false};  // up down left right
        if (ldir >= 0) {
            int sdir = (ldir + 1) & 7;  // screen direction 0 = up, clockwise
            want[0] = sdir == 7 || sdir == 0 || sdir == 1;
            want[3] = sdir >= 1 && sdir <= 3;
            want[1] = sdir >= 3 && sdir <= 5;
            want[2] = sdir >= 5 && sdir <= 7;
        }
        static const int keys[4] = {kKeyUp, kKeyDown, kKeyLeft, kKeyRight};
        for (int i = 0; i < 4; ++i)
            if (want[i] != atlas_keys_[i]) {
                atlas_keys_[i] = want[i];
                InputEvent e;
                e.type = want[i] ? InputEvent::kKeyDown : InputEvent::kKeyUp;
                e.key = keys[i];
                atlas_->handle(e);
            }
        in.pad_move = false;
        return;
    }
    bool blocked = w.gumps.top_modal() || help_page_ >= 0;
    if (w.ask_menu && !w.ask_menu->answers.empty() && !blocked) {
        // The stick moves the highlight like the D-pad (its stronger axis), and a held stick
        // or D-pad repeats after 300 ms, then every 150 ms.
        ask_sync();
        int sd = -1;
        int ax = std::abs(lx), ay = std::abs(ly);
        bool settle = menu_stick_dir_ == -2;
        int held = menu_stick_dir_ < 0 ? 0 : (menu_stick_dir_ <= kAskDown ? ay : ax);
        if (menu_stick_dir_ >= 0 && held > kMenuStick * 3 / 4) sd = menu_stick_dir_;  // hysteresis
        else if (std::max(ax, ay) > kMenuStick) sd = ay >= ax ? (ly > 0 ? kAskDown : kAskUp) : (lx > 0 ? kAskRight : kAskLeft);
        uint32_t now = now_ms();
        if (settle) {
            if (sd < 0) menu_stick_dir_ = -1;
        } else if (sd != menu_stick_dir_) {
            menu_stick_dir_ = sd;
            if (sd >= 0) { ask_nav(sd); menu_stick_next_ = now + kMenuRepeatFirstMs; }
        } else if (sd >= 0 && now >= menu_stick_next_) {
            ask_nav(sd);
            menu_stick_next_ = now + kMenuRepeatMs;
        }
        static const int kDpad[4] = {kPadUp, kPadDown, kPadLeft, kPadRight};
        if (menu_dpad_dir_ >= 0 && !pad_btn_[kDpad[menu_dpad_dir_]]) menu_dpad_dir_ = -1;
        if (menu_dpad_dir_ >= 0 && now >= menu_dpad_next_) {
            ask_nav(menu_dpad_dir_);
            menu_dpad_next_ = now + kMenuRepeatMs;
        }
        in.pad_move = false;
    } else {
        if (!w.ask_menu) ask_sel_ = -1;
        menu_stick_dir_ = menu_dpad_dir_ = -1;
        in.pad_move = !blocked && ldir >= 0;
        if (in.pad_move) {
            in.pad_dir = ldir;
            in.pad_len = lm > kStickRun ? 2 : 1;
        }
    }
    const Actor* av = w.avatar();
    in.pad_block = !blocked && av && av->is(Actor::kInCombat) && pad_axis_[kAxisLT] > kTriggerOn;
    // A held in a scene keeps fast-forwarding.
    if (pad_btn_[kPadA] && scene_running() && !w.barks.empty() && !pointer_active()) text_hold_ = true;
    in.pad_target = target_;
    update_targets();
}

}  // namespace pg
