// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Turn-based combat (game/turn_combat.h, app/turn_ui.h, PLAN-TURNBASED.md) through the real game
// mode: a party fight against a group of monsters played through the keyboard menu (victory,
// turns in DEX order), Party Auto finishing a fight with no input, mouse and controller flows
// (move with the route preview and confirm, attack, Party Auto on RB), escape, defeat -> the
// death menu, a monster that notices the party joining next round, a save / load in the middle
// of a fight, a scripted fight staying in real time, and Real-time = the old combat.
//   pagan_turn_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [only]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "app/turn_ui.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/game_log.h"
#include "game/item_powers.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "usecode/uc_machine.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool companions, bool turn = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = companions;
        w.party->beach_always = true;  // the companions on the beach, as these tests expect
        w.turn->enabled = turn;
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    TurnCombat& t() { return *gm.world().turn; }
    TurnUI& ui() { return gm.turn_ui(); }
    void frames(int n, bool draw = true) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            if (draw) gm.render(surf);
        }
    }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b; e.x = gm.mouse_x(); e.y = gm.mouse_y();
        ev(e);
    }
    void click(int x, int y) { move(x, y); button(1, true); button(1, false); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; e.pad = 7; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 3);
        std::printf("  shot %s\n", path.c_str());
    }
    uint16_t cid(const std::string& name) { int i = p().find_def(name); return i >= 0 ? p().state[size_t(i)].id : 0; }
    int idx(const std::string& name) { return p().find_def(name); }
    void avatar_to(int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().move_to(1, x, y, z, false);
        frames(2);
    }
    void combat_key() { key('c'); }
};

bool find_free(World& w, unsigned shape, int x, int y, int z, int r0, int r1, uint16_t ignore, int& ox, int& oy, int& oz,
               double ang0 = 0) {
    for (int r = r0; r <= r1; r += 16)
        for (int k = 0; k < 16; ++k) {
            double ang = ang0 + 6.2831853 * k / 16;
            int px = x + int(r * std::cos(ang)), py = y + int(r * std::sin(ang));
            for (int dz : {0, 8, -8, 16}) {
                if (z + dz < 0) continue;
                World::Placement pl = w.can_exist_shape(shape, px, py, z + dz, ignore);
                if (pl.valid && pl.supported && pl.land) { ox = px; oy = py; oz = z + dz; return true; }
            }
        }
    return false;
}

uint16_t spawn(Ui& u, unsigned shape, int r0, int r1, double ang = 0) {
    const Actor* av = u.w().avatar();
    uint16_t m = u.w().create_monster(shape, 0);
    int x, y, z;
    if (!m || !find_free(u.w(), shape, av->x, av->y, av->z, r0, r1, m, x, y, z, ang)) return 0;
    u.w().move_to(m, x, y, z, false);
    return m;
}

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

// The rounds' turns are in DEX order (highest first); `waited` turns may come later.
bool dex_order_ok(Ui& u, int& rounds_checked) {
    const TurnCombat& t = u.t();
    rounds_checked = 0;
    bool ok = true;
    for (size_t i = 0; i + 1 < t.turn_history.size(); ++i) {
        if (t.turn_rounds[i] != t.turn_rounds[i + 1]) { ++rounds_checked; continue; }
        const Actor* a = u.w().actor(t.turn_history[i]);
        const Actor* b = u.w().actor(t.turn_history[i + 1]);
        if (a && b && a->dex < b->dex) {
            std::printf("  order: %u (dex %d) before %u (dex %d) in round %d\n", a->id, a->dex, b->id, b->dex, t.turn_rounds[i]);
            ok = false;
        }
    }
    return ok;
}

// Plays the Avatar's turns through the keyboard menu: attack the nearest foe (moving in if it
// can be reached this turn), else move toward it, else end the turn.
void avatar_turn_keys(Ui& u) {
    TurnCombat& t = u.t();
    if (!t.awaiting_player() || t.current() != 1) return;
    std::vector<uint16_t> foes = t.enemies_of(1);
    if (foes.empty()) { u.key('9'); return; }
    bool reach = false;
    for (uint16_t f : foes) reach |= t.attack_reachable(f);
    if (reach) {
        u.key('2');  // Attack: the nearest foe is picked first; arrows cycle to one in reach
        for (size_t k = 0; k < foes.size() && !t.attack_reachable(u.ui().picked_target() ? u.ui().picked_target() : foes[k]); ++k) {
            if (t.attack_reachable(foes[k])) break;
            u.key(kKeyRight);
        }
        u.key(kKeyReturn);
        if (t.awaiting_player()) { u.key(kKeyEscape); u.key('9'); }
        return;
    }
    if (!t.moved()) {
        u.key('1');  // Move: the pick starts at the spot nearest the foe
        u.frames(1);
        u.key(kKeyReturn);
        if (!t.awaiting_player()) return;
        u.key(kKeyEscape);
    }
    u.key('9');
}

void join_party(Ui& u, std::initializer_list<const char*> names) {
    for (const char* n : names) u.p().join(u.idx(n));
}

// ---------------------------------------------------------------------------------------------

