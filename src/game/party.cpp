// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Companions: the data file, the party state, spawning, saving, recolour tables and the hooks
// the shared engine code calls (see party.h).
#include "data/compat.h"
#include "game/banter.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/turn_combat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/config.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/critpath.h"
#include "game/processes.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using json = nlohmann::json;

namespace {

std::vector<std::string> str_list(const json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& e : j)
            if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

int tri(const json& j, const char* k) {
    if (!j.contains(k)) return -1;
    return j[k].get<bool>() ? 1 : 0;
}

PartyCond parse_cond(const json& j) {
    PartyCond c;
    if (!j.is_object()) return c;
    c.in_party = tri(j, "in_party");
    c.party_full = tri(j, "party_full");
    c.waiting = tri(j, "waiting");
    c.combat = tri(j, "combat");
    c.joined_before = tri(j, "joined_before");
    c.dead_ally = tri(j, "dead_ally");
    c.map = j.value("map", -1);
    if (j.contains("present")) c.present = str_list(j["present"]);
    if (j.contains("absent")) c.absent = str_list(j["absent"]);
    if (j.contains("flag")) c.flags = str_list(j["flag"]);
    if (j.contains("not_flag")) c.not_flags = str_list(j["not_flag"]);
    if (j.contains("step_done")) c.step_done = str_list(j["step_done"]);
    if (j.contains("step_not_done")) c.step_not_done = str_list(j["step_not_done"]);
    if (j.contains("asked")) c.asked = str_list(j["asked"]);
    if (j.contains("not_asked")) c.not_asked = str_list(j["not_asked"]);
    c.leveling = tri(j, "leveling");
    c.reagents = j.value("reagents", std::string());
    c.not_reagents = j.value("not_reagents", std::string());
    c.empty = j.empty();
    return c;
}

PartyTopic parse_topic(const json& j) {
    PartyTopic t;
    t.id = j.value("id", "");
    t.label = j.value("label", t.id);
    if (t.id.empty()) t.id = t.label;
    if (j.contains("when")) t.when = parse_cond(j["when"]);
    if (j.contains("say")) t.say = str_list(j["say"]);
    if (j.contains("alt"))
        for (const json& a : j["alt"]) t.alt.push_back({parse_cond(a.value("when", json::object())), str_list(a.value("say", json::array()))});
    t.action = j.value("action", "");
    if (j.contains("set")) t.set_flags = str_list(j["set"]);
    if (j.contains("clear")) t.clear_flags = str_list(j["clear"]);
    if (j.contains("topics"))
        for (const json& s : j["topics"]) t.topics.push_back(parse_topic(s));
    return t;
}

RecolourRule parse_rule(const json& j) {
    RecolourRule r;
    if (j.contains("hues") && j["hues"].is_array() && j["hues"].size() == 2) {
        r.hue0 = j["hues"][0].get<float>();
        r.hue1 = j["hues"][1].get<float>();
    }
    r.min_sat = j.value("min_sat", r.min_sat);
    r.max_sat = j.value("max_sat", r.max_sat);
    r.min_val = j.value("min_val", r.min_val);
    r.max_val = j.value("max_val", r.max_val);
    r.add_sat = j.value("add_sat", 0.0f);
    if (j.contains("to_hue")) r.hue = j["to_hue"].get<float>();
    if (j.contains("shift")) { r.hue = j["shift"].get<float>(); r.shift = true; }
    r.sat = j.value("sat", 1.0f);
    r.val = j.value("val", 1.0f);
    return r;
}

std::vector<ItemSpec> parse_items(const json& j) {
    std::vector<ItemSpec> out;
    if (!j.is_array()) return out;
    for (const json& e : j) {
        ItemSpec s;
        s.shape = e.value("shape", 0u);
        s.frame = e.value("frame", 0u);
        s.qty = e.value("qty", 0u);
        if (s.shape) out.push_back(s);
    }
    return out;
}

void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}

void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(h + 360.0f, 360.0f);
    float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = rr + m; g = gg + m; b = bb + m;
}

bool hue_in(float h, float a, float b) { return a <= b ? (h >= a && h <= b) : (h >= a || h <= b); }

// An index table built from hue rules: each palette colour whose hue falls in a rule's range is
// turned and then matched to the nearest palette colour (translucency indices are kept).
std::array<uint8_t, 256> build_table(const GameData& d, const std::vector<RecolourRule>& rules) {
    std::array<uint8_t, 256> t;
    for (int i = 0; i < 256; ++i) t[size_t(i)] = uint8_t(i);
    if (rules.empty()) return t;
    auto usable = [&](int i) { return i >= 16 && !(d.xform.loaded && d.xform.is_xform(uint8_t(i))); };
    for (int i = 0; i < 256; ++i) {
        if (!usable(i)) continue;
        float r = d.palette.rgb8[size_t(i)][0] / 255.0f, g = d.palette.rgb8[size_t(i)][1] / 255.0f,
              b = d.palette.rgb8[size_t(i)][2] / 255.0f, h, s, v;
        rgb_to_hsv(r, g, b, h, s, v);
        const RecolourRule* rule = nullptr;
        for (const RecolourRule& ru : rules)
            if (s >= ru.min_sat && s <= ru.max_sat && v >= ru.min_val && v <= ru.max_val && hue_in(h, ru.hue0, ru.hue1)) {
                rule = &ru;
                break;
            }
        if (!rule) continue;
        float nh = rule->hue < 0 ? h : rule->shift ? h + rule->hue : rule->hue;
        float ns = std::clamp(s * rule->sat + rule->add_sat, 0.0f, 1.0f), nv = std::clamp(v * rule->val, 0.0f, 1.0f);
        hsv_to_rgb(nh, ns, nv, r, g, b);
        int best = i, bestd = 1 << 30;
        for (int k = 0; k < 256; ++k) {
            if (!usable(k)) continue;
            int dr = int(r * 255) - d.palette.rgb8[size_t(k)][0], dg = int(g * 255) - d.palette.rgb8[size_t(k)][1],
                db = int(b * 255) - d.palette.rgb8[size_t(k)][2];
            int dist = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (dist < bestd) { bestd = dist; best = k; }
        }
        t[size_t(i)] = uint8_t(best);
    }
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Data

Party::Party(World& w) : banter(std::make_unique<Banter>(w, *this)), w_(w) {}
Party::~Party() = default;

bool Party::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    size_t slash = path.find_last_of('/');
    data_dir = slash == std::string::npos ? "." : path.substr(0, slash);
    return load_text(ss.str(), err);
}

