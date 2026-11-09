// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Turn-based combat (see turn_combat.h and PLAN-TURNBASED.md): starting a fight, freezing the
// world through the kernel's hold filter, the DEX turn order, the reachable area, moves along a
// planned route, attacks / spells / items / defending / waiting, the Auto and monster AI, the
// end conditions, and the real-time fallback for scripted fights.
#include "game/creatures.h"
#include "game/turn_combat.h"
#include "game/spellbook.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <unordered_map>

#include "core/config.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/assist.h"
#include "game/avatar_mover.h"
#include "game/combat.h"
#include "game/companion_magic.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"
#include "game/item_powers.h"
#include "usecode/uc_machine.h"

namespace pg {

namespace {

enum : unsigned { kWalk = 0, kStand = 2, kReady = 5, kUnready = 6, kAttack = 7, kAdvance = 8, kCombatStand = 15,
                  kStartBlock = 59 };

long long d2(int ax, int ay, int bx, int by) {
    long long dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}
int dist(const Item& a, const Item& b) { return int(std::sqrt(double(d2(a.x, a.y, b.x, b.y)))); }

// Items the Avatar can use in a turn (consumables) and the spell items whose usecode casts.
bool use_shape(unsigned s) {
    switch (s) {
    case 766: case 537: case 538: case 180: case 624: case 465: case 466: case 228: return true;  // potions, food, drink, oil flask
    default: return item_powers::is_power_shape(s);  // items with a power (game/item_powers.h)
    }
}

void collect(const World& w, const Item& c, std::vector<uint16_t>& out, bool (*want)(unsigned)) {
    for (uint16_t k : c.contents)
        if (const Item* it = w.item(k)) {
            if (want(it->shape)) out.push_back(k);
            if (!it->contents.empty()) collect(w, *it, out, want);
        }
}

// Does the shape's attack animation have a frame that reaches out (actors.md 7.5)? Some
// companion shapes (Korick's 431, Orlok's 403) have a one-frame attack without one, so the
// animation alone never lands a blow; their hits are resolved directly (see do_attack).
bool melee_frames(const World& w, unsigned shape) {
    const AnimSequence* s = w.data().anim.get(shape, 7);
    if (!s) return false;
    for (unsigned d = 0; d < 8; ++d)
        for (unsigned i = 0; i < s->length; ++i)
            if (s->at(d, i).attack_range()) return true;
    return false;
}

}  // namespace

TurnCombat::TurnCombat(World& w) : w_(w) {
    w_.kernel().hold = [this](const Process& p) { return hold(p); };
}

TurnCombat::~TurnCombat() = default;  // the World's kernel goes first (declared after `turn`)

// ---------------------------------------------------------------------------------------------
// Settings

void TurnCombat::load_settings() {
    const Config& c = Config::global();
    enabled = c.get("combat_style", "turn") != "realtime";
    start_on_hit = c.get_bool("turn_on_hit", true);
    speed = std::clamp(c.get_int("turn_speed", 0), 0, 2);
}

void TurnCombat::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    auto save = [] {
        Config& c = Config::global();
        if (!c.path().empty()) c.save();
    };
    World::extra_option_rows().push_back([save](World& w) {
        TurnCombat* t = w.turn.get();
        MenuGump::OptionRow r{"Combat:", [t] { return std::string(t && t->enabled ? "Turn-based" : "Real-time"); },
                              [t, save](int) {
                                  if (!t) return;
                                  t->enabled = !t->enabled;
                                  if (!t->enabled && t->active()) t->end_fight(kAborted);
                                  Config::global().set("combat_style", t->enabled ? "turn" : "realtime");
                                  save();
                              }};
        r.group = "Controls";
        r.kind = MenuGump::OptionRow::kChoice;
        return r;
    });
    World::extra_option_rows().push_back([save](World& w) {
        TurnCombat* t = w.turn.get();
        MenuGump::OptionRow r{"Turns When Hit:", [t] { return std::string(t && t->start_on_hit ? "On" : "Off"); },
                              [t, save](int) {
                                  if (!t) return;
                                  t->start_on_hit = !t->start_on_hit;
                                  Config::global().set_bool("turn_on_hit", t->start_on_hit);
                                  save();
                              }};
        r.group = "Controls";
        r.visible = [t] { return t && t->enabled; };
        return r;
    });
    World::extra_option_rows().push_back([save](World& w) {
        TurnCombat* t = w.turn.get();
        static const char* const kNames[3] = {"Normal", "Fast", "Fastest"};
        MenuGump::OptionRow r{"Turn Speed:", [t] { return std::string(kNames[t ? std::clamp(t->speed, 0, 2) : 0]); },
                              [t, save](int dir) {
                                  if (!t) return;
                                  t->speed = (t->speed + (dir < 0 ? 2 : 1)) % 3;
                                  Config::global().set_int("turn_speed", t->speed);
                                  save();
                              }};
        r.group = "Controls";
        r.kind = MenuGump::OptionRow::kChoice;
        r.visible = [t] { return t && t->enabled; };
        return r;
    });
}

// ---------------------------------------------------------------------------------------------
// Who is who

std::string TurnCombat::name_of(uint16_t id) const {
    const Actor* a = w_.actor(id);
    if (!a) return "?";
    if (w_.party && w_.party->is_companion(id)) return w_.party->display_name(id);
    if (id == 1) return w_.avatar_name.empty() ? "Avatar" : w_.avatar_name;
    if (!a->name.empty()) return a->name;
    if (std::string cn = creature_display_name(w_, id); !cn.empty()) {  // "Lurker's pet", "Young troll"
        cn[0] = char(std::toupper(static_cast<unsigned char>(cn[0])));
        return cn;
    }
    std::string n = w_.data().usecode.class_name(a->shape);
    if (n.empty()) return "monster";
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) out += char(i ? std::tolower(n[i]) : n[i]);
    return out;
}

int TurnCombat::max_hp(const Actor& a) const {
    if (w_.party && w_.party->is_companion(a.id)) return w_.party->max_hp(a);
    if (a.id == 1) return std::max(1, 2 * int(a.str));
    if (const MonsterInfo* m = monster_info(a.shape)) return std::max<int>({1, m->hp1, int(a.hp)});
    return std::max({1, 2 * int(a.str), int(a.hp)});
}

uint16_t TurnCombat::current() const {
    return idx_ >= 0 && size_t(idx_) < order_.size() ? order_[size_t(idx_)].id : 0;
}

TurnCombat::Fighter* TurnCombat::fighter(uint16_t id) {
    for (Fighter& f : order_)
        if (f.id == id) return &f;
    for (Fighter& f : joiners_)
        if (f.id == id) return &f;
    return nullptr;
}
const TurnCombat::Fighter* TurnCombat::fighter(uint16_t id) const {
    for (const Fighter& f : order_)
        if (f.id == id) return &f;
    for (const Fighter& f : joiners_)
        if (f.id == id) return &f;
    return nullptr;
}
bool TurnCombat::is_fighter(uint16_t id) const { return active() && fighter(id) != nullptr; }
bool TurnCombat::is_party_fighter(uint16_t id) const {
    const Fighter* f = active() ? fighter(id) : nullptr;
    return f && f->party;
}

bool TurnCombat::keep_auto(uint16_t id) const {
    const Fighter* f = fighter(id);
    return f && f->keep_auto;
}
void TurnCombat::toggle_keep_auto(uint16_t id) {
    if (Fighter* f = fighter(id); f && f->controllable) f->keep_auto = !f->keep_auto;
}
void TurnCombat::toggle_party_auto() {
    party_auto = !party_auto;
    say(party_auto ? "Party Auto: on" : "Party Auto: off (you take the next party turn)");
}

void TurnCombat::say(const std::string& s, bool to_log) {
    status = s;
    log.push_back(s);
    if (to_log) w_.game_log.add(GameLog::kCombat, GameLog::kTurn, s);
    if (log.size() > 400) log.erase(log.begin(), log.begin() + 100);
    PG_LOG("turn: %s", s.c_str());
}

// The party in this fight's sense: the Avatar, following companions on this map, their summons.
std::vector<uint16_t> TurnCombat::party_actors() const {
    std::vector<uint16_t> out;
    const Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead)) return out;
    out.push_back(1);
    if (w_.party && w_.party->enabled) {
        for (int m : w_.party->members) {
            const CompanionState& st = w_.party->state[size_t(m)];
            if (st.mode != CompanionState::kFollow) continue;
            const Actor* a = w_.actor(st.id);
            if (!a || a->is(Actor::kDead) || a->chunk < 0 || a->mapnum != av->mapnum) continue;
            if (!in_area(*av, *a, kAreaMargin)) continue;
            out.push_back(st.id);
        }
        for (const Party::Summon& s : w_.party->summons) {
            const Actor* a = w_.actor(s.id);
            if (a && !a->is(Actor::kDead) && a->chunk >= 0 && in_area(*av, *a, kAreaMargin)) out.push_back(s.id);
        }
    }
    return out;
}

