// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "data/mapfile.h"

namespace pg {

struct GameData;

// The map patch layer for new content (NOTES.md "The buried cave mouth"): named groups of extra
// FIXED records added when a map loads, after the data fixes; the original files never change.
// Data: data/patches/map_content.json, with a built-in copy in map_patches.cpp for when the file
// is missing (ctest `underworld` checks they agree).
namespace map_patches {

// An original NONFIXED item that had to make room: only unimportant items, each recorded in NOTES.md.
struct Move {
    uint16_t shape = 0;
    uint8_t frame = 0;
    uint16_t x = 0, y = 0;  // where the file has it
    uint8_t z = 0;
    uint16_t to_x = 0, to_y = 0;
    uint8_t to_z = 0;
};

struct Group {
    std::string id;
    unsigned map = 0;
    std::vector<ItemRecord> items;
    std::vector<Move> moves;
    bool road = false;  // from world_roads.json: on only while the stitched layout names it
};

const std::vector<Group>& groups();
const Group* find(const std::string& id);
bool load_text(const std::string& text, std::string& err);  // a bad text changes nothing
bool load_file(const std::string& path, std::string& err);
// Roads of the stitched world (data/patches/world_roads.json, NOTES.md "The docks road"): groups
// added beside the content groups (a reload replaces the roads only). A road is on only while
// set_active_roads names it: World::set_world_mode does, from the layout's "road" seams, so the
// Original maps never see one and neither does a layout without that seam.
bool load_roads_text(const std::string& text, std::string& err);
bool load_roads_file(const std::string& path, std::string& err);
void set_active_roads(const std::vector<std::string>& ids);
bool road_active(const std::string& id);
// Whether a road group with this id is loaded (a road seam without it keeps its teleporters).
bool road_loaded(const std::string& id);

// Whether this copy's map is the one the group was made for (else it is off, with one log line).
bool data_matches(const Group& g);
// Whether a group is in the world now (its On / Off rules live here, one line per group).
bool group_on(const Group& g);
bool moves_on(const Group& g);
// Appends the records of every group that is on for this map (from apply_map_fixes).
void apply(unsigned map, std::vector<ItemRecord>& fixed);
// Applies the moves of the groups that are on to a map's NONFIXED records as read from the file.
void apply_moves(unsigned map, std::vector<ItemRecord>& nonfixed);

// Hidden cells of the stitched world (world_layout.json "hidden", NOTES.md "Which map is real"):
// where two maps draw the same ground, the one whose version is a copy of its neighbour's hides
// its scenery in those 128-unit cells (its own grid: local x / 128, y / 128). Fixed items only,
// and never an egg: nonfixed things and everything scripts use stay. A map with hidden cells gets
// its glob eggs expanded into plain fixed records first, so a glob is cut cell by cell. On only
// while World::set_world_mode sets them (the Original maps never hide anything).
struct HiddenRow { int y = 0, x0 = 0, x1 = 0; bool keep_ground = false; };  // cells x0..x1 of row y (keep_ground: its floor stays)
void set_hidden(const GameData* d, const std::vector<std::pair<unsigned, std::vector<HiddenRow>>>& maps);
bool hides(unsigned map);
bool hidden_cell(unsigned map, int lx, int ly);  // local 128-unit cell
size_t hidden_count(unsigned map);
std::string hidden_signature(unsigned map);  // changes when the map's hidden cells do
// Drops the hidden records of `map` (from apply_map_fixes, last).
void apply_hidden(unsigned map, std::vector<ItemRecord>& fixed);

}  // namespace map_patches
}  // namespace pg
