// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <vector>

#include "core/bytes.h"
#include "data/flex.h"

namespace pg {

class TypeFlags;

// Item status flags (specs/formats/maps.md section 2).
enum ItemFlag : uint16_t {
    kItemDisposable = 0x0002, kItemOwned = 0x0004, kItemContained = 0x0008, kItemInvisible = 0x0010,
    kItemFlipped = 0x0020, kItemInNpcList = 0x0040, kItemFastOnly = 0x0080, kItemGumpOpen = 0x0100,
    kItemEquipped = 0x0200, kItemBouncing = 0x0400, kItemEthereal = 0x0800, kItemHanging = 0x1000,
    kItemFastArea = 0x2000, kItemLowFriction = 0x4000,
};

// One 16-byte record of FIXED.DAT / NONFIXED.DAT.
struct ItemRecord {
    uint16_t x = 0, y = 0;
    uint8_t z = 0;
    uint16_t shape = 0;
    uint8_t frame = 0;
    uint16_t flags = 0;
    uint16_t quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
    uint16_t next = 0;
};

// A record placed in the item tree: top-level items have parent -1. For contained
// items, x is the nesting depth and y the packed gump position (gx = low, gy = high byte).
struct MapEntry {
    ItemRecord rec;
    int parent = -1;  // index into the same list
};

// Raw records of one map entry, in file order.
std::vector<ItemRecord> parse_item_records(ByteView data);
// Rebuilds container nesting (pre-order walk, maps.md "Containers and contents").
std::vector<MapEntry> build_item_tree(const std::vector<ItemRecord>& records, const TypeFlags& tf);

// GLOB.FLX: prefab groups of fixed items.
struct GlobItem {
    uint8_t x = 0, y = 0, z = 0;  // x, y in 2-unit steps within the chunk; z in world units
    uint16_t shape = 0;
    uint8_t frame = 0;
};

class GlobFile {
public:
    bool load(Bytes file);
    unsigned count() const { return unsigned(globs_.size()); }
    // Empty if the glob does not exist.
    const std::vector<GlobItem>& get(unsigned n) const { return n < globs_.size() ? globs_[n] : empty_; }

private:
    std::vector<std::vector<GlobItem>> globs_;
    std::vector<GlobItem> empty_;
};

// Permanent NPCs from GAMEDAT/ITEMCACH.DAT + NPCDATA.DAT (specs/world/startup.md, maps.md 6).
struct NpcRecord {
    unsigned number = 0;  // = object id
    uint16_t x = 0, y = 0;
    uint8_t z = 0;
    uint16_t shape = 0;
    uint16_t frame = 0;  // 16-bit: ITEMCACH low byte + NPCDATA 0x07 high byte
    uint16_t flags = 0;
    uint16_t quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
    // NPCDATA
    uint8_t str = 0, dex = 0, intel = 0, hp = 0, dir = 0;
    uint16_t last_anim = 0;
    uint8_t alignment = 0, unk0c = 0;
    int16_t mana = 0;
    uint32_t actor_flags = 0;  // bits 0-7 from 0x1B, 8-15 from 0x2F, 16-23 from 0x30
};

// Returns the NPCs 1..255 that exist (shape != 0). False if a file is malformed.
bool load_npcs(Bytes itemcache, Bytes npcdata, std::vector<NpcRecord>& out);

}  // namespace pg
