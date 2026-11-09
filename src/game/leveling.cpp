// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Levels and experience: see leveling.h and NOTES.md "Levels and XP".
#include "game/leveling.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/config.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/critpath.h"
#include "game/party.h"
#include "game/companion_magic.h"
#include "game/stat_growth.h"
#include "game/turn_combat.h"
#include "game/virtues.h"
#include "game/world.h"
#include "game/world_stages.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "json/json.hpp"

namespace pg {

using nlohmann::json;

namespace {
const int kFloatFrames = 75;  // 2.5 s
bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
}  // namespace

Leveling::Leveling(World& w)
    : growth(std::make_unique<StatGrowth>(w, *this)), magic(std::make_unique<CompanionMagic>(w, *this)), w_(w) {}
Leveling::~Leveling() = default;

// ---------------------------------------------------------------------------------------------
// The option

void Leveling::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World& w) {
        Leveling* l = w.leveling.get();
        MenuGump::OptionRow r{"Leveling:", [l] { return std::string(l && l->enabled ? "On" : "Off"); },
                              [l](int) {
                                  if (!l) return;
                                  l->set_enabled(!l->enabled);
                                  Config& c = Config::global();
                                  c.set_bool("leveling", l->enabled);
                                  if (!c.path().empty()) c.save();
                              }};
        r.group = "Game";
        return r;
    });
}

void Leveling::set_enabled(bool on) {
    enabled = on;
    if (!on)
        if (auto* g = w_.gumps.find<LevelPromptGump>()) g->close();
}

// ---------------------------------------------------------------------------------------------
// Data

bool Leveling::load_monsters_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "monster_levels.json: not valid JSON"; return false; }
    monsters.clear();
    // A variant: level, xp and (optional) stats: hp [lo, hi], dex [lo, hi], str, int, damage [lo, hi].
    auto read_variant = [](const json& v, int level, int xp) {
        MonsterVariant mv;
        mv.level = std::clamp(v.value("level", level), 1, kMaxLevel);
        mv.xp = std::max(0, v.value("xp", xp));
        auto range = [&](const char* key, int& lo, int& hi) {
            if (!v.contains(key) || !v[key].is_array() || v[key].size() != 2) return false;
            lo = std::max(0, v[key][0].get<int>());
            hi = std::max(lo, v[key][1].get<int>());
            return true;
        };
        bool hp = range("hp", mv.hp0, mv.hp1), dex = range("dex", mv.dex0, mv.dex1), dmg = range("damage", mv.dmg0, mv.dmg1);
        mv.has_stats = hp && dex && dmg;
        mv.str = std::max(0, v.value("str", 0));
        mv.intel = std::max(0, v.value("int", 0));
        if (mv.hp0 < 1) mv.hp0 = 1;
        if (mv.hp1 < mv.hp0) mv.hp1 = mv.hp0;
        return mv;
    };
    if (j.contains("monsters") && j["monsters"].is_array())
        for (const json& m : j["monsters"]) {
            MonsterLevel ml;
            ml.name = m.value("name", std::string());
            ml.level = std::clamp(m.value("level", 1), 1, kMaxLevel);
            ml.xp = std::max(0, m.value("xp", 0));
            ml.weak_prefix = m.value("weak_prefix", std::string());
            ml.variants.push_back(read_variant(m, ml.level, ml.xp));
            if (m.contains("variants") && m["variants"].is_array())
                for (const json& v : m["variants"]) {
                    MonsterVariant mv = read_variant(v, ml.level, ml.xp);
                    auto same = std::find_if(ml.variants.begin(), ml.variants.end(), [&](const MonsterVariant& o) { return o.level == mv.level; });
                    if (same != ml.variants.end()) *same = mv;
                    else ml.variants.push_back(mv);
                }
            std::sort(ml.variants.begin(), ml.variants.end(), [](const MonsterVariant& a, const MonsterVariant& b) { return a.level < b.level; });
            if (m.contains("maps") && m["maps"].is_object())
                for (auto it = m["maps"].begin(); it != m["maps"].end(); ++it)
                    ml.map_level[unsigned(std::atoi(it.key().c_str()))] = std::clamp(it.value().get<int>(), 1, kMaxLevel);
            monsters[unsigned(m.value("shape", 0))] = ml;
        }
    if (j.contains("level_difference_percent") && j["level_difference_percent"].is_array()) {
        diff_percent.clear();
        for (const json& v : j["level_difference_percent"]) diff_percent.push_back(v.get<int>());
    }
    return true;
}

