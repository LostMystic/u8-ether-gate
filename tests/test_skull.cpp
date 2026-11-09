// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Item powers (game/item_powers.h, NOTES.md "Item powers"): the Skull of Quakes. Its original use
// on the red slot still works; used anywhere else it hurts hostile creatures only; the cooldown
// blocks, expires and survives a save; a turn-based Use item; the option; the data file.
//   pagan_skull_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [only]
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/game_mode.h"
#include "app/turn_ui.h"
#include "check.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/item_powers.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;
const int kX = 14300, kY = 6150;  // open ground on the beach map (as in the turn tests)
constexpr unsigned kSkull = 814, kSlot = 805;
constexpr unsigned kGlobalSlotDone = 16;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool turn = false) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = true;
        w.turn->enabled = turn;
        gm.start();
        frames(60);
    }
    World& w() { return gm.world(); }
    TurnCombat& t() { return *gm.world().turn; }
    void frames(int n, bool draw = false) {
        for (int i = 0; i < n; ++i) {
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 200);
            gm.tick();
            if (draw) gm.render(surf);
        }
    }
    void ev(InputEvent e) { gm.handle(e); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void click(int x, int y) {
        InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e);
        e.type = InputEvent::kMouseDown; e.button = 1; ev(e);
        e.type = InputEvent::kMouseUp; ev(e);
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
    void avatar_to(int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().move_to(1, x, y, z, false);
        frames(10);
    }
    uint16_t give_skull(unsigned quality = 0) {
        uint16_t s = w().create_item(kSkull, 0);
        w().item(s)->quality = uint16_t(quality);
        w().move_to_container(s, w().equipped(1, 7));
        return s;
    }
    // Answers the target cursor the skull's usecode raises.
    bool pick(uint16_t target) {
        for (int i = 0; i < 30 && !w().target_pid; ++i) frames(1);
        if (!w().target_pid) return false;
        if (Process* p = w().kernel().get(w().target_pid)) { p->result = target; p->terminate(); }
        w().target_pid = 0;
        frames(2);
        return true;
    }
    uint16_t floor_item() {  // something to click on that is not the slot
        std::vector<uint16_t> near;
        w().area_search(w().avatar()->x, w().avatar()->y, 256, false, near);
        for (uint16_t k : near)
            if (const Item* it = w().item(k); it && !it->as_actor()) return k;
        return 0;
    }
};

bool free_spot(World& w, unsigned shape, int r0, int r1, uint16_t ignore, int& ox, int& oy, int& oz, double ang0) {
    const Actor* av = w.avatar();
    for (int r = r0; r <= r1; r += 16)
        for (int k = 0; k < 16; ++k) {
            double ang = ang0 + 6.2831853 * k / 16;
            int px = av->x + int(r * std::cos(ang)), py = av->y + int(r * std::sin(ang));
            for (int dz : {0, 8, -8, 16}) {
                if (av->z + dz < 0) continue;
                World::Placement pl = w.can_exist_shape(shape, px, py, av->z + dz, ignore);
                if (pl.valid && pl.supported && pl.land) { ox = px; oy = py; oz = av->z + dz; return true; }
            }
        }
    return false;
}

uint16_t spawn(Ui& u, unsigned shape, int r0, int r1, double ang = 0) {
    uint16_t m = u.w().create_monster(shape, 0);
    int x, y, z;
    if (!m || !free_spot(u.w(), shape, r0, r1, m, x, y, z, ang)) return 0;
    u.w().move_to(m, x, y, z, false);
    return m;
}

bool log_has(const World& w, const std::string& s) {
    for (const GameLog::Line& l : w.game_log.lines(GameLog::kCombat))
        if (l.text.find(s) != std::string::npos) return true;
    return false;
}
bool bark_has(const World& w, uint16_t speaker, const std::string& s) {
    for (const Bark& b : w.barks)
        if (b.speaker == speaker)
            {
                std::string all;  // (the words wrap onto several lines)
                for (const TextPage& p : b.pages)
                    for (const auto& line : p.lines) all += line + " ";
                if (all.find(s) != std::string::npos) return true;
            }
    return false;
}
const item_powers::Ability& skull_ability() { return item_powers::abilities().front(); }