void test_party_fight(const GameData& d) {
    std::printf("party fight (keyboard menu for the Avatar, companions on Auto)\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"brannoc", "varn", "ilvaran", "kalvax"});
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    Actor* av = u.w().avatar();
    av->hp = uint16_t(2 * av->str);
    uint16_t m[3] = {spawn(u, 142, 260, 420, 0.3), spawn(u, 142, 260, 420, 1.3), spawn(u, 807, 220, 400, 2.4)};
    CHECK(m[0] && m[1] && m[2]);
    u.frames(2);
    CHECK(!u.t().active());
    u.combat_key();
    u.frames(3);
    CHECK(u.t().active());
    std::printf("  fight started: %zu fighters\n", u.t().order().size() + 0);
    u.frames(40);
    u.shot("turn_strip");
    for (uint16_t c : {u.cid("brannoc"), u.cid("varn"), u.cid("ilvaran"), u.cid("kalvax")}) u.t().toggle_keep_auto(c);
    bool menu_shot = false, route_shot = false, target_shot = false, spell_shot = false;
    int f = 0;
    int casts = 0;
    for (; f < 9000 && u.t().active(); ++f) {
        if (u.t().awaiting_player() && u.t().current() == 1) {
            if (!menu_shot) { u.frames(2); u.shot("turn_menu"); menu_shot = true; }
            if (!route_shot && !u.t().moved()) {
                u.key('1');
                u.frames(2);
                u.key(kKeyLeft);
                u.frames(2);
                u.shot("turn_reach_route");
                route_shot = true;
                u.key(kKeyEscape);
            }
            if (!target_shot && !u.t().enemies_of(1).empty()) {
                u.key('2');
                u.frames(2);
                u.shot("turn_targeting");
                target_shot = true;
                u.key(kKeyEscape);
            }
            avatar_turn_keys(u);
        }
        if (!spell_shot && !u.w().party->bolts.empty()) { u.shot("turn_spell"); spell_shot = true; }
        u.frames(1, f % 3 == 0);
    }
    for (const std::string& l : u.t().log) casts += l.find(" casts ") != std::string::npos;
    int rounds = 0;
    bool order_ok = dex_order_ok(u, rounds);
    int dead = 0;
    for (uint16_t id : m) dead += !u.w().actor(id) || u.w().actor(id)->is(Actor::kDead);
    std::printf("  ended after %d frames: %s; %d rounds, %zu turns, %d spells cast, %d of 3 foes dead, Avatar hp %d\n", f,
                u.t().last_end == TurnCombat::kVictory ? "victory" : "not victory", u.t().round(), u.t().turn_history.size(),
                casts, dead, av->hp);
    CHECK(!u.t().active());
    CHECK(u.t().last_end == TurnCombat::kVictory);
    CHECK_EQ(dead, 3);
    CHECK(order_ok);
    CHECK(rounds >= 1);
    CHECK(casts >= 1);
    CHECK(!av->is(Actor::kDead));
    u.frames(5);
    u.shot("turn_victory");
    // After the fight the world runs again: the companions follow.
    int x0 = u.w().actor(u.cid("brannoc"))->x;
    u.avatar_to(av->x + 400, av->y, av->z);
    u.frames(200);
    CHECK(u.w().actor(u.cid("brannoc"))->x != x0);
}

void test_party_auto(const GameData& d) {
    std::printf("Party Auto: a fight with no input at all\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"brannoc", "varn", "wynna"});
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    uint16_t m[2] = {spawn(u, 142, 260, 420, 0.5), spawn(u, 807, 260, 420, 2.0)};
    CHECK(m[0] && m[1]);
    u.frames(2);
    u.combat_key();
    u.frames(3);
    CHECK(u.t().active());
    // The strip's Party Auto button (mouse).
    u.frames(30);
    int bx, by;
    CHECK(u.ui().strip_auto_point(bx, by));
    u.click(bx, by);
    CHECK(u.t().party_auto);
    u.frames(30);
    u.shot("turn_party_auto");
    int f = 0, player_waits = 0;
    for (; f < 9000 && u.t().active(); ++f) {
        if (u.t().awaiting_player()) ++player_waits;
        u.frames(1, f % 4 == 0);
    }
    std::printf("  ended after %d frames (%d rounds): %s; frames waiting for the player: %d\n", f, u.t().round(),
                u.t().last_end == TurnCombat::kVictory ? "victory" : "other", player_waits);
    CHECK(u.t().last_end == TurnCombat::kVictory);
    CHECK_EQ(player_waits, 0);
    // Taking back control: Party Auto off with the A key in the next fight (the win ended combat
    // mode, so this one starts by drawing the weapon again).
    uint16_t g = spawn(u, 142, 260, 420, 1.0);
    CHECK(g);
    u.frames(2);
    u.combat_key();
    u.frames(10);
    CHECK(u.t().active());
    u.key('a');
    CHECK(!u.t().party_auto);
    bool mine = false;
    for (int i = 0; i < 900 && u.t().active() && !mine; ++i) {
        u.frames(1, false);
        mine = u.t().awaiting_player();
    }
    CHECK(mine);
    u.t().end_fight(TurnCombat::kAborted);
}

void test_mouse_and_pad(const GameData& d) {
    std::printf("mouse and controller flows\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    uint16_t g = spawn(u, 142, 300, 420, 0.8);
    CHECK(g);
    u.t().auto_end = false;  // the menu after a Move is tested here (test_auto_end: the rule)
    u.frames(2);
    u.combat_key();
    for (int i = 0; i < 300 && !(u.t().awaiting_player() && u.t().current() == 1); ++i) u.frames(1);
    CHECK(u.t().awaiting_player() && u.t().current() == 1);
    // Mouse: Move, point at a spot, click, click again.
    u.frames(1);
    int mx, my;
    CHECK(u.ui().menu_point(TurnUI::kEMove, mx, my));
    u.click(mx, my);
    u.frames(1);
    CHECK(u.ui().mode() == TurnUI::kPickMove);
    const auto& r = u.t().reach();
    std::printf("  reachable area: %zu spots\n", r.size());
    CHECK(r.size() > 20);
    int far = 1;
    for (size_t i = 1; i < r.size(); ++i)
        if (r[i].cost > r[size_t(far)].cost) far = int(i);
    CHECK(r[size_t(far)].cost <= TurnCombat::kMoveRange);
    CHECK(r[size_t(far)].cost >= TurnCombat::kMoveRange - 24);
    int nx, ny;
    u.ui().node_screen(far, nx, ny);
    u.click(nx, ny);
    u.frames(1);
    CHECK(u.ui().picked_node() > 0);
    CHECK(u.t().awaiting_player());  // the first click only picks (the route is shown)
    int px, py, pz;
    u.t().node_point(u.ui().picked_node(), px, py, pz);
    const Actor* av = u.w().avatar();
    int x0 = av->x, y0 = av->y;
    u.click(nx, ny);  // again: go
    CHECK(!u.t().awaiting_player());
    for (int i = 0; i < 200 && !u.t().awaiting_player(); ++i) u.frames(1);
    double moved = std::hypot(av->x - x0, av->y - y0);
    double off = std::hypot(av->x - (px + 16), av->y - (py + 16));
    std::printf("  moved %.0f units (the planned spot is %.0f away from where he stopped)\n", moved, off);
    CHECK(moved > 30 && moved < TurnCombat::kMoveRange + 40);
    CHECK(u.t().moved());
    CHECK(u.t().awaiting_player() && u.t().current() == 1);
    // Controller: D-pad down to Defend (entry 5) and A.
    u.pad(kPadY);
    for (int k = 0; k < TurnUI::kEDefend; ++k) u.pad(kPadDown);
    u.pad(kPadA);
    CHECK(!u.t().awaiting_player());
    // RB: Party Auto on and off.
    u.pad(kPadRB);
    CHECK(u.t().party_auto);
    u.pad(kPadRB);
    CHECK(!u.t().party_auto);
    // Next Avatar turn: controller Attack on the ghoul (moving in if needed), A to pick, A to confirm.
    bool attacked = false;
    for (int i = 0; i < 3000 && u.t().active() && !attacked; ++i) {
        if (u.t().awaiting_player() && u.t().current() == 1) {
            u.pad(kPadY);
            if (u.t().attack_reachable(g)) {
                u.pad(kPadDown);  // Attack
                u.pad(kPadA);
                CHECK(u.ui().mode() == TurnUI::kPickTarget);
                u.pad(kPadA);
                attacked = !u.t().awaiting_player();
            } else {
                u.pad(kPadA);  // Move (the pick starts toward the foe)
                u.pad(kPadA);
                if (u.t().awaiting_player()) { u.pad(kPadB); for (int k = 0; k < TurnUI::kEEnd; ++k) u.pad(kPadDown); u.pad(kPadA); }
            }
        }
        u.frames(1, false);
    }
    CHECK(attacked);
    u.t().end_fight(TurnCombat::kAborted);
}

void test_escape(const GameData& d) {
    std::printf("escape\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    uint16_t g = spawn(u, 142, 300, 420);
    CHECK(g);
    u.frames(2);
    u.combat_key();
    u.frames(5);
    CHECK(u.t().active());
    // Far away (a teleport counts like running off): every foe is out of the area.
    u.avatar_to(14300 + 2400, 6150 - 1800, 0);
    bool escaped = false;
    for (int i = 0; i < 5 && !escaped; ++i) {
        u.frames(1);
        escaped = !u.t().active() && u.t().last_end == TurnCombat::kEscape;
    }
    std::printf("  after the jump: %s\n", escaped ? "escaped" : "still fighting");
    CHECK(escaped);
    // The ghoul hunts in real time again.
    const Actor* ga = u.w().actor(g);
    CHECK(ga && u.w().kernel().count_exact(g, kProcCombat) == 1);
}

void test_defeat(const GameData& d) {
    std::printf("defeat -> the death menu\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    Actor* av = u.w().avatar();
    av->hp = 3;
    uint16_t m[2] = {spawn(u, 120, 120, 300, 0.2), spawn(u, 120, 120, 300, 2.2)};  // toraxes
    CHECK(m[0] && m[1]);
    u.frames(2);
    u.combat_key();
    u.frames(3);
    CHECK(u.t().active());
    int f = 0;
    for (; f < 6000 && !av->is(Actor::kDead); ++f) {
        if (u.t().awaiting_player()) u.key('5');  // Defend (and wait for the end)
        u.frames(1, false);
    }
    std::printf("  Avatar dead after %d frames; fight: %s\n", f, u.t().last_end == TurnCombat::kDefeat ? "defeat" : "?");
    CHECK(av->is(Actor::kDead));
    u.frames(2);
    CHECK(!u.t().active());
    CHECK(u.t().last_end == TurnCombat::kDefeat);
    // The normal death sequence: the gravestone, then the main menu.
    for (int i = 0; i < 900 && !u.w().game_over; ++i) {
        u.frames(1, false);
        if (u.w().reader() && i % 30 == 29) u.key(kKeyEscape);
    }
    CHECK(u.w().game_over);
    u.frames(3);
    CHECK(u.w().gumps.find<MenuGump>() != nullptr);
}

void test_join(const GameData& d) {
    std::printf("joining mid-fight\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    uint16_t g = spawn(u, 142, 300, 420);
    // A second ghoul far out of the area, not fighting yet.
    uint16_t late = spawn(u, 142, 1500, 1800, 3.6);
    CHECK(g && late);
    u.frames(2);
    u.combat_key();
    u.frames(3);
    CHECK(u.t().active());
    CHECK(!u.t().is_fighter(late));
    for (int i = 0; i < 120 && u.t().round() < 1; ++i) u.frames(1, false);
    int r0 = u.t().round();
    CHECK(r0 >= 1);
    // It notices the party: walks up (put close) and attacks.
    const Actor* av = u.w().avatar();
    int x, y, z;
    CHECK(find_free(u.w(), 142, av->x, av->y, av->z, 200, 320, late, x, y, z, 4.0));
    u.w().move_to(late, x, y, z, false);
    u.w().set_in_combat(late);
    if (Actor* la = u.w().actor(late)) la->target = 1;
    CHECK(!u.t().is_fighter(late));  // not in the middle of the round
    int f = 0;
    for (; f < 3000 && u.t().active() && u.t().round() == r0; ++f) {
        if (u.t().awaiting_player()) u.key('5');
        u.frames(1, false);
    }
    std::printf("  round %d -> %d after %d frames: the late ghoul %s\n", r0, u.t().round(), f,
                u.t().is_fighter(late) ? "joined" : "did not join");
    CHECK(u.t().is_fighter(late));
    bool said = false;
    for (const std::string& l : u.t().log) said |= l.find("joins the fight") != std::string::npos;
    CHECK(said);
    u.t().end_fight(TurnCombat::kAborted);
}

void test_save_load(const GameData& d) {
    std::printf("save / load in the middle of a fight\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"brannoc", "kalvax"});
    u.avatar_to(14300, 6150, 0);
    u.frames(120);
    uint16_t g = spawn(u, 142, 260, 420);
    CHECK(g);
    u.frames(2);
    u.combat_key();
    for (int i = 0; i < 600 && !(u.t().awaiting_player() && u.t().current() == 1); ++i) u.frames(1, false);
    CHECK(u.t().awaiting_player());
    // The save is allowed (Esc -> main menu -> diary in the game; here directly).
    CHECK(u.w().can_save());
    CHECK(u.gm.save_slot(5, "In a fight"));
    std::vector<uint8_t> a = save_bytes(u.w());
    CHECK(u.gm.load_slot(5));
    std::vector<uint8_t> b = save_bytes(u.w());
    CHECK(a == b);  // nothing lost or changed by the round trip
    CHECK(u.w().actor(g) != nullptr);
    // The fight picks up again (the Avatar is still in combat mode with the ghoul there).
    for (int i = 0; i < 30 && !u.t().active(); ++i) u.frames(1, false);
    CHECK(u.t().active());
    CHECK(u.t().is_fighter(g));
    // ... and can be fought to the end.
    u.t().party_auto = true;
    int f = 0;
    for (; f < 6000 && u.t().active(); ++f) u.frames(1, false);
    std::printf("  after the load the fight ended: %s (%d frames)\n", u.t().last_end == TurnCombat::kVictory ? "victory" : "other", f);
    CHECK(u.t().last_end == TurnCombat::kVictory);
    // A save during the enemy's action (a frozen world with things in motion) loads too.
    uint16_t g2 = spawn(u, 142, 220, 300);
    u.frames(2, false);
    u.combat_key();
    u.frames(5, false);
    CHECK(u.t().active());
    for (int i = 0; i < 600 && (u.t().awaiting_player() || u.t().current() != g2); ++i) {
        if (u.t().awaiting_player()) u.key('5');
        u.frames(1, false);
    }
    u.frames(8, false);
    CHECK(u.gm.save_slot(6, "Enemy turn"));
    CHECK(u.gm.load_slot(6));
    u.frames(60, false);
    CHECK(u.w().avatar() && !u.w().avatar()->is(Actor::kDead));
}

void test_scripted(const GameData& d) {
    std::printf("scripted fights stay in real time\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    // Devon (NPC 2, class DEVON with a cast-event handler) turned hostile beside the Avatar.
    Actor* dv = u.w().actor(2);
    CHECK(dv != nullptr);
    if (!dv) return;
    const Actor* av = u.w().avatar();
    int x, y, z;
    CHECK(find_free(u.w(), dv->shape, av->x, av->y, av->z, 200, 320, 2, x, y, z));
    u.w().kernel().kill(2, kProcAny);
    if (dv->chunk >= 0) u.w().detach(2);
    dv->mapnum = uint8_t(u.w().map().map_number());
    u.w().move_to(2, x, y, z, false);
    dv->enemy_alignment = 2;
    u.w().set_in_combat(2);
    if ((dv = u.w().actor(2))) { dv->target = 1; dv->fixed_target = 1; }
    u.frames(3);
    std::string why;
    CHECK(u.t().is_scripted(2, &why));
    u.combat_key();
    u.frames(5);
    CHECK(!u.t().active());
    bool noted = false;
    for (const auto& n : u.t().fallbacks) noted |= n.id == 2;
    std::printf("  Devon: %s (%s)\n", u.t().active() ? "turn-based" : "real time", why.c_str());
    CHECK(noted);
}

void test_realtime(const GameData& d) {
    std::printf("Real-time setting: the old combat\n");
    Ui u(d, false, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(60);
    uint16_t g = spawn(u, 142, 260, 400);
    CHECK(g);
    u.w().set_in_combat(g);
    if (Actor* ga = u.w().actor(g)) { ga->target = 1; ga->fixed_target = 1; }
    u.combat_key();
    bool ever = false;
    int hp0 = u.w().avatar()->hp;
    int d0 = int(std::hypot(u.w().actor(g)->x - u.w().avatar()->x, u.w().actor(g)->y - u.w().avatar()->y));
    for (int i = 0; i < 600; ++i) {
        u.frames(1, false);
        ever |= u.t().active();
    }
    CHECK(!ever);
    int d1 = int(std::hypot(u.w().actor(g)->x - u.w().avatar()->x, u.w().actor(g)->y - u.w().avatar()->y));
    std::printf("  real time: the ghoul came from %d to %d units, Avatar hp %d -> %d\n", d0, d1, hp0, u.w().avatar()->hp);
    CHECK(d1 < d0 - 50 || int(u.w().avatar()->hp) < hp0);  // the ghoul fought in real time
    // Switching the option on in the options page rows starts turns (C again).
    u.t().enabled = true;
    u.frames(5, false);
    CHECK(u.t().active());
}

void test_pull(const GameData& d) {
    std::printf("companions left behind come to the edge of the screen\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"brannoc", "varn", "kalvax", "ilvaran"});
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    // Ilvaran is told to wait here.
    u.p().wait(u.idx("ilvaran"));
    u.frames(5);
    const Actor* il = u.w().actor(u.cid("ilvaran"));
    int ilx = il->x, ily = il->y;
    // The Avatar runs off (a jump of ~900 units): the followers are left far behind.
    u.avatar_to(14300 + 600, 6150 - 700, 0);
    const Actor* av = u.w().avatar();
    std::printf("  Avatar at %d,%d,%d\n", av->x, av->y, av->z);
    uint16_t g = spawn(u, 142, 300, 420, 0.4);
    CHECK(g);
    int off = 0;
    for (const char* n : {"brannoc", "varn", "kalvax"}) {
        const Actor* a = u.w().actor(u.cid(n));
        int dx = a->x - av->x, dy = a->y - av->y;
        off += std::abs((dx - dy) / 4) > 160 || std::abs((dx + dy) / 8) > 100;
    }
    std::printf("  followers off screen before: %d of 3\n", off);
    CHECK_EQ(off, 3);
    u.combat_key();
    u.frames(3);
    CHECK(u.t().active());
    u.shot("turn_pull_edge");
    for (const char* n : {"brannoc", "varn", "kalvax"}) {
        const Actor* a = u.w().actor(u.cid(n));
        int dx = a->x - av->x, dy = a->y - av->y, sx = (dx - dy) / 4, sy = (dx + dy) / 8 - (a->z - av->z);
        World::Placement pl = u.w().can_exist(a->x, a->y, a->z, 0, 0, 0, 0, a->id);
        (void)pl;
        Box b = u.w().box_of(*a);
        World::Placement fit = u.w().can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, u.w().shape_flags(*a), a->id);
        const Actor* ga = u.w().actor(g);
        int dg = int(std::hypot(a->x - ga->x, a->y - ga->y));
        std::printf("  %s at screen %d,%d from the Avatar (dz %d), %d from the ghoul, fits %d supported %d, in the fight %d\n", n, sx,
                    sy, a->z - av->z, dg, fit.valid, fit.supported, u.t().is_fighter(a->id));
        CHECK(std::abs(sx) <= 160 && std::abs(sy) <= 100);
        CHECK(std::abs(sx) >= 60 || sy <= -16 || sy >= 50);  // at an edge of the visible world (below the top bars), not beside him
        CHECK(fit.valid && fit.supported);
        CHECK(std::abs(a->z - av->z) <= 24);
        CHECK(dg >= 192);
        CHECK(u.t().is_fighter(a->id));
    }
    il = u.w().actor(u.cid("ilvaran"));
    CHECK(il->x == ilx && il->y == ily);  // the waiting one stays
    CHECK(!u.t().is_fighter(il->id));
    u.t().end_fight(TurnCombat::kAborted);
    // Real time too: leave combat, run off again, enter combat mode.
    u.t().enabled = false;
    u.combat_key();
    u.frames(5);
    u.avatar_to(av->x - 900, av->y + 500, 0);
    u.frames(2);
    u.combat_key();
    u.frames(2);
    int on = 0;
    for (const char* n : {"brannoc", "varn", "kalvax"}) {
        const Actor* a = u.w().actor(u.cid(n));
        int dx = a->x - av->x, dy = a->y - av->y;
        on += std::abs((dx - dy) / 4) <= 160 && std::abs((dx + dy) / 8 - (a->z - av->z)) <= 100;
    }
    std::printf("  real-time combat mode: %d of 3 followers on screen\n", on);
    CHECK_EQ(on, 3);
}

// A fight that starts in the middle of a conversation ends the conversation (the answer menu,
// the speech, the conversation's usecode process and the stasis it set), so the fight can go
// on; a fight that belongs to the conversation's own scene does not.
void test_talk_interrupt(const GameData& d) {
    std::printf("a fight ends a conversation\n");
    auto wait_menu = [](Ui& u, int max) {
        for (int f = 0; f < max && !u.w().ask_menu; ++f) u.frames(1, false);
        return u.w().ask_menu != nullptr;
    };
    auto hostile = [](Ui& u, uint16_t g) {
        u.w().set_in_combat(g);
        if (Actor* ga = u.w().actor(g)) { ga->target = 1; ga->fixed_target = 1; }
    };
    // 1. Talking to a companion, the player draws (C) with a ghoul about: turns start, the talk ends.
    {
        Ui u(d, true);
        u.frames(60);
        join_party(u, {"brannoc"});
        u.avatar_to(14300, 6150, 0);
        u.frames(120);
        uint16_t g = spawn(u, 142, 300, 420, 0.4);
        CHECK(g);
        u.p().start_talk(u.idx("brannoc"));
        CHECK(wait_menu(u, 600));
        CHECK(u.p().talking());
        u.frames(30, false);
        u.combat_key();
        u.frames(3);
        std::printf("  companion talk + C: menu %d, talking %d, fight %d\n", u.w().ask_menu != nullptr, u.p().talking(), u.t().active());
        CHECK(!u.w().ask_menu);
        CHECK(!u.p().talking());
        CHECK(u.t().active());
        CHECK_EQ(u.w().conversations_ended, 1u);
        CHECK(!u.w().in_conversation());
    }
    // 2. Devon's conversation (usecode, with stasis), in real time: a ghoul's blow ends it, the
    // stasis is lifted and the Avatar can fight. Then the same with turns: the fight starts.
    for (int turn = 0; turn < 2; ++turn) {
        Ui u(d, false, turn == 1);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(60);
        u.w().call_event(2, kEvUse);
        CHECK(wait_menu(u, 900));
        uint16_t talk = u.w().talk_pid;
        CHECK(talk != 0);
        CHECK(u.w().in_conversation());
        bool stasis = u.w().stasis;
        u.frames(30, false);
        uint16_t g = spawn(u, 142, 64, 160, 2.0);
        CHECK(g);
        hostile(u, g);
        int f = 0;
        for (; f < 900 && u.w().conversations_ended == 0; ++f) u.frames(1, false);
        u.frames(3, false);
        const Process* tp = u.w().kernel().get(talk);
        std::printf("  Devon (%s, stasis %d): ended after %d frames; menu %d, stasis %d, script %s, fight %d\n",
                    turn ? "turns" : "real time", stasis, f, u.w().ask_menu != nullptr, u.w().stasis,
                    tp && tp->live() ? "running" : "ended", u.t().active());
        CHECK_EQ(u.w().conversations_ended, 1u);
        CHECK(!u.w().ask_menu);
        CHECK(!u.w().stasis);
        CHECK(!(tp && tp->live() && dynamic_cast<const UCProcess*>(tp)));
        CHECK(!u.w().in_conversation());
        bool speech = false;
        for (const Bark& b : u.w().barks) speech |= b.speaker == 1 || b.speaker == 2;
        CHECK(!speech);
        if (turn) {
            CHECK(u.t().active());
            u.shot("turn_talk_interrupted");
        } else {
            // The Avatar is free: C draws his weapon.
            u.combat_key();
            u.frames(3, false);
            CHECK(u.w().avatar()->is(Actor::kInCombat));
        }
    }
    // 3. Between the menus of a conversation (someone speaking) a fight by the one talked to is
    // the scene's own (a script's "then die!"): it goes on. One by an outsider ends it.
    {
        Ui u(d, false, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(60);
        u.w().call_event(2, kEvUse);
        CHECK(wait_menu(u, 900));
        u.frames(45, false);
        u.w().choose_answer(0);
        u.frames(2, false);
        bool between = !u.w().ask_menu && u.w().in_conversation();
        std::printf("  between menus: %s\n", between ? "Devon speaks" : "no (the menu came back at once)");
        if (between) {
            CHECK(!u.w().combat_interrupts_conversation(2, "test: Devon attacks"));
            CHECK(u.w().in_conversation());
            CHECK(u.w().combat_interrupts_conversation(999, "test: an outsider attacks"));
            CHECK(!u.w().in_conversation());
            CHECK(!u.w().stasis);
        }
    }
}


// --- Combat changes of 2026-11-06 (NOTES.md "Combat round two")

// Runs until `who` waits for a command; other party members defend.
bool to_turn(Ui& u, uint16_t who, int limit = 3000) {
    u.frames(6, false);  // a fight starts within three frames of entering combat mode
    for (int i = 0; i < limit && u.t().active(); ++i) {
        if (u.t().awaiting_player()) {
            if (u.t().current() == who) return true;
            u.key('5');
        }
        u.frames(1, false);
    }
    return false;
}

// A reachable node: the farthest from `foe`, or the first the attack can hit `foe` from.
int node_far_from(Ui& u, uint16_t foe) {
    const auto& r = u.t().reach();
    const Item* f = u.w().item(foe);
    int best = -1;
    long long bd = -1;
    for (size_t i = 1; i < r.size(); ++i) {
        long long dx = r[i].st.x - f->x, dy = r[i].st.y - f->y, d = dx * dx + dy * dy;
        if (d > bd) { bd = d; best = int(i); }
    }
    return best;
}
int node_near(Ui& u, uint16_t who, uint16_t foe) {
    const auto& r = u.t().reach();
    for (size_t i = 1; i < r.size(); ++i)
        if (r[i].cost > 8 && u.t().can_hit_from(who, r[i].st, foe)) return int(i);
    return -1;
}
// Waits for the Move to finish: back in the menu (true) or the turn over (false).
bool after_move(Ui& u, uint16_t who) {
    for (int i = 0; i < 400; ++i) {
        u.frames(1, false);
        if (u.t().awaiting_player() && u.t().current() == who) return true;
        if (u.t().current() != who) return false;
        if (u.t().moved() && !u.t().awaiting_player() && i > 3) {
            // Walking or defending; a defence shows in the fighter.
            for (const auto& f : u.t().order())
                if (f.id == who && f.defending) return false;
        }
    }
    return u.t().awaiting_player();
}

void test_auto_end(const GameData& d) {
    std::printf("the turn ends by itself after a Move with nothing left to do\n");
    {
        // Far from the foe, nothing to cast, full health: the Move ends the turn, with a defence.
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        Actor* av = u.w().avatar();
        u.frames(30);
        uint16_t g = spawn(u, 142, 560, 700);
        CHECK(g);
        u.frames(2);
        u.combat_key();
        CHECK(to_turn(u, 1));
        CHECK(!u.t().has_followup(1) || !u.t().use_items(1).empty());
        std::printf("  in the pack: %zu usable; followup %d\n", u.t().use_items(1).size(), int(u.t().has_followup(1)));
        av->hp = uint16_t(2 * av->str);
        CHECK(!u.t().has_followup(1));
        int n = node_far_from(u, g);
        CHECK(n > 0);
        CHECK(u.t().cmd_move(n));
        CHECK(!after_move(u, 1));
        bool found = false;
        for (const std::string& l : u.t().log) found |= l.find("nothing more to do") != std::string::npos;
        CHECK(found);
        found = false;
        for (const std::string& l : u.t().log) found |= l.find(" defends") != std::string::npos;
        CHECK(found);
        // The foe's turn, then his again: a fresh Move, nothing skipped at the turn's start.
        for (int i = 0; i < 3000 && u.t().active() && !(u.t().awaiting_player() && u.t().current() == 1); ++i) u.frames(1, false);
        CHECK(u.t().awaiting_player() && !u.t().moved());
        // Hurt, with a potion in the pack: drinking is worth staying for.
        uint16_t pot = u.w().create_item(766, 0);
        u.w().move_to_container(pot, u.w().equipped(1, 7));
        av->hp = uint16_t(std::max(1, int(av->hp) - 5));
        CHECK(u.t().has_followup(1));
        av->hp = uint16_t(2 * av->str);
        CHECK(!u.t().has_followup(1));  // at full health the potion is no reason to wait
        u.t().end_fight(TurnCombat::kAborted);
    }
    {
        // A Move that ends next to a foe: the attack is still to come, the menu stays.
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(30);
        uint16_t g = spawn(u, 142, 200, 300);
        CHECK(g);
        u.frames(2);
        u.combat_key();
        CHECK(to_turn(u, 1));
        int n = node_near(u, 1, g);
        CHECK(n > 0);
        if (n > 0) {
            CHECK(u.t().cmd_move(n));
            CHECK(after_move(u, 1));
            CHECK(u.t().moved());
            CHECK(u.t().has_followup(1));
        }
        u.t().end_fight(TurnCombat::kAborted);
    }
    {
        // A caster: with the mana for a spell in range the menu stays; without it, the turn ends.
        Ui u(d, true);
        u.frames(60);
        join_party(u, {"kalvax"});
        u.avatar_to(14300, 6150, 0);
        u.frames(120);
        uint16_t k = u.cid("kalvax");
        uint16_t g = spawn(u, 142, 560, 620);
        CHECK(g);
        u.w().actor(g)->hp = 500;
        u.frames(2);
        u.combat_key();
        CHECK(to_turn(u, k));
        Actor* ka = u.w().actor(k);
        ka->mana = 30;
        CHECK(u.t().has_followup(k));
        int n = node_far_from(u, g);
        CHECK(n > 0);
        CHECK(u.t().cmd_move(n));
        CHECK(after_move(u, k));
        CHECK(u.t().moved());
        ka->mana = 0;
        CHECK(!u.t().has_followup(k));
        u.t().end_fight(TurnCombat::kAborted);
        // The next fight, no mana from the start: the Move ends the turn.
        ka->mana = 0;
        u.combat_key();
        u.frames(3);
        if (!u.t().active()) { u.combat_key(); u.frames(3); }
        CHECK(to_turn(u, k));
        ka->mana = 0;
        n = node_far_from(u, g);
        CHECK(n > 0);
        CHECK(u.t().cmd_move(n));
        CHECK(!after_move(u, k));
        u.t().end_fight(TurnCombat::kAborted);
    }
}

void test_classic_menu(const GameData& d) {
    std::printf("Classic HUD: the floating menu stays one column\n");
    Ui u(d, false);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(30);
    CHECK(spawn(u, 142, 260, 420));
    u.frames(2);
    u.combat_key();
    CHECK(to_turn(u, 1));
    u.frames(1);
    CHECK_EQ(u.ui().menu_size(), int(TurnUI::kEntries));
    CHECK_EQ(u.ui().menu_columns(), 1);
    CHECK_EQ(u.ui().selected_entry(), 0);
    u.key(kKeyDown);
    CHECK_EQ(u.ui().selected_entry(), 1);
    u.key(kKeyRight);
    CHECK_EQ(u.ui().selected_entry(), 1);  // no second column
    u.key(kKeyLeft);
    CHECK_EQ(u.ui().selected_entry(), 1);
    u.pad(kPadUp);
    u.pad(kPadUp);
    CHECK_EQ(u.ui().selected_entry(), int(TurnUI::kEntries) - 1);  // wraps
    u.pad(kPadDown);
    CHECK_EQ(u.ui().selected_entry(), 0);
    u.t().end_fight(TurnCombat::kAborted);
}

int spell_of(Ui& u, uint16_t who, const char* id) {
    std::vector<TurnCombat::CastOption> o = u.t().cast_options(who);
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i].spell >= 0 && u.p().spells[size_t(o[i].spell)].id == id) return int(i);
    return -1;
}

void test_targeting(const GameData& d) {
    std::printf("targeting: one press fires; out of range uses nothing\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"varn"});
    u.avatar_to(14300, 6150, 0);
    u.frames(120);
    uint16_t v = u.cid("varn");
    // Wave reaches 512 now: one foe beyond that, one well within it.
    uint16_t far = spawn(u, 142, 640, 760, 0.8), nearf = spawn(u, 142, 240, 320, 3.9);
    CHECK(far && nearf);
    u.w().actor(far)->hp = 500;
    u.w().actor(nearf)->hp = 500;
    u.frames(2);
    u.combat_key();
    CHECK(to_turn(u, v));
    Actor* va = u.w().actor(v);
    va->mana = 40;
    int w = spell_of(u, v, "wave");
    CHECK(w >= 0);
    u.pad(kPadY);
    u.key('3');
    CHECK(u.ui().mode() == TurnUI::kList);
    for (int i = 0; i < w; ++i) u.pad(kPadDown);
    int r = 0;
    CHECK(u.ui().range_circle(r));  // highlighted in the list: its reach shows
    CHECK_EQ(r, 512);
    u.pad(kPadA);
    CHECK(u.ui().mode() == TurnUI::kPickTarget);
    CHECK(u.ui().range_circle(r));
    // On the far foe: the tiny words, and A uses up nothing.
    int guard = 0;
    u.frames(1);
    while (u.ui().picked_target() != far && guard++ < 8) {
        u.frames(1);
        if (u.ui().out_of_range_shown()) break;
        u.pad(kPadRight);
    }
    u.frames(1);
    CHECK(u.ui().out_of_range_shown());
    u.shot("combat2_classic_out_of_range");
    int mana0 = va->mana;
    u.pad(kPadA);
    CHECK(u.t().awaiting_player());
    CHECK(u.ui().mode() == TurnUI::kPickTarget);
    CHECK_EQ(va->mana, mana0);
    CHECK(u.t().status == "Out of range");
    CHECK(!u.t().moved());
    // On to the near one: one press of A and the spell goes off.
    u.pad(kPadRight);
    u.frames(1);
    CHECK(!u.ui().out_of_range_shown());
    int hp0 = u.w().actor(nearf)->hp;
    u.pad(kPadA);
    CHECK(!u.t().awaiting_player());
    CHECK(va->mana < mana0);
    CHECK(!u.ui().range_circle(r));  // fired: the circle is gone
    u.frames(40, false);
    CHECK(u.w().actor(nearf)->hp < hp0);
    // Cancelled: the circle goes too. Enter fires like A.
    CHECK(to_turn(u, v));
    va->mana = 40;
    u.key('3');
    for (int i = 0; i < w; ++i) u.key(kKeyDown);
    u.key(kKeyReturn);
    CHECK(u.ui().range_circle(r));
    u.key(kKeyEscape);
    u.key(kKeyEscape);
    CHECK(u.ui().mode() == TurnUI::kMenu);
    CHECK(!u.ui().range_circle(r));
    u.key('3');
    for (int i = 0; i < w; ++i) u.key(kKeyDown);
    u.key(kKeyReturn);
    u.frames(1);
    for (int i = 0; i < 8 && u.ui().out_of_range_shown(); ++i) { u.key(kKeyRight); u.frames(1); }
    mana0 = va->mana;
    u.key(kKeyReturn);
    CHECK(!u.t().awaiting_player());
    CHECK(va->mana < mana0);
    u.t().end_fight(TurnCombat::kAborted);
    // An attack with a foe out of reach shows the words too, and keeps the turn.
    u.frames(3);
    u.combat_key();
    u.frames(3);
    if (!u.t().active()) { u.combat_key(); u.frames(3); }
    CHECK(to_turn(u, 1));
    u.key('2');
    u.frames(1);
    bool oor = false;
    for (int i = 0; i < 4 && !oor; ++i) { oor = u.ui().out_of_range_shown(); if (!oor) { u.key(kKeyRight); u.frames(1); } }
    if (oor) {
        u.key(kKeyReturn);
        CHECK(u.t().awaiting_player());
    }
    u.t().end_fight(TurnCombat::kAborted);
}

void test_cursor_items(const GameData& d) {
    std::printf("a script's target cursor in a fight: the Skull of Quakes, A and Enter\n");
    for (int how : {0, 1}) {
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(30);
        uint16_t g = spawn(u, 142, 300, 400);
        CHECK(g);
        u.w().actor(g)->hp = 500;
        uint16_t skull = u.w().create_item(814, 0);  // the Skull of Quakes, at the quality that asks for a target
        u.w().move_to_container(skull, u.w().equipped(1, 7));
        u.frames(2);
        u.combat_key();
        CHECK(to_turn(u, 1));
        std::vector<uint16_t> items = u.t().use_items(1);
        int line = int(std::find(items.begin(), items.end(), skull) - items.begin());
        CHECK(size_t(line) < items.size());
        u.key('4');
        for (int i = 0; i < line; ++i) u.key(kKeyDown);
        if (how == 0) u.pad(kPadA);
        else u.key(kKeyReturn);
        for (int i = 0; i < 60 && !u.w().target_pid; ++i) u.frames(1, false);
        CHECK(u.w().target_pid != 0);
        u.frames(2, false);
        CHECK_EQ(u.ui().cursor_target(), g);
        // Fifteen seconds and more at the cursor: it is the player's time, the turn stays his.
        u.frames(500, false);
        CHECK(u.w().target_pid != 0);
        CHECK_EQ(u.t().current(), uint16_t(1));
        CHECK_EQ(u.t().timeouts, 0);
        int hp0 = u.w().actor(g)->hp;
        const unsigned fired0 = item_powers::stats().fired;
        if (how == 0) u.pad(kPadA);
        else u.key(kKeyReturn);
        u.frames(1, false);
        CHECK(!u.w().target_pid);
        u.frames(90, false);
        CHECK(item_powers::stats().fired > fired0);
        CHECK(u.w().actor(g)->hp < hp0);
        u.t().end_fight(TurnCombat::kAborted);
    }
}

void test_spell_ranges(const GameData& d) {
    std::printf("spell reach: twice the data file's numbers, no friendly fire\n");
    Ui u(d, true);
    u.frames(60);
    for (const SpellDef& s : u.p().spells) {
        int scale = s.kind == "raise" ? 1 : kSpellRangeScale;
        CHECK_EQ(s.range, s.base_range * scale);
        CHECK_EQ(s.radius, s.base_radius * scale);
        std::printf("  %-22s %-9s range %4d -> %4d  radius %3d -> %3d\n", s.name.c_str(), s.kind.c_str(), s.base_range, s.range, s.base_radius, s.radius);
    }
    join_party(u, {"kalvax", "brannoc"});
    u.avatar_to(14300, 6150, 0);
    u.frames(120);
    uint16_t k = u.cid("kalvax"), b = u.cid("brannoc");
    // The field lands on a foe beside Brannoc and the Avatar: only the foe is hurt.
    Actor* ba = u.w().actor(b);
    uint16_t g = u.w().create_monster(142, 0);
    int x, y, z;
    CHECK(find_free(u.w(), 142, ba->x, ba->y, ba->z, 64, 150, g, x, y, z));
    u.w().move_to(g, x, y, z, false);
    u.w().actor(g)->hp = 500;
    int fi = u.p().spell_index("fire_field");
    CHECK(fi >= 0);
    CHECK_EQ(u.p().spells[size_t(fi)].radius, 192);
    u.w().actor(k)->mana = 40;
    const int hp_b = ba->hp, hp_a = u.w().avatar()->hp, hp_k = u.w().actor(k)->hp, hp_g = u.w().actor(g)->hp;
    CHECK(u.p().cast(u.idx("kalvax"), fi, g));
    u.frames(5, false);
    CHECK(u.w().actor(g)->hp < hp_g);
    CHECK_EQ(int(ba->hp), hp_b);
    CHECK_EQ(int(u.w().avatar()->hp), hp_a);
    CHECK_EQ(int(u.w().actor(k)->hp), hp_k);
}

struct StuckProcess : Process {
    explicit StuckProcess(uint16_t id) { item = id; type = kProcActorAnim; }
    void run() override {}
    const char* name() const override { return "stuck (test)"; }
};

void test_timeout(const GameData& d) {
    std::printf("a turn that never ends is called off after 15 seconds\n");
    for (int speed : {0, 2}) {
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(30);
        uint16_t g = spawn(u, 142, 240, 320);
        CHECK(g);
        u.w().actor(g)->hp = 500;
        u.frames(2);
        u.combat_key();
        u.t().speed = speed;
        for (int i = 0; i < 2000 && u.t().current() != g; ++i) {
            if (u.t().awaiting_player()) u.key('5');
            u.frames(1, false);
        }
        CHECK_EQ(u.t().current(), g);
        u.frames(2, false);
        // Past the wait for an old animation at the turn's start: its action is under way.
        for (int i = 0; i < 200 && (u.t().current() != g || i < (speed ? 3 : 10) || !u.w().kernel().count_exact(g, kProcActorAnim)); ++i) {
            if (u.t().awaiting_player()) u.key('5');
            u.frames(1, false);
        }
        // Its animation hangs (its lock with it).
        u.w().kernel().add(std::make_unique<StuckProcess>(g));
        u.w().actor(g)->set(Actor::kAnimLock, true);
        int ticks = 0;
        while (!u.t().timeouts && ticks < 1200) { u.frames(1, false); ++ticks; }
        std::printf("  speed %d: the turn ended after %d game frames (%.1f s)\n", speed, ticks, ticks / 30.0);
        CHECK(ticks >= 300 && ticks <= 460);  // 15 s from the turn's start, some of them already gone
        CHECK_EQ(u.t().timeouts, 1);
        CHECK(u.t().notice.find("missed its turn") != std::string::npos);
        bool logged = false;
        for (const std::string& l : u.t().log) logged |= l.find(" missed its turn") != std::string::npos;
        CHECK(logged);
        const Actor* ga = u.w().actor(g);
        // The stuck process is gone, and it is free (or already at its next turn, if it comes first).
        CHECK(ga && (!ga->is(Actor::kAnimLock) || u.t().current() == g));
        bool gone = true;
        u.w().kernel().for_each([&](Process* p) { gone &= !(dynamic_cast<StuckProcess*>(p) && p->live()); });
        CHECK(gone);
        CHECK(u.t().active());
        u.shot(speed ? "combat2_classic_missed_turn_fast" : "combat2_classic_missed_turn");
        // The fight goes on: the next turn comes up and the same monster acts again later.
        for (int i = 0; i < 900 && !u.t().awaiting_player(); ++i) u.frames(1, false);
        CHECK(u.t().awaiting_player());
        u.key('5');
        bool again = u.t().current() == g;
        for (int i = 0; i < 1500 && !again && u.t().active(); ++i) {
            if (u.t().awaiting_player()) u.key('5');
            u.frames(1, false);
            again = u.t().current() == g;
        }
        CHECK(again);
        CHECK_EQ(u.t().timeouts, 1);  // a normal turn now
        u.t().end_fight(TurnCombat::kAborted);
    }
    {
        // Time in a menu does not count; Auto party members have the same limit.
        Ui u(d, true);
        u.frames(60);
        join_party(u, {"brannoc"});
        u.avatar_to(14300, 6150, 0);
        u.frames(120);
        uint16_t b = u.cid("brannoc");
        uint16_t g = spawn(u, 142, 300, 400);
        CHECK(g);
        u.w().actor(g)->hp = 500;
        u.frames(2);
        u.combat_key();
        u.t().party_auto = true;
        for (int i = 0; i < 3000 && u.t().current() != b; ++i) u.frames(1, false);
        CHECK_EQ(u.t().current(), b);
        u.frames(2, false);
        u.w().kernel().add(std::make_unique<StuckProcess>(b));
        u.w().actor(b)->set(Actor::kAnimLock, true);
        u.frames(100, false);
        u.w().open_menu(MenuGump::kMenu);
        u.frames(600, false);
        CHECK_EQ(u.t().current(), b);  // 700 frames gone, 100 of them counted
        CHECK_EQ(u.t().timeouts, 0);
        u.key(kKeyEscape);
        u.frames(2, false);
        CHECK(!u.w().gumps.top_modal());
        int ticks = 0;
        while (!u.t().timeouts && ticks < 800) { u.frames(1, false); ++ticks; }
        CHECK(ticks >= 300 && ticks <= 380);
        CHECK(u.t().notice.find(" missed a turn") != std::string::npos);
        u.t().end_fight(TurnCombat::kAborted);
    }
}

// How long monsters' turns take as they stand (NOTES.md: why enemy turns feel slow).
void test_turn_lengths(const GameData& d) {
    std::printf("turn lengths of monsters, in game frames\n");
    for (unsigned shape : {142u, 807u, 119u, 403u}) {
        for (int dist : {120, 700}) {
            Ui u(d, false);
            u.frames(60);
            u.avatar_to(14300, 6150, 0);
            u.frames(30);
            uint16_t g = spawn(u, shape, dist, dist + 120);
            if (!g) continue;
            u.w().actor(g)->hp = 500;
            u.w().avatar()->hp = 500;
            u.frames(2);
            u.combat_key();
            u.frames(6, false);
            int rounds = 0, start = -1, tick = 0;
            std::string out;
            for (; tick < 4000 && rounds < 3 && u.t().active(); ++tick) {
                if (u.t().awaiting_player()) { u.key('5'); }
                bool now = u.t().current() == g;
                if (now && start < 0) start = tick;
                if (!now && start >= 0) { out += " " + std::to_string(tick - start); start = -1; ++rounds; }
                u.frames(1, false);
            }
            std::printf("  shape %u from %d units: %s\n", shape, dist, out.c_str());
            u.t().end_fight(TurnCombat::kAborted);
        }
    }
}

void test_lightning(const GameData& d) {
    std::printf("Lightning: the bolt, then the hit and one frame of flash\n");
    Ui u(d, true);
    u.frames(60);
    join_party(u, {"wynna"});
    u.avatar_to(14300, 6150, 0);
    u.frames(120);
    uint16_t wy = u.cid("wynna");
    uint16_t g = spawn(u, 142, 300, 380);
    CHECK(g);
    u.w().actor(g)->hp = 500;
    u.frames(2);
    u.combat_key();
    CHECK(to_turn(u, wy));
    u.w().actor(wy)->mana = 40;
    int li = spell_of(u, wy, "lightning");
    CHECK(li >= 0);
    u.key('3');
    for (int i = 0; i < li; ++i) u.key(kKeyDown);
    u.key(kKeyReturn);
    u.frames(1);
    u.shot("combat2_classic_range_circle");
    int hp0 = u.w().actor(g)->hp;
    u.key(kKeyReturn);
    CHECK(!u.t().awaiting_player());
    CHECK_EQ(u.w().sky_bolts.size(), size_t(1));
    CHECK_EQ(u.w().actor(g)->hp, hp0);  // not yet
    CHECK(!u.w().sky_flash);
    // The bolt: drawn over a picture without it.
    std::vector<uint32_t> before;
    int frames_seen = 0;
    while (!u.w().sky_bolts.empty() && frames_seen < 10) {
        if (frames_seen == 0) u.shot("combat2_classic_bolt1");
        if (frames_seen == 1) u.shot("combat2_classic_bolt2");
        u.gm.tick();
        ++frames_seen;
        if (!u.w().sky_bolts.empty()) CHECK_EQ(u.w().actor(g)->hp, hp0);
    }
    std::printf("  bolt seen for %d game frames\n", frames_seen);
    CHECK_EQ(frames_seen, kLightningFrames);
    CHECK(u.w().actor(g)->hp < hp0);   // the hit lands with the end of the bolt
    CHECK_EQ(u.w().sky_flash, 1);
    // One drawn frame of flash, and pale all over.
    const auto brightness = [&] {
        std::vector<uint32_t> rgba;
        u.surf.to_rgba(u.w().data().palette, u.gm.palette(), rgba);
        long long sum = 0;
        for (uint32_t c : rgba) sum += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
        return sum / (long long)rgba.size();
    };
    u.gm.render(u.surf);
    long long lit = brightness();
    CHECK_EQ(u.w().sky_flash, 0);
    u.shot("combat2_classic_after_flash");
    long long after = brightness();
    std::printf("  brightness in the flash %lld, the next frame %lld\n", lit, after);
    CHECK(lit > after + 150);
    CHECK(lit > 500);
    u.t().end_fight(TurnCombat::kAborted);
}

void test_peace(const GameData& d) {
    std::printf("after a win or an escape: out of combat mode, peaceful\n");
    {
        Ui u(d, true);
        u.frames(60);
        join_party(u, {"brannoc", "wynna"});
        u.avatar_to(14300, 6150, 0);
        u.frames(120);
        uint16_t g = spawn(u, 142, 260, 400);
        CHECK(g);
        u.frames(2);
        u.combat_key();
        u.frames(3);
        CHECK(u.t().active());
        CHECK(u.w().avatar()->is(Actor::kInCombat));
        CHECK(u.w().sound.music_combat());
        u.t().party_auto = true;
        int f = 0;
        for (; f < 9000 && u.t().active(); ++f) u.frames(1, false);
        CHECK(u.t().last_end == TurnCombat::kVictory);
        const int fights = u.t().fights;
        const Actor* av = u.w().avatar();
        CHECK(!av->is(Actor::kInCombat));
        CHECK(!av->is(Actor::kCombatRun));
        CHECK(!u.w().sound.music_combat());
        CHECK(!u.w().actor(u.cid("brannoc"))->is(Actor::kInCombat));
        CHECK(!u.w().actor(u.cid("wynna"))->is(Actor::kInCombat));
        u.t().party_auto = false;
        u.frames(120);
        // Still at peace: he puts the weapon away, nobody draws theirs again, no new fight.
        CHECK(!u.t().active());
        CHECK_EQ(u.t().fights, fights);
        CHECK(!av->is(Actor::kInCombat));
        CHECK(!u.w().actor(u.cid("brannoc"))->is(Actor::kInCombat));
        CHECK(!av->is(Actor::kAnimLock));
        CHECK(av->last_anim != 15u && av->last_anim != 7u);  // not the combat stance
        u.shot("combat2_classic_peace");
    }
    {
        // Running away ends combat mode too; the hunter can start a new fight only by landing a blow.
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(60);
        uint16_t g = spawn(u, 142, 300, 420);
        CHECK(g);
        u.frames(2);
        u.combat_key();
        u.frames(5);
        CHECK(u.t().active());
        u.avatar_to(14300 + 2400, 6150 - 1800, 0);
        u.frames(3);
        CHECK(u.t().last_end == TurnCombat::kEscape);
        CHECK(!u.w().avatar()->is(Actor::kInCombat));
        CHECK(!u.w().sound.music_combat());
    }
    {
        // Someone new in view as the last foe falls: the fight goes on with them, no false victory.
        Ui u(d, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(60);
        uint16_t g = spawn(u, 142, 260, 330);
        CHECK(g);
        u.frames(2);
        u.combat_key();
        CHECK(to_turn(u, 1));
        uint16_t h = spawn(u, 142, 500, 600, 3.0);
        CHECK(h);
        u.w().receive_hit(g, 1, 0, 9999, 0x80);
        for (int i = 0; i < 400 && u.t().active() && !u.t().is_fighter(h); ++i) { if (u.t().awaiting_player()) u.key('5'); u.frames(1, false); }
        CHECK(u.t().active());
        CHECK(u.t().is_fighter(h));
        u.t().end_fight(TurnCombat::kAborted);
    }
    {
        // Real time: combat mode ends once the foes are gone, not when none ever came.
        Ui u(d, false, false);
        u.frames(60);
        u.avatar_to(14300, 6150, 0);
        u.frames(30);
        u.combat_key();
        u.frames(120);
        CHECK(u.w().avatar()->is(Actor::kInCombat));  // his own choice: nothing was about
        uint16_t g = spawn(u, 142, 300, 400);
        CHECK(g);
        u.frames(10);
        CHECK(u.w().avatar()->is(Actor::kInCombat));
        u.w().receive_hit(g, 1, 0, 9999, 0x80);
        for (int i = 0; i < 200 && u.w().avatar()->is(Actor::kInCombat); ++i) u.frames(1, false);
        CHECK(!u.w().avatar()->is(Actor::kInCombat));
        CHECK(!u.w().sound.music_combat());
    }
}

void test_banter(const GameData& d) {
    std::printf("idle talk: one line in five, scripts unharmed\n");
    // The rule itself.
    {
        Ui u(d, false);
        u.frames(30);
        World& w = u.w();
        w.banter_one_in = 1 << 30;  // nearly every idle line goes unsaid
        UCProcess amb(w.vm(), 1029, 0x411, false, {}, {});  // Darion's muttering
        amb.start_cls = 1029;
        amb.start_addr = 0x411;
        amb.type = 0x205;
        amb.item = 5;
        const uint16_t pid = w.skip_banter(amb, 5, "My blade needs cleaning... ");
        CHECK(pid != 0);
        if (pid) CHECK(w.kernel().get(pid) != nullptr);
        // It ends by itself in about the bark's time.
        int n = 0;
        while (pid && w.kernel().get(pid) && n < 2000) { u.frames(1, false); ++n; }
        CHECK(n > 5 && n < 2000);
        CHECK_EQ(w.skip_banter(amb, 1, "Avatar line"), uint16_t(0));
        amb.item = 6;
        CHECK_EQ(w.skip_banter(amb, 5, "someone else's"), uint16_t(0));
        amb.item = 5;
        w.stasis = true;
        CHECK_EQ(w.skip_banter(amb, 5, "in a scene"), uint16_t(0));
        w.stasis = false;
        amb.type = 0x207;  // a use event: a conversation
        CHECK_EQ(w.skip_banter(amb, 5, "talk"), uint16_t(0));
        amb.type = 0x205;
        // Korick's forging (typed ambient, but it asks a question).
        UCProcess forge(w.vm(), 1024 + 18, 0, false, {}, {});
        for (unsigned c = 0; c < w.data().usecode.class_count(); ++c)
            if (w.data().usecode.class_name(c) == "KORICK") { forge.cls = forge.start_cls = uint16_t(c); forge.start_addr = 0x6662; }
        forge.type = 0x205;
        forge.item = 5;
        CHECK_EQ(w.skip_banter(forge, 5, "Do you have a chunk of silver ore?"), uint16_t(0));
    }
    // A live town, counted: Tenebrae's tavern and smithy (map 39: Jenna, Korick, Orlok), the same
    // scripts, every line against one in five. Each script goes on working in both.
    long long counts[2] = {0, 0};
    const int kFrames = 30000;
    for (int pass = 0; pass < 2; ++pass) {
        Ui u(d, false);
        u.frames(30);
        World& w = u.w();
        w.banter_one_in = pass == 0 ? 1 : World::kBanterOneIn;
        CHECK(w.change_map(39));
        u.frames(5, false);
        const Actor* who = w.actor(24);  // Jenna
        CHECK(who && who->chunk >= 0);
        u.avatar_to(who->x + 96, who->y + 96, who->z);
        std::set<uint16_t> seen;
        unsigned ambient_running = 0;
        for (int f = 0; f < kFrames; ++f) {
            u.gm.tick();
            for (const Bark& b : w.barks)
                if (b.speaker != 1 && seen.insert(b.pid).second) ++counts[pass];
            if (f == kFrames - 1)
                w.kernel().for_each([&](Process* p) { if (dynamic_cast<UCProcess*>(p) && p->type == 0x205) ++ambient_running; });
        }
        std::printf("  %s: %lld lines in %d frames, map %u, %u ambient scripts still running\n", pass == 0 ? "every line" : "one in five", counts[pass], kFrames, unsigned(w.map().map_number()), ambient_running);
        CHECK(ambient_running > 0);
    }
    CHECK(counts[0] >= 20);
    double ratio = double(counts[0]) / double(std::max(1LL, counts[1]));
    std::printf("  ratio %.1f\n", ratio);
    CHECK(ratio > 3.0 && ratio < 8.5);
    // Companions: the idle remarks.
    long long cc[2] = {0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        Ui u(d, true);
        u.w().banter_one_in = pass == 0 ? 1 : World::kBanterOneIn;
        u.frames(60);
        join_party(u, {"brannoc"});
        size_t l0 = u.p().log.size();
        u.frames(36000, false);
        for (size_t i = l0; i < u.p().log.size(); ++i)
            if (u.p().log[i].find(" bark ") != std::string::npos) ++cc[pass];
        std::printf("  companion remarks, %s: %lld\n", pass == 0 ? "every line" : "one in five", cc[pass]);
    }
    CHECK(cc[0] >= 10);
    CHECK(cc[1] * 2 < cc[0]);
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_turn_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");  // Nerissa's own figure (data/shapes/)
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, void (*f)(const GameData&)) { if (only.empty() || only == n) f(d); };
    run("fight", test_party_fight);
    run("auto", test_party_auto);
    run("input", test_mouse_and_pad);
    run("escape", test_escape);
    run("defeat", test_defeat);
    run("join", test_join);
    run("save", test_save_load);
    run("scripted", test_scripted);
    run("realtime", test_realtime);
    run("pull", test_pull);
    run("talk", test_talk_interrupt);
    run("autoend", test_auto_end);
    run("classic", test_classic_menu);
    run("targeting", test_targeting);
    run("cursor", test_cursor_items);
    run("ranges", test_spell_ranges);
    run("timeout", test_timeout);
    run("lengths", test_turn_lengths);
    run("lightning", test_lightning);
    run("peace", test_peace);
    run("banter", test_banter);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
