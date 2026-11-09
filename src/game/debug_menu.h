// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The debug menu (F10 when Debug mode is on in the options): a modal, pausing window in the
// game font with pages of rows. Each row has a label, a shortcut, an optional value and an
// action; pages can open sub-pages (lists of maps, NPCs, items, steps...). Usable with the
// mouse (hover selects, left click acts, right click / Esc goes back, wheel scrolls), the
// keyboard (Up/Down, PgUp/PgDn, Left/Right change, Enter acts, digits type a number, Esc
// back, F10 closes) and the controller (D-pad, A, B; routed by the game mode).
#include <functional>
#include <string>
#include <vector>

#include "game/gumps.h"

namespace pg {

struct DebugRow {
    std::string label;
    std::string shortcut;
    std::function<std::string()> value;      // shown on the right (may be empty)
    std::function<void(int dir)> act;        // 0 = Enter / click, -1 / +1 = Left / Right
    std::function<void(int n)> number;       // digits typed + Enter (optional)
    bool header = false;                     // a section title (not selectable)
    bool closes = false;                     // acting closes the whole menu
};

struct DebugPage {
    std::string title;
    std::function<std::vector<DebugRow>()> build;  // rebuilt every frame (live values)
};

class DebugMenuGump : public Gump {
public:
    DebugMenuGump(World& w, DebugPage root);
    const char* kind() const override { return "debugmenu"; }
    void paint(Surface8& s) override;
    bool hit(int lx, int ly) const override { return true; }
    bool press(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void tick() override;
    void wheel(int dir);
    void push(DebugPage p);
    void back();  // previous page, or close on the root
    void activate(int dir);
    size_t depth() const { return pages_.size(); }
    const std::string& title() const { return pages_.back().title; }
    int selected() const { return sel_; }
    int top() const { return top_; }  // the first row shown
    const std::vector<DebugRow>& rows() const { return rows_; }
    // Selects the first row whose label starts with `label` (tests, scripts); false if none.
    bool select(const std::string& label);
    std::string status;  // a line shown at the bottom (results of the last action)

private:
    void rebuild();
    int row_at(int ly) const;
    void move_sel(int d);
    void keep_visible();
    int visible_rows() const;
    std::vector<DebugPage> pages_;
    std::vector<int> sel_stack_;
    std::vector<DebugRow> rows_;
    int sel_ = 0, top_ = 0;
    std::string digits_;
    uint8_t col_bg_ = 0, col_hi_ = 0, col_edge_ = 0, col_dark_ = 0, col_title_ = 0;
};

}  // namespace pg