// On the screen around `from` plus `margin` game pixels (the screen projection of the offset).
bool TurnCombat::in_area(const Actor& from, const Actor& o, int margin) const {
    int dx = o.x - from.x, dy = o.y - from.y, dz = o.z - from.z;
    int sx = (dx - dy) / 4, sy = (dx + dy) / 8 - dz;
    return std::abs(sx) <= 160 + margin && std::abs(sy) <= 100 + margin;
}

bool TurnCombat::party_side(uint16_t id) const { return id == 1 || w_.same_party(1, id); }

bool TurnCombat::is_hostile(uint16_t id) const {
    const Actor* o = w_.actor(id);
    const Actor* av = w_.avatar();
    if (!o || !av || id == 1 || o->is(Actor::kDead) || o->chunk < 0) return false;
    if (party_side(id) || (o->ext_flags & kExtCompanion)) return false;
    if (w_.fast_ready() && !(o->flags & kItemFastArea)) return false;
    if (o->mapnum != av->mapnum) return false;
    if (o->flags & (kItemInvisible | kItemEthereal)) return false;
    if (w_.creatures && w_.creatures->withdrawn(id)) return false;  // a new creature that lost interest
    bool engaged = o->is(Actor::kInCombat) && o->target && party_side(o->target);
    // Townsfolk and guards (own side 1, actors.md) are the Avatar's enemies by table only: they
    // join a fight once they actually fight the party, not while idle or busy with a monster.
    if (o->alignment & 1) return engaged;
    bool monster = is_enemy(*o, *av) && monster_info(o->shape) && !(o->ext_flags & kExtPermanentNpc);
    bool fighting = o->is(Actor::kInCombat) && is_enemy(*o, *av);
    return engaged || monster || fighting;
}

std::vector<uint16_t> TurnCombat::hostiles_in_area() const {
    std::vector<uint16_t> out;
    std::vector<uint16_t> party = party_actors();
    for (unsigned n = 2; n < 256; ++n) {
        if (!is_hostile(uint16_t(n)) || (active() && fighter(uint16_t(n)))) continue;
        const Actor* o = w_.actor(uint16_t(n));
        for (uint16_t p : party)
            if (const Actor* pa = w_.actor(p); pa && in_area(*pa, *o, kAreaMargin)) { out.push_back(uint16_t(n)); break; }
    }
    return out;
}

// Story fights the turn controller leaves in real time: named NPCs whose usecode drives their
// combat (a cast-event handler: the scripted casters and bosses), and anyone a usecode script
// is running on right now (a scene, a summoning, a scripted attack).
bool TurnCombat::is_scripted(uint16_t id, std::string* why) const {
    const Actor* o = w_.actor(id);
    if (!o) return false;
    if ((o->ext_flags & kExtPermanentNpc) && w_.data().usecode.event_entry(1024u + id, kEvCast)) {
        if (why) *why = "usecode combat (cast event of " + w_.data().usecode.class_name(1024u + id) + ")";
        return true;
    }
    bool script = false;
    w_.kernel().for_each([&](Process* p) {
        if (!script && p->item == id && p->live() && dynamic_cast<UCProcess*>(p)) script = true;
    });
    if (script) {
        if (why) *why = "a usecode script is running on it";
        return true;
    }
    return false;
}

bool TurnCombat::valid_fighter(uint16_t id) const {
    const Actor* a = w_.actor(id);
    const Actor* av = w_.avatar();
    return a && av && !a->is(Actor::kDead) && a->chunk >= 0 && a->mapnum == av->mapnum &&
           !(w_.creatures && w_.creatures->withdrawn(id));
}

std::vector<uint16_t> TurnCombat::enemies_of(uint16_t id) const {
    std::vector<uint16_t> out;
    const Fighter* me = fighter(id);
    const Actor* a = w_.actor(id);
    if (!me || !a) return out;
    for (const Fighter& f : order_)
        if (f.party != me->party && valid_fighter(f.id) && !(w_.creatures && w_.creatures->untargetable(f.id))) out.push_back(f.id);
    std::sort(out.begin(), out.end(), [&](uint16_t x, uint16_t y) {
        return d2(w_.actor(x)->x, w_.actor(x)->y, a->x, a->y) < d2(w_.actor(y)->x, w_.actor(y)->y, a->x, a->y);
    });
    return out;
}

bool TurnCombat::busy(uint16_t id) const {
    const Actor* a = w_.actor(id);
    if (!a) return false;
    return a->is(Actor::kAnimLock) || w_.kernel().count_exact(id, kProcActorAnim) ||
           w_.kernel().count_exact(id, kProcPathfind) || w_.kernel().count_exact(id, kProcGravity);
}

// ---------------------------------------------------------------------------------------------
// Freezing

bool TurnCombat::hold(const Process& p) const {
    if (phase_ == kIdle) return false;
    if (!let_run_.empty() && let_run_.count(p.pid())) return false;  // what a timed-out turn left going
    // Never frozen: the view, windows and barks, egg hatching, death, teleports, palette effects.
    if (p.type == kProcGumpNotify || p.type == kProcDestroyItem) return false;
    if (dynamic_cast<const CameraProcess*>(&p) || dynamic_cast<const EggHatcherProcess*>(&p) ||
        dynamic_cast<const AvatarDeathProcess*>(&p) || dynamic_cast<const TeleportProcess*>(&p) ||
        dynamic_cast<const FadeProcess*>(&p) || dynamic_cast<const InvertProcess*>(&p) ||
        dynamic_cast<const StartupProcess*>(&p))
        return false;
    // Always frozen: the real-time AI and the player's controller.
    if (p.pid() == w_.mover_pid || dynamic_cast<const AvatarMoverProcess*>(&p)) return true;
    if (p.type == kProcCombat || p.type == kProcLoiter) return true;
    if (p.item && p.item == acting_) return false;  // the character whose turn it is
    if (p.type == kProcActorAnim || p.type == kProcGravity) {
        if (p.item && is_party_fighter(p.item)) return false;  // the party settles into place
        return p.serial() <= snap_;                             // hit reactions, deaths: run
    }
    if (p.type == kProcPathfind && p.item && fighter(p.item)) return true;
    return p.serial() <= snap_;
}

bool TurnCombat::holds_ai(uint16_t actor) const { return active() && fighter(actor) != nullptr; }

// ---------------------------------------------------------------------------------------------
// Starting and ending

