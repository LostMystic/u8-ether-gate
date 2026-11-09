// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Levels and XP (game/leveling.h, PLAN-LEVELING.md 2 and 3) through the real game mode: the option,
// starting stats, use-training off, gains, the prompt by mouse, keys and controller, kill and quest
// XP, recruits, old saves, save round trips, and the data's budget against the real monster eggs.
//   pagan_leveling_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "game/combat.h"
#include "game/critpath.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;
using nlohmann::json;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool leveling, bool companions = true, bool opening = false) : gm(d) {
        World& w = gm.world();
        w.skip_opening = !opening;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;  // all within reach for these tests
        w.party->enabled = companions;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Leveling& l() { return *gm.world().leveling; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b; e.x = gm.mouse_x(); e.y = gm.mouse_y();
        ev(e);
    }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; e.pad = 1; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    int idx(const std::string& id) { return p().find_def(id); }
    CompanionState& st(const std::string& id) { return p().state[size_t(idx(id))]; }
    Actor* a(const std::string& id) { return w().actor(st(id).id); }
    Actor* av() { return w().avatar(); }
    uint16_t monster(unsigned shape, int dx = 300) {
        uint16_t g = w().create_monster(shape, 0);
        const Actor* v = av();
        w().move_to(g, v->x + dx, v->y, v->z, false);
        return g;
    }
    // A blow that always lands and kills.
    void kill(uint16_t id, uint16_t by) { w().receive_hit(id, by, 0, 9999, kDmgPierce); }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 2);
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

// The same save as version 5 wrote it: no LVL1 section.
std::vector<uint8_t> as_v5(std::vector<uint8_t> b) {
    static const char kLvl[4] = {'L', 'V', 'L', '1'}, kEnd[4] = {'E', 'N', 'D', '.'};
    auto lvl = std::find_end(b.begin(), b.end(), kLvl, kLvl + 4);
    auto end = std::find_end(b.begin(), b.end(), kEnd, kEnd + 4);
    CHECK(lvl != b.end() && end != b.end() && lvl < end);
    b.erase(lvl, end);
    b[8] = 5; b[9] = b[10] = b[11] = 0;
    return b;
}

// Sets a story global so that a critical-path step whose done_when is a plain global is done.
bool do_step(World& w, const CritPath& cp, const std::string& id) {
    int k = cp.step_index(id);
    if (k < 0) return false;
    const CritCond* c = &cp.steps[size_t(k)].done_when;
    while (c->kind == CritCond::kAll && !c->sub.empty()) c = &c->sub[0];
    if (c->kind != CritCond::kGlobal) return false;
    w.vm().set_global(unsigned(c->a), unsigned(c->b), c->cmp == CritCond::kEq || c->cmp == CritCond::kGe ? uint16_t(c->value) : 1);
    return true;
}

