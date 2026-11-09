// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "data/mapfile.h"

namespace pg {

struct GameData;
class CurrentMap;

// Renders loose item records with the real world renderer (WorldView + ItemSorter) into one
// RGBA image that shows all of them. No SDL. Used by the map dresser's previews
// (pagan_dress --png) and the tile-kit miner's reports.
struct AreaImage {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;  // 0xAARRGGBB
};

struct AreaRenderOptions {
    int margin = 48;          // pixels around the projected bounds
    bool show_editor = false; // draw eggs and other editor markers
    int max_w = 4096, max_h = 4096;
};

// Places the items (area-local, all coordinates >= 0) at (ox, oy, oz) in an otherwise empty
// map, renders and returns the picture. ox/oy default to a spot well inside a 32768 map.
AreaImage render_items(const GameData& data, const std::vector<ItemRecord>& items,
                       const AreaRenderOptions& opt = {}, int ox = 4096, int oy = 4096, int oz = 0);

// Loads items into a CurrentMap (viewer) at an offset and makes their chunks fast; returns
// the ids. Used by render_items and the map viewer's --view-area.
std::vector<uint16_t> place_items(CurrentMap& map, const std::vector<ItemRecord>& items, int ox, int oy, int oz);

// Renders the rectangle [x0, x1) x [y0, y1) of a real map (fixed + nonfixed, globs expanded).
AreaImage render_map_region(const GameData& data, unsigned map, int x0, int y0, int x1, int y1,
                            const AreaRenderOptions& opt = {});

}  // namespace pg