// Companions left behind come to the fight (the user's addition to the plan): every member
// who is following but off screen (beyond the encounter margin) appears at the edge of the view
// on the Avatar's side, away from the enemies, on a safe, supported spot on his level from where
// the pathfinder can reach him, never next to a foe. Members on screen stay where they are;
// waiting or parted companions never move. Returns how many were moved.
int TurnCombat::pull_companions(const std::vector<uint16_t>& hostiles) {
    Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead) || av->chunk < 0 || !w_.party || !w_.party->enabled || w_.stasis) return 0;
    Party& p = *w_.party;
    // Away from the enemies (their centre), else no preference.
    double ax = 0, ay = 0;
    for (uint16_t h : hostiles)
        if (const Actor* o = w_.actor(h)) { ax += av->x - o->x; ay += av->y - o->y; }
    double al = std::hypot(ax, ay);
    if (al > 0) { ax /= al; ay /= al; }
    // The visible area in world pixels (zoom makes it bigger), just inside its edge.
    // (in from the edge, and below the party bar and the turn strip along the top).
    // The action menu takes the bottom left, the party bar and the strip the top: the feet go
    // below them so the whole figure shows.
    const int hw = std::max(110, w_.view_w / 2 - 30), hh = std::max(60, w_.view_h / 2 - 24);
    const int hw_left = std::max(70, w_.view_w / 2 - 84), hh_top = std::max(22, w_.view_h / 2 - 78);
    int moved = 0;
    std::vector<std::array<int, 3>> placed;
    for (int m : p.members) {
        CompanionState& st = p.state[size_t(m)];
        if (st.mode != CompanionState::kFollow || st.hidden) continue;
        Actor* a = w_.actor(st.id);
        if (!a || a->is(Actor::kDead)) continue;
        bool here = a->chunk >= 0 && a->mapnum == av->mapnum && !(a->flags & kItemEthereal);
        if (here && in_area(*av, *a, kAreaMargin)) continue;  // already with him
        // Its own direction from the Avatar (when it is on this map).
        double cx = here ? a->x - av->x : -ax, cy = here ? a->y - av->y : -ay;
        double cl = std::hypot(cx, cy);
        if (cl > 0) { cx /= cl; cy /= cl; }
        const ShapeInfo& si = w_.data().typeflags.get(a->shape);
        int xd, yd, zd;
        si.footprint(false, xd, yd, zd);
        // Candidates round the screen edge (and further in), best first.
        struct Cand { int x, y; double score; };
        std::vector<Cand> cands;
        for (double f : {1.0, 0.8, 0.6, 0.4}) {
            for (int k = 0; k < 48; ++k) {
                double t = k * 2 * M_PI / 48;
                // A point on the edge of the screen rectangle in that screen direction.
                double ux = std::cos(t), uy = std::sin(t);
                double sc = std::min((ux < 0 ? hw_left : hw) / std::max(1e-6, std::abs(ux)), (uy < 0 ? hh_top : hh) / std::max(1e-6, std::abs(uy)));
                double sx = ux * sc * f, sy = uy * sc * f;
                double dx = 2 * sx + 4 * sy, dy = 4 * sy - 2 * sx;  // screen offset -> world offset
                double dl = std::hypot(dx, dy);
                double behind = al > 0 ? (dx * ax + dy * ay) / std::max(1.0, dl) : 0;
                double own = (dx * cx + dy * cy) / std::max(1.0, dl);
                // The top of the view is mostly under the bars: the sides and the bottom show better.
                double top = uy < -0.3 ? 1.0 : 0.0;
                cands.push_back({av->x + int(dx), av->y + int(dy), 2 * behind + own + 4 * f - top});  // the edge first
            }
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        bool done = false;
        for (const Cand& c : cands) {
            if (done) break;
            for (int dz : {0, 8, -8, 16, -16, 24}) {
                int z = av->z + dz;
                if (z < 0) continue;
                World::Placement pl = w_.can_exist(c.x, c.y, z, xd, yd, zd, si.flags(), st.id);
                if (!pl.valid || !pl.supported || !pl.land) continue;
                const Item* land = w_.item(pl.land);
                if (!land || land->as_actor()) continue;
                const ShapeInfo& li = w_.data().typeflags.get(land->shape);
                if (li.is(ShapeInfo::kDamaging) || (li.is(ShapeInfo::kSea) && !li.is(ShapeInfo::kLand))) continue;
                if (assist::hazard(w_, *a, c.x, c.y, z)) continue;  // no corner over water or lava either
                bool near_foe = false;
                for (uint16_t h : hostiles)
                    if (const Actor* o = w_.actor(h); o && d2(o->x, o->y, c.x, c.y) < 192LL * 192) near_foe = true;
                for (unsigned n = 2; n < 256 && !near_foe; ++n)
                    if (is_hostile(uint16_t(n)))
                        if (const Actor* o = w_.actor(uint16_t(n)); o && d2(o->x, o->y, c.x, c.y) < 128LL * 128) near_foe = true;
                for (const auto& q : placed)
                    if (d2(q[0], q[1], c.x, c.y) < 48LL * 48) near_foe = true;  // not on top of each other
                if (near_foe) break;
                // Can it walk to the Avatar from there?
                Pathfinder pf(w_);
                ActorState from;
                from.x = c.x; from.y = c.y; from.z = z;
                from.dir = uint8_t(direction_to(av->x - c.x, av->y - c.y));
                from.in_combat = a->is(Actor::kInCombat);
                pf.set_actor(st.id, &from);
                pf.set_target_item(1, false);
                std::vector<PathStep> path;
                if (!pf.plan(path)) continue;
                // Move it there.
                w_.kernel().kill(st.id, kProcAny);
                a->set(Actor::kAnimLock, false);
                if (a->chunk >= 0) w_.detach(st.id);
                a->flags &= uint16_t(~kItemEthereal);
                a->mapnum = uint8_t(w_.map().map_number());
                a->dir = from.dir;
                w_.move_to(st.id, c.x, c.y, z, false);
                if (const AnimSequence* sq = w_.data().anim.get(a->shape, kStand)) a->frame = sq->at(a->dir, 0).frame;
                st.placing = false;
                st.stuck = 0;
                st.far_frames = 0;
                w_.kernel().add(std::make_unique<SpriteProcess>(w_, 578, 66, 80, 1, 1, c.x, c.y, z));  // a puff of smoke
                placed.push_back({c.x, c.y, z});
                say(name_of(st.id) + " joins you");
                ++moved;
                done = true;
                break;
            }
        }
        if (!done) PG_LOG("turn: no spot at the edge for %s", name_of(st.id).c_str());
    }
    return moved;
}

bool TurnCombat::start_fight(const char* reason) {
    Actor* av = w_.avatar();
    if (!enabled || active() || !av || av->is(Actor::kDead) || w_.stasis) return false;
    std::vector<uint16_t> hostiles = hostiles_in_area();
    // Scripted encounters stay in real time while any of their actors is about.
    std::vector<uint16_t> fight;
    bool scripted = false;
    for (uint16_t h : hostiles) {
        std::string why;
        if (fallback_ids_.count(h) || is_scripted(h, &why)) {
            if (!fallback_ids_.count(h)) {
                fallback_ids_.insert(h);
                fallbacks.push_back({h, w_.actor(h)->shape, why});
                say("Scripted fight (" + name_of(h) + ": " + why + "): real time");
            }
            scripted = true;
            continue;
        }
        fight.push_back(h);
    }
    if (scripted) { last_end = kFallback; return false; }
    if (fight.empty()) return false;
    // A fight that starts in the middle of a conversation ends it (it would stay frozen).
    w_.combat_interrupts_conversation(fight.front(), "turn-based fight");
    pull_companions(fight);  // before the turn order: the followers take part
    ++fights;
    order_.clear();
    joiners_.clear();
    avatar_was_combat_ = av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    if (!avatar_was_combat_) {
        av->set(Actor::kCombatRun, false);
        w_.set_in_combat(1);
        if (!av->is(Actor::kAnimLock)) w_.do_anim(1, kReady, av->dir);
    }
    for (uint16_t p : party_actors()) {
        Fighter f;
        f.id = p;
        f.party = true;
        f.controllable = p == 1 || (w_.party && w_.party->is_companion(p));
        if (Actor* a = w_.actor(p)) {
            if (p != 1) {
                w_.kernel().kill(p, kProcPathfind);
                if (f.controllable) a->set(Actor::kInCombat, true);
            }
        }
        joiners_.push_back(f);
    }
    for (uint16_t h : fight) {
        Fighter f;
        f.id = h;
        // Their combat flag is left as it is (a save in the fight stays consistent); the end of
        // the fight puts the survivors into real combat.
        w_.kernel().kill(h, kProcPathfind);
        w_.kernel().kill(h, kProcLoiter);
        joiners_.push_back(f);
    }
    phase_ = kStart;
    phase_frames_ = 0;
    round_ = 0;
    idx_ = -1;
    acting_ = 0;
    snap_ = w_.kernel().serial();
    last_end = kNone;
    say(std::string("Turn-based combat (") + reason + "): " + std::to_string(fight.size()) + " foe" +
        (fight.size() == 1 ? "" : "s"));
    return true;
}

void TurnCombat::end_fight(End why) {
    if (phase_ == kIdle) return;
    static const char* const kNames[] = {"", "Victory", "Defeat", "Escaped", "Real time", "Ended"};
    say(std::string(kNames[why]) + (why == kVictory || why == kEscape ? "!" : ""));
    last_end = why;
    Actor* av = w_.avatar();
    // Survivors go back to the real-time AI.
    for (const Fighter& f : order_) {
        Actor* a = w_.actor(f.id);
        if (!a || a->is(Actor::kDead)) continue;
        a->set(Actor::kAnimLock, a->is(Actor::kAnimLock) && w_.kernel().count_exact(f.id, kProcActorAnim));
        if (f.party) continue;
        if (!w_.kernel().count_exact(f.id, kProcCombat) && av) {
            a->set(Actor::kInCombat, false);
            if (why != kEscape || is_enemy(*a, *av)) {
                uint16_t tgt = a->target && w_.actor(a->target) ? a->target : 1;
                w_.set_in_combat(f.id);
                if ((a = w_.actor(f.id))) a->target = tgt;
            }
        }
    }
    phase_ = kIdle;
    order_.clear();
    joiners_.clear();
    reach_.clear();
    route_.clear();
    acting_ = 0;
    idx_ = -1;
    pending_attack_ = 0;
    wait_pid_ = 0;
    let_run_.clear();
    // Won or got away: back to peace, whatever the stance was before the fight (the user's rule,
    // PLAN-TURNBASED.md). A fight handed over to real time or stopped leaves things as they were.
    if (why == kVictory || why == kEscape) {
        leave_combat();
    } else if (av && !av->is(Actor::kDead) && !avatar_was_combat_ && why != kDefeat) {
        w_.clear_in_combat(1);
        if (!av->is(Actor::kAnimLock)) w_.do_anim(1, kUnready, av->dir);
    }
}

void TurnCombat::leave_combat() {
    Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead)) return;
    if (w_.party) {
        w_.party->avatar_target = 0;
        for (int m : w_.party->members) {
            CompanionState& st = w_.party->state[size_t(m)];
            st.target = 0;
            Actor* a = w_.actor(st.id);
            if (!a || a->is(Actor::kDead) || !a->is(Actor::kInCombat)) continue;
            a->set(Actor::kInCombat, false);
            if (!busy(st.id)) w_.do_anim(st.id, kStand, a->dir);
        }
    }
    av->set(Actor::kCombatRun, false);
    w_.clear_in_combat(1);  // the music goes back with it
    pending_unready_ = true;
    if (!av->is(Actor::kAnimLock)) { w_.do_anim(1, kUnready, av->dir); pending_unready_ = false; }
}