// ---------------------------------------------------------------------------------------------

void test_original_use(const GameData& d) {
    std::printf("the original use: the real skull from the chest on the red slot\n");
    for (bool powers : {true, false}) {
        item_powers::enabled() = powers;
        Ui u(d);
        World& w = u.w();
        // The real one: map 54, in its trapped chest, as the game ships it.
        w.teleport_actor(1, 18911, 8735, 8, 54);
        u.frames(5);
        uint16_t skull = 0;
        for (unsigned i = 1; i < w.objects().slots() && !skull; ++i)
            if (const Item* it = w.item(uint16_t(i)); it && it->shape == kSkull) skull = uint16_t(i);
        CHECK(skull);
        if (!skull) continue;
        CHECK_EQ(w.item(skull)->quality, 0);
        CHECK(w.item(w.item(skull)->parent) != nullptr);  // in the chest
        w.move_to_container(skull, w.equipped(1, 7));
        w.teleport_actor(1, 15519 + 64, 6143, 48, 50);
        u.frames(5);
        uint16_t slot = 0;
        for (unsigned i = 1; i < w.objects().slots() && !slot; ++i)
            if (const Item* it = w.item(uint16_t(i)); it && it->shape == kSlot) slot = uint16_t(i);
        CHECK(slot);
        int ax = w.avatar()->x, ay = w.avatar()->y, az = w.avatar()->z;
        CHECK_EQ(w.vm().get_global(kGlobalSlotDone, 1), 0);
        item_powers::Stats before = item_powers::stats();
        w.call_event(skull, kEvUse);
        CHECK(u.pick(slot));
        bool shook = false;
        for (int f = 0; f < 1500 && !w.vm().get_global(kGlobalSlotDone, 1); ++f) { u.frames(1); shook |= w.quake == 5; }
        u.frames(200);
        std::printf("  powers %s: G[16:1] %u, quake seen %d, q %u, Avatar back at %d,%d,%d (was %d,%d,%d) map %u\n",
                    powers ? "on" : "off", w.vm().get_global(kGlobalSlotDone, 1), int(shook), unsigned(w.item(skull)->quality),
                    w.avatar()->x, w.avatar()->y, w.avatar()->z, ax, ay, az, w.map_number_now());
        CHECK_EQ(w.vm().get_global(kGlobalSlotDone, 1), 1);   // the floor changes
        CHECK(shook);                                          // the scene's own quake...
        CHECK_EQ(w.quake, 0);                                  // ... which the scene ends
        CHECK_EQ(w.item(skull)->quality, 1);                   // the skull is spent
        CHECK(!w.item(skull)->parent);                         // and sits in the slot
        CHECK_EQ(w.map_number_now(), 50);
        CHECK_EQ(item_powers::stats().fired, before.fired);    // no power, no cooldown
        CHECK(w.power_cooldown.empty());
        CHECK(w.power_pending.empty());
    }
    item_powers::enabled() = true;
}

