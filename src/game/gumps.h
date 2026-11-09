// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/text_layout.h"

namespace pg {

class World;
class Surface8;
class SaveWriter;
class SaveReader;
struct ShapeFrame;

// Gump layers (specs/presentation/gumps.md 1.1).
enum GumpLayer : int { kLayerNormal = 0, kLayerAbove = 8, kLayerModal = 12 };

// Mouse cursor frames of U8MOUSE.SHP used by the UI (specs/presentation/input.md 4).
enum CursorFrame : int {
    kCursorArrow = -1, kCursorHidden = -2, kCursorTarget = 34, kCursorWait = 35, kCursorHand = 36,
    kCursorQuill = 38, kCursorLens = 39, kCursorRedCross = 40,
};

// One window or widget on screen (gumps.md). Coordinates are 320x200 screen pixels; a gump's
// local origin is its top-left. Top-level gumps live in World::gumps.
class Gump {
public:
    explicit Gump(World& w) : w_(w) {}
    virtual ~Gump() = default;
    virtual const char* kind() const = 0;

    int x = 0, y = 0, w = 0, h = 0;
    int layer = kLayerNormal;
    unsigned shape = 0, frame = 0;  // picture in U8GUMPS (0 = none)
    uint16_t owner = 0;             // the item it belongs to
    uint16_t notify_pid = 0;        // ended (with `result`) when the gump closes
    uint32_t result = 0;
    bool modal = false, draggable = false, item_dependent = false;
    bool pauses = false;            // modal and pausing the world (gumps.md 1.3)
    int cursor = kCursorArrow;      // the cursor while this gump is the top modal one
    bool closed = false;
    uint32_t open_seq = 0;          // opening order (Gumps::add); the controller's B closes LIFO

    // Drawing and hit testing (local coordinates).
    virtual void paint(Surface8& s);
    virtual bool hit(int lx, int ly) const;
    virtual void tick() {}
    // Mouse, in local coordinates. press returns false to let the press fall through.
    virtual bool press(int button, int lx, int ly) { return true; }
    virtual void release(int button, int lx, int ly) {}
    virtual void click(int button, int lx, int ly) {}
    virtual void double_click(int button, int lx, int ly) {}
    virtual void hover(int lx, int ly) {}
    // Keys; true if handled.
    virtual bool key(int key, unsigned mods) { return false; }
    virtual bool text(const std::string& utf8) { return false; }

    // Items shown in the gump (containers, paperdoll).
    virtual uint16_t trace_item(int lx, int ly) const { return 0; }
    virtual bool can_drag_out(uint16_t item) const { return false; }
    // Where an item dragged from it would be drawn (local), for the hidden-item rule.
    enum Drop { kDropNo, kDropPlace, kDropInto };
    // gx, gy: the grab offset (cursor minus the item's draw anchor).
    virtual Drop drag_over(uint16_t item, int lx, int ly, int gx, int gy) { return kDropNo; }
    std::string drop_why;  // why the last drag_over said kDropNo ("" = no reason given)
    virtual void drag_leave() {}
    virtual bool drop(uint16_t item, int lx, int ly, int gx, int gy) { return false; }
    // Called when the gump goes away (before it is deleted).
    virtual void on_close() {}
    // Savegames: only non-modal gumps are stored.
    virtual void save(SaveWriter& out) const;
    virtual void load(SaveReader& in);

    // Draw anchor of an item shown in this gump (screen), for the drag ghost.
    virtual bool item_anchor(uint16_t item, int& sx, int& sy) const { return false; }