bool Leveling::load_quests_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "quest_xp.json: not valid JSON"; return false; }
    quest_xp.clear();
    old_save_levels.clear();
    if (j.contains("steps") && j["steps"].is_object())
        for (auto it = j["steps"].begin(); it != j["steps"].end(); ++it) quest_xp[it.key()] = std::max(0, it.value().get<int>());
    if (j.contains("old_save_levels") && j["old_save_levels"].is_array())
        for (const json& e : j["old_save_levels"])
            old_save_levels.emplace_back(e.value("step", std::string()), std::clamp(e.value("level", 1), 1, kMaxLevel));
    return true;
}

bool Leveling::load_dir(const std::string& dir, std::string& err) {
    data_dir = dir;
    std::string text;
    if (!read_all(dir + "/monster_levels.json", text)) { err = dir + "/monster_levels.json not found"; return false; }
    if (!load_monsters_text(text, err)) return false;
    if (!read_all(dir + "/quest_xp.json", text)) { err = dir + "/quest_xp.json not found"; return false; }
    if (!load_quests_text(text, err)) return false;
    // Stage 4 (optional files: without them there are no trainers, tomes, elixirs or ordeals).
    std::string e;
    if (read_all(dir + "/stat_growth.json", text) && !growth->load_text(text, e)) PG_LOG("leveling: stat_growth.json: %s", e.c_str());
    if (read_all(dir + "/quest_stats.json", text) && !growth->load_quests_text(text, e)) PG_LOG("leveling: quest_stats.json: %s", e.c_str());
    // Stage 5 (optional: without it companion spells are never locked and count no uses).
    if (read_all(dir + "/companion_magic.json", text) && !magic->load_text(text, e)) PG_LOG("leveling: companion_magic.json: %s", e.c_str());
    return true;
}

bool Leveling::crit_load() {
    if (!crit_tried_) {
        crit_tried_ = true;
        crit_ = std::make_unique<CritPath>();
        std::string err;
        std::string p = (data_dir.empty() ? std::string("data") : data_dir) + "/critpath.json";
        if (!crit_->load_file(p, err)) PG_LOG("leveling: %s: %s", p.c_str(), err.c_str());
    }
    return crit_ && !crit_->empty();
}

// ---------------------------------------------------------------------------------------------
// Queries

int Leveling::level_for_xp(int xp) {
    int l = 1;
    while (l < kMaxLevel && xp >= kLevelXp[l]) ++l;
    return l;
}

int Leveling::level_of(uint16_t actor) const {
    auto it = progress.find(actor);
    return it == progress.end() ? 1 : it->second.level;
}

int Leveling::xp_of(uint16_t actor) const {
    auto it = progress.find(actor);
    return it == progress.end() ? 0 : it->second.xp;
}

bool Leveling::rebased(uint16_t actor) const {
    auto it = progress.find(actor);
    return started && it != progress.end() && !it->second.legacy;
}

int Leveling::kill_xp(int base_xp, int monster_level, int earner_level) const {
    if (base_xp <= 0) return 0;
    const int diff = earner_level - monster_level;
    int pct = 100;
    if (diff > 0 && !diff_percent.empty()) pct = diff_percent[std::min(size_t(diff), diff_percent.size() - 1)];
    return std::max(1, base_xp * pct / 100);
}

int Leveling::level_from_story() {
    if (!crit_load()) return 1;
    WorldCritContext ctx(w_);
    std::vector<bool> done = crit_->complete(crit_->done(ctx));
    int level = 1;
    for (const auto& e : old_save_levels) {
        int k = crit_->step_index(e.first);
        if (k >= 0 && size_t(k) < done.size() && done[size_t(k)]) level = std::max(level, e.second);
    }
    return level;
}

// One place, on purpose: a future shrine rule (a limit per actor, from the shrines visited) goes
// here and nowhere else. add_xp and on_recruit obey it.
int Leveling::level_limit(uint16_t) const { return kMaxLevel; }