void test_data(const GameData& d) {
    std::printf("the data files\n");
    Ui u(d, true);
    Leveling& l = u.l();
    CHECK(!l.monsters.empty());
    CHECK(!l.quest_xp.empty());
    // Levels by total XP (the plan's table).
    const int want[8] = {0, 100, 200, 400, 800, 1600, 3200, 6400};
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(kLevelXp[i], want[i]);
        CHECK_EQ(Leveling::level_for_xp(want[i]), i + 1);
        if (i) CHECK_EQ(Leveling::level_for_xp(want[i] - 1), i);
    }
    CHECK_EQ(Leveling::level_for_xp(99999), 8);
    // Every step named exists; quest XP is 30% of the top level.
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_data + "/critpath.json", err));
    int quest_sum = 0;
    for (const auto& q : l.quest_xp) {
        CHECK(cp.step_index(q.first) >= 0);
        quest_sum += q.second;
    }
    // Stage 3: the stage 2 quest XP (1920) with the steps from step 73 (to_fortress) on doubled (late-game XP).
    CHECK_EQ(quest_sum, 3059);
    // Old saves: every level from 2 to 8 has a step, rising along the story; 8 in the last chapter.
    int last = 1, last_k = -1;
    for (const auto& e : l.old_save_levels) {
        int k = cp.step_index(e.first);
        CHECK(k > last_k);
        CHECK_EQ(e.second, last + 1);
        last = e.second;
        last_k = k;
    }
    CHECK_EQ(last, 8);
    CHECK(last_k >= 83);  // level 8 around the last chapter (stage 3's pacing; it was "in the Ether" before)
    // The monster budget against the real eggs: about 70% of 6400, leaving room for some refighting.
    // Each egg pays the XP of its type's variant on the egg's own map (stage 3).
    long long sum = 0;
    for (unsigned m = 0; m < d.fixed.count(); ++m)
        for (const ItemRecord& r : parse_item_records(d.fixed.entry(m)))
            if (d.typeflags.get(r.shape).family() == kFamMonsterEgg) {
                auto it = l.monsters.find(r.quality & 0x7FFu);
                if (it != l.monsters.end()) sum += it->second.variant(it->second.level_on_map(m))->xp;
            }
    // The new creatures (data/creatures.json, PLAN-CREATURES.md): each place once ("alt" places are the
    // same place in another state of a map, counted once). With "New creatures" on, the default.
    json crt;
    {
        std::ifstream cf(g_data + "/creatures.json");
        crt = json::parse(cf, nullptr, false);
        CHECK(!crt.is_discarded());
    }
    long long creature_sum = 0;
    if (!crt.is_discarded())
        for (const json& c : crt["creatures"])
            for (const json& p : c["places"])
                if (!p.value("alt", false)) creature_sum += c.value("xp", 0);
    std::printf("  the new creatures pay %lld XP\n", creature_sum);
    CHECK(creature_sum > 0);
    sum += creature_sum;
    // Late-game XP was raised (stage 3) so level 8 comes near the last chapter; the monsters' share of all
    // XP stays about 70%.
    const double share = double(sum) / double(sum + quest_sum);
    std::printf("  monster eggs pay %lld XP once each, quests %d: monsters %.1f%%\n", sum, quest_sum, 100 * share);
    CHECK(sum >= 5800 && sum <= 7400);
    CHECK(share >= 0.67 && share <= 0.73);
    // Pacing along the critical path (specs/data/monster_counts.json): each chain's hatchable eggs
    // (optional maps and the scripted spawners' first hatch at half), spread over its steps, with
    // 15% refighting and the level-difference rule; quest XP as the steps are done. Level 8 should
    // come around the last chapter: steps 84 to 90 (the Ether chapter starts at 90). Old saves' levels
    // (quest_xp.json old_save_levels) follow the same pacing: each within 3 steps of where it is reached here.
    {
        std::ifstream f(g_data + "/monster_counts.json");
        json mc = json::parse(f, nullptr, false);
        CHECK(!mc.is_discarded());
        std::map<std::string, std::vector<std::pair<int, double>>> by_chain;  // (xp, level) weight
        std::map<std::string, std::vector<std::tuple<int, int, double>>> mons;  // xp, level, count
        if (!mc.is_discarded())
            for (auto it = mc["maps"].begin(); it != mc["maps"].end(); ++it) {
                const unsigned map = unsigned(std::atoi(it.key().c_str()));
                const json& mv = it.value();
                const double wgt = mv.value("optional", false) ? 0.5 : 1.0;
                auto add = [&](unsigned shape, double n) {
                    auto m = l.monsters.find(shape);
                    if (m == l.monsters.end() || n <= 0) return;
                    const MonsterVariant* v = m->second.variant(m->second.level_on_map(map));
                    if (v->xp > 0) mons[mv.value("stage", std::string())].emplace_back(v->xp, v->level, n);
                };
                for (auto e = mv["eggs"].begin(); e != mv["eggs"].end(); ++e) add(unsigned(std::atoi(e.key().c_str())), wgt * e.value().value("hatchable", 0));
                for (const json& se : mv["scripted_eggs"]) {
                    const std::string n = se.value("name", std::string());
                    const unsigned shape = n == "GHOULEGG" || n == "EGHOUL" ? 142 : n == "GHOSTEGG" ? 411 : n == "SKELEGG" ? 119 : 0;
                    if (shape) add(shape, 0.5 * wgt * se.value("count", 0));
                }
            }
        // The new creatures, in the chain of their map (or of the step they wait for).
        if (!crt.is_discarded())
            for (const json& c : crt["creatures"])
                for (const json& p : c["places"]) {
                    if (p.value("alt", false)) continue;
                    std::string stage;
                    if (p.contains("after")) {
                        int k = cp.step_index(p["after"].get<std::string>());
                        if (k >= 0) stage = cp.steps[size_t(k)].chain;
                    } else if (!mc.is_discarded()) {
                        stage = mc["maps"].value(std::to_string(p["map"].get<int>()), json::object()).value("stage", std::string());
                    }
                    CHECK(!stage.empty());
                    mons[stage].emplace_back(c.value("xp", 0), c.value("level", 1), 1.0);
                }
        double xp = 0;
        int reached[kMaxLevel + 1] = {};
        std::vector<std::string> chains;
        for (const CritStep& st : cp.steps)
            if (std::find(chains.begin(), chains.end(), st.chain) == chains.end()) chains.push_back(st.chain);
        for (const std::string& ch : chains) {
            int nsteps = 0;
            for (const CritStep& st : cp.steps) nsteps += st.chain == ch;
            for (const CritStep& st : cp.steps) {
                if (st.chain != ch) continue;
                for (const auto& m : mons[ch])
                    xp += 1.15 * std::get<2>(m) / nsteps * l.kill_xp(std::get<0>(m) * 100, std::get<1>(m), Leveling::level_for_xp(int(xp))) / 100.0;
                auto q = l.quest_xp.find(st.id);
                if (q != l.quest_xp.end()) xp += q->second;
                const int lv = Leveling::level_for_xp(int(xp));
                if (!reached[lv]) reached[lv] = cp.step_index(st.id) + 1;
            }
        }
        std::printf("  pacing (step reached, 1-based):");
        for (int lv = 2; lv <= kMaxLevel; ++lv) std::printf(" L%d %d", lv, reached[lv]);
        std::printf("; all done: %.0f XP\n", xp);
        int ether = -1;
        for (const CritStep& st : cp.steps)
            if (st.chain == "ether" && ether < 0) ether = cp.step_index(st.id) + 1;
        // The windows (stage 3): early and middle as the monsters there were tuned for, the extra XP late.
        CHECK(reached[2] >= 6 && reached[2] <= 10);
        CHECK(reached[3] >= 9 && reached[3] <= 13);
        CHECK(reached[4] >= 16 && reached[4] <= 22);
        CHECK(reached[5] >= 30 && reached[5] <= 36);
        CHECK(reached[6] >= 55 && reached[6] <= 63);
        CHECK(reached[7] >= 72 && reached[7] <= 78);
        CHECK(reached[8] >= 84 && reached[8] <= 90 && reached[8] >= ether - 6);  // around the last chapter
        for (int lv = 3; lv <= kMaxLevel; ++lv) CHECK(reached[lv] > reached[lv - 1]);
        for (const auto& e : l.old_save_levels) CHECK(std::abs(cp.step_index(e.first) + 1 - reached[e.second]) <= 3);
    }
    // Companions: rebased level 1 stats that fit, the classes of the plan.
    const std::map<std::string, std::string> classes = {{"brannoc", "fighter"}, {"morvain", "caster"}, {"kalvax", "caster"},
        {"ilvaran", "caster"}, {"wynna", "caster"}, {"nerissa", "caster"}, {"varn", "hybrid"}, {"corvin", "hybrid"}};
    for (const CompanionDef& c : u.p().defs) {
        CHECK(classes.at(c.id) == c.lv_class);
        CHECK(std::max({c.lv_str, c.lv_dex, c.lv_int}) >= 13 && std::max({c.lv_str, c.lv_dex, c.lv_int}) <= 14);
        CHECK(std::min({c.lv_str, c.lv_dex, c.lv_int}) >= 9);
        if (c.lv_class == "caster") { CHECK(c.lv_int >= 13); CHECK_EQ(c.gain_str, 0); CHECK_EQ(c.gain_dex, 1); CHECK_EQ(c.gain_int, 2); }
        if (c.lv_class == "fighter") { CHECK(c.lv_str >= 13); CHECK_EQ(c.gain_str, 2); CHECK_EQ(c.gain_dex, 1); CHECK_EQ(c.gain_int, 0); }
        if (c.lv_class == "hybrid") { CHECK_EQ(c.gain_str, 1); CHECK_EQ(c.gain_dex, 1); CHECK_EQ(c.gain_int, 1); }
        // Everyone can pay for each of their spells at level 1.
        for (const std::string& sid : c.spells)
            if (const SpellDef* s = u.p().spell(sid)) CHECK(std::max(c.lv_mana, 2 * c.lv_int) >= s->mana);
    }
}

void test_option(const GameData& d) {
    std::printf("the option\n");
    Leveling::register_options();
    Ui u(d, true);
    u.frames(5);
    MenuGump::OptionRow* found = nullptr;
    std::vector<MenuGump::OptionRow> rows;
    for (const auto& f : World::extra_option_rows()) rows.push_back(f(u.w()));
    for (auto& r : rows)
        if (r.label == "Leveling:") found = &r;
    CHECK(found != nullptr);
    if (found) {
        CHECK(found->group == "Game");
        CHECK(found->value() == "On");
        found->change(1);
        CHECK(!u.l().enabled);
        CHECK(found->value() == "Off");
        CHECK(!Config::global().get_bool("leveling", true));
        found->change(1);
        CHECK(u.l().enabled);
        CHECK(Config::global().get_bool("leveling", false));
    }
}

void test_start_stats(const GameData& d) {
    std::printf("a new game's stats\n");
    Ui off(d, false, false);
    off.frames(5);
    const int s0 = off.av()->str, d0 = off.av()->dex, i0 = off.av()->intel, h0 = off.av()->hp, m0 = off.av()->mana;
    std::printf("  option off: STR %d DEX %d INT %d HP %d mana %d\n", s0, d0, i0, h0, m0);
    CHECK(!off.l().started);
    Ui on(d, true, false);
    on.frames(5);
    CHECK(on.l().started);
    CHECK_EQ(on.av()->str, 11);
    CHECK_EQ(on.av()->dex, 10);
    CHECK_EQ(on.av()->intel, 10);
    CHECK_EQ(on.av()->hp, 22);
    CHECK_EQ(on.av()->mana, 20);
    CHECK_EQ(on.l().level_of(1), 1);
    CHECK_EQ(on.l().xp_of(1), 0);
    // The real opening does not put them back (it runs for a while with no input).
    Ui op(d, true, false, true);
    op.frames(900);
    CHECK_EQ(op.av()->str, 11);
    CHECK_EQ(op.av()->dex, 10);
    CHECK_EQ(op.av()->intel, 10);
    CHECK(op.av()->hp <= 22);    // the data's 30 HP and 30 mana are above the new maximums
    CHECK(op.av()->mana <= 20);
}

