// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Leveling stage 5: companion spells and reagents (game/companion_magic.h, NOTES.md "Companion
// spells and reagents").
#include "game/companion_magic.h"
#include "game/banter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "core/log.h"
#include "core/serial.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/stat_growth.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using nlohmann::json;

bool MagicCompanion::counts_uses() const {
    return school == "Necromancy" || school == "Sorcery" || school == "Thaumaturgy";
}

CompanionMagic::CompanionMagic(World& w, Leveling& l) : w_(w), l_(l) {}

int CompanionMagic::price_of(int reagents, int reagent_price, int doses) {
    if (reagents <= 0 || reagent_price <= 0) return 0;
    doses = std::max(1, doses);
    // Rounded half up: 4.5 -> 5.
    return std::max(1, (2 * reagents * reagent_price + doses) / (2 * doses));
}

bool CompanionMagic::load_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not valid JSON"; return false; }
    companions.clear();
    avatar_spells.clear();
    reagent_price.clear();
    doses = std::max(1, j.value("doses", doses));
    mythran = j.value("mythran_npc", mythran);
    if (j.contains("reagent_price") && j["reagent_price"].is_object())
        for (auto it = j["reagent_price"].begin(); it != j["reagent_price"].end(); ++it)
            if (it.value().is_number_integer()) reagent_price[it.key()] = it.value().get<int>();
    if (j.contains("avatar_spells") && j["avatar_spells"].is_object())
        for (auto it = j["avatar_spells"].begin(); it != j["avatar_spells"].end(); ++it) {
            const json& e = it.value();
            AvatarSpell a;
            a.id = it.key();
            a.name = e.value("name", a.id);
            a.why = e.value("why", std::string());
            if (e.contains("global") && e["global"].is_array() && e["global"].size() >= 2) {
                a.global_pos = e["global"][0].get<int>();
                a.global_width = e["global"][1].get<int>();
                if (e["global"].size() >= 3) a.global_value = e["global"][2].get<int>();
            }
            for (const json& s : e.value("items", json::array()))
                if (s.is_array() && s.size() >= 2) a.items.emplace_back(s[0].get<unsigned>(), s[1].get<unsigned>());
            avatar_spells[a.id] = a;
        }
    if (j.contains("companions") && j["companions"].is_object())
        for (auto it = j["companions"].begin(); it != j["companions"].end(); ++it) {
            MagicCompanion c;
            c.id = it.key();
            c.school = it.value().value("school", std::string());
            const json& sp = it.value().value("spells", json::object());
            for (auto s = sp.begin(); s != sp.end(); ++s) {
                MagicSpellRule r;
                r.spell = s.key();
                const json& e = s.value();
                const json u = e.value("unlock", json());
                if (u.is_string() && u.get<std::string>() == "always") r.always = true;
                else if (u.is_object()) {
                    r.level = std::clamp(u.value("level", 1), 1, kMaxLevel);
                    for (const json& a : u.value("avatar", json::array()))
                        if (a.is_string()) {
                            if (!avatar_spells.count(a.get<std::string>())) { err = "unknown avatar spell " + a.get<std::string>(); return false; }
                            r.avatar.push_back(a.get<std::string>());
                        }
                }
                r.stock = std::max(0, e.value("stock", 0));
                r.model = e.value("model", std::string());
                r.reagents = std::max(0, e.value("reagents", 0));
                auto pr = reagent_price.find(c.school);
                r.price = price_of(r.reagents, pr == reagent_price.end() ? 0 : pr->second, doses);
                if (c.counts_uses() && (r.stock <= 0 || r.price <= 0)) { err = c.id + "." + r.spell + ": a reagent spell needs stock and a price"; return false; }
                if (!c.counts_uses()) r.stock = 0;
                c.spells.push_back(r);
            }
            companions[c.id] = c;
        }
    loaded = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Queries

const MagicCompanion* CompanionMagic::companion(const std::string& id) const {
    auto it = companions.find(id);
    return it == companions.end() ? nullptr : &it->second;
}

