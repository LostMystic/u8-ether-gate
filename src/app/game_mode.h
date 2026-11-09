// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/savegame.h"
#include "game/world.h"
#include "platform/input.h"
#include "world/world_view.h"

namespace pg {

struct GameData;
class Surface8;
class AtlasView;
class TileCache;
struct ShapeFrame;
class ShapeFont;
class DebugTools;
class TurnUI;
class HudPanel;

// The default mode: a new game running its own usecode. The world is drawn from the camera;
// gumps (containers, paperdoll, readers, the menu book...), barks and the answer menu are
// drawn on top. Mouse and keys go to the gumps first (specs/presentation/gumps.md 1.2-1.6),
// then to the Avatar controller through World::input (specs/world/avatar.md,
// specs/presentation/input.md): right button = move, left click = look, double click = use,
// drag = move/throw items or move windows, both buttons / Space = jump, C = combat,
// Esc = main menu. Alt+arrows scroll the view as a debug aid (Home gives the camera back).
class GameMode {
public:
    explicit GameMode(const GameData& data);
    ~GameMode();
    void start();
    // The launch (NOTES.md "Start menu"): start(), then, if `start_menu` and a save exists, the
    // "On Start" setting decides: "menu" (default) loads the most recent save and opens the
    // start menu over it, "continue" just loads it, "intro" leaves the new game (with its
    // intro). Returns the slot loaded (0: a new game).
    int launch(bool start_menu);
    // The "On Start:" options row (Game group).
    static void register_options();
    void handle(const InputEvent& ev);
    void tick();  // one game frame
    void render(Surface8& surf);
    World& world() { return *world_; }
    void set_hud(bool on) { hud_ = on; }
    void set_mouse(int x, int y) { mouse_x_ = x; mouse_y_ = y; mouse_fx_ = x + 0.5f; mouse_fy_ = y + 0.5f; }
    void set_log_items(bool on) { opt_.log_items = on; }
    void set_roof_hiding(bool on) { opt_.roof_hiding = on; }
    void set_show_editor(bool on) { opt_.show_editor = on; }
    // The palette transform to present with.
    const PaletteTransform& palette() const { return world_->palette; }
    // The player confirmed quitting (quit box).
    bool wants_quit() const { return quit_; }
    // Scripted input for headless runs: "FRAME:ACTION[:ARGS];..." with actions
    // move:X,Y  ldown  lup  rdown  rup  mdown  mup  key:K  keyup:K  (K = up, down, left,
    // right, space, shift, ctrl, esc, return, backspace, f1..f12 or a letter) and type:TEXT;
    // events at FRAME are fed before that frame.
    // Debug actions: warp:X,Y,Z,MAP  spawn:SHAPE,DX,DY (a hostile monster near the Avatar)
    // equip:SHAPE (put a weapon or armour piece on the Avatar)  give:SHAPE[,QTY] (into the
    // backpack)  save:SLOT[,DESC]  load:SLOT  shot:FILE (screenshot at that frame).
    // debug:NAME runs a debug-menu action (hud boxes editor routes crit noclip god mana heal
    // pause step speed hour snapshot load_snapshot warp_atlas...; app/debug_tools.cpp).
    bool set_script(const std::string& script);
    // Restarts after the Avatar's death in headless (--auto-ui) runs.
    unsigned restarts() const { return restarts_; }

    // Savegames (formats/savegame.md): slots in `saves()`; false on failure.
    SaveStore& saves() { return saves_; }
    bool save_slot(int slot, const std::string& description);
    bool load_slot(int slot);
    // A save held in memory (the debug menu's story checkpoints): as load_slot, no slot.
    bool load_bytes(const std::vector<uint8_t>& data, const std::string& what);
    // Screenshot requests from the script (the runner writes them).
    std::vector<std::string> shot_requests;
    // Mouse state for tests.
    int mouse_x() const { return mouse_x_; }
    int mouse_y() const { return mouse_y_; }
    bool dragging() const { return drag_ != kDragNone; }
    // The object drawn at a screen pixel in the last rendered frame (0 if none). Screen
    // pixels are game-surface (UI) pixels at every zoom.
    uint16_t pick(int x, int y);
    uint16_t pick_f(float x, float y);

