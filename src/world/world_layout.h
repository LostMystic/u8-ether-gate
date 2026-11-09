// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
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
    std::string basis = "votes"; // why it is a seam: "votes" (identical border items) or "teleport edge" (declared)
    int strip[4] = {0, 0, 0, 0}; // world box (x0, y0, x1, y1) of the 64-unit cells both maps cover
    std::vector<WorldPortal> portals;
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

struct WorldLayoutFile {
    AtlasLayout atlas;  // groups with dx, dy, dz, layer
    std::vector<WorldSeam> seams;
    std::vector<WorldLink> links;
    std::vector<WorldVariant> variants;
    std::map<int, std::string> layer_names;
    // Generation of the curated layout plan this file was made from (1: derived by rule, 2: the
    // fan-map layout with no overlap on any layer). The editor replaces a stored layout of an
    // older generation by the shipped one (keeping its painted areas).
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
constexpr int kLayoutRevision = 2;  // generate_world_layout's plan

}  // namespace pg
