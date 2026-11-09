// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// Item powers: see item_powers.h.
#include "data/compat.h"
#include "game/item_powers.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

#include "core/config.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "json/json.hpp"
#include "usecode/uc_machine.h"

namespace pg {
namespace item_powers {

namespace {

constexpr uint32_t kFramesPerMinute = 30 * 60;
// Armour and to-hit do not apply (pierce), magic immunity neither: the ground itself moves.
constexpr uint16_t kQuakeDamage = kDmgBlunt | kDmgMagic | kDmgPierce;
// A puff of dust at the foe's feet (the smoke puff of the pulled companions).
constexpr int kDustShape = 578, kDustFirst = 66, kDustLast = 80;

Ability skull() {
    Ability a;
    a.id = "skull_of_quakes";
    a.name = "Skull of Quakes";
    a.log_name = "earthquake";
    a.log_text = "The earth heaves under the Skull of Quakes!";
    a.quiet_text = "The skull is still quiet.";
    a.shape = 814;
    a.cooldown_frames = 30 * kFramesPerMinute;
    a.sfx = {48, 49};
    a.original_quality = 0;
    a.original_target_shape = 805;
    return a;
}

const Ability* by_id(const std::string& id) {
    for (const Ability& a : abilities())
        if (a.id == id) return &a;
    return nullptr;
}

bool hostile(const World& w, uint16_t id) {
    const Actor* o = w.actor(id);
    const Actor* av = w.avatar();
    if (!o || !av) return false;
    // Townsfolk and guards (own side 1, actors.md) are the Avatar's enemies by table only: they
    // count once they actually fight the party, not while idle or fighting something else.
    if (o->alignment & 1) {
        bool at_party = o->is(Actor::kInCombat) && o->target && (o->target == 1 || w.same_party(1, o->target));
        if (!at_party) return false;
    }
    if (w.turn) return w.turn->is_hostile(id);
    return id != 1 && !o->is(Actor::kDead) && !w.same_party(1, id) &&
           o->mapnum == av->mapnum && is_enemy(*o, *av) && monster_info(o->shape);
}

void quake_effect(World& w, const Ability& ab) {
    Actor* av = w.avatar();
    if (!av) return;
    // The shake: not over a script's own quake (a scene's), which it must end by itself.
    if (w.quake == 0 || w.power_quake > 0 || w.engine_quake > 0) {
        w.quake = ab.quake_strength;
        w.power_quake = ab.quake_frames;
        w.power_quake_strength = ab.quake_strength;
    }
    for (int s : ab.sfx) w.sound.play_sfx(s, ab.sfx_priority);
    if (!ab.log_text.empty()) w.game_log.add(GameLog::kCombat, GameLog::kSpell, ab.log_text, 1);
    std::vector<uint16_t> victims;
    for (unsigned n = 2; n < 256; ++n) {
        const Actor* o = w.actor(uint16_t(n));
        if (!o || !hostile(w, uint16_t(n))) continue;
        long dx = o->x - av->x, dy = o->y - av->y;
        if (dx * dx + dy * dy > long(ab.radius) * ab.radius || std::abs(o->z - av->z) > ab.height) continue;
        victims.push_back(uint16_t(n));
    }
    // The hits go through the combat pipeline in the Avatar's name (the log, fighting back, a
    // turn-based fight); last_anim 58 keeps them from training his strength like his own blows.
    const unsigned anim = av->last_anim;
    av->last_anim = 58;
    w.game_log.spell_context = ab.log_name;
    for (uint16_t k : victims) {
        const Actor* o = w.actor(k);
        if (!o) continue;
        int dmg = ab.damage_min + w.rand_below(std::max(1, ab.damage_max - ab.damage_min + 1));
        int dir = (direction_to(o->x - av->x, o->y - av->y) + 4) & 7;
        w.kernel().add(std::make_unique<SpriteProcess>(w, kDustShape, kDustFirst, kDustLast, 1, 1, o->x, o->y, o->z));
        w.receive_hit(k, 1, dir, dmg, kQuakeDamage);
        ++stats().hits;
    }
    w.game_log.spell_context.clear();
    if ((av = w.avatar())) av->last_anim = decltype(av->last_anim)(anim);
    PG_LOG("item power %s: %zu foes hit within %d", ab.id.c_str(), victims.size(), ab.radius);
}

}  // namespace

Stats& stats() {
    static Stats s;
    return s;
}

std::vector<Ability>& abilities() {
    static std::vector<Ability> table{skull()};
    return table;
}

bool load_text(const std::string& text, std::string& err) {
    using json = nlohmann::json;
    std::vector<Ability> table;
    try {
        json j = json::parse(text, nullptr, true, true);
        for (const json& e : j.at("abilities")) {
            Ability a;
            a.id = e.at("id").get<std::string>();
            a.effect = e.value("effect", a.effect);
            if (a.effect != "quake") { err = a.id + ": unknown effect " + a.effect; return false; }
            a.shape = e.at("shape").get<unsigned>();
            a.frame = e.value("frame", a.frame);
            a.name = e.value("name", a.id);
            a.log_name = e.value("log_name", a.name);
            a.log_text = e.value("log_text", std::string());
            a.quiet_text = e.value("quiet_text", std::string("It is still quiet."));
            a.cooldown_frames = uint32_t(e.value("cooldown_minutes", 30.0) * kFramesPerMinute);
            a.radius = e.value("radius", a.radius);
            a.height = e.value("height", a.height);
            if (e.contains("damage")) {
                a.damage_min = e.at("damage").at(0).get<int>();
                a.damage_max = e.at("damage").at(1).get<int>();
            }
            a.quake_strength = e.value("quake_strength", a.quake_strength);
            a.quake_frames = e.value("quake_frames", a.quake_frames);
            a.sfx = e.value("sfx", std::vector<int>{});
            a.sfx_priority = e.value("sfx_priority", a.sfx_priority);
            if (e.contains("original_use")) {
                a.original_quality = e.at("original_use").at("quality").get<int>();
                a.original_target_shape = e.at("original_use").at("target_shape").get<int>();
            }
            table.push_back(std::move(a));
        }
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    abilities() = std::move(table);
    return true;
}

bool load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_text(ss.str(), err);
}

bool& enabled() {
    static bool on = true;
    return on;
}

void register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World&) {
        MenuGump::OptionRow r{"Item powers:", [] { return std::string(enabled() ? "On" : "Off"); },
                              [](int) {
                                  enabled() = !enabled();
                                  Config& c = Config::global();
                                  c.set_bool("item_powers", enabled());
                                  if (!c.path().empty()) c.save();
                              }};
        r.group = "Game";
        return r;
    });
}

