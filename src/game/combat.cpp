// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Combat (specs/world/combat.md, specs/world/actors.md 5 and 9): the weapon, armour and
// monster tables, a hit's damage pipeline, death, corpses and treasure, the NPC combat AI,
// turning on the spot, stat training and the Avatar's death.
#include "game/combat.h"
#include "game/creatures.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/turn_combat.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/world.h"

namespace pg {

namespace {

const WeaponInfo kWeapons[] = {
    {540, 4, 9, 0, 0, 0, 0x04, 7, 0},   {541, 5, 12, 0, 0, 0, 0x02, 5, 1}, {542, 5, 12, 0, 0, 0, 0x02, 3, 1},
    {420, 5, 13, 0, 0, 0, 0x02, 2, 1},  {419, 6, 14, 0, 0, 0, 0x06, 1, 2}, {418, 3, 7, 0, 0, 0, 0x02, 15, 3},
    {417, 4, 11, 0, 0, 0, 0x04, 9, 0},  {815, 11, 9, 0, 0, 0, 0x24, 0, 0}, {822, 8, 14, 2, 2, 2, 0x26, 0, 2},
    {816, 4, 11, 0, 0, 0, 0x64, 0, 0},  {821, 5, 12, 1, 0, 0, 0x32, 0, 1}, {820, 7, 7, 4, 0, 1, 0x22, 0, 3},
    {819, 5, 13, 7, 0, 0, 0x22, 0, 1},  {818, 5, 13, 0, 4, 5, 0x22, 0, 1}, {817, 5, 12, 1, 0, 0, 0x2A, 0, 1},
};

const MonsterInfo kMonsters[] = {
    {142, 35, 50, 14, 16, 6, 12, 15, 0x68, 0x0001, 0x0050, false, false, false, 0, ""},
    {411, 80, 95, 15, 21, 0, 0, 16, 0x68, 0x0201, 0x0070, true, false, false, 0, "potion;obsidian*20-40;firegem*1-5"},
    {119, 35, 45, 15, 20, 7, 13, 17, 0x68, 0x0002, 0x0050, false, true, false, 0,
     "potion;weapon;obsidian*7-15;firegem*1-4;oilflask*1-4"},
    {120, 40, 55, 8, 12, 7, 14, 28, 0x24, 0x0001, 0x0082, false, false, false, 0, "meat"},
    {83, 150, 225, 7, 10, 10, 28, 18, 0xE8, 0x0004, 0x0080, false, false, false, 0,
     "potion;weapon;obsidian*30-75;firegem*1-8;oilflask*1-8"},
    {96, 80, 100, 14, 21, 12, 18, 19, 0xA4, 0x0029, 0x0088, false, false, false, 0, "demon1;demon2;demon3"},
    {214, 150, 175, 7, 8, 15, 25, 18, 0x24, 0x0004, 0x008A, false, false, false, 0, "obsidian*30-75;firegem*1-8"},
    {509, 75, 100, 17, 25, 12, 16, 14, 0x68, 0x0001, 0x0000, false, false, false, 458, ""},
    {413, 2, 5, 12, 15, 1, 3, 0, 0xA4, 0x0081, 0x0010, false, false, false, 0, ""},
    {357, 30, 45, 12, 16, 7, 10, 11, 0x24, 0x0001, 0x0000, false, false, true, 0, "obsidian*7-15;firegem*1-3;oilflask*1-3"},
    {574, 35, 65, 15, 25, 8, 15, 25, 0xE1, 0x0002, 0x0000, false, false, false, 0,
     "weapon;obsidian*10-20;firegem*1-4;oilflask*1-4"},
    {807, 2, 6, 8, 12, 1, 1, 0, 0x24, 0x0002, 0, false, false, false, 0, ""},
    {806, 2, 7, 8, 12, 1, 2, 0, 0x24, 0x0002, 0, false, false, false, 0, ""},
    {707, 10, 20, 10, 15, 2, 5, 10, 0x21, 0x0001, 0, false, false, false, 0, ""},
    {76, 85, 105, 14, 18, 12, 16, 17, 0x28, 0x0004, 0x0080, false, false, false, 0, ""},
    {808, 15, 25, 8, 12, 4, 6, 5, 0x24, 0x0001, 0, false, false, false, 0, ""},
    {824, 18, 22, 15, 23, 3, 5, 3, 0x24, 0x0002, 0, false, false, false, 0, ""},
    {835, 15, 25, 10, 15, 2, 5, 10, 0x21, 0x0001, 0, false, false, false, 0, ""},
    {836, 15, 25, 10, 15, 2, 5, 10, 0x21, 0x0001, 0, false, false, false, 0, ""},
    {708, 7, 14, 10, 15, 1, 1, 5, 0x21, 0x0001, 0, false, false, false, 0, ""},
    {200, 17, 24, 12, 16, 5, 10, 12, 0x24, 0x0001, 0, false, false, false, 0, "potion;sorcreag1;sorcreag2"},
    {187, 17, 24, 12, 16, 5, 10, 12, 0x24, 0x0001, 0, false, false, false, 0, "potion;sorcreag1;sorcreag2"},
    {269, 17, 24, 12, 16, 5, 10, 12, 0x24, 0x0001, 0, false, false, false, 0, "potion;obsidian*5-10"},
    {377, 40, 60, 15, 22, 6, 12, 5, 0x24, 0x0001, 0x0020, false, false, false, 0, ""},
    {1, 1, 1, 5, 10, 7, 15, 15, 0x24, 0x0001, 0, false, false, false, 0, ""},
    {401, 1, 1, 5, 10, 6, 10, 13, 0x21, 0x0002, 0, false, false, false, 0, ""},
    {144, 1, 1, 5, 10, 9, 15, 10, 0x21, 0x0002, 0, false, false, false, 0, ""},
    {405, 1, 1, 5, 10, 6, 10, 13, 0x21, 0x0002, 0, false, false, false, 0, ""},
};

// Armour pieces (combat.md 5): shape, frame, armour, defence type, kick bonus.
struct ArmourInfo { uint16_t shape, frame; uint8_t ac; uint16_t def; uint8_t kick; };
const ArmourInfo kArmour[] = {
    {531, 0, 4, 0, 0}, {531, 2, 2, 0, 0}, {531, 4, 2, 0, 0}, {531, 6, 3, 0, 0}, {531, 8, 3, 0, 0},
    {523, 0, 4, 0, 0}, {523, 2, 3, 0, 0}, {523, 4, 2, 0, 0}, {530, 0, 2, 0, 0}, {530, 2, 2, 0, 0},
    {530, 4, 1, 0, 0}, {530, 6, 3, 0, 0}, {532, 0, 2, 0, 0}, {532, 2, 3, 0, 0}, {539, 0, 4, 0, 0},
    {539, 2, 3, 0, 0}, {64, 0, 6, 0, 0},  {841, 0, 8, 0, 0}, {842, 0, 5, 0x08, 0}, {843, 0, 5, 0, 0},
    {844, 0, 4, 0, 0}, {845, 0, 4, 0, 3},
};

const ArmourInfo* armour_info(const Item& it) {
    if (it.shape == 823 && it.frame % 2 == 0 && it.frame <= 24) {
        static const ArmourInfo clothes{823, 0, 1, 0, 0};
        return &clothes;
    }
    for (const ArmourInfo& a : kArmour)
        if (a.shape == it.shape && a.frame == it.frame) return &a;
    return nullptr;
}

// Default treasure types (combat.md 7).
struct TreasureType { const char* name; uint16_t shape; uint8_t f0, f1; int chance; int c0, c1; };
const TreasureType kTreasure[] = {
    {"obsidian", 143, 0, 0, 500, 1, 1}, {"potion", 766, 0, 7, 500, 1, 1},   {"firegem", 592, 0, 4, 330, 1, 1},
    {"oilflask", 579, 0, 4, 330, 1, 1}, {"meat", 537, 2, 2, 1000, 1, 1},    {"sorcreag1", 398, 0, 13, 1000, 0, 9},
    {"sorcreag2", 398, 14, 20, 1000, 0, 5}, {"demon1", 398, 18, 18, 800, 1, 1}, {"demon2", 398, 19, 19, 600, 1, 1},
    {"demon3", 398, 20, 20, 750, 0, 2},
};

int roll(World& w, int lo, int hi) { return hi <= lo ? lo : lo + w.rand_below(hi - lo + 1); }

}  // namespace

const WeaponInfo* weapon_info(unsigned shape) {
    for (const WeaponInfo& wi : kWeapons)
        if (wi.shape == shape) return &wi;
    return nullptr;
}

std::vector<MonsterInfo>& extra_monster_rows() {
    static std::vector<MonsterInfo> rows;
    return rows;
}

const MonsterInfo* monster_info(unsigned shape) {
    for (const MonsterInfo& m : kMonsters)
        if (m.shape == shape) return &m;
    for (const MonsterInfo& m : extra_monster_rows())  // the new creatures (game/creatures.h)
        if (m.shape == shape) return &m;
    return nullptr;
}

bool is_enemy(const Actor& a, const Actor& b) { return (a.enemy_alignment & b.alignment) != 0; }

uint16_t World::equipped(uint16_t id, unsigned slot) const {
    const Actor* a = actor(id);
    if (!a) return 0;
    for (uint16_t k : a->contents)
        if (const Item* it = item(k); it && (it->flags & kItemEquipped)) {
            if (slot == 7 && it->shape == 529) return k;
            if (slot < 7 && data_.typeflags.get(it->shape).equip_type() == slot) return k;
        }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Attacker and defender values (combat.md 3)

namespace {
// Companions (game/party.h) fight with their equipment by the Avatar's rules.
bool uses_gear(const World& w, const Actor& a) { return a.id == 1 || (w.party && w.party->is_companion(a.id)); }

const WeaponInfo* avatar_weapon(const World& w, const Actor& a) {
    if (!uses_gear(w, a)) return nullptr;
    const Item* wp = w.item(w.equipped(a.id, 6));
    return wp ? weapon_info(wp->shape) : nullptr;
}
}  // namespace

int attack_damage(const World& cw, const Actor& a) {
    World& w = const_cast<World&>(cw);
    if (uses_gear(w, a)) {
        if (a.last_anim == 58) {  // kick
            int bonus = 0;
            if (const Item* legs = w.item(w.equipped(1, 5)))
                if (const ArmourInfo* ai = armour_info(*legs)) bonus = ai->kick;
            return roll(w, 0, a.str / 2) + bonus;
        }
        if (const WeaponInfo* wi = avatar_weapon(w, a)) {
            const int d = roll(w, 0, wi->mod) + wi->base + a.str / 5;
            return w.leveling ? w.leveling->weapon_damage(a, d) : d;  // Leveling: a heavy weapon below its STR
        }
        return roll(w, 0, a.str / 2) + 1;
    }
    // Leveling (stage 3): the monster's level variant, and STR / 5 for every attacker, as the
    // Avatar's weapon blows have always had (monsters' STR was 0 in the original).
    const bool lv = w.leveling && w.leveling->active();
    if (const MonsterVariant* v = lv ? w.leveling->monster_variant(a) : nullptr) return roll(w, v->dmg0, v->dmg1) + a.str / 5;
    if (const MonsterInfo* m = monster_info(a.shape)) return roll(w, m->dmg0, m->dmg1) + (lv ? a.str / 5 : 0);
    return 1 + (lv ? std::max(0, int(a.str)) / 5 : 0);
}

uint16_t attack_type(const World& w, const Actor& a) {
    if (uses_gear(w, a)) {
        if (const WeaponInfo* wi = avatar_weapon(w, a)) return wi->type;
        return kDmgNormal;
    }
    if (const MonsterInfo* m = monster_info(a.shape)) return m->dmg_type;
    return kDmgNormal;
}

int item_armour(const Item& it) {
    if (const ArmourInfo* ai = armour_info(it)) return ai->ac;
    if (const WeaponInfo* wi = weapon_info(it.shape)) return wi->armour;
    return 0;
}

int armour_class(const World& w, const Actor& a) {
    if (uses_gear(w, a)) {
        int ac = 0;
        for (unsigned s = 1; s <= 5; ++s)
            if (const Item* it = w.item(w.equipped(a.id, s)))
                if (const ArmourInfo* ai = armour_info(*it)) ac += ai->ac;
        if (const WeaponInfo* wi = avatar_weapon(w, a)) ac += wi->armour;
        return ac;
    }
    if (const MonsterInfo* m = monster_info(a.shape)) return m->ac;
    return 0;
}

uint16_t defence_type(const World& w, const Actor& a) {
    if (uses_gear(w, a)) {
        uint16_t d = 0;
        for (unsigned s = 1; s <= 5; ++s)
            if (const Item* it = w.item(w.equipped(a.id, s)))
                if (const ArmourInfo* ai = armour_info(*it)) d |= ai->def;
        return d;
    }
    if (const MonsterInfo* m = monster_info(a.shape)) return m->def_type;
    return 0;
}

int attack_dex(const World& w, const Actor& a) {
    int d = a.dex;
    if (const WeaponInfo* wi = avatar_weapon(w, a)) d += wi->atk_dex;
    return d;
}

int defend_dex(const World& w, const Actor& a) {
    int d = a.dex;
    if (const WeaponInfo* wi = avatar_weapon(w, a)) d += wi->def_dex;
    return std::max(1, d);
}

int calculate_damage(World& w, int d, uint16_t t, Actor& target, Actor* attacker) {
    uint16_t f = defence_type(w, target);
    // 1. Strip; 2. magic immunity.
    t &= uint16_t(~(f & ~(kDmgUndead | kDmgMagic | kDmgPierce)));
    if ((f & kDmgMagic) && !(t & kDmgMagic)) d = 0;
    bool slayer_kill = false;
    // 3. Specials.
    if (d && t) {
        if ((t & kDmgSlayer) && w.rand_below(10) == 0) { d = 255; slayer_kill = true; }
        if ((t & kDmgUndead) && (f & kDmgUndead)) d *= 2;
        if ((f & kDmgPierce) && !(t & (kDmgBlade | kDmgFire | kDmgPierce))) d /= 2;
    } else {
        d = 0;
    }
    const bool stunned = target.is(Actor::kStunned);
    // 4. Armour and blocking.
    if (!(t & kDmgPierce) && !slayer_kill && d) {
        if ((target.last_anim == 59 || target.last_anim == 60) && !stunned) d -= target.str / 5;
        int acm = 3 * armour_class(w, target);
        if (t & kDmgFire) acm /= 2;
        if (stunned) acm /= 2;
        acm = std::min(acm, 100);
        d = std::max(0, (100 - acm) * d / 100);
    }
    // 5. To-hit.
    if (!(t & kDmgPierce) && attacker && d) {
        int ad = std::max(0, attack_dex(w, *attacker)), dd = defend_dex(w, target);
        bool hits = stunned || w.rand_below(ad + 3) > w.rand_below(dd);
        if (hits && attacker->id == 1) w.train_stat(2, ad > dd ? 2 * (ad - dd) : 2);
        if (!hits) d = 0;
        // Leveling: a blow that would land can still be dodged, by the defender's DEX.
        if (d && !stunned && w.leveling && w.leveling->active() && w.leveling->roll_dodge(target)) {
            d = 0;
            w.leveling->last_dodged = true;
        }
    }
    return d;
}

void World::train_stat(int which, int points) {
    Actor* av = avatar();
    if (!av || which < 0 || which > 2) return;
    if (leveling && leveling->enabled) return;  // Leveling: stats rise by levels, not by use (and past 25)
    int16_t& stat = which == 0 ? av->str : which == 1 ? av->intel : av->dex;
    if (stat >= 25) return;
    int& acc = stat_accumulator[size_t(which)];
    acc += points;
    if (acc >= 650 || rand_below(651 - acc) == 0) {
        ++stat;
        acc = 0;
        sound.play_sfx(0x36);
        PG_LOG("avatar %s rises to %d", which == 0 ? "STR" : which == 1 ? "INT" : "DEX", stat);
    }
}

// ---------------------------------------------------------------------------------------------
// Turning (actors.md 7.9)

uint16_t turn_actor(World& w, uint16_t id, int dir) {
    Actor* a = w.actor(id);
    if (!a || dir < 0) return 0;
    dir &= 7;
    int cur = a->dir;
    if (cur == dir) return 0;
    int cw = (dir - cur + 8) % 8;
    int step = cw <= 4 ? 1 : -1;  // 180 degrees: clockwise
    bool combat = a->is(Actor::kInCombat) && !a->is(Actor::kCombatRun);
    unsigned turn_anim = combat ? 15u : (step > 0 ? 33u : 32u);
    unsigned stand = combat ? 15u : 2u;
    const AnimDat& ad = w.data().anim;
    uint16_t prev = 0;
    auto chain = [&](unsigned action, int d) {
        uint16_t pid = w.do_anim(id, action, d);
        if (pid && prev)
            if (Process* p = w.kernel().get(pid)) p->wait_for(prev);
        if (pid) prev = pid;
    };
    if (!ad.get(a->shape, turn_anim)) {
        chain(stand, dir);
        return prev;
    }
    for (int d = cur; d != dir; d = (d + step + 8) % 8) chain(turn_anim, d);
    chain(stand, dir);
    return prev;
}

// ---------------------------------------------------------------------------------------------
// Being hit (combat.md 2, actors.md 5.1)

void World::receive_hit(uint16_t vid, uint16_t aid, int dir, int damage, uint16_t type) {
    Item* vi = item(vid);
    if (!vi) return;
    dir &= 7;
    Actor* v = vi->as_actor();
    if (!v) {
        // A plain item: gotHit usecode, else explode / break / knock away (physics.md 8).
        if (call_event_ww(vid, kEvGotHit, aid, 0)) return;
        if (!(vi = item(vid))) return;
        const ShapeInfo& si = data_.typeflags.get(vi->shape);
        if (si.is(ShapeInfo::kExplode)) explode(vid, true, true);
        else if (si.family() == kFamBreakable) destroy(vid);
        else if (!si.is(ShapeInfo::kFixed) && si.weight() > 0) hurl(vid, -16 * dir_x(dir), -16 * dir_y(dir), 16, 4);
        return;
    }
    if (v->is(Actor::kDead)) return;
    if (aid && same_party(vid, aid)) return;  // companions: no harm between party members
    if (party && party->untouchable(vid)) return;  // nobody can hurt a companion before they are recruited
    Actor* att = actor(aid);
    const bool weapon_blow = damage == 0 && type == 0 && att;  // the new creatures' rules (game/creatures.h)
    if (damage == 0 && att) damage = attack_damage(*this, *att);
    if (type == 0) type = att ? attack_type(*this, *att) : kDmgNormal;
    const int raw_damage = damage;  // the HUD's combat log (game/game_log.h)
    if (aid == 1 && att && att->last_anim != 58) train_stat(0, damage / 4);
    // No gotHit usecode for actors on this path (combat.md 2: their handlers would misfire).
    if (leveling) leveling->last_dodged = false;
    damage = calculate_damage(*this, damage, type, *v, att);
    const bool dodged = leveling && leveling->last_dodged;
    if (creatures) damage = creatures->on_hit(vid, aid, damage, type, weapon_blow);  // immunities, weaknesses, mana drain
    // A hostile's blow on the party in the middle of a conversation ends the conversation.
    if (aid && att && (vid == 1 || same_party(1, vid)) && !same_party(1, aid)) combat_interrupts_conversation(aid, "attacked");
    if (turn) damage = turn->on_hit(vid, aid, damage);  // turn-based combat: Defend, starting a fight
    if (party && party->enabled) party->on_hit(vid, aid, damage);
    game_log.on_hit(*this, vid, aid, raw_damage, damage, v->last_anim == 59 || v->last_anim == 60,
                    damage > 0 && damage >= int(v->hp) && !v->is(Actor::kImmortal) && !v->is(Actor::kInvincible) &&
                        !v->is(Actor::kWithstandDeath),
                    dodged);  // the HUD's combat log
    PG_LOG("hit: %u -> %u dir %d type %04X: %d damage (hp %u)", aid, vid, dir, type, damage, v->hp);
    // Blood on the Avatar (the sprite).
    if (vid == 1 && aid && damage >= 4) {
        int first = (dir >= 3 && dir <= 7) ? 13 : 0;
        kernel_.add(std::make_unique<SpriteProcess>(*this, 620, first, first + 12, 1, 1, v->x, v->y, v->z + rand_below(24)));
    }
    if (damage > 0 && !v->is(Actor::kImmortal) && !v->is(Actor::kInvincible)) {
        if (damage >= int(v->hp)) {
            if (v->is(Actor::kWithstandDeath)) {
                v->hp = uint16_t(std::max(1, 2 * int(v->str)));
                v->set(Actor::kWithstandDeath, false);
                sound.play_sfx(59, SoundSystem::kDefaultPriority, vid);
            } else {
                if (leveling) leveling->on_kill(vid, aid);  // XP, before the death
                kill_actor(vid, type);
            }
            return;
        }
        v->hp = uint16_t(v->hp - damage);
    }
    bool fell = false;
    if (vid == 1 && damage > 0) {
        if ((type & kDmgFalling) && damage >= 6) {
            kernel_.kill(1, kProcActorAnim);
            v->set(Actor::kAnimLock, false);
            do_anim(1, 44, (dir + 4) & 7);
            v->set(Actor::kStunned, true);
            return;
        }
        // Cancel the current activity except a running die / stand-up.
        bool keep = false;
        if (auto* p = dynamic_cast<ActorAnimProcess*>(kernel_.find(1, kProcActorAnim)))
            keep = p->action() == 14 || p->action() == 4;
        if (!keep) {
            kernel_.kill(1, kProcActorAnim);
            kernel_.kill(1, kProcPathfind);
            if ((v = actor(1))) v->set(Actor::kAnimLock, false);
        }
    }
    if (!(v = actor(vid))) return;
    if (creatures && creatures->is_creature(vid)) return;  // its own brain fights back; no stagger (game/creatures.h)
    // The Avatar blocking: a flinch.
    if (vid == 1 && v->last_anim == 59) {
        kernel_.kill(1, kProcActorAnim);
        v->set(Actor::kAnimLock, false);
        uint16_t p1 = do_anim(1, 60, -1);
        uint16_t p2 = do_anim(1, 59, -1);
        unsigned r = sound.random_pitch();
        sound.play_sfx(damage > 0 ? 50 + int(r & 1) : 20 + int(r % 3), SoundSystem::kDefaultPriority, 1);
        if (p1 && p2)
            if (Process* p = kernel_.get(p2)) p->wait_for(p1);
        return;
    }
    // An NPC fights back (companions have their own AI, game/companion_ai.cpp).
    if (vid != 1 && !(v->ext_flags & kExtCompanion)) {
        uint16_t tgt = att ? aid : 1;
        if (!v->is(Actor::kInCombat)) set_in_combat(vid);
        if ((v = actor(vid))) {
            v->target = tgt;
            if (!v->fixed_target) v->fixed_target = tgt;
        }
    }
    if (!(v = actor(vid))) return;
    // Stagger.
    if (damage > 0 && !fell && v->last_anim != 14 && v->last_anim != 44) {
        kernel_.kill(vid, kProcActorAnim);
        v->set(Actor::kAnimLock, false);
        uint16_t p1 = do_anim(vid, 13, dir);
        uint16_t p2 = do_anim(vid, v->is(Actor::kInCombat) ? 15 : 2, dir);
        if (p1 && p2)
            if (Process* p = kernel_.get(p2)) p->wait_for(p1);
    }
}

// ---------------------------------------------------------------------------------------------
// Death (actors.md 5.4, combat.md 8)

void World::kill_actor(uint16_t id, uint16_t damage_type) {
    Actor* a = actor(id);
    if (!a || a->is(Actor::kDead)) return;
    PG_LOG("actor %u (%s, shape %u) dies", id, a->name.c_str(), a->shape);
    game_log.on_death(*this, id);  // the HUD's combat log
    bool hostile_to_avatar = avatar() && is_enemy(*a, *avatar());
    a->hp = 0;
    a->set(Actor::kDead, true);
    a->flags |= 0x8000;  // Broken
    a->set(Actor::kInCombat, false);
    // Kill everything but a running die animation.
    bool dying = false;
    std::vector<uint16_t> kill;
    kernel_.for_each([&](Process* p) {
        if (p->item != id || !p->live()) return;
        auto* ap = dynamic_cast<ActorAnimProcess*>(p);
        if (ap && ap->action() == 14) { dying = true; return; }
        kill.push_back(p->pid());
    });
    for (uint16_t pid : kill)
        if (Process* p = kernel_.get(pid)) p->fail();
    if (!(a = actor(id))) return;
    a->set(Actor::kAnimLock, dying);
    if (!dying && a->last_anim != 14 && a->last_anim != 44 && !(creatures && creatures->is_creature(id))) do_anim(id, 14, -1);
    if (id == 1) {
        stasis = true;
        sound.music_death();
        if (!death_pid) death_pid = kernel_.add(std::make_unique<AvatarDeathProcess>(*this));
        return;
    }
    if (hostile_to_avatar && avatar() && avatar()->is(Actor::kInCombat)) sound.combat_fanfare();
    if (a->flags & kItemFastOnly) destroy_contents(id);
    // Treasure. (Companions borrow monster shapes, but keep only their own things and do not rise.)
    const MonsterInfo* m = (a->ext_flags & kExtCompanion) ? nullptr : monster_info(a->shape);
    if (m && m->treasure[0]) {
        std::string list = m->treasure;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t end = list.find(';', pos);
            if (end == std::string::npos) end = list.size();
            std::string spec = list.substr(pos, end - pos);
            pos = end + 1;
            if (spec.empty()) continue;
            int c0 = -1, c1 = -1;
            size_t star = spec.find('*');
            std::string name = spec.substr(0, star);
            if (star != std::string::npos) std::sscanf(spec.c_str() + star + 1, "%d-%d", &c0, &c1);
            if (name == "weapon") {
                for (const WeaponInfo& wi : kWeapons)
                    if (wi.treasure && rand_below(100) < wi.treasure) {
                        uint16_t t = create_item(wi.shape, 0);
                        if (Item* ti = item(t)) { ti->flags |= kItemDisposable; ti->quality = 1; }
                        move_to_container(t, id);
                        break;
                    }
                continue;
            }
            for (const TreasureType& tt : kTreasure) {
                if (name != tt.name) continue;
                if (tt.chance < 1000 && rand_below(1001) > tt.chance) break;
                int lo = c0 >= 0 ? c0 : tt.c0, hi = c1 >= 0 ? c1 : tt.c1;
                int count = roll(*this, lo, hi);
                if (count <= 0) break;
                unsigned fam = data_.typeflags.get(tt.shape).family();
                if (fam == kFamQuantity || fam == kFamReagent) {
                    uint16_t t = create_item(tt.shape, tt.f0);
                    if (Item* ti = item(t)) { ti->flags |= kItemDisposable; ti->quality = uint16_t(count); }
                    move_to_container(t, id);
                    call_event(t, kEvCombine);
                } else {
                    for (int k = 0; k < count; ++k) {
                        unsigned fr = tt.f0 == tt.f1 ? tt.f0 : (tt.f1 == 4 ? (rand_below(2) ? 4u : 0u) : unsigned(roll(*this, tt.f0, tt.f1)));
                        uint16_t t = create_item(tt.shape, fr);
                        if (Item* ti = item(t)) ti->flags |= kItemDisposable;
                        move_to_container(t, id);
                    }
                }
                break;
            }
        }
    }
    // Resurrection (skeletons) and exploding (seekers).
    if (m && m->resurrect && !(damage_type & kDmgFire))
        kernel_.add(std::make_unique<ResurrectProcess>(*this, id, 30 * roll(*this, 5, 30)));
    if (m && m->explode) {
        for (int k = 0; k < 5; ++k) {
            const Shape* sh = data_.shapes.shape(m->explode);
            unsigned frames = sh ? sh->frame_count() : 1;
            uint16_t p = create_item(m->explode, unsigned(rand_below(int(std::max(1u, frames)))));
            if (Item* pi = item(p)) pi->flags |= kItemFastOnly;
            move_to(p, a->x + 32 * roll(*this, -4, 4), a->y + 32 * roll(*this, -4, 4), a->z + rand_below(8), false);
            hurl(p, roll(*this, -25, 25), roll(*this, -25, 25), roll(*this, 10, 20), 4);
            if (!(a = actor(id))) return;
        }
    }
}

void World::explode(uint16_t id, bool destroy_item, bool damage) {
    Item* it = item(id);
    if (!it) return;
    int x, y, z;
    location(*it, x, y, z);
    kernel_.add(std::make_unique<SpriteProcess>(*this, 578, 20, 34, 1, 1, x, y, z));
    sound.play_sfx((sound.random_pitch() & 1) ? 31 : 158, SoundSystem::kDefaultPriority, id);
    if (destroy_item) destroy(id);
    if (!damage) return;
    std::vector<uint16_t> near;
    area_search(x, y, 160, false, near);
    for (uint16_t k : near) {
        if (k == id) continue;
        const Item* o = item(k);
        if (!o || o->parent) continue;
        if (std::abs(o->z - z) > 160) continue;
        int d = direction_to(x - o->x, y - o->y);  // pointing back at the blast
        receive_hit(k, 0, d, 6 + rand_below(6), kDmgBlunt | kDmgFire);
    }
}

// ---------------------------------------------------------------------------------------------
// The NPC combat AI (actors.md 9.2)

bool CombatProcess::valid_target(uint16_t t) const {
    const Actor* a = w_.actor(item);
    const Actor* o = w_.actor(t);
    if (!a || !o || t == item || o->is(Actor::kDead)) return false;
    if (w_.party && w_.party->untouchable(t)) return false;  // monsters ignore a companion not yet recruited
    if (w_.fast_ready() && !(o->flags & kItemFastArea)) return false;
    if (o->is(Actor::kFeignDeath)) {
        const MonsterInfo* m = monster_info(a->shape);
        if ((m && (m->def_type & kDmgUndead)) || a->shape == 96) return false;
    }
    return true;
}

uint16_t CombatProcess::find_target() const {
    const Actor* a = w_.actor(item);
    if (!a) return 0;
    if (a->fixed_target && valid_target(a->fixed_target)) return a->fixed_target;
    std::vector<uint16_t> near;
    w_.area_search(a->x, a->y, 768, false, near);
    for (uint16_t k : near) {
        const Actor* o = w_.actor(k);
        if (o && valid_target(k) && is_enemy(*a, *o)) return k;
    }
    return 0;
}

bool CombatProcess::in_range(uint16_t t) const {
    const Actor* a = w_.actor(item);
    if (!a) return false;
    if (const MonsterInfo* m = monster_info(a->shape); m && m->ranged) return true;
    AnimTracker tr(w_);
    if (!tr.init(item, 7, a->dir)) return false;
    while (tr.step())
        if (tr.hit()) return tr.hit() == t;
    return false;
}

void CombatProcess::run() {
    Actor* a = w_.actor(item);
    if (!a || a->is(Actor::kDead) || !a->is(Actor::kInCombat)) { terminate(); return; }
    if (w_.fast_ready() && !(a->flags & kItemFastArea)) return;
    if (a->is(Actor::kAnimLock) || w_.kernel().count_exact(item, kProcActorAnim)) return;
    if (delay_ > 0) { --delay_; return; }
    // Companions: a party member clearly nearer than the target draws the attack (game/party.h).
    if (w_.party && w_.party->enabled)
        if (uint16_t t = w_.party->monster_retarget(item, a->target); t && valid_target(t)) a->target = t;
    // 1. Target.
    if (!valid_target(a->target)) {
        a->target = find_target();
        if (!a->target) {
            // Nobody to fight: wander one cycle and look again.
            mode_ = kSearching;
            uint16_t lp = w_.kernel().add(std::make_unique<LoiterProcess>(w_, item, 1));
            wait_for(lp);
            return;
        }
        mode_ = kWaiting;
    }
    const Actor* t = w_.actor(a->target);
    Box tb = w_.box_of(*t), ab = w_.box_of(*a);
    // 2. Face it.
    int dir = direction_to((tb.x - tb.xd / 2) - (ab.x - ab.xd / 2), (tb.y - tb.yd / 2) - (ab.y - ab.yd / 2));
    if (a->dir != dir) {
        if (uint16_t p = turn_actor(w_, item, dir)) { wait_for(p); return; }
        a->dir = uint8_t(dir);
    }
    // 3. Attack if in range, else path there.
    if (in_range(a->target)) {
        const AnimDat& ad = w_.data().anim;
        bool idle1 = ad.get(a->shape, 47), idle2 = ad.get(a->shape, 48);
        unsigned action = 7;
        if ((idle1 || idle2) && w_.rand_below(5) == 0) action = idle1 && idle2 ? 47u + unsigned(w_.rand_below(2)) : idle1 ? 47u : 48u;
        uint16_t p = w_.do_anim(item, action, dir);
        if (a->dex < 25) delay_ = 3 * (25 - a->dex);
        mode_ = kWaiting;
        if (p) wait_for(p);
        return;
    }
    if (mode_ != kPathing) {
        mode_ = kPathing;
        uint16_t p = w_.kernel().add(std::make_unique<PathfindProcess>(w_, item, 0, 0, 0, a->target, true));
        wait_for(p);
        return;
    }
    mode_ = kWaiting;  // pathing did not get us in range: wait a moment and try again
    delay_ = 15 + w_.rand_below(30);
}

// ---------------------------------------------------------------------------------------------
// The Avatar's death (avatar.md 11) and resurrection (actors.md 5.4)

void AvatarDeathProcess::run() {
    switch (stage_) {
    case 0:  // wait for the die animation
        if (w_.kernel().count_exact(1, kProcActorAnim)) return;
        stage_ = 1;
        wait_for(kernel()->add(std::make_unique<DelayProcess>(150)));
        return;
    case 1: {
        stage_ = 2;
        w_.palette = w_.palette_target = PaletteTransform{};
        uint16_t r = w_.open_reader(ReaderGump::kGrave, 0, 27, "HERE LIES*THE AVATAR*REST IN PEACE");
        if (r) wait_for(r);
        return;
    }
    default:
        w_.sound.music_stop();
        w_.game_over = true;
        terminate();
        return;
    }
}

void ResurrectProcess::run() {
    if (--left_ > 0) return;
    Actor* a = w_.actor(item);
    if (a && a->is(Actor::kDead)) {
        a->flags &= uint16_t(~(kItemGumpOpen | 0x8000));
        a->set(Actor::kDead, false);
        a->set(Actor::kWithstandDeath, false);
        if (const MonsterInfo* m = monster_info(a->shape)) {
            a->hp = uint16_t(m->hp0 + w_.rand_below(m->hp1 - m->hp0 + 1));
            a->dex = int16_t(m->dex0 + w_.rand_below(m->dex1 - m->dex0 + 1));
        }
        if (w_.leveling) w_.leveling->on_monster_revived(*a);  // Leveling: its own variant's HP and DEX
        w_.set_in_combat(item);
        w_.do_anim(item, 4, -1);
    }
    terminate();
}

}  // namespace pg