// The targeted jump at STR 11, about 400 units away: past 32 x STR, within the original's 480.
void test_jump(const GameData& d) {
    std::printf("the targeted jump's reach\n");
    auto jump = [&](bool leveling, int str) {
        Ui u(d, leveling, false);
        World& w = u.w();
        w.controls.responsive = false;
        u.frames(40);
        w.avatar()->str = int16_t(str);
        u.move(160 + 100, 114 + 50);
        u.frames(2);
        u.button(1, true);
        u.button(3, true);
        unsigned seen = 0;
        for (int f = 0; f < 60; ++f) {
            u.frames(1);
            if (f == 1) { u.button(1, false); u.button(3, false); }
            unsigned la = w.avatar()->last_anim;
            if (!seen && (la == 3 || la == 11)) seen = la;  // jump up, shake head
        }
        return seen;
    };
    CHECK_EQ(jump(false, 11), 11u);  // Off: 352, out of reach: the head shake, as always
    CHECK_EQ(jump(false, 15), 3u);   // Off at 15: the reach the story was made for
    CHECK_EQ(jump(true, 11), 3u);    // On: the same reach at 11
    Ui u(d, true, false);
    Actor a;
    a.str = 11;
    CHECK_EQ(u.l().reach_str(a), 15);
    a.str = 20;
    CHECK_EQ(u.l().reach_str(a), 20);
    u.l().enabled = false;
    a.str = 11;
    CHECK_EQ(u.l().reach_str(a), 11);
}

void test_training(const GameData& d) {
    std::printf("use-training\n");
    // Off: today's rules, the cap of 25 too.
    Ui off(d, false, false);
    off.frames(5);
    const int s0 = off.av()->str;
    off.w().stat_accumulator[0] = 649;
    off.w().train_stat(0, 10);
    CHECK_EQ(off.av()->str, s0 + 1);
    off.av()->str = 25;
    off.w().stat_accumulator[0] = 649;
    off.w().train_stat(0, 10);
    CHECK_EQ(off.av()->str, 25);
    // On: nothing rises by use, whatever the path (blows; the usecode gain intrinsics call the same).
    Ui on(d, true, false);
    on.frames(5);
    on.w().stat_accumulator = {649, 649, 649};
    for (int w = 0; w < 3; ++w) on.w().train_stat(w, 100);
    CHECK_EQ(on.av()->str, 11);
    CHECK_EQ(on.av()->intel, 10);
    CHECK_EQ(on.av()->dex, 10);
    CHECK_EQ(on.w().stat_accumulator[0], 649);
    // No cap: gains pass 25 (and 30).
    on.av()->str = 30;
    on.l().add_xp(1, 100);
    CHECK_EQ(on.av()->str, 31);
}

void test_gains(const GameData& d) {
    std::printf("level-ups and gains\n");
    Ui u(d, true);
    u.frames(40);
    Party& p = u.p();
    Leveling& l = u.l();
    Actor* av = u.av();
    // The Avatar: +1 each, HP and mana raised by what the maximum rose.
    av->hp = 10;
    av->mana = 5;
    l.add_xp(1, 99);
    CHECK_EQ(l.level_of(1), 1);
    CHECK_EQ(l.pending_points, 0);
    l.add_xp(1, 1);
    CHECK_EQ(l.level_of(1), 2);
    CHECK_EQ(av->str, 12);
    CHECK_EQ(av->dex, 11);
    CHECK_EQ(av->intel, 11);
    CHECK_EQ(av->hp, 12);
    CHECK_EQ(av->mana, 7);
    CHECK_EQ(l.pending_points, 1);
    bool fl = false;
    for (const auto& f : l.floats) fl |= f.actor == 1 && f.text == "Level up";
    CHECK(fl);
    // Several at once, none past 8; the XP stays.
    l.add_xp(1, 100000);
    CHECK_EQ(l.level_of(1), 8);
    CHECK_EQ(av->str, 18);
    CHECK_EQ(l.pending_points, 7);
    CHECK_EQ(l.xp_of(1), 100100);
    // The text goes away by itself.
    u.frames(90);
    CHECK(l.floats.empty());
    // Companions, by class.
    struct { const char* id; int s, d, i; } cases[] = {{"brannoc", 2, 1, 0}, {"morvain", 0, 1, 2}, {"varn", 1, 1, 1}, {"corvin", 1, 1, 1}};
    for (const auto& c : cases) {
        Actor* a = u.a(c.id);
        CHECK_EQ(a->str, p.defs[size_t(u.idx(c.id))].lv_str);  // spawned rebased
        const int s = a->str, dx = a->dex, in = a->intel, mh = p.max_hp(*a), mm = p.max_mana(*a), hp = a->hp, mana = a->mana;
        CHECK_EQ(hp, mh);
        l.add_xp(a->id, 100);
        CHECK_EQ(l.level_of(a->id), 2);
        CHECK_EQ(a->str, s + c.s);
        CHECK_EQ(a->dex, dx + c.d);
        CHECK_EQ(a->intel, in + c.i);
        CHECK_EQ(p.max_hp(*a), mh + 2 * c.s);
        CHECK_EQ(a->hp, hp + (p.max_hp(*a) - mh));
        CHECK_EQ(a->mana, mana + (p.max_mana(*a) - mm));
    }
    // Brannoc has no mana to raise.
    CHECK_EQ(p.max_mana(*u.a("brannoc")), 0);
    CHECK_EQ(u.a("brannoc")->mana, 0);
    CHECK_EQ(l.pending_points, 7);  // a companion's level-up asks nothing
    // The shrine hook: the one limit add_xp obeys is the top level, for now.
    CHECK_EQ(l.level_limit(1), kMaxLevel);
}

void clear_foes(Ui& u) {
    for (int t = 0; t < 4; ++t) {
        std::vector<uint16_t> near;
        u.w().area_search(u.av()->x, u.av()->y, 1200, false, near);
        for (uint16_t k : near)
            if (const Actor* o = u.w().actor(k); o && k != 1 && !o->is(Actor::kDead) && o->is(Actor::kInCombat) && !u.w().same_party(1, k))
                u.w().kill_actor(k, kDmgNormal);
        u.frames(5);
    }
}

