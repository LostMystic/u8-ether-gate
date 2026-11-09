// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Edit mode (PLAN-WORLD-EDITOR.md section 2): a full-screen world painter over the atlas
// layout. The original maps are drawn as rendered (zoom tiles from world/tile_cache.h: far
// zoom from the box-filter pyramid, near zoom from full renders, magnified up to 4x), one layer
// at a time with the layers above and below ghosted. Painted areas (editor/area_model.h) are
// drawn as coloured cells on the 128-unit grid. Native `--edit`, web `?edit`, and "Editor"
// in the debug menu. Controls: NOTES.md "Edit mode".
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "editor/area_model.h"
#include "editor/editor_storage.h"
#include "gfx/font.h"
#include "platform/input.h"
#include "world/atlas_layout.h"
#include "world/tile_cache.h"

namespace pg {

struct GameData;
struct RgbaCanvas;

// Sets up the default storage: native files in data_dir (areas/, world_layout.json; history in
// ~/.pagan/editor_history); web: /editor kept in IndexedDB, autosaving, with data_dir (/data)
// as the shipped seed.
class EditMode;
void setup_editor_storage(EditMode& m, const std::string& data_dir);

class EditMode {
public:
    // kSegments (the default): grab a segment (a map group or a painted area) and drag it.
    enum class Tool { kSegments, kBrush, kLine, kRect, kFill, kErase, kMarker, kMove };
    // Paint kinds of the palette; kHeight paints only the height of painted cells.
    static constexpr char kHeightOnly = '^';

    explicit EditMode(const GameData& data);
    ~EditMode();

    // Storage (editor/editor_storage.h) and the layout used when it has none yet.
    void set_storage(std::unique_ptr<EditorStorage> s) {
        storage_ = std::move(s);
        loaded_ = false;
        // Messages and conflicts that a network backend finds after save() returned.
        if (storage_) {
            storage_->on_notice = [this](const std::string& m, int ticks) { say(m, ticks); };
            storage_->on_conflict = [this]() { force_next_ = true; };
        }
    }
    EditorStorage* storage() { return storage_.get(); }
    void set_fallback_layout(const AtlasLayout& l) { fallback_ = l; }
    // Areas / layout shipped with the program (web: /data), used when the storage is empty.
    void set_seed_storage(std::unique_ptr<EditorStorage> s) { seed_ = std::move(s); }
    // Loads everything from the storage (waits while it is not ready).
    bool load_all();
    bool loaded() const { return loaded_; }
    // Opened from the game: Back / Esc returns there.
    void set_from_game(bool on) { from_game_ = on; }
    bool take_exit() { bool b = exit_; exit_ = false; return b; }
    // Web: autosave a few seconds after the last change.
    void set_autosave(bool on) { autosave_ = on; }
    // Per frame: tiles are built within this budget (ms); < 0 builds everything (screenshots).
    void set_budget(double ms) { budget_ms_ = ms; }

    // Centres the view on (x, y, z) of `map` (its group's layer becomes the current one).
    void show_map_point(unsigned map, int x, int y, int z, int level);
    void show_world_point(int x, int y, int level);

    void handle(const InputEvent& ev);
    void tick();
    // Draws at 320*S x 200*S (S output pixels per game pixel, at least 2).
    void render(std::vector<uint32_t>& out, int& w, int& h, int S);

    // --- State (public for the tests and the toolbar)
    EditDoc doc;
    Tool tool = Tool::kSegments;
    char kind = cell::kFloor;
    BrushShape brush = BrushShape::kSquare;
    int brush_size = 1;
    int layer = 0;   // shown / painted layer
    int height = 0;  // paint height level
    std::string theme = "cave";
    std::string selected;    // area id
    int selected_group = -1;
    bool show_grid = true;
    bool panel = false;
    bool layers_panel = false;    // the Layers panel (Y): every layer, its groups and areas
    // Segments: dragging moves only the grabbed segment unless this is on (then its seam
    // partners come along); the snap grid for segments (512, 256, 128, 64; 0 = off); the
    // spots views: 0 the layout as edited, kPagan / kBaseline every segment where that restore
    // point has it (view only).
    bool move_joined = false;
    int snap_grid = 512;
    int show_spots = 0;
    // Further segments selected with Shift+click or a box (Segments tool; Layers list); the
    // resets apply to the whole selection (selected_group + these).
    std::set<int> multi;
    std::set<int> hidden_layers;  // layers switched off in the Layers panel (not drawn, not ghosted)
    bool help = false;
    std::string message;
    int message_ticks = 0;
    int level() const { return level_; }
    size_t pending_tiles() const { return pending_; }
    bool dirty() const;
    static const std::vector<std::string>& themes();