bool Party::load_text(const std::string& text, std::string& err) {
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    defs.clear();
    spells.clear();
    try {
        for (const json& s : j.value("spells", json::array())) {
            SpellDef d;
            d.id = s.value("id", "");
            d.name = s.value("name", d.id);
            d.kind = s.value("kind", "bolt");
            d.mana = s.value("mana", 0);
            if (s.contains("damage") && s["damage"].is_array() && s["damage"].size() == 2) {
                d.dmg0 = s["damage"][0].get<int>();
                d.dmg1 = s["damage"][1].get<int>();
            }
            d.dmg_type = uint16_t(s.value("type", 0x20));
            d.base_range = s.value("range", d.range);
            d.base_radius = s.value("radius", 0);
            // Raising the fallen stays as it was: it is cast out of combat at a body beside you.
            const int scale = d.kind == "raise" ? 1 : kSpellRangeScale;
            d.range = d.base_range * scale;
            d.radius = d.base_radius * scale;
            if (s.contains("missile")) {
                d.proj_shape = s["missile"].value("shape", 0u);
                d.proj_frame = s["missile"].value("frame", 0u);
            }
            if (s.contains("effect")) {
                d.fx_shape = s["effect"].value("shape", 0u);
                d.fx_first = s["effect"].value("first", 0u);
                d.fx_last = s["effect"].value("last", d.fx_first);
            }
            d.sfx = s.value("sound", -1);
            d.cast_sfx = s.value("cast_sound", -1);
            d.combat = s.value("combat", true);
            d.outside = s.value("outside", false);
            d.flash = s.value("flash", false);
            d.only = s.value("only", "");
            if (!d.id.empty()) spells.push_back(d);
        }
        for (const json& c : j.at("companions")) {
            CompanionDef d;
            d.id = c.at("id").get<std::string>();
            d.name = c.value("name", d.id);
            d.title = c.value("title", "");
            d.sex = c.value("sex", "M");
            d.element = c.value("element", "");
            d.school = c.value("school", "");
            d.role = c.value("role", "");
            if (c.contains("stats")) {
                const json& s = c["stats"];
                d.str = s.value("str", d.str);
                d.dex = s.value("dex", d.dex);
                d.intel = s.value("int", d.intel);
                d.hp = s.value("hp", d.hp);
                d.mana = s.value("mana", d.mana);
            }
            d.lv_str = d.str; d.lv_dex = d.dex; d.lv_int = d.intel; d.lv_hp = d.hp; d.lv_mana = d.mana;
            if (c.contains("leveling")) {
                const json& s = c["leveling"];
                d.lv_class = s.value("class", std::string());
                d.lv_str = s.value("str", d.lv_str);
                d.lv_dex = s.value("dex", d.lv_dex);
                d.lv_int = s.value("int", d.lv_int);
                d.lv_hp = s.value("hp", 2 * d.lv_str);
                d.lv_mana = s.value("mana", d.mana > 0 ? 2 * d.lv_int : 0);
                if (j.contains("level_classes") && j["level_classes"].contains(d.lv_class)) {
                    const json& g = j["level_classes"][d.lv_class];
                    d.gain_str = g.value("str", d.gain_str);
                    d.gain_dex = g.value("dex", d.gain_dex);
                    d.gain_int = g.value("int", d.gain_int);
                }
            }
            if (c.contains("art")) {
                const json& a = c["art"];
                d.shape = a.value("shape", 1u);
                // An engine-made shape (data/shapes/) that is not loaded: the fallback art.
                const json* art = &a;
                const GameData& gd = w_.data();
                if (a.contains("fallback") && (!gd.shapes.shape(d.shape) || !gd.anim.has_shape(d.shape))) {
                    art = &a["fallback"];
                    d.shape = art->value("shape", 1u);
                    PG_LOG("party: %s: shape %u is not loaded, using shape %u", d.name.c_str(), a.value("shape", 1u), d.shape);
                }
                d.own_doll = art->value("paperdoll", "") == "own";
                d.portrait = a.value("portrait", "");
                if (art->contains("paperdoll_head")) {
                    const json& h = (*art)["paperdoll_head"];
                    d.doll_head = h.value("file", "");
                    d.doll_head_dx = h.value("dx", 0);
                    d.doll_head_dy = h.value("dy", 0);
                }
                d.sheet_portrait = a.value("sheet_portrait", "");
                for (const json& r : art->value("recolour", json::array())) d.recolour.push_back(parse_rule(r));
                for (const json& r : art->value("paperdoll_recolour", json::array())) d.doll_recolour.push_back(parse_rule(r));
            }
            d.equip = parse_items(c.value("equipment", json::array()));
            d.pack = parse_items(c.value("backpack", json::array()));
            d.spells = str_list(c.value("spells", json::array()));
            if (c.contains("spawn")) {
                const json& sp = c["spawn"];
                d.spawn[0] = sp.value("x", 0);
                d.spawn[1] = sp.value("y", 0);
                d.spawn[2] = sp.value("z", 0);
                d.spawn_dir = sp.value("dir", 4);
            }
            if (c.contains("home")) {
                const json& h = c["home"];
                d.has_home = true;
                d.home_map = h.value("map", 3u);
                d.home[0] = h.value("x", 0);
                d.home[1] = h.value("y", 0);
                d.home[2] = h.value("z", 0);
                d.home_dir = h.value("dir", 4);
                d.home_place = h.value("place", "");
            }
            d.caster = c.value("caster", !d.spells.empty());
            d.healer = c.value("healer", false);
            d.rogue = c.value("rogue", false);
            d.gear = c.value("gear", d.lv_class == "caster" ? "caster" : "warrior");
            for (const json& t : c.value("talk", json::array())) d.topics.push_back(parse_topic(t));
            if (c.contains("barks"))
                for (auto it = c["barks"].begin(); it != c["barks"].end(); ++it) d.barks[it.key()] = str_list(it.value());
            for (const json& a : c.value("area_barks", json::array())) {
                CompanionDef::AreaBark ab;
                for (const json& m : a.value("maps", json::array())) ab.maps.push_back(m.get<int>());
                ab.lines = str_list(a.value("say", json::array()));
                d.area_barks.push_back(ab);
            }
            defs.push_back(std::move(d));
        }
    } catch (const std::exception& e) {
        err = e.what();
        defs.clear();
        return false;
    }
    state.assign(defs.size(), CompanionState{});
    members.clear();
    remap_.clear();
    doll_remap_.clear();
    PG_LOG("party: %zu companions, %zu spells", defs.size(), spells.size());
    return true;
}