const MagicSpellRule* CompanionMagic::rule(const std::string& cid, const std::string& spell) const {
    const MagicCompanion* c = companion(cid);
    if (!c) return nullptr;
    for (const MagicSpellRule& r : c->spells)
        if (r.spell == spell) return &r;
    return nullptr;
}

int CompanionMagic::left(const std::string& cid, const std::string& spell) const {
    const MagicSpellRule* r = rule(cid, spell);
    if (!r || r->stock <= 0) return -1;
    auto c = uses.find(cid);
    if (c != uses.end()) {
        auto s = c->second.find(spell);
        if (s != c->second.end()) return std::clamp(s->second, 0, r->stock);
    }
    return r->stock;  // never touched: a new game, or an old save (the starting stock)
}

bool CompanionMagic::is_low(const MagicSpellRule& r, int n) const {
    if (r.stock <= 0 || n < 0) return false;
    return n <= std::max(2, r.stock / 4);
}

bool CompanionMagic::learned_now(const std::string& id) const {
    auto it = avatar_spells.find(id);
    if (it == avatar_spells.end()) return false;
    const AvatarSpell& a = it->second;
    if (a.global_pos >= 0 && int(w_.vm().get_global(unsigned(a.global_pos), unsigned(a.global_width))) == a.global_value) return true;
    if (a.items.empty()) return false;
    bool found = false;
    std::function<void(uint16_t)> walk = [&](uint16_t id) {
        const Item* item = w_.item(id);
        if (!item || found) return;
        for (const auto& sf : a.items)
            if (item->shape == sf.first && item->frame == sf.second) found = true;
        for (uint16_t c : item->contents) walk(c);
    };
    if (const Item* av = w_.item(1))
        for (uint16_t c : av->contents) walk(c);
    return found;
}

CompanionMagic::Gate CompanionMagic::gate(int idx, const std::string& spell) const {
    Gate g;
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return g;
    const std::string& cid = p.defs[size_t(idx)].id;
    const MagicSpellRule* r = rule(cid, spell);
    if (!r) return g;  // not in the table: never locked, mana only
    if (!r->always) {
        if (!r->avatar.empty()) {
            bool any = false;
            for (const std::string& a : r->avatar) any = any || learned.count(a);
            if (!any) {
                g.locked = true;
                auto a = avatar_spells.find(r->avatar.front());
                g.why = "needs " + (a != avatar_spells.end() ? a->second.name : r->avatar.front());
                for (size_t k = 1; k < r->avatar.size(); ++k) {
                    auto b = avatar_spells.find(r->avatar[k]);
                    g.why += " or " + (b != avatar_spells.end() ? b->second.name : r->avatar[k]);
                }
            }
        } else if (l_.level_of(p.state[size_t(idx)].id) < r->level) {
            g.locked = true;
            g.why = "level " + std::to_string(r->level);
        }
    }
    g.stock = r->stock;
    g.uses = left(cid, spell);
    if (!g.locked && g.uses == 0) {
        g.empty = true;
        g.why = "out of reagents";
    }
    return g;
}

std::string CompanionMagic::reagent_state(int idx) const {
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return std::string();
    const MagicCompanion* c = companion(p.defs[size_t(idx)].id);
    if (!c || !c->counts_uses()) return std::string();
    bool low = false, out = false;
    for (const MagicSpellRule& r : c->spells) {
        if (gate(idx, r.spell).locked) continue;
        const int n = left(c->id, r.spell);
        if (n == 0) out = true;
        else if (is_low(r, n)) low = true;
    }
    return out ? "out" : low ? "low" : "ok";
}

bool CompanionMagic::worth_casting(int idx, const std::string& spell, uint16_t target) const {
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return true;
    const MagicSpellRule* r = rule(p.defs[size_t(idx)].id, spell);
    if (!r || r->stock <= 0) return true;
    const int n = left(p.defs[size_t(idx)].id, spell);
    if (!is_low(*r, n)) return true;
    const Actor* t = w_.actor(target);
    if (!t || p.party_member(target) || target == p.state[size_t(idx)].id) return true;  // self, summons
    // A monster well below the caster's level, or one nearly finished: a blow will do.
    const int ml = l_.monster_level(*t);
    if (ml > 0 && ml + 2 <= l_.level_of(p.state[size_t(idx)].id)) return false;
    const SpellDef* s = p.spell(spell);
    if (s && s->dmg1 > 0 && int(t->hp) * 2 <= (s->dmg0 + s->dmg1) / 2) return false;
    return true;
}

