// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/mapfile.h"
#include "world/item.h"
#include "world/world_layout.h"

namespace pg {

struct GameData;

// Which 64 x 64-unit cells of a map hold fixed scenery (glob contents included): bit 0 any
// drawn fixed item, bit 1 a LAND or ROOF surface. Local coordinates of that map.
struct MapCoverage {
    static constexpr int kCell = 64, kN = 32768 / kCell;
    unsigned map = 0;
    std::vector<uint8_t> bits = std::vector<uint8_t>(size_t(kN) * kN, 0);
    uint8_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= 32768 || y >= 32768) return 0;
        return bits[size_t(y / kCell) * kN + size_t(x / kCell)];
    }
    bool any(int x, int y) const { return at(x, y) & 1; }
    bool land(int x, int y) const { return at(x, y) & 2; }
};
MapCoverage compute_coverage(const GameData& data, unsigned map);

// The fixed items of a map with the data fixes and globs expanded, bucketed by 512-unit chunk
// (local coordinates). Eggs and other EDITOR shapes are left out.
struct MapStatics {
    unsigned map = 0;
    std::vector<std::vector<ItemRecord>> chunks = std::vector<std::vector<ItemRecord>>(64 * 64);
};

// The stitched world at run time (PLAN-WORLD-EDITOR.md section 1): the layout's offsets, the
// seams and their portals, and the conversion between world points (32-bit) and
// (map, local 16-bit point). The live game always simulates one map (the "active region"), in
// that map's own coordinates, exactly as the original; this class is the translation layer
// between it and world space.
class WorldSpace {
public:
    explicit WorldSpace(const GameData& data) : data_(data) {}
    bool load(const WorldLayoutFile& layout);
    bool load_file(const std::string& path, std::string& err);
    const WorldLayoutFile& layout() const { return layout_; }
    const std::string& path() const { return path_; }

    bool placed(unsigned map) const { return map < 256 && region_[map].placed; }
    // The map's group offset in world units, and its layer.
    void offset(unsigned map, int& dx, int& dy, int& dz) const;
    int layer(unsigned map) const { return map < 256 ? region_[map].layer : 0; }
    int group(unsigned map) const { return map < 256 ? region_[map].group : -1; }
    void to_world(unsigned map, int x, int y, int z, int32_t out[3]) const;
    // Local point of world point w in `map`.
    void to_local(unsigned map, const int32_t w[3], int out[3]) const;
    // The map whose content covers world point (x, y) on `layer` (the densest fixed scenery
    // there; variants: the one `variant_pick` chooses). false if none.
    bool map_at(int32_t x, int32_t y, int layer, unsigned& map, const std::function<unsigned(const WorldVariant&)>& variant_pick = {}) const;

    // Seams: the maps joined to `map` by a walkable seam, and the seam between two maps.
    const std::vector<unsigned>& seam_partners(unsigned map) const { static const std::vector<unsigned> none; return map < 256 ? region_[map].partners : none; }
    const WorldSeam* seam(unsigned a, unsigned b) const { return layout_.seam(a, b); }
    // A teleporter egg on `map` leading to `to_map` that is a seam crossing.
    bool is_portal(unsigned map, unsigned to_map, unsigned id) const;
    // A hash of the layout (saves record which layout they were made with).
    uint32_t layout_hash() const { return hash_; }

    // Lazily computed per map.
    const MapCoverage& coverage(unsigned map);
    const MapStatics& statics(unsigned map);

    // A map patch group changed (world/map_patches.h): the map's cached statics are stale.
    void forget_statics(unsigned map) { statics_.erase(map); }

private:
    struct Region { bool placed = false; int group = -1; int dx = 0, dy = 0, dz = 0, layer = 0; std::vector<unsigned> partners; };
    const GameData& data_;
    WorldLayoutFile layout_;
    std::string path_;
    Region region_[256];
    uint32_t hash_ = 0;
    std::map<unsigned, std::unique_ptr<MapCoverage>> coverage_;
    std::map<unsigned, std::unique_ptr<MapStatics>> statics_;
};

// Streams the scenery of the seam neighbours of the active map into the active map's own
// coordinates for drawing: the fixed items (globs expanded) and the stored nonfixed items of
// every neighbour chunk near the view, without the copy of the duplicated border strip (cells
// the active map covers itself are left to it). Far chunks are dropped again. Nothing here is
// simulated (no ids, no events): it is the view across the seam.
class NeighborStream {
public:
    explicit NeighborStream(const GameData& data) : data_(data) {}
    // Recomputes for the active map and a camera point (active-local) with a view of w x h
    // screen pixels. stored(map) gives a neighbour's stored nonfixed items (may be null).
    void update(WorldSpace& ws, unsigned active, const int cam[3], int w, int h,
                const std::function<const std::vector<MapEntry>*(unsigned)>& stored);
    const std::vector<const Item*>& items() const { return items_; }
    void clear();

    // Statistics (tests, the HUD, NOTES): chunk builds, their total and longest time.
    unsigned chunk_builds = 0, chunks_live = 0;
    double build_ms_total = 0, build_ms_max = 0, update_ms_last = 0;

private:
    struct Chunk { std::vector<Item> items; uint32_t last_used = 0; };
    const GameData& data_;
    unsigned active_ = 0xFFFF;
    uint32_t tick_ = 0;
    std::map<std::pair<unsigned, int>, Chunk> chunks_;  // (map, chunk index) -> items (world coords)
    std::vector<const Item*> items_;
};

}  // namespace pg