void test_hostiles_only(const GameData& d) {
    std::printf("the quake hurts hostile creatures only, within its radius\n");
    Ui u(d);
    World& w = u.w();
    u.avatar_to(kX, kY, 0);
    w.party->join(w.party->find_def("brannoc"));
    u.frames(120);
    uint16_t ally = w.party->state[size_t(w.party->find_def("brannoc"))].id;
    uint16_t near1 = spawn(u, 142, 200, 300, 0.3), near2 = spawn(u, 807, 200, 300, 2.0);  // ghoul, spider
    uint16_t far = spawn(u, 142, 600, 800, 4.0);                                           // beyond the radius
    uint16_t peasant = spawn(u, 707, 150, 300, 3.0);  // idle, table says "enemy of the Avatar"
    uint16_t guard = spawn(u, 574, 150, 300, 3.8);    // an idle guard
    uint16_t angry = spawn(u, 835, 150, 300, 1.2);    // a peasant the Avatar has angered: he is
    uint16_t devon = 2;
    int x, y, z;
    CHECK(free_spot(w, w.actor(devon)->shape, 120, 300, devon, x, y, z, 5.0));
    w.teleport_actor(devon, x, y, z, w.map_number_now());
    u.frames(5);
    Actor* foe1 = w.actor(near1); Actor* foe2 = w.actor(near2);
    CHECK(foe1 && foe2);
    foe1->hp = foe2->hp = 200;  // survive, so the amount can be read
    int far_hp = w.actor(far) ? w.actor(far)->hp : -1;
    int av_hp = w.avatar()->hp, ally_hp = w.actor(ally)->hp, devon_hp = w.actor(devon)->hp;
    int peasant_hp = peasant ? w.actor(peasant)->hp : 0;
    int str = w.avatar()->str, acc = w.stat_accumulator[0];
    CHECK(peasant && guard && angry);
    w.set_in_combat(angry);
    w.actor(angry)->target = 1;
    w.actor(angry)->hp = 200;
    int guard_hp = w.actor(guard)->hp;
    CHECK(w.turn->is_hostile(near1));
    CHECK(!w.turn->is_hostile(devon));
    CHECK(!w.turn->is_hostile(peasant));                   // idle townsfolk and guards never start or join a fight
    CHECK(!w.turn->is_hostile(guard));
    uint16_t skull = u.give_skull();
    w.call_event(skull, kEvUse);
    for (int i = 0; i < 10 && !w.target_pid; ++i) u.frames(1);
    CHECK(w.target_pid != 0);
    u.frames(1, true);  // (a click picks from what was painted)
    u.click(100, 110);  // a real click on the ground through the game mode
    u.frames(2);
    std::printf("  foes %d, %d (from 200); far %d (was %d); Avatar %d, ally %d, Devon %d, peasant %d\n", foe1->hp, foe2->hp,
                w.actor(far) ? w.actor(far)->hp : -1, far_hp, w.avatar()->hp, w.actor(ally)->hp, w.actor(devon)->hp,
                peasant ? w.actor(peasant)->hp : -1);
    CHECK(foe1->hp >= 200 - 25 && foe1->hp <= 200 - 10);
    CHECK(foe2->hp >= 200 - 25 && foe2->hp <= 200 - 10);
    CHECK_EQ(w.actor(far) ? w.actor(far)->hp : -1, far_hp);
    CHECK_EQ(w.avatar()->hp, av_hp);
    CHECK_EQ(w.actor(ally)->hp, ally_hp);
    CHECK_EQ(w.actor(devon)->hp, devon_hp);
    CHECK_EQ(w.actor(peasant)->hp, peasant_hp);            // idle townsfolk: unharmed, though "enemies" by table
    CHECK(!w.actor(peasant)->is(Actor::kInCombat));
    CHECK_EQ(w.actor(guard)->hp, guard_hp);                // an idle guard: unharmed
    CHECK(w.actor(angry)->hp <= 200 - 10);                 // a townsman fighting the Avatar: hit
    CHECK(!w.actor(devon)->is(Actor::kInCombat));          // he did not take offence
    CHECK_EQ(w.avatar()->str, str);                        // no strength training from a quake
    CHECK_EQ(w.stat_accumulator[0], acc);
    CHECK_EQ(w.quake, 5);                                  // the screen shakes...
    CHECK(w.sound.describe().find("48") != std::string::npos);  // ... with the rumble
    CHECK(log_has(w, "earthquake hits"));
    CHECK(log_has(w, "The earth heaves"));
    CHECK(w.kernel().count_exact(near1, kProcActorAnim));  // the hit reaction
    {
        unsigned puffs = 0, frame = 0;  // the dust at the foes' feet
        for (unsigned i = 1; i < w.objects().slots(); ++i)
            if (const Item* it = w.item(uint16_t(i)); it && it->shape == 578) { ++puffs; frame = it->frame; }
        std::printf("  dust sprites: %u (frame %u)\n", puffs, frame);
        CHECK(puffs >= 2);
    }
    u.frames(3, true);
    u.shot("skull_quake_shake");
    bool offset = false;
    for (int i = 0; i < 30; ++i) { u.frames(1); offset |= w.quake_offset[0] || w.quake_offset[1]; }
    CHECK(offset);
    u.frames(60);
    CHECK_EQ(w.quake, 0);                                  // and stops by itself
    CHECK_EQ(w.quake_offset[0], 0);
    u.shot("skull_quake_after");
    // A guard in combat with a monster is not the Avatar's foe (his AI would pick the target
    // next frame, so the quake comes at once); the same guard turning on the Avatar is.
    w.power_cooldown.clear();
    w.set_in_combat(guard);
    w.actor(guard)->target = near1;
    guard_hp = w.actor(guard)->hp;
    CHECK(item_powers::fire(w, skull_ability()));
    CHECK_EQ(w.actor(guard)->hp, guard_hp);
    w.power_cooldown.clear();
    w.actor(guard)->target = 1;
    w.actor(guard)->hp = 200;
    CHECK(item_powers::fire(w, skull_ability()));
    CHECK(w.actor(guard)->hp <= 200 - 10);
    // A kill goes through the normal death.
    Actor* f = w.actor(near1);
    f->hp = 1;
    w.power_cooldown.clear();
    CHECK(item_powers::fire(w, skull_ability()));
    u.frames(5);
    CHECK(w.actor(near1)->is(Actor::kDead));
    CHECK(log_has(w, "kills"));
}

