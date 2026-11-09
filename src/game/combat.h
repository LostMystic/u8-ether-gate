// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <vector>

#include "kernel/kernel.h"

namespace pg {

class World;
class Actor;
class Item;

// Damage and defence type bits (combat.md 1).
enum DamageType : uint16_t {
    kDmgNormal = 0x0001, kDmgBlade = 0x0002, kDmgBlunt = 0x0004, kDmgFire = 0x0008, kDmgUndead = 0x0010,
    kDmgMagic = 0x0020, kDmgSlayer = 0x0040, kDmgPierce = 0x0080, kDmgFalling = 0x0100,
};

// A weapon row (combat.md 4).
struct WeaponInfo {
    uint16_t shape;
    uint8_t base, mod;
    int8_t atk_dex, def_dex, armour;
    uint16_t type;
    uint8_t treasure;  // percent
    uint8_t overlay;   // WPNOVLAY type
};
const WeaponInfo* weapon_info(unsigned shape);

// A monster row (combat.md 6).
struct MonsterInfo {
    uint16_t shape;
    uint8_t hp0, hp1, dex0, dex1, dmg0, dmg1, ac, align;
    uint16_t dmg_type, def_type;
    bool ranged, resurrect, shifter;
    uint16_t explode;
    const char* treasure;  // ';'-separated default treasure names (combat.md 7)
};
const MonsterInfo* monster_info(unsigned shape);
// Rows the engine adds for its own monsters (the new creatures, game/creatures.h), searched after
// the original table. Their shapes are engine shapes (2048 and up), so nothing overlaps.
std::vector<MonsterInfo>& extra_monster_rows();

// The rolled values of an attacker / defender (combat.md 3).
int attack_damage(const World& w, const Actor& a);
uint16_t attack_type(const World& w, const Actor& a);
int armour_class(const World& w, const Actor& a);
// The armour an item adds when worn (armour pieces, or a weapon's armour bonus).
int item_armour(const Item& it);
uint16_t defence_type(const World& w, const Actor& a);
int attack_dex(const World& w, const Actor& a);
int defend_dex(const World& w, const Actor& a);
// The damage calculation of combat.md 2.1 (armour, blocking, to-hit).
int calculate_damage(World& w, int damage, uint16_t type, Actor& target, Actor* attacker);
bool is_enemy(const Actor& a, const Actor& b);

// Turns an actor on the spot (actors.md 7.9). Returns the pid of the last animation (0 if
// it already faces that way).
uint16_t turn_actor(World& w, uint16_t actor, int dir);

// An NPC's combat AI (actors.md 9.2).
class CombatProcess : public Process {
public:
    CombatProcess(World& w, uint16_t actor) : w_(w) { item = actor; type = kProcCombat; }
    void run() override;
    const char* name() const override { return "combat"; }
    PG_SAVED_PROCESS("combat")

private:
    bool valid_target(uint16_t t) const;
    uint16_t find_target() const;
    bool in_range(uint16_t t) const;
    World& w_;
    enum Mode { kSearching, kWaiting, kPathing } mode_ = kSearching;
    int delay_ = 0;
};

// The Avatar's death sequence (avatar.md 11). Type 1, so it survives map changes.
class AvatarDeathProcess : public Process {
public:
    explicit AvatarDeathProcess(World& w) : w_(w) { type = kProcPersistent; }
    void run() override;
    const char* name() const override { return "avatar death"; }
    PG_SAVED_PROCESS("death")

private:
    World& w_;
    int stage_ = 0;
};

// Brings a dead skeleton back (actors.md 5.4 step 7).
class ResurrectProcess : public Process {
public:
    ResurrectProcess(World& w, uint16_t actor, int frames) : w_(w), left_(frames) { item = actor; type = kProcResurrect; }
    void run() override;
    const char* name() const override { return "resurrect"; }
    PG_SAVED_PROCESS("resurrect")

private:
    World& w_;
    int left_;
};

}  // namespace pg
