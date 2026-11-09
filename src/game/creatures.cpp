// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The new creatures (PLAN-CREATURES.md; NOTES.md "New creatures"). See creatures.h.
#include "game/creatures.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#include "core/config.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/critpath.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/virtues.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using json = nlohmann::json;

namespace {

long long d2(int ax, int ay, int bx, int by) { return (long long)(ax - bx) * (ax - bx) + (long long)(ay - by) * (ay - by); }
bool is_water(unsigned shape) { return shape == 347 || shape == 438; }
bool is_lava(unsigned shape) { return shape == 409; }
bool is_food(const Item& it) { return it.shape == 537 || it.shape == 538 || it.shape == 624; }
constexpr int kTurnMove = 300;      // as far as a turn-based Move (TurnCombat::kMoveRange)
constexpr int kLeash = 1400;        // never strays further than this from where it rose
constexpr int kDeadFrames = 45;     // a dead creature fades, then its body goes
constexpr int kSayFrames = 60;      // one rule line per creature per 2 s

CreatureDraw read_draw(const json& j) {
    CreatureDraw d;
    if (j.is_string() && j.get<std::string>() == "own") {
        d.own = d.set = true;
        return d;
    }
    if (!j.is_array() || j.size() < 3) return d;
    d.shape = j[0].get<unsigned>();
    d.first = j[1].get<int>();
    d.last = j[2].get<int>();
    d.ticks = j.size() > 3 ? std::max(1, j[3].get<int>()) : 4;
    d.set = true;
    return d;
}

bool read_all(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::unique_ptr<CritPath>& crit_for_creatures() {
    static std::unique_ptr<CritPath> cp;
    return cp;
}
std::string& creatures_dir() {
    static std::string d;
    return d;
}

}  // namespace

Creatures::Creatures(World& w) : w_(w) {}

void Creatures::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World& w) {
        Creatures* c = w.creatures.get();
        MenuGump::OptionRow r{"New creatures:", [c] { return std::string(c && c->enabled ? "On" : "Off"); },
                              [c](int) {
                                  if (!c) return;
                                  c->enabled = !c->enabled;
                                  Config& cf = Config::global();
                                  cf.set_bool("new_creatures", c->enabled);
                                  if (!cf.path().empty()) cf.save();
                              }};
        r.group = "Game";
        return r;
    });
}

// ---------------------------------------------------------------------------------------------
// Data

bool Creatures::load_dir(const std::string& dir, std::string& err) {
    std::string text;
    if (!read_all(dir + "/creatures.json", text)) { err = dir + "/creatures.json not found"; return false; }
    creatures_dir() = dir;
    return load_text(text, err);
}

bool Creatures::load_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "creatures.json: not valid JSON"; return false; }
    defs.clear();
    spell_tags.clear();
    magic_weapons.clear();
    metal_armour.clear();
    try {
        for (const json& m : j.value("magic_weapons", json::array())) magic_weapons.insert(m.get<unsigned>());
        for (const json& m : j.value("metal_armour", json::array()))
            metal_armour.emplace_back(m.at(0).get<unsigned>(), m.at(1).get<int>());
        if (j.contains("spell_tags"))
            for (auto it = j["spell_tags"].begin(); it != j["spell_tags"].end(); ++it)
                for (const json& t : it.value()) spell_tags[it.key()].push_back(t.get<std::string>());
        for (const json& c : j.at("creatures")) {
            CreatureDef d;
            d.id = c.at("id").get<std::string>();
            d.name = c.at("name").get<std::string>();
            d.look = c.value("look", d.name);
            d.kind = c.value("kind", std::string("walker"));
            d.body = c.at("body").get<unsigned>();
            d.level = std::clamp(c.value("level", 1), 1, kMaxLevel);
            d.xp = c.value("xp", 0);
            auto range = [&](const char* k, int& lo, int& hi) {
                if (c.contains(k)) { lo = c[k][0].get<int>(); hi = std::max(lo, c[k][1].get<int>()); }
            };
            range("hp", d.hp0, d.hp1);
            range("dex", d.dex0, d.dex1);
            range("damage", d.dmg0, d.dmg1);
            d.hp0 = std::clamp(d.hp0, 1, 999);
            d.hp1 = std::clamp(d.hp1, d.hp0, 999);
            d.str = c.value("str", 0);
            d.armour = c.value("armour", 0);
            d.dmg_type = uint16_t(c.value("damage_type", 1));
            if (c.contains("attack")) {
                const json& a = c["attack"];
                d.attack = a.value("name", d.attack);
                d.reach = a.value("reach", d.reach);
                d.every = std::max(10, a.value("every", d.every));
                d.sound = a.value("sound", -1);
            }
            if (c.contains("lash")) {
                const json& a = c["lash"];
                d.lash = a.value("name", d.lash);
                d.lash_reach = a.value("reach", 0);
                d.lash_type = uint16_t(a.value("damage_type", 4));
            }
            d.speed = c.value("speed", d.speed);
            d.aggro = c.value("aggro", d.aggro);
            if (c.contains("surface")) {
                const json& s = c["surface"];
                d.up_frames = s.value("up_frames", d.up_frames);
                d.down_frames = s.value("down_frames", d.down_frames);
                d.bait_frames = s.value("bait_frames", 0);
            }
            d.mana_drain = c.value("mana_drain", 0);
            if (c.contains("arc")) {
                d.arc_range = c["arc"].value("range", 0);
                d.arc_pct = c["arc"].value("pct", 0);
                d.arc_metal_range = c["arc"].value("metal_range", d.arc_range);
            }
            d.metal_draws = c.value("metal_draws", false);
            d.fire_protect = c.value("fire_protect", false);
            d.lava_ok = c.value("lava_ok", false);
            if (c.contains("weakened_by")) {
                const json& wb = c["weakened_by"];
                d.weakened_by = wb.value("spell", std::string());
                d.weak_frames = 30 * wb.value("seconds", 20);
                d.weak_rounds = wb.value("rounds", 3);
                d.weak_say = wb.value("say", std::string());
            }
            for (const json& r : c.value("rules", json::array())) {
                CreatureRule cr;
                for (const json& t : r.at("when")) cr.when.push_back(t.get<std::string>());
                cr.pct = r.value("pct", 100);
                cr.say = r.value("say", std::string());
                d.rules.push_back(cr);
            }
            if (c.contains("draw")) {
                const json& dr = c["draw"];
                if (dr.contains("hidden")) d.draw_hidden = read_draw(dr["hidden"]);
                if (dr.contains("rising")) d.draw_rising = read_draw(dr["rising"]);
                if (dr.contains("up")) d.draw_up = read_draw(dr["up"]);
                if (dr.contains("sinking")) d.draw_sinking = read_draw(dr["sinking"]);
                if (dr.contains("strike")) d.draw_strike = read_draw(dr["strike"]);
                if (dr.contains("particle")) d.particle = read_draw(dr["particle"]);
                d.particle_every = dr.value("particle_every", 0);
                d.ghost = dr.value("ghost", false);
                d.tint = dr.value("tint", std::string());
            }
            for (const json& p : c.value("places", json::array())) {
                CreaturePlace cp;
                cp.map = p.at("map").get<unsigned>();
                cp.x = p.at("x").get<int>();
                cp.y = p.at("y").get<int>();
                cp.z = p.at("z").get<int>();
                cp.after = p.value("after", std::string());
                cp.note = p.value("note", std::string());
                d.places.push_back(cp);
            }
            defs.push_back(d);
        }
    } catch (const std::exception& e) {
        err = std::string("creatures.json: ") + e.what();
        defs.clear();
        return false;
    }
    register_rows();
    build_tint();
    return true;
}

