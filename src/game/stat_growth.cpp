// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Leveling stage 4: trainers, tomes, elixirs, ordeals and quest stats (game/stat_growth.h,
// NOTES.md "Trainers, tomes, elixirs and ordeals").
#include "game/stat_growth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "game/critpath.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using nlohmann::json;

namespace {
const char* const kStatNames[3] = {"STR", "DEX", "INT"};
const char* const kStatWords[3] = {"Strength", "Dexterity", "Intelligence"};

std::vector<std::string> lines_of(const json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& s : j)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

void replace_all(std::string& s, const std::string& key, const std::string& value) {
    for (size_t a = 0; (a = s.find(key, a)) != std::string::npos; a += value.size()) s.replace(a, key.size(), value);
}

}  // namespace

StatGrowth::StatGrowth(World& w, Leveling& l) : w_(w), l_(l) {}
StatGrowth::~StatGrowth() = default;

CritPath* StatGrowth::crit() {
    if (!crit_tried_) {
        crit_tried_ = true;
        crit_ = std::make_unique<CritPath>();
        std::string err;
        std::string p = (l_.data_dir.empty() ? std::string("data") : l_.data_dir) + "/critpath.json";
        if (!crit_->load_file(p, err)) PG_LOG("stat growth: %s: %s", p.c_str(), err.c_str());
    }
    return crit_ && !crit_->empty() ? crit_.get() : nullptr;
}

const char* StatGrowth::stat_name(int stat) { return stat >= 0 && stat < 3 ? kStatNames[stat] : "?"; }
const char* StatGrowth::stat_word(int stat) { return stat >= 0 && stat < 3 ? kStatWords[stat] : "?"; }
int StatGrowth::stat_from(const std::string& s) {
    if (s == "str") return kGrowStr;
    if (s == "dex") return kGrowDex;
    if (s == "int") return kGrowInt;
    return -1;
}
int StatGrowth::stat_value(const Actor& a, int stat) { return stat == kGrowStr ? a.str : stat == kGrowDex ? a.dex : a.intel; }

// ---------------------------------------------------------------------------------------------
// Data

bool StatGrowth::load_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not valid JSON"; return false; }
    trainers.clear();
    items.clear();
    ordeals.clear();
    words.clear();
    if (j.contains("training") && j["training"].is_object()) {
        const json& t = j["training"];
        train_cap = t.value("cap", train_cap);
        if (t.contains("price") && t["price"].is_object()) {
            const json& p = t["price"];
            price_base = std::max(1, p.value("base", price_base));
            price_from = p.value("from_stat", price_from);
            price_growth_permille = std::max(0, p.value("growth_permille", price_growth_permille));
            price_round = std::max(1, p.value("round", price_round));
        }
        for (const json& n : t.value("trainers", json::array())) {
            Trainer tr;
            tr.npc = n.value("npc", 0u);
            tr.name = n.value("name", std::string());
            for (const json& s : n.value("stats", json::array()))
                if (s.is_string() && stat_from(s.get<std::string>()) >= 0) tr.stats.push_back(stat_from(s.get<std::string>()));
            if (n.contains("lines") && n["lines"].is_object())
                for (auto it = n["lines"].begin(); it != n["lines"].end(); ++it) tr.lines[it.key()] = lines_of(it.value());
            if (!tr.npc || tr.stats.empty()) { err = "a trainer needs npc and stats"; return false; }
            trainers.push_back(tr);
        }
    }
    if (j.contains("tomes") && j["tomes"].is_object()) tome_min_int = j["tomes"].value("min_int", tome_min_int);
    if (j.contains("words") && j["words"].is_object())
        for (auto it = j["words"].begin(); it != j["words"].end(); ++it) words[it.key()] = lines_of(it.value());
    auto read_item = [&](const json& e, const std::string& kind) {
        GrowthItem g;
        g.id = e.value("id", std::string());
        g.kind = kind;
        g.name = e.value("name", g.id);
        g.stat = stat_from(e.value("stat", std::string(kind == "tome" ? "int" : "str")));
        g.gain = e.value("gain", kind == "tome" ? 2 : 1);
        g.map = e.value("map", 0u);
        g.x = e.value("x", 0);
        g.y = e.value("y", 0);
        g.z = e.value("z", 0);
        g.shape = uint16_t(kind == "tome" ? kTomeShape : kElixirShape);
        g.frame = uint16_t(e.value("frame", 0));
        g.where = e.value("where", std::string());
        if (e.contains("text")) g.text = lines_of(e["text"]);
        return g;
    };
    if (j.contains("tomes") && j["tomes"].is_object())
        for (const json& e : j["tomes"].value("items", json::array())) items.push_back(read_item(e, "tome"));
    if (j.contains("elixirs") && j["elixirs"].is_object())
        for (const json& e : j["elixirs"].value("items", json::array())) items.push_back(read_item(e, "elixir"));
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id.empty() || items[i].stat < 0 || !items[i].map) { err = "item " + items[i].id + ": needs id, stat and map"; return false; }
        if (i >= 256) { err = "too many items"; return false; }
        items[i].quality = uint16_t(kQualityBase + i);
    }
    for (const json& e : j.value("ordeals", json::array())) {
        GrowthOrdeal o;
        o.id = e.value("id", std::string());
        o.name = e.value("name", o.id);
        o.stat = stat_from(e.value("stat", std::string("str")));
        o.gain = e.value("gain", 2);
        o.map = e.value("map", 0u);
        o.where = e.value("where", std::string());
        o.line = e.value("line", std::string());
        const json& b = e.value("box", json::array());
        if (o.id.empty() || o.stat < 0 || !o.map || !b.is_array() || b.size() != 6) { err = "ordeal " + o.id + ": needs id, stat, map and box"; return false; }
        o.x0 = b[0]; o.y0 = b[1]; o.z0 = b[2]; o.x1 = b[3]; o.y1 = b[4]; o.z1 = b[5];
        ordeals.push_back(o);
    }
    loaded = true;
    return true;
}

