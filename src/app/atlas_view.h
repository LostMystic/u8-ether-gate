// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "app/rgba_draw.h"
#include "gfx/font.h"
#include "platform/input.h"
#include "world/atlas_layout.h"
#include "world/tile_cache.h"

namespace pg {

struct GameData;

// Atlas mode: every populated map on one canvas, placed by an AtlasLayout (groups with
// offsets, engine/data/atlas_layout.json). Drawn from TileCache tiles at 640 x 400 (two
// output pixels per game pixel). Wheel / + / - zoom about the pointer, arrows or a plain
// drag pan, Ctrl+drag moves a group, a click jumps into the map viewer there, S saves the
// layout, Home shows everything, L labels, G regenerates the layout from the data.
class AtlasView {
public:
    static constexpr int kW = 640, kH = 400;

    AtlasView(const GameData& data, std::vector<ShapeFont>& fonts);

    // Layout: from the file if it loads, else derived from the data. path is also where S
    // saves (native).
    void load_layout(const std::string& path);
    const AtlasLayout& layout() const { return layout_; }
    const std::string& layout_status() const { return layout_status_; }

    // Centres the view on world point (x, y, z) of `map` (or everything if the map is not in
    // the layout), at `level` (one output pixel = 2^level native pixels).
    void show(unsigned map, int x, int y, int z, int level);
    void show_all();

    void handle(const InputEvent& ev);
    void set_mouse(int x, int y) { mouse_x_ = x; mouse_y_ = y; }
    void tick();
    // Draws into out (kW x kH). budget_ms < 0: build every missing tile now.
    void render(std::vector<uint32_t>& out, double budget_ms);

    // Set when a click asked to open a map: map and world point.
    bool take_jump(unsigned& map, int at[3]);
    bool wants_viewer() { bool b = back_; back_ = false; return b; }

    // Exports the whole atlas at one level to a PNG (labels and outlines unless plain).
    bool export_png(const std::string& path, int level, bool plain);

    // Milestone 6, the atlas opened from the game: a marker for the Avatar, and clicks that
    // only pan (take_jump is never set).
    void set_marker(unsigned map, int x, int y, int z) { marker_ = true; mk_map_ = map; mk_[0] = x; mk_[1] = y; mk_[2] = z; }
    void set_from_game(bool on) { from_game_ = on; }
    // The stitched world's layout (world_layout.json) has layers: one layer is shown at a time
    // (PgUp / PgDn), the outlines of the layers just above and below ghosted. An atlas-only
    // layout has one layer and draws as before.
    bool layered() const { return layered_; }
    int layer() const { return layer_; }
    void set_layer(int l) { layer_ = l; }
    bool on_layer(const AtlasGroup& g) const { return !layered_ || g.layer == layer_; }
    // Output pixel (640 x 400) of the marker; false if its map is not placed.
    bool marker_pixel(int& u, int& v) const;
    // Zoom by one step about the view centre (+1 = out).
    void zoom_step(int d) { set_level(level_ + d, kW / 2, kH / 2); }
    int level() const { return level_; }
    size_t pending() const { return pending_; }
    TileCache& cache() { return cache_; }

private:
    struct Placed { unsigned map; int group; int ox, oy; };
    std::vector<PlacedMap> placed() const;
    void refresh_exclusions();
    // Atlas native point under output pixel (u, v).
    void to_native(int u, int v, long& nx, long& ny) const;
    // The map and world point under an atlas native point; false if none.
    bool hit(long nx, long ny, unsigned& map, int& group, int at[3]) const;
    void set_level(int lv, int u, int v);
    void draw_overlay(RgbaCanvas& c, long vx0, long vy0, int level, bool labels) const;
    void group_screen_box(const AtlasGroup& g, int& l, int& t, int& r, int& b) const;

    const GameData& data_;
    std::vector<ShapeFont>& fonts_;
    TileCache cache_;
    AtlasLayout layout_;
    std::vector<MapFootprint> fps_;
    std::vector<std::pair<unsigned, std::bitset<64 * 64>>> excl_;
    std::string path_, layout_status_;
    long cx_ = 0, cy_ = 0;  // native atlas point at the view centre
    int level_ = 5;
    bool held_[4] = {};
    bool fast_ = false;
    bool labels_ = true;
    int mouse_x_ = 160, mouse_y_ = 100;
    // Mouse drag: pan, or (Ctrl) move a group.
    bool down_ = false, moved_ = false;
    int drag_group_ = -1;
    int down_x_ = 0, down_y_ = 0, last_x_ = 0, last_y_ = 0;
    long drag_acc_x_ = 0, drag_acc_y_ = 0;
    int hover_group_ = -1;
    bool dirty_ = false;
    bool jump_ = false, back_ = false;
    unsigned jump_map_ = 0;
    int jump_at_[3] = {0, 0, 0};
    size_t pending_ = 0;
    std::string message_;
    int message_ticks_ = 0;
    bool marker_ = false, from_game_ = false;
    unsigned mk_map_ = 0;
    int mk_[3] = {0, 0, 0};
    unsigned blink_ = 0;
    bool layered_ = false, world_file_ = false;
    int layer_ = 0;
};

}  // namespace pg
