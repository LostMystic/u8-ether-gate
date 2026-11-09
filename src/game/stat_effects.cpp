// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Leveling stage 3: what STR, DEX and INT do, and the leveled monsters (PLAN-LEVELING.md 5 and 6;
// NOTES.md "Stat effects and monster tuning"). Every rule here is behind Leveling::active(): with
// the option off the callers get the original's numbers back.
#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/world.h"

namespace pg {

namespace {
const int kFloatFrames = 60;
int roll(World& w, int lo, int hi) { return hi <= lo ? lo : lo + w.rand_below(hi - lo + 1); }
bool uses_gear(const World& w, const Actor& a) { return a.id == 1 || (w.party && w.party->is_companion(a.id)); }
const char* heavy_name(unsigned shape) { return shape == 540 ? "hammer" : shape == 419 ? "axe" : shape == 542 ? "sabre" : "weapon"; }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Monster variants

const MonsterVariant* MonsterLevel::variant(int lv) const {
    if (variants.empty()) return nullptr;
    const MonsterVariant* best = &variants.front();
    for (const MonsterVariant& v : variants)
        if (v.level <= lv) best = &v;
    return best;
}

int MonsterLevel::level_on_map(unsigned map) const {
    auto it = map_level.find(map);
    return it == map_level.end() ? level : it->second;
}

int Leveling::monster_level(const Actor& a) const {
    if (w_.party && w_.party->is_companion(a.id)) return 0;
    auto m = monsters.find(a.shape);
    if (m == monsters.end()) return 0;
    if (int stored = (a.ext_flags & kExtLevelMask) >> kExtLevelShift) return stored;
    return m->second.level_on_map(a.mapnum);
}

std::string Leveling::weak_prefix(const Actor& a) const {
    if (!active() || (a.ext_flags & 0x0040)) return std::string();  // (companions wear monster shapes)
    auto m = monsters.find(a.shape);
    if (m == monsters.end() || m->second.weak_prefix.empty()) return std::string();
    const int lv = monster_level(a);
    return lv && lv < m->second.level ? m->second.weak_prefix + " " : std::string();
}

const MonsterVariant* Leveling::monster_variant(const Actor& a) const {
    if (!active()) return nullptr;
    int lv = monster_level(a);
    if (!lv) return nullptr;
    const MonsterVariant* v = monsters.at(a.shape).variant(lv);
    return v && v->has_stats ? v : nullptr;
}

void Leveling::apply_monster(Actor& a, int level) {
    auto m = monsters.find(a.shape);
    if (m == monsters.end()) return;
    const MonsterVariant* v = m->second.variant(level);
    if (!v) return;
    a.ext_flags = uint16_t((a.ext_flags & ~kExtLevelMask) | ((v->level << kExtLevelShift) & kExtLevelMask));
    if (!v->has_stats) return;
    a.hp = uint16_t(roll(w_, v->hp0, v->hp1));
    a.dex = int16_t(roll(w_, v->dex0, v->dex1));
    a.str = int16_t(v->str);
    a.intel = int16_t(v->intel);
}

void Leveling::on_monster_created(uint16_t id) {
    if (!active()) return;
    Actor* a = w_.actor(id);
    if (!a) return;
    auto m = monsters.find(a->shape);
    if (m == monsters.end()) return;
    apply_monster(*a, m->second.level_on_map(a->mapnum));
}

void Leveling::on_monster_revived(Actor& a) {
    if (const MonsterVariant* v = monster_variant(a)) {
        a.hp = uint16_t(roll(w_, v->hp0, v->hp1));
        a.dex = int16_t(roll(w_, v->dex0, v->dex1));
    }
}

// ---------------------------------------------------------------------------------------------
// INT

int Leveling::int_scaled(int value, int intel) {
    const int k = std::max(20, 10 + intel);  // (1 + (INT - 10) / 20) in twentieths, never below 1
    return (value * k + 10) / 20;
}

uint16_t Leveling::usecode_spell_caster(const World& w, uint16_t start_cls, uint16_t start_addr, const uint8_t* stack,
                                        unsigned stack_size) {
    // 1411:233D is spawned with a 6-byte argument block and no this: the caster, the bolt, the kind
    // (leveling_facts.md 2.1). Traps pass themselves (an item, not an actor) and are left alone.
    if (start_cls != 1411 || start_addr != 0x233D || stack_size < 6 || !compat::class_known(1411)) return 0;
    const uint16_t caster = uint16_t(stack[stack_size - 6] | (stack[stack_size - 5] << 8));
    const Actor* a = w.actor(caster);
    return a ? caster : 0;
}

// ---------------------------------------------------------------------------------------------
// STR: heavy weapons

int Leveling::heavy_min_str(unsigned shape) {
    for (const HeavyWeapon& h : kHeavyWeapons)
        if (h.shape == shape) return h.min_str;
    return 0;
}

int Leveling::heavy_short(const Actor& a) const {
    if (!active() || !uses_gear(w_, a)) return 0;
    const Item* wp = w_.item(w_.equipped(a.id, 6));
    if (!wp) return 0;
    return std::max(0, heavy_min_str(wp->shape) - int(a.str));
}

int Leveling::weapon_damage(const Actor& a, int damage) const {
    int s = heavy_short(a);
    if (!s) return damage;
    return damage * (100 - std::min(kHeavyDamageMaxLossPct, kHeavyDamageLossPct * s)) / 100;
}

int Leveling::attack_delay_extra(const Actor& a) const { return std::min(10, kHeavyDelayFrames * heavy_short(a)); }

void Leveling::on_equip(uint16_t actor, uint16_t item) {
    if (!active()) return;
    const Item* it = w_.item(item);
    const Actor* a = w_.actor(actor);
    if (!it || !a || !uses_gear(w_, *a)) return;
    const int need = heavy_min_str(it->shape);
    if (!need || a->str >= need) return;
    char buf[160];
    std::snprintf(buf, sizeof buf, "The %s is heavy for %s (STR %d needed): weaker and slower blows.", heavy_name(it->shape),
                  GameLog::name_of(w_, actor).c_str(), need);
    w_.game_log.add(GameLog::kMessages, GameLog::kInfo, buf, actor);
    float_text(actor, "Too heavy");
    log.push_back("heavy " + std::to_string(it->shape));
}

// ---------------------------------------------------------------------------------------------
// Animation speed (attacks past DEX 25, heavy swings, the Avatar's climbing)

int Leveling::anim_speed(const Actor& a, unsigned action, bool attack_seq) const {
    if (!active()) return 100;
    int pct = 100;
    if (attack_seq) {
        if (a.dex > kDexFastFrom) pct += std::min(kDexFastMaxPct, kDexFastPct * (a.dex - kDexFastFrom));
        pct -= std::min(kHeavySlowMaxPct, kHeavySlowPct * heavy_short(a));
    }
    if (a.id == 1 && action >= 19 && action <= 22 && a.str > kClimbFastFrom)  // climb 16 .. climb 40
        pct += std::min(kClimbFastMaxPct, kClimbFastPctX2 * (a.str - kClimbFastFrom) / 2);
    return std::max(50, pct);
}

// ---------------------------------------------------------------------------------------------
// DEX: dodge, turn-based Move range, traps

int Leveling::dodge_permille(const Actor& d) const {
    if (!active() || d.dex <= 0) return 0;
    int p = d.dex <= kDodgeKnee ? int(d.dex) : kDodgeKnee + 15 * (d.dex - kDodgeKnee);  // DEX / 10 %, then 1.5% a point
    return std::min(kDodgeMaxPermille, p);
}

bool Leveling::roll_dodge(const Actor& d) {
    int p = dodge_permille(d);
    if (p <= 0 || w_.rand_below(1000) >= p) return false;
    float_text(d.id, "Dodged");
    ++dodges;
    return true;
}

int Leveling::move_bonus(const Actor& a) const {
    if (!active()) return 0;
    return std::min(kMoveBonusMax, kMoveBonusPerDex * std::max(0, int(a.dex) - 10));
}

int Leveling::hp_bonus(uint16_t actor) const {
    if (!active() || !w_.party) return 0;
    int i = w_.party->index_of(actor);
    if (i < 0 || w_.party->defs[size_t(i)].lv_class != "caster") return 0;
    return (level_of(actor) - 1) * kCasterHpPerLevel;
}

void Leveling::float_text(uint16_t actor, const std::string& text) {
    floats.erase(std::remove_if(floats.begin(), floats.end(), [&](const FloatText& f) { return f.actor == actor; }), floats.end());
    floats.push_back({actor, text, kFloatFrames});
}

// Chest traps (leveling_facts.md 7): shape 756 inside a chest, quality high byte 0 = armed. Our
// design: near a chest, the Avatar gets one try per visit of the map to notice its trap, with
// (DEX - 10) x 10 % (so none at the new game's DEX 10, always from DEX 20). Noticed, it is disarmed:
// taken out of the chest, so opening it is safe, with a line in the log and "Trap!" over him.
void Leveling::check_traps() {
    const Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead)) return;
    if (trap_map != av->mapnum) {
        trap_map = av->mapnum;
        trap_checked.clear();
    }
    std::vector<uint16_t> near;
    w_.area_search(av->x, av->y, kTrapNoticeRange, false, near);
    for (uint16_t k : near) {
        const Item* chest = w_.item(k);
        if (!chest || chest->as_actor() || chest->contents.empty() || trap_checked.count(k)) continue;
        if (std::abs(chest->z - av->z) > 96) continue;
        uint16_t trap = 0;
        for (uint16_t c : chest->contents)
            if (const Item* t = w_.item(c); t && t->shape == 756 && (t->quality >> 8) == 0) { trap = c; break; }
        if (!trap) continue;
        trap_checked.insert(k);
        const int pct = std::clamp((int(av->dex) - kTrapDexFloor) * kTrapPctPerDex, 0, 100);
        if (w_.rand_below(100) >= pct) { log.push_back("trap missed " + std::to_string(k)); continue; }
        w_.destroy(trap);
        float_text(1, "Trap!");
        w_.game_log.add(GameLog::kMessages, GameLog::kInfo,
                        GameLog::name_of(w_, 1) + " spots a trap on a chest and disarms it.", 1);
        log.push_back("trap disarmed " + std::to_string(k));
        PG_LOG("leveling: trap in chest %u disarmed (DEX %d)", k, av->dex);
    }
}

}  // namespace pg