bool StatGrowth::load_quests_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not valid JSON"; return false; }
    quest_stats.clear();
    main_stat.clear();
    for (const json& e : j.value("steps", json::array())) {
        QuestStat q;
        q.step = e.value("step", std::string());
        q.companions = e.value("companions", 1);
        q.avatar = e.value("avatar", 0);
        q.avatar_stat = std::max(0, stat_from(e.value("avatar_stat", std::string("str"))));
        if (q.step.empty()) { err = "a step needs an id"; return false; }
        quest_stats.push_back(q);
    }
    if (j.contains("main_stat") && j["main_stat"].is_object())
        for (auto it = j["main_stat"].begin(); it != j["main_stat"].end(); ++it)
            if (it.value().is_string() && stat_from(it.value().get<std::string>()) >= 0) main_stat[it.key()] = stat_from(it.value().get<std::string>());
    return true;
}

// ---------------------------------------------------------------------------------------------
// Queries

int StatGrowth::price(int v) const {
    if (v >= train_cap) return 0;
    const int n = std::max(0, v - price_from);
    const double p = price_base * std::pow(1.0 + price_growth_permille / 1000.0, n);
    return std::max(price_round, int(std::lround(p / price_round)) * price_round);
}

const GrowthItem* StatGrowth::item_of(const Item& it) const {
    if ((it.shape != kTomeShape && it.shape != kElixirShape) || it.quality < kQualityBase) return nullptr;
    size_t k = size_t(it.quality - kQualityBase);
    if (k >= items.size() || items[k].shape != it.shape) return nullptr;
    return &items[k];
}

const GrowthItem* StatGrowth::find_item(const std::string& id) const {
    for (const GrowthItem& g : items)
        if (g.id == id) return &g;
    return nullptr;
}

const Trainer* StatGrowth::trainer(unsigned npc) const {
    for (const Trainer& t : trainers)
        if (t.npc == npc) return &t;
    return nullptr;
}

int StatGrowth::obsidian(uint16_t actor) const {
    int n = 0;
    std::function<void(uint16_t)> walk = [&](uint16_t id) {
        const Item* it = w_.item(id);
        if (!it) return;
        if (it->shape == kMoneyShape) n += it->quality;
        for (uint16_t c : it->contents) walk(c);
    };
    if (const Item* a = w_.item(actor))
        for (uint16_t c : a->contents) walk(c);
    return n;
}

int StatGrowth::main_stat_of(uint16_t companion) const {
    const Party& p = *w_.party;
    int i = p.index_of(companion);
    if (i < 0) return kGrowStr;
    const CompanionDef& d = p.defs[size_t(i)];
    auto m = main_stat.find(d.id);
    if (m != main_stat.end()) return m->second;
    if (d.lv_class == "fighter") return kGrowStr;
    if (d.lv_class == "caster") return kGrowInt;
    if (d.lv_dex > d.lv_str && d.lv_dex >= d.lv_int) return kGrowDex;
    return d.lv_int > d.lv_str ? kGrowInt : kGrowStr;
}

