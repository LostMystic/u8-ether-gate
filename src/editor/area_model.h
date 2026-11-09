// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The edit mode's paint model: painted areas (docs/area_format.md, "pagan-area" v1), the
// brushes that change them, and the world layout they are placed in (the atlas layout's map
// groups with z offsets and layers, plus the painted areas). No SDL, no game data: the tests
// use it directly.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "world/atlas_layout.h"

namespace pg {

// Cell codes (docs/area_format.md 1, "Cell codes").
namespace cell {
constexpr char kVoid = ' ', kFloor = '.', kWall = '#', kDoor = '+', kPassage = '/', kWater = '~', kStairs = '=', kChasm = 'x';
bool valid(char c);
const char* name(char c);  // "floor", "wall" ...
}  // namespace cell

struct AreaCell {
    char kind = cell::kFloor;
    uint8_t height = 0;   // level (0..35), world z = base_z + height * level_z
    std::string theme;    // "" = the area's theme
    bool operator==(const AreaCell& o) const { return kind == o.kind && height == o.height && theme == o.theme; }
    bool operator!=(const AreaCell& o) const { return !(*this == o); }
};

struct AreaPoi {
    std::string id;
    int x = 0, y = 0;  // cell
    std::string note;
    std::vector<std::string> tags;
    std::string prefab;  // optional (the dresser's prefab id)
    int radius = 2;
    bool operator==(const AreaPoi& o) const {
        return id == o.id && x == o.x && y == o.y && note == o.note && tags == o.tags && prefab == o.prefab && radius == o.radius;
    }
};

// One painted area. Cells are sparse and area-local (cell (cx, cy) covers world x in
// [origin.x + cx*cell, ... + cell), likewise y); painting may go to negative cells, which
// normalize() (and to_json) shift back to 0 by moving the origin, so the world position of
// every cell stays the same.
struct EditArea {
    std::string id;        // storage key: [a-z0-9_-]
    std::string name;
    std::string theme = "cave";
    uint32_t seed = 1;
    int cell = 128;
    int base_z = 0, level_z = 8;
    int origin[3] = {0, 0, 0};  // world units (stitched / atlas world space)
    int layer = 0;              // editor level (0 = the surface, -1 below, +1 above ...)
    std::map<std::pair<int, int>, AreaCell> cells;  // key (cy, cx): row-major order
    std::vector<AreaPoi> pois;
    int revision = 0;  // storage version this copy was loaded from / saved as

    const AreaCell* at(int cx, int cy) const;
    char kind(int cx, int cy) const { const AreaCell* c = at(cx, cy); return c ? c->kind : cell::kVoid; }
    void set(int cx, int cy, const AreaCell& c);  // a void kind erases the cell
    void erase(int cx, int cy) { cells.erase({cy, cx}); }
    // Inclusive bounds of the painted cells (and POIs); false when empty.
    bool bounds(int& x0, int& y0, int& x1, int& y1) const;
    // Shifts cells / POIs so the smallest painted cell is (0, 0), moving the origin to match.
    void normalize();
    // World point (x, y, z) of the near corner of cell (cx, cy) at its height.
    void cell_world(int cx, int cy, int& x, int& y, int& z) const;
    // The cell under world (x, y) (floor division).
    void world_cell(int wx, int wy, int& cx, int& cy) const;
    int poi_index(int cx, int cy) const;  // -1 if none
    std::string next_poi_id() const;