void test_cooldown(const GameData& d) {
    std::printf("the cooldown: half an hour of play\n");
    Ui u(d);
    World& w = u.w();
    u.avatar_to(kX, kY, 0);
    uint16_t foe = spawn(u, 142, 200, 300);
    u.frames(5);
    w.actor(foe)->hp = 250;
    w.avatar()->set(Actor::kImmortal, true);  // the ghoul has half an hour here
    uint16_t skull = u.give_skull();
    w.call_event(skull, kEvUse);
    CHECK(u.pick(u.floor_item()));
    const item_powers::Ability& ab = skull_ability();
    CHECK_EQ(ab.cooldown_frames, 30u * 60 * 30);
    uint32_t left0 = item_powers::cooldown_left(w, ab);
    CHECK(left0 > 53990 && left0 <= 54000);
    int hp = w.actor(foe)->hp;
    CHECK(hp < 250);
    // Blocked: at the cursor (the slot must stay reachable), and at once for a spent skull.
    unsigned fired = item_powers::stats().fired;
    w.call_event(skull, kEvUse);
    CHECK(u.pick(u.floor_item()));
    CHECK(bark_has(w, 1, "still quiet"));
    CHECK(bark_has(w, 1, "30 minutes"));
    u.frames(5, true);
    u.shot("skull_quake_quiet");
    CHECK_EQ(item_powers::stats().fired, fired);
    CHECK_EQ(w.actor(foe)->hp, hp);
    w.close_bark(1);
    uint16_t spent = u.give_skull(1);
    CHECK_EQ(w.call_event(spent, kEvUse), 0);
    CHECK(!w.target_pid);
    CHECK(bark_has(w, 1, "still quiet"));
    w.close_bark(1);
    // The look shows the time left.
    u.frames(30);
    w.call_event(skull, kEvLook);
    u.frames(3);
    CHECK(bark_has(w, skull, "Skull of Quakes"));
    CHECK(bark_has(w, skull, "quiet for"));
    u.shot("skull_quake_look");
    w.close_bark(skull);
    // It counts play frames, and not while paused.
    uint32_t a = item_powers::cooldown_left(w, ab);
    u.frames(1800);
    uint32_t b = item_powers::cooldown_left(w, ab);
    std::printf("  1800 frames: %u -> %u\n", a, b);
    CHECK_EQ(a - b, 1800);
    w.kernel().pause();
    for (int i = 0; i < 100; ++i) w.frame();
    CHECK_EQ(item_powers::cooldown_left(w, ab), b);
    w.kernel().unpause();
    // Expires: ready again, no look text, fires.
    w.power_cooldown[ab.id] = 3;
    u.frames(4);
    CHECK_EQ(item_powers::cooldown_left(w, ab), 0);
    CHECK(w.power_cooldown.empty());
    w.call_event(skull, kEvLook);
    u.frames(3);
    CHECK(bark_has(w, skull, "Skull of Quakes"));
    CHECK(!bark_has(w, skull, "quiet for"));
    w.close_bark(skull);
    int hp2 = w.actor(foe)->hp;
    w.call_event(spent, kEvUse);  // a spent skull fires at once, without a cursor
    u.frames(2);
    CHECK(!w.target_pid);
    CHECK_EQ(item_powers::stats().fired, fired + 1);
    CHECK(w.actor(foe)->hp < hp2);
    CHECK(item_powers::cooldown_left(w, ab) > 53990);
    // Text.
    CHECK(item_powers::time_left_text(54000) == "30 minutes");
    CHECK(item_powers::time_left_text(1801) == "2 minutes");
    CHECK(item_powers::time_left_text(1800) == "less than a minute");
}

