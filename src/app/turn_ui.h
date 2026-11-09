// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The turn-based combat screen (game/turn_combat.h; PLAN-TURNBASED.md): the turn-order strip,
// the marker under the character whose turn it is, the action menu, the highlighted reachable
// area with the route preview, target picking, and the mouse / keyboard / controller input
// for all of it. Owned by the game mode.
//
//   Mouse: click a menu entry; Move: point at a spot (the route follows), click to pick, click
//   again to confirm; Attack / Cast: click a target, again to confirm; right click = back.
//   Keys: arrows + Enter (or 1-9) in the menu (Left / Right along a row of icons, Up / Down to
//   the other row, same column), arrows move the pick, Enter confirms at once, Esc back (Esc in
//   the menu = the main menu as usual), A = Party Auto, Space held = faster turns.
//   Pad: D-pad / left stick as the arrows, A confirm (one press fires), B back, Y the action
//   menu, RB Party Auto, A held during other turns = faster.
//   A script's own target cursor during a fight (a focus, the Skull of Quakes): arrows / D-pad
//   go through the foes, then the party; Enter / A picks at once; Esc / B cancels; a click (or
//   the right-stick pointer and A) picks whatever is under it.
#include <cstdint>
#include <string>
#include <vector>

#include "game/turn_combat.h"

namespace pg {

class GameMode;
class Surface8;
struct InputEvent;

class TurnUI {
public:
    explicit TurnUI(GameMode& gm);
    bool handle(const InputEvent& ev);  // keys and mouse; true = used
    bool pad(const InputEvent& ev);     // controller (game_pad.cpp, after the wake-up press)
    void tick();                        // once per game frame, before the world runs
    int extra_world_frames() const;     // world frames beyond the first this game frame
    void draw_world(Surface8& s);       // marker, reachable area, route, target rings (under windows)
    void draw_hud(Surface8& s);         // strip, menu, hints (over windows)

    // --- For tests and screenshots
    enum Mode { kMenu, kPickMove, kPickTarget, kList };
    enum Entry { kEMove, kEAttack, kECast, kEUse, kEDefend, kEWait, kEAuto, kEKeepAuto, kEEnd, kEntries };
    static constexpr int kEPartyAuto = kEntries;  // the side panel's tenth icon (Classic: not in the menu)
    // A step through a grid of n icons, `cols` to a row: dx along the row (wraps in the row), dy to
    // another row (wraps, keeps the column, the last entry of a shorter row if it has to).
    static int grid_step(int cur, int n, int cols, int dx, int dy);
    Mode mode() const { return mode_; }
    bool menu_point(int entry, int& x, int& y) const;    // centre of a menu entry (last frame)
    bool list_point(int i, int& x, int& y) const;        // centre of list line i (cast / use)
    bool strip_auto_point(int& x, int& y) const;         // the strip's Party Auto button
    bool node_screen(int node, int& x, int& y) const;    // a reachable node on the screen
    int hover_node() const { return hover_node_; }
    int picked_node() const { return picked_node_; }
    uint16_t picked_target() const { return picked_target_; }
    bool shown() const { return shown_; }
    bool out_of_range_shown() const { return out_of_range_shown_; }  // the tiny words were drawn

    // --- The HUD side panel's action buttons (app/hud_panel.h) drive the same menu. With the
    // panel on, the floating menu and the turn strip are not drawn (the panel shows them).
    static const char* entry_name(int entry);
    static const char* entry_help(int entry);       // tooltip text
    bool awaiting() const;                          // a party member's turn waits for a command
    int selected_entry() const { return mode_ == kMenu ? sel_ : -1; }
    int active_entry() const;                       // the entry whose pick / list is open, -1
    bool entry_on(int entry);                       // can be chosen now
    void press_entry(int entry);                    // as a click on the floating menu's entry
    void set_selected(int entry) { if (mode_ == kMenu && entry >= 0 && entry < menu_size()) sel_ = entry; }
    int menu_size() const;                          // ten with the side panel, nine in Classic
    int menu_columns() const;                       // the panel's icons to a row; Classic: one column
    // The spell whose reach is shown (a very light circle round the caster): the one picked or
    // highlighted in the list. False if none, or it has no reach of its own (the Avatar's spell
    // items, summons). Tests.
    bool range_circle(int& range) const;
    uint16_t cursor_target() const { return cursor_sel_; }  // a script's target cursor: the pick
    // Remaps a finished 8-bit picture for the Lightning flash.
    void flash_surface(Surface8& s);
    // The "Auto mode: ON" button at the bottom left of the game picture (Party Auto on): a click,
    // Esc or the pad's B turn Party Auto off for the whole party.
    bool auto_button_point(int& x, int& y) const;
    void party_auto_off();
    // Move picking: the destination (keys / pad move it, clamped to the reachable range).
    bool dest_screen(int& x, int& y) const { x = cur_x_; y = cur_y_; return mode_ == kPickMove; }

private:
    TurnCombat& tc() const;
    void to_screen(int x, int y, int z, int& sx, int& sy) const;
    void back();
    void choose(int entry);
    bool entry_enabled(int entry);
    void confirm();
    void nav(int dx, int dy);  // keys / D-pad / stick
    void update_hover();
    void default_node();
    int node_at(int mx, int my) const;
    uint16_t target_at(int mx, int my) const;
    std::vector<uint16_t> targets() const;
    void reset_pick();
    void ring(Surface8& s, uint16_t id, uint8_t c, int pad, bool dashed);
    uint8_t col(int r, int g, int b);
    void ensure_colours();
    void draw_world_marks(Surface8& s, TurnCombat& t, uint16_t cur, bool pulse);
    void draw_sky(Surface8& s);
    void draw_range_circle(Surface8& s, uint16_t cur, int range);
    std::vector<uint16_t> cursor_targets() const;
    void cursor_cycle(int d);
    bool cursor_confirm();
    bool target_cursor_up() const;

    GameMode& gm_;
    Mode mode_ = kMenu;
    int sel_ = 0;              // menu entry / list line
    int hover_node_ = -1, picked_node_ = -1;
    uint16_t hover_target_ = 0, picked_target_ = 0, cursor_sel_ = 0;
    int target_sel_ = 0;
    enum Purpose { kForAttack, kForCast } purpose_ = kForAttack;
    enum ListKind { kListCast, kListUse } list_ = kListCast;
    TurnCombat::CastOption cast_;
    uint16_t last_current_ = 0;
    int frame_ = 0;
    bool shown_ = false, out_of_range_shown_ = false;
    bool consumed_down_[3] = {false, false, false};
    bool space_held_ = false, pad_a_held_ = false, mouse_held_ = false;
    int stick_x_ = 0, stick_y_ = 0, stick_repeat_ = 0;
    // Layout of the last frame (UI pixels).
    struct Rect { int x = 0, y = 0, w = 0, h = 0; bool in(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; } };
    Rect menu_, list_box_, auto_btn_, auto_mode_btn_;
    int cur_x_ = 0, cur_y_ = 0;  // the Move destination cursor (screen; keys and the pad)
    void move_cursor(int dx, int dy);
    std::vector<Rect> entry_rects_, list_rects_;
    uint8_t colours_[16] = {};
    uint8_t grey_lut_[256] = {};  // every glyph pixel grey: a locked spell in the list
    bool have_colours_ = false;
    std::vector<uint8_t> flash_lut_;
};

}  // namespace pg