    void close() { closed = true; }
    bool contains(int sx, int sy) const { return sx >= x && sy >= y && sx < x + w && sy < y + h; }
    World& world() const { return w_; }

protected:
    void draw_shape(Surface8& s, unsigned gshape, unsigned gframe, int lx, int ly) const;
    const ShapeFrame* gump_frame(unsigned gshape, unsigned gframe) const;
    World& w_;
};

// A container's or dead body's window (gumps.md 2.6). The paperdoll derives from it.
class ContainerGump : public Gump {
public:
    ContainerGump(World& w, uint16_t item, unsigned gump_shape);
    const char* kind() const override { return "container"; }
    void paint(Surface8& s) override;
    bool hit(int lx, int ly) const override;
    void tick() override;
    void click(int button, int lx, int ly) override;
    void double_click(int button, int lx, int ly) override;
    uint16_t trace_item(int lx, int ly) const override;
    bool can_drag_out(uint16_t item) const override;
    Drop drag_over(uint16_t item, int lx, int ly, int gx, int gy) override;
    void drag_leave() override { ghost_item_ = 0; }
    bool drop(uint16_t item, int lx, int ly, int gx, int gy) override;
    void on_close() override;
    bool item_anchor(uint16_t item, int& sx, int& sy) const override;
    void save(SaveWriter& out) const override;
    void load(SaveReader& in) override;

    // Item-relative placement (gumps.md 1.4): offset from the anchor, added by dragging.
    int off_x = 0, off_y = 0;
    bool placed = false;
    void follow_item();
    // Where an item sits inside (item-area coordinates); places unplaced ones at random.
    void place_contents();
    int area_l = 0, area_t = 0, area_w = 0, area_h = 0;
    // The dragged item is not drawn at its old spot.
    uint16_t hidden_item = 0;

    // Sort (game/container_sort.h): the button below the item area, and the re-layout once
    // the combine events of merged stacks have ended.
    virtual bool can_sort() const { return true; }
    bool sort_button(int& bx, int& by, int& bw, int& bh) const;
    // Someone else's container (a body, a chest; not the Avatar's or a party member's): a double-click
    // on an item takes it into the backpack (a container item still opens), and a TAKE ALL button
    // left of SORT takes everything that fits (NOTES.md "Looting").
    bool foreign() const;
    bool take_button(int& bx, int& by, int& bw, int& bh) const;
    int take_all();  // how many were taken
    bool take_down = false;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    std::vector<uint16_t> sort_pids;
    int sort_wait = 0;
    bool sort_down = false;

protected:
    uint16_t ghost_item_ = 0;
    int ghost_x_ = 0, ghost_y_ = 0;
    uint16_t ghost_target_ = 0;
};

// The Avatar's paperdoll: equipment, stats and the stats button (gumps.md 2.5).
class PaperdollGump : public ContainerGump {
public:
    PaperdollGump(World& w, uint16_t actor);
    const char* kind() const override { return "paperdoll"; }
    bool can_sort() const override { return false; }
    void paint(Surface8& s) override;
    bool hit(int lx, int ly) const override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void double_click(int button, int lx, int ly) override;
    uint16_t trace_item(int lx, int ly) const override;
    bool can_drag_out(uint16_t item) const override;
    Drop drag_over(uint16_t item, int lx, int ly, int gx, int gy) override;
    bool drop(uint16_t item, int lx, int ly, int gx, int gy) override;
    bool item_anchor(uint16_t item, int& sx, int& sy) const override;
    static void slot_pos(unsigned slot, int& x, int& y);
    void click(int button, int lx, int ly) override;
    void tick() override;