void test_prompt(const GameData& d) {
    std::printf("the Avatar's point\n");
    Ui u(d, true, true);
    u.frames(40);
    Leveling& l = u.l();
    Actor* av = u.av();
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    l.add_xp(1, 100);
    u.frames(15);
    LevelPromptGump* g = u.w().gumps.find<LevelPromptGump>();
    CHECK(g != nullptr);
    if (!g) return;
    u.shot("level_prompt");
    // It does not stop play: not modal, not paused, and it waits.
    CHECK(!g->modal);
    CHECK(!u.w().kernel().paused());
    uint32_t f0 = u.w().kernel().frame_number();
    u.frames(600);
    CHECK(u.w().kernel().frame_number() >= f0 + 600);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    CHECK_EQ(l.pending_points, 1);
    // The controller's B does not close it (it closes windows the player opened).
    CHECK(u.w().gumps.newest_window() != g);
    // Keys: 3 is INT.
    int in = av->intel, mana = av->mana;
    u.key('3');
    CHECK_EQ(av->intel, in + 1);
    CHECK_EQ(av->mana, mana + 2);
    CHECK_EQ(l.pending_points, 0);
    u.frames(15);
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    // The mouse: the second button is DEX, the first STR.
    l.add_xp(1, 100);
    u.frames(15);
    g = u.w().gumps.find<LevelPromptGump>();
    CHECK(g != nullptr);
    if (!g) return;
    int sx, sy, dx = av->dex;
    CHECK(g->button_point(1, sx, sy));
    u.move(sx, sy);
    u.frames(2);
    u.button(1, true);
    u.button(1, false);
    u.frames(30);  // single clicks wait out the double-click time
    CHECK_EQ(av->dex, dx + 1);
    CHECK_EQ(l.pending_points, 0);
    // The controller: L3 for the focus, Right, A. And B leaves without choosing.
    l.add_xp(1, 200);
    u.frames(15);
    g = u.w().gumps.find<LevelPromptGump>();
    CHECK(g != nullptr);
    if (!g) return;
    u.pad(kPadLStick);
    CHECK(g->pad_focus);
    u.shot("level_prompt_pad");
    u.pad(kPadB);
    CHECK(!g->pad_focus);
    CHECK_EQ(l.pending_points, 1);
    u.pad(kPadLStick);
    u.pad(kPadRight);
    CHECK_EQ(g->selected, 1);
    u.pad(kPadLeft);
    CHECK_EQ(g->selected, 0);
    int st = av->str, hp = av->hp;
    u.pad(kPadA);
    CHECK_EQ(av->str, st + 1);
    CHECK_EQ(av->hp, hp + 2);
    CHECK_EQ(l.pending_points, 0);
    // Not during a fight, a conversation or a scene; it comes back afterwards.
    l.add_xp(1, 400);
    u.frames(15);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    av->set(Actor::kInCombat, true);
    u.frames(15);
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    st = av->str;
    u.key('1');  // nothing to answer: the key does nothing now
    CHECK_EQ(av->str, st);
    CHECK_EQ(l.pending_points, 1);
    av->set(Actor::kInCombat, false);
    u.frames(15);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    // A monster at him, weapon sheathed: still a fight.
    uint16_t gh = u.monster(142, 200);
    u.w().set_in_combat(gh);
    u.w().actor(gh)->target = 1;
    u.frames(15);
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    u.kill(gh, 1);
    u.frames(15);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    {
        const Actor* co = u.a("corvin");
        u.w().kernel().kill(1, kProcAny);
        u.w().teleport_actor(1, co->x + 100, co->y, co->z, co->mapnum);
        u.frames(3);
    }
    u.p().start_talk(u.idx("corvin"));
    u.frames(15);
    CHECK(u.w().in_conversation());
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    u.p().end_talk();
    for (int i = 0; i < 300 && u.w().in_conversation(); ++i) u.frames(1);
    u.frames(15);
    // (Corvin's shore has its spiders: one coming at the Avatar is a fight too. Clear them.)
    clear_foes(u);
    u.frames(15);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    u.w().stasis = true;
    u.frames(15);
    CHECK(!u.w().gumps.find<LevelPromptGump>());
    u.w().stasis = false;
    u.frames(15);
    CHECK(u.w().gumps.find<LevelPromptGump>() != nullptr);
    // Unanswered points survive a save (the prompt is offered again).
    CHECK_EQ(l.pending_points, 1);
    std::vector<uint8_t> b = save_bytes(u.w());
    Ui v(d, true, true);
    v.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(v.w().load_game(r));
    CHECK_EQ(v.l().pending_points, 1);
    clear_foes(v);
    v.frames(15);
    CHECK(v.w().gumps.find<LevelPromptGump>() != nullptr);
    // Off: no prompt, whatever the save holds.
    Ui o(d, false, true);
    o.frames(5);
    SaveReader r2(b.data(), b.size());
    CHECK(o.w().load_game(r2));
    o.frames(15);
    CHECK(!o.w().gumps.find<LevelPromptGump>());
}

void test_kills(const GameData& d) {
    std::printf("kill XP\n");
    Ui u(d, true);
    u.frames(40);
    Party& p = u.p();
    Leveling& l = u.l();
    p.join(u.idx("brannoc"));
    p.join(u.idx("morvain"));
    p.join(u.idx("varn"));
    u.frames(5);
    p.wait(u.idx("morvain"));
    p.join(u.idx("corvin"));
    u.frames(5);
    p.part_ways(u.idx("corvin"));
    // A ghoul by the Avatar: full to him and to Brannoc and Varn, who walk with him. (Its XP is the
    // data's: its variant on this map, stage 3.)
    uint16_t g = u.monster(142);
    int base = l.monsters.at(142).variant(l.monster_level(*u.w().actor(g)))->xp;
    CHECK(base > 0);
    u.kill(g, 1);
    CHECK_EQ(l.xp_of(1), base);
    CHECK_EQ(l.xp_of(u.st("brannoc").id), base);
    CHECK_EQ(l.xp_of(u.st("varn").id), base);
    // The one told to wait, the one who parted, the never-recruited: nothing.
    CHECK_EQ(l.xp_of(u.st("morvain").id), 0);
    CHECK_EQ(l.xp_of(u.st("corvin").id), 0);
    CHECK_EQ(l.xp_of(u.st("wynna").id), 0);
    // By a companion: the same.
    g = u.monster(142);
    u.kill(g, u.st("brannoc").id);
    CHECK_EQ(l.xp_of(1), 2 * base);
    CHECK_EQ(l.xp_of(u.st("varn").id), 2 * base);
    // By a monster: nobody's. No attacker: near the Avatar yes; a long way off no.
    uint16_t other = u.monster(120, 600);
    g = u.monster(142);
    u.kill(g, other);
    CHECK_EQ(l.xp_of(1), 2 * base);
    g = u.monster(142);
    u.w().receive_hit(g, 0, 0, 9999, kDmgPierce);
    CHECK_EQ(l.xp_of(1), 3 * base);
    g = u.monster(142, 1500);
    u.w().receive_hit(g, 0, 0, 9999, kDmgPierce);
    CHECK_EQ(l.xp_of(1), 3 * base);
    // A dead companion earns nothing.
    u.w().kill_actor(u.st("varn").id, kDmgNormal);
    u.frames(2);
    int vx = l.xp_of(u.st("varn").id);
    g = u.monster(142);
    u.kill(g, 1);
    CHECK_EQ(l.xp_of(u.st("varn").id), vx);
    // Peasants and guards pay nothing.
    int ax = l.xp_of(1);
    for (unsigned shape : {707u, 574u}) {
        g = u.monster(shape);
        u.kill(g, 1);
    }
    CHECK_EQ(l.xp_of(1), ax);
    // A body that rises again pays once: a real skeleton, a real rising (up to 30 s).
    uint16_t sk = u.monster(119);
    u.kill(sk, 1);
    CHECK_EQ(l.xp_of(1), ax + l.monsters.at(119).variant(l.monster_level(*u.w().actor(sk)))->xp);
    int after = l.xp_of(1);
    Actor* ska = u.w().actor(sk);
    CHECK(ska && ska->is(Actor::kDead));
    for (int i = 0; i < 1000 && ska->is(Actor::kDead); ++i) {
        u.av()->hp = uint16_t(2 * u.av()->str);
        u.frames(1);
    }
    CHECK(!ska->is(Actor::kDead));
    u.kill(sk, 1);
    CHECK_EQ(l.xp_of(1), after);
    // Far below the earner's level pays little: the rule, then in play.
    CHECK_EQ(l.kill_xp(40, 3, 3), 40);  // the same level
    CHECK_EQ(l.kill_xp(40, 5, 3), 40);  // above
    CHECK_EQ(l.kill_xp(40, 3, 4), 40);  // one below: full
    CHECK_EQ(l.kill_xp(40, 3, 5), 20);
    CHECK_EQ(l.kill_xp(40, 3, 6), 10);
    CHECK_EQ(l.kill_xp(40, 3, 7), 4);
    CHECK_EQ(l.kill_xp(40, 1, 8), 4);
    CHECK_EQ(l.kill_xp(2, 1, 8), 1);  // never nothing
    CHECK_EQ(l.kill_xp(0, 1, 1), 0);
    // Each earner by his own level.
    Ui h(d, true);
    h.frames(40);
    h.p().join(h.idx("brannoc"));
    h.frames(5);
    h.l().add_xp(1, 6400);  // the Avatar at 8, Brannoc at 1
    g = h.monster(142);
    int a0 = h.l().xp_of(1), b0 = h.l().xp_of(h.st("brannoc").id);
    h.kill(g, 1);
    const MonsterVariant* gv = h.l().monsters.at(142).variant(h.l().monster_level(*h.w().actor(g)));
    CHECK_EQ(h.l().xp_of(1), a0 + h.l().kill_xp(gv->xp, gv->level, 8));  // 10%, at least 1
    CHECK(gv->level <= 3 && h.l().kill_xp(gv->xp, gv->level, 8) == std::max(1, gv->xp / 10));
    CHECK_EQ(h.l().xp_of(h.st("brannoc").id), b0 + gv->xp);
    // Off: nobody earns.
    Ui o(d, false);
    o.frames(40);
    g = o.monster(142);
    o.kill(g, 1);
    CHECK_EQ(o.l().xp_of(1), 0);
    CHECK(!(o.w().actor(g)->ext_flags & kExtXpPaid));
}