const SpellDef* Party::spell(const std::string& id) const {
    int i = spell_index(id);
    return i >= 0 ? &spells[size_t(i)] : nullptr;
}

int Party::spell_index(const std::string& id) const {
    for (size_t i = 0; i < spells.size(); ++i)
        if (spells[i].id == id) return int(i);
    return -1;
}

int Party::find_def(const std::string& id) const {
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].id == id) return int(i);
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Options

void Party::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    auto onoff = [](bool v) { return std::string(v ? "On" : "Off"); };
    World::extra_option_rows().push_back([onoff](World& w) {
        Party* p = w.party.get();
        MenuGump::OptionRow r{"Companions:", [p, onoff] { return onoff(p && p->enabled); },
                                   [p](int) {
                                       if (!p) return;
                                       p->set_enabled(!p->enabled);
                                       Config& c = Config::global();
                                       c.set_bool("companions", p->enabled);
                                       if (!c.path().empty()) c.save();
                                   }};
        r.group = "Party";
        return r;
    });
    World::extra_option_rows().push_back([onoff](World& w) {
        Party* p = w.party.get();
        MenuGump::OptionRow r{"Party Bar:", [p, onoff] { return onoff(p && p->show_bar); },
                                   [p](int) {
                                       if (!p) return;
                                       p->show_bar = !p->show_bar;
                                       Config& c = Config::global();
                                       c.set_bool("party_bar", p->show_bar);
                                       if (!c.path().empty()) c.save();
                                       p->update_bar();
                                   }};
        r.group = "Party";
        return r;
    });
}

void Party::set_enabled(bool on) {
    if (on == enabled) return;
    enabled = on;
    if (!on) despawn_all();
    else respawn_hidden();
    update_bar();
}

// ---------------------------------------------------------------------------------------------
// State

void Party::reset() {
    state.assign(defs.size(), CompanionState{});
    members.clear();
    bolts.clear();
    summons.clear();
    spawned_once = false;
    avatar_target = 0;
    innocent_ = 0;
    talk_ = Talk{};
    next_ambient_ = 0;
    start_frame_ = w_.kernel().frame_number();
    last_map_ = 0;
    log.clear();
    homes_dirty_ = false;
    banter->reset();
}

int Party::index_of(uint16_t actor) const {
    if (actor == 0 || actor >= 256) return -1;
    for (size_t i = 0; i < state.size(); ++i)
        if (state[i].id == actor && state[i].mode != CompanionState::kUnspawned) return int(i);
    return -1;
}

bool Party::in_party(int idx) const {
    return std::find(members.begin(), members.end(), idx) != members.end();
}

bool Party::party_member(uint16_t actor) const {
    if (actor == 1) return true;
    int i = index_of(actor);
    return i >= 0 && in_party(i);
}

bool Party::untouchable(uint16_t actor) const {
    if (!enabled) return false;
    int i = index_of(actor);
    return i >= 0 && !state[size_t(i)].joined_before && !in_party(i);
}

int Party::max_hp(const Actor& a) const {
    int i = index_of(a.id);
    int base = i >= 0 ? (w_.leveling && w_.leveling->rebased(a.id) ? defs[size_t(i)].lv_hp : defs[size_t(i)].hp) : 0;
    const int lv = w_.leveling ? w_.leveling->hp_bonus(a.id) : 0;  // Leveling: HP from levels (0 for now)
    return std::max({1, base, 2 * int(a.str)}) + lv;
}

int Party::max_mana(const Actor& a) const {
    int i = index_of(a.id);
    if (i < 0) return std::max(0, 2 * int(a.intel));
    const int base = w_.leveling && w_.leveling->rebased(a.id) ? defs[size_t(i)].lv_mana : defs[size_t(i)].mana;
    if (base <= 0) return 0;
    return std::max(base, 2 * int(a.intel));
}

std::string Party::display_name(uint16_t actor) const {
    if (actor == 1) return w_.avatar_name;
    int i = index_of(actor);
    return i >= 0 ? defs[size_t(i)].name : std::string();
}

bool Party::step_done(const std::string& id) const {
    Party* self = const_cast<Party*>(this);
    if (!critpath_tried_) {
        self->critpath_tried_ = true;
        self->crit_ = std::make_unique<CritPath>();
        std::string err;
        for (const char* n : {"critpath.json", "map_links.json"}) {
            std::string p = (data_dir.empty() ? std::string("data") : data_dir) + "/" + n;
            if (!self->crit_->load_file(p, err)) PG_LOG("party: %s: %s", p.c_str(), err.c_str());
        }
    }
    if (!crit_ || crit_->empty()) return false;
    int k = crit_->step_index(id);
    if (k < 0) return false;
    uint32_t f = w_.kernel().frame_number();
    if (f != crit_frame_ || crit_done_.empty()) {
        WorldCritContext ctx(const_cast<World&>(w_));
        crit_done_ = crit_->complete(crit_->done(ctx));
        crit_frame_ = f;
    }
    return size_t(k) < crit_done_.size() && crit_done_[size_t(k)];
}

// ---------------------------------------------------------------------------------------------
// Spawning

