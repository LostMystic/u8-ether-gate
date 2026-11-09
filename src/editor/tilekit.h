// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pg {

// The tile kit: what the map dresser knows about the original maps' building blocks
// (PLAN-WORLD-EDITOR.md section 3). It is mined from the game data by pagan_tilekit
// (editor/tilekit_miner.*) and kept as data/tilekit.json, which may be edited by hand.
// Loading it needs no game data, so the dresser runs anywhere.

enum class Role : uint8_t {
    kNone, kFloor, kWater, kLava, kRoof, kWallEW, kWallNS, kPost, kDoor, kTrim, kStairs,
    kLight, kPlant, kTree, kDecoration, kClutter, kFurniture, kContainer, kEgg, kNpc, kEffect,
};
const char* role_name(Role r);
Role role_from_name(const std::string& s);

// One (shape, frame) choice with a weight (usually how often the original maps use it).
struct KitPiece {
    uint16_t shape = 0;
    uint16_t frame = 0;
    double weight = 1;
};

// What the kit records about each shape that the original maps use.
struct ShapeEntry {
    unsigned shape = 0;
    Role role = Role::kNone;
    std::string name;            // usecode class name, if the shape has one
    int fp[3] = {0, 0, 0};       // footprint in world units (x, y, z)
    uint16_t tflags = 0;         // ShapeInfo::flags() (SOLID 0x0002, LAND 0x0008, ROOF 0x0400 ...)
    unsigned family = 0;
    int count = 0;               // uses in the original maps (fixed + globs + nonfixed)
    int nonfixed = 0;            // of which nonfixed
    int z = 0;                   // most common z
    std::vector<KitPiece> frames;                      // frames used, weight = uses
    std::vector<std::pair<std::string, double>> themes;  // share of uses per theme (top few)
    bool solid() const { return tflags & 0x0002; }
    bool land() const { return tflags & 0x0008; }
};

// A floor material: (shape, frame) tiles that the original maps lay next to each other,
// with their pairwise neighbour counts (used for weighted, non-repeating fills).
struct FloorSet {
    std::string id;                  // "fs3" ...
    std::string kind;                // floor, water, lava, roof
    std::string label;               // a readable guess ("wood planks" ...), editable
    std::vector<KitPiece> tiles;     // fill candidates, weight = uses
    std::vector<KitPiece> accents;   // tiles of the material that stand out (patterns, rugs): not used for fills
    // {a, b, h, v}: tile b east of tile a (+x) h times, south of it (+y) v times.
    std::vector<std::array<int, 4>> pairs;
    // Edge frames: for a cell whose 4-neighbour mask (bit 0 N(-y), 1 E(+x), 2 S(+y), 3 W(-x)
    // = neighbour of the same set) is m, edge[m] lists tiles seen with exactly that mask.
    std::array<std::vector<KitPiece>, 16> edge;
    double roof_fraction = 0;        // share of tiles under a roof (interior floors)
    int count = 0;
    int z = 0;                       // most common z
    int footprint = 128;             // tile size in world units
};

// A wall style: posts and straight pieces of one look, mined from how pieces touch.
struct WallStyle {
    std::string id;                  // "ws149" (named after its most used EW piece)
    std::string label;
    int height = 40;                 // world units
    int count = 0;
    bool land_top = false;           // the top is walkable (cliffs, ledges)
    std::array<std::vector<KitPiece>, 5> ew, ns;  // by length in 32-unit steps (1..4); [0] unused
    std::vector<KitPiece> post;      // 32 x 32 pieces at corners, T-junctions and ends
    std::vector<KitPiece> jamb;      // posts seen next to doors
    KitPiece door_ew, door_ns;       // shape 0 = no door seen with this style
    uint16_t door_quality = 0;
    std::vector<std::pair<std::string, double>> themes;
};

// A theme: what one kind of place is made of (clustered from the original maps' areas).
struct ThemeKit {
    std::string name;                // cave, catacombs, tenebrae ...
    std::string label;
    std::vector<unsigned> maps;      // seed maps (from map names)
    std::vector<std::pair<unsigned, int>> area_maps;  // map -> areas assigned to the theme
    int areas = 0;
    std::string floor, interior_floor, water, lava, roof;  // FloorSet ids ("" = none)
    std::vector<std::string> floors_alt;                    // other floor sets used
    std::string wall, ledge;         // WallStyle ids
    std::vector<KitPiece> lights, plants, trees, decor, clutter, furniture;
    std::array<std::vector<KitPiece>, 4> trim_void;    // edge pieces by side (N E S W) facing void
    // Shore tiles laid in water cells next to land, by the cell's wet-neighbour mask (bit N1 E2 S4
    // W8 = that neighbour is water); they stand at the water's z and are 8 high.
    std::array<std::vector<KitPiece>, 16> shore;
    double scatter = 0.05;           // share of open floor cells that get a decoration
    std::vector<std::string> fallback;
};

struct PrefabItem {
    uint16_t shape = 0, frame = 0;
    int dx = 0, dy = 0, dz = 0;      // stored corner relative to the prefab's box (min x, min y, floor z)
    uint16_t flags = 0, quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
};

// A point-of-interest prefab lifted from the original maps.
struct Prefab {
    std::string id, label;
    std::vector<std::string> keywords;
    std::string theme;
    unsigned map = 0;
    int x = 0, y = 0, z = 0;         // where it was found (box min x, min y; floor z)
    int sx = 0, sy = 0, sz = 0;      // box size in world units
    std::vector<PrefabItem> items;
};

// Adjacency statistics (for reports and later tuning): b relative to a at (dx, dy, dz).
struct AdjStat {
    std::string kind;                // floor_h, floor_v, wall_join, on_top ...
    unsigned a = 0, b = 0;
    int dx = 0, dy = 0, dz = 0;
    int count = 0;
};

struct TileKit {
    int version = 1;
    int cell = 128;
    std::string source;              // what it was mined from
    std::map<unsigned, ShapeEntry> shapes;
    std::vector<FloorSet> floors;
    std::vector<WallStyle> walls;
    std::map<std::string, ThemeKit> themes;
    std::vector<Prefab> prefabs;
    std::vector<AdjStat> adjacency;
    std::map<std::string, int> counts;  // report numbers

    const ShapeEntry* shape(unsigned s) const {
        auto it = shapes.find(s);
        return it == shapes.end() ? nullptr : &it->second;
    }
    const FloorSet* floor(const std::string& id) const;
    const WallStyle* wall(const std::string& id) const;
    const Prefab* prefab(const std::string& id) const;
    // The theme, or its fallback chain, or "cave", or the first theme.
    const ThemeKit* theme(const std::string& name) const;
};

std::string tilekit_to_json(const TileKit& kit);
bool parse_tilekit(const std::string& text, TileKit& kit, std::string& err);
bool load_tilekit(const std::string& path, TileKit& kit, std::string& err);

}  // namespace pg
