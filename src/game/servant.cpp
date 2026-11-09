// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The fighting Aerial Servant (game/servant.h, NOTES.md "The Aerial Servant").
#include "game/servant.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

#include "core/log.h"
#include "data/compat.h"
#include "game/combat.h"
#include "game/game_log.h"
#include "game/item_powers.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "json/json.hpp"
#include "usecode/uc_machine.h"

namespace pg {
namespace servant {

namespace {

constexpr unsigned kAirSpell = 1430;       // AIRSPEL
constexpr unsigned kAerialServant = 0x1093;  // its Aerial Servant routine (magic.md 5)
constexpr uint16_t kProcServant = 0x26A;   // engine-only process type (not a usecode one)

// The servant's own animation (MINIAIR's while the spell lasts): frames 13 to 24 cycling, then
// 25 to 34 as it runs out.
constexpr int kLoopFirst = 13, kLoopLast = 24, kFadeLast = 34;

class ServantProcess : public Process {
public:
    ServantProcess(World& w, uint16_t target) : w_(w), target_(target) {
        type = kProcServant;
        prevents_saving = true;  // twenty seconds at most: not worth a save format
    }
    const char* name() const override { return "aerial servant"; }
    void run() override;
    void on_terminate() override {
        if (servant_ && w_.item(servant_)) w_.destroy(servant_);
        servant_ = 0;
    }

private:
    enum Stage { kAppear, kFly, kFight, kVanish } stage_ = kAppear;
    bool target_ok() const {
        const Actor* t = w_.actor(target_);
        return t && !t->is(Actor::kDead) && t->chunk >= 0;
    }
    // Where it hovers while fighting: beside the creature, on the Avatar's side of it.
    void hover_point(int& x, int& y, int& z) const;
    void animate();
    bool fly_to(int x, int y, int z);  // one step; true when there
    void vanish();
    World& w_;
    uint16_t target_ = 0, servant_ = 0;
    int wait_ = 0, timer_ = 0, strikes_ = 0, frame_ = kLoopFirst, hp_seen_ = 0;
};

void ServantProcess::hover_point(int& x, int& y, int& z) const {
    const Actor* t = w_.actor(target_);
    const Actor* av = w_.avatar();
    Box tb = w_.box_of(*t);
    int sxd = 64, syd = 64, szd = 24;
    if (const Item* s = w_.item(servant_)) w_.footprint(*s, sxd, syd, szd);
    // From the creature's centre toward the Avatar, just outside its footprint.
    int cx = tb.x - tb.xd / 2, cy = tb.y - tb.yd / 2;
    int dx = av ? (av->x - 32) - cx : 1, dy = av ? (av->y - 32) - cy : 0;
    int d = direction_to(dx, dy);
    int rx = tb.xd / 2 + sxd / 2 - 8, ry = tb.yd / 2 + syd / 2 - 8;
    x = cx + dir_x(d) * rx + sxd / 2;
    y = cy + dir_y(d) * ry + syd / 2;
    z = tb.z + params().hover;
}

void ServantProcess::animate() {
    Item* s = w_.item(servant_);
    if (!s) return;
    if (stage_ == kVanish) { if (frame_ < kFadeLast) ++frame_; }
    else if (++frame_ > kLoopLast) frame_ = kLoopFirst;
    s->frame = uint16_t(frame_);
}

bool ServantProcess::fly_to(int x, int y, int z) {
    Item* s = w_.item(servant_);
    if (!s) return true;
    int dx = x - s->x, dy = y - s->y, dz = z - s->z;
    int far = std::max(std::abs(dx), std::abs(dy));
    const int sp = std::max(1, params().speed);
    int nx = x, ny = y;
    if (far > sp) { nx = s->x + dx * sp / far; ny = s->y + dy * sp / far; }
    int step_z = std::abs(dz) / 20 + 1;  // z follows as the original servant's flight does
    int nz = std::abs(dz) <= step_z ? z : s->z + (dz > 0 ? step_z : -step_z);
    w_.move_to(servant_, nx, ny, std::clamp(nz, 0, 255));
    return far <= sp && std::abs(z - nz) <= 1;
}

void ServantProcess::vanish() {
    if (stage_ == kVanish) return;
    stage_ = kVanish;
    if (Item* s = w_.item(servant_)) {
        w_.sound.play_sfx(params().sfx_vanish, SoundSystem::kDefaultPriority, servant_);
        // The puff: a sparkle where it was.
        w_.kernel().add(std::make_unique<SpriteProcess>(w_, 587, 0, 0, 1, 20, s->x, s->y, s->z + 10));
        if (Actor* a = s->as_actor()) a->set(Actor::kInvincible, true);  // no more blows while it fades
    }
    frame_ = std::max(frame_, kLoopLast);
    ++stats().dispelled;
}

void ServantProcess::run() {
    const Params& P = params();
    Actor* av = w_.avatar();
    if (!av) { terminate(); return; }
    Actor* s = servant_ ? w_.actor(servant_) : nullptr;
    if (stage_ != kAppear && !s) { terminate(); return; }
    if (s && stage_ != kVanish) {
        // Its blows taken (any HP lost); dead = dispelled.
        if (int(s->hp) < hp_seen_) { ++stats().hits_taken; hp_seen_ = s->hp; }
        if (s->is(Actor::kDead)) {
            PG_LOG("aerial servant: dispelled by a blow");
            s->set(Actor::kDead, false);  // (it is not a body: it fades and goes)
            s->hp = 0;
            vanish();
        }
    }
    animate();
    switch (stage_) {
    case kAppear: {
        // Released from the Avatar's hands (animation 29) at his head, as the original servant.
        w_.do_anim(1, 29, -1);
        w_.sound.play_sfx(P.sfx_appear, SoundSystem::kDefaultPriority, 1);
        servant_ = w_.create_monster(P.shape, unsigned(kLoopFirst));
        Actor* n = w_.actor(servant_);
        if (!n) { terminate(); return; }
        n->name = P.name;
        n->hp = uint16_t(std::max(1, P.hp));
        n->str = int16_t(std::max(1, P.hp / 2));
        n->dex = int16_t(P.dex);
        n->intel = 1;
        n->alignment = av->alignment;  // on the Avatar's side
        n->enemy_alignment = av->enemy_alignment;
        n->dir = av->dir;
        hp_seen_ = n->hp;
        w_.move_to(servant_, av->x, av->y, std::min(255, av->z + 45));
        ++stats().summoned;
        PG_LOG("aerial servant: summoned (actor %u) against %u", servant_, target_);
        stage_ = kFly;
        return;
    }
    case kFly: {
        if (!target_ok()) { vanish(); return; }
        int x, y, z;
        hover_point(x, y, z);
        if (fly_to(x, y, z)) {
            stage_ = kFight;
            wait_ = P.first_strike;
            timer_ = P.duration;
            // The Avatar lets it go (the spell's common ending: animation 28, sound 116).
            w_.do_anim(1, 28, -1);
        }
        return;
    }
    case kFight: {
        if (!target_ok() || strikes_ >= P.strikes || --timer_ <= 0) { vanish(); return; }
        int x, y, z;
        hover_point(x, y, z);
        fly_to(x, y, z);  // keeps beside it as it moves
        if (--wait_ > 0) return;
        wait_ = P.strike_interval;
        Actor* t = w_.actor(target_);
        int dir = direction_to(t->x - s->x, t->y - s->y);
        s->dir = uint8_t(dir);
        int dmg = std::max(1, P.damage_min + (P.damage_max > P.damage_min ? w_.rand_below(P.damage_max - P.damage_min + 1) : 0));
        unsigned hp0 = t->hp;
        w_.sound.play_sfx(P.sfx_strike, SoundSystem::kDefaultPriority, servant_);
        // A flash where the gust lands (the spell's yellow sparkle, 587 frame 20).
        w_.kernel().add(std::make_unique<SpriteProcess>(w_, 587, 20, 20, 1, 8, t->x - 16, t->y - 16, t->z + 24));
        // Wind: its blows always land and armour does not stop them (pierce); magic, so the
        // creatures only magic can hurt feel it too. A killing blow goes the usual way
        // (receive_hit: death, XP, treasure); a lesser one only stings: no stagger, so the
        // creature is free to turn on it and strike back.
        const uint16_t type = uint16_t(kDmgMagic | kDmgPierce);
        int real = calculate_damage(w_, dmg, type, *t, s);
        if (t->is(Actor::kImmortal) || t->is(Actor::kInvincible)) real = 0;
        if (real >= int(t->hp)) {
            w_.receive_hit(target_, servant_, dir, dmg, type);
        } else {
            t->hp = uint16_t(int(t->hp) - real);
            if (!t->is(Actor::kInCombat)) w_.set_in_combat(target_);
            if ((t = w_.actor(target_))) t->target = servant_;
        }
        ++strikes_;
        ++stats().strikes;
        if (Actor* t2 = w_.actor(target_)) stats().damage += unsigned(std::max(0, int(hp0) - int(t2->is(Actor::kDead) ? 0 : t2->hp)));
        PG_LOG("aerial servant: strike %d on %u for %d", strikes_, target_, dmg);
        return;
    }
    case kVanish:
        if (frame_ >= kFadeLast) terminate();
        return;
    }
}

}  // namespace

Params& params() {
    static Params p;
    return p;
}

Stats& stats() {
    static Stats s;
    return s;
}

bool load_text(const std::string& text, std::string& err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "servant.json: expected an object"; return false; }
    Params p;
    auto num = [&](const char* k, int& v) { if (j.contains(k) && j[k].is_number_integer()) v = j[k].get<int>(); };
    if (j.contains("fights")) p.fights = j.value("fights", true);
    if (j.contains("shape")) p.shape = j.value("shape", 481u);
    num("hp", p.hp);
    num("dex", p.dex);
    num("strikes", p.strikes);
    if (j.contains("damage") && j["damage"].is_array() && j["damage"].size() == 2) {
        p.damage_min = j["damage"][0].get<int>();
        p.damage_max = j["damage"][1].get<int>();
    }
    num("strike_interval", p.strike_interval);
    num("first_strike", p.first_strike);
    num("duration", p.duration);
    num("speed", p.speed);
    num("hover", p.hover);
    num("sfx_strike", p.sfx_strike);
    num("sfx_appear", p.sfx_appear);
    num("sfx_vanish", p.sfx_vanish);
    p.name = j.value("name", p.name);
    if (p.hp < 1 || p.strikes < 0 || p.damage_min < 0 || p.damage_max < p.damage_min || p.speed < 1 || p.duration < 1 ||
        p.strike_interval < 1) {
        err = "servant.json: a value is out of range";
        return false;
    }
    params() = p;
    return true;
}

bool load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_text(ss.str(), err);
}