std::string CompanionMagic::summary(int idx) const {
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return std::string();
    std::string out;
    for (const std::string& sid : p.defs[size_t(idx)].spells) {
        const SpellDef* s = p.spell(sid);
        if (!s) continue;
        Gate g = gate(idx, sid);
        std::string e = s->name;
        if (g.locked) e += " (locked: " + g.why + ")";
        else if (g.uses >= 0) e += " " + std::to_string(g.uses) + "/" + std::to_string(g.stock);
        out += (out.empty() ? "" : ", ") + e;
    }
    return out;
}

int CompanionMagic::restock_cost(int idx) const {
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return 0;
    const MagicCompanion* c = companion(p.defs[size_t(idx)].id);
    if (!c || !c->counts_uses()) return 0;
    int total = 0;
    for (const MagicSpellRule& r : c->spells) total += (r.stock - left(c->id, r.spell)) * r.price;
    return total;
}

// ---------------------------------------------------------------------------------------------
// Events

void CompanionMagic::on_cast(int idx, const std::string& spell) {
    const Party& p = *w_.party;
    if (idx < 0 || size_t(idx) >= p.defs.size()) return;
    const std::string& cid = p.defs[size_t(idx)].id;
    const int n = left(cid, spell);
    if (n < 0) return;
    uses[cid][spell] = std::max(0, n - 1);
    log.push_back(cid + " used " + spell + " " + std::to_string(std::max(0, n - 1)));
}

void CompanionMagic::check_learned() {
    for (const auto& e : avatar_spells)
        if (!learned.count(e.first) && learned_now(e.first)) {
            learned.insert(e.first);
            log.push_back("learned " + e.first);
            PG_LOG("companion magic: the Avatar knows %s", e.second.name.c_str());
        }
}

void CompanionMagic::banter() {
    Party& p = *w_.party;
    const Actor* av = w_.avatar();
    if (!av || !l_.calm() || p.banter->playing()) return;  // not over two companions' banter
    for (const Bark& b : w_.barks)
        if (b.speaker == 1 || p.is_companion(b.speaker)) return;  // one party line at a time
    for (int m : p.members) {
        const CompanionState& st = p.state[size_t(m)];
        const Actor* a = w_.actor(st.id);
        if (!a || a->is(Actor::kDead) || st.hidden || st.mode != CompanionState::kFollow || a->mapnum != av->mapnum) continue;
        if (std::abs(a->x - av->x) + std::abs(a->y - av->y) > 700) continue;
        const std::string& cid = p.defs[size_t(m)].id;
        const std::string s = reagent_state(m);
        if (s == "out" && !said_out.count(cid)) {
            said_out.insert(cid);
            said_low.insert(cid);
            p.bark_trigger(m, "reagents_out", true);
            log.push_back(cid + " says out");
            return;  // one at a time
        }
        if (s == "low" && !said_low.count(cid)) {
            said_low.insert(cid);
            p.bark_trigger(m, "reagents_low", true);
            log.push_back(cid + " says low");
            return;
        }
    }
}