void Creatures::register_rows() {
    std::vector<MonsterInfo>& rows = extra_monster_rows();
    for (const CreatureDef& d : defs) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const MonsterInfo& m) { return m.shape == d.body; }), rows.end());
        // (the monster row holds HP in a byte: with Leveling on the variant below gives the full numbers)
        MonsterInfo m{uint16_t(d.body), uint8_t(std::min(255, d.hp0)), uint8_t(std::min(255, d.hp1)), uint8_t(d.dex0), uint8_t(d.dex1), uint8_t(d.dmg0),
                      uint8_t(d.dmg1), uint8_t(d.armour), 0x24, d.dmg_type, 0, false, false, false, 0, ""};
        rows.push_back(m);
        if (w_.leveling) {
            MonsterLevel ml;
            ml.name = d.name;
            ml.level = d.level;
            ml.xp = d.xp;
            MonsterVariant v;
            v.level = d.level;
            v.xp = d.xp;
            v.has_stats = true;
            v.hp0 = d.hp0; v.hp1 = d.hp1; v.dex0 = d.dex0; v.dex1 = d.dex1; v.str = d.str; v.intel = 0;
            v.dmg0 = d.dmg0; v.dmg1 = d.dmg1;
            ml.variants.push_back(v);
            w_.leveling->monsters[d.body] = ml;
        }
    }
}

void Creatures::build_tint() {
    // A smoky dark recolour: each colour's brightness onto a dim grey-violet ramp.
    const Palette& p = w_.data().palette;
    dark_.assign(256, 0);
    for (int i = 0; i < 256; ++i) {
        const int l = (p.rgb8[i][0] * 3 + p.rgb8[i][1] * 4 + p.rgb8[i][2] * 2) / 9;
        const int r = 14 + l * 28 / 100, g = 10 + l * 24 / 100, b = 22 + l * 36 / 100;
        int best = 0, bd = INT_MAX;
        for (int k = 0; k < 256; ++k) {
            if (k >= 8 && k <= 15) continue;  // never a translucency index
            int dr = p.rgb8[k][0] - r, dg = p.rgb8[k][1] - g, db = p.rgb8[k][2] - b;
            int dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (dd < bd) { bd = dd; best = k; }
        }
        dark_[size_t(i)] = uint8_t(best);
    }
    dark_[0] = 0;
}

int Creatures::def_index(const std::string& id) const {
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].id == id) return int(i);
    return -1;
}

const CreatureDef* Creatures::def_of(uint16_t actor) const {
    auto it = live.find(actor);
    if (it == live.end() || it->second.def < 0 || size_t(it->second.def) >= defs.size()) return nullptr;
    return &defs[size_t(it->second.def)];
}

// ---------------------------------------------------------------------------------------------
// Names and looks

std::string creature_display_name(const World& w, uint16_t id) {
    if (w.creatures)
        if (const CreatureDef* d = w.creatures->def_of(id)) return d->name;
    const Actor* a = w.actor(id);
    if (!a || !w.leveling) return std::string();
    std::string pre = w.leveling->weak_prefix(*a);
    if (pre.empty()) return std::string();
    std::string n = w.data().usecode.class_name(a->shape), out;
    if (n.empty()) return std::string();
    for (char c : n) out += c == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(c)));
    return pre + out;
}

bool Creatures::look_event(uint16_t id) {
    if (const CreatureDef* d = def_of(id)) {
        w_.bark(id, d->look);
        return true;
    }
    std::string n = creature_display_name(w_, id);  // a weaker variant: "young troll"
    if (n.empty()) return false;
    w_.bark(id, n);
    return true;
}