// Real-time combat: the same peace once every foe around is gone. Only after a foe was seen in
// this spell of combat mode (the player may draw his weapon with nothing about), and not while
// anything is still fighting the party or a scene runs.
void TurnCombat::realtime_calm() {
    const Actor* av = w_.avatar();
    bool cm = av && !av->is(Actor::kDead) && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    if (!cm || w_.stasis || w_.kernel().paused()) {
        rt_foes_seen_ = false;
        rt_calm_ = 0;
        return;
    }
    if (w_.kernel().frame_number() % 5) return;
    if (!hostiles_in_area().empty()) {
        rt_foes_seen_ = true;
        rt_calm_ = 0;
        return;
    }
    if (!rt_foes_seen_ || (rt_calm_ += 5) < kRealtimeCalmFrames) return;
    rt_foes_seen_ = false;
    rt_calm_ = 0;
    PG_LOG("turn: real time, no foe left: combat mode ends");
    leave_combat();
}

void TurnCombat::world_replaced() {
    if (active()) PG_LOG("turn: world replaced (load / new game), fight dropped");
    reset_silently();
    avatar_seen_ = nullptr;  // pre_frame picks up the new Avatar (and its combat mode) next frame
}

void TurnCombat::reset_silently() {
    phase_ = kIdle;
    order_.clear();
    joiners_.clear();
    reach_.clear();
    route_.clear();
    acting_ = 0;
    idx_ = -1;
    pending_attack_ = 0;
    wait_pid_ = 0;
    pending_start_ = false;
    pending_unready_ = false;
    let_run_.clear();
    notice.clear();
    notice_frames = 0;
    rt_foes_seen_ = false;
    rt_calm_ = 0;
    fallback_ids_.clear();
}

int TurnCombat::on_hit(uint16_t victim, uint16_t attacker, int damage) {
    if (!enabled) return damage;
    if (active()) {
        const Fighter* f = fighter(victim);
        if (f && f->defending && damage > 0) damage = std::max(0, damage / 2);
        return damage;
    }
    if (!start_on_hit || !attacker || !party_side(victim) || party_side(attacker)) return damage;
    const Actor* av = w_.avatar();
    if (av && !(av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun))) pending_start_ = true;
    return damage;
}

// ---------------------------------------------------------------------------------------------
// Frames

void TurnCombat::pre_frame() {
    const Actor* av = w_.avatar();
    // A load or a new game replaced every object: whatever fight there was is gone.
    if (av != avatar_seen_ || w_.kernel().frame_number() < last_frame_) {
        if (active() || pending_start_) PG_LOG("turn: world replaced (load / new game), fight dropped");
        reset_silently();
        avatar_seen_ = av;
        prev_combat_ = av && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);  // a load is not "entering"

    }
    last_frame_ = w_.kernel().frame_number();
    // Entering combat mode (either combat style): companions left behind come to the edge.
    bool cm = av && !av->is(Actor::kDead) && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    if (cm && !prev_combat_ && !active()) pull_companions(hostiles_in_area());
    prev_combat_ = cm;
    if (pending_unready_ && av && !active()) {
        if (av->is(Actor::kDead) || av->is(Actor::kInCombat)) pending_unready_ = false;
        else if (!av->is(Actor::kAnimLock)) { w_.do_anim(1, kUnready, av->dir); pending_unready_ = false; }
    }
    if (!enabled) { pending_start_ = false; realtime_calm(); return; }
    if (active() || !av || av->is(Actor::kDead) || w_.stasis || w_.kernel().paused()) return;
    // Hostiles of a scripted encounter that died or left no longer block turn-based fights.
    for (auto it = fallback_ids_.begin(); it != fallback_ids_.end();) {
        if (!is_hostile(*it)) it = fallback_ids_.erase(it);
        else ++it;
    }
    bool combat_mode = av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    if (pending_start_) {
        pending_start_ = false;
        if (start_fight("attacked")) return;
    }
    // In combat mode (C, the pad, a double-click on the Avatar) with a hostile in the area.
    if (combat_mode && w_.kernel().frame_number() % 3 == 0) start_fight("combat mode");
}

void TurnCombat::frame() {
    if (phase_ == kIdle) return;
    Actor* av = w_.avatar();
    if (!enabled) { end_fight(kAborted); return; }
    if (!av || av->is(Actor::kDead)) { end_fight(kDefeat); return; }
    if (w_.stasis) {
        // A scene started in the middle of the fight: it needs real time.
        for (const Fighter& f : order_)
            if (!f.party) fallback_ids_.insert(f.id);
        end_fight(kFallback);
        return;
    }
    if (w_.kernel().paused()) return;  // the main menu and the like: no time passes in the fight
    const bool asked = asking_player();  // the player's time does not count
    ++phase_frames_;
    if (!asked) ++sub_frames_;
    if (notice_frames > 0 && --notice_frames == 0) notice.clear();
    if (phase_ == kStart) {
        // The party finishes the step it is in (drawing weapons), then the first round.
        bool settling = false;
        for (const Fighter& f : joiners_)
            if (f.party && busy(f.id)) settling = true;
        if (!settling || phase_frames_ > 30) {
            phase_ = kTurn;
            new_round();
        }
        return;
    }
    if (check_end()) return;
    uint16_t id = current();
    if (!valid_fighter(id)) { next_turn(); return; }
    // The time limit: whatever is holding this turn up (a stuck animation, a walk that never
    // ends, a script) must not hold up the fight. Waiting for the player's command is not counted.
    if (sub_ == kChoose) {
        turn_age_ = 0;
    } else if (!asked) {
        turn_age_ += 1.0 / std::max(1, world_frames());
        if (turn_age_ >= kTurnTimeoutFrames) { turn_timed_out(); return; }
    }
    switch (sub_) {
    case kBegin: {
        // Whatever the character was doing at the freeze finishes first.
        if (busy(id) && sub_frames_ < 90) return;
        if (busy(id)) {
            w_.kernel().kill(id, kProcActorAnim);
            w_.kernel().kill(id, kProcPathfind);
            if (Actor* a = w_.actor(id)) a->set(Actor::kAnimLock, false);
        }
        const Fighter* f = fighter(id);
        bool manual = f && f->controllable && !f->keep_auto && !party_auto && !w_.auto_ui;
        sub_ = manual ? kChoose : kThink;
        sub_frames_ = 0;
        reach_dirty_ = true;
        if (manual) status = name_of(id) + ": your move";
        return;
    }
    case kChoose: {
        const Fighter* f = fighter(id);
        if (f && (f->keep_auto || party_auto)) { sub_ = kThink; sub_frames_ = 0; }  // Party Auto switched on
        return;
    }
    case kThink:
        if (sub_frames_ < (speed == 0 ? 6 : 1) && !speed_held) return;  // a beat before the AI acts
        ai_think();
        return;
    case kMoving:
        step_route();
        return;
    case kActing:
        if (direct_hit_ && --direct_in_ <= 0) {
            // The blow of a shape whose animation cannot land one: the normal hit pipeline.
            uint16_t t = direct_hit_;
            direct_hit_ = 0;
            if (valid_fighter(t)) w_.receive_hit(t, id, direct_dir_, 0, 0);
        }
        if (sub_frames_ < 4 || direct_hit_) return;
        if (!settled()) return;
        finish_action();
        return;
    case kPause:
        if (--pause_left_ > 0 && !speed_held) return;
        next_turn();
        return;
    }
}

int TurnCombat::world_frames() const {
    if (!active()) return 1;
    if (awaiting_player()) return 1;
    if (speed_held) return 4;
    return speed == 0 ? 1 : speed == 1 ? 2 : 4;
}

bool TurnCombat::check_end() {
    Actor* av = w_.avatar();
    bool foe = false;
    for (const Fighter& f : order_)
        if (!f.party && valid_fighter(f.id)) foe = true;
    for (const Fighter& f : joiners_)
        if (!f.party && valid_fighter(f.id)) foe = true;
    if (!foe && sub_ != kActing && sub_ != kMoving) {
        // Someone else turned up as the last one fell: the fight goes on with them (out of combat
        // mode they would only start a new one at once).
        if (!hostiles_in_area().empty()) {
            new_round();
            return true;
        }
        end_fight(kVictory);
        return true;
    }
    // Escape: no living foe is in the encounter area of any party member.
    if (foe && sub_ != kActing) {
        bool near = false;
        std::vector<uint16_t> party = party_actors();
        auto test = [&](const std::vector<Fighter>& v) {
            for (const Fighter& f : v) {
                if (f.party || !valid_fighter(f.id)) continue;
                const Actor* o = w_.actor(f.id);
                for (uint16_t p : party)
                    if (const Actor* pa = w_.actor(p); pa && in_area(*pa, *o, kAreaMargin)) near = true;
            }
        };
        test(order_);
        test(joiners_);
        if (!near && av) {
            end_fight(kEscape);
            return true;
        }
    }
    return false;
}

