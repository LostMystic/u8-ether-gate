// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

struct GameData;

// The world atlas: every populated map laid out on one big canvas. Each group (one map, or
// several maps that share one coordinate space) has an offset in world units that is added
// to the x and y of all of its items. The layout lives in a human-editable JSON file
// (engine/data/atlas_layout.json); generate_atlas_layout() derives one from the game data.

// Where a map has content: a 64 x 64 chunk occupancy mask (fixed + nonfixed + NPCs).
struct MapFootprint {
    unsigned map = 0;
    std::bitset<64 * 64> occ;
    std::vector<uint16_t> count = std::vector<uint16_t>(64 * 64, 0);  // records per chunk
    int cx0 = 64, cy0 = 64, cx1 = -1, cy1 = -1;  // inclusive chunk bounds
    int chunks = 0;
    int median_z = 0;
    bool has(int cx, int cy) const { return cx >= 0 && cy >= 0 && cx < 64 && cy < 64 && occ[size_t(cy) * 64 + cx]; }
};

// A teleporter egg on one map and the destination egg it leads to on another.
struct TeleportLink {
    unsigned from_map = 0, to_map = 0, id = 0;
    int fx = 0, fy = 0, fz = 0;  // the teleporter
    int tx = 0, ty = 0, tz = 0;  // the destination marker
};

struct AtlasGroup {
    std::string name;            // identifier, e.g. "map40"
    std::string label;           // drawn in atlas mode
    std::vector<unsigned> maps;  // member maps (drawn in this order)
    int dx = 0, dy = 0;          // offset in world units
    int dz = 0;                  // vertical offset (world z) for groups stacked above / below others
    int layer = 0;               // level in the world editor: 0 surface, -1 below, +1 above ...
    std::string derived;         // how the offset was found
    bool from_data = false;      // true: from teleport links; false: placed by a rule / guess
    std::string extra;           // other fields of the group (JSON object text), kept on save
};

struct AtlasLayout {
    std::vector<AtlasGroup> groups;
    std::string comment;
    // A stitched world layout's walkable seams (pairs of maps; from its "seams", broken ones
    // left out). When there are any, only seam partners compete in atlas_exclusions (their
    // duplicated border strip is drawn once); every other overlap draws both maps whole.
    std::vector<std::pair<unsigned, unsigned>> seam_pairs;

    // Index of the group holding `map`, or -1.
    int group_of(unsigned map) const;
    std::string to_json() const;
    bool from_json(const std::string& text, std::string& err);
    bool load(const std::string& path, std::string& err);
    bool save(const std::string& path) const;
};

MapFootprint compute_footprint(const GameData& data, unsigned map);
std::vector<MapFootprint> compute_footprints(const GameData& data);  // all populated maps
std::vector<TeleportLink> teleport_links(const GameData& data);
// Derives a layout: teleport links between maps give relative offsets (averaged over both
// directions); maps that would overlap an already placed map (interiors, other levels,
// variants of the same area) are moved to the nearest free spot beside their entrance;
// maps with no link to the rest are packed around it.
AtlasLayout generate_atlas_layout(const GameData& data);

// Where placed maps overlap, each atlas chunk shows only the map with the most records
// there (ties: the earlier group). Returns, per map, the chunks it must not draw. Only groups
// on the same layer compete (a layout without layers: all of them); in a layout with seams
// only seam partners do (nothing else is ever hidden).
std::vector<std::pair<unsigned, std::bitset<64 * 64>>> atlas_exclusions(const AtlasLayout& layout,
                                                                        const std::vector<MapFootprint>& footprints);

// For the stitched world's layout (world/world_layout.h): identical-item votes for the offset of
// map b's fixed items onto map a's near (ox, oy) (best offset in bx, by), and a map's top-level
// records (fixed with the data fixes + top-level nonfixed).
int atlas_seam_votes(const GameData& data, unsigned a, unsigned b, int ox, int oy, int& bx, int& by);
std::vector<struct ItemRecord> atlas_top_level_records(const GameData& data, unsigned map);

// Isometric screen offset (native pixels) of a world offset.
inline void atlas_screen_offset(int dx, int dy, int& sx, int& sy) {
    sx = (dx - dy) / 4;
    sy = (dx + dy) / 8;
}

}  // namespace pg