    // Actions (buttons, keys and tests).
    void save(bool force = false);
    void export_bundle();   // web: download; native: ~/.pagan/editor_bundle.json
    void import_bundle();   // web: file picker; native: the same file
    bool import_text(const std::string& text);
    void dress();           // the map dresser on the selected area (if built in)
    void reroll();
    bool dresser_available() const;
    // data/tilekit.json (the dresser's tile kit); Dress is off without it.
    void set_tilekit_path(const std::string& p) { kit_path_ = p; }
    bool dressed() const { return dressed_ != nullptr; }
    // While dressed: the dresser's validation, POI choices and warnings (shown over the map),
    // and its "pagan-dressed" JSON.
    std::vector<std::string> dress_report() const;
    std::string dressed_json() const;
    size_t dress_items_() const;
    bool dress_ok_() const;
    void say(const std::string& m, int ticks = 120) { message = m; message_ticks = ticks; }
    void select_area(const std::string& id);  // also sets the layer and theme to the area's
    // Map groups (and painted areas): select one (its layer becomes the current one, the view
    // centres on it), nudge it, change its layer. A group joined to others by walkable seams
    // moves and changes layer together with them (the seams stay exact). Each call is one undo step.
    void select_group(int gi, bool centre = true);
    bool nudge_selected(int dx, int dy);
    bool selected_layer_step(int d);
    std::vector<int> seam_cluster(int gi) const;  // gi and every group joined to it by seams
    std::string layer_name(int L) const;          // the layout's "layers" text ("" if none)
    std::vector<int> layers_in_use() const;       // every layer with a group or an area, sorted
    // Overlaps of the current layout (world_layout.h check_world_layout); shown after a move.
    std::vector<std::string> layout_problems() const;
    bool replaced_stale_layout() const { return replaced_stale_; }
    // The two restore points: kPagan, the original Ultima 8 placement (data/atlas_layout.json;
    // x / y only, the atlas has no layers) and kBaseline, the layout frozen on 2026-11-06
    // (data/world_layout.baseline.json: places, layers, z, seams and links).
    static constexpr int kPagan = 1, kBaseline = 2;
    void set_reference_layout(int which, const std::string& json_text);
    bool has_reference(int which) const;
    static const char* reference_name(int which) { return which == kPagan ? "Pagan" : "Baseline"; }
    bool reset_group(int gi, int which);    // one undo step; seams re-checked
    bool reset_selection(int which);        // the selected segments, one undo step
    bool reset_all(int which);              // one undo step (Baseline: its seams and links too)
    std::vector<int> selection() const;     // selected_group + multi
    // Box select: every segment of the current layer touching the canvas rectangle.
    int select_box(double u0, double v0, double u1, double v1, bool add);
    // Where a segment is shown: its layout place, or a restore point's in a spots view.
    void shown_place(int gi, int& dx, int& dy, int& lay) const;
    // The seams in the layout and whether each is walkable now (an exact offset) or broken.
    struct SeamInfo { unsigned a, b; int dx, dy, dz; bool walk; };
    std::vector<SeamInfo> seams() const;
    // Groups on one layer whose segments overlap (the validator's rule): marked in red.
    const std::set<int>& overlapping_groups();
    // A segment drag in code (tests): grab group gi, move it by (dx, dy) world units with the
    // snapping of a mouse drag, drop it. Returns the message shown.
    std::string drag_group_by(int gi, double dx, double dy);
    const TileCache& tiles() const { return cache_; }
    int hover_group() const { return hover_group_; }

    // Coordinates (tests): canvas pixel of a world point at local z; the world point under a
    // canvas pixel at local z.
    void world_to_canvas(double x, double y, double z, double& u, double& v) const;
    void canvas_to_world(double u, double v, double z, double& x, double& y) const;
    int canvas_w() const { return W_; }
    int canvas_h() const { return H_; }
    int scale() const { return S_; }

private:
    struct Button {
        int x = 0, y = 0, w = 0, h = 0;
        std::string label, tip;
        std::function<void()> act;
        bool on = false, enabled = true;
        uint32_t swatch = 0;
    };
    struct Dialog {
        std::string title;
        std::vector<std::string> labels, values;
        int focus = 0;
        std::string ok = "OK", extra;  // extra: a third button (e.g. "Delete")
        std::function<void(const std::vector<std::string>&)> on_ok;
        std::function<void()> on_extra;
        bool confirm = false;  // no fields: a question
    };
    struct Placed { unsigned map; int group; int ox, oy; };