void TurnCombat::new_round() {
    // The fallen and the gone leave; those who noticed the party join.
    std::vector<Fighter> next;
    for (Fighter& f : order_)
        if (valid_fighter(f.id)) next.push_back(f);
    for (Fighter& f : joiners_)
        if (valid_fighter(f.id) && std::none_of(next.begin(), next.end(), [&](const Fighter& g) { return g.id == f.id; }))
            next.push_back(f);
    joiners_.clear();
    order_ = next;
    std::vector<uint16_t> fresh = hostiles_in_area();
    for (uint16_t h : fresh) {
        std::string why;
        if (is_scripted(h, &why)) {
            fallback_ids_.insert(h);
            fallbacks.push_back({h, w_.actor(h)->shape, why});
            say("Scripted fight (" + name_of(h) + ": " + why + "): real time");
            end_fight(kFallback);
            return;
        }
        Fighter f;
        f.id = h;
        w_.kernel().kill(h, kProcPathfind);
        w_.kernel().kill(h, kProcLoiter);
        order_.push_back(f);
        if (round_ > 0) say(name_of(h) + " joins the fight");
    }
    // New party members (a summoned skeleton, a companion arriving).
    for (uint16_t p : party_actors())
        if (!fighter(p)) {
            Fighter f;
            f.id = p;
            f.party = true;
            f.controllable = p == 1 || (w_.party && w_.party->is_companion(p));
            order_.push_back(f);
        }
    for (Fighter& f : order_) {
        const Actor* a = w_.actor(f.id);
        f.dex = a ? a->dex : 0;
        f.tiebreak = w_.rand_below(1000);
        f.waited = false;
    }
    std::stable_sort(order_.begin(), order_.end(), [](const Fighter& a, const Fighter& b) {
        return a.dex != b.dex ? a.dex > b.dex : a.tiebreak < b.tiebreak;
    });
    ++round_;
    idx_ = 0;
    w_.game_log.add(GameLog::kCombat, GameLog::kTurn, "- Round " + std::to_string(round_) + " -");
    begin_turn();
}

void TurnCombat::begin_turn() {
    uint16_t id = current();
    sub_ = kBegin;
    sub_frames_ = 0;
    moved_ = false;
    pending_attack_ = 0;
    wait_pid_ = 0;
    direct_hit_ = 0;
    route_.clear();
    reach_dirty_ = true;
    turn_age_ = 0;
    acting_ = id;
    snap_ = w_.kernel().serial();
    if (Fighter* f = fighter(id)) f->defending = false;
    if (id) {
        turn_history.push_back(id);
        turn_rounds.push_back(round_);
        w_.game_log.add(GameLog::kCombat, GameLog::kTurn, name_of(id) + "'s turn", id);  // the HUD's combat log
        if (Actor* a = w_.actor(id); a && fighter(id) && !fighter(id)->party) {
            w_.kernel().kill(id, kProcPathfind);  // the real-time AI's plans are stale
            w_.kernel().kill(id, kProcLoiter);
        }
    }
}

void TurnCombat::next_turn() {
    acting_ = 0;
    snap_ = w_.kernel().serial();
    ++idx_;
    while (idx_ >= 0 && size_t(idx_) < order_.size() && !valid_fighter(order_[size_t(idx_)].id)) ++idx_;
    if (size_t(idx_) >= order_.size()) {
        if (check_end()) return;
        new_round();
        return;
    }
    begin_turn();
}

void TurnCombat::finish_action() {
    acting_ = current();
    sub_ = kPause;
    sub_frames_ = 0;
    pause_left_ = speed == 0 ? 15 : speed == 1 ? 6 : 1;
    if (Actor* a = w_.actor(current()); a && !a->is(Actor::kDead) && !busy(a->id) && a->id != 1)
        if (a->last_anim == kAttack) w_.do_anim(a->id, a->is(Actor::kInCombat) ? kCombatStand : kStand, a->dir);
}

bool TurnCombat::asking_player() const {
    return w_.target_pid || w_.gumps.top_modal() || w_.ask_menu;
}

// The turn took too long: it is over, and the actor is left free. A blow or a spell already on
// its way still lands; hit reactions and falls it caused run on; only the actor's own animation,
// walk and script end, so they cannot lock him up or stall the next turns.
void TurnCombat::turn_timed_out() {
    uint16_t id = current();
    const Fighter* f = fighter(id);
    notice = name_of(id) + (f && f->party ? " missed a turn" : " missed its turn");
    notice_frames = kNoticeFrames;
    say(notice);
    ++timeouts;
    if (direct_hit_) {
        uint16_t t = direct_hit_;
        direct_hit_ = 0;
        if (valid_fighter(t)) w_.receive_hit(t, id, direct_dir_, 0, 0);
    }
    std::vector<Process*> stop;
    w_.kernel().for_each([&](Process* p) {
        if (!p->live()) return;
        if (p->item == id) {
            if (p->type == kProcGravity) let_run_.insert(p->pid());
            else if (p->serial() > snap_ && dynamic_cast<UCProcess*>(p)) stop.push_back(p);
        } else if (p->serial() > snap_ && (p->type == kProcActorAnim || p->type == kProcGravity || dynamic_cast<SpriteProcess*>(p))) {
            let_run_.insert(p->pid());
        }
    });
    for (Process* p : stop) p->terminate();
    w_.kernel().kill(id, kProcActorAnim);
    w_.kernel().kill(id, kProcPathfind);
    pending_attack_ = 0;
    route_.clear();
    wait_pid_ = 0;
    if (Actor* a = w_.actor(id); a && !a->is(Actor::kDead)) {
        a->set(Actor::kAnimLock, false);
        if (!w_.kernel().count_exact(id, kProcGravity)) {
            unsigned stand = a->is(Actor::kInCombat) && w_.data().anim.get(a->shape, kCombatStand) ? kCombatStand : kStand;
            if (const AnimSequence* sq = w_.data().anim.get(a->shape, stand)) {
                a->frame = sq->at(a->dir, 0).frame;
                a->last_anim = uint16_t(stand);
                a->last_anim_dir = a->dir;
            }
        }
    }
    next_turn();
}

bool TurnCombat::settled() const {
    uint16_t id = current();
    if (busy(id)) return false;
    if (w_.target_pid || w_.gumps.top_modal() || w_.ask_menu) return false;  // the player is picking / reading
    if (w_.party && !w_.party->bolts.empty()) return false;                    // a spell in flight
    if (wait_pid_)
        if (Process* p = w_.kernel().get(wait_pid_); p && p->live()) return false;
    bool running = false;
    w_.kernel().for_each([&](Process* p) {
        if (running || !p->live() || p->serial() <= snap_ || hold(*p)) return;
        if (p->type == kProcActorAnim || p->type == kProcGravity || dynamic_cast<UCProcess*>(p) ||
            dynamic_cast<SpriteProcess*>(p))
            running = true;
    });
    return !running;
}

// ---------------------------------------------------------------------------------------------
// The reachable area

const std::vector<TurnCombat::Node>& TurnCombat::reach() {
    uint16_t id = current();
    if (reach_dirty_ || reach_for_ != id) compute_reach(id);
    return reach_;
}