    // docs/area_format.md v1 (plus the editor's own optional fields id, layer, revision).
    std::string to_json() const;
    bool from_json(const std::string& text, std::string& err);
    bool operator==(const EditArea& o) const;
};

// --- Brushes ---------------------------------------------------------------------------
enum class BrushShape { kSquare, kRound };

// What a stroke writes: a kind (or keep), a height (or keep), a theme (or keep). Erase is
// kind = void. Height-only painting never creates cells.
struct PaintValue {
    char kind = cell::kFloor;  // 0 = keep the kind (height / theme only)
    int height = -1;           // -1 = keep (new cells get 0)
    std::optional<std::string> theme;  // nullopt = keep ("" = the area's theme)
};

// The cells a brush of `size` (1 = one cell; diameter) covers around (cx, cy).
std::vector<std::pair<int, int>> brush_cells(BrushShape shape, int size, int cx, int cy);
// The cells of a line from (x0, y0) to (x1, y1) (Bresenham), stamped with the brush.
std::vector<std::pair<int, int>> line_cells(BrushShape shape, int size, int x0, int y0, int x1, int y1);
// A rectangle (corners inclusive); outline only if `outline`.
std::vector<std::pair<int, int>> rect_cells(int x0, int y0, int x1, int y1, bool outline);
// Applies a value to cells; returns how many changed.
int paint_cells(EditArea& a, const std::vector<std::pair<int, int>>& cells, const PaintValue& v);
// Flood fill (4-connected) from (cx, cy) over cells with the same kind and height as the start
// (void too), bounded by the painted bounds grown by `margin` cells. Returns cells changed.
int flood_fill(EditArea& a, int cx, int cy, const PaintValue& v, int margin = 1);

// --- The world layout ------------------------------------------------------------------
// A painted area's entry in the layout (the area file holds the cells; this places it).
struct LayoutArea {
    std::string id, name;
    int offset[3] = {0, 0, 0};
    int layer = 0;
};

// engine/data/world_layout.json: the atlas layout's groups (with "offset": [dx, dy, dz] and
// "layer"), the painted areas, and fields kept for the stitched world (seams, teleports).
// AtlasLayout::from_json reads it too (it ignores the extra fields).
struct WorldLayout {
    AtlasLayout atlas;
    std::vector<LayoutArea> areas;
    std::string extra;  // other top-level fields, kept verbatim (JSON object text) for section 1
    int revision = 0;
    std::string to_json() const;
    bool from_json(const std::string& text, std::string& err);
};

// --- The document: areas + layout + undo -----------------------------------------------
// Everything the edit mode changes goes through an EditDoc so that it can be undone. A
// change is recorded as before/after copies of the areas (and layout groups) it touched.
class EditDoc {
public:
    WorldLayout layout;
    std::vector<EditArea> areas;

    int find(const std::string& id) const;  // index or -1
    std::string unique_id(const std::string& name) const;

    // Start a change of area `id` (or of layout group `group`) and commit it. Brush strokes
    // begin on mouse down and commit on mouse up, so a stroke undoes in one step.
    void begin(const std::string& label);
    void touch_area(const std::string& id);  // record its "before" (once per change)
    void touch_group(int group);
    void touch_layout_extra();  // the layout's other fields (seams, links ...): a seam broken / re-joined
    void commit();   // records "after"; drops the change if nothing differs
    void cancel();   // restores "before"
    bool in_change() const { return open_; }

    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::string undo_label() const { return undo_.empty() ? "" : undo_.back().label; }
    std::string redo_label() const { return redo_.empty() ? "" : redo_.back().label; }
    size_t undo_depth() const { return undo_.size(); }
    static constexpr size_t kMaxUndo = 200;
    int changes = 0;  // bumped on every commit / undo / redo (autosave and dirty tracking)

    // Common operations (each one a full undoable change).
    EditArea& create_area(const std::string& name, const int origin[3], int layer, const std::string& theme);
    bool delete_area(const std::string& id);
    bool rename_area(const std::string& id, const std::string& name);
    bool move_area(const std::string& id, int dx, int dy, int dz);
    bool set_area_layer(const std::string& id, int layer);
    bool set_area_theme(const std::string& id, const std::string& theme);
    bool set_area_seed(const std::string& id, uint32_t seed);
    bool move_group(int group, int dx, int dy, int dz);
    bool set_group_layer(int group, int layer);
    bool add_poi(const std::string& id, int cx, int cy, const std::string& note, const std::vector<std::string>& tags);
    bool edit_poi(const std::string& id, int index, const std::string& note, const std::vector<std::string>& tags);
    bool delete_poi(const std::string& id, int index);

    // The layout's area list from the areas (offsets, layers, names), then the layout JSON.
    void sync_layout();
    // One file with everything (Download / Upload): {"format": "pagan-editor-bundle", ...}.
    std::string bundle_json();
    bool load_bundle(const std::string& text, std::string& err);  // replaces areas + layout; clears undo

private:
    struct Snap {
        std::string id;  // area
        int group = -1;  // or layout group
        std::optional<EditArea> before, after;
        std::optional<AtlasGroup> gbefore, gafter;
        std::optional<std::string> xbefore, xafter;  // group == -2: layout.extra
    };
    struct Change {
        std::string label;
        std::vector<Snap> snaps;
    };
    void apply(const Change& c, bool forward);
    void place_area(const std::string& id, const std::optional<EditArea>& a);
public:
    void clear_history() { undo_.clear(); redo_.clear(); open_ = false; cur_ = Change{}; }
private:
    Change cur_;
    bool open_ = false;
    std::vector<Change> undo_, redo_;
};

// Base-36 height digit (0-9, a-z).
char height_digit(int h);
int height_value(char c);

}  // namespace pg
