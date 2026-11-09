// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The turn-based combat screen: see turn_ui.h.
#include "app/turn_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/world.h"
#include "game/item_powers.h"
#include "gfx/surface.h"
#include "platform/input.h"

namespace pg {

namespace {

const char* const kEntryNames[TurnUI::kEntries] = {"Move", "Attack", "Cast", "Use item", "Defend", "Wait",
                                                   "Auto", "Keep on Auto", "End turn"};
const char* const kEntryHelp[TurnUI::kEntries] = {
    "Walk up to 300 units: pick a spot, then confirm. One move, then one action.",
    "Strike a foe in reach (walks in first if the move can get there).",
    "Cast a spell, then pick its target.",
    "Drink a potion or use an item. Ends the turn.",
    "Block until your next turn: half damage.",
    "Act later, at the end of this round (once a round).",
    "This character acts by himself this turn.",
    "This character acts by himself every turn, until you take him back.",
    "End this character's turn now.",
};

void plot(Surface8& s, int x, int y, uint8_t c) {
    if (x >= 0 && y >= 0 && x < s.width() && y < s.height()) s.fill_rect(x, y, 1, 1, c);
}

void line(Surface8& s, int x0, int y0, int x1, int y1, uint8_t c, bool thick = false, int dash = 0, int phase = 0) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int i = 0; i < 3000; ++i) {
        if (!dash || ((i + phase) % dash) < dash * 2 / 3) {
            plot(s, x0, y0, c);
            if (thick) plot(s, x0, y0 + 1, c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void frame_rect(Surface8& s, int x, int y, int w, int h, uint8_t c) {
    s.fill_rect(x, y, w, 1, c);
    s.fill_rect(x, y + h - 1, w, 1, c);
    s.fill_rect(x, y, 1, h, c);
    s.fill_rect(x + w - 1, y, 1, h, c);
}

}  // namespace

TurnUI::TurnUI(GameMode& gm) : gm_(gm) {}

const char* TurnUI::entry_name(int e) { return e >= 0 && e < kEntries ? kEntryNames[e] : ""; }
const char* TurnUI::entry_help(int e) { return e >= 0 && e < kEntries ? kEntryHelp[e] : ""; }

bool TurnUI::awaiting() const {
    World& w = gm_.world();
    return w.turn && tc().active() && tc().awaiting_player();
}

int TurnUI::active_entry() const {
    if (!awaiting()) return -1;
    if (mode_ == kPickMove) return kEMove;
    if (mode_ == kPickTarget && purpose_ == kForAttack) return kEAttack;
    if (mode_ == kList || (mode_ == kPickTarget && purpose_ == kForCast)) return list_ == kListCast ? kECast : kEUse;
    return -1;
}

bool TurnUI::entry_on(int e) { return awaiting() && entry_enabled(e); }

void TurnUI::press_entry(int e) {
    if (!awaiting() || e < 0 || e >= menu_size()) return;
    if (tc().current() != last_current_) { mode_ = kMenu; sel_ = 0; reset_pick(); last_current_ = tc().current(); }
    if (mode_ != kMenu) { mode_ = kMenu; reset_pick(); }
    sel_ = e;
    choose(e);
}

bool TurnUI::auto_button_point(int& x, int& y) const {
    if (!auto_mode_btn_.w) return false;
    x = auto_mode_btn_.x + auto_mode_btn_.w / 2;
    y = auto_mode_btn_.y + auto_mode_btn_.h / 2;
    return true;
}

int TurnUI::grid_step(int cur, int n, int cols, int dx, int dy) {
    if (n <= 0) return 0;
    cols = std::clamp(cols, 1, n);
    const int rows = (n + cols - 1) / cols;
    int row = std::clamp(cur, 0, n - 1) / cols, col = std::clamp(cur, 0, n - 1) % cols;
    if (dy) {
        row = (row + (dy > 0 ? 1 : -1) + rows) % rows;
        col = std::min(col, std::min(cols, n - row * cols) - 1);
    }
    if (dx) {
        const int len = std::min(cols, n - row * cols);
        col = (col + (dx > 0 ? 1 : -1) + len) % len;
    }
    return row * cols + col;
}

int TurnUI::menu_size() const { return gm_.panel_on() ? kEPartyAuto + 1 : int(kEntries); }
int TurnUI::menu_columns() const { return gm_.panel_on() ? gm_.hud_panel().columns() : 1; }

void TurnUI::party_auto_off() {
    World& w = gm_.world();
    if (w.turn && w.turn->party_auto) {
        w.turn->toggle_party_auto();
        mode_ = kMenu;
        reset_pick();
    }
}

TurnCombat& TurnUI::tc() const { return *gm_.world().turn; }

uint8_t TurnUI::col(int r, int g, int b) {
    const Palette& p = gm_.world().data().palette;
    int best = 1, bd = 1 << 30;
    for (int i = 1; i < 256; ++i) {
        int dr = p.rgb8[size_t(i)][0] - r, dg = p.rgb8[size_t(i)][1] - g, db = p.rgb8[size_t(i)][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}

void TurnUI::to_screen(int x, int y, int z, int& sx, int& sy) const {
    World& w = gm_.world();
    if (w.screen_mapper) { w.screen_mapper(x, y, z, sx, sy); return; }
    int cam[3];
    w.camera_point(cam);
    int camSx = (cam[0] - cam[1]) / 4, camSy = (cam[0] + cam[1]) / 8 - cam[2];
    sx = 160 + (x - y) / 4 - camSx;
    sy = 100 + (x + y) / 8 - z - camSy;
}

bool TurnUI::node_screen(int node, int& x, int& y) const {
    TurnCombat& t = tc();
    if (node < 0 || size_t(node) >= t.reach().size()) return false;
    int wx, wy, wz;
    t.node_point(node, wx, wy, wz);
    to_screen(wx, wy, wz, x, y);
    return true;
}

bool TurnUI::menu_point(int entry, int& x, int& y) const {
    if (entry < 0 || size_t(entry) >= entry_rects_.size() || entry_rects_[size_t(entry)].w == 0) return false;
    const Rect& r = entry_rects_[size_t(entry)];
    x = r.x + r.w / 2;
    y = r.y + r.h / 2;
    return true;
}

bool TurnUI::list_point(int i, int& x, int& y) const {
    if (i < 0 || size_t(i) >= list_rects_.size()) return false;
    x = list_rects_[size_t(i)].x + list_rects_[size_t(i)].w / 2;
    y = list_rects_[size_t(i)].y + list_rects_[size_t(i)].h / 2;
    return true;
}

bool TurnUI::strip_auto_point(int& x, int& y) const {
    if (!auto_btn_.w) return false;
    x = auto_btn_.x + auto_btn_.w / 2;
    y = auto_btn_.y + auto_btn_.h / 2;
    return true;
}

void TurnUI::reset_pick() {
    hover_node_ = picked_node_ = -1;
    hover_target_ = picked_target_ = 0;
    target_sel_ = 0;
}

std::vector<uint16_t> TurnUI::targets() const {
    TurnCombat& t = tc();
    uint16_t me = t.current();
    if (purpose_ == kForCast && cast_.friendly) {
        std::vector<uint16_t> out;
        for (const TurnCombat::Fighter& f : t.order())
            if (f.party) out.push_back(f.id);
        return out;
    }
    return t.enemies_of(me);
}

// ---------------------------------------------------------------------------------------------
// Menu logic

bool TurnUI::entry_enabled(int e) {
    TurnCombat& t = tc();
    uint16_t me = t.current();
    switch (e) {
    case kEMove: return !t.moved() && t.reach().size() > 1;
    case kEAttack: return !t.enemies_of(me).empty();
    case kECast: return !t.cast_options(me).empty();
    case kEUse: return !t.use_items(me).empty();
    default: return true;  // what cannot be chosen is still stepped over by the arrows: a press says why
    }
}

void TurnUI::back() {
    // One step back: a picked spot / target is let go first, then the pick, then the list.
    if (mode_ == kPickMove && picked_node_ >= 0) { picked_node_ = -1; return; }
    if (mode_ == kPickTarget && picked_target_) { picked_target_ = 0; return; }
    if (mode_ == kPickTarget && purpose_ == kForCast && !gm_.world().turn->cast_options(tc().current()).empty()) {
        mode_ = kList;
        list_ = kListCast;
        sel_ = 0;
    } else {
        mode_ = kMenu;
    }
    reset_pick();
}

void TurnUI::choose(int e) {
    TurnCombat& t = tc();
    if (!t.awaiting_player()) return;
    if (!entry_enabled(e)) {
        t.status = e == kEMove ? "Already moved" : e == kECast ? "No spells" : e == kEUse ? "Nothing to use" : "Not now";
        return;
    }
    reset_pick();
    switch (e) {
    case kEMove: {
        mode_ = kPickMove;
        // The destination cursor starts on the character (keys / pad push it out).
        int sx = 0, sy = 0;
        node_screen(0, sx, sy);
        cur_x_ = sx;
        cur_y_ = sy;
        hover_node_ = -1;
        break;
    }
    case kEAttack:
        mode_ = kPickTarget;
        purpose_ = kForAttack;
        break;
    case kECast:
        mode_ = kList;
        list_ = kListCast;
        sel_ = 0;
        break;
    case kEUse:
        mode_ = kList;
        list_ = kListUse;
        sel_ = 0;
        break;
    case kEDefend: t.cmd_defend(); break;
    case kEWait: t.cmd_wait(); break;
    case kEAuto: t.cmd_auto(false); break;
    case kEKeepAuto: t.cmd_auto(true); break;
    case kEEnd: t.cmd_end_turn(); break;
    case kEPartyAuto: t.toggle_party_auto(); break;
    default: break;
    }
    if (mode_ == kPickTarget) {
        std::vector<uint16_t> ts = targets();
        hover_target_ = ts.empty() ? 0 : ts.front();
    }
}

void TurnUI::confirm() {
    TurnCombat& t = tc();
    switch (mode_) {
    case kMenu: choose(sel_); return;
    case kPickMove: {
        int n = picked_node_ >= 0 ? picked_node_ : hover_node_;
        if (n <= 0) { t.status = "Pick a spot first"; return; }
        if (t.cmd_move(n)) { mode_ = kMenu; sel_ = kEAttack; reset_pick(); }
        return;
    }
    case kPickTarget: {
        uint16_t id = picked_target_ ? picked_target_ : hover_target_;
        if (!id) return;
        // One press: it fires now, or (out of range, not enough mana) nothing is used up and the
        // pick stays open with the reason in the status line.
        bool ok = purpose_ == kForAttack ? t.cmd_attack(id) : t.cmd_cast(cast_, id);
        if (ok) { mode_ = kMenu; sel_ = 0; reset_pick(); }
        return;
    }
    case kList: {
        uint16_t me = t.current();
        if (list_ == kListCast) {
            std::vector<TurnCombat::CastOption> opts = t.cast_options(me);
            if (sel_ < 0 || size_t(sel_) >= opts.size()) return;
            cast_ = opts[size_t(sel_)];
            if (cast_.locked) { t.status = cast_.name + ": " + cast_.why; return; }  // greyed: says why
            if (cast_.self) {
                if (t.cmd_cast(cast_, 0)) { mode_ = kMenu; sel_ = 0; }
                return;
            }
            mode_ = kPickTarget;
            purpose_ = kForCast;
            reset_pick();
            std::vector<uint16_t> ts = targets();
            hover_target_ = ts.empty() ? 0 : ts.front();
        } else {
            std::vector<uint16_t> items = t.use_items(me);
            if (sel_ < 0 || size_t(sel_) >= items.size()) return;
            if (t.cmd_use(items[size_t(sel_)])) { mode_ = kMenu; sel_ = 0; }
        }
        return;
    }
    }
}

// Keys / D-pad / stick: menu icons by row and column, the move pick in screen directions, targets
// cycle. (Left and Right did nothing in the menu before: it was one column, and with the panel's
// rows Up and Down just stepped along them.) Disabled icons are not skipped: the highlight keeps
// its column, and pressing one says why it cannot be used.
void TurnUI::nav(int dx, int dy) {
    TurnCombat& t = tc();
    if (mode_ == kMenu) {
        if (sel_ >= menu_size()) sel_ = 0;
        sel_ = grid_step(sel_, menu_size(), menu_columns(), dx, dy);
        return;
    }
    if (mode_ == kList) {
        int n = list_ == kListCast ? int(t.cast_options(t.current()).size()) : int(t.use_items(t.current()).size());
        if (n > 0 && dy) sel_ = (sel_ + dy + n) % n;
        return;
    }
    if (mode_ == kPickTarget) {
        std::vector<uint16_t> ts = targets();
        if (ts.empty()) return;
        int cur = int(std::find(ts.begin(), ts.end(), hover_target_) - ts.begin());
        if (size_t(cur) >= ts.size()) cur = 0;
        cur = (cur + (dx + dy > 0 ? 1 : -1) + int(ts.size())) % int(ts.size());
        hover_target_ = ts[size_t(cur)];
        picked_target_ = 0;
        return;
    }
    if (mode_ == kPickMove) move_cursor(dx * 6, dy * 4);
}

// The Move destination cursor: pushed by keys / the pad in screen directions; it snaps to the
// reachable spot nearest to it, and when pushed past the range it stays on the edge (it is put
// back onto that spot).
void TurnUI::move_cursor(int dx, int dy) {
    TurnCombat& t = tc();
    if (t.reach().size() < 2) return;
    int nx = cur_x_ + dx, ny = cur_y_ + dy;
    int n = node_at(nx, ny);
    if (n <= 0) return;
    int sx, sy;
    node_screen(n, sx, sy);
    // Reachable spots are a few pixels apart, so a cursor this far from the nearest one is past
    // the range: it goes back onto that spot, the edge (pushing on keeps it there).
    if ((sx - nx) * (sx - nx) + 4 * (sy - ny) * (sy - ny) > 4 * 4) { nx = sx; ny = sy; }
    cur_x_ = nx;
    cur_y_ = ny;
    hover_node_ = n;
    picked_node_ = -1;
}

int TurnUI::node_at(int mx, int my) const {
    TurnCombat& t = tc();
    const auto& r = t.reach();
    int best = -1, bd = 1 << 30;
    for (size_t i = 1; i < r.size(); ++i) {
        int sx, sy;
        node_screen(int(i), sx, sy);
        int d = (sx - mx) * (sx - mx) + 4 * (sy - my) * (sy - my);
        if (d < bd) { bd = d; best = int(i); }
    }
    return best;
}

uint16_t TurnUI::target_at(int mx, int my) const {
    std::vector<uint16_t> ts = targets();
    uint16_t id = gm_.pick(mx, my);
    if (std::find(ts.begin(), ts.end(), id) != ts.end()) return id;
    // Near the feet of one (small monsters are hard to hit with the pointer).
    World& w = gm_.world();
    uint16_t best = 0;
    int bd = 18 * 18;
    for (uint16_t t : ts) {
        const Item* it = w.item(t);
        if (!it) continue;
        Box b = w.box_of(*it);
        int sx, sy;
        to_screen(b.x - b.xd / 2, b.y - b.yd / 2, b.z + b.zd / 2, sx, sy);
        int d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
        if (d < bd) { bd = d; best = t; }
    }
    return best;
}

void TurnUI::update_hover() {
    if (mode_ == kPickMove) {
        // The pointer out of range: the nearest reachable spot (the marker stays on the edge).
        int n = node_at(gm_.mouse_x(), gm_.mouse_y());
        if (n > 0 && n != hover_node_) { hover_node_ = n; node_screen(n, cur_x_, cur_y_); }
    } else if (mode_ == kPickTarget) {
        if (uint16_t id = target_at(gm_.mouse_x(), gm_.mouse_y())) hover_target_ = id;
    } else if (mode_ == kList) {
        for (size_t i = 0; i < list_rects_.size(); ++i)
            if (list_rects_[i].in(gm_.mouse_x(), gm_.mouse_y())) sel_ = int(i);
    } else if (mode_ == kMenu) {
        for (size_t i = 0; i < entry_rects_.size(); ++i)
            if (entry_rects_[i].in(gm_.mouse_x(), gm_.mouse_y())) sel_ = int(i);
    }
}

// ---------------------------------------------------------------------------------------------
// Input

bool TurnUI::handle(const InputEvent& ev) {
    World& w = gm_.world();
    if (ev.type == InputEvent::kMouseUp) {
        int b = ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2;
        if (consumed_down_[b]) { consumed_down_[b] = false; if (b == 0) mouse_held_ = false; return true; }
    }
    // Party Auto on: Esc or a click on the "Auto mode: ON" button turns it off for everyone.
    if (w.turn && w.turn->enabled && w.turn->party_auto && !w.gumps.top_modal() && !w.target_pid && !gm_.atlas_open() &&
        !gm_.help_open()) {
        if (ev.type == InputEvent::kKeyDown && ev.key == kKeyEscape && !ev.repeat) { party_auto_off(); return true; }
        if (ev.type == InputEvent::kMouseDown && ev.button == 1 && auto_mode_btn_.in(ev.x, ev.y)) {
            party_auto_off();
            consumed_down_[0] = true;
            return true;
        }
    }
    if (!w.turn || !tc().active()) {
        mode_ = kMenu;
        for (bool& b : consumed_down_) b = false;
        space_held_ = mouse_held_ = false;
        return false;
    }
    TurnCombat& t = tc();
    // Mouse releases of presses this screen used.
    if (ev.type == InputEvent::kMouseUp) {
        int b = ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2;
        if (b == 0) mouse_held_ = false;
        if (consumed_down_[b]) { consumed_down_[b] = false; return true; }
        return false;
    }
    if (ev.type == InputEvent::kKeyUp) {
        if (ev.key == kKeySpace) space_held_ = false;
        return false;
    }
    // A script's target cursor (a focus, the Skull of Quakes): the arrows pick among the fighters
    // and Enter fires at once; Esc and the mouse work as everywhere (GameMode).
    if (target_cursor_up() && ev.type == InputEvent::kKeyDown) {
        switch (ev.key) {
        case kKeyUp: case kKeyLeft: cursor_cycle(-1); return true;
        case kKeyDown: case kKeyRight: cursor_cycle(1); return true;
        case kKeyReturn: if (!ev.repeat) cursor_confirm(); return true;
        default: break;
        }
    }
    if (w.gumps.top_modal() || w.target_pid || gm_.atlas_open() || gm_.help_open()) return false;
    // Any time in a fight: Party Auto, the speed-up, and the Party Auto button on the strip.
    if (ev.type == InputEvent::kKeyDown && !(ev.mods & (kModCtrl | kModAlt))) {
        if (ev.key == 'a' && !ev.repeat) { t.toggle_party_auto(); if (t.party_auto) mode_ = kMenu; return true; }
        if (ev.key == 'c') { t.status = "Turn-based fight: combat mode stays on"; return true; }
        if (ev.key == kKeySpace) { space_held_ = true; if (!t.awaiting_player()) return true; }
    }
    if (ev.type == InputEvent::kMouseDown && ev.button == 1 && auto_btn_.in(ev.x, ev.y)) {
        t.toggle_party_auto();
        consumed_down_[0] = true;
        return true;
    }
    if (ev.type == InputEvent::kMouseDown && ev.button == 1 && !t.awaiting_player() && !w.gumps.at(ev.x, ev.y)) {
        // Holding the left button through other turns speeds them up (and does nothing else).
        mouse_held_ = true;
        consumed_down_[0] = true;
        return true;
    }
    if (!t.awaiting_player()) return false;
    // The current character's turn.
    if (t.current() != last_current_) { mode_ = kMenu; sel_ = 0; reset_pick(); last_current_ = t.current(); }
    if (ev.type == InputEvent::kMouseMove) {
        update_hover();
        return false;  // the pointer itself still moves
    }
    if (ev.type == InputEvent::kKeyDown) {
        switch (ev.key) {
        case kKeyUp: nav(0, -1); return true;
        case kKeyDown: nav(0, 1); return true;
        case kKeyLeft: nav(-1, 0); return true;
        case kKeyRight: nav(1, 0); return true;
        case kKeyReturn: if (!ev.repeat) confirm(); return true;
        case kKeySpace: if (!ev.repeat && mode_ != kMenu) confirm(); return true;
        case kKeyEscape:
            if (mode_ == kMenu) return false;  // the main menu (save, load...)
            back();
            return true;
        default: break;
        }
        if (ev.key >= '1' && ev.key <= '9' && !ev.repeat) {
            int n = ev.key - '1';
            if (mode_ == kMenu && n < kEntries) { sel_ = n; choose(n); }
            else if (mode_ == kList) { sel_ = n; confirm(); }
            return true;
        }
        return false;
    }
    if (ev.type == InputEvent::kMouseDown) {
        int b = ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2;
        int mx = ev.x, my = ev.y;
        if (b == 2) {  // right click: back
            if (mode_ == kMenu) return false;
            back();
            consumed_down_[b] = true;
            return true;
        }
        if (b != 0) return false;
        // Menu entries and list lines.
        for (size_t i = 0; i < entry_rects_.size(); ++i)
            if (entry_rects_[i].in(mx, my)) {
                consumed_down_[0] = true;
                if (mode_ != kMenu) { mode_ = kMenu; reset_pick(); }
                sel_ = int(i);
                choose(int(i));
                return true;
            }
        if (mode_ == kList) {
            for (size_t i = 0; i < list_rects_.size(); ++i)
                if (list_rects_[i].in(mx, my)) {
                    consumed_down_[0] = true;
                    sel_ = int(i);
                    confirm();
                    return true;
                }
            if (list_box_.in(mx, my)) { consumed_down_[0] = true; return true; }
        }
        if (mode_ == kPickMove) {
            consumed_down_[0] = true;
            int n = node_at(mx, my);
            if (n <= 0) return true;
            if (n == picked_node_) confirm();  // click again to confirm
            else { picked_node_ = n; hover_node_ = n; }
            return true;
        }
        if (mode_ == kPickTarget) {
            consumed_down_[0] = true;
            uint16_t id = target_at(mx, my);
            if (!id) return true;
            if (id == picked_target_) confirm();
            else { picked_target_ = id; hover_target_ = id; }
            return true;
        }
        if (menu_.in(mx, my)) { consumed_down_[0] = true; return true; }
        return false;  // the world: look / drag as usual
    }
    return false;
}

bool TurnUI::pad(const InputEvent& ev) {
    World& w = gm_.world();
    // Party Auto on: B turns it off for everyone.
    if (ev.type == InputEvent::kPadDown && ev.button == kPadB && w.turn && w.turn->enabled && w.turn->party_auto &&
        !w.gumps.top_modal() && !w.target_pid && !gm_.atlas_open() && !gm_.help_open()) {
        party_auto_off();
        return true;
    }
    if (!w.turn || !tc().active()) { pad_a_held_ = false; stick_x_ = stick_y_ = 0; return false; }
    TurnCombat& t = tc();
    // A script's target cursor: as the keys. With the right-stick pointer in use, A clicks under
    // it instead (the code below), and B cancels there too.
    if (target_cursor_up() && ev.type == InputEvent::kPadDown) {
        switch (ev.button) {
        case kPadA:
            if (gm_.pointer_active()) break;
            cursor_confirm();
            return true;
        case kPadUp: case kPadLeft: cursor_cycle(-1); return true;
        case kPadDown: case kPadRight: cursor_cycle(1); return true;
        default: break;
        }
    }
    if (w.gumps.top_modal() || w.target_pid || gm_.atlas_open() || gm_.help_open()) return false;
    if (ev.type == InputEvent::kPadAxis) {
        if (ev.axis == kAxisLX) stick_x_ = ev.value;
        if (ev.axis == kAxisLY) stick_y_ = ev.value;
        if (ev.axis == kAxisLX || ev.axis == kAxisLY) {
            int dx = stick_x_ > 16000 ? 1 : stick_x_ < -16000 ? -1 : 0;
            int dy = stick_y_ > 16000 ? 1 : stick_y_ < -16000 ? -1 : 0;
            if ((dx || dy) && stick_repeat_ <= 0 && t.awaiting_player()) { nav(dx, dy); stick_repeat_ = 8; }
            if (!dx && !dy) stick_repeat_ = 0;
            return t.awaiting_player();
        }
        return false;
    }
    if (ev.type != InputEvent::kPadDown && ev.type != InputEvent::kPadUp) return false;
    bool down = ev.type == InputEvent::kPadDown;
    if (ev.button == kPadA && !down) { pad_a_held_ = false; return true; }
    if (ev.button == kPadRB) {
        if (down) t.toggle_party_auto();
        return true;
    }
    if (!t.awaiting_player()) {
        if (ev.button == kPadA) { pad_a_held_ = down; return true; }  // held: faster turns
        return ev.button == kPadX || ev.button == kPadY || ev.button == kPadB || (ev.button >= kPadUp && ev.button <= kPadRight);
    }
    if (t.current() != last_current_) { mode_ = kMenu; sel_ = 0; reset_pick(); last_current_ = t.current(); }
    if (!down) return ev.button != kPadStart && ev.button != kPadBack && ev.button != kPadLB;
    switch (ev.button) {
    case kPadA: confirm(); return true;
    case kPadB: if (mode_ != kMenu) back(); return true;
    case kPadY: mode_ = kMenu; sel_ = 0; reset_pick(); return true;
    case kPadX: return true;
    case kPadUp: nav(0, -1); return true;
    case kPadDown: nav(0, 1); return true;
    case kPadLeft: nav(-1, 0); return true;
    case kPadRight: nav(1, 0); return true;
    default: return false;  // Start (main menu), Back (atlas), LB (paperdoll) work as usual
    }
}

bool TurnUI::target_cursor_up() const {
    World& w = gm_.world();
    return w.turn && tc().active() && w.target_pid && !w.gumps.top_modal() && !gm_.atlas_open() && !gm_.help_open();
}

// The fighters a script's target cursor offers to the keys and the pad: the foes (nearest first),
// then the party, for the spells that are meant for a friend.
std::vector<uint16_t> TurnUI::cursor_targets() const {
    TurnCombat& t = tc();
    std::vector<uint16_t> out = t.enemies_of(t.current());
    for (const TurnCombat::Fighter& f : t.order())
        if (f.party && gm_.world().actor(f.id)) out.push_back(f.id);
    return out;
}

void TurnUI::cursor_cycle(int d) {
    std::vector<uint16_t> ts = cursor_targets();
    if (ts.empty()) return;
    int cur = int(std::find(ts.begin(), ts.end(), cursor_sel_) - ts.begin());
    if (size_t(cur) >= ts.size()) { cursor_sel_ = ts.front(); return; }
    cursor_sel_ = ts[size_t((cur + d + int(ts.size())) % int(ts.size()))];
}

bool TurnUI::cursor_confirm() {
    World& w = gm_.world();
    std::vector<uint16_t> ts = cursor_targets();
    if (std::find(ts.begin(), ts.end(), cursor_sel_) == ts.end()) cursor_sel_ = ts.empty() ? 0 : ts.front();
    Process* p = w.kernel().get(w.target_pid);
    if (!cursor_sel_ || !p) return false;
    p->result = cursor_sel_;
    p->terminate();
    return true;
}

void TurnUI::tick() {
    ++frame_;
    World& w = gm_.world();
    if (!w.turn) return;
    TurnCombat& t = tc();
    if (!t.active()) { mode_ = kMenu; last_current_ = 0; cursor_sel_ = 0; t.speed_held = false; return; }
    if (target_cursor_up()) {
        // The cursor opens on the nearest foe; a pick that has fallen is replaced.
        std::vector<uint16_t> ts = cursor_targets();
        if (std::find(ts.begin(), ts.end(), cursor_sel_) == ts.end()) cursor_sel_ = ts.empty() ? 0 : ts.front();
    } else {
        cursor_sel_ = 0;
    }
    t.speed_held = (space_held_ || pad_a_held_ || mouse_held_) && !t.awaiting_player();
    if (stick_repeat_ > 0 && --stick_repeat_ == 0 && t.awaiting_player()) {
        int dx = stick_x_ > 16000 ? 1 : stick_x_ < -16000 ? -1 : 0;
        int dy = stick_y_ > 16000 ? 1 : stick_y_ < -16000 ? -1 : 0;
        if (dx || dy) { nav(dx, dy); stick_repeat_ = 6; }
    }
    if (t.awaiting_player() && t.current() != last_current_) {
        mode_ = kMenu;
        sel_ = 0;
        reset_pick();
        last_current_ = t.current();
    }
    if (!t.awaiting_player() && mode_ != kMenu) { mode_ = kMenu; reset_pick(); }
    // A pick whose node or target went away.
    if (mode_ == kPickMove && t.moved()) { mode_ = kMenu; reset_pick(); }
}

// The move pick starts at the reachable spot nearest the nearest foe.
void TurnUI::default_node() {
    TurnCombat& t = tc();
    const auto& r = t.reach();
    if (r.size() < 2) return;
    std::vector<uint16_t> foes = t.enemies_of(t.current());
    const Item* f = foes.empty() ? nullptr : gm_.world().item(foes.front());
    int best = 1;
    long long bd = 1LL << 60;
    for (size_t i = 1; i < r.size(); ++i) {
        long long dx = f ? r[i].st.x - f->x : 0, dy = f ? r[i].st.y - f->y : 0;
        long long d = f ? dx * dx + dy * dy : -r[i].cost;
        if (d < bd) { bd = d; best = int(i); }
    }
    hover_node_ = best;
}

int TurnUI::extra_world_frames() const {
    World& w = gm_.world();
    if (!w.turn || !tc().active()) return 0;
    return std::max(0, tc().world_frames() - 1);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void TurnUI::ring(Surface8& s, uint16_t id, uint8_t c, int pad, bool dashed) {
    World& w = gm_.world();
    const Item* it = w.item(id);
    if (!it || it->chunk < 0) return;
    Box b = w.box_of(*it);
    double cx = b.x - b.xd / 2.0, cy = b.y - b.yd / 2.0;
    double r = std::max(b.xd, b.yd) / 2.0 + pad;
    int px = 0, py = 0;
    for (int k = 0; k <= 32; ++k) {
        double a = k * 2 * M_PI / 32;
        int sx, sy;
        to_screen(int(cx + r * std::cos(a)), int(cy + r * std::sin(a)), b.z, sx, sy);
        if (k && (!dashed || (k & 1))) line(s, px, py, sx, sy, c);
        px = sx;
        py = sy;
    }
}

// The Lightning spell's bolt: a jagged line from above the picture down onto the target, a new
// shape each frame. In UI pixels, so it is the same at every zoom.
void TurnUI::draw_sky(Surface8& s) {
    World& w = gm_.world();
    if (w.sky_bolts.empty()) return;
    const uint8_t core = col(255, 255, 255), glow = col(170, 200, 255);
    for (const SkyBolt& b : w.sky_bolts) {
        int px = b.x, py = b.y, pz = b.z + 28;
        if (const Item* it = w.item(b.target)) {
            Box tb = w.box_of(*it);
            px = tb.x - tb.xd / 2;
            py = tb.y - tb.yd / 2;
            pz = tb.z + tb.zd / 2;
        }
        int ex, ey;
        to_screen(px, py, pz, ex, ey);
        uint32_t r = b.seed * 2654435761u + uint32_t(b.frames) * 40503u + 1;
        auto rnd = [&r](int n) { r = r * 1103515245u + 12345u; return int((r >> 16) % uint32_t(n)); };
        int x = ex + rnd(61) - 30, y = -4;
        const int steps = std::max(5, (ey - y) / 13);
        for (int i = 1; i <= steps; ++i) {
            int ny = y + (ey - y) / (steps - i + 1);
            int nx = i == steps ? ex : x + (ex - x) / (steps - i + 1) + rnd(17) - 8;
            line(s, x - 1, y, nx - 1, ny, glow);
            line(s, x + 2, y, nx + 2, ny, glow);
            line(s, x, y, nx, ny, core);
            line(s, x + 1, y, nx + 1, ny, core);
            if (i > 1 && i < steps && rnd(3) == 0) {  // a short fork
                int fx = nx + (rnd(2) ? 1 : -1) * (6 + rnd(10)), fy = ny + 5 + rnd(9);
                line(s, nx, ny, fx, fy, glow);
            }
            x = nx;
            y = ny;
        }
    }
}

// The flash after the bolt, on a finished 8-bit picture: everything nearly white, the picture's
// shapes still just there (the scaled view does the same on its RGBA picture, GameMode::render).
void TurnUI::flash_surface(Surface8& s) {
    const Palette& p = gm_.world().data().palette;
    if (flash_lut_.empty()) {
        flash_lut_.resize(256);
        for (int i = 0; i < 256; ++i) {
            int r = (p.rgb8[size_t(i)][0] * 3 + 240 * 7) / 10, g = (p.rgb8[size_t(i)][1] * 3 + 244 * 7) / 10,
                b = (p.rgb8[size_t(i)][2] * 3 + 255 * 7) / 10;
            flash_lut_[size_t(i)] = col(r, g, b);
        }
    }
    for (int y = 0; y < s.height(); ++y) {
        uint8_t* row = s.row(y);
        for (int x = 0; x < s.width(); ++x) row[x] = flash_lut_[row[x]];
    }
}

bool TurnUI::range_circle(int& range) const {
    if (!awaiting()) return false;
    TurnCombat::CastOption o;
    if (mode_ == kPickTarget && purpose_ == kForCast) {
        o = cast_;
    } else if (mode_ == kList && list_ == kListCast) {
        std::vector<TurnCombat::CastOption> opts = tc().cast_options(tc().current());
        if (sel_ < 0 || size_t(sel_) >= opts.size()) return false;
        o = opts[size_t(sel_)];
    } else {
        return false;
    }
    if (o.book >= 0 || o.self || o.spell < 0) return false;
    range = o.range;
    return true;
}

// A very light, thin dotted circle on the ground round the caster. It is laid out in world units
// and projected, so on screen it is the game's flattened ellipse, at every zoom.
void TurnUI::draw_range_circle(Surface8& s, uint16_t cur, int range) {
    World& w = gm_.world();
    const Item* it = w.item(cur);
    if (!it || it->chunk < 0) return;
    Box b = w.box_of(*it);
    const double cx = b.x - b.xd / 2.0, cy = b.y - b.yd / 2.0;
    const uint8_t c = col(214, 220, 232);
    constexpr int kSegments = 128, kDotEvery = 4;  // one pixel in four: faint on any ground
    int px = 0, py = 0, phase = 0;
    for (int k = 0; k <= kSegments; ++k) {
        double a = k * 2 * M_PI / kSegments;
        int sx, sy;
        to_screen(int(cx + range * std::cos(a)), int(cy + range * std::sin(a)), b.z, sx, sy);
        const bool seen = std::max(std::abs(sx - 160), std::abs(px - 160)) < 2000 && std::max(std::abs(sy - 100), std::abs(py - 100)) < 2000;
        if (k && seen) {
            const int n = std::max(std::abs(sx - px), std::abs(sy - py));
            for (int i = 0; i < n; ++i, ++phase)
                if (phase % kDotEvery == 0) plot(s, px + (sx - px) * i / n, py + (sy - py) * i / n, c);
        }
        px = sx;
        py = sy;
    }
}

void TurnUI::draw_world(Surface8& s) {
    World& w = gm_.world();
    shown_ = false;
    draw_sky(s);  // both combat styles: Wynna casts in real time too
    if (!w.turn || !tc().active()) return;
    shown_ = true;
    ensure_colours();
    TurnCombat& t = tc();
    uint16_t cur = t.current();
    bool pulse = (frame_ / 6) % 2;
    draw_world_marks(s, t, cur, pulse);
}

void TurnUI::ensure_colours() {
    if (!have_colours_) {
        have_colours_ = true;
        colours_[0] = col(255, 230, 0);    // yellow: the party's marker, the route
        colours_[1] = col(230, 40, 30);    // red: foes
        colours_[2] = col(90, 200, 255);   // light blue: the reachable area
        colours_[3] = col(255, 255, 255);  // white
        colours_[4] = col(20, 20, 20);     // near black
        colours_[5] = col(222, 206, 160);  // parchment
        colours_[6] = col(150, 120, 80);   // darker parchment (selection band)
        colours_[7] = col(60, 220, 60);    // green: HP
        colours_[8] = col(120, 120, 120);  // grey (disabled)
        colours_[9] = col(40, 60, 140);    // blue ink (titles)
        colours_[10] = col(120, 30, 20);   // dark red: foe cells
        for (int k = 0; k < 256; ++k) grey_lut_[k] = colours_[8];  // a greyed line of text (locked spells)
    }
}

void TurnUI::draw_world_marks(Surface8& s, TurnCombat& t, uint16_t cur, bool pulse) {
    int reach_range = 0;
    if (range_circle(reach_range)) draw_range_circle(s, cur, reach_range);
    // The destination marker and the route to it (the reachable area itself is not shown).
    if (t.awaiting_player() && mode_ == kPickMove) {
        int n = picked_node_ >= 0 ? picked_node_ : hover_node_;
        if (n <= 0) {
            // Nothing picked yet: a small marker on the character, where the cursor starts.
            int sx, sy;
            node_screen(0, sx, sy);
            line(s, sx - 3, sy, sx + 3, sy, colours_[2]);
            line(s, sx, sy - 2, sx, sy + 2, colours_[2]);
        }
        if (n > 0) {
            std::vector<int> c;
            t.chain(n, c);
            int px = 0, py = 0;
            for (size_t i = 0; i < c.size(); ++i) {
                int sx, sy;
                node_screen(c[i], sx, sy);
                if (i) line(s, px, py, sx, sy, colours_[0], true);
                px = sx;
                py = sy;
            }
            // The destination: a flat diamond on the ground with a cross in it.
            uint8_t end = picked_node_ >= 0 ? (pulse ? colours_[3] : colours_[0]) : colours_[0];
            line(s, px - 7, py, px, py - 4, colours_[4]);
            line(s, px, py - 4, px + 7, py, colours_[4]);
            line(s, px + 7, py, px, py + 4, colours_[4]);
            line(s, px, py + 4, px - 7, py, colours_[4]);
            line(s, px - 6, py, px, py - 3, end);
            line(s, px, py - 3, px + 6, py, end);
            line(s, px + 6, py, px, py + 3, end);
            line(s, px, py + 3, px - 6, py, end);
            line(s, px - 3, py - 2, px + 3, py + 2, end, true);
            line(s, px - 3, py + 2, px + 3, py - 2, end, true);
        }
    }
    // Targets.
    if (t.awaiting_player() && mode_ == kPickTarget) {
        bool friendly = purpose_ == kForCast && cast_.friendly;
        for (uint16_t id : targets()) {
            bool sel = id == (picked_target_ ? picked_target_ : hover_target_);
            bool reach = purpose_ == kForCast ? t.cast_reaches(cur, cast_, id) : t.attack_reachable(id);
            uint8_t c = friendly ? colours_[7] : colours_[1];
            ring(s, id, sel && pulse ? colours_[3] : c, sel ? 10 : 6, !reach);  // dashed: out of reach this turn
        }
    }
    // A script's target cursor: the fighter the keys and the pad have picked.
    if (cursor_sel_ && gm_.world().target_pid) ring(s, cursor_sel_, pulse ? colours_[3] : colours_[1], 10, false);
    // The marker under the character whose turn it is.
    if (cur) {
        const TurnCombat::Fighter* f = nullptr;
        for (const auto& g : t.order())
            if (g.id == cur) f = &g;
        uint8_t c = f && f->party ? colours_[0] : colours_[1];
        ring(s, cur, c, 8 + (frame_ / 8) % 3, false);
    }
}

void TurnUI::draw_hud(Surface8& s) {
    World& w = gm_.world();
    entry_rects_.clear();
    list_rects_.clear();
    menu_ = list_box_ = auto_btn_ = auto_mode_btn_ = Rect{};
    out_of_range_shown_ = false;
    if (!w.turn) return;
    ensure_colours();
    const ShapeFont& f = w.font(0);
    const ShapeFont& mf = w.font(1);
    const bool panel = gm_.panel_on();  // the HUD side panel shows the strip and the menu
    // --- "Auto mode: ON" (Party Auto), bottom left of the picture: click / Esc / B turn it off.
    if (w.turn->enabled && w.turn->party_auto) {
        const std::string label = "Auto mode: ON";
        int bw = f.text_width(label) + 8, bh = f.height() + 4;
        auto_mode_btn_ = Rect{3, 197 - bh, bw, bh};
        bool hot = auto_mode_btn_.in(gm_.mouse_x(), gm_.mouse_y());
        s.fill_rect(auto_mode_btn_.x, auto_mode_btn_.y, bw, bh, hot ? colours_[6] : colours_[4]);
        frame_rect(s, auto_mode_btn_.x, auto_mode_btn_.y, bw, bh, (frame_ / 15) % 2 ? colours_[0] : colours_[5]);
        f.draw_top(s, auto_mode_btn_.x + 4, auto_mode_btn_.y + 2, label);
        if (hot) {
            const char* tip = "Click, Esc or B: Auto off";
            int tw = mf.text_width(tip) + 6;
            s.fill_rect(auto_mode_btn_.x, auto_mode_btn_.y - mf.height() - 5, tw, mf.height() + 4, colours_[5]);
            frame_rect(s, auto_mode_btn_.x, auto_mode_btn_.y - mf.height() - 5, tw, mf.height() + 4, colours_[4]);
            mf.draw_top(s, auto_mode_btn_.x + 3, auto_mode_btn_.y - mf.height() - 3, tip);
        }
    }
    if (!tc().active() || !have_colours_) return;
    TurnCombat& t = tc();
    // --- The turn-order strip (under the party bar when it is shown).
    int sy = 2;
    if (w.party && w.party->enabled && w.party->bar_wanted()) sy = 38;
    int x = 2;
    uint16_t cur = t.current();
    const auto& ord = t.order();
    auto label = [&](uint16_t id) {
        std::string n;
        const Actor* a = w.actor(id);
        if (!a) return n;
        if (w.party && w.party->is_companion(id)) n = w.party->display_name(id);
        else if (id == 1) n = "Avatar";
        else {
            n = w.data().usecode.class_name(a->shape);
            for (size_t i = 1; i < n.size(); ++i) n[i] = char(std::tolower(n[i]));
            if (n.empty()) n = "Foe";
        }
        if (n.size() > 8 && n.find(' ') != std::string::npos) n = n.substr(0, n.find(' '));
        if (n.size() > 8) n = n.substr(0, 8);
        return n;
    };
    const int ch = f.height() + 6;
    for (size_t i = 0; i < ord.size() && x < 300 && !panel; ++i) {
        const auto& g = ord[i];
        const Actor* a = w.actor(g.id);
        if (!a || a->is(Actor::kDead)) continue;
        std::string n = label(g.id);
        int tw = f.text_width(n) + 6;
        bool now = g.id == cur;
        bool done = int(i) < t.current_index();
        s.fill_rect(x, sy, tw, ch, now ? colours_[5] : g.party ? colours_[4] : colours_[10]);
        frame_rect(s, x, sy, tw, ch, now ? colours_[0] : done ? colours_[8] : g.party ? colours_[0] : colours_[1]);
        f.draw_top(s, x + 3, sy + 2, n);
        // HP bar.
        int mh = 1;
        if (w.party && w.party->is_companion(g.id)) mh = w.party->max_hp(*a);
        else if (g.id == 1) mh = std::max(1, 2 * int(a->str));
        else mh = std::max<int>(1, a->hp);
        static std::vector<std::pair<uint16_t, int>> seen;  // foes: the highest HP seen is their full bar
        if (!g.party) {
            auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& p) { return p.first == g.id; });
            if (it == seen.end()) { seen.push_back({g.id, a->hp}); if (seen.size() > 64) seen.erase(seen.begin()); }
            else { it->second = std::max<int>(it->second, a->hp); mh = it->second; }
        }
        int bw = std::clamp(int(a->hp) * (tw - 4) / std::max(1, mh), 0, tw - 4);
        s.fill_rect(x + 2, sy + ch - 3, tw - 4, 1, colours_[4]);
        s.fill_rect(x + 2, sy + ch - 3, bw, 1, g.party ? colours_[7] : colours_[1]);
        if (g.keep_auto || (g.party && t.party_auto && g.controllable)) plot(s, x + tw - 3, sy + 2, colours_[2]);
        if (g.defending) plot(s, x + tw - 3, sy + 4, colours_[3]);
        x += tw + 2;
    }
    // Round number and the Party Auto button.
    char buf[64];
    std::snprintf(buf, sizeof buf, "Round %d", t.round());
    int by = panel ? 2 : sy + ch + 2;
    if (!panel) {
        f.draw_top(s, 3, by, buf);
        std::string pa = std::string("A: Party Auto ") + (t.party_auto ? "ON" : "off");
        int pw = f.text_width(pa) + 6;
        auto_btn_ = Rect{4 + f.text_width(buf) + 6, by - 1, pw, f.height() + 3};
        s.fill_rect(auto_btn_.x, auto_btn_.y, auto_btn_.w, auto_btn_.h, t.party_auto ? colours_[0] : colours_[4]);
        frame_rect(s, auto_btn_.x, auto_btn_.y, auto_btn_.w, auto_btn_.h, colours_[0]);
        f.draw_top(s, auto_btn_.x + 3, auto_btn_.y + 2, pa);
    }
    // Status line.
    if (!t.status.empty() && t.status != t.notice) {
        int tw = mf.text_width(t.status);
        int sx = std::max(2, (320 - tw) / 2);
        if (auto_mode_btn_.w) sx = std::max(sx, auto_mode_btn_.x + auto_mode_btn_.w + 6);
        s.fill_rect(sx - 3, 186 - mf.height(), tw + 6, mf.height() + 4, colours_[5]);
        mf.draw_top(s, sx, 188 - mf.height(), t.status);
    }
    // A brief message that outlives the turn ("Ghoul missed its turn"), above the status line.
    if (!t.notice.empty()) {
        int tw = mf.text_width(t.notice);
        int sx = std::max(2, (320 - tw) / 2), ny = 180 - 2 * mf.height();
        s.fill_rect(sx - 3, ny - 2, tw + 6, mf.height() + 4, colours_[5]);
        frame_rect(s, sx - 3, ny - 2, tw + 6, mf.height() + 4, colours_[10]);
        mf.draw_top(s, sx, ny, t.notice);
    }
    // A script's target cursor: the pick's name, and what to press.
    if (w.target_pid && cursor_sel_) {
        if (const Item* it = w.item(cursor_sel_)) {
            Box b = w.box_of(*it);
            int px, py;
            to_screen(b.x - b.xd / 2, b.y - b.yd / 2, b.z + b.zd, px, py);
            const Actor* a = it->as_actor();
            std::string n = label(cursor_sel_) + (a ? " " + std::to_string(a->hp) + " HP" : "");
            int tw = f.text_width(n);
            int lx = std::clamp(px - tw / 2, 1, 318 - std::min(316, tw)), ly = std::clamp(py - 14, 1, 190);
            s.fill_rect(lx - 2, ly - 1, tw + 4, f.height() + 2, colours_[4]);
            f.draw_top(s, lx, ly, n);
        }
    }
    if (!t.awaiting_player()) {
        if (!t.party_auto) {
            const char* hint = "Hold Space / A: faster";
            f.draw_top(s, 318 - f.text_width(hint), by, hint);
        }
        return;
    }
    // --- The action menu (bottom left).
    std::string title = label(cur) + (t.moved() ? " (moved)" : "");
    const int lh = mf.line_advance() + 1;
    int mw = std::max(mf.text_width(title) + 8, 0);
    for (int e = 0; e < kEntries; ++e) mw = std::max(mw, mf.text_width(std::to_string(e + 1) + " " + kEntryNames[e]) + 10);
    int mh = lh * (kEntries + 1) + 6;
    menu_ = Rect{3, 180 - mh - mf.height(), mw, mh};
    if (menu_.y < by + 12) menu_.y = by + 12;
    // With the side panel its buttons are the menu: nothing floats over the picture; the spell /
    // item list and the hints go to the bottom left.
    if (panel) menu_ = Rect{0, 0, 0, 176};
    for (int e = 0; e < kEntries && !panel; ++e) {
        if (e == 0) {
            s.fill_rect(menu_.x, menu_.y, menu_.w, menu_.h, colours_[5]);
            frame_rect(s, menu_.x, menu_.y, menu_.w, menu_.h, colours_[4]);
            mf.draw_top(s, menu_.x + 4, menu_.y + 3, title);
        }
        Rect r{menu_.x + 2, menu_.y + 3 + lh * (e + 1), menu_.w - 4, lh};
        entry_rects_.push_back(r);
        bool on = entry_enabled(e);
        if (e == sel_ && mode_ == kMenu) s.fill_rect(r.x, r.y - 1, r.w, r.h, colours_[6]);
        if (mode_ != kMenu && ((mode_ == kPickMove && e == kEMove) || (mode_ == kPickTarget && purpose_ == kForAttack && e == kEAttack) ||
                               ((mode_ == kList || (mode_ == kPickTarget && purpose_ == kForCast)) &&
                                e == (list_ == kListCast ? kECast : kEUse))))
            s.fill_rect(r.x, r.y - 1, r.w, r.h, colours_[6]);
        std::string txt = std::to_string(e + 1) + " " + kEntryNames[e];
        if (e == kEKeepAuto && t.keep_auto(cur)) txt += " *";
        mf.draw_top(s, r.x + 2, r.y, txt);
        if (!on) s.fill_rect(r.x + 2, r.y + mf.height() / 2, mf.text_width(txt), 1, colours_[8]);
    }
    // --- Lists (spells, items).
    if (mode_ == kList) {
        std::vector<std::string> lines;
        uint16_t me = cur;
        if (list_ == kListCast)
            for (const auto& o : t.cast_options(me)) {
                std::string l = o.name + (o.mana ? " (" + std::to_string(o.mana) + ")" : "");
                // Leveling stage 5: the uses left, or why it cannot be cast (greyed below).
                if (o.locked) l += " - " + o.why;
                else if (o.uses >= 0) l += ", " + std::to_string(o.uses) + " left";
                lines.push_back(l);
            }
        else
            for (uint16_t k : t.use_items(me)) {
                const Item* it = w.item(k);
                std::string n = it ? w.data().usecode.class_name(it->shape) : "?";
                for (size_t i = 1; i < n.size(); ++i) n[i] = char(std::tolower(n[i]));
                if (it && !item_powers::display_name(w, *it).empty()) n = item_powers::display_name(w, *it);
                if (it && it->quality > 1 && w.data().typeflags.get(it->shape).family() == kFamQuantity)
                    n += " x" + std::to_string(it->quality);
                lines.push_back(n);
            }
        int lw = 60;
        for (const auto& l : lines) lw = std::max(lw, mf.text_width(l) + 10);
        list_box_ = Rect{menu_.x + menu_.w + 3, menu_.y, lw, lh * int(lines.size() + 1) + 6};
        if (panel) list_box_.y = std::max(2, 162 - list_box_.h);
        s.fill_rect(list_box_.x, list_box_.y, list_box_.w, list_box_.h, colours_[5]);
        frame_rect(s, list_box_.x, list_box_.y, list_box_.w, list_box_.h, colours_[4]);
        mf.draw_top(s, list_box_.x + 4, list_box_.y + 3, list_ == kListCast ? "Spells" : "Items");
        const Actor* a = w.actor(me);
        const std::vector<TurnCombat::CastOption> cast_opts = list_ == kListCast ? t.cast_options(me) : std::vector<TurnCombat::CastOption>();
        for (size_t i = 0; i < lines.size(); ++i) {
            Rect r{list_box_.x + 2, list_box_.y + 3 + lh * int(i + 1), list_box_.w - 4, lh};
            list_rects_.push_back(r);
            if (int(i) == sel_) s.fill_rect(r.x, r.y - 1, r.w, r.h, colours_[6]);
            const bool locked = list_ == kListCast && i < cast_opts.size() && cast_opts[i].locked;
            mf.draw_top(s, r.x + 2, r.y, lines[i], locked ? grey_lut_ : nullptr);
            if (list_ == kListCast && a && !locked) {
                if (i < cast_opts.size() && cast_opts[i].mana > a->mana) s.fill_rect(r.x + 2, r.y + mf.height() / 2, mf.text_width(lines[i]), 1, colours_[8]);
            }
        }
    }
    // --- Hints for picking.
    const char* hint = nullptr;
    if (mode_ == kPickMove) hint = picked_node_ >= 0 ? "Click again / Enter: go   Right click / Esc: back" : "Pick a spot (click, arrows)";
    if (mode_ == kPickTarget) hint = picked_target_ ? "Click again, Enter or A: fire" : "Pick a target: Enter or A fires";
    if (hint) {
        int tw = f.text_width(hint);
        s.fill_rect(menu_.x + menu_.w + 3, menu_.y + menu_.h - f.height() - 4, tw + 6, f.height() + 4, colours_[4]);
        f.draw_top(s, menu_.x + menu_.w + 6, menu_.y + menu_.h - f.height() - 2, hint);
    }
    // The name over a hovered target.
    if (mode_ == kPickTarget) {
        uint16_t id = picked_target_ ? picked_target_ : hover_target_;
        if (const Item* it = w.item(id)) {
            Box b = w.box_of(*it);
            int px, py;
            to_screen(b.x - b.xd / 2, b.y - b.yd / 2, b.z + b.zd, px, py);
            const Actor* a = it->as_actor();
            std::string n = label(id) + (a ? " " + std::to_string(a->hp) + " HP" : "");
            int tw = f.text_width(n);
            int lx = std::clamp(px - tw / 2, 1, 318 - tw), ly = std::clamp(py - 14, 1, 190);
            s.fill_rect(lx - 2, ly - 1, tw + 4, f.height() + 2, colours_[4]);
            f.draw_top(s, lx, ly, n);
            // Tiny words above the name: this one cannot be hit from here (a press uses nothing).
            bool can = purpose_ == kForCast ? t.cast_reaches(cur, cast_, id) : t.attack_reachable(id);
            out_of_range_shown_ = !can;
            if (!can) {
                const char* oor = "out of range";
                int ow = mf.text_width(oor);
                int ox = std::clamp(px - ow / 2, 2, 317 - ow), oy = std::max(1, ly - mf.height() - 2);
                s.fill_rect(ox - 1, oy, ow + 2, mf.height(), colours_[5]);
                mf.draw_top(s, ox, oy, oor);
            }
        }
    }
}

}  // namespace pg