void test_quests(const GameData& d) {
    std::printf("quest XP\n");
    Ui u(d, true);
    u.frames(40);
    Leveling& l = u.l();
    u.p().join(u.idx("brannoc"));
    u.frames(5);
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_data + "/critpath.json", err));
    // A step with a plain story global, with XP, not done yet.
    std::string step;
    for (const CritStep& s : cp.steps) {
        auto q = l.quest_xp.find(s.id);
        const CritCond* c = &s.done_when;
        while (c->kind == CritCond::kAll && !c->sub.empty()) c = &c->sub[0];
        if (q != l.quest_xp.end() && q->second > 0 && c->kind == CritCond::kGlobal && !l.paid_steps.count(s.id) && s.chain == "sorcery") { step = s.id; break; }
    }
    CHECK(!step.empty());
    std::printf("  step %s pays %d\n", step.c_str(), step.empty() ? 0 : l.quest_xp[step]);
    if (step.empty()) return;
    u.frames(35);
    const int a0 = l.xp_of(1), b0 = l.xp_of(u.st("brannoc").id), w0 = l.xp_of(u.st("wynna").id);
    const size_t paid0 = l.paid_steps.size();
    CHECK(do_step(u.w(), cp, step));
    u.frames(35);
    CHECK(l.paid_steps.count(step));
    // Earlier milestones count as done with it, and pay too; it was paid in full to both.
    const int gain = l.xp_of(1) - a0;
    CHECK(gain >= l.quest_xp[step]);
    CHECK_EQ(l.xp_of(u.st("brannoc").id) - b0, gain);
    CHECK_EQ(l.xp_of(u.st("wynna").id), w0);
    CHECK(l.paid_steps.size() > paid0);
    // Once.
    u.frames(65);
    CHECK_EQ(l.xp_of(1), a0 + gain);
    // Still once after a save and load.
    std::vector<uint8_t> b = save_bytes(u.w());
    Ui v(d, true);
    v.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(v.w().load_game(r));
    v.frames(65);
    CHECK_EQ(v.l().xp_of(1), a0 + gain);
}

void test_recruits(const GameData& d) {
    std::printf("recruits\n");
    Ui u(d, true);
    u.frames(40);
    Leveling& l = u.l();
    Party& p = u.p();
    // The Avatar at 1: they join at 1.
    p.join(u.idx("corvin"));
    CHECK_EQ(l.level_of(u.st("corvin").id), 1);
    CHECK_EQ(u.a("corvin")->str, p.defs[size_t(u.idx("corvin"))].lv_str);
    // The Avatar at 4 (400 XP): at 3, with the class's gains twice over and the XP of level 3.
    l.add_xp(1, 400);
    CHECK_EQ(l.level_of(1), 4);
    Actor* br = u.a("brannoc");
    const int hp = br->hp, s = br->str, dx = br->dex, in = br->intel;
    p.join(u.idx("brannoc"));
    CHECK_EQ(l.level_of(br->id), 3);
    CHECK_EQ(l.xp_of(br->id), 200);
    CHECK_EQ(br->str, s + 4);
    CHECK_EQ(br->dex, dx + 2);
    CHECK_EQ(br->intel, in);
    CHECK_EQ(br->hp, hp + 8);
    Actor* wy = u.a("wynna");
    const int wi = wy->intel, wm = wy->mana;
    p.join(u.idx("wynna"));
    CHECK_EQ(wy->intel, wi + 4);
    CHECK_EQ(wy->mana, wm + 8);
    // A caster's XP goes on from there: next level at 400.
    l.add_xp(wy->id, 199);
    CHECK_EQ(l.level_of(wy->id), 3);
    l.add_xp(wy->id, 1);
    CHECK_EQ(l.level_of(wy->id), 4);
    // The Avatar at 8: never above 5 on joining.
    l.add_xp(1, 6400);
    p.join(u.idx("varn"));
    CHECK_EQ(l.level_of(u.st("varn").id), 5);
    CHECK_EQ(l.xp_of(u.st("varn").id), 800);
    // Away from the party, then back: no catching up (they earned nothing while away).
    const int cl = l.level_of(u.st("corvin").id);
    const int cs = u.a("corvin")->str;
    p.part_ways(u.idx("corvin"));
    u.frames(5);
    p.join(u.idx("corvin"));
    CHECK_EQ(l.level_of(u.st("corvin").id), cl);
    CHECK_EQ(u.a("corvin")->str, cs);
}

