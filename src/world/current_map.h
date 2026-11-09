// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "data/mapfile.h"
#include "world/item.h"

namespace pg {

struct GameData;

// Milestone-1 name for a map item; items are now real objects (world/item.h).
using WorldItem = Item;

// What the game world does when items enter or leave the fast area (items.md 7). The map
// viewer has no hooks: it only expands globs and drops fast-only items.
class MapHooks {
public:
    virtual ~MapHooks() = default;
    // Called for an item becoming fast, after glob expansion and before FASTAREA is set.
    virtual void item_enter_fast(Item& it) = 0;
    // Called for an item leaving the fast area while it is still flagged FASTAREA.
    virtual void item_leave_fast(Item& it) = 0;
    // Destroys a fast-only item that left the fast area (it is already unlinked).
    virtual void destroy_item(uint16_t id) = 0;
};

// The currently loaded map: items linked into 64 x 64 chunk lists, the fast area and
// glob expansion (specs/world/items.md 7-9, specs/presentation/rendering.md 2 and 4).
class CurrentMap {
public:
    static constexpr int kChunks = 64, kChunkSize = 512;

    // Map viewer: owns its own object table.
    explicit CurrentMap(const GameData& data);
    // Game: shares the world's object table.
    CurrentMap(const GameData& data, ObjectTable& objects);
    void set_hooks(MapHooks* h) { hooks_ = h; }

    // Viewer: clears everything, loads FIXED + NONFIXED entry `map` (with the data fixes)
    // and the NPCs on it.
    void load(unsigned map);
    // Game: empties the chunk lists and fast area for `map` (objects are not touched).
    void reset(unsigned map);
    unsigned map_number() const { return map_; }
    // Map numbers with any fixed or nonfixed items.
    static std::vector<unsigned> populated_maps(const GameData& data);

    // Creates an item from a record (fixed / nonfixed), with a fresh id, not linked.
    uint16_t create_item(const ItemRecord& r, Item::Origin origin);
    // Creates a whole record tree (containers get their contents) and links the top-level
    // items. Returns the ids of the top-level items.
    std::vector<uint16_t> create_tree(const std::vector<MapEntry>& tree, Item::Origin origin);
    // Adds/removes a top-level item to/from its chunk list (front or back).
    void link(uint16_t id, bool at_front = false);
    void unlink(uint16_t id);

    // Recomputes the fast area for a camera moving from prev to cur (world points) and a
    // view of w x h pixels. Chunks entering the area expand their glob eggs; chunks leaving
    // it drop their FAST_ONLY items.
    void update_fast_area(const int prev[3], const int cur[3], int w, int h);
    bool is_fast(int cx, int cy) const { return fast_[size_t(cy) * kChunks + cx]; }
    std::vector<bool>& fast_bits() { return fast_; }  // savegames
    void set_fast(int cx, int cy);  // force one chunk fast (legal-create intrinsics)
    bool point_is_fast(int x, int y) const;

    // The roof limit for camera point p (rendering.md section 4); 65536 if no roof.
    int roof_limit(const int p[3]) const;

    const std::vector<uint16_t>& chunk(int cx, int cy) const { return chunks_[size_t(cy) * kChunks + cx]; }
    // The object with this id (a dead placeholder if there is none).
    Item& item(uint32_t id);
    const Item& item(uint32_t id) const;
    size_t item_slots() const { return objs_->slots(); }
    size_t live_items() const;
    ObjectTable& objects() { return *objs_; }
    // Finds the first live item with this shape (and quality if >= 0) near (x, y).
    const Item* find_item(unsigned shape, int quality, int x, int y, int range) const;
    const Item* npc(unsigned number) const;

    // Contained items of the nonfixed list (viewer).
    const std::vector<MapEntry>& nonfixed_tree() const { return nonfixed_tree_; }
    void expand_glob(uint16_t egg);

private:
    void enter_chunk(int cx, int cy);
    void leave_chunk(int cx, int cy);

    const GameData& data_;
    std::unique_ptr<ObjectTable> own_;
    ObjectTable* objs_;
    MapHooks* hooks_ = nullptr;
    unsigned map_ = 0;
    std::vector<std::vector<uint16_t>> chunks_ = std::vector<std::vector<uint16_t>>(kChunks * kChunks);
    std::vector<bool> fast_ = std::vector<bool>(kChunks * kChunks, false);
    std::vector<MapEntry> nonfixed_tree_;
    Item dead_;
};

// The data fixes for missing or misplaced fixed tiles (specs/world/items.md section 9).
// With the "Original bug fixes" option On (core/bugfix.h) also the further map-7 and map-62 fixes
// (NOTES.md "Original bug fixes"); they take effect at the next map load. Also adds the map patch
// groups that are on (world/map_patches.h: new content, not fixes).
void apply_map_fixes(unsigned map, std::vector<ItemRecord>& fixed);

}  // namespace pg