void apply_options() { enabled() = Config::global().get_bool("item_powers", true); }

// The power sits on the item's own usecode class: it must be the one it was made for.
static bool data_fits(const Ability& a) {
    if (compat::class_known(a.shape)) return true;
    compat::off("the item power " + a.id, "usecode class " + std::to_string(a.shape) + " differs in this copy");
    return false;
}

const Ability* find(const Item& it) {
    if (!enabled()) return nullptr;
    for (const Ability& a : abilities())
        if (a.shape == it.shape && (a.frame < 0 || a.frame == int(it.frame))) return data_fits(a) ? &a : nullptr;
    return nullptr;
}

bool is_power_shape(unsigned shape) {
    if (!enabled()) return false;
    for (const Ability& a : abilities())
        if (a.shape == shape) return data_fits(a);
    return false;
}

uint32_t cooldown_left(const World& w, const Ability& ab) {
    auto it = w.power_cooldown.find(ab.id);
    return it == w.power_cooldown.end() ? 0 : it->second;
}

std::string time_left_text(uint32_t frames) {
    uint32_t minutes = (frames + kFramesPerMinute - 1) / kFramesPerMinute;
    if (minutes <= 1) return "less than a minute";
    return std::to_string(minutes) + " minutes";
}

std::string display_name(const World& w, const Item& it) {
    const Ability* ab = find(it);
    if (!ab) return {};
    uint32_t left = cooldown_left(w, *ab);
    if (!left) return ab->name;
    return ab->name + " (" + std::to_string((left + kFramesPerMinute - 1) / kFramesPerMinute) + " min)";
}

