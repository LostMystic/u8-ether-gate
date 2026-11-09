// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gfx/font.h"
#include "platform/input.h"
#include "world/current_map.h"
#include "world/tile_cache.h"
#include "world/world_view.h"

namespace pg {

struct GameData;
class Surface8;
class AtlasView;

// `pagan --view-map N --at x,y,z`: a free camera over one map, with no game logic.
// Arrows scroll (Shift = faster), PgUp/PgDn change height, N / P next / previous map,
// R roof hiding, E editor items (eggs), T test pattern, Home back to the start point.
// Zoom: wheel or + / - (1x, 1/2, 1/4 ... 1/128 of the game scale). 1x is the normal 320x200
// render; zoomed views are drawn from cached tiles (world/tile_cache.h) at 640x400. M opens
// the world atlas (app/atlas_view.h).
class MapViewer {
public:
    static constexpr int kMaxZoom = 7;

    explicit MapViewer(const GameData& data);
    ~MapViewer();

    // at: the point of interest (like an actor's feet); the camera looks at at + (0,0,20).
    // nullptr picks a default for the map (the Avatar, a teleport destination, or the
    // middle of the map's items).
    void open_map(unsigned map, const int* at = nullptr);
    void set_test_pattern(bool on) { test_pattern_ = on; }
    void set_roof_hiding(bool on) { opt_.roof_hiding = on; }
    void set_show_editor(bool on) { opt_.show_editor = on; }
    void set_log_items(bool on) { opt_.log_items = on; }
    void set_mouse(int x, int y) { mouse_x_ = x; mouse_y_ = y; }

    void handle(const InputEvent& ev);
    void tick();  // one game frame (30 per second)
    void render(Surface8& surf);

    // Zoom step: 0 = 1x (render() draws the 320x200 view), n = 1/2^n (render_rgba()).
    void set_zoom(int z) { change_zoom(z, 160, 100); }
    int zoom() const { return zoom_; }
    // True when the frame comes from render_rgba() (zoomed or atlas) instead of render().
    bool wants_rgba() const { return zoom_ > 0 || atlas_; }
    void render_rgba(std::vector<uint32_t>& out, int& w, int& h);
    // Time per frame for building tiles; < 0 builds everything at once (screenshots).
    void set_render_budget(double ms) { budget_ms_ = ms; }

    // Atlas: the layout file (loaded on first use, saved there by S).
    void set_atlas_layout_path(const std::string& p) { atlas_path_ = p; }
    void open_atlas(bool show_all);
    AtlasView& atlas();
    bool in_atlas() const { return atlas_; }

    unsigned map() const { return map_.map_number(); }
    CurrentMap& current_map() { return map_; }  // --view-area adds a dressed area
    const int* focus() const { return focus_; }

private:
    void default_focus(unsigned map, int out[3]) const;
    void draw_cursor(Surface8& surf);
    void draw_test_pattern(Surface8& surf);
    void change_zoom(int z, int mx, int my);
    void cam_screen(const int focus[3], long& sx, long& sy) const;

    const GameData& data_;
    CurrentMap map_;
    WorldView view_;
    WorldView::Options opt_;
    std::vector<unsigned> maps_;
    int focus_[3] = {0, 0, 0};
    int start_[3] = {0, 0, 0};
    int prev_cam_[3] = {0, 0, 0};
    bool held_[4] = {};  // up, down, left, right
    bool fast_ = false;
    bool test_pattern_ = false;
    int mouse_x_ = 200, mouse_y_ = 60;
    unsigned picked_ = 0;
    std::vector<ShapeFont> fonts_;

    // Zoomed view: centre_ is the native screen point (map screen space) at the middle.
    int zoom_ = 0;
    long centre_[2] = {0, 0};
    int saved_focus_[3] = {0, 0, 0};
    long saved_cam_[2] = {0, 0};
    double budget_ms_ = 10;
    size_t pending_ = 0;
    std::unique_ptr<TileCache> tiles_;
    std::unique_ptr<AtlasView> atlas_view_;
    std::string atlas_path_;
    bool atlas_ = false;
};

}  // namespace pg
