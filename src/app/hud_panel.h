// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The HUD side panel (PLAN-HUD.md; the engine's own design): the margin a 4:3 picture leaves in a
// wide window, drawn in the game's own pixels and fonts (a panel_w x 200 surface at the game's
// scale, right of the picture; game x 320.. is the panel).
//
//   Outside combat: the party's portraits in one row at the top (the Avatar first; they shrink
//   so they always fit one line; click = the party bar's click, an item dropped on one goes
//   into that member's backpack), the message log below (looks, conversation lines and the
//   answers picked, other messages), scrollable (wheel, the drag bar, the controller).
//   In combat (a turn-based fight, or combat mode in real time): the roster (a tiny portrait,
//   the name, HP cur/max, the Avatar first; the one whose turn it is highlighted), one slot for
//   the monster acting now, the action buttons of the turn-based menu (icons, tooltips; they
//   drive app/turn_ui's menu), and the combat log.
//
//   Controller: R3 (the right stick's press) gives the panel the focus: the D-pad moves the
//   highlight (portraits, the log, the buttons), A presses, Up / Down or the left stick on the
//   log and LB / RB scroll it, B or R3 again gives the focus back. The virtual pointer (right
//   stick) also reaches the panel, A clicks there.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/game_log.h"

namespace pg {

class GameMode;
class Surface8;
class ShapeFont;
struct InputEvent;

class HudPanel {
public:
    explicit HudPanel(GameMode& gm);
    enum Button { kBMove, kBAttack, kBCast, kBUse, kBDefend, kBWait, kBAuto, kBKeepAuto, kBEnd, kBPartyAuto, kButtons };

    void draw(Surface8& s);  // the whole panel (s is panel width x 200)
    void tick();             // once per game frame
    bool combat_view() const;
    bool turn_buttons() const;  // the action buttons are shown (a turn-based fight)

    // Mouse, in panel coordinates (x 0 = the panel's left edge).
    bool mouse_down(int button, int x, int y);
    void mouse_up(int button, int x, int y);
    void mouse_move(int x, int y);
    void leave();
    bool wheel(int dir, int x, int y);
    uint16_t portrait_actor(int x, int y) const;  // the party member under (x, y), 0 = none
    // Controller (focus mode).
    bool pad(const InputEvent& ev);
    bool focused() const { return focus_; }
    void set_focus(bool on);

    // --- Tests and screenshots (rectangles of the last draw, panel coordinates)
    bool button_point(int b, int& x, int& y) const;
    int columns() const { return cols_; }
    bool portrait_point(int i, int& x, int& y) const;
    bool log_point(int& x, int& y) const;  // the middle of the visible log
    int portraits() const { return int(portraits_.size()); }
    int portrait_width() const { return portraits_.empty() ? 0 : portraits_[0].r.w; }
    int scroll() const;              // wrapped lines scrolled up in the log shown
    int log_lines() const;           // wrapped lines in the log shown
    int log_visible() const;         // lines that fit
    const std::string& tooltip() const { return tip_; }
    int focus_item() const { return focus_item_; }
    int roster_rows() const { return roster_rows_; }
    uint16_t monster_slot() const { return monster_; }

    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
        bool in(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };

private:
    struct Wrapped { std::string text; GameLog::Kind kind; bool first; };
    struct LogBox {
        GameLog::Channel ch = GameLog::kMessages;
        Rect r, track, thumb;
        int scroll = 0;          // wrapped lines up from the bottom
        int width = -1;
        uint32_t serial = 0;     // the log serial the wrap was built for
        std::vector<Wrapped> lines;
        bool dragging = false;
        int drag_y0 = 0, drag_scroll0 = 0;
    };
    void layout(int pw);
    void rewrap(LogBox& b);
    int visible(const LogBox& b) const;
    void scroll_by(LogBox& b, int d);
    LogBox& shown_log() { return combat_view() ? combat_ : messages_; }
    const LogBox& shown_log() const { return combat_view() ? combat_ : messages_; }
    void draw_log(Surface8& s, LogBox& b, const char* title);
    void draw_portrait(Surface8& s, uint16_t id, int x, int y, int w, int h);
    void draw_actor_mini(Surface8& s, uint16_t id, int x, int y, int w, int h);
    void draw_icon(Surface8& s, int b, int x, int y, bool on);
    void draw_tooltip(Surface8& s);
    void bevel(Surface8& s, const Rect& r, bool sunk);
    void press_button(int b);
    bool button_enabled(int b) const;
    std::vector<uint16_t> party_ids() const;
    int max_hp_of(uint16_t id);
    const uint8_t* tint(int which);
    uint8_t nearest(int r, int g, int b) const;
    void ensure_colours();
    int focus_count() const;
    int cols_ = 5;                   // the buttons to a row (last layout)
    std::string label_short(uint16_t id, int max_w) const;

    GameMode& gm_;
    int pw_ = 0;
    LogBox messages_, combat_;
    struct Portrait { Rect r; uint16_t id; };
    std::vector<Portrait> portraits_;  // outside combat: the top row; in combat: the roster
    struct Btn { Rect r; int b; };
    std::vector<Btn> buttons_;
    Rect roster_, monster_r_, title_r_;
    int roster_rows_ = 0;
    uint16_t monster_ = 0;           // the monster in the slot
    std::map<uint16_t, int> foe_max_;  // highest HP seen per foe (their "max")
    int hover_x_ = -1, hover_y_ = -1;
    int hover_frames_ = 0;
    int hover_what_ = -1;            // button index (0..) or 100 + portrait index
    std::string tip_;
    Rect tip_anchor_;
    bool focus_ = false;
    int focus_item_ = 0;             // 0..n-1 portraits, n = the log, n+1.. the buttons
    int frame_ = 0;
    int stick_y_ = 0, stick_repeat_ = 0;
    bool have_colours_ = false;
    uint8_t c_bg_ = 0, c_box_ = 0, c_edge_ = 0, c_light_ = 0, c_dark_ = 0, c_hl_ = 0, c_band_ = 0, c_hp_ = 0, c_mana_ = 0,
            c_foe_ = 0, c_grey_ = 0, c_btn_ = 0, c_btn_on_ = 0, c_tip_ = 0;
    uint8_t tints_[8][256] = {};
    std::map<char, uint8_t> icon_cols_;
};

}  // namespace pg