    // --- Milestone 6 (PLAN-M6.md): the composed frame, zoom, smooth scrolling, controller,
    // auto-target, the atlas from the game, the help page, click-through for barks.
    // Draws a frame at `scale` output pixels per game pixel: the world (zoomed, from the
    // smooth camera, actors at their displayed positions) under the UI layer. alpha is the
    // time since the last world tick in ticks (0..1), dt the real time since the last frame.
    void render_frame(std::vector<uint32_t>& out, int& w, int& h, int scale, double alpha, double dt);
    // Zoom: the world drawn `step` output pixels smaller per game pixel (0 = 1x).
    void zoom_by(int delta);
    int zoom_step() const { return world_->controls.zoom_step; }
    // The zoom as a fraction n/d at the current frame scale.
    void zoom_fraction(int& n, int& d) const;
    int zoom_limit(int S) const;  // the furthest step the player may use (debug mode: all)
    static constexpr int kMaxZoomStep = 3;
    static constexpr int kFarSteps = 3;  // far zoom levels beyond the live limit (2, 4, 8 world px per output px)
    // Tile building time per frame at far zoom (ms; < 0 = build everything now: screenshots).
    void set_far_budget(double ms) { far_budget_ms_ = ms; }
    int far_missing() const { return far_missing_; }
    static int max_zoom_step(int frame_scale);
    // Called when a setting the application persists changed (zoom, controls...).
    std::function<void()> on_settings_changed;
    void toast(const std::string& text, int frames = 60);
    // The world atlas opened from the game (M / Tab, the main menu, the controller's Back).
    void set_atlas_layout_path(const std::string& p) { atlas_path_ = p; }
    void open_atlas();
    void close_atlas();
    bool atlas_open() const { return atlas_on_; }
    // The help page (F1, holding Back, the main menu's Controls entry).
    bool help_open() const { return help_page_ >= 0; }
    int help_pages() const;  // the help book's page count (sections split to fit; game_view.cpp)
    int help_overflows() const;  // lines too wide for their column (tests; game_view.cpp)
    // Debug mode, the critical path and autosaves (app/debug_tools.h). open_debug_menu() is
    // F10 in debug mode (a controller binding can call it too).
    DebugTools& debug() { return *debug_; }
    void open_debug_menu();
    void open_help() { help_page_ = 0; }
    // Controller state (tests read it).
    bool pad_connected() const { return pads_ > 0; }
    bool pointer_active() const;
    uint16_t auto_target() const { return target_; }
    const std::vector<uint16_t>& target_candidates() const { return targets_; }
    // Frames the game mode has run (its input clock).
    int frames() const { return frames_; }
    // The centre of answer button i of the last drawn answer menu (tests).
    bool ask_button_point(unsigned i, int& x, int& y) const {
        const AskMenu* m = world_->ask_menu.get();
        if (!m || i >= m->buttons.size()) return false;
        x = ask_x_ + m->buttons[i].x + m->buttons[i].w / 2;
        y = ask_y_ + m->buttons[i].y + m->buttons[i].h / 2;
        return true;
    }
    // Bark pages ended early by clicks / keys / the pad (statistics).
    unsigned pages_skipped() const { return pages_skipped_; }
    // Mouse position with sub-pixel precision (tests).
    void set_mouse_f(float x, float y) { mouse_fx_ = x; mouse_fy_ = y; mouse_x_ = int(x); mouse_y_ = int(y); }
    // The last composed view (tests and screenshots): output scale, world pixel size.
    int view_scale() const { return vs_.S; }
    int view_k() const { return vs_.k; }
    // Displayed position of an actor in the last frame (smooth motion), world units.
    bool display_pos(uint16_t id, int& x, int& y, int& z) const;
    // The smooth camera's screen position in the last frame (camSx, camSy as reals).
    void view_camera(double& sx, double& sy) const { sx = vs_.cam_sx_f; sy = vs_.cam_sy_f; }
    // Time of the last composed frame: the world render and the composition (ms).
    double world_ms() const { return world_ms_; }
    double compose_ms() const { return compose_ms_; }

private:
    // The world point on top of the item under screen pixel (px, py); false if none.
    bool world_point(int px, int py, uint16_t& id, int pt[3]);
    void draw_barks(Surface8& surf);
    void draw_ask(Surface8& surf);
    void draw_cursor(Surface8& surf);
    bool ask_hit(int mx, int my, unsigned& index) const;
    void update_cursor();
    void look_item(uint16_t id);
    void toggle_combat(bool with_anim);
    void hotkey_use(unsigned shape);
    // Loot (double-click in someone else's container, its Take All button): into the backpack.
    bool take_item(uint16_t id);
public:
    // v2.12 hotkeys (NOTES.md "v2.12 features"): F7 quick-save to SaveStore::kQuickSlot (refused
    // with the red cross in stasis, with a modal window open, while a process blocks saving or
    // when he is dead), F8 loads it (nothing there: the red cross). True if it happened.
    bool quick_save();
    bool quick_load();
    // B / R / K: use the first bedroll (534) / recall item (833) / keyring (79) in the Avatar's
    // inventory, nested containers too (nothing in stasis or with none). The item used, or 0.
    uint16_t use_first_item(unsigned shape);
    // v2.12 stacking: item `id`, just put into a container by the player, joins the first
    // compatible stack there ("v2.12 changes" On). The receiving stack's id, 0 if none.
    static uint16_t auto_merge(World& w, uint16_t id);
    static constexpr unsigned kBedroll = 534, kRecall = 833, kKeyring = 79;
private:
    void key_down(const InputEvent& ev);
    void mouse_down(int b);
    void mouse_up(int b);
    void mouse_move();
    // Drag and drop (avatar.md 8, gumps.md 1.6).
    void start_drag();
    void update_drag();
    void finish_drag();
    void cancel_drag();
    void update_world_drop();
    // Performs a drop of `id` (or `amount` of it, splitting a stack) to the current target.
    struct DropAt {
        Gump* gump = nullptr;
        int lx = 0, ly = 0, gx = 0, gy = 0;
        int kind = 0;  // world: DropKind
        int pt[3] = {0, 0, 0};
        uint16_t target = 0;
    };
    bool perform_drop(uint16_t id, const DropAt& at);
    // The end of a drop: the stack slider, then perform_drop and the theft check.
    void drop_now(uint16_t id, const DropAt& at);
    // Walk to act (app/auto_walk.cpp, NOTES.md "Walk to act"): a use or a pick-up into a pack
    // that is out of reach but within auto_walk_range and safely walkable makes the Avatar walk
    // there first and act on arrival; anything the player does meanwhile cancels it.
    struct AutoWalk {
        enum Kind { kNone, kUse, kDrop } kind = kNone;
        uint16_t target = 0;  // the item to use / to pick up
        DropAt at;            // kDrop: where it goes
        uint16_t pid = 0;     // the Avatar's pathfind process
        int tries = 0, frames = 0;
    } walk_;
    bool far_drag_ = false;  // dragging an item out of reach: only a pack can take it (walk first)
    bool auto_walk_eligible(uint16_t id) const;
    bool auto_walk_path(uint16_t id) const;
    bool auto_walk_begin(AutoWalk::Kind kind, uint16_t id, const DropAt* at);
    void auto_walk_tick();
    void auto_walk_input(const InputEvent& ev);
    void update_far_drop();
public:
    // Walk to act: the furthest gap (world units, x and y, between the footprints) the Avatar
    // walks to reach a target; 0 = off (the red cross as before). 512 = 16 footprint cells,
    // about four floor tiles: roughly to the edge of the unzoomed screen.
    int auto_walk_range = 512;
    static constexpr int kAutoWalkRepaths = 3;          // re-paths (a target that moved) before giving up
    static constexpr int kAutoWalkMaxFrames = 30 * 20;  // 20 s
    bool auto_walk_pending() const { return walk_.kind != AutoWalk::kNone; }
    void auto_walk_cancel(const char* why);
private:
    int slider_start(uint16_t id, const DropAt& at) const;  // a stack's split: how many to offer first
    void handle_requests();
    uint32_t now_ms() const { return uint32_t(uint64_t(frames_) * 1000 / 30); }
    bool gump_alive(const Gump* g) const { return g && world_->gumps.is_open(g); }
    // Milestone 6 helpers (game_view.cpp, game_pad.cpp).
    void render_world(Surface8& surf, const int cam[3]);
    void render_ui(Surface8& surf, bool flip_world_only);
    void flash_picture(std::vector<uint32_t>& out);
    void draw_overlays(Surface8& surf);  // auto-target, toasts, help page
    void draw_help(Surface8& surf);
    void record_motion();                // per tick: actor positions for smooth display
    void screen_to_world_px(float ux, float uy, int& wx, int& wy) const;
    void pad_event(const InputEvent& ev);
    void pad_tick();
    void update_targets();
    bool bark_click(bool anywhere);      // click-through: true if it ended a page
    void bark_tick();
    bool scene_running() const;          // stasis / a usecode scene with barks up
    void choose_ask_selection();
    // Answer menu highlight (game/ask_nav.h): a new menu resets it; d is an AskDir.
    void ask_sync();
    void ask_nav(int d);
    int ask_exit() const;  // the answer B picks (bye / back), -1 none
    void synth_mouse(int button, bool down);
    void pointer_to_target();
    bool fast_forward() const;
    void game_atlas_input(const InputEvent& ev);
    void atlas_tick();
public:
    // Click-through guard (PLAN-M6.md): a bark page, or the answer menu, cannot be advanced or
    // answered for this many frames (0.5 s) after it appears.
    static constexpr int kTextGuardFrames = 15;
    // An answer menu ignores clicks and the pad's A only in its first moment (4 frames: a
    // press meant for the line before); later presses always work.
    static constexpr int kMenuGuardFrames = 4;
    int last_pad_answer() const { return last_pad_answer_; }
    int ask_highlight() const { return ask_sel_; }
    // Stick / D-pad held in an answer menu: first repeat after 300 ms, then every 150 ms.
    static constexpr int kMenuRepeatFirstMs = 300, kMenuRepeatMs = 150;
    static constexpr int kMenuStick = 16000;  // stick deflection that moves the highlight
private:
    std::unique_ptr<Surface8> world_surf_own_, ui_surf_own_;
    std::vector<uint8_t> ui_mask_;
    std::vector<uint32_t> world_rgba_;
    std::string pad_name_;
    int back_down_frame_ = -1, start_down_frame_ = -1;
    bool start_long_ = false;
    bool lb_down_ = false, lb_used_ = false;
    int party_pick_frame_ = -1000;
    bool back_help_ = false;
    bool atlas_keys_[4] = {};          // stick directions sent to the atlas as held arrows
    float mouse_fx_ = -1, mouse_fy_ = -1;
    float mfx() const { return mouse_fx_ >= 0 ? mouse_fx_ : mouse_x_ + 0.5f; }
    float mfy() const { return mouse_fy_ >= 0 ? mouse_fy_ : mouse_y_ + 0.5f; }
    // The composed view of the last frame (identity = the plain 320 x 200 render).
    struct ViewState {
        bool layered = false;
        int S = 1, k = 1, ww = 320, wh = 200;
        double frac_x = 0, frac_y = 0;  // sub-pixel camera offset in world pixels
        int cam_sx = 0, cam_sy = 0;     // integer camera screen position used for the render
        double cam_sx_f = 0, cam_sy_f = 0;
        int far = 0;     // far zoom: 2^far world pixels per output pixel (0 = live)
        double kf = 1;   // output pixels per world pixel
    } vs_;
    // Far zoom (game_view.cpp render_far).
    void render_far(std::vector<uint32_t>& out, int S, int far, bool glide);
    std::unique_ptr<TileCache> far_cache_;
    struct FarDrawn { uint16_t id; const ShapeFrame* f; bool flipped; double x0, y0, w, h, m; };
    std::vector<FarDrawn> far_drawn_;
    int far_missing_ = 0;
    // Smooth motion: per actor the last two positions and when they changed (world ticks).
    struct Motion { int from[3], to[3]; int t = 0, dur = 1; int last_change = -100; unsigned map = 0; };
    std::map<uint16_t, Motion> motion_;
    int cam_prev_[3] = {0, 0, 0}, cam_cur_[3] = {0, 0, 0};
    unsigned cam_map_ = 0xFFFF;
    double ease_[2] = {0, 0};          // camera ease offset in screen pixels (decays)
    double last_view_[2] = {0, 0};
    bool have_last_view_ = false;
    uint32_t shift_seen_ = 0;
    std::string atlas_loaded_path_;  // the layout file the atlas was built from  // World::region_shift.serial last applied (stitched seam crossings)
    double alpha_ = 0;
    double world_ms_ = 0, compose_ms_ = 0;
    double far_budget_ms_ = 6;
    // Toasts and help.
    std::string toast_;
    int toast_frames_ = 0;
    int help_page_ = -1;
    // The atlas.
    std::unique_ptr<AtlasView> atlas_;
    std::vector<ShapeFont> atlas_fonts_;
    std::string atlas_path_;
    bool atlas_on_ = false;
    // Controller.
    int pads_ = 0;
    bool pad_btn_[15] = {};
    int pad_axis_[6] = {};
    std::map<int, std::array<int, 6>> pad_axes_by_id_;
    std::set<int> pad_waiting_;  // connected controllers that have not pressed a button yet
    int pad_press_frame_[15] = {};
    int pointer_frames_ = 100000;      // frames since the right stick last moved the pointer
    bool pointer_on_target_ = false;   // A put the pointer on the auto-target (the stick has not moved it since)
    bool pointer_buttons_[2] = {};     // A / B are acting as the left / right mouse button
    int dpad_repeat_ = 0;
    uint16_t target_ = 0;
    std::vector<uint16_t> targets_;
    int target_pick_frame_ = -1000;    // the player cycled the target at this frame
    int ask_sel_ = -1;
    int last_pad_answer_ = -1;
    uint16_t ask_pid_ = 0;             // the menu ask_sel_ belongs to
    bool ask_keys_ = false;            // the arrow keys moved the highlight in this menu (Enter answers)
    int menu_stick_dir_ = -1, menu_dpad_dir_ = -1;  // held direction (AskDir) of the stick / D-pad
    uint32_t menu_stick_next_ = 0, menu_dpad_next_ = 0;  // when it repeats (now_ms)
    // Click-through.
    std::map<uint16_t, std::pair<unsigned, int>> bark_seen_;  // speaker -> (page, frame shown)
    unsigned pages_skipped_ = 0;
    bool text_hold_ = false;           // a click-through button is held (fast-forward)
    int text_hold_frames_ = 0;
    bool swallow_left_ = false;        // the left press ended a page: its release does nothing
    struct BarkRect { uint16_t speaker; int x, y, w, h; };
    std::vector<BarkRect> bark_rects_;