void CompanionMagic::restock() {
    Party& p = *w_.party;
    const Actor* av = w_.avatar();
    if (!av || !l_.growth) return;
    StatGrowth& g = *l_.growth;
    bool barked = false;
    for (int m : p.members) {
        const CompanionState& st = p.state[size_t(m)];
        const Actor* a = w_.actor(st.id);
        const std::string& cid = p.defs[size_t(m)].id;
        const MagicCompanion* c = companion(cid);
        if (!a || !c || !c->counts_uses() || a->is(Actor::kDead) || st.hidden || a->mapnum != av->mapnum) continue;
        if (restock_cost(m) <= 0) continue;  // full
        int money = g.obsidian(st.id);
        std::map<std::string, int> bought;
        int spent = 0;
        // One use at a time, the emptiest spell (by share of its stock) first.
        for (;;) {
            const MagicSpellRule* best = nullptr;
            double best_share = 2;
            for (const MagicSpellRule& r : c->spells) {
                const int n = left(cid, r.spell);
                if (n < 0 || n >= r.stock || r.price > money) continue;
                const double share = double(n) / r.stock;
                if (share < best_share) { best_share = share; best = &r; }
            }
            if (!best) break;
            uses[cid][best->spell] = left(cid, best->spell) + 1;
            money -= best->price;
            spent += best->price;
            bought[best->spell] += 1;
        }
        std::string name = GameLog::name_of(w_, st.id, false);
        if (!name.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        if (bought.empty()) {
            w_.game_log.add(GameLog::kMessages, GameLog::kInfo, name + " has no obsidian for reagents.", st.id);
            if (!barked) { p.bark_trigger(m, "restock_poor", true); barked = true; }
            log.push_back(cid + " poor");
            continue;
        }
        g.pay_own(st.id, spent);
        std::string text = name + " buys reagents: ";
        bool first = true;
        for (const MagicSpellRule& r : c->spells) {
            auto b = bought.find(r.spell);
            if (b == bought.end()) continue;
            const SpellDef* s = p.spell(r.spell);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s%s %d use%s (%d obsidian)", first ? "" : ", ", s ? s->name.c_str() : r.spell.c_str(),
                          b->second, b->second == 1 ? "" : "s", b->second * r.price);
            text += buf;
            first = false;
        }
        text += ".";
        w_.game_log.add(GameLog::kMessages, GameLog::kInfo, text, st.id);
        PG_LOG("companion magic: %s", text.c_str());
        log.push_back(cid + " bought " + std::to_string(spent));
        said_low.erase(cid);
        said_out.erase(cid);
        if (!barked) { p.bark_trigger(m, "restock", true); barked = true; }
    }
}

void CompanionMagic::frame() {
    if (!loaded || !w_.party || !w_.party->enabled) return;
    const uint32_t now = w_.kernel().frame_number();
    if (now >= next_learn_check_) {
        next_learn_check_ = now + 30;
        check_learned();
        banter();
    }
    // The Mythran restock: his conversation (its usecode process) has ended.
    const bool talking = w_.in_conversation() && w_.talk_item == mythran;
    if (talking) talking_mythran_ = true;
    else if (talking_mythran_ && !w_.in_conversation()) {
        talking_mythran_ = false;
        log.push_back("mythran talk ended");
        restock();
    }
}

void CompanionMagic::reset() {
    uses.clear();
    learned.clear();
    said_low.clear();
    said_out.clear();
    log.clear();
    talking_mythran_ = false;
    next_learn_check_ = 0;
}

void CompanionMagic::save(SaveWriter& o) const {
    o.tag("LVL5");
    o.u8(1);
    o.u32(uint32_t(uses.size()));
    for (const auto& c : uses) {
        o.str(c.first);
        o.u32(uint32_t(c.second.size()));
        for (const auto& s : c.second) {
            o.str(s.first);
            o.i32(s.second);
        }
    }
    for (const std::set<std::string>* s : {&learned, &said_low, &said_out}) {
        o.u32(uint32_t(s->size()));
        for (const std::string& x : *s) o.str(x);
    }
}

bool CompanionMagic::load(SaveReader& in) {
    reset();
    if (!in.tag("LVL5")) return false;
    const unsigned ver = in.u8();
    if (ver < 1) return false;
    uint32_t n = std::min<uint32_t>(in.u32(), 64);
    for (uint32_t k = 0; k < n && !in.fail(); ++k) {
        std::string cid = in.str();
        uint32_t m = std::min<uint32_t>(in.u32(), 64);
        for (uint32_t q = 0; q < m && !in.fail(); ++q) {
            std::string sid = in.str();
            uses[cid][sid] = in.i32();
        }
    }
    for (std::set<std::string>* s : {&learned, &said_low, &said_out}) {
        uint32_t c = std::min<uint32_t>(in.u32(), 256);
        for (uint32_t k = 0; k < c && !in.fail(); ++k) s->insert(in.str());
    }
    return !in.fail();
}

}  // namespace pg