void test_save_load(const GameData& d) {
    std::printf("the cooldown survives save and load\n");
    Ui u(d);
    World& w = u.w();
    const item_powers::Ability& ab = skull_ability();
    // No cooldown: no section at all, the save is as it always was.
    {
        SaveWriter o;
        w.save_game(o, "plain");
        std::string s(o.data().begin(), o.data().end());
        CHECK(s.find("ITPW") == std::string::npos);
        CHECK(u.gm.save_slot(7, "Before"));
    }
    u.avatar_to(kX, kY, 0);
    uint16_t skull = u.give_skull();
    w.call_event(skull, kEvUse);
    CHECK(u.pick(u.floor_item()));
    CHECK(w.quake == 5);
    u.frames(5);
    // Saved in mid-shake.
    CHECK(u.gm.save_slot(8, "Shaking"));
    uint32_t left = item_powers::cooldown_left(w, ab);
    CHECK(left > 0);
    CHECK(u.gm.load_slot(8));
    CHECK_EQ(item_powers::cooldown_left(u.w(), ab), left);
    CHECK_EQ(u.w().quake, 5);
    u.frames(70);
    CHECK_EQ(u.w().quake, 0);                                  // the loaded shake ends too
    CHECK_EQ(item_powers::cooldown_left(u.w(), ab), left - 70);
    // Round trip.
    SaveWriter a, b;
    u.w().save_game(a, "x");
    SaveReader r(a.data().data(), a.data().size());
    CHECK(u.w().load_game(r));
    u.w().save_game(b, "x");
    CHECK(a.data() == b.data());
    // A save without the section (an old one): ready, not loaded with this game's cooldown.
    CHECK(u.gm.load_slot(7));
    CHECK(u.w().power_cooldown.empty());
    CHECK_EQ(u.w().power_quake, 0);
    // A new game: ready.
    CHECK(u.gm.load_slot(8));
    CHECK(!u.w().power_cooldown.empty());
    u.w().new_game();
    CHECK(u.w().power_cooldown.empty());
}

void test_turn_based(const GameData& d) {
    std::printf("turn-based: a Use item action that ends the turn\n");
    Ui u(d, true);
    World& w = u.w();
    u.avatar_to(kX, kY, 0);
    uint16_t skull = u.give_skull();
    uint16_t g = spawn(u, 142, 220, 320, 0.8);
    u.frames(5);
    w.actor(g)->hp = 300;
    u.key('c');
    for (int i = 0; i < 600 && !(u.t().awaiting_player() && u.t().current() == 1); ++i) u.frames(1);
    CHECK(u.t().active());
    CHECK(u.t().awaiting_player());
    std::vector<uint16_t> items = u.t().use_items(1);
    size_t idx = size_t(std::find(items.begin(), items.end(), skull) - items.begin());
    CHECK(idx < items.size());
    // Through the menu: Use item, down to the skull, Enter.
    u.gm.turn_ui().press_entry(TurnUI::kEUse);
    for (size_t k = 0; k < idx; ++k) u.key(kKeyDown);
    u.frames(2, true);
    u.shot("skull_quake_menu");
    u.key(kKeyReturn);
    CHECK(u.pick(u.floor_item()));
    std::printf("  ghoul hp %d; log: %s\n", w.actor(g)->hp, u.t().log.empty() ? "" : u.t().log.back().c_str());
    CHECK(w.actor(g)->hp >= 300 - 25 && w.actor(g)->hp <= 300 - 10);
    CHECK_EQ(item_powers::stats().hits > 0, 1);
    CHECK(!u.t().awaiting_player() || u.t().current() != 1);   // the turn is over
    u.frames(2, true);
    u.shot("skull_quake_turn");
    int turns0 = 0;
    for (uint16_t t : u.t().turn_history) turns0 += t == g;
    for (int i = 0; i < 2000 && !(u.t().active() && u.t().awaiting_player() && u.t().current() == 1); ++i) u.frames(1);
    int turns1 = 0;
    for (uint16_t t : u.t().turn_history) turns1 += t == g;
    CHECK(turns1 > turns0);                                     // the ghoul had its turn
    CHECK(u.t().active());
    // On cooldown the action is refused: the words, and no turn is lost.
    int hp = w.actor(g)->hp;
    CHECK(!u.t().cmd_use(skull));
    CHECK(u.t().status.find("still quiet") != std::string::npos);
    CHECK(u.t().awaiting_player());
    CHECK_EQ(w.actor(g)->hp, hp);
    CHECK(item_powers::display_name(w, *w.item(skull)).find("min)") != std::string::npos);
    u.gm.turn_ui().press_entry(TurnUI::kEUse);
    u.frames(2, true);
    u.shot("skull_quake_menu_cooldown");
    u.key(kKeyEscape);
    // Option off: not in the list.
    item_powers::enabled() = false;
    items = u.t().use_items(1);
    CHECK(std::find(items.begin(), items.end(), skull) == items.end());
    item_powers::enabled() = true;
}