bool is_foe(const World& w, uint16_t target) {
    const Actor* t = w.actor(target);
    const Actor* av = w.avatar();
    if (!t || !av || target == 1 || t->is(Actor::kDead) || t->chunk < 0) return false;
    if (w.same_party(1, target) || (w.party && w.party->untouchable(target))) return false;
    if (t->shape == params().shape) return false;  // another servant
    const bool at_party = t->is(Actor::kInCombat) && t->target && (t->target == 1 || w.same_party(1, t->target));
    // Townsfolk and guards (own side 1, actors.md) are the Avatar's enemies by table only: they
    // count once they actually fight the party (the same rule as the turn-based combat's).
    if (t->alignment & 1) return at_party;
    if (w.turn) return w.turn->is_hostile(target);
    return at_party || (is_enemy(*t, *av) && monster_info(t->shape));
}

uint16_t summon(World& w, uint16_t target) {
    if (!is_foe(w, target)) return 0;
    return w.kernel().add(std::make_unique<ServantProcess>(w, target));
}

bool on_weight_check(World& w, UCProcess& proc, uint16_t target) {
    if (proc.start_cls != kAirSpell || proc.start_addr != kAerialServant || proc.cls != kAirSpell) return false;
    if (!params().fights || !item_powers::enabled() || !compat::class_known(kAirSpell)) return false;
    if (!is_foe(w, target)) return false;  // the original: fetch, operate or refuse
    if (!summon(w, target)) return false;
    PG_LOG("aerial servant: cast on hostile %u, the fighting servant instead of the refusal", target);
    proc.terminate_deferred();  // the spell's own process ends here (mana and INT already counted)
    return true;
}

}  // namespace servant
}  // namespace pg