    void layout_ui();
    void draw_ui(RgbaCanvas& c);
    void draw_areas(RgbaCanvas& c, uint32_t* px);
    void draw_grid(RgbaCanvas& c);
    void draw_preview(RgbaCanvas& c);
    void draw_dialog(RgbaCanvas& c);
    void draw_dressed(RgbaCanvas& c);
    void draw_dress_report(RgbaCanvas& c);
    void draw_help(RgbaCanvas& c);
    void draw_maps(std::vector<uint32_t>& out);
    void draw_group_outlines(RgbaCanvas& c);
    void draw_layer_banner(RgbaCanvas& c);
    void after_group_change(const std::string& what);
    std::vector<int> moving_set(int gi) const;
    void start_group_drag(int gi);
    void group_drag_update();
    void finish_group_drag();
    std::string refresh_seam_modes();  // inside an open change: exact seams walk, others teleport
    bool snap_target(int lead, const std::vector<int>& moving, double cx, double cy, int& tx, int& ty, std::string& why) const;
    void draw_segment_grid(RgbaCanvas& c);
    bool ui_hit(int u, int v) const;
    void mouse_down(int u, int v, int button, unsigned mods);
    void mouse_move(int u, int v);
    void mouse_up(int u, int v, int button);
    void key(const InputEvent& ev);
    void pad(const InputEvent& ev);
    void open_dialog(Dialog d) { dialog_ = std::make_unique<Dialog>(std::move(d)); }
    void dialog_key(const InputEvent& ev);
    void set_level(int lv, double u, double v);
    void refresh_exclusions();
    std::vector<PlacedMap> placed(int lay) const;
    // The area painted into: the selected one (if on this layer), else a new one at the cell.
    EditArea* target_area(double u, double v, bool create, int* cx, int* cy);
    bool cell_under(const EditArea& a, double u, double v, int& cx, int& cy) const;
    int area_under(double u, double v) const;  // index of an area with a cell there (this layer)
    int group_under(double u, double v) const;
    PaintValue value(bool erase) const;
    void stroke_to(int cx, int cy);
    void marker_at(double u, double v);
    void back();
    void after_storage_change();
    void autosave_tick();
    EditArea* sel();
    bool compute_dirty() const;
    void publish_state();  // web: Module.pgEditState (for test scripts)

    const GameData& data_;
    std::vector<ShapeFont> fonts_;
    TileCache cache_;
    std::vector<MapFootprint> fps_;
    std::unique_ptr<EditorStorage> storage_, seed_;
    AtlasLayout fallback_;
    bool loaded_ = false, from_game_ = false, exit_ = false, autosave_ = false;
    double budget_ms_ = 10;
    size_t pending_ = 0;

    int S_ = 2, W_ = 640, H_ = 400, ui_ = 1;
    double cx_ = 0, cy_ = 0;  // native (flattened) screen point at the canvas centre
    int level_ = 3;           // 2^level native pixels per canvas pixel; < 0 magnifies
    double mouse_u_ = 320, mouse_v_ = 200;
    bool held_[4] = {};
    bool fast_ = false;
    std::vector<Button> buttons_;
    int hover_button_ = -1;
    int panel_x_ = 0, top_h_ = 0, bottom_y_ = 0;
    std::unique_ptr<Dialog> dialog_;

    // Mouse drags.
    enum class Drag { kNone, kPan, kStroke, kShape, kMoveArea, kMoveGroup, kBox, kUi } drag_ = Drag::kNone;
    int drag_button_ = 0;
    double down_u_ = 0, down_v_ = 0, last_u_ = 0, last_v_ = 0;
    int stroke_cx_ = 0, stroke_cy_ = 0, start_cx_ = 0, start_cy_ = 0;
    bool shift_ = false;
    double move_acc_[2] = {0, 0};
    int moved_[2] = {0, 0};

    // Controller: a virtual pointer.
    double pad_axis_[6] = {};
    bool pad_a_ = false;

    // Saving.
    std::map<std::string, std::string> saved_text_;  // id -> JSON as last saved / loaded
    std::map<std::string, int> saved_rev_;
    std::string saved_layout_;
    int layout_rev_ = 0;
    bool force_next_ = false;
    bool replaced_stale_ = false;
    int layers_scroll_ = 0;
    int hover_group_ = -1;
    std::vector<int> drag_set_;
    int drag_start_[2] = {0, 0};
    std::string snap_why_;
    std::string ref_[3];
    double box_[4] = {0, 0, 0, 0};
    bool shift_box_ = false;  // the box adds to the selection
    std::set<int> overlap_;
    int overlap_stamp_ = -1;  // the stored layout was older than the shipped one
    bool autosaving_ = false;  // save() called by the autosave (server reminders are rate-limited)
    int last_changes_ = 0, changed_ticks_ = 0;
    int save_gen_ = 0;
    mutable int dirty_stamp_ = -1, dirty_gen_ = -1;
    mutable bool dirty_cache_ = false;
    unsigned ticks_ = 0;

    // The dressed result shown in place (when the dresser is built in).
    struct Dressed;
    std::unique_ptr<Dressed> dressed_;
    std::string kit_path_;
};

}  // namespace pg
