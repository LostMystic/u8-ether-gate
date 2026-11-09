// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "editor/tilekit.h"

namespace pg {

struct GameData;

// One item of the original maps as the miner sees it (globs expanded, contents dropped).
struct MinedItem {
    unsigned map = 0;
    int x = 0, y = 0, z = 0;
    int xd = 0, yd = 0, zd = 0;
    uint16_t shape = 0, frame = 0, flags = 0, quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
    bool nonfixed = false;
    int x0() const { return x - xd + 1; }
    int y0() const { return y - yd + 1; }
};

// The maps the miner learns from (places with content; the unused test maps are skipped).
std::vector<unsigned> tilekit_maps();
// Every top-level item of a map: FIXED (with the data fixes and expanded globs) + NONFIXED.
std::vector<MinedItem> gather_map_items(const GameData& data, unsigned map);

struct MineOptions {
    bool verbose = false;
};

// Learns the tile kit from all maps (roles, floor materials, wall styles, themes, prefabs,
// adjacency). `report` gets a human-readable summary with counts.
bool mine_tilekit(const GameData& data, TileKit& kit, std::string& report, const MineOptions& opt = {});

}  // namespace pg