    // Engine extras (game/sheet_extras.cpp; PLAN-VIRTUES.md 2), all in local coordinates.
    // The level line under the stats button (everyone's sheet, Leveling on): "LEVEL N" and a short
    // XP bar on the right; a click on the bar tells the XP.
    bool level_line_shown() const;
    bool xp_bar_rect(int& bx, int& by, int& bw, int& bh) const;  // false when not shown
    int xp_bar_fill(int width) const;  // filled pixels of a bar this wide
    // The virtue octagon (the Avatar's sheet, Virtues on): gems 0..7 are the virtues
    // (game/virtues.h order), 8 the centre gem.
    bool octagon_shown() const;
    static void gem_centre(int gem, int& cx, int& cy);
    int gem_at(int lx, int ly) const;  // -1: none (or the octagon is not shown)
    // What a left click at that spot says ("" = nothing; the click goes on to the items).
    std::string click_message(int lx, int ly) const;
    // The message on the sheet after such a click (not saved).
    std::string note;
    int note_ticks = 0, note_x = 0, note_y = 0;

private:
    bool over_backpack(int lx, int ly) const;
    void paint_level_line(Surface8& s);
    void paint_octagon(Surface8& s);
    void paint_note(Surface8& s);
    bool stats_down_ = false;
    uint32_t anim_ = 0;  // ticks this sheet has been open (the gems' pulse)
};

// The minimised paperdoll: hit point and mana bars (gumps.md 2.7).
class MiniStatsGump : public Gump {
public:
    explicit MiniStatsGump(World& w);
    const char* kind() const override { return "ministats"; }
    void paint(Surface8& s) override;
    void double_click(int button, int lx, int ly) override;
};

// A quantity slider (gumps.md 2.10). Modal. On close it calls `done(value)` (stack splits) or
// wakes the usecode process that asked (intrinsic 0x04B).
class SliderGump : public Gump {
public:
    SliderGump(World& w, int min, int max, int step, int value);
    const char* kind() const override { return "slider"; }
    void paint(Surface8& s) override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void on_close() override;
    void set_value(int v);
    int value() const { return value_; }
    void accept() { accepted_ = true; close(); }

