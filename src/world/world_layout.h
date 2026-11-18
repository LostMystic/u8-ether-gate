// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "world/atlas_layout.h"

namespace pg {

struct GameData;

// The stitched world's layout (PLAN-WORLD-EDITOR.md section 1): every map group of the atlas
// placed in one world space with an x / y offset, a z offset (dz) and a layer, plus how the
// groups are linked. It is engine/data/world_layout.json: the atlas layout's "groups" (with
// "offset": [dx, dy, dz] and "layer") and these extra top-level fields, which the editor keeps
// verbatim when it saves (editor/area_model.h WorldLayout::extra):
//   "seams"    - walkable joins: maps that repeat a strip of each other's border
//   "links"    - every other link between maps, kept as a teleport
//   "variants" - one place in two states (16/26, 24/25 by G[52:2])
//   "layers"   - what each layer number holds
// A world point is (map's group offset) + (the map's own 16-bit point).

// A teleporter egg between two seam maps: where the original game moved the Avatar across.
struct WorldPortal {
    unsigned map = 0, to_map = 0, id = 0;
    int x = 0, y = 0, z = 0;  // local to `map`
};

struct WorldSeam {
    unsigned a = 0, b = 0;       // a < b
    int dx = 0, dy = 0;          // b's offset in a's coordinates (a point of b + (dx, dy) = a's point)
    int dz = 0;                  // b's group dz - a's (a height trim: the two maps' ground at one world z)
    int votes = 0;               // identical border items that line up at that offset
    std::string basis = "votes"; // why it is a seam: "votes" (identical border items), "teleport edge" (declared)
                                 // or "road": the maps were moved apart and a road built between them
                                 // (`road`, a map patch group of world_roads.json, part of one of them)
    int strip[4] = {0, 0, 0, 0}; // world box (x0, y0, x1, y1) of the 64-unit cells both maps cover
    std::vector<WorldPortal> portals;  // a road seam's are left idle (the road replaces the jump)
    std::string road;                 // basis "road": the road's patch group id
    // basis "road": where to walk to cross, in each map's own coordinates (end_a in map a, end_b in b)
    int end_a[3] = {0, 0, 0}, end_b[3] = {0, 0, 0};
    std::string derived;
};

struct WorldLink {
    unsigned from_map = 0, to_map = 0;
    std::string kind;            // "egg", "usecode", "maptele"
    int id = -1;                 // egg id (teleporters, teleportToEgg)
    bool has_from = false;       // false: no fixed source point (an item or spell)
    int from[3] = {0, 0, 0}, to[3] = {0, 0, 0};  // local points
    std::string mode = "teleport";  // kept as a teleport (later: stairs, ramps, lifts)
    std::string condition;       // JSON text of the critical path's condition ("" = always)
    std::string derived;
};

struct WorldVariant {
    std::string group;           // the group holding both maps
    unsigned global_pos = 52, global_width = 2;  // the selecting global G[pos:width]
    unsigned when_zero = 0, when_nonzero = 0;    // map numbers
    std::string derived;
};

// Cells where a map hides its scenery in the stitched world because a neighbour's version of that
// ground is the real one (map_patches::set_hidden; NOTES.md "Which map is real"). Rows of 128-unit
// cells in the map's own coordinates: {y, x0, x1}.
struct WorldHidden {
    unsigned map = 0;
    std::vector<std::array<int, 3>> rows;
    // Of those, the cells where the map's floor stays (the map kept there has no floor of its own
    // under part of the cell): only what stands on the floor is hidden.
    std::vector<std::array<int, 3>> ground_rows;
    std::string derived;
};

struct WorldLayoutFile {
    AtlasLayout atlas;  // groups with dx, dy, dz, layer
    std::vector<WorldSeam> seams;
    std::vector<WorldLink> links;
    std::vector<WorldVariant> variants;
    std::vector<WorldHidden> hidden;
    std::map<int, std::string> layer_names;
    // Generation of the curated layout plan this file was made from (1: derived by rule, 2: the
    // fan-map layout with no overlap on any layer, 3: the planes moved to kPlanesLayer, 4: the
    // Cemetery and Herdsman's Valley aligned, 5: the docks moved, the docks road, 6: map pieces). The editor replaces a stored layout of generation 0
    // or 1 by the shipped one (keeping its painted areas); one of generation 2 to 4 is updated in
    // place (update_stored_layout): the planes moved up, maps 6 and 7 aligned if still at their old
    // spots; every other group keeps its place.
    int layout_revision = 0;