uint16_t StatGrowth::holder(uint16_t item) const {
    const Item* it = w_.item(item);
    if (!it || !it->parent) return 0;
    const Item* r = w_.root_container(*it);
    return r && r->as_actor() ? r->id : 0;
}

std::vector<uint16_t> StatGrowth::trainees() const {
    std::vector<uint16_t> out{1};
    const Party& p = *w_.party;
    const Actor* av = w_.avatar();
    if (!p.enabled || !av) return out;
    for (int m : p.members) {
        const CompanionState& st = p.state[size_t(m)];
        const Actor* a = w_.actor(st.id);
        if (a && !a->is(Actor::kDead) && st.mode == CompanionState::kFollow && !st.hidden && a->mapnum == av->mapnum)
            out.push_back(st.id);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Words

std::string StatGrowth::word(const std::string& key, uint16_t who, const std::string& thing, int stat, int n) const {
    auto it = words.find(key);
    if (it == words.end() || it->second.empty()) return std::string();
    const std::vector<std::string>& v = it->second;
    std::string s = v[size_t(w_.kernel().frame_number()) % v.size()];
    replace_all(s, "{who}", who ? GameLog::name_of(w_, who, false) : std::string());
    replace_all(s, "{avatar}", w_.avatar_name);
    replace_all(s, "{thing}", thing);
    if (stat >= 0) {
        replace_all(s, "{stat}", stat_name(stat));
        replace_all(s, "{Stat}", stat_word(stat));
    }
    replace_all(s, "{n}", std::to_string(n));
    return s;
}

std::string StatGrowth::line(const Trainer& t, const std::string& moment, const std::string& who, int amount, int stat) const {
    auto it = t.lines.find(moment);
    if (it == t.lines.end() || it->second.empty()) return std::string();
    std::string s = it->second[size_t(w_.kernel().frame_number()) % it->second.size()];
    replace_all(s, "{who}", who);
    replace_all(s, "{avatar}", w_.avatar_name);
    replace_all(s, "{price}", std::to_string(amount));
    if (stat >= 0) {
        replace_all(s, "{stat}", stat_name(stat));
        replace_all(s, "{Stat}", stat_word(stat));
        replace_all(s, "{stat_word}", std::string(stat_word(stat)));
    }
    return s;
}

void StatGrowth::announce(uint16_t who, int stat, int gain, const std::string& why) {
    Actor* a = w_.actor(who);
    if (!a) return;
    char buf[64];
    std::snprintf(buf, sizeof buf, "+%d %s", gain, stat_name(stat));
    l_.float_text(who, buf);
    std::string name = GameLog::name_of(w_, who, false);
    if (!name.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    std::snprintf(buf, sizeof buf, "'s %s rises to %d", stat_name(stat), stat_value(*a, stat));
    std::string text = name + buf + (why.empty() ? std::string(".") : " (" + why + ").");
    w_.game_log.add(GameLog::kMessages, GameLog::kInfo, text, who);
    w_.sound.play_sfx(0x36, SoundSystem::kDefaultPriority, who);  // the stat-rise sound, as for a level
    PG_LOG("stat growth: %s", text.c_str());
}

// ---------------------------------------------------------------------------------------------
// Trainers

bool StatGrowth::pay(uint16_t trainee, int amount) {
    const int have = obsidian(trainee) + (trainee != 1 ? obsidian(1) : 0);
    if (amount <= 0 || have < amount) return false;
    const int own = std::min(amount, obsidian(trainee));
    if (own > 0) pay_own(trainee, own);
    if (amount > own) pay_own(1, amount - own);
    return true;
}

bool StatGrowth::pay_own(uint16_t who, int amount) {
    if (amount <= 0 || obsidian(who) < amount) return false;
    int left = amount;
    std::vector<uint16_t> stacks;
    std::function<void(uint16_t)> walk = [&](uint16_t id) {
        const Item* it = w_.item(id);
        if (!it) return;
        if (it->shape == kMoneyShape) stacks.push_back(id);
        for (uint16_t c : it->contents) walk(c);
    };
    if (const Item* a = w_.item(who))
        for (uint16_t c : a->contents) walk(c);
    for (uint16_t id : stacks) {
        if (left <= 0) break;
        Item* it = w_.item(id);
        const int t = std::min<int>(left, it->quality);
        left -= t;
        if (t >= it->quality) {
            w_.destroy_now(id);
        } else {
            it->quality = uint16_t(it->quality - t);
            w_.call_event(id, kEvCombine);  // the stack's picture follows its amount
        }
    }
    return left == 0;
}

bool StatGrowth::train(uint16_t npc, uint16_t trainee, int stat, std::string& say) {
    const Trainer* t = trainer(npc);
    Actor* a = w_.actor(trainee);
    if (!t || !a || stat < 0 || stat > 2) return false;
    std::string who = trainee == 1 ? std::string("you") : GameLog::name_of(w_, trainee, false);
    const int p = price(stat_value(*a, stat));
    if (!p) { say = line(*t, "capped", who, 0, stat); return false; }
    if (obsidian(trainee) + (trainee != 1 ? obsidian(1) : 0) < p) {
        say = line(*t, "poor", who, p, stat);
        log.push_back("poor " + std::to_string(trainee) + " " + stat_name(stat));
        return false;
    }
    pay(trainee, p);
    l_.raise(*a, stat == kGrowStr, stat == kGrowDex, stat == kGrowInt);
    ++trained[trainee][size_t(stat)];
    w_.game_log.add(GameLog::kMessages, GameLog::kInfo, "Paid " + std::to_string(p) + " obsidian.", trainee);
    announce(trainee, stat, 1, "training");
    log.push_back("trained " + std::to_string(trainee) + " " + stat_name(stat) + " " + std::to_string(p));
    say = line(*t, trainee == 1 ? "done" : "done_other", who, p, stat);
    return true;
}

DialogAction StatGrowth::dialog_action(uint16_t npc, const std::string& action) {
    DialogAction out;
    const Trainer* t = trainer(npc);
    if (!t || !l_.active()) return out;
    auto say = [&](const std::string& s) { if (!s.empty()) out.say.push_back(s); };
    auto name_of = [&](uint16_t id) { return id == 1 ? std::string("you") : GameLog::name_of(w_, id, false); };
    auto stats_menu = [&](uint16_t id) {
        const Actor* a = w_.actor(id);
        if (!a) return;
        for (int s : t->stats) {
            const int p = price(stat_value(*a, s));
            if (!p) continue;
            DialogTopic d;
            d.id = std::string("session_") + stat_name(s);
            d.label = std::string(stat_word(s)) + ", " + std::to_string(p) + " obsidian.";
            d.action = "session:" + std::to_string(id) + ":" + std::to_string(s);
            out.topics.push_back(d);
        }
        if (out.topics.empty()) say(line(*t, "capped", name_of(id)));
    };
    auto valid = [&](uint16_t id) {
        std::vector<uint16_t> ts = trainees();
        return std::find(ts.begin(), ts.end(), id) != ts.end();
    };
    if (action == "train") {
        std::vector<uint16_t> ts = trainees();
        if (ts.size() == 1) {
            say(line(*t, "intro_alone", "you"));
            stats_menu(1);
            return out;
        }
        say(line(*t, "intro"));
        for (uint16_t id : ts) {
            DialogTopic d;
            d.id = "trainee_" + std::to_string(id);
            d.label = id == 1 ? line(*t, "label_self") : line(*t, "label_other", GameLog::name_of(w_, id, false));
            if (d.label.empty()) d.label = id == 1 ? "Me." : GameLog::name_of(w_, id, false) + ".";
            d.action = "trainee:" + std::to_string(id);
            out.topics.push_back(d);
        }
        return out;
    }
    if (action.compare(0, 8, "trainee:") == 0) {
        uint16_t id = uint16_t(std::atoi(action.c_str() + 8));
        if (!valid(id)) return out;
        say(line(*t, id == 1 ? "pick" : "pick_other", name_of(id)));
        stats_menu(id);
        return out;
    }
    if (action.compare(0, 8, "session:") == 0) {
        unsigned id = 0;
        int stat = -1;
        if (std::sscanf(action.c_str() + 8, "%u:%d", &id, &stat) != 2 || !valid(uint16_t(id))) return out;
        std::string s;
        train(npc, uint16_t(id), stat, s);
        say(s);
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Tomes and elixirs

void StatGrowth::spawn_here() {
    const unsigned map = w_.map_number_now();
    for (const GrowthItem& g : items) {
        if (g.map != map || spawned.count(g.id)) continue;
        uint16_t id = w_.create_item(g.shape, g.frame);
        Item* it = w_.item(id);
        if (!it) continue;
        it->quality = g.quality;
        it->mapnum = uint8_t(map);
        w_.move_to(id, g.x, g.y, g.z, false);
        spawned.insert(g.id);
        log.push_back("spawned " + g.id);
        PG_LOG("stat growth: %s placed on map %u at (%d, %d, %d)", g.id.c_str(), map, g.x, g.y, g.z);
    }
}

bool StatGrowth::consume(uint16_t item, uint16_t who) {
    const Item* it = w_.item(item);
    const GrowthItem* g = it ? item_of(*it) : nullptr;
    Actor* a = w_.actor(who);
    if (!g || !a || a->is(Actor::kDead)) return false;
    const bool companion = who != 1;
    std::string pages;
    for (const std::string& p : g->text) pages += (pages.empty() ? "" : "*") + p;
    if (g->kind == "tome") {
        if (!l_.active()) {  // Leveling off: a book like any other, nothing to learn
            if (!companion) w_.open_reader(ReaderGump::kBook, item, 6, pages);
            return false;
        }
        if (used.count(g->id)) {
            if (companion) w_.bark(who, word("tome_spent_companion", who, g->name));
            else w_.open_reader(ReaderGump::kBook, item, 6, pages + "*" + word("tome_spent", who, g->name));
            return false;
        }
        if (a->intel < tome_min_int) {
            if (companion) w_.bark(who, word("tome_beyond_companion", who, g->name));
            else w_.open_reader(ReaderGump::kBook, item, 6, (g->text.empty() ? std::string() : g->text[0] + "*") + word("tome_beyond", who, g->name));
            log.push_back("beyond " + g->id + " " + std::to_string(who));
            return false;
        }
        l_.raise(*a, 0, 0, g->gain);
        used.insert(g->id);
        announce(who, kGrowInt, g->gain, g->name);
        log.push_back("read " + g->id + " " + std::to_string(who));
        if (companion) w_.bark(who, word("tome_read_companion", who, g->name));
        else w_.open_reader(ReaderGump::kBook, item, 6, pages + "*" + word("tome_read", who, g->name));
        return true;
    }
    // An elixir.
    if (!l_.active()) {
        w_.bark(who, word("elixir_inert", who, g->name));
        return false;
    }
    l_.raise(*a, g->stat == kGrowStr ? g->gain : 0, g->stat == kGrowDex ? g->gain : 0, 0);
    used.insert(g->id);
    announce(who, g->stat, g->gain, g->name);
    log.push_back("drank " + g->id + " " + std::to_string(who));
    std::string s = word(companion ? "elixir_companion" : "elixir", who, g->name, g->stat);
    if (!s.empty()) w_.bark(who, s);
    w_.destroy_now(item);
    return true;
}

bool StatGrowth::on_event(uint16_t item, unsigned event) {
    const Item* it = w_.item(item);
    const GrowthItem* g = it ? item_of(*it) : nullptr;
    if (!g) return false;
    if (event == kEvLook) {
        std::string name = g->name;
        if (g->kind == "tome" && used.count(g->id)) name += " (read)";
        w_.bark(item, name);
        return true;
    }
    // Use: whoever carries it reads or drinks it; on the ground, the Avatar.
    uint16_t who = holder(item);
    if (who && who != 1 && w_.party->index_of(who) < 0) return true;  // someone else's: nothing
    consume(item, who ? who : 1);
    return true;
}

void StatGrowth::on_given(uint16_t item, uint16_t companion) {
    const Item* it = w_.item(item);
    if (!it || !item_of(*it) || !l_.active() || w_.party->index_of(companion) < 0) return;
    consume(item, companion);
}

// ---------------------------------------------------------------------------------------------
// Ordeals and quest stats

void StatGrowth::check_ordeals() {
    const Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead) || (av->flags & kItemHanging) || av->parent) return;
    const unsigned map = w_.map_number_now();
    for (const GrowthOrdeal& o : ordeals) {
        if (o.map != map || ordeals_done.count(o.id)) continue;
        if (av->x < o.x0 || av->x > o.x1 || av->y < o.y0 || av->y > o.y1 || av->z < o.z0 || av->z > o.z1) continue;
        ordeals_done.insert(o.id);
        Actor* a = w_.avatar();
        l_.raise(*a, o.stat == kGrowStr ? o.gain : 0, o.stat == kGrowDex ? o.gain : 0, o.stat == kGrowInt ? o.gain : 0);
        if (!o.line.empty()) w_.game_log.add(GameLog::kMessages, GameLog::kInfo, o.line, 1);
        announce(1, o.stat, o.gain, o.name);
        log.push_back("ordeal " + o.id);
    }
}

void StatGrowth::adopt() {
    steps_adopted = true;
    CritPath* crit = this->crit();
    if (!crit) return;
    WorldCritContext ctx(w_);
    std::vector<bool> done = crit->complete(crit->done(ctx));
    for (const QuestStat& q : quest_stats) {
        int k = crit->step_index(q.step);
        if (k >= 0 && size_t(k) < done.size() && done[size_t(k)]) steps_paid.insert(q.step);
    }
}

void StatGrowth::check_quests() {
    if (quest_stats.empty()) return;
    if (!steps_adopted) { adopt(); return; }  // a save from before stage 4: what is done is paid
    CritPath* crit = this->crit();
    if (!crit) return;
    WorldCritContext ctx(w_);
    std::vector<bool> done = crit->complete(crit->done(ctx));
    for (const QuestStat& q : quest_stats) {
        int k = crit->step_index(q.step);
        if (k < 0 || size_t(k) >= done.size() || !done[size_t(k)] || !steps_paid.insert(q.step).second) continue;
        log.push_back("quest stat " + q.step);
        if (q.avatar > 0)
            if (Actor* av = w_.avatar(); av && !av->is(Actor::kDead)) {
                l_.raise(*av, q.avatar_stat == kGrowStr ? q.avatar : 0, q.avatar_stat == kGrowDex ? q.avatar : 0,
                         q.avatar_stat == kGrowInt ? q.avatar : 0);
                announce(1, q.avatar_stat, q.avatar, "the quest");
            }
        if (q.companions <= 0) continue;
        for (uint16_t id : trainees()) {
            if (id == 1) continue;
            Actor* a = w_.actor(id);
            const int s = main_stat_of(id);
            l_.raise(*a, s == kGrowStr ? q.companions : 0, s == kGrowDex ? q.companions : 0, s == kGrowInt ? q.companions : 0);
            announce(id, s, q.companions, "the quest");
            log.push_back("quest stat " + q.step + " " + std::to_string(id) + " " + stat_name(s));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Frames, state

void StatGrowth::frame() {
    if (!l_.active()) return;
    if (w_.map_number_now() != spawn_map_) {
        spawn_map_ = w_.map_number_now();
        spawn_here();
    }
    const uint32_t now = w_.kernel().frame_number();
    if (now % 10 == 0) check_ordeals();
    if (now >= next_check_) {
        next_check_ = now + 30;
        check_quests();
    }
}

void StatGrowth::reset() {
    spawned.clear();
    used.clear();
    ordeals_done.clear();
    steps_paid.clear();
    trained.clear();
    steps_adopted = false;
    spawn_map_ = 0xFFFF;
    next_check_ = 0;
    log.clear();
}

void StatGrowth::new_game() {
    reset();
    steps_adopted = true;
}

void StatGrowth::save(SaveWriter& o) const {
    o.tag("LVL4");
    o.u8(1);
    for (const std::set<std::string>* s : {&spawned, &used, &ordeals_done, &steps_paid}) {
        o.u32(uint32_t(s->size()));
        for (const std::string& x : *s) o.str(x);
    }
    o.b(steps_adopted);
    o.u32(uint32_t(trained.size()));
    for (const auto& e : trained) {
        o.u16(e.first);
        for (int n : e.second) o.i32(n);
    }
}

bool StatGrowth::load(SaveReader& in) {
    reset();
    if (!in.tag("LVL4")) return false;
    if (in.u8() != 1) return false;
    for (std::set<std::string>* s : {&spawned, &used, &ordeals_done, &steps_paid}) {
        uint32_t n = std::min<uint32_t>(in.u32(), 4096);
        for (uint32_t k = 0; k < n && !in.fail(); ++k) s->insert(in.str());
    }
    steps_adopted = in.b();
    uint32_t n = std::min<uint32_t>(in.u32(), 512);
    for (uint32_t k = 0; k < n && !in.fail(); ++k) {
        uint16_t id = uint16_t(in.u16());
        std::array<int, 3> c{};
        for (int& x : c) x = in.i32();
        trained[id] = c;
    }
    return !in.fail();
}

}  // namespace pg