void TurnCombat::compute_reach(uint16_t id) {
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer {
        const std::chrono::steady_clock::time_point t0;
        const std::vector<Node>& r;
        ~Timer() {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r.size() > 1) PG_LOG("turn: reachable area %zu spots in %.1f ms", r.size(), ms);
        }
    } timer{t0, reach_};
    reach_.clear();
    reach_for_ = id;
    reach_dirty_ = false;
    const Actor* a = w_.actor(id);
    if (!a || moved_) {
        if (a) {
            Node n0;
            n0.st.x = a->x; n0.st.y = a->y; n0.st.z = a->z; n0.st.dir = a->dir;
            reach_.push_back(n0);
        }
        return;
    }
    // Leveling: a little further with DEX (game/leveling.h kMoveBonusPerDex).
    const int move_range = kMoveRange + (w_.leveling ? w_.leveling->move_bonus(*a) : 0);
    Node n0;
    n0.st.x = a->x; n0.st.y = a->y; n0.st.z = a->z;
    n0.st.dir = a->dir;
    n0.st.last_anim = a->last_anim;
    n0.st.last_anim_dir = a->last_anim_dir;
    n0.st.first_step = a->is(Actor::kFirstStep);
    n0.st.flipped = a->flags & kItemFlipped;
    n0.st.in_combat = a->is(Actor::kInCombat);
    reach_.push_back(n0);
    const unsigned action = n0.st.in_combat && w_.data().anim.get(a->shape, kAdvance) ? kAdvance : kWalk;
    auto key = [](const ActorState& s) {
        // 16-unit cells: a 300-unit Move covers ~1100 of them per level (8-unit cells took 4x
        // the spots and ~0.3 s).
        return (int64_t(s.x >> 4) << 32) ^ (int64_t(s.y >> 4) << 12) ^ int64_t((s.z >> 3) & 0xFFF);
    };
    // Each edge plays the first k frames of one walk / advance cycle from the parent's state,
    // exactly as the move will be played (so the preview is the walk). Cost = distance; the
    // queue order also counts the edges, so routes prefer long stretches of the walk.
    std::unordered_map<int64_t, int> best;  // grid cell -> best rank
    best[key(n0.st)] = 0;
    std::vector<int> edges(1, 0);
    using QE = std::pair<int, int>;  // rank, node
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    open.push({0, 0});
    AnimTracker tr(w_);
    while (!open.empty() && reach_.size() < kMaxNodes) {
        auto [rank, ni] = open.top();
        open.pop();
        if (rank > reach_[size_t(ni)].cost + 4 * edges[size_t(ni)]) continue;
        const ActorState from = reach_[size_t(ni)].st;
        const int cost = reach_[size_t(ni)].cost;
        const int ne = edges[size_t(ni)] + 1;
        for (int dir = 0; dir < 8; ++dir) {
            ActorState st = from;
            if (!tr.init(id, action, dir, &st)) return;
            int steps = 0, emitted = 0;
            while (tr.step()) {
                ++steps;
                if ((tr.blocked() && !tr.unstoppable()) || tr.unsupported()) break;
                if (std::abs(tr.z() - from.z) > 24) break;  // no climbing or dropping in a Move
                double moved = std::sqrt(double(d2(tr.x(), tr.y(), from.x, from.y)));
                if (moved < 12.0 * (emitted + 1) && !tr.done()) continue;
                if (moved < 4) break;
                ++emitted;
                int nc = cost + int(std::lround(moved));
                if (nc > move_range) break;
                ActorState ns = tr.state();
                int nr = nc + 4 * ne;
                int64_t k = key(ns);
                auto it = best.find(k);
                if (it != best.end() && it->second <= nr) continue;
                best[k] = nr;
                Node n;
                n.st = ns;
                n.parent = ni;
                n.step = PathStep{action, dir, tr.done() ? 0 : steps};
                n.cost = nc;
                reach_.push_back(n);
                edges.push_back(ne);
                open.push({nr, int(reach_.size() - 1)});
                if (reach_.size() >= kMaxNodes) break;
            }
        }
    }
}

void TurnCombat::chain(int node, std::vector<int>& out) const {
    out.clear();
    for (int n = node; n >= 0 && size_t(n) < reach_.size(); n = reach_[size_t(n)].parent) out.push_back(n);
    std::reverse(out.begin(), out.end());
}

void TurnCombat::node_point(int node, int& x, int& y, int& z) const {
    x = y = z = 0;
    if (node < 0 || size_t(node) >= reach_.size()) return;
    const Actor* a = w_.actor(reach_for_);
    int xd = 0, yd = 0, zd = 0;
    if (a) w_.footprint(*a, xd, yd, zd);
    x = reach_[size_t(node)].st.x - xd / 2;
    y = reach_[size_t(node)].st.y - yd / 2;
    z = reach_[size_t(node)].st.z;
}

void TurnCombat::start_route(int node) {
    std::vector<int> c;
    chain(node, c);
    route_.clear();
    for (size_t i = 1; i < c.size(); ++i) route_.push_back(reach_[size_t(c[i])].step);
    const Actor* a = w_.actor(current());
    hp_at_move_ = a ? a->hp : 0;
    sub_ = kMoving;
    sub_frames_ = 0;
    snap_ = w_.kernel().serial();
}

void TurnCombat::step_route() {
    uint16_t id = current();
    Actor* a = w_.actor(id);
    if (!a) { next_turn(); return; }
    bool stop = sub_frames_ > 300 || int(a->hp) < hp_at_move_ || w_.kernel().count_exact(id, kProcGravity);
    if (busy(id) && !stop) return;
    if (busy(id)) return;  // falling: wait until it lands, the move is used up
    if (!route_.empty() && !stop) {
        PathStep s = route_.front();
        route_.erase(route_.begin());
        AnimTracker tr(w_);
        if (tr.init(id, s.action, s.dir, nullptr, s.steps) && tr.run_all() == AnimTracker::kOk) {
            w_.do_anim(id, s.action, s.dir, s.steps);
            return;
        }
        PG_LOG("turn: route step failed (action %u dir %d steps %d at %d,%d)", s.action, s.dir, s.steps, a->x, a->y);
        route_.clear();  // blocked now: the move ends here
    }
    // Arrived (or stopped).
    moved_ = true;
    reach_dirty_ = true;
    route_.clear();
    if (!a->is(Actor::kDead)) w_.do_anim(id, a->is(Actor::kInCombat) ? kCombatStand : kStand, a->dir);
    const Fighter* f = fighter(id);
    if (pending_attack_) {
        uint16_t t = pending_attack_;
        pending_attack_ = 0;
        if (valid_fighter(t) && can_hit_now(id, t)) {
            do_attack(id, t);
            return;
        }
        if (f && f->controllable && sub_ != kThink && !(f->keep_auto || party_auto)) status = "Out of reach";
    }
    bool manual = f && f->controllable && !f->keep_auto && !party_auto && !w_.auto_ui;
    if (manual && auto_end && !a->is(Actor::kDead) && !has_followup(id)) {
        // Nothing useful left after the move: the turn ends by itself.
        say(name_of(id) + ": nothing more to do");
        if (kAutoEndDefends) cmd_defend_now(id);
        else finish_action();
        return;
    }
    sub_ = manual ? kChoose : kThink;
    sub_frames_ = 0;
}