bool refuse_turn_use(const World& w, uint16_t item, std::string& status) {
    const Item* it = w.item(item);
    const Ability* ab = it ? find(*it) : nullptr;
    uint32_t left = ab ? cooldown_left(w, *ab) : 0;
    if (!left) return false;
    status = ab->quiet_text + " (" + time_left_text(left) + ")";
    return true;
}

bool fire(World& w, const Ability& ab) {
    if (uint32_t left = cooldown_left(w, ab)) {
        ++stats().refused;
        w.bark(1, ab.quiet_text + " (" + time_left_text(left) + ")");
        return false;
    }
    if (ab.cooldown_frames) w.power_cooldown[ab.id] = ab.cooldown_frames;
    ++stats().fired;
    quake_effect(w, ab);
    return true;
}

bool on_use(World& w, uint16_t item) {
    const Item* it = w.item(item);
    const Ability* ab = it ? find(*it) : nullptr;
    if (!ab || w.stasis) return false;
    if (ab->keeps_original_use() && int(it->quality) == ab->original_quality) return false;  // the usecode asks for a target
    fire(w, *ab);
    return true;
}

void on_target_pick(World& w, const UCProcess& proc, uint16_t target_pid) {
    const Item* it = w.item(proc.item);
    const Ability* ab = it ? find(*it) : nullptr;
    if (!ab || !ab->keeps_original_use() || int(proc.cls) != int(it->shape)) return;
    auto* n = dynamic_cast<NotifyProcess*>(w.kernel().get(target_pid));
    if (!n) return;
    std::string id = ab->id;
    int keep = ab->original_target_shape;
    World* wp = &w;
    n->on_end = [wp, id, keep](uint32_t picked) {
        if (!picked) return;  // cancelled
        const Item* t = wp->item(uint16_t(picked));
        if (t && int(t->shape) == keep) return;  // the original use: the usecode does it
        wp->power_pending = id;                  // World::frame fires it, once the cursor is gone
    };
}

void decorate_look(const World& w, const UCProcess& proc, std::string& text) {
    const Item* it = w.item(proc.item);
    const Ability* ab = it ? find(*it) : nullptr;
    if (!ab || int(proc.cls) != int(it->shape)) return;
    if (proc.start_addr != w.data().usecode.event_entry(proc.cls, 0)) return;  // look event only
    uint32_t left = cooldown_left(w, *ab);
    if (!left) return;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    text += " (quiet for " + time_left_text(left) + ")";
}

void frame(World& w) {
    if (!w.power_pending.empty()) {
        const Ability* ab = by_id(w.power_pending);
        w.power_pending.clear();
        if (ab && enabled() && !w.stasis) fire(w, *ab);
    }
    if (w.kernel().paused()) return;
    for (auto it = w.power_cooldown.begin(); it != w.power_cooldown.end();) {
        if (--it->second == 0) {
            PG_LOG("item power %s: ready again", it->first.c_str());
            it = w.power_cooldown.erase(it);
        } else {
            ++it;
        }
    }
    if (w.power_quake > 0 && --w.power_quake == 0) {
        if (w.quake == w.power_quake_strength) w.quake = 0;  // (a script's own later quake is left alone)
        w.power_quake_strength = 0;
    }
}

void new_game(World& w) {
    w.power_cooldown.clear();
    w.power_pending.clear();
    w.power_quake = w.power_quake_strength = 0;
}

void save(const World& w, SaveWriter& o) {
    if (w.power_cooldown.empty() && !w.power_quake) return;
    o.tag("ITPW");
    o.u32(uint32_t(w.power_cooldown.size()));
    for (const auto& c : w.power_cooldown) { o.str(c.first); o.u32(c.second); }
    o.i32(w.power_quake);
    o.i32(w.power_quake_strength);
}

void load(World& w, SaveReader& i) {
    new_game(w);
    if (!i.peek_tag("ITPW")) return;
    i.tag("ITPW");
    uint32_t n = std::min<uint32_t>(i.u32(), 1000);
    for (uint32_t k = 0; k < n; ++k) {
        std::string id = i.str();
        uint32_t left = i.u32();
        if (left) w.power_cooldown[id] = left;
    }
    w.power_quake = i.i32();
    w.power_quake_strength = i.i32();
}

}  // namespace item_powers
}  // namespace pg