    const GameData& data_;
    std::unique_ptr<World> world_;
    std::unique_ptr<DebugTools> debug_;
    friend class DebugTools;
    // Turn-based combat screen (app/turn_ui.h, game/turn_combat.h).
    std::unique_ptr<TurnUI> turn_ui_;
public:
    TurnUI& turn_ui() { return *turn_ui_; }
    // --- The HUD side panel (app/hud_panel.h, PLAN-HUD.md). The application sets its width in
    // game pixels each frame (0 = Classic: no panel, the party bar and the floating turn menu as
    // before; tests that never set it run the classic screen). Mouse events with x >= 320 are
    // the panel's; render_panel draws it (panel_width x 200).
    void set_panel_width(int pw);
    int panel_width() const { return panel_w_; }
    bool panel_on() const { return panel_w_ > 0; }
    HudPanel& hud_panel() { return *hud_panel_; }
    void render_panel(Surface8& s);
    void render_panel_rgba(std::vector<uint32_t>& out, int& w, int& h);
    // The "HUD: Side panel / Classic" setting (Config key "hud": "panel" (default) or "classic").
    static bool side_panel_setting();
private:
    std::unique_ptr<HudPanel> hud_panel_;
    int panel_w_ = 0;
    bool hud_press_[3] = {false, false, false};  // a press that went to the panel
    bool cursor_on_panel_ = false;                // draw_cursor is drawing on the panel
    std::unique_ptr<Surface8> panel_surf_;
    bool hud_route(const InputEvent& ev);
    std::string last_logged_toast_;
private:
    WorldView view_;
    WorldView::Options opt_;
    SaveStore saves_;
    bool hud_ = false;
    bool quit_ = false;
    bool scroll_[4] = {};
    int mouse_x_ = 200, mouse_y_ = 60;
    int ask_x_ = 0, ask_y_ = 0;  // menu origin of the last render
    int frames_ = 0;
    unsigned picked_ = 0;
    uint16_t press_item_[3] = {0, 0, 0};
    Gump* press_gump_[3] = {nullptr, nullptr, nullptr};  // the button's "down gump"
    int press_lx_[3] = {0, 0, 0}, press_ly_[3] = {0, 0, 0};
    Gump* hover_gump_ = nullptr;
    unsigned restarts_ = 0;
    // Drag and drop.
    enum DragKind { kDragNone, kDragItem, kDragGump, kDragInvalid } drag_ = kDragNone;
    uint16_t drag_item_ = 0;
    uint16_t drag_unequipped_ = 0;  // taken off the paperdoll at drag start (re-equipped if not dropped)
    Gump* drag_from_ = nullptr;  // the gump an item was dragged out of (nullptr = world)
    Gump* drag_gump_ = nullptr;  // a window being moved
    int grab_x_ = 0, grab_y_ = 0;  // cursor minus the item's draw anchor (or the gump's origin)
    Gump* drop_gump_ = nullptr;
    Gump::Drop gump_drop_ = Gump::kDropNo;
    enum DropKind { kDropNone, kDropPlace, kDropThrow, kDropAvatar, kDropMerge } drop_ = kDropNone;
    int drop_pt_[3] = {0, 0, 0};
    uint16_t drop_target_ = 0;
    // Why the current drop (or a drag that could not start) is refused, shown beside the red cross.
    std::string drop_why_, invalid_why_;
    std::string drop_reason() const;
    struct Scripted { int frame; InputEvent ev; std::string debug; };
    void run_debug(const std::string& cmd);
    std::vector<Scripted> script_;
    size_t script_next_ = 0;
};

}  // namespace pg