bool TurnCombat::has_followup(uint16_t id) {
    const Actor* a = w_.actor(id);
    if (!a) return false;
    std::vector<uint16_t> foes = enemies_of(id);
    for (uint16_t t : foes)
        if (can_hit_now(id, t)) return true;
    for (const CastOption& o : cast_options(id)) {
        if (a->mana < o.mana) continue;
        if (o.book >= 0) { if (!o.locked) return true; continue; }  // the Avatar's book: the script asks for a target
        if (o.self) {
            bool have = false;  // a summoner with a summon out has no use for another
            if (w_.party)
                for (const Party::Summon& sm : w_.party->summons) have |= w_.actor(sm.id) != nullptr;
            if (!have) return true;
            continue;
        }
        if (o.friendly) {
            for (const Fighter& f : order_) {
                const Actor* m = w_.actor(f.id);
                if (f.party && m && !m->is(Actor::kDead) && int(m->hp) < max_hp(*m) && cast_reaches(id, o, f.id)) return true;
            }
            continue;
        }
        for (uint16_t t : foes)
            if (cast_reaches(id, o, t)) return true;
    }
    const bool hurt = int(a->hp) < max_hp(*a) || (id == 1 && a->mana < 2 * int(a->intel));
    for (uint16_t k : use_items(id)) {
        const Item* it = w_.item(k);
        if (!it) continue;
        std::string why;
        if (item_powers::is_power_shape(it->shape)) {
            if (id != 1 || !item_powers::refuse_turn_use(w_, k, why)) return true;
            continue;
        }
        if (it->shape == 228 || hurt) return true;  // the oil flask can always be thrown
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Attacks, spells, items

bool TurnCombat::can_hit_from(uint16_t id, const ActorState& s, uint16_t target) const {
    const Actor* a = w_.actor(id);
    const Item* t = w_.item(target);
    if (!a || !t) return false;
    if (const MonsterInfo* m = monster_info(a->shape); m && m->ranged && id != 1 && !(w_.party && w_.party->is_companion(id)))
        return d2(s.x, s.y, t->x, t->y) < 1500LL * 1500LL;  // the ghost "attacks" from anywhere
    if (d2(s.x, s.y, t->x, t->y) > 260LL * 260LL) return false;
    int xd, yd, zd;
    w_.footprint(*a, xd, yd, zd);
    Box tb = w_.box_of(*t);
    if (!melee_frames(w_, a->shape)) {
        // No reaching frame: next to it (the footprints within 40 units, about the same height).
        int gx = std::max({0, (tb.x - tb.xd) - s.x, (s.x - xd) - tb.x});
        int gy = std::max({0, (tb.y - tb.yd) - s.y, (s.y - yd) - tb.y});
        return std::max(gx, gy) <= 40 && std::abs(tb.z - s.z) <= 24;
    }
    int dir = direction_to((tb.x - tb.xd / 2) - (s.x - xd / 2), (tb.y - tb.yd / 2) - (s.y - yd / 2));
    AnimTracker tr(w_);
    ActorState st = s;
    if (!tr.init(id, kAttack, dir, &st)) return false;
    while (tr.step())
        if (tr.hit()) return tr.hit() == target;
    return false;
}

bool TurnCombat::can_hit_now(uint16_t id, uint16_t target) const {
    const Actor* a = w_.actor(id);
    if (!a) return false;
    ActorState s;
    s.x = a->x; s.y = a->y; s.z = a->z; s.dir = a->dir;
    s.last_anim = a->last_anim;
    s.last_anim_dir = a->last_anim_dir;
    s.first_step = a->is(Actor::kFirstStep);
    s.flipped = a->flags & kItemFlipped;
    s.in_combat = a->is(Actor::kInCombat);
    return can_hit_from(id, s, target);
}

bool TurnCombat::attack_reachable(uint16_t target) {
    uint16_t id = current();
    if (can_hit_now(id, target)) return true;
    if (moved_) return false;
    return best_node_toward(id, target, true) > 0 && can_hit_from(id, reach_[size_t(best_node_toward(id, target, true))].st, target);
}

// The reachable node to move to: one the attack would hit from (cheapest), else the closest
// to the target (no nearer than keep_away).
int TurnCombat::best_node_toward(uint16_t id, uint16_t target, bool want_hit, int keep_away) {
    const std::vector<Node>& r = reach();
    const Item* t = w_.item(target);
    if (!t || r.empty()) return -1;
    int best = -1;
    if (want_hit) {
        int best_cost = 1 << 30;
        for (size_t i = 1; i < r.size(); ++i) {
            if (r[i].cost >= best_cost) continue;
            if (d2(r[i].st.x, r[i].st.y, t->x, t->y) > 260LL * 260LL) continue;
            if (can_hit_from(id, r[i].st, target)) { best = int(i); best_cost = r[i].cost; }
        }
        if (best > 0) return best;
    }
    long long bd = d2(r[0].st.x, r[0].st.y, t->x, t->y);
    long long keep = int64_t(keep_away) * keep_away;
    for (size_t i = 1; i < r.size(); ++i) {
        long long d = d2(r[i].st.x, r[i].st.y, t->x, t->y);
        if (d < keep) continue;
        if (d < bd - 64) { bd = d; best = int(i); }
    }
    return best;
}

int TurnCombat::node_away_from(uint16_t id, uint16_t threat) {
    const std::vector<Node>& r = reach();
    const Item* t = w_.item(threat);
    if (!t || r.size() < 2) return -1;
    int best = -1;
    long long bd = d2(r[0].st.x, r[0].st.y, t->x, t->y) + 1024;
    for (size_t i = 1; i < r.size(); ++i) {
        long long d = d2(r[i].st.x, r[i].st.y, t->x, t->y);
        if (d > bd) { bd = d; best = int(i); }
    }
    (void)id;
    return best;
}

void TurnCombat::do_attack(uint16_t id, uint16_t target) {
    Actor* a = w_.actor(id);
    const Item* t = w_.item(target);
    if (!a || !t) return;
    Box tb = w_.box_of(*t), ab = w_.box_of(*a);
    int dir = direction_to((tb.x - tb.xd / 2) - (ab.x - ab.xd / 2), (tb.y - tb.yd / 2) - (ab.y - ab.yd / 2));
    snap_ = w_.kernel().serial();
    uint16_t tp = a->dir != dir ? turn_actor(w_, id, dir) : 0;
    uint16_t ap = w_.do_anim(id, kAttack, dir);
    if (tp && ap)
        if (Process* p = w_.kernel().get(ap)) p->wait_for(tp);
    if (id == 1) {
        w_.last_attack_frame = w_.kernel().frame_number();
        w_.train_stat(0, 1 + w_.rand_below(2));
        w_.train_stat(2, 2 + w_.rand_below(2));
    }
    a->target = target;
    if (w_.party && w_.party->is_companion(id)) w_.party->state[size_t(w_.party->index_of(id))].target = target;
    if (!melee_frames(w_, a->shape)) { direct_hit_ = target; direct_dir_ = dir; direct_in_ = 4; }
    say(name_of(id) + " attacks " + name_of(target), false);  // the hit / miss line follows in the log
    sub_ = kActing;
    sub_frames_ = 0;
}

std::vector<TurnCombat::CastOption> TurnCombat::cast_options(uint16_t id) const {
    std::vector<CastOption> out;
    const Actor* a = w_.actor(id);
    if (!a) return out;
    if (w_.party && w_.party->is_companion(id)) {
        int idx = w_.party->index_of(id);
        for (const std::string& sid : w_.party->defs[size_t(idx)].spells) {
            int si = w_.party->spell_index(sid);
            if (si < 0) continue;
            const SpellDef& s = w_.party->spells[size_t(si)];
            if (!s.combat || s.kind == "raise") continue;
            CastOption o;
            o.name = s.name;
            o.spell = si;
            o.mana = s.mana;
            o.range = s.range;
            o.friendly = s.kind == "heal" || s.kind == "cure";
            o.self = s.kind == "summon";
            if (w_.leveling && w_.leveling->active() && w_.leveling->magic->loaded) {
                CompanionMagic::Gate g = w_.leveling->magic->gate(idx, s.id);
                o.locked = !g.ok();
                o.why = g.why;
                o.uses = g.uses;
            }
            out.push_back(o);
        }
    } else if (id == 1 && w_.spellbook) {
        // The Avatar casts what his spellbook lists (game/spellbook.h), with its casts left; with the
        // debug option on, every spell, unlimited.
        for (const Spellbook::Spell* sp : w_.spellbook->listed()) {
            const Spellbook::Status st = w_.spellbook->status(*sp);
            CastOption o;
            o.book = sp->key();
            o.name = sp->name;
            o.self = true;  // the spell's script asks for a target itself
            // Mana is shown (and checked) only where the cast pays it: a sorcery focus charge is free.
            o.mana = w_.spellbook->debug_infinite || (sp->school == Spellbook::kSorcery && st.talismans > 0) ? 0 : sp->cost;
            o.uses = st.casts < 0 ? -1 : st.casts;
            o.locked = !st.castable;
            o.why = st.note;
            out.push_back(o);
        }
    }
    return out;
}

std::vector<uint16_t> TurnCombat::use_items(uint16_t id) const {
    std::vector<uint16_t> out;
    const Actor* a = w_.actor(id);
    if (!a) return out;
    if (id == 1) {
        collect(w_, *a, out, use_shape);
    } else {
        collect(w_, *a, out, [](unsigned s) { return s == 766u; });  // companions: potions
    }
    return out;
}

bool TurnCombat::cast_reaches(uint16_t id, const CastOption& o, uint16_t target) const {
    if (target == id || o.self || o.book >= 0) return true;
    const Actor* a = w_.actor(id);
    const Actor* t = w_.actor(target);
    return a && t && dist(*a, *t) <= o.range && w_.can_reach(id, target, o.range + 64);
}

bool TurnCombat::do_cast(uint16_t id, const CastOption& o, uint16_t target) {
    Actor* a = w_.actor(id);
    if (!a) return false;
    snap_ = w_.kernel().serial();
    if (o.book >= 0) {
        const Spellbook::Spell* sp = Spellbook::find(o.book);
        std::string why;
        if (!sp || !w_.spellbook || !w_.spellbook->cast(*sp, why)) { status = (sp ? sp->name : "Spell") + std::string(": ") + why; return false; }
        wait_pid_ = w_.spellbook->last_cast_pid;
        say(name_of(id) + " casts " + o.name);
        sub_ = kActing;
        sub_frames_ = 0;
        return true;
    }
    int idx = w_.party ? w_.party->index_of(id) : -1;
    if (idx < 0) return false;
    const SpellDef& s = w_.party->spells[size_t(o.spell)];
    std::string why;
    if (!w_.party->spell_ready(idx, o.spell, &why)) {
        status = s.name + ": " + why;
        return false;
    }
    if (a->mana < s.mana) { status = "Not enough mana"; return false; }
    uint16_t tgt = o.self ? (target ? target : id) : target;
    if (!o.self && tgt != id && !cast_reaches(id, o, tgt)) { status = "Out of range"; return false; }
    if (!w_.party->cast(idx, o.spell, tgt)) { status = "The spell fails"; return false; }
    say(name_of(id) + " casts " + s.name + (tgt != id ? " at " + name_of(tgt) : ""));
    sub_ = kActing;
    sub_frames_ = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Commands

bool TurnCombat::cmd_move(int node) {
    if (!awaiting_player() && sub_ != kThink) return false;
    if (moved_) { status = "Already moved this turn"; return false; }
    const std::vector<Node>& r = reach();
    if (node <= 0 || size_t(node) >= r.size()) { status = "Can't go there"; return false; }
    say(name_of(current()) + " moves");
    start_route(node);
    return true;
}

bool TurnCombat::cmd_attack(uint16_t target) {
    if (!awaiting_player() && sub_ != kThink) return false;
    uint16_t id = current();
    const Fighter* tf = fighter(target);
    const Fighter* me = fighter(id);
    if (!tf || !me || tf->party == me->party || !valid_fighter(target)) { status = "Not a foe"; return false; }
    if (can_hit_now(id, target)) { do_attack(id, target); return true; }
    if (!moved_) {
        int n = best_node_toward(id, target, true);
        if (n > 0 && can_hit_from(id, reach_[size_t(n)].st, target)) {
            pending_attack_ = target;
            say(name_of(id) + " closes in on " + name_of(target));
            start_route(n);
            return true;
        }
    }
    status = "Out of reach";
    return false;
}

bool TurnCombat::cmd_cast(const CastOption& o, uint16_t target) {
    if (!awaiting_player() && sub_ != kThink) return false;
    return do_cast(current(), o, target);
}

bool TurnCombat::cmd_use(uint16_t item) {
    if (!awaiting_player() && sub_ != kThink) return false;
    uint16_t id = current();
    Actor* a = w_.actor(id);
    Item* it = w_.item(item);
    if (!a || !it) return false;
    if (id == 1 && item_powers::refuse_turn_use(w_, item, status)) return false;  // on cooldown: no turn lost
    snap_ = w_.kernel().serial();
    if (id == 1) {
        wait_pid_ = w_.call_event(item, kEvUse);
    } else if (it->shape == 766) {
        // Companions drink potions engine-side (no usecode runs on them): a third of their health.
        int heal = std::max(5, max_hp(*a) / 3);
        int before = a->hp;
        a->hp = uint16_t(std::min(max_hp(*a), int(a->hp) + heal));
        w_.game_log.on_heal(w_, id, id, int(a->hp) - before, "potion");
        if (it->quality > 1 && (w_.data().typeflags.get(it->shape).family() == kFamQuantity)) --it->quality;
        else w_.destroy(item);
    }
    std::string power_name = item_powers::display_name(w_, *it);
    say(name_of(id) + " uses " + (power_name.empty() ? w_.data().usecode.class_name(it->shape) : power_name));
    sub_ = kActing;
    sub_frames_ = 0;
    return true;
}

bool TurnCombat::cmd_defend() {
    if (!awaiting_player() && sub_ != kThink) return false;
    cmd_defend_now(current());
    return true;
}

void TurnCombat::cmd_defend_now(uint16_t id) {
    if (Fighter* f = fighter(id)) f->defending = true;
    snap_ = w_.kernel().serial();
    if (id == 1) w_.do_anim(1, kStartBlock, w_.avatar()->dir);
    say(name_of(id) + " defends");
    sub_ = kActing;
    sub_frames_ = 0;
}

bool TurnCombat::cmd_wait() {
    if (!awaiting_player()) return false;
    Fighter* f = fighter(current());
    if (!f) return false;
    if (f->waited || size_t(idx_) + 1 >= order_.size()) { status = "Nobody left to wait for"; return false; }
    Fighter copy = *f;
    copy.waited = true;
    order_.erase(order_.begin() + idx_);
    order_.push_back(copy);
    say(name_of(copy.id) + " waits");
    --idx_;
    next_turn();
    return true;
}

bool TurnCombat::cmd_auto(bool keep) {
    if (!awaiting_player()) return false;
    if (keep) toggle_keep_auto(current());
    sub_ = kThink;
    sub_frames_ = 0;
    return true;
}

bool TurnCombat::cmd_end_turn() {
    if (!awaiting_player()) return false;
    say(name_of(current()) + " ends the turn");
    finish_action();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Auto and monster AI: one decision each time it is called (a move, then again for the action).

bool TurnCombat::ai_heal(Actor& a) {
    if (!w_.party || !w_.party->is_companion(a.id)) return false;
    int idx = w_.party->index_of(a.id);
    if (!w_.party->defs[size_t(idx)].healer) return false;
    uint16_t worst = 0;
    int worst_pct = 50;
    for (const Fighter& f : order_) {
        const Actor* m = w_.actor(f.id);
        if (!f.party || !m || m->is(Actor::kDead)) continue;
        int pct = int(m->hp) * 100 / std::max(1, max_hp(*m));
        if (pct < worst_pct) { worst_pct = pct; worst = f.id; }
    }
    if (!worst) return false;
    for (const CastOption& o : cast_options(a.id)) {
        if (!o.friendly || a.mana < o.mana || o.locked) continue;
        const Actor* t = w_.actor(worst);
        if (worst != a.id && (dist(a, *t) > o.range || !w_.can_reach(a.id, worst, o.range + 64))) {
            if (!moved_) {
                int n = best_node_toward(a.id, worst, false);
                if (n > 0) { start_route(n); return true; }
            }
            continue;
        }
        if (do_cast(a.id, o, worst)) return true;
    }
    return false;
}

// Casters: the weakest attack spell that can kill or badly hurt the target, else the strongest.
bool TurnCombat::ai_cast(Actor& a, uint16_t target) {
    const Actor* t = w_.actor(target);
    if (!t || !w_.party || !w_.party->is_companion(a.id)) return false;
    int idx = w_.party->index_of(a.id);
    if (!w_.party->defs[size_t(idx)].caster) return false;
    const CastOption* pick = nullptr;
    std::vector<CastOption> opts = cast_options(a.id);
    int pick_mana = 1 << 30, strongest = -1;
    for (const CastOption& o : opts) {
        if (o.friendly || a.mana < o.mana || o.locked || !w_.party->spell_ready(idx, o.spell, nullptr, target)) continue;
        const SpellDef& s = w_.party->spells[size_t(o.spell)];
        if (s.kind == "summon") {
            bool have = false;
            for (const Party::Summon& sm : w_.party->summons) have |= w_.actor(sm.id) != nullptr;
            if (have) continue;
        }
        int avg = (s.dmg0 + s.dmg1) / 2;
        bool good = s.dmg1 >= int(t->hp) || 2 * avg >= int(t->hp);
        if (good && o.mana < pick_mana) { pick = &o; pick_mana = o.mana; }
        if (avg > strongest) {
            strongest = avg;
            if (!pick || !good) {}
        }
    }
    if (!pick) {
        for (const CastOption& o : opts) {
            if (o.friendly || a.mana < o.mana || o.locked || !w_.party->spell_ready(idx, o.spell, nullptr, target)) continue;
            const SpellDef& s = w_.party->spells[size_t(o.spell)];
            if ((s.dmg0 + s.dmg1) / 2 == strongest && s.kind != "summon") { pick = &o; break; }
        }
    }
    if (!pick) return false;
    bool in_range = pick->self || (dist(a, *t) <= pick->range && w_.can_reach(a.id, target, pick->range + 64));
    if (!in_range) {
        if (moved_) return false;
        int n = best_node_toward(a.id, target, false, 128);
        if (n > 0) { start_route(n); return true; }
        return false;
    }
    return do_cast(a.id, *pick, target);
}

void TurnCombat::ai_think() {
    uint16_t id = current();
    Actor* a = w_.actor(id);
    const Fighter* f = fighter(id);
    if (!a || !f) { next_turn(); return; }
    // The new creatures take their whole turn by their own rules (game/creatures.h).
    if (!f->party && w_.creatures && w_.creatures->turn_act(id)) {
        if (!w_.creatures->log.empty()) status = name_of(id) + " acts";
        finish_action();
        return;
    }
    std::vector<uint16_t> foes = enemies_of(id);
    if (foes.empty()) { finish_action(); return; }
    // Target: monsters keep the one the real-time AI chose while it is in the fight.
    uint16_t target = foes.front();
    if (!f->party && a->target && std::find(foes.begin(), foes.end(), a->target) != foes.end()) {
        const Actor* cur = w_.actor(a->target);
        const Actor* nf = w_.actor(foes.front());
        if (dist(*a, *cur) < dist(*a, *nf) + 160) target = a->target;
    }
    if (f->party && w_.party && w_.party->is_companion(id)) {
        uint16_t st = w_.party->state[size_t(w_.party->index_of(id))].target;
        if (st && std::find(foes.begin(), foes.end(), st) != foes.end()) target = st;
    }
    if (f->party) {
        if (ai_heal(*a)) return;
        // Badly hurt: back off, then defend.
        if (int(a->hp) * 4 < max_hp(*a)) {
            if (!moved_) {
                int n = node_away_from(id, foes.front());
                if (n > 0) { say(name_of(id) + " falls back"); start_route(n); return; }
            }
            cmd_defend();
            return;
        }
        if (ai_cast(*a, target)) return;
    }
    if (can_hit_now(id, target)) { do_attack(id, target); return; }
    // Another foe in reach right now beats walking to the chosen one.
    for (uint16_t o : foes)
        if (o != target && can_hit_now(id, o)) { do_attack(id, o); return; }
    if (!moved_) {
        int n = best_node_toward(id, target, true);
        if (n > 0) {
            if (can_hit_from(id, reach_[size_t(n)].st, target)) pending_attack_ = target;
            start_route(n);
            return;
        }
    }
    // Nothing to do this turn.
    if (f->party) cmd_defend();
    else finish_action();
}

}  // namespace pg