int Leveling::reach_str(const Actor& a) const { return enabled ? std::max<int>(a.str, kOriginalAvatarStr) : a.str; }

// ---------------------------------------------------------------------------------------------
// New games and old games

void Leveling::reset() {
    started = false;
    pending_points = 0;
    progress.clear();
    paid_steps.clear();
    floats.clear();
    log.clear();
    trap_checked.clear();
    trap_map = 0xFFFF;
    next_quest_check_ = next_calm_check_ = 0;
    calm_cached_ = false;
    growth->reset();
    magic->reset();
}

void Leveling::new_game() {
    reset();
    if (!enabled) return;
    started = true;
    growth->new_game();
    progress[1] = LevelProgress{};
    if (Actor* av = w_.avatar()) {
        av->str = kAvatarStartStr;
        av->dex = kAvatarStartDex;
        av->intel = kAvatarStartInt;
        av->hp = uint16_t(2 * av->str);
        av->mana = int16_t(2 * av->intel);
    }
    PG_LOG("leveling: on (%zu monster shapes, %zu quest steps from %s)", monsters.size(), quest_xp.size(), data_dir.c_str());
}

// An old save (or a game played with the option off): stats stay, the level comes from the story,
// and the steps already done never pay.
void Leveling::adopt_old_game() {
    started = true;
    const int level = level_from_story();
    LevelProgress p;
    p.level = level;
    p.xp = kLevelXp[level - 1];
    p.legacy = true;
    progress[1] = p;
    Party& party = *w_.party;
    for (size_t i = 0; i < party.state.size(); ++i) {
        const CompanionState& st = party.state[i];
        if (st.mode == CompanionState::kUnspawned) continue;
        LevelProgress c;
        c.legacy = true;
        if (st.joined_before || party.in_party(int(i))) {
            c.level = level;
            c.xp = p.xp;
        }
        progress[st.id] = c;
    }
    if (crit_load()) {
        WorldCritContext ctx(w_);
        std::vector<bool> done = crit_->complete(crit_->done(ctx));
        for (size_t k = 0; k < done.size() && k < crit_->steps.size(); ++k)
            if (done[k]) paid_steps.insert(crit_->steps[k].id);
    }
    growth->adopt();
    log.push_back("adopted level " + std::to_string(level));
    PG_LOG("leveling: an older game: level %d from the story so far, stats kept", level);
}

void Leveling::on_spawned(uint16_t actor) {
    if (enabled) {
        LevelProgress p;
        progress[actor] = p;
    } else if (started) {
        LevelProgress p;
        p.legacy = true;
        progress[actor] = p;
    }
}

// ---------------------------------------------------------------------------------------------
// Gains

void Leveling::raise(Actor& a, int str, int dex, int intel) {
    Party& party = *w_.party;
    const int hp0 = party.max_hp(a), mana0 = party.max_mana(a);
    a.str = int16_t(a.str + str);
    a.dex = int16_t(a.dex + dex);
    a.intel = int16_t(a.intel + intel);
    a.hp = uint16_t(int(a.hp) + std::max(0, party.max_hp(a) - hp0));
    a.mana = int16_t(int(a.mana) + std::max(0, party.max_mana(a) - mana0));
}

void Leveling::level_up(uint16_t actor) {
    Actor* a = w_.actor(actor);
    LevelProgress& p = progress[actor];
    ++p.level;
    log.push_back(std::to_string(actor) + " level " + std::to_string(p.level));
    if (!a) return;
    if (actor == 1) {
        raise(*a, kAvatarGainStr, kAvatarGainDex, kAvatarGainInt);
        ++pending_points;
    } else {
        int i = w_.party->index_of(actor);
        if (i >= 0) {
            const CompanionDef& d = w_.party->defs[size_t(i)];
            raise(*a, d.gain_str, d.gain_dex, d.gain_int);
        }
    }
    w_.sound.play_sfx(0x36, SoundSystem::kDefaultPriority, actor);  // the original's stat-rise sound
    floats.erase(std::remove_if(floats.begin(), floats.end(), [&](const FloatText& f) { return f.actor == actor; }), floats.end());
    floats.push_back({actor, "Level up", kFloatFrames});
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s reached level %d.", GameLog::name_of(w_, actor).c_str(), p.level);
    std::string line = buf;
    if (!line.empty()) line[0] = char(std::toupper(static_cast<unsigned char>(line[0])));
    w_.game_log.add(GameLog::kMessages, GameLog::kInfo, line, actor);
    PG_LOG("leveling: %s", line.c_str());
}

