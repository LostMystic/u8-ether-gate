// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The map dresser (PLAN-WORLD-EDITOR.md section 3): turns a painted area (docs/area_format.md,
// "pagan-area" v1) into original-style map items using the tile kit (editor/tilekit.h).
// No SDL and no game data: everything it needs about shapes is in the kit. Seeded and
// deterministic: the same area, kit and seed give the same items.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "data/mapfile.h"
#include "editor/tilekit.h"

namespace pg {

struct PaintedCell {
    char kind = ' ';     // docs/area_format.md "Cell codes"
    int level = 0;       // height level
    std::string theme;   // "" = the area's theme
};

struct PaintedPoi {
    std::string id;
    int x = 0, y = 0;    // cell
    std::string note;
    std::vector<std::string> tags;
    std::string prefab;  // forced prefab id ("" = choose)
    int radius = 2;
};

struct PaintedArea {
    std::string name;
    std::string theme = "cave";
    uint32_t seed = 1;
    int cell = 128, base_z = 0, level_z = 8;
    int origin[3] = {0, 0, 0};
    int w = 0, h = 0;                  // grid size in cells
    std::vector<PaintedCell> cells;    // row-major, h * w
    std::vector<PaintedPoi> pois;
    // Optional overrides of the theme's choices: wall, ledge (wall style ids), floor,
    // interior_floor, water, roof (floor set ids), as in data/tilekit.json.
    std::map<std::string, std::string> styles;

    const PaintedCell& at(int x, int y) const;
    char kind(int x, int y) const { return at(x, y).kind; }
    int z(int x, int y) const { return base_z + at(x, y).level * level_z; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};

// Parses a "pagan-area" JSON document (docs/area_format.md section 1).
bool parse_area(const std::string& json, PaintedArea& area, std::string& err);
// Builds an area from rows (tests and tools); heights are base-36 digits like the JSON.
PaintedArea make_area(const std::string& theme, const std::vector<std::string>& rows,
                      const std::vector<std::string>& heights = {}, uint32_t seed = 1);

struct DressedItem {
    uint16_t shape = 0, frame = 0;
    int x = 0, y = 0, z = 0;           // stored corner, area-local world units
    uint16_t flags = 0, quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
    std::string role;                  // floor, water, wall, post, door, jamb, roof, ledge, trim, poi, decor ...
    int cx = 0, cy = 0;                // the cell it was placed for
    std::string poi;                   // POI id for prefab items
};

struct PoiResult {
    std::string id, note, prefab, label;
    double score = 0;
    bool placed = false;
    int x = 0, y = 0;                  // cell of the marker
    int box[4] = {0, 0, 0, 0};         // placed box (area-local x0, y0, x1, y1), when placed
};

struct Validation {
    bool ok = false;
    bool floor_covered = false, walls_closed = false, walkable = false;
    int floor_cells = 0, uncovered_cells = 0;
    int wall_links = 0, wall_gaps = 0;
    int overlaps = 0;                  // solid items whose volumes intersect
    int unsupported = 0;               // objects standing on nothing
    int positions = 0, reachable = 0;  // avatar positions tested / reached from the entrances
    std::vector<std::string> unreachable_pois;
    std::vector<std::string> problems; // human-readable, the first few of each kind
};

struct DressResult {
    uint32_t seed = 0;
    int size[2] = {0, 0};              // area size in world units
    std::vector<DressedItem> items;
    std::vector<PoiResult> pois;
    std::vector<std::array<int, 2>> entrances;  // door / passage cells
    std::vector<std::string> warnings;
    Validation validation;
};

struct DressOptions {
    bool roofs = true;                 // roof tiles over enclosed rooms when the theme has them
    bool scatter = true;               // loose decorations on open floor
    bool validate = true;
};

// Dresses the area. seed 0 = the area's own seed.
DressResult dress(const PaintedArea& area, const TileKit& kit, uint32_t seed = 0, const DressOptions& opt = {});
// The dresser's checks (also run by dress()): placement rules of specs/world/physics.md 2
// (no solid overlaps, everything supported), full floors, closed walls, and walkability from
// the entrances to every POI with an avatar-sized box (64 x 64 x 40, steps of at most 8).
Validation validate(const DressResult& r, const PaintedArea& area, const TileKit& kit);

// "pagan-dressed" JSON (docs/area_format.md section 4).
std::string dressed_to_json(const PaintedArea& area, const DressResult& r);
// Reads the items back (for the viewer and the stitcher). origin gets the area's origin.
bool parse_dressed(const std::string& json, std::vector<ItemRecord>& items, int origin[3], std::string& err);
std::vector<ItemRecord> to_records(const DressResult& r, bool with_roofs = true);

}  // namespace pg