void test_saves(const GameData& d) {
    std::printf("saves\n");
    Ui u(d, true);
    u.frames(40);
    Leveling& l = u.l();
    u.p().join(u.idx("brannoc"));
    u.frames(5);
    l.add_xp(1, 250);
    l.add_xp(u.st("brannoc").id, 120);
    u.frames(5);
    uint16_t gh = u.monster(142);
    u.kill(gh, 1);
    const int ax = l.xp_of(1);
    std::vector<uint8_t> b = save_bytes(u.w());
    CHECK_EQ(b[8], 6);
    Ui v(d, true);
    v.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(v.w().load_game(r));
    CHECK(v.l().started);
    CHECK_EQ(v.l().xp_of(1), ax);
    CHECK_EQ(v.l().level_of(1), 3);
    CHECK_EQ(v.l().pending_points, 2);
    CHECK_EQ(v.l().xp_of(v.st("brannoc").id), 120 + l.monsters.at(142).xp);
    CHECK_EQ(v.l().level_of(v.st("brannoc").id), 2);
    CHECK_EQ(v.av()->str, u.av()->str);
    CHECK_EQ(v.a("brannoc")->str, u.a("brannoc")->str);
    CHECK(v.l().paid_steps == l.paid_steps);
    CHECK(v.w().actor(gh)->ext_flags & kExtXpPaid);  // the body's flag comes back with it
    // Saved again, identically: nothing lost on the way.
    std::vector<uint8_t> b2 = save_bytes(v.w());
    auto lv = [](const std::vector<uint8_t>& x) {
        static const char t[4] = {'L', 'V', 'L', '1'};
        return std::vector<uint8_t>(std::find_end(x.begin(), x.end(), t, t + 4), x.end());
    };
    CHECK(lv(b) == lv(b2));
}

// A game with the old rules (Leveling off, as every older build played), then loaded as version 5.
void test_old_saves(const GameData& d) {
    std::printf("old saves\n");
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_data + "/critpath.json", err));
    Ui old(d, false);
    old.frames(40);
    old.p().join(old.idx("brannoc"));
    old.p().join(old.idx("corvin"));
    old.frames(5);
    old.p().part_ways(old.idx("corvin"));
    old.frames(5);
    const CompanionDef& bd = old.p().defs[size_t(old.idx("brannoc"))];
    CHECK_EQ(old.a("brannoc")->str, bd.str);  // Off: the numbers of today
    CHECK_EQ(old.a("brannoc")->dex, bd.dex);
    CHECK_EQ(old.p().max_hp(*old.a("brannoc")), bd.hp);
    old.av()->str = 19; old.av()->dex = 17; old.av()->intel = 21; old.av()->hp = 30; old.av()->mana = 33;
    // A story as far as meeting Lithos ("meet_lithos": level 4 by the table; level 5 is the step after).
    int k = cp.step_index("meet_lithos");
    CHECK(k >= 0);
    CHECK(do_step(old.w(), cp, "meet_lithos"));
    old.frames(5);
    std::vector<uint8_t> b = as_v5(save_bytes(old.w()));
    // Loaded with the option on: stats kept, level from the story, past steps never pay.
    Ui u(d, true);
    u.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(u.w().load_game(r));
    CHECK(!u.l().started);
    u.frames(3);
    CHECK(u.l().started);
    int want = 1;
    {
        WorldCritContext ctx(u.w());
        std::vector<bool> done = cp.complete(cp.done(ctx));
        CHECK(done[size_t(k)]);
        for (const auto& e : u.l().old_save_levels) {
            int s = cp.step_index(e.first);
            if (done[size_t(s)]) want = std::max(want, e.second);
        }
    }
    std::printf("  level %d from the story\n", want);
    CHECK(want >= 2);
    CHECK_EQ(u.l().level_of(1), want);
    CHECK_EQ(u.l().xp_of(1), kLevelXp[want - 1]);
    CHECK_EQ(u.av()->str, 19);
    CHECK_EQ(u.av()->dex, 17);
    CHECK_EQ(u.av()->intel, 21);
    CHECK_EQ(u.av()->hp, 30);
    CHECK_EQ(u.av()->mana, 33);
    CHECK_EQ(u.l().pending_points, 0);
    Actor* br = u.a("brannoc");
    CHECK_EQ(u.l().level_of(br->id), want);
    CHECK_EQ(br->str, bd.str);
    CHECK_EQ(u.p().max_hp(*br), bd.hp);  // their old base HP stands
    const int xp = u.l().xp_of(1);
    u.frames(65);
    CHECK_EQ(u.l().xp_of(1), xp);  // what was done before does not pay now
    CHECK(u.l().paid_steps.count("meet_lithos"));
    // Rejoining someone who parted, or a first recruit there: old stats stand, level follows.
    Actor* wy = u.a("wynna");
    const int wi = wy->intel;
    CHECK_EQ(wi, u.p().defs[size_t(u.idx("wynna"))].intel);
    u.p().join(u.idx("wynna"));
    CHECK_EQ(u.l().level_of(wy->id), std::min(want - 1, 5));
    CHECK_EQ(wy->intel, wi);
    // From there on, normal gains.
    const int s0 = u.av()->str;
    u.l().add_xp(1, kLevelXp[want] - xp);
    CHECK_EQ(u.av()->str, s0 + 1);
    CHECK_EQ(u.l().pending_points, 1);
    // And it all saves as a new save.
    std::vector<uint8_t> c = save_bytes(u.w());
    Ui u2(d, true);
    u2.frames(5);
    SaveReader r2(c.data(), c.size());
    CHECK(u2.w().load_game(r2));
    u2.frames(3);
    CHECK_EQ(u2.l().level_of(1), want + 1);
    CHECK_EQ(u2.av()->str, s0 + 1);
    // An old save with the option off: exactly as it was.
    Ui o(d, false);
    o.frames(5);
    SaveReader r3(b.data(), b.size());
    CHECK(o.w().load_game(r3));
    o.frames(40);
    CHECK(!o.l().started);
    CHECK_EQ(o.av()->str, 19);
    CHECK_EQ(o.a("brannoc")->str, bd.str);
    // A version 6 save of a game played with the option off, loaded with it on: the same adoption.
    std::vector<uint8_t> v6 = save_bytes(old.w());
    Ui a6(d, true);
    a6.frames(5);
    SaveReader r4(v6.data(), v6.size());
    CHECK(a6.w().load_game(r4));
    a6.frames(3);
    CHECK(a6.l().started);
    CHECK_EQ(a6.av()->str, 19);
    CHECK_EQ(a6.l().level_of(1), want);
    // Turned off in the middle of a leveled game: stops earning, training is back; on again: the same XP.
    Ui t(d, true);
    t.frames(40);
    t.l().add_xp(1, 150);
    t.l().set_enabled(false);
    uint16_t g = t.monster(142);
    t.kill(g, 1);
    CHECK_EQ(t.l().xp_of(1), 150);
    const int ts = t.av()->str;
    t.w().stat_accumulator[0] = 649;
    t.w().train_stat(0, 10);
    CHECK_EQ(t.av()->str, ts + 1);
    t.l().set_enabled(true);
    t.frames(3);
    CHECK_EQ(t.l().xp_of(1), 150);
    CHECK_EQ(t.l().level_of(1), 2);
}

