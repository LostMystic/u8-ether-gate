// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "data/mapfile.h"

namespace pg {

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
};

const std::vector<Group>& groups();
const Group* find(const std::string& id);
bool load_text(const std::string& text, std::string& err);  // a bad text changes nothing
bool load_file(const std::string& path, std::string& err);

// Whether this copy's map is the one the group was made for (else it is off, with one log line).
bool data_matches(const Group& g);
// Whether a group is in the world now (its On / Off rules live here, one line per group).
bool group_on(const Group& g);
bool moves_on(const Group& g);
// Appends the records of every group that is on for this map (from apply_map_fixes).
void apply(unsigned map, std::vector<ItemRecord>& fixed);
// Applies the moves of the groups that are on to a map's NONFIXED records as read from the file.
void apply_moves(unsigned map, std::vector<ItemRecord>& nonfixed);

}  // namespace map_patches
}  // namespace pg