const CreatureDraw& Creatures::drawing(const CreatureState& st, const CreatureDef& d) const {
    auto pick = [&](const CreatureDraw& a, const CreatureDraw& b) -> const CreatureDraw& { return a.set ? a : b; };
    switch (st.phase) {
    case CreatureState::kHidden: return pick(d.draw_hidden, d.draw_up);
    case CreatureState::kRising: return pick(d.draw_rising, d.draw_up);
    case CreatureState::kSinking: return pick(d.draw_sinking, d.draw_hidden.set ? d.draw_hidden : d.draw_up);
    default: return d.draw_up;
    }
}

bool Creatures::appearance(uint16_t id, WorldView::Options::Look& look) const {
    auto it = live.find(id);
    if (it == live.end()) return false;
    const CreatureState& st = it->second;
    const CreatureDef* d = def_of(id);
    const Actor* a = w_.actor(id);
    if (!d || !a) return false;
    const uint32_t now = w_.game_frames();
    look.ghost = d->ghost;
    if (d->tint == "dark" && !dark_.empty()) look.remap = dark_.data();
    if (a->is(Actor::kDead)) {
        // It breaks up: the sinking drawing once, then nothing.
        const CreatureDraw& s = d->draw_sinking.set ? d->draw_sinking : d->draw_up;
        const int n = std::max(1, s.last - s.first + 1);
        const int k = st.dead_frames / std::max(1, s.ticks);
        if (k >= n || s.own) { look.hide = st.dead_frames > 10 || !s.own; if (s.own && !look.hide) { look.ghost = true; return true; } return true; }
        look.shape = uint16_t(s.shape);
        look.frame = uint16_t(s.first + k);
        return true;
    }
    const CreatureDraw& dr = drawing(st, *d);
    if (!dr.set) return false;
    if (dr.own) {
        look.shape = uint16_t(d->body);
        if (const AnimSequence* seq = w_.data().anim.get(d->body, 2)) look.frame = uint16_t(seq->at(a->dir & 7, 0).frame);
        return true;
    }
    const int n = std::max(1, dr.last - dr.first + 1);
    int k;
    if (st.phase == CreatureState::kRising || st.phase == CreatureState::kSinking) {
        const int total = n * dr.ticks;
        k = std::clamp((total - st.timer) / dr.ticks, 0, n - 1);
    } else {
        k = int((now / unsigned(dr.ticks) + unsigned(id) * 7u) % unsigned(n));
    }
    look.shape = uint16_t(dr.shape);
    look.frame = uint16_t(dr.first + k);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Spawning

uint16_t Creatures::spawn_at(int di, int x, int y, int z) {
    if (di < 0 || size_t(di) >= defs.size()) return 0;
    const CreatureDef& d = defs[size_t(di)];
    if (!w_.data().shapes.shape(d.body)) return 0;  // no body shape (data/shapes/ not loaded)
    if (!w_.leveling || !w_.leveling->monsters.count(d.body) || !monster_info(d.body)) register_rows();
    uint16_t id = w_.create_monster(d.body, 0);
    Actor* a = w_.actor(id);
    if (!a) return 0;
    a->dir = uint8_t(w_.rand_below(8));
    w_.move_to(id, x, y, z, false);
    CreatureState st;
    st.actor = id;
    st.def = di;
    st.home_x = x;  // where it rose (the leash)
    st.home_y = y;
    st.phase = (d.kind == "water" || d.kind == "lava") ? CreatureState::kHidden : CreatureState::kUp;
    st.timer = d.down_frames / 2;
    live[id] = st;
    log.push_back("spawn " + d.id + " " + std::to_string(id));
    PG_LOG("creatures: %s (actor %u) at %d,%d,%d map %u", d.id.c_str(), id, x, y, z, a->mapnum);
    return id;
}

uint16_t Creatures::spawn(int di, int place) {
    if (di < 0 || size_t(di) >= defs.size() || place < 0 || size_t(place) >= defs[size_t(di)].places.size()) return 0;
    const CreaturePlace& p = defs[size_t(di)].places[size_t(place)];
    uint16_t id = spawn_at(di, p.x, p.y, p.z);
    if (id) {
        live[id].place = place;
        spawned.insert(defs[size_t(di)].id + ":" + std::to_string(place));
    }
    return id;
}

void Creatures::spawn_here() {
    const Actor* av = w_.avatar();
    if (!av || w_.stasis) return;
    std::vector<bool> done;
    bool have_done = false;
    for (size_t di = 0; di < defs.size(); ++di)
        for (size_t pi = 0; pi < defs[di].places.size(); ++pi) {
            const CreaturePlace& p = defs[di].places[pi];
            if (p.map != av->mapnum || p.map != w_.map_number_now()) continue;
            if (spawned.count(defs[di].id + ":" + std::to_string(pi))) continue;
            if (!p.after.empty()) {
                auto& cp = crit_for_creatures();
                if (!cp) {
                    cp = std::make_unique<CritPath>();
                    std::string err;
                    cp->load_file((creatures_dir().empty() ? std::string("data") : creatures_dir()) + "/critpath.json", err);
                }
                const int k = cp->step_index(p.after);
                if (k < 0) continue;
                if (!have_done) {
                    WorldCritContext ctx(w_);
                    done = cp->complete(cp->done(ctx));
                    have_done = true;
                }
                if (size_t(k) >= done.size() || !done[size_t(k)]) continue;
            }
            spawn(int(di), int(pi));
        }
}

void Creatures::reset() {
    live.clear();
    spawned.clear();
    last_map_ = 0xFFFF;
}

// ---------------------------------------------------------------------------------------------
// Movement

bool Creatures::habitat_ok(const CreatureDef& d, int x, int y, int z, uint16_t self, int& out_z) const {
    const ShapeInfo& si = w_.data().typeflags.get(d.body);
    int xd, yd, zd;
    si.footprint(false, xd, yd, zd);
    const int cx = x - xd / 2, cy = y - yd / 2;
    for (int dz : {0, 8, -8, 16, -16, 24, -24}) {
        const int cz = z + dz;
        World::Placement p = w_.can_exist(x, y, cz, xd, yd, zd, si.flags(), self);
        if (!p.valid) continue;
        // What lies right under its middle, and how high.
        std::vector<uint16_t> near;
        w_.area_search(cx, cy, 320, false, near);
        unsigned floor = 0;
        int top = -10000;
        for (uint16_t k : near) {
            const Item* o = w_.item(k);
            if (!o || o->parent || k == self || k < 256 || (o->ext_flags & kExtSprite)) continue;
            const ShapeInfo& os = w_.data().typeflags.get(o->shape);
            if (!os.is(ShapeInfo::kSolid) && !os.is(ShapeInfo::kLand)) continue;
            int oxd, oyd, ozd;
            os.footprint(o->flags & kItemFlipped, oxd, oyd, ozd);
            if (!(cx > o->x - oxd && cx <= o->x && cy > o->y - oyd && cy <= o->y)) continue;
            const int t = o->z + ozd;
            if (t > cz || t <= top) continue;
            top = t;
            floor = o->shape;
        }
        if (top != cz) continue;
        bool ok;
        if (d.kind == "water") ok = is_water(floor);
        else if (d.kind == "lava") ok = is_lava(floor);
        else ok = p.supported && !is_water(floor) && (!is_lava(floor) || d.lava_ok);
        if (!ok) continue;
        out_z = cz;
        return true;
    }
    return false;
}

bool Creatures::step_toward(Actor& a, const CreatureDef& d, int tx, int ty, int dist_max, int stop_at) {
    auto sit = live.find(a.id);
    const int hx = sit != live.end() ? sit->second.home_x : a.x, hy = sit != live.end() ? sit->second.home_y : a.y;
    bool moved = false;
    int left = dist_max;
    while (left > 0) {
        const long long dd = d2(a.x, a.y, tx, ty);
        if (dd <= (long long)stop_at * stop_at) break;
        const int dir = direction_to(tx - a.x, ty - a.y);
        const int step = std::min(left, 8);
        bool ok = false;
        for (int turn : {0, 1, -1, 2, -2}) {
            const int dr = (dir + turn + 8) & 7;
            const int nx = a.x + dir_x(dr) * step, ny = a.y + dir_y(dr) * step;
            if (d2(nx, ny, hx, hy) > (long long)kLeash * kLeash) continue;
            if (turn && d2(nx, ny, tx, ty) >= dd) continue;  // a side step must still get nearer
            int nz;
            if (!habitat_ok(d, nx, ny, a.z, a.id, nz)) continue;
            w_.move_to(a.id, nx, ny, nz, false);
            a.dir = uint8_t(dr);
            ok = moved = true;
            break;
        }
        if (!ok) break;
        left -= step;
    }
    return moved;
}

int Creatures::dist_to(const Actor& a, const Actor& t) const {
    // Box to box on the ground plane (a long serpent reaches from its middle).
    Box ab = w_.box_of(a), tb = w_.box_of(t);
    const int gx = std::max({0, (tb.x - tb.xd) - ab.x, (ab.x - ab.xd) - tb.x});
    const int gy = std::max({0, (tb.y - tb.yd) - ab.y, (ab.y - ab.yd) - tb.y});
    return int(std::sqrt(double(gx) * gx + double(gy) * gy));
}

int Creatures::metal_pieces(uint16_t actor) const {
    int n = 0;
    for (unsigned s = 1; s <= 5; ++s) {
        const Item* it = w_.item(w_.equipped(actor, s));
        if (!it) continue;
        for (const auto& m : metal_armour)
            if (m.first == it->shape && (m.second < 0 || (m.second & ~1) == (it->frame & ~1))) { ++n; break; }
    }
    return n;
}

uint16_t Creatures::pick_target(const Actor& a, const CreatureDef& d, int range) const {
    const Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead) || av->mapnum != a.mapnum) return 0;
    std::vector<uint16_t> cand{1};
    if (w_.party && w_.party->enabled) {
        for (int m : w_.party->members) {
            const CompanionState& cs = w_.party->state[size_t(m)];
            if (cs.mode == CompanionState::kFollow && !cs.hidden) cand.push_back(cs.id);
        }
        for (const Party::Summon& s : w_.party->summons) cand.push_back(s.id);
    }
    uint16_t best = 0;
    long long bs = LLONG_MAX;
    for (uint16_t c : cand) {
        const Actor* o = w_.actor(c);
        if (!o || o->is(Actor::kDead) || o->mapnum != a.mapnum || o->chunk < 0) continue;
        if (w_.party && w_.party->untouchable(c)) continue;
        if (std::abs(o->z - a.z) > 160) continue;
        const int dd = dist_to(a, *o);
        if (dd > range) continue;
        long long score = dd;
        if (d.metal_draws) score -= 160LL * metal_pieces(c);  // metal armour draws its strikes
        if (score < bs) { bs = score; best = c; }
    }
    return best;
}