void Leveling::add_xp(uint16_t actor, int xp) {
    if (!active() || xp <= 0) return;
    LevelProgress& p = progress[actor];
    p.xp += xp;
    while (p.level < std::min(kMaxLevel, level_limit(actor)) && p.xp >= kLevelXp[p.level]) level_up(actor);
}

void Leveling::award_party(int xp) {
    if (const Actor* av = w_.avatar(); av && !av->is(Actor::kDead)) add_xp(1, xp);
    const Party& party = *w_.party;
    if (!party.enabled) return;
    for (int m : std::vector<int>(party.members)) {
        const CompanionState& st = party.state[size_t(m)];
        const Actor* a = w_.actor(st.id);
        if (a && !a->is(Actor::kDead) && st.mode == CompanionState::kFollow && !st.hidden) add_xp(st.id, xp);
    }
}

void Leveling::on_kill(uint16_t victim, uint16_t attacker) {
    // Virtues: Valor from fights (game/virtues.h), Leveling on or off.
    if (w_.virtues && w_.virtues->enabled)
        if (const Actor* va = w_.actor(victim)) {
            int vl = -1, al = -1;
            if (active()) {
                auto mi = monsters.find(va->shape);
                const MonsterVariant* mv = mi == monsters.end() ? nullptr : mi->second.variant(monster_level(*va));
                if (mv) { vl = mv->level; al = level_of(1); }
            }
            w_.virtues->on_kill(victim, attacker, vl, al);
        }
    if (!active()) return;
    Actor* v = w_.actor(victim);
    const Actor* av = w_.avatar();
    if (!v || !av || (v->ext_flags & kExtXpPaid) || victim == 1 || w_.same_party(1, victim)) return;
    auto m = monsters.find(v->shape);
    if (m == monsters.end()) return;
    // The variant it fought as (its stored level, else its map's): its own level and XP.
    const MonsterVariant* mv = m->second.variant(monster_level(*v));
    if (!mv || mv->xp <= 0) return;
    // Townsfolk the Avatar has quarrelled with never pay: only those hostile to him by their side.
    if (!is_enemy(*v, *av)) return;
    bool ours = attacker == 1 || (attacker && w_.same_party(1, attacker));
    // No attacker (a blast, a trap): the party's if it happens near the Avatar.
    if (!attacker) {
        long long dx = v->x - av->x, dy = v->y - av->y;
        ours = v->mapnum == av->mapnum && dx * dx + dy * dy <= 768LL * 768LL;
    }
    if (!ours) return;
    v->ext_flags |= kExtXpPaid;  // a body that rises again pays once (the flag is saved with it)
    const MonsterLevel& ml = m->second;
    log.push_back("kill " + ml.name);
    auto pay = [&](uint16_t id) { add_xp(id, kill_xp(mv->xp, mv->level, level_of(id))); };
    if (!av->is(Actor::kDead)) pay(1);
    // The heartland's skipped hatches pay out here, on the frontier (game/world_stages.h, 5.16).
    if (w_.stages && !av->is(Actor::kDead))
        if (int b = w_.stages->frontier_bonus(kill_xp(mv->xp, mv->level, level_of(1)))) {
            log.push_back("frontier bonus " + std::to_string(b));
            award_party(b);
        }
    const Party& party = *w_.party;
    if (!party.enabled) return;
    for (int i : std::vector<int>(party.members)) {
        const CompanionState& st = party.state[size_t(i)];
        const Actor* a = w_.actor(st.id);
        if (a && !a->is(Actor::kDead) && st.mode == CompanionState::kFollow && !st.hidden) pay(st.id);
    }
}

