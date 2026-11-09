// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kernel/id_alloc.h"

namespace pg {

class Actor;

// Engine-only item flags (never visible to usecode; specs/world/items.md section 3).
enum ItemExtFlag : uint16_t {
    kExtFromFixed = 0x0001, kExtInChunk = 0x0002, kExtNoInterp = 0x0004, kExtCameraFollow = 0x0008,
    kExtSprite = 0x0010, kExtPermanentNpc = 0x0020, kExtGhost = 0x0080,
    kExtXpPaid = 0x0100,  // this body has paid its XP (game/leveling.h); party.h uses 0x0040
};

// Anything in the world: a wall, a coin, an egg, an NPC (specs/world/items.md).
// Containers are items whose shape family is 6 (and actors); their contents are ids.
class Item {
public:
    enum Origin : uint8_t { kFixed, kNonFixed, kNpc, kGlob, kCreated };
    virtual ~Item() = default;
    virtual Actor* as_actor() { return nullptr; }
    virtual const Actor* as_actor() const { return nullptr; }

    uint16_t id = 0;
    uint16_t shape = 0;
    uint16_t frame = 0;
    int32_t x = 0, y = 0, z = 0;
    uint16_t flags = 0;      // usecode-visible status flags (kItem*)
    uint16_t ext_flags = 0;  // engine-only (kExt*)
    uint16_t quality = 0;
    uint8_t npcnum = 0, mapnum = 0;
    Origin origin = kFixed;
    bool alive = true;        // kept for the map viewer and tests
    uint16_t parent = 0;      // container id, 0 if top level
    std::vector<uint16_t> contents;
    uint16_t gravity_pid = 0;
    int32_t chunk = -1;       // chunk list index while linked into the current map
    bool hatched = false;     // eggs
};

// An NPC or monster (specs/world/actors.md).
class Actor : public Item {
public:
    Actor* as_actor() override { return this; }
    const Actor* as_actor() const override { return this; }

    enum Flag : uint32_t {
        kInvincible = 0x000001, kAnimLock = 0x000008, kFirstStep = 0x000400, kInCombat = 0x000800,
        kDead = 0x001000, kCombatRun = 0x008000, kAirWalk = 0x010000, kImmortal = 0x040000,
        kWithstandDeath = 0x080000, kFeignDeath = 0x100000, kStunned = 0x200000, kPoisoned = 0x400000,
        kPathfinding = 0x800000,
    };
    int16_t str = 0, dex = 0, intel = 0;
    uint16_t hp = 0;
    int16_t mana = 0;
    uint8_t dir = 0;           // 0..7
    uint16_t last_anim = 2;
    uint16_t last_anim_dir = 0;
    uint8_t alignment = 0, enemy_alignment = 0;
    uint8_t unk0c = 0;
    uint32_t actor_flags = 0;
    uint16_t target = 0;
    uint16_t fixed_target = 0;  // the first combat target set (actors.md 9.2)
    uint16_t activity = 0;
    std::string name;
    bool is(uint32_t f) const { return (actor_flags & f) != 0; }
    void set(uint32_t f, bool on) { if (on) actor_flags |= f; else actor_flags &= ~f; }
};

// All live objects by id, with the id pools of specs/world/kernel.md section 6:
// actors 1..255, items 512..32766 (256..511 and 666 reserved).
class ObjectTable {
public:
    ObjectTable() { clear(); }
    void clear();

    Item* get(uint16_t id) const { return id < objs_.size() ? objs_[id].get() : nullptr; }
    Actor* actor(uint16_t id) const { Item* it = get(id); return it ? it->as_actor() : nullptr; }

    // Gives the object a fresh id from the item (or actor) pool and stores it. 0 on failure.
    uint16_t add_item(std::unique_ptr<Item> it);
    uint16_t add_actor(std::unique_ptr<Actor> a);
    // Stores an object at a fixed id (permanent NPCs). False if the id is taken.
    bool add_at(uint16_t id, std::unique_ptr<Item> it);
    // Deletes the object and frees its id (does not touch containers or chunks).
    void erase(uint16_t id);
    // Takes the object out without freeing it (id released).
    std::unique_ptr<Item> take(uint16_t id);
    size_t live_count() const { return live_; }
    // Savegames: the id pools (the objects themselves are written by the world).
    void save_ids(SaveWriter& out) const { item_ids_.save(out); actor_ids_.save(out); }
    void load_ids(SaveReader& in) {
        item_ids_.load(in);
        actor_ids_.load(in);
        for (unsigned i = 1; i < objs_.size(); ++i)
            if (!objs_[i] && i != 666) { if (i < 256) actor_ids_.release(i); else item_ids_.release(i); }
    }
    size_t slots() const { return objs_.size(); }

private:
    std::vector<std::unique_ptr<Item>> objs_;
    IdAllocator item_ids_, actor_ids_;
    size_t live_ = 0;
};

// Direction helpers (specs/world/actors.md introduction).
inline int dir_x(int d) { static const int t[8] = {0, 1, 1, 1, 0, -1, -1, -1}; return t[d & 7]; }
inline int dir_y(int d) { static const int t[8] = {-1, -1, 0, 1, 1, 1, 0, -1}; return t[d & 7]; }
int direction_to(int dx, int dy);

}  // namespace pg