bool Creatures::fire_protected(const Actor& v) const {
    // As the game's lava and BUBLAST: an Endure Heat aura (703) within 64. Our addition: the Fire
    // Shield ring (569) within 96, and standing in the Armor of Flames (400 frame 10 and up).
    std::vector<uint16_t> near;
    w_.area_search(v.x, v.y, 160, false, near);
    for (uint16_t k : near) {
        const Item* o = w_.item(k);
        if (!o || o->parent) continue;
        const long long dd = d2(o->x, o->y, v.x, v.y);
        if (o->shape == 703 && dd <= 64LL * 64 * 2) return true;
        if (o->shape == 569 && dd <= 96LL * 96 * 2) return true;
        if (o->shape == 400 && o->frame >= 10 && o->frame < 20 && dd <= 48LL * 48 * 2) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Striking

void Creatures::strike(uint16_t id, uint16_t target) {
    Actor* a = w_.actor(id);
    Actor* t = w_.actor(target);
    const CreatureDef* d = def_of(id);
    if (!a || !t || !d || a->is(Actor::kDead) || t->is(Actor::kDead)) return;
    a->dir = uint8_t(direction_to(t->x - a->x, t->y - a->y));
    a->target = target;
    ++strikes;
    log.push_back("strike " + std::to_string(target));
    struct Ctx {
        GameLog& g;
        ~Ctx() { g.spell_context.clear(); }
    } ctx{w_.game_log};
    const int dmg = std::max(1, attack_damage(w_, *a));
    const int dir = direction_to(a->x - t->x, a->y - t->y);
    const bool lash = d->lash_reach > 0 && dist_to(*a, *t) <= d->lash_reach;  // close: the lash, not the spray
    const std::string attack = lash ? d->lash : d->attack;
    const uint16_t type = lash ? d->lash_type : d->dmg_type;
    auto hit = [&](uint16_t v, int amount) {
        Actor* va = w_.actor(v);
        if (!va || va->is(Actor::kDead)) return;
        if (d->draw_strike.set && !lash)
            w_.kernel().add(std::make_unique<SpriteProcess>(w_, d->draw_strike.shape, d->draw_strike.first, d->draw_strike.last, 1,
                                                            std::max(1, d->draw_strike.ticks / 2), va->x, va->y, va->z + 8));
        if (d->fire_protect && !lash && fire_protected(*va)) {
            w_.game_log.add(GameLog::kCombat, GameLog::kBlock, GameLog::name_of(w_, v, false) + " is shielded from the " + attack + ".", v);
            log.push_back("shielded " + std::to_string(v));
            return;
        }
        w_.game_log.spell_context = attack;
        w_.receive_hit(v, id, dir, std::max(1, amount), type);
        w_.game_log.spell_context.clear();
    };
    hit(target, dmg);
    if (d->sound >= 0) w_.sound.play_sfx(d->sound, SoundSystem::kDefaultPriority, id);
    // Arcs: to everyone of the party close to the one struck (metal armour draws it further).
    if (d->arc_range > 0) {
        std::vector<uint16_t> cand{1};
        if (w_.party)
            for (int m : w_.party->members) cand.push_back(w_.party->state[size_t(m)].id);
        if (w_.party)
            for (const Party::Summon& s : w_.party->summons) cand.push_back(s.id);
        const Actor* t2 = w_.actor(target);
        for (uint16_t c : cand) {
            if (c == target || !t2) continue;
            const Actor* o = w_.actor(c);
            if (!o || o->is(Actor::kDead) || o->mapnum != a->mapnum || o->chunk < 0) continue;
            if (c != 1 && !w_.same_party(1, c)) continue;
            const int r = metal_pieces(c) ? d->arc_metal_range : d->arc_range;
            if (d2(o->x, o->y, t2->x, t2->y) > (long long)r * r || std::abs(o->z - t2->z) > 64) continue;
            ++arcs;
            log.push_back("arc " + std::to_string(c));
            hit(c, dmg * d->arc_pct / 100);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Being hit: the rules

std::vector<std::string> Creatures::hit_tags(uint16_t victim, uint16_t attacker, uint16_t type, bool weapon) const {
    std::vector<std::string> t;
    auto it = live.find(victim);
    if (it != live.end()) {
        const CreatureDef* d = def_of(victim);
        if (d && it->second.phase != CreatureState::kUp && !it->second.trapped && (d->kind == "water" || d->kind == "lava"))
            t.push_back("hidden");
        if (it->second.weak_frames > 0 || it->second.weak_rounds > 0) t.push_back("weakened");
    }
    t.push_back(weapon ? "weapon" : "spell");
    if (type & kDmgBlade) t.push_back("blade");
    if (type & kDmgBlunt) t.push_back("blunt");
    if (type & kDmgFire) t.push_back("fire");
    if (type & kDmgUndead) t.push_back("undead");
    if (type & kDmgMagic) t.push_back("magic");
    if (type & kDmgPierce) t.push_back("pierce");
    const Item* ai = w_.item(attacker);
    const Actor* aa = ai ? ai->as_actor() : nullptr;
    if (weapon && aa) {
        const bool gear = attacker == 1 || (w_.party && w_.party->is_companion(attacker));
        if (gear) {
            const Item* wp = w_.item(w_.equipped(attacker, 6));
            const WeaponInfo* wi = wp ? weapon_info(wp->shape) : nullptr;
            if (wp && (magic_weapons.count(wp->shape) || (wi && (wi->type & kDmgMagic)))) t.push_back("magic_weapon");
        } else if (type & kDmgMagic) {
            t.push_back("magic_weapon");  // a daemon's blow (combat.md: the only monster with magic damage)
        }
    }
    if (!weapon) {
        const std::string& ctx = w_.game_log.spell_context;
        if (!ctx.empty() && w_.party)
            for (const SpellDef& s : w_.party->spells)
                if (s.name == ctx) {
                    t.push_back(s.id);
                    auto st = spell_tags.find(s.id);
                    if (st != spell_tags.end()) t.insert(t.end(), st->second.begin(), st->second.end());
                }
        if (ctx.empty() && type == (kDmgMagic | kDmgPierce | kDmgFire)) t.push_back("grant_peace");
        if ((ai && is_lava(ai->shape)) || (!aa && type == kDmgPierce)) t.push_back("lava");
    }
    return t;
}

void Creatures::say(CreatureState& st, const std::string& line) {
    const uint32_t now = w_.game_frames();
    // The same line at most once in 2 s per creature (a flurry of blows says it once).
    if (line.empty() || (st.said && now < st.said + kSayFrames && st.said_line == line)) return;
    st.said = now;
    st.said_line = line;
    w_.game_log.add(GameLog::kCombat, GameLog::kMiss, line, st.actor);
    log.push_back("say " + line);
}

int Creatures::on_hit(uint16_t victim, uint16_t attacker, int damage, uint16_t type, bool weapon) {
    if (live.empty()) return damage;
    // Its own blows: mana drain.
    if (const CreatureDef* ad = def_of(attacker); ad && ad->mana_drain > 0 && damage > 0)
        if (Actor* v = w_.actor(victim); v && v->mana > 0) {
            const int take = std::min<int>(v->mana, ad->mana_drain);
            v->mana = int16_t(v->mana - take);
            w_.game_log.add(GameLog::kCombat, GameLog::kSpell,
                            "The " + ad->name + " drains " + std::to_string(take) + " mana from " + GameLog::name_of(w_, victim, false) + ".", victim);
            log.push_back("drain " + std::to_string(take));
        }
    auto it = live.find(victim);
    const CreatureDef* d = def_of(victim);
    if (it == live.end() || !d) return damage;
    CreatureState& st = it->second;
    if (st.calm && attacker) { st.calm = false; log.push_back("calm ends"); }  // hurt: it fights again
    std::vector<std::string> tags = hit_tags(victim, attacker, type, weapon);
    auto has = [&](const std::string& s) { return std::find(tags.begin(), tags.end(), s) != tags.end(); };
    // Weakened by a named spell (it may still take nothing from the spell itself).
    if (!d->weakened_by.empty() && has(d->weakened_by)) {
        st.weak_frames = d->weak_frames;
        if (w_.turn && w_.turn->active()) st.weak_rounds = d->weak_rounds;
        w_.game_log.add(GameLog::kCombat, GameLog::kSpell, d->weak_say.empty() ? "The " + d->name + " is weakened." : d->weak_say, victim);
        log.push_back("weakened");
        tags.push_back("weakened");
    }
    for (const CreatureRule& r : d->rules) {
        if (!std::all_of(r.when.begin(), r.when.end(), has)) continue;
        if (r.pct != 100) {
            if (damage > 0 || r.pct == 0) say(st, r.say);
            damage = damage * r.pct / 100;
        }
        log.push_back("rule " + (r.when.empty() ? std::string("-") : r.when[0]) + " " + std::to_string(r.pct));
        break;
    }
    return damage;
}

void Creatures::on_touch(uint16_t floor, uint16_t toucher) {
    const Item* f = w_.item(floor);
    const Item* t = w_.item(toucher);
    if (!enabled || !f || !t || !is_water(f->shape) || !is_food(*t)) return;
    int x, y, z;
    w_.location(*t, x, y, z);
    for (auto& e : live) {
        const CreatureDef* d = def_of(e.first);
        const Actor* a = w_.actor(e.first);
        if (!d || !a || d->bait_frames <= 0 || a->is(Actor::kDead) || a->mapnum != w_.map_number_now()) continue;
        if (d2(a->x, a->y, x, y) > 900LL * 900) continue;
        e.second.bait_frames = d->bait_frames;
        e.second.bait_x = x;
        e.second.bait_y = y;
        e.second.withdrawn = false;
        ++baits;
        log.push_back("bait " + std::to_string(e.first));
        w_.game_log.add(GameLog::kMessages, GameLog::kInfo, "Something stirs in the water.", e.first);
        // Virtues (PLAN-VIRTUES.md 3.19): food for a water creature that is still alive calms it.
        // It leaves the party alone from then on (until somebody hurts it) and pays Compassion
        // once per creature: the Avatar fed it instead of killing it.
        if (w_.virtues && w_.virtues->enabled && d->kind == "water" && !e.second.calm) {
            e.second.calm = true;
            log.push_back("calm " + std::to_string(e.first));
            w_.game_log.add(GameLog::kMessages, GameLog::kInfo, "The " + d->name + " grows calm.", e.first);
            w_.virtues->earn(kCompassion, 3, "calm/" + d->id + "/" + std::to_string(e.second.place));
        }
    }
}

bool Creatures::untargetable(uint16_t id) const {
    auto it = live.find(id);
    if (it == live.end()) return false;
    const CreatureDef* d = def_of(id);
    return d && (d->kind == "water" || d->kind == "lava") && it->second.phase != CreatureState::kUp && !it->second.trapped;
}

bool Creatures::withdrawn(uint16_t id) const {
    auto it = live.find(id);
    return it != live.end() && (it->second.withdrawn || it->second.calm);  // calm (Virtues): out of the fight
}

bool Creatures::trap_in_rock(uint16_t id) {
    auto it = live.find(id);
    const CreatureDef* d = def_of(id);
    if (it == live.end() || !d || d->kind != "lava") return false;
    it->second.trapped = true;
    it->second.phase = CreatureState::kUp;
    log.push_back("trapped " + std::to_string(id));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The brains

void Creatures::set_phase(CreatureState& st, const CreatureDef& d, CreatureState::Phase p) {
    st.phase = p;
    const CreatureDraw& dr = drawing(st, d);
    const int anim = std::max(1, dr.last - dr.first + 1) * std::max(1, dr.ticks);
    switch (p) {
    case CreatureState::kHidden: st.timer = d.down_frames; break;
    case CreatureState::kRising: st.timer = anim; break;
    case CreatureState::kUp: st.timer = d.up_frames; break;
    case CreatureState::kSinking: st.timer = anim; break;
    }
}

void Creatures::effects(CreatureState& st, const Actor& a, const CreatureDef& d) {
    if (!d.particle.set || d.particle_every <= 0 || w_.rand_below(d.particle_every)) return;
    if ((d.kind == "water" || d.kind == "lava") && st.phase != CreatureState::kUp && d.kind == "water") return;
    w_.kernel().add(std::make_unique<SpriteProcess>(w_, d.particle.shape, d.particle.first, d.particle.last, 1,
                                                    std::max(1, d.particle.ticks / 2), a.x - 16 + w_.rand_below(32),
                                                    a.y - 16 + w_.rand_below(32), a.z + 8 + w_.rand_below(24)));
}

void Creatures::brain(CreatureState& st, Actor& a, const CreatureDef& d) {
    if (st.cooldown > 0) --st.cooldown;
    if (st.timer > 0) --st.timer;
    st.weak_rounds = 0;
    effects(st, a, d);
    const bool habitat = d.kind == "water" || d.kind == "lava";
    const uint16_t tid = st.calm ? 0 : pick_target(a, d, d.aggro);
    const Actor* t = w_.actor(tid);
    const int dist = t ? dist_to(a, *t) : INT_MAX;
    if (t && dist <= d.reach) { st.withdrawn = false; st.idle_turns = 0; }
    if (!habitat) {
        st.phase = CreatureState::kUp;
        if (!t) return;
        if (!a.is(Actor::kInCombat)) a.set(Actor::kInCombat, true);
        if (dist > d.reach * 3 / 4) step_toward(a, d, t->x, t->y, d.speed, d.reach / 2);
        else a.dir = uint8_t(direction_to(t->x - a.x, t->y - a.y));
        if (dist <= d.reach && st.cooldown <= 0) {
            strike(a.id, tid);
            st.cooldown = d.every;
        }
        return;
    }
    // Water and lava: rise, strike, sink; bait draws it up and keeps it up, calm, while it eats.
    if (st.bait_frames > 0) {
        --st.bait_frames;
        switch (st.phase) {
        case CreatureState::kHidden:
            if (!step_toward(a, d, st.bait_x, st.bait_y, d.speed * 2, 40) || d2(a.x, a.y, st.bait_x, st.bait_y) < 64LL * 64)
                set_phase(st, d, CreatureState::kRising);
            break;
        case CreatureState::kRising:
            if (st.timer <= 0) set_phase(st, d, CreatureState::kUp);
            break;
        case CreatureState::kSinking:
            set_phase(st, d, CreatureState::kRising);
            break;
        default:
            st.timer = std::max(st.timer, d.up_frames);  // it stays up while it eats, then a while more
            break;
        }
        return;
    }
    switch (st.phase) {
    case CreatureState::kHidden:
        if (st.trapped) { set_phase(st, d, CreatureState::kUp); break; }
        if (!t) break;
        if (dist > d.reach / 2) step_toward(a, d, t->x, t->y, d.speed, d.reach / 3);
        if (st.timer <= 0 && dist_to(a, *t) <= d.reach) set_phase(st, d, CreatureState::kRising);
        break;
    case CreatureState::kRising:
        if (st.timer <= 0) { set_phase(st, d, CreatureState::kUp); st.cooldown = std::min(st.cooldown, 10); }
        break;
    case CreatureState::kUp:
        if (t) a.dir = uint8_t(direction_to(t->x - a.x, t->y - a.y));
        if (t && dist <= d.reach && st.cooldown <= 0) {
            strike(a.id, tid);
            st.cooldown = d.every;
        }
        if (st.timer <= 0 && !st.trapped) set_phase(st, d, CreatureState::kSinking);
        break;
    case CreatureState::kSinking:
        if (st.timer <= 0) set_phase(st, d, CreatureState::kHidden);
        break;
    }
}

bool Creatures::turn_act(uint16_t id) {
    auto it = live.find(id);
    const CreatureDef* d = def_of(id);
    Actor* a = w_.actor(id);
    if (it == live.end() || !d || !a) return false;
    CreatureState& st = it->second;
    if (a->is(Actor::kDead)) return true;
    if (st.weak_rounds > 0 && --st.weak_rounds == 0) { st.weak_frames = 0; log.push_back("weak ends"); }
    if (st.calm && st.bait_frames <= 0) {  // calm (Virtues): it keeps to itself
        st.phase = CreatureState::kHidden;
        return true;
    }
    st.weak_frames = 0;
    const bool habitat = d->kind == "water" || d->kind == "lava";
    const uint16_t tid = pick_target(*a, *d, 1400);
    Actor* t = w_.actor(tid);
    auto idle = [&]() {
        if (++st.idle_turns >= 3) {
            st.withdrawn = true;
            if (habitat) st.phase = CreatureState::kHidden;
            w_.game_log.add(GameLog::kCombat, GameLog::kTurn, "The " + d->name + " loses interest.", id);
            log.push_back("withdraws");
        }
    };
    if (!t) { idle(); return true; }
    if (!habitat) {
        st.phase = CreatureState::kUp;
        if (dist_to(*a, *t) > d->reach * 3 / 4) step_toward(*a, *d, t->x, t->y, kTurnMove, d->reach / 2);
        if (dist_to(*a, *t) <= d->reach) { st.idle_turns = 0; strike(id, tid); }
        else idle();
        return true;
    }
    if (st.bait_frames > 0) {
        // Baited: up at the food and eating, for a few of its turns.
        step_toward(*a, *d, st.bait_x, st.bait_y, kTurnMove, 40);
        st.phase = CreatureState::kUp;
        st.bait_frames = std::max(0, st.bait_frames - 120);
        st.idle_turns = 0;
        w_.game_log.add(GameLog::kCombat, GameLog::kTurn, "The " + d->name + " feeds at the surface.", id);
        log.push_back("feeds");
        return true;
    }
    if (st.phase == CreatureState::kUp && !st.trapped) {
        // It dives after a turn up, and moves under.
        st.phase = CreatureState::kHidden;
        step_toward(*a, *d, t->x, t->y, kTurnMove, d->reach / 3);
        w_.game_log.add(GameLog::kCombat, GameLog::kTurn, "The " + d->name + " sinks out of sight.", id);
        log.push_back("dives");
        return true;
    }
    step_toward(*a, *d, t->x, t->y, kTurnMove, d->reach / 3);
    if (dist_to(*a, *t) <= d->reach) {
        st.phase = CreatureState::kUp;
        st.idle_turns = 0;
        strike(id, tid);
        // The Lurker's pet bites and is gone again, mostly: only bait keeps it up for long.
        if (d->kind == "water" && !st.trapped && w_.rand_below(3) != 0) {
            st.phase = CreatureState::kHidden;
            w_.game_log.add(GameLog::kCombat, GameLog::kTurn, "The " + d->name + " sinks out of sight.", id);
            log.push_back("dives");
        }
    } else {
        idle();
    }
    return true;
}

void Creatures::frame() {
    if (!loaded()) return;
    if (w_.leveling && !w_.leveling->monsters.count(defs.front().body)) register_rows();  // Leveling reloaded its data
    const Actor* av = w_.avatar();
    if (!av) return;
    if (!enabled) {
        // Off: none are out. Those that were go, and their places may fill again once it is on.
        std::vector<uint16_t> gone;
        for (auto& e : live) gone.push_back(e.first);
        for (uint16_t id : gone) {
            const CreatureState& st = live[id];
            const Actor* a = w_.actor(id);
            if (st.def >= 0 && st.place >= 0 && !(a && a->is(Actor::kDead)))
                spawned.erase(defs[size_t(st.def)].id + ":" + std::to_string(st.place));
            if (a) w_.destroy(id);
            live.erase(id);
        }
        return;
    }
    if (last_map_ != av->mapnum || w_.game_frames() % 30 == 0) {
        last_map_ = av->mapnum;
        spawn_here();
    }
    const bool fight = w_.turn && w_.turn->active();
    std::vector<uint16_t> ids;
    for (auto& e : live) ids.push_back(e.first);
    for (uint16_t id : ids) {
        auto it = live.find(id);
        if (it == live.end()) continue;
        Actor* a = w_.actor(id);
        const CreatureDef* d = def_of(id);
        if (!a || !d || a->shape != d->body) { live.erase(id); continue; }
        if (a->is(Actor::kDead)) {
            if (++it->second.dead_frames >= kDeadFrames && a->chunk >= 0) {
                w_.destroy(id);
                live.erase(id);
            }
            continue;
        }
        // A weakening wears off in real time wherever it is (turn-based fights count it in rounds).
        if (!fight && it->second.weak_frames > 0 && --it->second.weak_frames == 0) log.push_back("weak ends");
        if (a->mapnum != av->mapnum || a->chunk < 0 || fight || w_.stasis) continue;
        brain(it->second, *a, *d);
    }
}

// ---------------------------------------------------------------------------------------------
// Saving: optional section CRTR after the banter (written only when something has spawned)

void Creatures::save(SaveWriter& o) const {
    o.tag("CRTR");
    o.u8(2);
    o.u32(uint32_t(spawned.size()));
    for (const std::string& s : spawned) o.str(s);
    o.u32(uint32_t(live.size()));
    for (const auto& e : live) {
        const CreatureState& s = e.second;
        o.u16(s.actor);
        o.str(s.def >= 0 && size_t(s.def) < defs.size() ? defs[size_t(s.def)].id : std::string());
        o.i32(s.place); o.u8(s.phase); o.i32(s.timer); o.i32(s.cooldown); o.i32(s.weak_frames); o.i32(s.weak_rounds);
        o.i32(s.bait_frames); o.i32(s.bait_x); o.i32(s.bait_y); o.i32(s.idle_turns); o.b(s.withdrawn); o.b(s.trapped);
        o.i32(s.home_x); o.i32(s.home_y);
        o.i32(s.dead_frames);
        o.b(s.calm);  // version 2
    }
}

bool Creatures::load(SaveReader& in) {
    reset();
    if (!in.tag("CRTR")) return false;
    const unsigned ver = in.u8();
    if (ver != 1 && ver != 2) return false;
    uint32_t n = std::min<uint32_t>(in.u32(), 100000);
    for (uint32_t i = 0; i < n; ++i) spawned.insert(in.str());
    n = std::min<uint32_t>(in.u32(), 1000);
    for (uint32_t i = 0; i < n; ++i) {
        CreatureState s;
        s.actor = uint16_t(in.u16());
        s.def = def_index(in.str());
        s.place = in.i32(); s.phase = CreatureState::Phase(in.u8() & 3); s.timer = in.i32(); s.cooldown = in.i32();
        s.weak_frames = in.i32(); s.weak_rounds = in.i32(); s.bait_frames = in.i32(); s.bait_x = in.i32(); s.bait_y = in.i32();
        s.idle_turns = in.i32(); s.withdrawn = in.b(); s.trapped = in.b(); s.home_x = in.i32(); s.home_y = in.i32(); s.dead_frames = in.i32();
        if (ver >= 2) s.calm = in.b();
        if (s.def >= 0) live[s.actor] = s;
    }
    return !in.fail();
}

}  // namespace pg