void Leveling::on_recruit(uint16_t actor) {
    if (!active()) return;
    LevelProgress& p = progress[actor];
    const int target = std::min({std::max(1, level_of(1) - 1), kRecruitLevelCap, level_limit(actor)});
    if (p.level >= target) return;
    const int steps = target - p.level;
    p.level = target;
    p.xp = std::max(p.xp, kLevelXp[target - 1]);
    if (p.legacy) return;  // an old save's stats stand
    Actor* a = w_.actor(actor);
    int i = w_.party->index_of(actor);
    if (a && i >= 0) {
        const CompanionDef& d = w_.party->defs[size_t(i)];
        raise(*a, d.gain_str * steps, d.gain_dex * steps, d.gain_int * steps);
    }
}

bool Leveling::base_stats(uint16_t actor, int level, int& str, int& dex, int& intel) const {
    level = std::clamp(level, 1, kMaxLevel);
    const int ups = level - 1;
    if (actor == 1) {
        int pts[3] = {0, 0, 0};
        for (int k = 2; k <= level; ++k) ++pts[(k - 2) % 3];
        str = kAvatarStartStr + ups * kAvatarGainStr + pts[0];
        dex = kAvatarStartDex + ups * kAvatarGainDex + pts[1];
        intel = kAvatarStartInt + ups * kAvatarGainInt + pts[2];
        return true;
    }
    const int i = w_.party ? w_.party->index_of(actor) : -1;
    if (i < 0) return false;
    const CompanionDef& d = w_.party->defs[size_t(i)];
    str = d.lv_str + ups * d.gain_str;
    dex = d.lv_dex + ups * d.gain_dex;
    intel = d.lv_int + ups * d.gain_int;
    return true;
}

void Leveling::set_level(uint16_t actor, int level, bool rebuild, int xp) {
    level = std::clamp(level, 1, kMaxLevel);
    if (enabled) started = true;
    LevelProgress& p = progress[actor];
    const int old = p.level;
    p.level = level;
    p.xp = xp >= 0 ? std::clamp(xp, kLevelXp[level - 1], level < kMaxLevel ? kLevelXp[level] - 1 : 1 << 30) : kLevelXp[level - 1];
    Actor* a = w_.actor(actor);
    if (!a) return;
    int s = 0, d = 0, n = 0;
    if (rebuild && base_stats(actor, level, s, d, n)) {
        a->str = int16_t(s);
        a->dex = int16_t(d);
        a->intel = int16_t(n);
        p.legacy = false;
        if (actor == 1) pending_points = 0;
    } else {
        int gs = kAvatarGainStr, gd = kAvatarGainDex, gi = kAvatarGainInt;
        if (actor != 1) {
            const int i = w_.party ? w_.party->index_of(actor) : -1;
            if (i < 0) { gs = gd = gi = 0; }
            else { const CompanionDef& c = w_.party->defs[size_t(i)]; gs = c.gain_str; gd = c.gain_dex; gi = c.gain_int; }
        }
        int add[3] = {0, 0, 0};
        if (actor == 1) {
            // Points not spent yet: spread now, as the prompt would have been answered.
            for (int k = 0; k < pending_points; ++k) ++add[k % 3];
            pending_points = 0;
        }
        for (int k = old + 1; k <= level; ++k) {  // up
            add[0] += gs; add[1] += gd; add[2] += gi;
            if (actor == 1) ++add[(k - 2) % 3];
        }
        for (int k = old; k > level; --k) {  // down
            add[0] -= gs; add[1] -= gd; add[2] -= gi;
            if (actor == 1) --add[(k - 2) % 3];
        }
        a->str = int16_t(std::max(1, a->str + add[0]));
        a->dex = int16_t(std::max(1, a->dex + add[1]));
        a->intel = int16_t(std::max(1, a->intel + add[2]));
    }
    a->hp = uint16_t(w_.party->max_hp(*a));
    a->mana = int16_t(w_.party->max_mana(*a));
    if (actor == 1)
        if (auto* g = w_.gumps.find<LevelPromptGump>()) g->close();
    log.push_back("set " + std::to_string(actor) + " level " + std::to_string(level));
}

void Leveling::start_at_story(int level, int xp) {
    reset();
    if (!enabled) return;
    started = true;
    if (crit_load()) {
        WorldCritContext ctx(w_);
        std::vector<bool> done = crit_->complete(crit_->done(ctx));
        for (size_t k = 0; k < done.size() && k < crit_->steps.size(); ++k)
            if (done[k]) paid_steps.insert(crit_->steps[k].id);
    }
    growth->adopt();
    progress[1] = LevelProgress{};
    set_level(1, level, true, xp);
    PG_LOG("leveling: put at level %d (%d XP) for a point of the story; %zu steps paid", level, progress[1].xp, paid_steps.size());
}