void test_option_off(const GameData& d) {
    std::printf("the option off: the original game\n");
    item_powers::enabled() = false;
    Ui u(d);
    World& w = u.w();
    u.avatar_to(kX, kY, 0);
    uint16_t foe = spawn(u, 142, 200, 300);
    u.frames(5);
    int hp = w.actor(foe)->hp;
    uint16_t skull = u.give_skull();
    unsigned fired = item_powers::stats().fired;
    w.call_event(skull, kEvUse);
    CHECK(u.pick(u.floor_item()));   // its cursor, and nothing comes of it
    u.frames(5);
    CHECK_EQ(item_powers::stats().fired, fired);
    CHECK_EQ(w.actor(foe)->hp, hp);
    CHECK_EQ(w.quake, 0);
    CHECK(w.power_cooldown.empty());
    uint16_t spent = u.give_skull(1);
    w.call_event(spent, kEvUse);     // a spent skull: the usecode does nothing
    u.frames(5);
    CHECK(!w.target_pid);
    CHECK_EQ(item_powers::stats().fired, fired);
    item_powers::enabled() = true;
}

void test_data_file() {
    std::printf("the data file\n");
    std::vector<item_powers::Ability> builtin = item_powers::abilities();
    std::string err;
    CHECK(item_powers::load_file(g_data + "/items/abilities.json", err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    const std::vector<item_powers::Ability>& t = item_powers::abilities();
    CHECK_EQ(t.size(), 1);
    if (!t.empty()) {
        const item_powers::Ability& a = t[0], &b = builtin[0];
        CHECK(a.id == b.id && a.name == b.name && a.log_name == b.log_name && a.log_text == b.log_text);
        CHECK(a.quiet_text == b.quiet_text && a.sfx == b.sfx);
        CHECK(a.shape == b.shape && a.frame == b.frame && a.cooldown_frames == b.cooldown_frames);
        CHECK(a.radius == b.radius && a.height == b.height && a.damage_min == b.damage_min && a.damage_max == b.damage_max);
        CHECK(a.quake_strength == b.quake_strength && a.quake_frames == b.quake_frames && a.sfx_priority == b.sfx_priority);
        CHECK(a.original_quality == b.original_quality && a.original_target_shape == b.original_target_shape);
    }
    CHECK(!item_powers::load_text(R"({"abilities":[{"id":"x","shape":1,"effect":"nope"}]})", err));
    CHECK(!item_powers::load_text("not json", err));
    CHECK_EQ(item_powers::abilities().size(), 1);  // a bad file changes nothing
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_skull_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, void (*f)(const GameData&)) { if (only.empty() || only == n) f(d); };
    if (only.empty() || only == "data") test_data_file();
    run("original", test_original_use);
    run("hostiles", test_hostiles_only);
    run("cooldown", test_cooldown);
    run("save", test_save_load);
    run("turn", test_turn_based);
    run("off", test_option_off);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