bool Party::create_actor(int idx, int x, int y, int z, unsigned map) {
    const CompanionDef& d = defs[size_t(idx)];
    CompanionState& st = state[size_t(idx)];
    auto a = std::make_unique<Actor>();
    a->shape = uint16_t(d.shape);
    a->origin = Item::kCreated;
    a->flags = kItemInNpcList;
    a->ext_flags = kExtCompanion;
    const bool lv = w_.leveling && w_.leveling->enabled;  // the rebased stats with Leveling on
    a->str = int16_t(lv ? d.lv_str : d.str);
    a->dex = int16_t(lv ? d.lv_dex : d.dex);
    a->intel = int16_t(lv ? d.lv_int : d.intel);
    a->mana = int16_t(std::max(lv ? d.lv_mana : d.mana, 0));
    a->dir = uint8_t(d.spawn_dir & 7);
    a->last_anim = 2;
    a->last_anim_dir = a->dir;
    a->name = d.name;
    if (const Actor* av = w_.avatar()) {
        a->alignment = av->alignment;
        a->enemy_alignment = av->enemy_alignment;
    }
    if (const AnimSequence* s = w_.data().anim.get(d.shape, 2)) a->frame = s->at(a->dir, 0).frame;
    // Ids 248..255 when free (well away from the 61 permanent NPCs and the monsters' low ids).
    uint16_t id = 0;
    ObjectTable& objs = w_.objects();
    for (unsigned want = 248 + unsigned(idx); want <= 255 && !id; ++want)
        if (!objs.get(uint16_t(want))) {
            auto* raw = a.get();
            if (objs.add_at(uint16_t(want), std::move(a))) id = uint16_t(want);
            else a.reset(raw);
        }
    if (!id) {
        for (unsigned want = 255; want >= 200 && !id; --want)
            if (!objs.get(uint16_t(want)) && a) {
                auto* raw = a.get();
                if (objs.add_at(uint16_t(want), std::move(a))) id = uint16_t(want);
                else a.reset(raw);
            }
    }
    if (!id && a) id = objs.add_actor(std::move(a));
    if (!id) return false;
    Actor* na = w_.actor(id);
    na->npcnum = uint8_t(id);
    na->mapnum = uint8_t(map);
    na->hp = uint16_t(max_hp(*na));
    na->mana = int16_t(max_mana(*na));
    st.id = id;
    // A backpack (slot 7) like the Avatar's, the starting equipment and the pack's contents.
    uint16_t pack = w_.create_item(529, 0);
    if (Item* p = w_.item(pack)) {
        p->parent = id;
        p->flags |= kItemEquipped;
        p->z = 7;
        na->contents.push_back(pack);
    }
    for (const ItemSpec& s : d.equip) {
        uint16_t it = w_.create_item(s.shape, s.frame);
        if (Item* ii = w_.item(it)) {
            if (s.qty) ii->quality = uint16_t(s.qty);
            if (!w_.equip(id, it) && !(pack && w_.move_to_container(it, pack))) w_.destroy_now(it);
        }
    }
    for (const ItemSpec& s : d.pack) {
        uint16_t it = w_.create_item(s.shape, s.frame);
        if (Item* ii = w_.item(it)) {
            if (s.qty) ii->quality = uint16_t(s.qty);
            if (!pack || !w_.move_to_container(it, pack)) w_.destroy_now(it);
        }
    }
    st.wait_map = uint8_t(map);
    if (map == w_.map().map_number()) w_.move_to(id, x, y, z, false);
    else {
        na->x = x;
        na->y = y;
        na->z = z;
    }
    st.mode = CompanionState::kIdle;
    if (w_.leveling) w_.leveling->on_spawned(id);
    if (lv) {
        na->hp = uint16_t(max_hp(*na));
        na->mana = int16_t(max_mana(*na));
    }
    return true;
}

bool Party::spawn_all() {
    if (!enabled || defs.empty()) return false;
    spawned_once = true;
    build_remaps();
    int made = 0;
    for (size_t i = 0; i < defs.size(); ++i) {
        if (state[i].mode != CompanionState::kUnspawned) continue;
        const CompanionDef& d = defs[i];
        unsigned map = 3;
        int pos[3], dir = d.spawn_dir;
        if (!resting_place(int(i), map, pos, dir)) continue;
        int x = pos[0], y = pos[1], z = pos[2];
        if (w_.map().map_number() == map) {
            int sx, sy, sz;
            if (find_spot(x, y, z, 0, sx, sy, sz, 0, 96)) { x = sx; y = sy; z = sz; }
        }
        if (create_actor(int(i), x, y, z, map)) {
            ++made;
            CompanionState& st = state[i];
            st.wait_pos[0] = x;
            st.wait_pos[1] = y;
            st.wait_pos[2] = z;
            st.wait_map = uint8_t(map);
            st.next_bark = w_.kernel().frame_number() + 300 + 97 * unsigned(i);
            if (Actor* a = w_.actor(st.id)) {
                a->dir = uint8_t(dir & 7);
                a->last_anim_dir = a->dir;
                if (const AnimSequence* s = w_.data().anim.get(d.shape, 2)) a->frame = s->at(a->dir, 0).frame;
            }
            PG_LOG("party: %s (actor %u, shape %u) at %d,%d,%d on map %u", d.name.c_str(), st.id, d.shape, x, y, z, map);
        }
    }
    log.push_back("spawned " + std::to_string(made));
    return made > 0;
}

void Party::spawn_here() {
    if (!enabled) return;
    build_remaps();
    spawned_once = true;
    const Actor* av = w_.avatar();
    if (!av) return;
    for (size_t i = 0; i < defs.size(); ++i) {
        if (state[i].mode == CompanionState::kUnspawned) {
            if (!create_actor(int(i), av->x, av->y, av->z, w_.map().map_number())) continue;
        }
        Actor* a = w_.actor(state[i].id);
        if (!a) continue;
        int x, y, z;
        if (!find_spot(av->x, av->y, av->z, a->id, x, y, z, 48, 320)) continue;
        if (a->mapnum != w_.map().map_number()) {
            w_.detach(a->id);
            a->mapnum = uint8_t(w_.map().map_number());
        }
        w_.move_to(a->id, x, y, z, false);
        state[i].wait_pos[0] = x;
        state[i].wait_pos[1] = y;
        state[i].wait_pos[2] = z;
        state[i].wait_map = uint8_t(w_.map().map_number());
    }
}

namespace {
// The two states of one place (specs/world/map_index.md): only one is ever walked, by a story global.
unsigned twin_map(unsigned m) {
    switch (m) {
    case 16: return 26;
    case 26: return 16;
    case 24: return 25;
    case 25: return 24;
    default: return m;
    }
}
}  // namespace

bool Party::resting_place(int idx, unsigned& map, int pos[3], int& dir, bool own_home) const {
    const CompanionDef& d = defs[size_t(idx)];
    const bool beach_ok = compat::fixed_map_known(3) && compat::nonfixed_map_known(3);
    const bool home_ok = d.has_home && compat::fixed_map_known(d.home_map) && compat::nonfixed_map_known(d.home_map);
    const bool wants_beach = (!own_home && (debug_beach_ || beach_always)) || !d.has_home;
    if (home_ok && !(wants_beach && beach_ok)) {
        map = d.home_map;
        std::copy(d.home, d.home + 3, pos);
        dir = d.home_dir;
        return true;
    }
    if (beach_ok) {
        // A copy of the game whose home map differs: this one waits on the beach instead.
        if (d.has_home && !home_ok)
            compat::off(d.name + "'s home", "map " + std::to_string(d.home_map) + " differs in this copy; on the beach instead");
        map = 3;
        std::copy(d.spawn, d.spawn + 3, pos);
        dir = d.spawn_dir;
        return true;
    }
    // Both maps differ: there is no spot we can vouch for, so this one does not appear.
    compat::off(d.name, "map 3" + (d.has_home && d.home_map != 3 ? " and map " + std::to_string(d.home_map) : std::string()) + " differ in this copy");
    return false;
}