// Stage 3 (PLAN-LEVELING.md 5 and 6): what STR, DEX and INT do, the leveled monsters, and nothing of
// it with the option off.
void test_stats(const GameData& d) {
    std::printf("stat effects and leveled monsters\n");
    // --- Off: the original's monsters and rules, untouched.
    {
        Ui o(d, false, false);
        o.frames(20);
        Leveling& l = o.l();
        uint16_t g = o.monster(83);
        Actor* t = o.w().actor(g);
        const MonsterInfo* mi = monster_info(83);
        CHECK(t && t->hp >= mi->hp0 && t->hp <= mi->hp1);
        CHECK_EQ(t->str, 0);
        CHECK_EQ(t->ext_flags & kExtLevelMask, 0);
        CHECK(l.monster_variant(*t) == nullptr);
        int lo = 999, hi = 0;
        for (int i = 0; i < 400; ++i) { int dm = attack_damage(o.w(), *t); lo = std::min(lo, dm); hi = std::max(hi, dm); }
        CHECK_EQ(lo, mi->dmg0);
        CHECK_EQ(hi, mi->dmg1);
        t->dex = 60;
        CHECK_EQ(l.dodge_permille(*t), 0);
        CHECK_EQ(l.anim_speed(*t, 7, true), 100);
        CHECK_EQ(l.move_bonus(*t), 0);
        o.av()->str = 30;
        CHECK_EQ(l.anim_speed(*o.av(), 20, false), 100);
        uint16_t hammer = o.w().create_item(540, 0);
        o.w().move_to_container(hammer, o.w().equipped(1, 7));
        o.av()->str = 5;
        CHECK(o.w().equip(1, hammer));
        CHECK_EQ(l.heavy_short(*o.av()), 0);
        CHECK_EQ(l.weapon_damage(*o.av(), 20), 20);
    }
    Ui u(d, true, false);
    u.frames(20);
    Leveling& l = u.l();
    // --- Every hostile type has a level 1-8 and XP on every map it is placed on, stats on each
    // variant but the people's; the provisional level-8 trolls of maps 7 and 8 are gone.
    for (unsigned shape : {76u, 83u, 96u, 119u, 120u, 142u, 214u, 357u, 411u, 413u, 509u, 808u, 824u}) {
        const MonsterLevel& ml = l.monsters.at(shape);
        CHECK(!ml.variants.empty());
        for (const MonsterVariant& v : ml.variants) {
            CHECK(v.level >= 1 && v.level <= 8);
            CHECK(v.xp > 0);
            CHECK(v.has_stats);
        }
    }
    const MonsterLevel& troll = l.monsters.at(83);
    CHECK(troll.level_on_map(39) <= 1);
    CHECK(troll.level_on_map(7) <= 2);
    CHECK(troll.level_on_map(8) <= 3);
    CHECK_EQ(troll.level_on_map(47), 6);  // one level per type (PLAN-CREATURES.md): the Ether's trolls are trolls (6)
    CHECK_EQ(troll.level, 6);
    // A monster made on a map gets that map's variant, keeps its level, and fights with it.
    uint16_t g = u.monster(83);
    Actor* t = u.w().actor(g);
    const int lv = troll.level_on_map(t->mapnum);
    CHECK_EQ((t->ext_flags & kExtLevelMask) >> kExtLevelShift, lv);
    const MonsterVariant* v = troll.variant(lv);
    CHECK(t->hp >= v->hp0 && t->hp <= v->hp1);
    CHECK_EQ(t->str, v->str);
    l.apply_monster(*t, troll.level);
    v = troll.variant(troll.level);
    CHECK_EQ(l.monster_level(*t), 6);
    CHECK(l.weak_prefix(*t).empty());
    CHECK(l.monster_variant(*t) == v);
    CHECK(t->hp >= v->hp0 && t->hp <= v->hp1 && t->dex >= v->dex0 && t->dex <= v->dex1);
    CHECK_EQ(t->str, 24);  // the strongest STR now (one level per type: no level-8 troll)
    int lo = 999, hi = 0;
    for (int i = 0; i < 600; ++i) { int dm = attack_damage(u.w(), *t); lo = std::min(lo, dm); hi = std::max(hi, dm); }
    CHECK_EQ(lo, v->dmg0 + v->str / 5);  // STR: the minor bonus, monsters too
    CHECK_EQ(hi, v->dmg1 + v->str / 5);
    // XP by the variant it was.
    const int x0 = l.xp_of(1);
    u.kill(g, 1);
    CHECK_EQ(l.xp_of(1) - x0, l.kill_xp(v->xp, 6, l.level_of(1)));
    // --- INT: spell damage x (1 + (INT - 10) / 20), never below x 1.
    CHECK_EQ(Leveling::int_scaled(10, 10), 10);
    CHECK_EQ(Leveling::int_scaled(10, 30), 20);
    CHECK_EQ(Leveling::int_scaled(10, 20), 15);
    CHECK_EQ(Leveling::int_scaled(10, 0), 10);
    CHECK_EQ(Leveling::int_scaled(7, 20), 11);
    // The caster of 1411:233D is its first argument; a trap's own id (an item) is no caster.
    {
        uint8_t st[16] = {};
        st[10] = 1; st[11] = 0;  // the caster word, at the bottom of the 6-byte argument block
        CHECK_EQ(Leveling::usecode_spell_caster(u.w(), 1411, 0x233D, st, 16), 1);
        CHECK_EQ(Leveling::usecode_spell_caster(u.w(), 1411, 0x1FE4, st, 16), 0);
        CHECK_EQ(Leveling::usecode_spell_caster(u.w(), 756, 0x233D, st, 16), 0);
        uint16_t trap = u.w().create_item(756, 0);
        st[10] = uint8_t(trap); st[11] = uint8_t(trap >> 8);
        CHECK_EQ(Leveling::usecode_spell_caster(u.w(), 1411, 0x233D, st, 16), 0);
        u.w().destroy_now(trap);
    }
    // --- DEX: dodge, DEX / 10 % to 30, then 1.5% a point, at most 20%.
    Actor* av = u.av();
    av->dex = 10; CHECK_EQ(l.dodge_permille(*av), 10);
    av->dex = 30; CHECK_EQ(l.dodge_permille(*av), 30);
    av->dex = 40; CHECK_EQ(l.dodge_permille(*av), 180);
    av->dex = 60; CHECK_EQ(l.dodge_permille(*av), 200);
    {
        int n = 0;
        for (int i = 0; i < 4000; ++i) n += l.roll_dodge(*av);
        CHECK(n > 650 && n < 950);  // 20%
        bool shown = false;
        for (const auto& f : l.floats) shown |= f.actor == 1 && f.text == "Dodged";
        CHECK(shown);
    }
    // A dodge in a real blow: nothing lands, and the combat log says so.
    {
        uint16_t m2 = u.monster(142, 64);
        Actor* ma = u.w().actor(m2);
        ma->dex = 200;
        av->dex = 200;  // a sure hit roll and a 20% dodge
        av->hp = 999;
        int dodged = 0;
        bool logged = false;
        for (int i = 0; i < 200; ++i) {
            const int n0 = l.dodges, hp0 = av->hp;
            u.w().receive_hit(1, m2, 0, 0, 0);
            if (l.dodges > n0) {
                ++dodged;
                CHECK_EQ(av->hp, hp0);
                logged |= u.w().game_log.lines(GameLog::kCombat).back().text.find("dodges") != std::string::npos;
            }
            if (av->hp < 100) av->hp = 999;
        }
        CHECK(logged);
        CHECK(dodged > 15 && dodged < 70);
        av->dex = 10;
    }
    // Attacks past DEX 25 play faster, at most 30%; the turn-based Move goes further with DEX.
    av->dex = 25; CHECK_EQ(l.anim_speed(*av, 7, true), 100);
    av->dex = 30; CHECK_EQ(l.anim_speed(*av, 7, true), 110);
    av->dex = 60; CHECK_EQ(l.anim_speed(*av, 7, true), 130);
    CHECK_EQ(l.anim_speed(*av, 0, false), 100);  // walking is not an attack
    av->dex = 10; CHECK_EQ(l.move_bonus(*av), 0);
    av->dex = 20; CHECK_EQ(l.move_bonus(*av), 40);
    av->dex = 60; CHECK_EQ(l.move_bonus(*av), 80);
    av->dex = 10;
    // --- STR: the Avatar's climbing plays faster (capped), not other animations or other actors.
    av->str = 10; CHECK_EQ(l.anim_speed(*av, 20, false), 100);
    av->str = 20; CHECK_EQ(l.anim_speed(*av, 20, false), 125);
    av->str = 40; CHECK_EQ(l.anim_speed(*av, 22, false), 150);
    CHECK_EQ(l.anim_speed(*av, 1, false), 100);
    // --- Heavy weapons: below the minimum STR, weaker and slower, and the player is told.
    av->str = 11;
    uint16_t hammer = u.w().create_item(540, 0);
    u.w().move_to_container(hammer, u.w().equipped(1, 7));
    CHECK(u.w().equip(1, hammer));
    CHECK_EQ(Leveling::heavy_min_str(540), 15);
    CHECK_EQ(l.heavy_short(*av), 4);
    CHECK_EQ(l.weapon_damage(*av, 20), 12);
    CHECK_EQ(l.attack_delay_extra(*av), 8);
    CHECK(l.anim_speed(*av, 7, true) < 100);
    const auto& ml = u.w().game_log.lines(GameLog::kMessages);
    CHECK(!ml.empty() && ml.back().text.find("heavy") != std::string::npos);
    av->str = 15;
    CHECK_EQ(l.heavy_short(*av), 0);
    CHECK_EQ(l.weapon_damage(*av, 20), 20);
    CHECK_EQ(l.anim_speed(*av, 7, true), 100);
    CHECK_EQ(Leveling::heavy_min_str(420), 0);  // the sword is not heavy
    // --- Chest traps: noticed and disarmed with enough DEX, never at the new game's DEX 10.
    auto chest_with_trap = [&](int dx) {
        uint16_t c = u.w().create_item(117, 0);
        u.w().move_to(c, av->x + dx, av->y, av->z, false);
        uint16_t tr = u.w().create_item(756, 0);
        if (Item* ti = u.w().item(tr)) ti->quality = 0;  // armed
        u.w().move_to_container(tr, c);
        return std::make_pair(c, tr);
    };
    av->dex = 10;
    auto c1 = chest_with_trap(96);
    u.frames(25);
    CHECK(u.w().item(c1.second) != nullptr);  // not noticed
    av->dex = 20;
    l.trap_checked.clear();
    u.frames(25);
    CHECK(u.w().item(c1.second) == nullptr);  // noticed and taken out
    const auto& ml2 = u.w().game_log.lines(GameLog::kMessages);
    CHECK(!ml2.empty() && ml2.back().text.find("trap") != std::string::npos);
    // A lock (quality high byte 1) is not a trap.
    auto c2 = chest_with_trap(-96);
    if (Item* ti = u.w().item(c2.second)) ti->quality = 0x0130;
    l.trap_checked.clear();
    u.frames(25);
    CHECK(u.w().item(c2.second) != nullptr);
    // --- Companion spells by the caster's INT, and Morvain's skeleton lasts longer with it.
    {
        Ui c(d, true, true);
        c.frames(40);
        Party& p = c.p();
        p.join(c.idx("kalvax"));
        c.frames(5);
        Actor* ka = c.a("kalvax");
        int si = p.spell_index("fire_bolt");
        CHECK(si >= 0);
        p.spells[size_t(si)].dmg0 = p.spells[size_t(si)].dmg1 = 10;
        uint16_t gh = c.monster(142, 200);
        Actor* ga = c.w().actor(gh);
        auto bolt = [&](int intel) {
            ka->intel = int16_t(intel);
            ka->mana = 100;
            ga->hp = 500;
            CHECK(p.cast(c.idx("kalvax"), si, gh));
            c.frames(40);
            return 500 - int(ga->hp);
        };
        CHECK_EQ(bolt(10), 10);
        CHECK_EQ(bolt(30), 20);
        CHECK_EQ(bolt(5), 10);
        p.join(c.idx("morvain"));
        // Stage 5: Raise Bones needs the Avatar's Summon Dead (the third Necromancer's lesson, G[203:1]).
        c.w().vm().set_global(203, 1, 1);
        c.frames(35);
        Actor* mo = c.a("morvain");
        int rs = p.spell_index("raise_bones");
        CHECK(rs >= 0);
        auto summon_len = [&](int intel) {
            for (auto& sm : p.summons) c.w().destroy_now(sm.id);
            p.summons.clear();
            mo->intel = int16_t(intel);
            mo->mana = 100;
            CHECK(p.cast(c.idx("morvain"), rs, gh));
            c.frames(3);
            return p.summons.empty() ? 0 : int(p.summons.back().until - c.w().kernel().frame_number());
        };
        const int s10 = summon_len(10), s30 = summon_len(30);
        std::printf("  skeleton lasts %d frames at INT 10, %d at INT 30\n", s10, s30);
        CHECK(s10 > 0 && std::abs(s30 - 2 * s10) <= 10);
        // Off: the same bolt ignores INT.
        c.l().set_enabled(false);
        CHECK_EQ(bolt(30), 10);
    }
}