    // Reads the groups and the extra fields from world_layout.json text (an atlas-only file
    // gives no seams). False on bad JSON.
    bool from_json(const std::string& text, std::string& err);
    bool load(const std::string& path, std::string& err);
    // The extra top-level fields as one JSON object text: {"seams": [...], "links": [...],
    // "variants": [...], "layers": {...}}.
    std::string extra_json() const;
    // A whole file (groups + extra fields, no editor areas): for tools and tests.
    std::string to_json() const;
    const WorldSeam* seam(unsigned m1, unsigned m2) const;
    // The road patch groups the layout's road seams name (map_patches::set_active_roads).
    std::vector<std::string> roads() const;
    // Turns the layout's hidden cells on (map_patches::set_hidden) for the maps this copy has as
    // shipped; `d` null turns them off.
    void activate_hidden(const GameData* d) const;
};

// Derives the stitched layout from the game data and the critical path's map links
// (specs/data/map_links.json text; "" = teleporter eggs only):
// - groups as in the atlas (49 and 50 share coordinates), plus the two-state places 16/26 and
//   24/25 as one group each (a variant);
// - each group at the exact offset of its link to an already placed group (seams first, by
//   their votes; then teleporter eggs; then usecode teleports), on its preferred layer
//   (specs/world/map_index.md: dungeons, catacombs and caves below the surface, planes and high
//   places above); if its occupied chunks would overlap a group already on that layer, it goes
//   one layer further down (or up). Seam partners share a layer and overlap only in their
//   duplicated border strip;
// - unlinked (unused / test) maps packed on their own layer;
// - dz = 256 x layer (a map's z range is 0..255, so stacked layers never share z).
WorldLayoutFile generate_world_layout(const GameData& data, const std::string& map_links_json);

// Checks of a layout: pairs of groups on the same layer whose occupied chunks overlap (seam
// partners excepted) and maps in no group. Empty = fine.
std::vector<std::string> check_world_layout(const WorldLayoutFile& l, const std::vector<MapFootprint>& fps);

constexpr int kLayerZ = 256;
constexpr int kMaxHeightTrim = 64;  // a group's dz = 256 x layer + a trim of at most this (seam partners' ground heights)
constexpr int kLayoutRevision = 6;  // generate_world_layout's plan (3: the planes moved from layer 1 to kPlanesLayer;
                                    // 4: maps 6 and 7 aligned; 5: the docks (3) moved, the docks road; see layout_moves();
                                    // 6: pieces of maps 4 and 8 (their caves) placed on their own, AtlasGroup::part;
                                    // a layout may also carry "hidden" cells, NOTES.md "Which map is real")
// The planes and realms (ethereal, elemental, and any added later) live far above the surface, so the
// layers between (1 .. kPlanesLayer - 1) stay free for places above ground (the user, 2026-11-17).
constexpr int kPlanesLayer = 100;

// Groups a layout generation moved (for updating a stored older layout in place: a map still at
// `from` goes to `to`; one the user has moved stays where it is; the seams to it are taken from the
// shipped layout). Generation 4: the Cemetery (6) and Herdsman's Valley (7) aligned by the overlap
// report (pagan_overlaps); 5: the docks (3) moved apart and joined by the docks road. 2026-11-17.
struct LayoutMove { int generation; unsigned map; int from_dx, from_dy, to_dx, to_dy; };
const std::vector<LayoutMove>& layout_moves();

}  // namespace pg
