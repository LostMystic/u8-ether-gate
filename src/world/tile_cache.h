// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <bitset>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gfx/surface.h"
#include "world/atlas_layout.h"
#include "world/world_view.h"

namespace pg {

struct GameData;
class CurrentMap;

// Zoomed-out views of the world. A map is cut into square tiles in its own screen space
// (native pixels: sx = x/4 - y/4, sy = x/8 + y/8 - z). A level-L tile is kTile x kTile
// pixels covering (kTile << L) native pixels per side, so one tile pixel is 2^L x 2^L native
// pixels. Level 0 tiles come from full renders (the normal sorter, kBlock x kBlock native
// pixels at a time, roof hiding off); level L+1 tiles are 2x2 box filters of four level-L
// tiles. Pixels are premultiplied 0xAARRGGBB; palette index 0 (the cleared background) is
// transparent. Tiles are built on demand within a time budget (so the web build never
// stalls) and kept in an LRU cache with a memory limit. Empty tiles are known from the
// map's chunk occupancy without rendering.
class TileCache {
public:
    static constexpr int kTile = 256, kBlock = 512, kMaxLevel = 10;

    explicit TileCache(const GameData& data);
    ~TileCache();

    struct Tile {
        bool empty = true;
        std::vector<uint32_t> px;  // kTile * kTile when not empty
        uint64_t used = 0;
    };

    // The tile if it is ready (nullptr if not built yet).
    const Tile* get(unsigned map, int level, int tx, int ty);
    // Builds the tile (and what it needs) until `deadline_ms` (ticks of now_ms()). True when
    // it is ready.
    bool build(unsigned map, int level, int tx, int ty, double deadline_ms);
    // Whether the native screen rectangle may contain anything of the map.
    bool area_maybe_drawn(unsigned map, int sx0, int sy0, int w, int h);
    // Native-pixel screen bounds of the map's content (with margins for tall shapes).
    bool content_bounds(unsigned map, int& sx0, int& sy0, int& sx1, int& sy1);

    void set_show_editor(bool on);
    // Milestone 6 (the game's far zoom): only the fixed map (fixed items and their globs) is
    // drawn; everything that can move or change is drawn live on top by the game mode.
    void set_fixed_only(bool on) { fixed_only_ = on; loaded_dirty_ = true; }
    // Chunks of `map` that are not drawn (atlas overlaps). Drops the map's tiles if changed.
    void set_exclusion(unsigned map, const std::bitset<64 * 64>& chunks);
    void clear_map(unsigned map);
    // How many chunks of `map` are not drawn (0: the map is drawn whole).
    size_t excluded_chunks(unsigned map) const;
    void set_budget_bytes(size_t b) { budget_ = b; }
    // Starts a frame: tiles at display_level touched from now on are not evicted.
    void next_frame(int display_level) { frame_ = ++clock_; display_level_ = display_level; }
    void clear();

    size_t bytes() const { return bytes_; }
    size_t tiles() const { return tiles_.size(); }
    size_t renders() const { return renders_; }
    double render_ms() const { return render_ms_; }

    static double now_ms();

private:
    struct MapState;
    MapState& state(unsigned map);
    static uint64_t key(unsigned map, int level, int tx, int ty) {
        return (uint64_t(map) << 56) | (uint64_t(level & 0xFF) << 48) | (uint64_t(uint32_t(tx + 0x800000) & 0xFFFFFF) << 24) |
               (uint64_t(uint32_t(ty + 0x800000) & 0xFFFFFF));
    }
    Tile& put(uint64_t k);
    void render_block(unsigned map, int bx, int by);
    void evict();

    const GameData& data_;
    WorldView view_;
    WorldView::Options opt_;
    Surface8 surf_{kBlock, kBlock};
    uint32_t lut_[256];
    std::unordered_map<uint64_t, Tile> tiles_;
    std::unordered_map<unsigned, std::unique_ptr<MapState>> maps_;
    std::unique_ptr<CurrentMap> loaded_;
    bool loaded_dirty_ = true;
    size_t budget_;
    size_t bytes_ = 0;
    uint64_t frame_ = 1, clock_ = 1;
    int display_level_ = 0;
    size_t renders_ = 0;
    bool want_l0_ = false;
    bool fixed_only_ = false;
    double render_ms_ = 0;
};

// Draws the zoomed view of a set of placed maps into an RGBA buffer. `zoom` is a power of
// two: one output pixel = 2^zoom native pixels (zoom 0 = native). (cx, cy) is the native
// screen point (in atlas space: map screen + group offset) at the buffer centre. Missing
// tiles are built within the deadline; returns the number still missing.
struct PlacedMap {
    unsigned map;
    int ox, oy;  // native screen offset of the map in atlas space
};
int draw_tiles(TileCache& cache, const std::vector<PlacedMap>& maps, int zoom, int cx, int cy, uint32_t* out, int w, int h,
               double deadline_ms, uint32_t background = 0xFF000000u);

}  // namespace pg