    std::function<void(int)> done;
    uint16_t wake_pid = 0;  // usecode process suspended by 0x04B

private:
    int knob_x() const;
    int min_, max_, step_, value_;
    bool dragging_ = false, ok_down_ = false;
    int press_part_ = 0;
    bool accepted_ = false;
};

// Books, scrolls, gravestones and plaques (gumps.md 2.11). Modal; the world is paused.
class ReaderGump : public Gump {
public:
    enum Kind { kBook, kScroll, kGrave, kPlaque };
    ReaderGump(World& w, Kind kind, uint16_t item, unsigned gump_shape, const std::string& text);
    const char* kind() const override { return "reader"; }
    void paint(Surface8& s) override;
    bool press(int button, int lx, int ly) override;
    void click(int button, int lx, int ly) override;
    void double_click(int button, int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void advance();
    // Turns one page (a book: one spread) back (-1) or on (+1) without closing at the end:
    // Left / Right, PgUp / PgDn and the controller's LB / RB. False if there is no such page.
    bool turn(int dir);
    bool paged() const { return reader_kind == kBook || reader_kind == kScroll; }

    Kind reader_kind;
    int font = 9;
    std::vector<TextPage> pages;
    unsigned page = 0;
    int frames_open = 0;
    void tick() override;
};

// The quit confirmation (gumps.md 2.15).
class QuitGump : public Gump {
public:
    explicit QuitGump(World& w);
    const char* kind() const override { return "quit"; }
    void paint(Surface8& s) override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    bool key(int key, unsigned mods) override;

private:
    int down_ = 0;  // 1 yes, 2 no
};

// The main menu, the name entry and the diary (save / load) pages: all drawn on the diary
// book (gumps.md 2.2-2.4).
class MenuGump : public Gump {
public:
    enum Mode { kMenu, kName, kLoad, kSave, kOptions };
    MenuGump(World& w, Mode mode);
    const char* kind() const override { return "menu"; }
    void paint(Surface8& s) override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void click(int button, int lx, int ly) override;
    void double_click(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    bool text(const std::string& utf8) override;
    void tick() override;
    void on_close() override;
    void choose(int entry);       // menu entry 1..8
    // The start menu (engine design, not the original; NOTES.md "Start menu"): shown at launch
    // instead of the intro when saves exist, over the most recent save (already loaded, so
    // closing it is "Continue"). Adds "Continue" and "New Game" (with the intro) to the menu.
    bool start_menu = false;
    // Everything choosable on the menu page, in keyboard / controller order (codes: 0..7 the
    // entries, kItemMap / kItemHelp the text entries, kItemDeath the death menu's autosave line,
    // kItemContinue / kItemNewGame the start menu's lines). Up / Down move the highlight,
    // Enter / Space (pad A) choose it; the mouse highlights what it is over.
    enum { kItemMap = 10, kItemHelp = 11, kItemDeath = 20, kItemContinue = 30, kItemNewGame = 31 };
    std::vector<int> menu_items() const;
    int menu_item_at(int lx, int ly) const;  // -1 = none
    void activate(int item);
    int selected_item() const { return sel_; }
    int default_item() const;  // highlighted when the menu opens: the death or start menu's first line
    void select_item(int item) { sel_ = item; sel_mouse_ = false; }
    void set_mode(Mode m);
    Mode mode() const { return mode_; }
    // Menu entry i (0..7) is shown (7 Quotes / 8 End Game need the persistent flags).
    bool entry_visible(int i) const;
    // The options page rows (gumps.md 2.3 + engine extras; game/options_page.cpp, NOTES.md
    // "Options page"). change(dir): dir +1 click / Right / Enter / wheel up, -1 Left / right
    // click / wheel down. Changes are stored through World::store_options(). The three-field
    // form {label, value, change} still works: the group is then guessed from the label
    // (unknown labels go to "Extras").
    struct OptionRow {
        enum Kind { kToggle, kChoice, kSlider };
        std::string label;
        std::function<std::string()> value;
        std::function<void(int dir)> change;
        std::string group;                 // "Game", "Sound", "Controls", "Display", "Party", "Extras" or new
        Kind kind = kToggle;               // a slider also steps with the wheel
        std::function<bool()> visible;     // empty = always shown
    };
    std::vector<OptionRow> option_rows();
    void change_option(int i, int dir);
    // The options page layout (computed from the book picture and the font; every line lies
    // inside a page text area, see option_page_rect).
    struct OptionLine {
        int row = -1;                      // index into option_rows(); -1 = a group title, -2 the hint
        int page = 0;                      // spread = page / 2, side = page % 2 (0 left)
        int x = 0, y = 0, w = 0, h = 0;    // gump-local box of the line
        std::string label, value;          // as drawn (fitted to the box)
        int value_x = 0;                   // gump-local x of the value
    };
    struct OptionLayout {
        std::vector<OptionLine> lines;
        int pages = 0;
        int spreads() const { return (pages + 1) / 2; }
    };
    OptionLayout option_layout();
    // The text area of page side 0 (left) / 1 (right), gump-local, margins applied.
    void option_page_rect(int side, int& px, int& py, int& pw, int& ph) const;
    // The page-turn dog-ears (shape 34) on the options page: 0 previous, 1 next.
    bool option_dogear(int which, int& dx, int& dy, int& dw, int& dh) const;
    // Where the dog-ears are drawn on the book (diary and options, gump-local): 0 the previous
    // page's at the top-left corner of the left page, 1 the next page's at the top-right corner
    // of the right page (the mirror image of the left one: its tip on the page's outer edge).
    void dogear_box(int which, int& dx, int& dy, int& dw, int& dh) const;
    // A point is on a dog-ear when it is on (or within 2 pixels of) its drawn pixels.
    bool dogear_hit(int which, int lx, int ly) const;
    void draw_dogear(Surface8& s, int which) const;
    // The book has pages to turn (diary, options): LB / RB and PgUp / PgDn turn them.
    bool paged() const { return mode_ == kLoad || mode_ == kSave || mode_ == kOptions; }
    static constexpr int kOptionFont = 4, kOptionTitleFont = 9;
    int option_spread = 0;
    int option_selected() const { return option_sel_; }
    void select_option(int row);       // also turns to its spread
    void turn_options(int dir);        // next / previous spread
    void wheel(int dir);               // mouse wheel while the book is open
    // Milestone 6 text entries under the menu pictures (0 Map, 1 Controls; -1 none).
    int extra_at(int lx, int ly) const;
    void choose_extra(int i);
    int hover_extra_ = -1;
    // Diary: the slot under a point of the book (0 = none).
    int slot_at(int lx, int ly) const;
    void pick_slot(int slot);     // load it, or start editing its description
    void finish_edit();           // Enter in save mode
    // Autosaves (app/debug_tools.h): on the load page, spread -1 lists them before the diary;
    // the death menu offers the newest one.
    int min_spread() const;
    std::vector<int> autosave_order() const;  // slots, newest first
    void paint_autosaves(Surface8& s);
    bool death_autosave(int& slot) const;     // dead, and an autosave exists
    bool over_death_line(int lx, int ly) const;
    int spread = 0;
    // The diary entry highlighted by the keyboard / controller (Up / Down; Enter picks it).
    int diary_sel = 0;
    std::vector<int> spread_slots() const;  // the choosable slots on the current spread
    int editing = 0;              // slot whose description is being typed
    std::string edit_text;
    // The edit was only offered (Write Diary opens ready to type on the next free entry, or a
    // click on an entry): nothing typed yet, so the arrows, page turns and Esc still navigate.
    bool edit_touched = false;
    // Write Diary: the entry it opens on (the first free one after the newest diary entry).
    int next_free_slot() const;

private:
    void paint_menu(Surface8& s);
    void paint_diary(Surface8& s);
    void paint_options(Surface8& s);
    void paint_entry_number(Surface8& s, int n, int x, int y);
    int option_at(int lx, int ly);
    bool options_press(int button, int lx, int ly);
    bool options_release(int button, int lx, int ly);
    bool options_key(int key);
    int option_sel_ = -1;
    int option_hover_ = -1;
    bool text_line_box(int item, int& bx, int& by, int& bw, int& bh) const;
    int sel_ = -1;             // the highlighted menu item (menu_items code), -1 none
    bool sel_mouse_ = false;   // set by the mouse (cleared again when it leaves)
    int down_item_ = -1;       // the menu item the button went down on (acts on release)
    int mode_tick_ = -1000;    // caret_ticks_ when the mode last changed (double-click guard)
    Mode mode_;
    int hover_ = -1;
    int caret_ticks_ = 0;
    int down_button_ = 0;  // dog-ears: 1 previous, 2 next
    int ticks_ = 0, mode_ticks_ = -1;  // frames open / at the last mode change (text guard)
    void turn_diary(int dir);
};

// All open gumps, sorted by layer (last = topmost), with focus and modality.
class Gumps {
public:
    explicit Gumps(World& w) : w_(w) {}
    Gump* add(std::unique_ptr<Gump> g);
    void close_all();
    void close_item_dependent();
    // Removes closed gumps (calls on_close, ends notify processes, unpauses).
    void reap();
    void tick();
    void paint(Surface8& s, int min_layer, int max_layer);
    // Topmost gump hit at a screen point (modal gumps take every point).
    Gump* at(int sx, int sy) const;
    Gump* top_modal() const;
    Gump* focus() const { return list.empty() ? nullptr : list.back().get(); }
    Gump* container_gump(uint16_t item) const;
    template <class T> T* find() const {
        for (auto& g : list)
            if (auto* t = dynamic_cast<T*>(g.get()); t && !t->closed) return t;
        return nullptr;
    }
    bool is_open(const Gump* g) const;
    void raise(Gump* g);
    bool saving_blocked() const { return top_modal() != nullptr; }
    // The most recently opened window that the player can close (not modal, not the party
    // bar): the controller's B closes these one by one.
    Gump* newest_window() const;
    void save(SaveWriter& out) const;
    bool load(SaveReader& in);
    std::vector<std::unique_ptr<Gump>> list;

private:
    World& w_;
    uint32_t seq_ = 0;
};

}  // namespace pg