bool Leveling::spend_point(int stat) {
    if (pending_points <= 0 || stat < 0 || stat > 2) return false;
    Actor* av = w_.avatar();
    if (!av) return false;
    raise(*av, stat == 0, stat == 1, stat == 2);
    --pending_points;
    log.push_back(std::string("point ") + (stat == 0 ? "STR" : stat == 1 ? "DEX" : "INT"));
    w_.sound.play_sfx(0x36, SoundSystem::kDefaultPriority, 1);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frames

void Leveling::check_quests() {
    if (!crit_load() || quest_xp.empty()) return;
    WorldCritContext ctx(w_);
    std::vector<bool> done = crit_->complete(crit_->done(ctx));
    for (size_t k = 0; k < done.size() && k < crit_->steps.size(); ++k) {
        if (!done[k]) continue;
        const std::string& id = crit_->steps[k].id;
        if (!paid_steps.insert(id).second) continue;
        auto q = quest_xp.find(id);
        if (q == quest_xp.end() || q->second <= 0) continue;
        log.push_back("quest " + id);
        award_party(q->second);
    }
}

bool Leveling::calm() const {
    const Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead) || w_.game_over || w_.stasis || w_.in_conversation()) return false;
    if (av->is(Actor::kInCombat)) return false;
    if (w_.turn && w_.turn->active()) return false;
    // Someone fighting the party nearby is a fight, whether or not the Avatar drew his weapon.
    std::vector<uint16_t> near;
    w_.area_search(av->x, av->y, 768, false, near);
    for (uint16_t k : near) {
        const Actor* o = w_.actor(k);
        if (o && k != 1 && !o->is(Actor::kDead) && o->is(Actor::kInCombat) && o->target &&
            (o->target == 1 || w_.same_party(1, o->target)) && !w_.same_party(1, k))
            return false;
    }
    return true;
}

void Leveling::update_prompt() {
    LevelPromptGump* g = w_.gumps.find<LevelPromptGump>();
    if (pending_points <= 0) {
        if (g) g->close();
        return;
    }
    uint32_t now = w_.kernel().frame_number();
    if (now >= next_calm_check_) {
        next_calm_check_ = now + 10;
        calm_cached_ = calm();
    }
    if (calm_cached_ && !g) w_.gumps.add(std::make_unique<LevelPromptGump>(w_));
    else if (!calm_cached_ && g) g->close();
}

void Leveling::frame() {
    if (w_.kernel().paused()) return;
    for (FloatText& f : floats) --f.frames;
    floats.erase(std::remove_if(floats.begin(), floats.end(), [](const FloatText& f) { return f.frames <= 0; }), floats.end());
    if (!enabled) return;
    if (!started) {
        // After a load (old save) or the option turned on in a game begun without it; not while
        // the opening runs, and not before the companions exist if they are about to.
        if (w_.startup_state == 0 && !w_.skip_opening) return;
        adopt_old_game();
    }
    uint32_t now = w_.kernel().frame_number();
    if (now >= next_quest_check_) {
        next_quest_check_ = now + 30;
        check_quests();
    }
    if (now % 10 == 0) check_traps();
    growth->frame();
    magic->frame();
    update_prompt();
}

// ---------------------------------------------------------------------------------------------
// Saving (savegame version 6)

void Leveling::save(SaveWriter& o) const {
    o.tag("LVL1");
    o.b(started);
    o.i32(pending_points);
    o.u32(uint32_t(progress.size()));
    for (const auto& e : progress) {
        o.u16(e.first);
        o.i32(e.second.level);
        o.i32(e.second.xp);
        o.b(e.second.legacy);
    }
    o.u32(uint32_t(paid_steps.size()));
    for (const std::string& s : paid_steps) o.str(s);
    if (started) growth->save(o);  // optional section LVL4 (stage 4)
    if (started) magic->save(o);   // optional section LVL5 (stage 5)
}

