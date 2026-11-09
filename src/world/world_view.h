// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <functional>
#include <cstdint>
#include <vector>

#include "world/item_sorter.h"

namespace pg {

struct GameData;
class Item;
class CurrentMap;
class Surface8;

// Draws the isometric world (specs/presentation/rendering.md sections 1-5) into a surface.
// This is what the later GameMapGump will wrap.
class WorldView {
public:
    explicit WorldView(const GameData& data) : data_(data) {}

    struct Options {
        bool roof_hiding = true;   // section 4
        bool show_editor = false;  // draw EDITOR shapes (eggs) as a debug aid
        bool show_invisible = false;
        bool log_items = false;    // print every painted item (debugging)
        uint16_t hide_id = 0;      // not drawn (an item being dragged, gumps.md 1.6)
        // Called right after an item is painted, with its screen anchor (weapon overlays).
        std::function<void(Surface8&, const SortItem&, int, int)> after_paint;
        // Milestone 6: an explicit screen position of the camera (camSx, camSy) instead of
        // the one projected from `camera` (the smooth camera), and displayed positions for
        // actors (ids below 256) that differ from their world positions (interpolation).
        bool use_screen_camera = false;
        int camera_sx = 0, camera_sy = 0;
        std::function<bool(uint16_t id, int& x, int& y, int& z)> display_pos;
        // A colour table for an item (companions' recolours; nullptr = none). Asked for actors only.
        std::function<const uint8_t*(uint16_t id)> remap;
        // What an actor is drawn as instead of its own shape and frame (the new creatures'
        // effect bodies, game/creatures.h): asked for actors only; false = drawn as it is.
        // The footprint and sorting stay the actor's own.
        struct Look {
            uint16_t shape = 0, frame = 0;
            bool hide = false;          // nothing drawn (and nothing to pick)
            bool ghost = false;         // the 50% ghost blend
            const uint8_t* remap = nullptr;
        };
        std::function<bool(uint16_t id, Look& look)> appearance;
        // The stitched world (world/world_space.h): scenery of the neighbouring maps across a
        // seam, already in this map's coordinates, sorted and drawn with the map's own items
        // (never picked: their id is 0). nullptr / empty = only the map.
        const std::vector<const Item*>* extra = nullptr;
    };

    // camera: the camera point (cx, cy, cz). roof_probe: the point used for roof detection
    // (normally the same, without any earthquake offset).
    void render(CurrentMap& map, Surface8& surf, const int camera[3], const int roof_probe[3], const Options& opt);

    // Object id under screen pixel (px, py) from the last render; 0 if none.
    unsigned pick(int px, int py);

    int roof_limit() const { return roof_limit_; }
    size_t drawn_items() const { return sorter_.painted_count(); }
    size_t sorted_items() const { return sorter_.items().size(); }

private:
    const GameData& data_;
    ItemSorter sorter_;
    int roof_limit_ = 65536;
    int centre_x_ = 160, centre_y_ = 100;
    std::vector<uint8_t> ghost_;
};

}  // namespace pg