void test_shots(const GameData& d) {
    if (g_shots.empty()) return;
    std::printf("pictures\n");
    Ui u(d, true);
    u.frames(40);
    u.l().add_xp(1, 450);
    u.frames(6);
    u.shot("level_up_float");
    u.w().call_event(1, kEvUse);
    u.frames(10);
    u.shot("level_paperdoll");
    u.gm.set_panel_width(120);
    u.frames(5);
    if (g_shots.size()) {
        Surface8 p(120, 200);
        u.gm.render_panel(p);
        std::vector<uint32_t> rgba;
        p.to_rgba(u.w().data().palette, u.gm.palette(), rgba);
        save_png(g_shots + "/level_panel.png", rgba.data(), 120, 200, 2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_leveling_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("data")) test_data(d);
    if (want("option")) test_option(d);
    if (want("start")) test_start_stats(d);
    if (want("jump")) test_jump(d);
    if (want("training")) test_training(d);
    if (want("gains")) test_gains(d);
    if (want("prompt")) test_prompt(d);
    if (want("kills")) test_kills(d);
    if (want("quests")) test_quests(d);
    if (want("recruits")) test_recruits(d);
    if (want("saves")) test_saves(d);
    if (want("oldsaves")) test_old_saves(d);
    if (want("stats")) test_stats(d);
    if (want("shots")) test_shots(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