bool Leveling::load(SaveReader& in) {
    reset();
    if (!in.tag("LVL1")) return false;
    started = in.b();
    pending_points = in.i32();
    uint32_t n = std::min<uint32_t>(in.u32(), 512);
    for (uint32_t k = 0; k < n && !in.fail(); ++k) {
        uint16_t id = uint16_t(in.u16());
        LevelProgress p;
        p.level = std::clamp(in.i32(), 1, kMaxLevel);
        p.xp = in.i32();
        p.legacy = in.b();
        progress[id] = p;
    }
    n = std::min<uint32_t>(in.u32(), 4096);
    for (uint32_t k = 0; k < n && !in.fail(); ++k) paid_steps.insert(in.str());
    // Stage 4's section; a save from before it has none (its steps done so far never pay, the
    // items appear when their maps are next entered).
    if (in.peek_tag("LVL4") && !growth->load(in)) return false;
    // Stage 5's section; without it everyone has the starting stock of uses.
    if (in.peek_tag("LVL5") && !magic->load(in)) return false;
    return !in.fail();
}

// ---------------------------------------------------------------------------------------------
// The prompt

namespace {
const char* const kStatNames[3] = {"STR", "DEX", "INT"};
const int kBtnW = 50, kBtnH = 13, kBtnY = 13, kBtnX0 = 7, kBtnStep = 53;
}  // namespace

LevelPromptGump::LevelPromptGump(World& world) : Gump(world) {
    w = kW;
    h = kH;
    x = (320 - kW) / 2;
    y = 4;
    layer = kLayerNormal;
}

int LevelPromptGump::button_at(int lx, int ly) const {
    if (ly < kBtnY || ly >= kBtnY + kBtnH) return -1;
    for (int i = 0; i < 3; ++i)
        if (lx >= kBtnX0 + i * kBtnStep && lx < kBtnX0 + i * kBtnStep + kBtnW) return i;
    return -1;
}

bool LevelPromptGump::button_point(int stat, int& sx, int& sy) const {
    if (stat < 0 || stat > 2) return false;
    sx = x + kBtnX0 + stat * kBtnStep + kBtnW / 2;
    sy = y + kBtnY + kBtnH / 2;
    return true;
}

void LevelPromptGump::pick(int button, int lx, int ly) {
    if (button != 1) return;
    int b = button_at(lx, ly);
    if (b >= 0 && w_.leveling) w_.leveling->spend_point(b);
}

void LevelPromptGump::paint(Surface8& s) {
    if (!cols_) {
        cols_ = true;
        const Palette& pal = w_.data().palette;
        auto nearest = [&](int r, int g, int b) {
            int best = 0, bd = 1 << 30;
            for (int i = 16; i < 256; ++i) {
                if (w_.data().xform.loaded && w_.data().xform.is_xform(uint8_t(i))) continue;
                int dr = pal.rgb8[size_t(i)][0] - r, dg = pal.rgb8[size_t(i)][1] - g, db = pal.rgb8[size_t(i)][2] - b;
                int d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = i; }
            }
            return uint8_t(best);
        };
        bg_ = nearest(28, 24, 20);
        edge_ = nearest(150, 130, 90);
        hl_ = nearest(235, 210, 90);
    }
    s.fill_rect(x, y, w, h, edge_);
    s.fill_rect(x + 1, y + 1, w - 2, h - 2, bg_);
    const ShapeFont& f = w_.font(0);
    const int n = w_.leveling ? w_.leveling->pending_points : 0;
    std::string title = n > 1 ? "Level up! Choose " + std::to_string(n) + " points" : "Level up! Choose a point";
    if (pad_hint && !pad_focus) title += " (L3)";
    f.draw_top(s, x + (w - f.text_width(title)) / 2, y + 2, title);
    for (int i = 0; i < 3; ++i) {
        int bx = x + kBtnX0 + i * kBtnStep, by = y + kBtnY;
        s.fill_rect(bx, by, kBtnW, kBtnH, pad_focus && selected == i ? hl_ : edge_);
        s.fill_rect(bx + 1, by + 1, kBtnW - 2, kBtnH - 2, bg_);
        std::string label = std::to_string(i + 1) + ": " + kStatNames[i];
        f.draw_top(s, bx + (kBtnW - f.text_width(label)) / 2, by + 2, label);
    }
}

}  // namespace pg
