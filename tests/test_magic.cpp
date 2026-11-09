// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Leveling stage 5 (game/companion_magic.h, PLAN-LEVELING.md 7): companion spell unlocks, uses per
// spell for the reagent schools, the banter and talk about them, the restock at Mythran through
// his real conversation, obsidian given by drag and drop, saves, and Leveling off unchanged.
//   pagan_magic_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
//   ONLY: data, unlock, uses, ui, auto, banter, talk, mythran, drop, essential, saves, off
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/companion_magic.h"
#include "game/game_log.h"
#include "game/gumps.h"
#include "game/leveling.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/stat_growth.h"
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
    Ui(const GameData& d, bool leveling) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;
        w.party->enabled = true;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        if (!w.dialog->load_dir(g_data + "/dialog", err)) std::printf("  dialog: %s\n", err.c_str());
        w.dialog->enabled = false;
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Leveling& l() { return *gm.world().leveling; }
    CompanionMagic& m() { return *gm.world().leveling->magic; }
    StatGrowth& g() { return *gm.world().leveling->growth; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ev(InputEvent e) { gm.handle(e); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    int idx(const std::string& id) { return p().find_def(id); }
    CompanionState& st(const std::string& id) { return p().state[size_t(idx(id))]; }
    Actor* a(const std::string& id) { return w().actor(st(id).id); }
    Actor* av() { return w().avatar(); }
    int si(const std::string& spell) { return p().spell_index(spell); }
    void set(unsigned pos, unsigned n, unsigned v) { w().vm().set_global(pos, n, uint16_t(v)); }
    void level(const std::string& id, int lv) { l().progress[st(id).id].level = lv; }
    std::vector<std::string> menu() {
        std::vector<std::string> out;
        if (w().ask_menu) for (uint16_t sid : w().ask_menu->answers) out.push_back(w().vm().string(sid));
        return out;
    }
    std::string joined() {
        std::string o;
        for (const std::string& s : menu()) o += (o.empty() ? "" : " | ") + s;
        return o;
    }
    bool wait_menu(int settle = 6, int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (w().ask_menu && w().ask_menu->frames_open >= settle) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
    bool wait_idle(int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (!w().ask_menu && !w().in_conversation()) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
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

uint16_t give(World& w, uint16_t actor, unsigned shape, unsigned frame, uint16_t quality) {
    uint16_t id = w.create_item(shape, frame);
    w.item(id)->quality = quality;
    uint16_t pack = w.equipped(actor, 7);
    w.move_to_container(id, pack ? pack : actor);
    return id;
}

int money(Ui& u, uint16_t actor) { return u.g().obsidian(actor); }

bool has_log(const std::vector<std::string>& log, const std::string& s) {
    return std::find(log.begin(), log.end(), s) != log.end();
}
int count_log(const std::vector<std::string>& log, const std::string& s) { return int(std::count(log.begin(), log.end(), s)); }

bool game_log_has(World& w, const std::string& part) {
    for (const auto& e : w.game_log.lines(GameLog::kMessages))
        if (e.text.find(part) != std::string::npos) return true;
    return false;
}

// A hostile monster near the Avatar (a ghoul, shape 142), for casting at.
uint16_t foe(Ui& u, int r0 = 160, int r1 = 400) {
    World& w = u.w();
    const Actor* av = u.av();
    uint16_t m = w.create_monster(142, 0);
    if (!m) return 0;
    for (int r = r0; r <= r1; r += 16)
        for (int k = 0; k < 16; ++k) {
            double ang = 6.2831853 * k / 16;
            int px = av->x + int(r * std::cos(ang)), py = av->y + int(r * std::sin(ang));
            for (int dz : {0, 8, -8, 16}) {
                World::Placement pl = w.can_exist_shape(142, px, py, av->z + dz, m);
                if (pl.valid && pl.supported && pl.land) { w.move_to(m, px, py, av->z + dz, false); return m; }
            }
        }
    return m;
}

void test_data(const GameData& d) {
    std::printf("data: the table covers every companion spell; the prices\n");
    Ui u(d, true);
    CompanionMagic& m = u.m();
    CHECK(m.loaded);
    // Every spell of every companion has a rule; only the reagent schools count uses.
    for (const CompanionDef& c : u.p().defs) {
        for (const std::string& s : c.spells) {
            const MagicSpellRule* r = m.rule(c.id, s);
            CHECK(r != nullptr);
            if (!r) std::printf("  no rule for %s.%s\n", c.id.c_str(), s.c_str());
        }
        const MagicCompanion* mc = m.companion(c.id);
        const bool counts = c.id == "morvain" || c.id == "kalvax" || c.id == "wynna";
        CHECK(!c.spells.empty() == (mc != nullptr));
        if (mc) CHECK_EQ(mc->counts_uses(), counts);
    }
    // The prices: reagents x Mythran's price / doses, rounded.
    std::map<std::string, int> want{{"grave_drain", 2}, {"raise_bones", 3}, {"fire_bolt", 5},
                                    {"fire_field", 6}, {"lightning", 15}, {"gust", 15}};
    for (const auto& c : m.companions)
        for (const MagicSpellRule& r : c.second.spells) {
            if (r.stock > 0) std::printf("  %s %s: stock %d, %d obsidian a use (%s, %d reagents)\n", c.first.c_str(), r.spell.c_str(),
                                         r.stock, r.price, r.model.c_str(), r.reagents);
            if (want.count(r.spell)) CHECK_EQ(r.price, want[r.spell]);
        }
    CHECK_EQ(CompanionMagic::price_of(3, 15, 10), 5);  // 4.5 rounds up
    CHECK_EQ(CompanionMagic::price_of(1, 1, 10), 1);   // never free
    // The essential spells never lock.
    CHECK(m.rule("ilvaran", "raise") && m.rule("ilvaran", "raise")->always);
    CHECK(m.rule("nerissa", "tide_rite") && m.rule("nerissa", "tide_rite")->always);
    // The full restock of each reagent user.
    for (const char* id : {"morvain", "kalvax", "wynna"}) {
        int full = 0;
        for (const MagicSpellRule& r : m.companion(id)->spells) full += r.stock * r.price;
        std::printf("  %s: a full stock from empty costs %d obsidian\n", id, full);
        CHECK(full > 0 && full < 700);
    }
}

void test_unlock(const GameData& d) {
    std::printf("unlock: damaging and Tempest spells by level, the rest by the Avatar's learning (sticky)\n");
    Ui u(d, true);
    u.frames(40);
    for (const char* c : {"kalvax", "ilvaran", "morvain", "varn", "nerissa"}) u.p().join(u.idx(c));
    u.frames(5);
    CompanionMagic& m = u.m();
    const int k = u.idx("kalvax"), il = u.idx("ilvaran"), mo = u.idx("morvain"), va = u.idx("varn");
    CHECK_EQ(u.l().level_of(u.st("kalvax").id), 1);
    CHECK(m.gate(k, "fire_bolt").ok());
    CompanionMagic::Gate g = m.gate(k, "fire_field");
    CHECK(g.locked && g.why == "level 3");
    u.level("kalvax", 3);
    CHECK(m.gate(k, "fire_field").ok());
    // Tempest: level-based.
    CHECK(m.gate(va, "wave").ok() && m.gate(va, "healing_waters").locked);
    u.level("varn", 3);
    CHECK(m.gate(va, "healing_waters").ok());
    // Heals wait for the Avatar's Healing Touch or Restoration focus.
    g = m.gate(il, "heal");
    std::printf("  Ilvaran's Heal: %s\n", g.why.c_str());
    CHECK(g.locked && g.why == "needs Healing Touch or Restoration");
    CHECK(m.gate(il, "cure").locked);
    CHECK(m.gate(il, "ancient_light").ok());
    CHECK(m.gate(il, "raise").ok());  // essential
    CHECK(!u.p().spell_ready(il, u.si("heal")));
    CHECK(!u.p().cast(il, u.si("heal"), 1));  // refused, no mana spent
    uint16_t focus = give(u.w(), 1, 396, 9, 0);  // the Healing Touch focus
    u.frames(32);
    CHECK(u.m().learned.count("healing_touch"));
    CHECK(m.gate(il, "heal").ok() && m.gate(il, "cure").ok());
    u.w().destroy_now(focus);
    u.frames(32);
    CHECK(m.gate(il, "heal").ok());  // learned stays learned
    // Summon Dead: the third Ancient Necromancer's lesson (G[203:1]).
    CHECK(m.gate(mo, "raise_bones").locked && m.gate(mo, "raise_bones").why == "needs Summon Dead");
    u.set(203, 1, 1);
    u.frames(32);
    CHECK(m.gate(mo, "raise_bones").ok());
    // Or a Summon Dead token carried (a fresh game).
    Ui v(d, true);
    v.frames(40);
    v.p().join(v.idx("morvain"));
    give(v.w(), 1, 746, 3, 0);
    v.frames(32);
    CHECK(v.m().gate(v.idx("morvain"), "raise_bones").ok());
    // An item of another frame does not count.
    CHECK(!v.m().learned.count("restoration"));
}

void test_uses(const GameData& d) {
    std::printf("uses: a cast takes a use and the mana; none left, no cast; mana-only schools count nothing\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.p().join(u.idx("ilvaran"));
    u.frames(5);
    CompanionMagic& m = u.m();
    const int k = u.idx("kalvax");
    Actor* ka = u.a("kalvax");
    uint16_t f = foe(u);
    CHECK(f != 0);
    const int stock = m.rule("kalvax", "fire_bolt")->stock;
    CHECK_EQ(m.left("kalvax", "fire_bolt"), stock);
    ka->mana = 40;
    CHECK(u.p().cast(k, u.si("fire_bolt"), f));
    CHECK_EQ(m.left("kalvax", "fire_bolt"), stock - 1);
    CHECK_EQ(int(ka->mana), 40 - u.p().spells[size_t(u.si("fire_bolt"))].mana);
    u.frames(40);
    // No uses: refused, mana kept.
    m.uses["kalvax"]["fire_bolt"] = 0;
    const int mana = ka->mana;
    CHECK(!u.p().cast(k, u.si("fire_bolt"), f));
    CHECK_EQ(int(ka->mana), mana);
    CHECK(m.gate(k, "fire_bolt").empty && m.gate(k, "fire_bolt").why == "out of reagents");
    CHECK(m.reagent_state(k) == "out");
    // Not enough mana: refused, no use spent.
    m.uses["kalvax"]["fire_bolt"] = 5;
    ka->mana = 1;
    CHECK(!u.p().cast(k, u.si("fire_bolt"), f));
    CHECK_EQ(m.left("kalvax", "fire_bolt"), 5);
    CHECK(m.reagent_state(k) == "low");
    // Ilvaran: mana only.
    Actor* ia = u.a("ilvaran");
    ia->mana = 30;
    CHECK(u.p().cast(u.idx("ilvaran"), u.si("ancient_light"), f));
    CHECK_EQ(m.left("ilvaran", "ancient_light"), -1);
    CHECK(m.uses.count("ilvaran") == 0);
    CHECK(m.reagent_state(u.idx("ilvaran")).empty());
    // Out of uses in a real-time fight: Kalvax closes in and fights by hand instead of hanging back.
    m.uses["kalvax"]["fire_bolt"] = 0;
    ka->mana = 40;
    u.p().state[size_t(k)].target = f;
    const uint32_t start = u.w().kernel().frame_number();
    int casts = 0;
    for (int i = 0; i < 300; ++i) {
        u.frames(1);
        if (u.w().kernel().frame_number() < start) break;
    }
    for (const std::string& s : u.p().log) casts += s == "kalvax casts fire_bolt";
    CHECK_EQ(casts, 1);  // only the one above
}

void test_ui(const GameData& d) {
    std::printf("ui: the turn-based list shows uses and greys locked spells with the reason; the HUD tooltip\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.p().join(u.idx("ilvaran"));
    u.frames(5);
    TurnCombat& t = *u.w().turn;
    std::vector<TurnCombat::CastOption> o = t.cast_options(u.st("kalvax").id);
    CHECK_EQ(o.size(), size_t(2));
    if (o.size() == 2) {
        CHECK(!o[0].locked && o[0].uses == u.m().rule("kalvax", "fire_bolt")->stock);
        CHECK(o[1].locked && o[1].why == "level 3");
    }
    std::vector<TurnCombat::CastOption> io = t.cast_options(u.st("ilvaran").id);
    bool heal_locked = false;
    for (const auto& x : io) if (x.name == "Heal") heal_locked = x.locked && x.why.find("Healing Touch") != std::string::npos;
    CHECK(heal_locked);
    std::string s = u.m().summary(u.idx("kalvax"));
    std::printf("  Kalvax: %s\n", s.c_str());
    CHECK(s.find("Fire Bolt 30/30") != std::string::npos && s.find("Fire Field (locked: level 3)") != std::string::npos);
    std::printf("  Ilvaran: %s\n", u.m().summary(u.idx("ilvaran")).c_str());
    // A real turn-based fight: Kalvax's spell list, drawn (shot), and a locked pick refused with the reason.
    uint16_t f = foe(u, 200, 420);
    u.w().turn->enabled = true;
    u.key('c');
    for (int i = 0; i < 600 && !(t.active() && t.awaiting_player() && t.current() == u.st("kalvax").id); ++i) {
        if (t.active() && t.awaiting_player() && t.current() != u.st("kalvax").id) t.cmd_end_turn();
        u.frames(1);
    }
    std::printf("  turn-based: active %d, current %u (Kalvax %u), foe %u\n", t.active(), t.current(), u.st("kalvax").id, f);
    if (t.active() && t.current() == u.st("kalvax").id) {
        CHECK(!t.cmd_cast(o[1], f));
        std::printf("  status: %s\n", t.status.c_str());
        CHECK(t.status.find("level 3") != std::string::npos);
        u.gm.turn_ui().press_entry(TurnUI::kECast);
        u.frames(2);
        u.shot("magic_turn_list");
    }
}

void test_auto(const GameData& d) {
    std::printf("auto: the last uses are not spent on a trivial target\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.frames(5);
    CompanionMagic& m = u.m();
    const int k = u.idx("kalvax");
    uint16_t f = foe(u);
    Actor* fa = u.w().actor(f);
    CHECK(fa != nullptr);
    if (!fa) return;
    fa->hp = 40;
    CHECK(m.worth_casting(k, "fire_bolt", f));  // full stock: anything
    m.uses["kalvax"]["fire_bolt"] = 3;          // low
    CHECK(m.worth_casting(k, "fire_bolt", f));  // a healthy foe: yes
    fa->hp = 2;
    CHECK(!m.worth_casting(k, "fire_bolt", f)); // nearly dead: a blow will do
    CHECK(!u.p().spell_ready(k, u.si("fire_bolt"), nullptr, f));
    CHECK(u.p().spell_ready(k, u.si("fire_bolt")));  // the player may still cast it
    fa->hp = 40;
    u.level("kalvax", 6);
    const int ml = u.l().monster_level(*fa);
    std::printf("  ghoul level %d against Kalvax level 6\n", ml);
    if (ml > 0 && ml <= 4) CHECK(!m.worth_casting(k, "fire_bolt", f));
    // Turn-based Auto picks nothing then (it falls back to the staff).
    m.uses["kalvax"]["fire_bolt"] = 30;
    CHECK(m.worth_casting(k, "fire_bolt", f));
}

void test_banter(const GameData& d) {
    std::printf("banter: once when low, once when out, again after a restock\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("wynna"));
    u.frames(30);
    CompanionMagic& m = u.m();
    auto said = [&](const std::string& t) { return count_log(u.p().log, "wynna bark " + t); };
    u.frames(60);
    CHECK_EQ(said("reagents_low") + said("reagents_out"), 0);
    m.uses["wynna"]["lightning"] = 3;
    u.frames(60);
    CHECK_EQ(said("reagents_low"), 1);
    for (Bark& b : u.w().barks) b.ticks_left = 1;
    u.frames(400);
    CHECK_EQ(said("reagents_low"), 1);  // no spam
    m.uses["wynna"]["lightning"] = 0;
    u.frames(60);
    CHECK_EQ(said("reagents_out"), 1);
    for (Bark& b : u.w().barks) b.ticks_left = 1;
    u.frames(400);
    CHECK_EQ(said("reagents_out"), 1);
    // A restock (with money) clears it: the next low comes again.
    give(u.w(), u.st("wynna").id, StatGrowth::kMoneyShape, 0, 2000);
    m.restock();
    CHECK_EQ(m.left("wynna", "lightning"), m.rule("wynna", "lightning")->stock);
    CHECK(!m.said_low.count("wynna") && !m.said_out.count("wynna"));
    for (Bark& b : u.w().barks) b.ticks_left = 1;
    u.frames(5);
    m.uses["wynna"]["lightning"] = 2;  // (Gust is still locked at her level 1: it does not count)
    u.frames(90);
    if (said("reagents_low") != 2) {
        for (const std::string& s : m.log) std::printf("  log: %s\n", s.c_str());
        std::printf("  state %s, calm %d, barks %zu, said_low %d\n", m.reagent_state(u.idx("wynna")).c_str(), u.l().calm(), u.w().barks.size(), int(m.said_low.count("wynna")));
        for (const Bark& b : u.w().barks) std::printf("  bark by %u\n", b.speaker);
    }
    CHECK_EQ(said("reagents_low"), 2);
}

void test_talk(const GameData& d) {
    std::printf("talk: their own topic explains it; the out-of-spells question when out\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.frames(5);
    auto labels = [&]() {
        std::vector<std::string> out;
        for (const PartyTopic* t : u.p().visible_topics(u.idx("kalvax"))) out.push_back(t->label);
        return out;
    };
    auto has = [&](const std::string& l) { auto v = labels(); return std::find(v.begin(), v.end(), l) != v.end(); };
    u.p().start_talk(u.idx("kalvax"));
    u.frames(2);
    CHECK(has("your reagents"));
    CHECK(!has("What do you mean you're out of spells?"));
    u.p().end_talk();
    u.wait_idle(300);
    u.m().uses["kalvax"]["fire_bolt"] = 0;
    u.p().start_talk(u.idx("kalvax"));
    u.frames(2);
    CHECK(has("What do you mean you're out of spells?"));
    CHECK(!has("your reagents"));
    // Picked through the real answer menu: his lines.
    int pick = -1;
    u.wait_menu();
    std::vector<std::string> mm = u.menu();
    std::printf("  Kalvax's menu: %s\n", u.joined().c_str());
    for (size_t i = 0; i < mm.size(); ++i) if (mm[i].find("out of spells") != std::string::npos) pick = int(i);
    CHECK(pick >= 0);
    if (pick >= 0) {
        u.w().choose_answer(unsigned(pick));
        bool heard = false;
        for (int f = 0; f < 600 && !heard; ++f) {
            for (const Bark& b : u.w().barks)
                for (const TextPage& pg : b.pages)
                    for (const std::string& l : pg.lines) heard = heard || l.find("buy at the same time") != std::string::npos;
            u.frames(1);
        }
        CHECK(heard);
    }
    u.p().end_talk();
    u.wait_idle(300);
    // Ilvaran (mana only) has no such topic; Leveling off: nobody has.
    u.p().join(u.idx("ilvaran"));
    bool ilv = false;
    for (const PartyTopic* t : u.p().visible_topics(u.idx("ilvaran"))) ilv = ilv || t->id.rfind("reagents", 0) == 0;
    CHECK(!ilv);
}

// Through Mythran's real conversation: the farewell answer ends it.
bool talk_mythran_and_leave(Ui& u) {
    const Actor* g = u.w().actor(33);
    if (!g) return false;
    u.w().teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
    u.frames(30);
    std::printf("  Mythran on map %u at %d,%d,%d (the Avatar on %u)\n", g->mapnum, g->x, g->y, g->z, u.w().map_number_now());
    u.w().call_event(33, kEvUse);
    if (!u.wait_menu()) return false;
    for (int round = 0; round < 6; ++round) {
        std::printf("  his menu: %s\n", u.joined().c_str());
        std::vector<std::string> m = u.menu();
        int bye = -1;
        for (size_t i = 0; i < m.size(); ++i)
            if (m[i].find("arewell") != std::string::npos || m[i].find("oodbye") != std::string::npos || m[i].find("Bye") != std::string::npos)
                bye = int(i);
        if (bye < 0) return false;
        u.w().choose_answer(unsigned(bye));
        for (int f = 0; f < 3000; ++f) {
            if (!u.w().in_conversation()) return true;
            if (u.w().ask_menu && u.w().ask_menu->frames_open >= 6) break;
            for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            u.frames(1);
        }
    }
    return !u.w().in_conversation();
}

void test_mythran(const GameData& d) {
    std::printf("mythran: when his conversation ends, the reagent users in the party on his map buy uses with their own obsidian\n");
    Ui u(d, true);
    u.frames(40);
    u.set(89, 1, 1);
    u.set(284, 1, 1);  // met him before: his later-visits menu
    for (const char* c : {"kalvax", "wynna", "morvain", "brannoc"}) u.p().join(u.idx(c));
    u.frames(5);
    CompanionMagic& m = u.m();
    m.uses["kalvax"]["fire_bolt"] = 10;   // 20 short at 5
    m.uses["kalvax"]["fire_field"] = 0;   // 12 short at 6
    m.uses["wynna"]["lightning"] = 0;
    m.uses["morvain"]["grave_drain"] = 0;  // 30 short at 2
    m.uses["morvain"]["raise_bones"] = 2;  // 6 short at 3
    // Whatever they start with in their packs counts too.
    const int k0 = money(u, u.st("kalvax").id), m0 = money(u, u.st("morvain").id), w0 = money(u, u.st("wynna").id);
    std::printf("  their own obsidian at the start: Kalvax %d, Morvain %d, Wynna %d\n", k0, m0, w0);
    CHECK(w0 < 15);
    give(u.w(), u.st("kalvax").id, StatGrowth::kMoneyShape, 0, uint16_t(60 - k0));
    give(u.w(), u.st("morvain").id, StatGrowth::kMoneyShape, 0, uint16_t(500 - m0));
    give(u.w(), 1, StatGrowth::kMoneyShape, 0, 900);  // the Avatar's is not theirs
    CHECK(talk_mythran_and_leave(u));
    u.frames(5);
    for (const std::string& s : m.log) std::printf("  log: %s\n", s.c_str());
    CHECK(has_log(m.log, "mythran talk ended"));
    // Morvain: everything, 30 x 2 + 6 x 3 = 78.
    CHECK_EQ(m.left("morvain", "grave_drain"), 30);
    CHECK_EQ(m.left("morvain", "raise_bones"), 8);
    CHECK_EQ(money(u, u.st("morvain").id), 500 - 78);
    // Kalvax: 60 goes, the emptiest first: Fire Field before Fire Bolt.
    const int kb = m.left("kalvax", "fire_bolt") - 10, kf = m.left("kalvax", "fire_field");
    std::printf("  Kalvax bought Fire Bolt %d, Fire Field %d; %d obsidian left\n", kb, kf, money(u, u.st("kalvax").id));
    CHECK_EQ(kb * 5 + kf * 6 + money(u, u.st("kalvax").id), 60);
    CHECK(money(u, u.st("kalvax").id) < 5);
    CHECK(kf >= 4 && kb >= 4);
    // Wynna has nothing: nothing bought, told so.
    CHECK_EQ(m.left("wynna", "lightning"), 0);
    CHECK(has_log(m.log, "wynna poor"));
    CHECK_EQ(money(u, 1), 900);
    CHECK(game_log_has(u.w(), "Morvain buys reagents: Grave Drain 30 uses (60 obsidian), Raise Bones 6 uses (18 obsidian)."));
    CHECK(game_log_has(u.w(), "Kalvax buys reagents: Fire Bolt"));
    CHECK(game_log_has(u.w(), "Wynna has no obsidian for reagents."));
    bool barked = false;
    for (const std::string& s : u.p().log) barked = barked || s.rfind("morvain bark restock", 0) == 0 || s.rfind("kalvax bark restock", 0) == 0;
    CHECK(barked);
    // Someone waiting on another map buys nothing; the first meeting counts too.
    Ui v(d, true);
    v.frames(40);
    v.set(89, 1, 1);
    v.p().join(v.idx("kalvax"));
    v.frames(5);
    v.p().wait(v.idx("kalvax"));
    v.m().uses["kalvax"]["fire_bolt"] = 0;
    give(v.w(), v.st("kalvax").id, StatGrowth::kMoneyShape, 0, 500);
    const int kv = money(v, v.st("kalvax").id);
    CHECK(talk_mythran_and_leave(v));
    v.frames(5);
    CHECK(has_log(v.m().log, "mythran talk ended"));
    CHECK_EQ(v.m().left("kalvax", "fire_bolt"), 0);
    CHECK_EQ(money(v, v.st("kalvax").id), kv);
}

void test_drop(const GameData& d) {
    std::printf("drop: obsidian dropped on a reagent user: the split offers what they need; it joins their pile\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.frames(30);
    CompanionMagic& m = u.m();
    Actor* k = u.a("kalvax");
    Actor* av = u.av();
    u.w().move_to(k->id, av->x + 96, av->y, av->z, false);
    u.p().state[size_t(u.idx("kalvax"))].mode = CompanionState::kWait;
    m.uses["kalvax"]["fire_bolt"] = 20;  // 10 short: 50
    give(u.w(), k->id, StatGrowth::kMoneyShape, 0, 20);
    const int have = money(u, k->id);  // with what he starts with
    const int need = m.restock_cost(u.idx("kalvax")) - have;
    CHECK_EQ(m.restock_cost(u.idx("kalvax")), 50);
    CHECK(need > 0);
    auto piles_of = [&]() {
        int n = 0;
        if (const Item* p = u.w().item(u.w().equipped(k->id, 7)))
            for (uint16_t c : p->contents) if (const Item* it = u.w().item(c); it && it->shape == StatGrowth::kMoneyShape) ++n;
        return n;
    };
    const int piles0 = piles_of();
    // A pile of 300 on the ground between them.
    uint16_t id = u.w().create_item(StatGrowth::kMoneyShape, 0);
    u.w().item(id)->quality = 300;
    u.w().call_event(id, kEvCombine);
    int x = av->x + 40, y = av->y + 40;
    World::Placement pl = u.w().can_exist_shape(StatGrowth::kMoneyShape, x, y, av->z, id);
    CHECK(pl.valid);
    u.w().move_to(id, x, y, pl.supported ? av->z : pl.support_z, false);
    u.frames(3);
    // The player's drag (the real mouse path).
    auto drag = [&](uint16_t item, uint16_t target) {
        int sx, sy, ix = -1, iy = -1, tx = -1, ty = -1;
        u.frames(2);
        u.w().screen_point(*u.w().item(item), sx, sy);
        for (int dy = -20; dy <= 4 && ix < 0; ++dy)
            for (int dx = -16; dx <= 16 && ix < 0; ++dx)
                if (u.gm.pick(sx + dx, sy + dy) == item) { ix = sx + dx; iy = sy + dy; }
        u.w().screen_point(*u.w().item(target), sx, sy);
        for (int dy = -40; dy <= 4 && tx < 0; ++dy)
            for (int dx = -16; dx <= 16 && tx < 0; ++dx)
                if (u.gm.pick(sx + dx, sy + dy) == target) { tx = sx + dx; ty = sy + dy; }
        if (ix < 0 || tx < 0) return false;
        auto mv = [&](int x2, int y2) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x2; e.y = y2; u.ev(e); };
        auto bt = [&](bool down) {
            InputEvent e; e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp; e.button = 1;
            e.x = u.gm.mouse_x(); e.y = u.gm.mouse_y(); u.ev(e);
        };
        mv(ix, iy); bt(true); u.frames(1);
        mv(ix + 10, iy + 2); u.frames(1);
        mv(tx, ty); u.frames(1);
        bt(false); u.frames(2);
        return true;
    };
    CHECK(drag(id, k->id));
    SliderGump* s = u.w().gumps.find<SliderGump>();
    CHECK(s != nullptr);
    if (!s) return;
    std::printf("  the slider offers %d of 300\n", s->value());
    CHECK_EQ(s->value(), need);
    u.shot("magic_obsidian_slider");
    s->accept();
    u.frames(3);
    CHECK_EQ(money(u, k->id), have + need);
    // It joined a pile in his pack: no new pile.
    CHECK_EQ(piles_of(), piles0);
    const Item* rest = u.w().item(id);
    CHECK(rest && rest->quality == 300 - need && !rest->parent);
    // Full stock now (nothing needed): the slider offers the whole pile.
    m.uses["kalvax"].clear();
    CHECK(drag(id, k->id));
    s = u.w().gumps.find<SliderGump>();
    CHECK(s && s->value() == 300 - need);
    if (s) s->close();
}

void test_essential(const GameData& d) {
    std::printf("essential: Ilvaran's raise and Nerissa's rite work at level 1 with the Avatar knowing nothing\n");
    Ui u(d, true);
    u.frames(40);
    for (const char* c : {"ilvaran", "nerissa", "brannoc"}) u.p().join(u.idx(c));
    u.frames(10);
    CHECK_EQ(u.l().level_of(u.st("ilvaran").id), 1);
    CHECK(u.m().learned.empty());
    // Brannoc falls: Ilvaran raises him.
    u.w().kill_actor(u.st("brannoc").id, kDmgNormal);
    u.frames(60);
    CHECK(u.p().state[size_t(u.idx("brannoc"))].mode == CompanionState::kDead);
    Actor* il = u.a("ilvaran");
    il->mana = int16_t(u.p().max_mana(*il));
    CHECK(il->mana >= 30);
    CHECK(u.p().spell_ready(u.idx("ilvaran"), u.si("raise")));
    CHECK(u.p().cast(u.idx("ilvaran"), u.si("raise"), u.st("brannoc").id));
    u.frames(60);
    CHECK(u.p().state[size_t(u.idx("brannoc"))].mode != CompanionState::kDead);
    // Ilvaran falls: Nerissa's rite.
    u.w().kill_actor(u.st("ilvaran").id, kDmgNormal);
    u.frames(60);
    Actor* ne = u.a("nerissa");
    ne->mana = int16_t(u.p().max_mana(*ne));
    CHECK(ne->mana >= 30);
    CHECK(u.p().spell_ready(u.idx("nerissa"), u.si("tide_rite")));
    CHECK(u.p().cast(u.idx("nerissa"), u.si("tide_rite"), u.st("ilvaran").id));
    u.frames(60);
    CHECK(u.p().state[size_t(u.idx("ilvaran"))].mode != CompanionState::kDead);
}

void test_saves(const GameData& d) {
    std::printf("saves: uses, learned spells and banter flags round-trip; a 0.16.0 save starts with full stock\n");
    Ui u(d, true);
    u.frames(40);
    u.p().join(u.idx("kalvax"));
    u.frames(5);
    CompanionMagic& m = u.m();
    m.uses["kalvax"]["fire_bolt"] = 7;
    m.learned.insert("summon_dead");
    m.said_low.insert("kalvax");
    std::vector<uint8_t> b = save_bytes(u.w());
    Ui v(d, true);
    v.frames(5);
    SaveReader r(b.data(), b.size());
    CHECK(v.w().load_game(r));
    CHECK_EQ(v.m().left("kalvax", "fire_bolt"), 7);
    CHECK(v.m().learned.count("summon_dead") && v.m().said_low.count("kalvax"));
    std::vector<uint8_t> b2 = save_bytes(v.w());
    CHECK(b == b2);
    // Without the section (a 0.16.0 save): the starting stock.
    static const char t5[4] = {'L', 'V', 'L', '5'}, te[4] = {'E', 'N', 'D', '.'};
    std::vector<uint8_t> old = b;
    auto l5 = std::find_end(old.begin(), old.end(), t5, t5 + 4), en = std::find_end(old.begin(), old.end(), te, te + 4);
    CHECK(l5 != old.end() && en != old.end() && l5 < en);
    old.erase(l5, en);
    Ui o(d, true);
    o.frames(5);
    SaveReader ro(old.data(), old.size());
    CHECK(o.w().load_game(ro));
    CHECK_EQ(o.m().left("kalvax", "fire_bolt"), o.m().rule("kalvax", "fire_bolt")->stock);
    CHECK(o.m().uses.empty());
    o.frames(40);
    CHECK_EQ(o.m().left("kalvax", "fire_bolt"), o.m().rule("kalvax", "fire_bolt")->stock);
}

void test_off(const GameData& d) {
    std::printf("off: Leveling off: no locks, no uses, no topics, no restock\n");
    Ui u(d, false);
    u.frames(40);
    for (const char* c : {"kalvax", "ilvaran", "morvain"}) u.p().join(u.idx(c));
    u.frames(5);
    CHECK(u.p().spell_ready(u.idx("kalvax"), u.si("fire_field")));
    CHECK(u.p().spell_ready(u.idx("ilvaran"), u.si("heal")));
    uint16_t f = foe(u);
    Actor* ka = u.a("kalvax");
    ka->mana = 40;
    CHECK(u.p().cast(u.idx("kalvax"), u.si("fire_field"), f));
    CHECK(u.m().uses.empty());
    for (const auto& o : u.w().turn->cast_options(ka->id)) CHECK(!o.locked && o.uses == -1);
    bool topic = false;
    for (const PartyTopic* t : u.p().visible_topics(u.idx("kalvax"))) topic = topic || t->id.rfind("reagents", 0) == 0;
    CHECK(!topic);
    CHECK(u.m().summary(u.idx("kalvax")).find("Fire Field 30") == std::string::npos);
    // A save has no LVL5 section.
    std::vector<uint8_t> b = save_bytes(u.w());
    static const char t5[4] = {'L', 'V', 'L', '5'};
    CHECK(std::search(b.begin(), b.end(), t5, t5 + 4) == b.end());
    // Mythran: nothing happens.
    u.m().uses["kalvax"]["fire_bolt"] = 0;  // as if left from a game with it on
    give(u.w(), ka->id, StatGrowth::kMoneyShape, 0, 500);
    const int k0 = money(u, ka->id);
    CHECK(talk_mythran_and_leave(u));
    u.frames(5);
    CHECK_EQ(money(u, ka->id), k0);
    CHECK(!has_log(u.m().log, "mythran talk ended"));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_magic_saves";
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
    if (want("unlock")) test_unlock(d);
    if (want("uses")) test_uses(d);
    if (want("ui")) test_ui(d);
    if (want("auto")) test_auto(d);
    if (want("banter")) test_banter(d);
    if (want("talk")) test_talk(d);
    if (want("mythran")) test_mythran(d);
    if (want("drop")) test_drop(d);
    if (want("essential")) test_essential(d);
    if (want("saves")) test_saves(d);
    if (want("off")) test_off(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