void Party::relocate(int idx, unsigned map, const int pos[3], int dir) {
    CompanionState& st = state[size_t(idx)];
    Actor* a = w_.actor(st.id);
    if (!a) return;
    if (talk_.idx == idx) end_talk();
    w_.kernel().kill(st.id, kProcAny);
    w_.close_gump(st.id);
    w_.close_bark(st.id);
    a->set(Actor::kAnimLock, false);
    a->set(Actor::kInCombat, false);
    if (a->chunk >= 0) w_.detach(st.id);
    a->flags &= uint16_t(~kItemEthereal);
    a->mapnum = uint8_t(map);
    a->dir = uint8_t(dir & 7);
    a->last_anim = 2;
    a->last_anim_dir = a->dir;
    if (const AnimSequence* s = w_.data().anim.get(a->shape, 2)) a->frame = s->at(a->dir, 0).frame;
    int x = pos[0], y = pos[1], z = pos[2];
    if (map == w_.map().map_number()) {
        // On this map: beside the exact spot if the Avatar or something else stands on it.
        int sx, sy, sz;
        if (find_spot(x, y, z, st.id, sx, sy, sz, 0, 96)) { x = sx; y = sy; z = sz; }
        w_.move_to(st.id, x, y, z, false);
    } else {
        a->flags &= uint16_t(~kItemFastArea);
        a->x = x;
        a->y = y;
        a->z = z;
    }
    st.wait_pos[0] = x;
    st.wait_pos[1] = y;
    st.wait_pos[2] = z;
    st.wait_map = uint8_t(map);
    st.target = 0;
    st.pending_hit = 0;
    st.mode = CompanionState::kIdle;
}

void Party::sync_homes() {
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        // Recruited at any time (in the party, waiting, parted): they stay. The dead stay dead.
        if (st.mode != CompanionState::kIdle || st.hidden || st.joined_before || in_party(int(i))) continue;
        const Actor* a = w_.actor(st.id);
        if (!a || a->is(Actor::kDead)) continue;
        unsigned map = 3;
        int pos[3], dir = 4;
        if (!resting_place(int(i), map, pos, dir)) continue;
        const bool same_map = a->mapnum == map || a->mapnum == twin_map(map);
        if (same_map && std::abs(a->x - pos[0]) <= 96 && std::abs(a->y - pos[1]) <= 96) continue;
        relocate(int(i), map, pos, dir);
        log.push_back(defs[i].id + (map == 3 && !(defs[i].has_home && defs[i].home_map == 3) ? " beach" : " home"));
    }
}

void Party::send_dead_home() {
    const unsigned here = w_.map().map_number();
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        if (st.mode != CompanionState::kDead || st.hidden) continue;
        Actor* a = w_.actor(st.id);
        if (!a || !a->is(Actor::kDead)) continue;
        if (a->mapnum == here || twin_map(a->mapnum) == here) continue;  // still where the Avatar is: can be raised
        unsigned map = 3;
        int pos[3], dir = 4;
        // Someone recruited goes to their own home; the never-recruited (old saves) follow the rule
        // of their kind, the beach while debug mode gathers them.
        if (!resting_place(int(i), map, pos, dir, st.joined_before)) continue;
        a->set(Actor::kDead, false);
        a->set(Actor::kStunned, false);
        a->set(Actor::kPoisoned, false);
        a->flags &= uint16_t(~(kItemGumpOpen | 0x8000));
        relocate(int(i), map, pos, dir);  // mode: idle
        a->hp = uint16_t(max_hp(*a));
        a->mana = int16_t(max_mana(*a));
        st.before_death = CompanionState::kIdle;
        st.flags.insert("came_home");
        log.push_back(defs[i].id + " came home");
        PG_LOG("party: %s has gone home, alive", defs[i].name.c_str());
    }
}

int Party::send_all_home() {
    if (!enabled) return 0;
    int sent = 0;
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        if (st.mode == CompanionState::kUnspawned || st.hidden) continue;
        Actor* a = w_.actor(st.id);
        if (!a) continue;
        unsigned map = 3;
        int pos[3], dir = 4;
        if (!resting_place(int(i), map, pos, dir, true)) continue;
        if (in_party(int(i))) part_ways(int(i));
        members.erase(std::remove(members.begin(), members.end(), int(i)), members.end());
        a->set(Actor::kDead, false);
        a->set(Actor::kStunned, false);
        a->set(Actor::kPoisoned, false);
        a->flags &= uint16_t(~(kItemGumpOpen | 0x8000));
        relocate(int(i), map, pos, dir);  // mode: idle
        a->hp = uint16_t(max_hp(*a));
        a->mana = int16_t(max_mana(*a));
        st.before_death = CompanionState::kIdle;
        log.push_back(defs[i].id + " sent home");
        ++sent;
    }
    update_bar();
    return sent;
}

void Party::set_debug_beach(bool on) {
    if (on == debug_beach_) return;
    debug_beach_ = on;
    homes_dirty_ = true;
}

std::string Party::where_text(int idx) const {
    const CompanionState& st = state[size_t(idx)];
    const Actor* a = w_.actor(st.id);
    if (st.mode == CompanionState::kUnspawned || !a) return "not here";
    std::string at = "map " + std::to_string(a->mapnum) + " " + std::to_string(a->x) + "," + std::to_string(a->y);
    if (a->is(Actor::kDead)) return "dead, " + at;
    if (st.hidden) return "hidden";
    if (st.mode == CompanionState::kFollow) return "in the party";
    if (st.mode == CompanionState::kWait) return "waits, " + at;
    return (st.joined_before ? "parted, " : "") + at;
}

void Party::despawn_all() {
    end_talk();
    for (CompanionState& st : state) {
        if (st.mode == CompanionState::kUnspawned || st.hidden) continue;
        st.hidden = true;
        if (Actor* a = w_.actor(st.id)) {
            w_.kernel().kill(st.id, kProcAny);
            w_.close_gump(st.id);
            w_.close_bark(st.id);
            if (a->chunk >= 0) w_.detach(st.id);
            a->flags |= kItemEthereal;
        }
    }
    for (Summon& s : summons) w_.destroy(s.id);
    summons.clear();
    update_bar();
}

void Party::respawn_hidden() {
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        if (!st.hidden) continue;
        st.hidden = false;
        Actor* a = w_.actor(st.id);
        if (!a) continue;
        a->flags &= uint16_t(~kItemEthereal);
        if (in_party(int(i)) && st.mode == CompanionState::kFollow) { st.placing = true; continue; }
        if (a->mapnum == w_.map().map_number()) w_.move_to(st.id, a->x, a->y, a->z, false);
    }
    homes_dirty_ = true;  // debug mode may have changed while they were away
}

bool Party::on_screen(int x, int y, int z, int margin) const {
    Item probe;
    probe.x = x;
    probe.y = y;
    probe.z = z;
    int sx, sy;
    w_.screen_point(probe, sx, sy);
    int vw = std::max(320, w_.view_w), vh = std::max(200, w_.view_h);
    int cx = 160 - vw / 2, cy = 100 - vh / 2;
    return sx > cx - margin && sx < cx + vw + margin && sy > cy - margin && sy < cy + vh + 60 + margin;
}

bool Party::find_spot(int nx, int ny, int nz, uint16_t self, int& ox, int& oy, int& oz, int min_r, int max_r,
                      bool prefer_hidden) const {
    unsigned shape = 1;
    if (const Actor* a = w_.actor(self)) shape = a->shape;
    const ShapeInfo& si = w_.data().typeflags.get(shape);
    int xd, yd, zd;
    si.footprint(false, xd, yd, zd);
    bool have_fallback = false;
    int fx = 0, fy = 0, fz = 0;
    for (int r = min_r; r <= max_r; r += 32) {
        int n = r == 0 ? 1 : std::max(8, r / 16);
        for (int k = 0; k < n; ++k) {
            double ang = 6.2831853 * k / n;
            int x = nx + int(std::lround(r * std::cos(ang))), y = ny + int(std::lround(r * std::sin(ang)));
            for (int dz : {0, 8, -8, 16, -16, 24}) {
                int z = nz + dz;
                if (z < 0) continue;
                World::Placement pl = w_.can_exist(x, y, z, xd, yd, zd, si.flags(), self);
                if (!pl.valid || !pl.supported || !pl.land) continue;
                const Item* land = w_.item(pl.land);
                if (land) {
                    const ShapeInfo& li = w_.data().typeflags.get(land->shape);
                    if (li.is(ShapeInfo::kDamaging)) continue;
                    if (li.is(ShapeInfo::kSea) && !li.is(ShapeInfo::kLand)) continue;
                }
                // An actor in the way does not count as solid ground for us either.
                if (land && land->as_actor()) continue;
                if (prefer_hidden && on_screen(x, y, z)) {
                    if (!have_fallback) { have_fallback = true; fx = x; fy = y; fz = z; }
                    break;
                }
                ox = x; oy = y; oz = z;
                return true;
            }
        }
    }
    if (have_fallback) { ox = fx; oy = fy; oz = fz; return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Recolour tables

void Party::build_remaps() {
    if (remap_.size() == defs.size()) return;
    remap_.clear();
    doll_remap_.clear();
    for (const CompanionDef& d : defs) {
        remap_.push_back(build_table(w_.data(), d.recolour));
        doll_remap_.push_back(build_table(w_.data(), d.doll_recolour.empty() ? d.recolour : d.doll_recolour));
    }
}

const uint8_t* Party::remap(uint16_t actor) const {
    if (actor >= 256 || actor == 0) return nullptr;
    int i = index_of(actor);
    if (i < 0 || size_t(i) >= remap_.size()) return nullptr;
    return remap_[size_t(i)].data();
}

const uint8_t* Party::doll_remap(uint16_t actor) const {
    int i = index_of(actor);
    if (i < 0) return nullptr;
    const_cast<Party*>(this)->build_remaps();
    return size_t(i) < doll_remap_.size() ? doll_remap_[size_t(i)].data() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Hooks

bool Party::passes_through(uint16_t mover, uint16_t other) const {
    if (other >= 256 || other == 0 || state.empty()) return false;
    if (mover == other) return false;
    if (other != 1 && !is_companion(other)) return false;
    // During scenes nobody bumps into a companion (they stay out of scripted NPCs' way).
    if (other != 1 && w_.stasis) return true;
    // Party members (the Avatar and the companions) pass through each other.
    return mover == 1 || is_companion(mover);
}

bool Party::same_side(uint16_t a, uint16_t b) const {
    if (state.empty() || a == b || a == 0 || b == 0) return false;
    auto side = [&](uint16_t id) {
        if (id == 1 || is_companion(id)) return true;
        for (const Summon& s : summons)
            if (s.id == id) return true;
        return false;
    };
    return side(a) && side(b);
}

bool Party::handle_event(uint16_t id, unsigned event) {
    int i = index_of(id);
    if (i < 0) return false;
    const Actor* a = w_.actor(id);
    if (!a) return false;
    if (event == kEvLook) {
        const CompanionDef& d = defs[size_t(i)];
        std::string t = a->is(Actor::kDead) ? "the body of " + d.name : d.name;
        if (!a->is(Actor::kDead) && !d.title.empty()) t += ", " + d.title;
        w_.bark(id, t);
        return true;
    }
    if (event == kEvUse) {
        if (a->is(Actor::kDead)) return false;  // the body's window (World::use_item)
        if (talking()) end_talk();
        start_talk(i);
        return true;
    }
    return true;  // no usecode ever runs on a companion
}

void Party::on_hit(uint16_t victim, uint16_t attacker, int damage) {
    if (attacker == 1 && !same_side(1, victim)) {
        if (const Actor* v = w_.actor(victim); v && !v->is(Actor::kDead)) avatar_target = victim;
    }
    if (attacker == 1 && !members.empty()) {
        const Actor* v = w_.actor(victim);
        const Actor* av = w_.avatar();
        if (v && av && !is_enemy(*v, *av) && !is_enemy(*av, *v) && !is_companion(victim)) innocent_ = victim;
    }
    int vi = index_of(victim);
    if (vi >= 0) {
        if (attacker && !same_side(victim, attacker)) state[size_t(vi)].target = attacker;
        if (const Actor* v = w_.actor(victim); v && damage > 0 && !v->is(Actor::kDead) && int(v->hp) * 4 < max_hp(*v))
            bark_trigger(vi, "low_hp");
    }
    // Ally hurt: the others turn on whoever did it.
    if (party_member(victim) && attacker && !same_side(victim, attacker))
        for (int m : members)
            if (!state[size_t(m)].target) state[size_t(m)].target = attacker;
}

void Party::on_avatar_theft() {
    if (members.empty() || w_.rand_below(3) != 0) return;
    const Actor* av = w_.avatar();
    if (!av) return;
    // Only in company (a permanent NPC nearby): picking things up in the wild is not theft.
    std::vector<uint16_t> near;
    w_.area_search(av->x, av->y, 512, false, near);
    bool people = false;
    for (uint16_t k : near)
        if (const Actor* o = w_.actor(k); o && k != 1 && (o->ext_flags & kExtPermanentNpc) && !o->is(Actor::kDead)) people = true;
    if (!people) return;
    bark_trigger(members[size_t(w_.rand_below(int(members.size())))], "theft");
}

uint16_t Party::monster_retarget(uint16_t monster, uint16_t current) {
    if (!enabled || members.empty()) return 0;
    const Actor* m = w_.actor(monster);
    if (!m || same_side(monster, 1)) return 0;
    const Actor* cur = w_.actor(current);
    auto d2 = [&](const Actor& o) {
        long long dx = o.x - m->x, dy = o.y - m->y;
        return dx * dx + dy * dy;
    };
    long long best = cur && !cur->is(Actor::kDead) ? d2(*cur) : (1LL << 40);
    uint16_t pick = 0;
    for (int i : members) {
        const Actor* c = w_.actor(state[size_t(i)].id);
        if (!c || c->is(Actor::kDead) || c->id == current || c->mapnum != w_.map().map_number()) continue;
        if (!(c->flags & kItemFastArea) || !is_enemy(*m, *c)) continue;
        long long dd = d2(*c);
        // A companion clearly nearer than the current target draws the attack.
        if (dd * 2 < best && dd < 768LL * 768L) { best = dd; pick = c->id; }
    }
    return pick;
}

void Party::before_map_change(unsigned new_map) {
    if (!enabled) return;
    end_talk();
    for (size_t i = 0; i < state.size(); ++i) {
        CompanionState& st = state[i];
        // Carthax Lake and the lava tunnel are one place in two states (a story flag picks the map):
        // whoever rests in the other state is here in this one, alive or dead.
        if (st.mode != CompanionState::kUnspawned && st.mode != CompanionState::kFollow && twin_map(new_map) != new_map) {
            Actor* t = w_.actor(st.id);
            if (t && t->mapnum == twin_map(new_map)) {
                t->mapnum = uint8_t(new_map);
                st.wait_map = uint8_t(new_map);
            }
        }
        if (st.hidden || st.mode != CompanionState::kFollow) continue;
        Actor* a = w_.actor(st.id);
        if (!a || a->is(Actor::kDead)) continue;
        w_.kernel().kill(st.id, kProcAny);
        a->set(Actor::kAnimLock, false);
        a->set(Actor::kInCombat, false);
        a->mapnum = uint8_t(new_map);
        a->flags |= kItemEthereal;  // not linked by the map load; placed beside the Avatar next frame
        st.placing = true;
    }
    for (Summon& s : summons) w_.destroy(s.id);
    summons.clear();
    bolts.clear();
}

// ---------------------------------------------------------------------------------------------
// Frames

void Party::pre_frame() {
    if (!enabled || state.empty()) return;
    if (talking()) talk_frame();
}

// The backpack window opened by Trade goes beside the paperdoll instead of on top of it.
void Party::place_pack() {
    if (!place_pack_) return;
    auto* g = dynamic_cast<ContainerGump*>(w_.gumps.container_gump(place_pack_));
    auto* d = dynamic_cast<ContainerGump*>(w_.gumps.container_gump(place_doll_));
    if (!g || !d) {
        if (++place_tries_ > 60) place_pack_ = 0;
        return;
    }
    if (d->w + 2 + g->w > 320) {
        // Too wide side by side (a portrait sheet): the sheet at the top left, the backpack in the
        // bottom right corner, overlapping at most the sheet's bottom-right frame corner.
        int dx = 0, dy = 0;
        d->off_x += dx - d->x;
        d->off_y += dy - d->y;
        d->follow_item();
        g->off_x += (320 - g->w) - g->x;
        g->off_y += (200 - g->h) - g->y;
        g->follow_item();
        place_pack_ = 0;
        return;
    }
    // Side by side: shift the paperdoll left if the pair would not fit.
    int total = d->w + 2 + g->w;
    int dx = std::clamp(d->x, 0, std::max(0, 320 - total));
    if (dx != d->x) { d->off_x += dx - d->x; d->follow_item(); }
    int want_x = d->x + d->w + 2;
    if (want_x + g->w > 320) want_x = std::max(0, d->x - g->w - 2);
    int want_y = std::clamp(d->y, 0, std::max(0, 200 - g->h));
    g->off_x += want_x - g->x;
    g->off_y += want_y - g->y;
    g->follow_item();
    place_pack_ = 0;
}

void Party::frame() {
    if (!enabled || defs.empty()) return;
    place_pack();
    if (homes_dirty_ && spawned_once && !w_.stasis) {  // not in the middle of a scene
        homes_dirty_ = false;
        sync_homes();
    }
    if (w_.kernel().paused()) return;
    Actor* av = w_.avatar();
    if (!av) return;
    // Spawn once the opening has released the Avatar (never during the opening scene).
    if (!spawned_once) {
        bool opening_done = w_.skip_opening || w_.startup_state != 0;
        if (opening_done && !w_.stasis && w_.kernel().frame_number() > start_frame_ + 30 &&
            !av->is(Actor::kDead))
            spawn_all();
        return;
    }
    if (last_map_ != w_.map().map_number()) {
        last_map_ = w_.map().map_number();
        for (size_t i = 0; i < state.size(); ++i)
            if (state[i].mode != CompanionState::kUnspawned && !state[i].hidden && in_party(int(i)))
                bark_trigger(int(i), "map:" + std::to_string(last_map_));
    }
    check_deaths();
    if (!w_.stasis) send_dead_home();
    step_bolts();
    tick_summons();
    for (size_t i = 0; i < state.size(); ++i)
        if (state[i].mode != CompanionState::kUnspawned && !state[i].hidden &&
            !(w_.turn && w_.turn->holds_ai(state[i].id)))  // turn-based combat drives them
            ai(int(i));
    banter->frame();
    ambient_barks();
    update_bar();
}

// ---------------------------------------------------------------------------------------------
// Saving (savegame version 4, inside WRLD after the visited maps)

void Party::save(SaveWriter& o) const {
    o.tag("PRTY");
    o.b(spawned_once);
    // Only companions that exist (with Companions off or before the beach spawn this is empty).
    uint32_t n = 0;
    for (const CompanionState& s : state) n += s.mode != CompanionState::kUnspawned;
    o.u32(n);
    for (size_t i = 0; i < state.size(); ++i) {
        const CompanionState& s = state[i];
        if (s.mode == CompanionState::kUnspawned) continue;
        o.str(i < defs.size() ? defs[i].id : std::string());
        o.u16(s.id); o.u8(s.mode); o.u8(s.before_death); o.b(s.joined_before); o.b(s.hidden);
        o.u32(uint32_t(s.flags.size()));
        for (const std::string& f : s.flags) o.str(f);
        o.u32(uint32_t(s.asked.size()));
        for (const std::string& f : s.asked) o.str(f);
        for (int v : s.wait_pos) o.i32(v);
        o.u8(s.wait_map); o.u16(s.target); o.b(s.placing);
        o.u32(s.next_bark); o.u32(s.next_act); o.u32(s.next_cast); o.u32(s.next_regen); o.u32(s.last_seen_map);
        o.i32(s.stuck); o.i32(s.last_pos[0]); o.i32(s.last_pos[1]); o.i32(s.far_frames);
    }
    o.u32(uint32_t(members.size()));
    for (int m : members) o.str(defs[size_t(m)].id);
    o.u32(uint32_t(bolts.size()));
    for (const PartyBolt& b : bolts) {
        o.u16(b.item); o.u16(b.caster); o.u16(b.target); o.i32(b.spell);
        for (int v : b.from) o.i32(v);
        o.i32(b.frames); o.i32(b.total);
    }
    o.u32(uint32_t(summons.size()));
    for (const Summon& s : summons) { o.u16(s.id); o.u32(s.until); }
    o.u16(avatar_target);
    o.u32(next_ambient_); o.u32(start_frame_); o.u32(last_map_); o.u16(innocent_);
    // Extension (later v4 builds): AI-resolved melee blows, per spawned companion in order.
    o.tag("PRT2");
    for (const CompanionState& s : state)
        if (s.mode != CompanionState::kUnspawned) { o.u16(s.pending_hit); o.u32(s.pending_hit_at); }
}

bool Party::load(SaveReader& in) {
    reset();
    if (!in.tag("PRTY")) return false;
    spawned_once = in.b();
    uint32_t n = std::min<uint32_t>(in.u32(), 64);
    for (uint32_t k = 0; k < n && !in.fail(); ++k) {
        std::string id = in.str();
        CompanionState s;
        s.id = uint16_t(in.u16()); s.mode = CompanionState::Mode(in.u8()); s.before_death = CompanionState::Mode(in.u8());
        s.joined_before = in.b(); s.hidden = in.b();
        uint32_t nf = std::min<uint32_t>(in.u32(), 1000);
        for (uint32_t q = 0; q < nf; ++q) s.flags.insert(in.str());
        nf = std::min<uint32_t>(in.u32(), 1000);
        for (uint32_t q = 0; q < nf; ++q) s.asked.insert(in.str());
        for (int& v : s.wait_pos) v = in.i32();
        s.wait_map = uint8_t(in.u8()); s.target = uint16_t(in.u16()); s.placing = in.b();
        s.next_bark = in.u32(); s.next_act = in.u32(); s.next_cast = in.u32(); s.next_regen = in.u32(); s.last_seen_map = in.u32();
        s.stuck = in.i32(); s.last_pos[0] = in.i32(); s.last_pos[1] = in.i32(); s.far_frames = in.i32();
        int d = find_def(id);
        if (d >= 0) state[size_t(d)] = s;
        else if (s.id && s.mode != CompanionState::kUnspawned)
            PG_LOG("party: the save has companion '%s', who is not in companions.json", id.c_str());
    }
    uint32_t nm = std::min<uint32_t>(in.u32(), 64);
    for (uint32_t k = 0; k < nm && !in.fail(); ++k) {
        int d = find_def(in.str());
        if (d >= 0 && !in_party(d)) members.push_back(d);
    }
    uint32_t nb = std::min<uint32_t>(in.u32(), 256);
    for (uint32_t k = 0; k < nb && !in.fail(); ++k) {
        PartyBolt b;
        b.item = uint16_t(in.u16()); b.caster = uint16_t(in.u16()); b.target = uint16_t(in.u16()); b.spell = in.i32();
        for (int& v : b.from) v = in.i32();
        b.frames = in.i32(); b.total = in.i32();
        bolts.push_back(b);
    }
    uint32_t ns = std::min<uint32_t>(in.u32(), 64);
    for (uint32_t k = 0; k < ns && !in.fail(); ++k) {
        Summon s;
        s.id = uint16_t(in.u16());
        s.until = in.u32();
        summons.push_back(s);
    }
    avatar_target = uint16_t(in.u16());
    next_ambient_ = in.u32(); start_frame_ = in.u32(); last_map_ = in.u32(); innocent_ = uint16_t(in.u16());
    {
        SaveReader peek = in;
        if (peek.tag("PRT2")) {
            in.tag("PRT2");
            for (CompanionState& s : state)
                if (s.mode != CompanionState::kUnspawned) { s.pending_hit = uint16_t(in.u16()); s.pending_hit_at = in.u32(); }
        }
    }
    if (spawned_once) build_remaps();
    // Older builds kept every companion on the beach and sent anyone who parted walking back there:
    // the never-recruited go to their resting places at the next frame; a walk under way just ends.
    for (size_t i = 0; i < state.size(); ++i)
        if (state[i].mode == CompanionState::kReturn) {
            state[i].mode = CompanionState::kIdle;
            if (const Actor* a = w_.actor(state[i].id)) {
                state[i].wait_pos[0] = a->x; state[i].wait_pos[1] = a->y; state[i].wait_pos[2] = a->z;
                state[i].wait_map = a->mapnum;
            }
        }
    homes_dirty_ = spawned_once;
    // A save made when a companion wore other art (e.g. Nerissa before her own shape): the actor
    // takes the shape of the data now, standing (or lying dead) in its facing.
    for (size_t i = 0; i < state.size() && i < defs.size(); ++i) {
        Actor* a = w_.actor(state[i].id);
        if (!a || state[i].mode == CompanionState::kUnspawned || a->shape == defs[i].shape) continue;
        if (!w_.data().shapes.shape(defs[i].shape) || !w_.data().anim.has_shape(defs[i].shape)) continue;
        a->shape = uint16_t(defs[i].shape);
        bool dead = state[i].mode == CompanionState::kDead;
        if (const AnimSequence* s = w_.data().anim.get(defs[i].shape, dead ? 14 : 2))
            a->frame = s->at(a->dir, s->length - 1).frame;
    }
    if (!in.fail() && !enabled && spawned_once) despawn_all();  // a save with companions, loaded with them off
    return !in.fail();
}

}  // namespace pg
