// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// v2.12 features and the restored undead invasion (NOTES.md "v2.12 features", "Restored content:
// the undead invasion"; specs/world/v212_features.md, specs/world/cut_undead_invasion.md):
// quick-save / quick-load, the B / R / K item keys (keyboard and controller), v2.12 stacking,
// the targeted jump (Classic controls) and its range, the recall rules, the help book's pages,
// the invasion's trigger, clear, eggs, conversations and quakes.
//
//   pagan_v212_tests GAME_DIR DATA_DIR [SHOTS_DIR|""] [SAVE_DIR] [only]
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "core/variants.h"
#include "data/gamedata.h"
#include "game/gumps.h"
#include "game/savegame.h"
#include "game/variants.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "usecode/uc_machine.h"
#include "usecode/uc_patches.h"

using namespace pg;
int g_fail = 0, g_pass = 0;

namespace {
std::string g_saves, g_data, g_shots;
enum : unsigned { kJumpUp = 3, kShakeHead = 11, kJump = 17 };

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        gm.start();
        frames(30);
    }
    World& w() { return gm.world(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e; e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp; e.button = b;
        e.x = gm.mouse_x(); e.y = gm.mouse_y(); ev(e);
    }
    void key(int k, unsigned mods = 0) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; e.mods = mods; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b, bool down) { InputEvent e; e.type = down ? InputEvent::kPadDown : InputEvent::kPadUp; e.button = b; e.pad = 7; ev(e); }
    void pad_tap(int b) { pad(b, true); pad(b, false); }
    void pad_added() {
        InputEvent e; e.type = InputEvent::kPadAdded; e.pad = 7; std::strcpy(e.name, "test pad"); ev(e);
        pad_tap(kPadRight);  // the first press only wakes a new controller
    }
    unsigned g(unsigned pos, unsigned n = 1) { return w().vm().get_global(pos, n); }
    void set(unsigned pos, unsigned n, unsigned v) { w().vm().set_global(pos, n, uint16_t(v)); }
    void warp(int x, int y, int z, unsigned map) { w().teleport_actor(1, x, y, z, map); frames(20); }
    uint16_t give(unsigned shape, unsigned frame = 0, unsigned qty = 0, uint16_t into = 0) {
        uint16_t id = w().create_item(shape, frame);
        if (Item* it = w().item(id); it && qty) it->quality = uint16_t(qty);
        bool ok = into ? w().move_to_container(id, into) : w().give_to_avatar(id);
        if (!ok) { w().destroy_now(id); return 0; }
        return id;
    }
    // Every line said by anyone over n frames (speech hurried along), plus whether a menu came.
    std::string listen(int n, bool* menu = nullptr) {
        std::string all;
        for (int f = 0; f < n; ++f) {
            for (Bark& b : w().barks) {
                for (const TextPage& p : b.pages)
                    for (const std::string& l : p.lines)
                        if (all.find(l) == std::string::npos) all += l + " ";
                b.ticks_left = std::min(b.ticks_left, 2);
            }
            if (menu && w().ask_menu && !w().ask_menu->answers.empty()) *menu = true;
            frames(1);
        }
        return all;
    }
    std::vector<std::string> menu() {
        std::vector<std::string> out;
        if (w().ask_menu) for (uint16_t sid : w().ask_menu->answers) out.push_back(w().vm().string(sid));
        return out;
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        mkdir(g_shots.c_str(), 0755);
        std::vector<uint32_t> rgba;
        int fw, fh;
        gm.render_frame(rgba, fw, fh, 3, 0.0, 1.0 / 30);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), fw, fh, 1);
        std::printf("  shot %s\n", path.c_str());
    }
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// --- Quick-save / quick-load (F7 / F8, LB + D-pad on the controller) ---------------------------
void test_quicksave(const GameData& d) {
    std::printf("quicksave: F7 / F8 round trip, refusals, the diary's autosave page, the controller\n");
    { SaveStore st(g_saves); std::remove(st.path(SaveStore::kQuickSlot).c_str()); }
    Ui u(d);
    World& w = u.w();
    // F8 with nothing saved: the red cross, nothing loaded.
    u.key(kKeyF8);
    CHECK(w.red_cross_frames > 0);
    CHECK(!u.gm.saves().exists(SaveStore::kQuickSlot));
    u.frames(10);
    const Actor* av = w.avatar();
    int x0 = av->x, y0 = av->y, z0 = av->z;
    u.set(300, 1, 1);
    u.key(kKeyF7);
    CHECK(u.gm.saves().exists(SaveStore::kQuickSlot));
    std::printf("  quick-saved: \"%s\"\n", u.gm.saves().description(SaveStore::kQuickSlot).c_str());
    CHECK(contains(u.gm.saves().description(SaveStore::kQuickSlot), "Quick save"));
    CHECK_EQ(u.gm.saves().latest(), SaveStore::kQuickSlot);  // the start menu's Continue
    // Change things, then F8 puts them back.
    u.set(300, 1, 0);
    w.teleport_actor(1, x0 + 300, y0 + 200, z0, w.map_number_now());
    u.frames(5);
    CHECK(std::abs(w.avatar()->x - x0) > 100);
    u.key(kKeyF8);
    u.frames(3);
    CHECK_EQ(u.g(300), 1);
    CHECK(std::abs(w.avatar()->x - x0) < 16 && std::abs(w.avatar()->y - y0) < 16);
    std::printf("  quick-loaded: Avatar at %d,%d (saved at %d,%d), G[300:1] = %u\n", w.avatar()->x, w.avatar()->y, x0, y0, u.g(300));
    // Refused in stasis (conversations, scenes) and while a modal window is open.
    SaveStore& st = u.gm.saves();
    std::vector<uint8_t> before, after;
    st.read(SaveStore::kQuickSlot, before);
    w.red_cross_frames = 0;
    w.stasis = true;
    u.set(300, 1, 0);
    CHECK(!u.gm.quick_save());
    CHECK(w.red_cross_frames > 0);
    w.stasis = false;
    w.gumps.add(std::make_unique<QuitGump>(w));
    CHECK(!u.gm.quick_save());
    w.gumps.close_all();
    w.gumps.reap();
    st.read(SaveStore::kQuickSlot, after);
    CHECK(before == after);
    // The diary's autosave page lists it (right column, last entry) and it loads from there.
    MenuGump* m = w.open_menu(MenuGump::kLoad);
    u.frames(2);
    CHECK_EQ(m->spread, -1);
    std::vector<int> slots = m->spread_slots();
    CHECK(std::find(slots.begin(), slots.end(), SaveStore::kQuickSlot) != slots.end());
    u.frames(70);  // (the toast fades)
    u.shot("v212_diary_quicksave");
    w.gumps.close_all();
    w.gumps.reap();
    u.frames(2);
    // The controller: LB held + D-pad Up saves, + Down loads; LB alone still opens the paperdoll.
    u.pad_added();
    std::remove(st.path(SaveStore::kQuickSlot).c_str());
    st.refresh();
    u.set(300, 1, 1);
    u.pad(kPadLB, true);
    u.pad_tap(kPadUp);
    u.pad(kPadLB, false);
    u.frames(2);
    CHECK(st.exists(SaveStore::kQuickSlot));
    CHECK(w.gumps.container_gump(1) == nullptr);  // (the LB press was used: no paperdoll)
    u.set(300, 1, 0);
    u.pad(kPadLB, true);
    u.pad_tap(kPadDown);
    u.pad(kPadLB, false);
    u.frames(2);
    CHECK_EQ(u.g(300), 1);
    std::printf("  controller: LB + Up saved, LB + Down loaded\n");
}

// --- B / R / K (and LB + B / Y / X) --------------------------------------------------------------
void test_item_keys(const GameData& d) {
    std::printf("item keys: B / R / K use the first bedroll / recall item / keyring, nested too\n");
    Ui u(d);
    World& w = u.w();
    uint16_t pack = w.equipped(1, 7);
    CHECK(pack != 0);
    // A keyring loose in the backpack after a bag that holds another: the bag's comes first.
    uint16_t bag = u.give(637);  // a bag
    uint16_t inner = u.give(GameMode::kKeyring, 0, 0, bag);
    uint16_t outer = u.give(GameMode::kKeyring);
    std::printf("  bag %u, keyring in the bag %u, keyring in the backpack %u\n", bag, inner, outer);
    CHECK(inner && outer);
    w.red_cross_frames = 0;
    uint16_t used = u.gm.use_first_item(GameMode::kKeyring);
    u.frames(3);
    const Item* bi = w.item(bag);
    bool bag_first = bi && w.item(outer) &&
                     std::find(w.item(pack)->contents.begin(), w.item(pack)->contents.end(), bag) <
                         std::find(w.item(pack)->contents.begin(), w.item(pack)->contents.end(), outer);
    CHECK_EQ(used, bag_first ? inner : outer);
    CHECK(w.target_pid != 0);  // the keyring's targeting cursor
    std::printf("  K: used %u, the target cursor is %s\n", used, w.target_pid ? "up" : "not up");
    u.key(kKeyEscape);
    u.frames(3);
    // R: the recall item asks for a destination (Central Tenebrae's waypoint set).
    u.set(68, 1, 1);
    uint16_t rec = u.give(GameMode::kRecall);
    CHECK(rec != 0);
    u.key('r');
    bool menu = false;
    u.listen(90, &menu);
    std::vector<std::string> answers = u.menu();
    CHECK(menu);
    CHECK(std::find_if(answers.begin(), answers.end(), [](const std::string& a) { return contains(a, "Central Tenebrae"); }) != answers.end());
    u.shot("v212_recall_menu");
    for (size_t i = 0; i < answers.size(); ++i) if (contains(answers[i], "Nowhere")) w.choose_answer(unsigned(i));
    u.listen(60);
    // Shift+R is roof hiding now (and does not use the recall item).
    u.key('r', kModShift);
    bool menu2 = false;
    u.listen(30, &menu2);
    CHECK(!menu2);
    // B: the bedroll's use event.
    uint16_t bed = u.give(GameMode::kBedroll);
    CHECK(bed != 0);
    CHECK_EQ(u.gm.use_first_item(GameMode::kBedroll), bed);
    u.listen(200);
    // In stasis nothing happens; with none, nothing either.
    w.stasis = true;
    CHECK_EQ(u.gm.use_first_item(GameMode::kRecall), 0);
    w.stasis = false;
    w.destroy_now(rec);
    CHECK_EQ(u.gm.use_first_item(GameMode::kRecall), 0);
    // The controller: LB + X keyring (B bedroll, Y recall).
    u.frames(5);
    u.pad_added();
    w.red_cross_frames = 0;
    u.pad(kPadLB, true);
    u.pad_tap(kPadX);
    u.pad(kPadLB, false);
    u.frames(3);
    CHECK(w.target_pid != 0);
    std::printf("  controller LB + X: the keyring's target cursor %s\n", w.target_pid ? "up" : "not up");
    u.key(kKeyEscape);
}

// --- The recall rules ------------------------------------------------------------------------
struct RecallMap { unsigned map; int x, y, z; bool v210, v212; const char* name; };

void test_recall(const GameData& d) {
    std::printf("recall: where the recall item works, v2.10 vs v2.12\n");
    const RecallMap maps[] = {
        {40, 13038, 15042, 48, true, true, "Central Tenebrae"},
        {12, 5727, 18815, 96, false, true, "Daemon's Crag"},
        {14, 15935, 2303, 8, false, false, "Obsidian Fortress"},
        {24, 20287, 24191, 64, false, true, "lava river tunnel"},
        {62, 1695, 14655, 48, false, true, "Ancient Necromancers"},
        {43, 16767, 17279, 16, false, false, "Ethereal Plane"},
        {46, 13951, 7455, 176, false, false, "Plane of Air"},
    };
    for (bool v212 : {false, true}) {
        v212_changes() = v212;
        for (const RecallMap& m : maps) {
            Ui u(d);
            u.set(68, 1, 1);
            u.warp(m.x, m.y, m.z, m.map);
            u.give(GameMode::kRecall);
            u.w().stasis = false;
            u.gm.use_first_item(GameMode::kRecall);
            bool menu = false;
            u.listen(60, &menu);
            bool want = v212 ? m.v212 : m.v210;
            std::printf("  %s map %2u %-20s: %s\n", v212 ? "v2.12" : "v2.10", m.map, m.name, menu ? "destination menu" : "nothing");
            CHECK_EQ(menu, want);
        }
    }
    v212_changes() = true;
}

// --- v2.12 stacking --------------------------------------------------------------------------
void test_stacking(const GameData& d) {
    std::printf("stacking: a reagent put into a container joins the stack there (v2.12)\n");
    for (bool v212 : {false, true}) {
        v212_changes() = v212;
        Ui u(d);
        World& w = u.w();
        uint16_t pack = w.equipped(1, 7);
        uint16_t a = u.give(395, 0, 5);   // necromancy reagents, frame 0 (blood)
        uint16_t elbow = u.give(395, 20, 1);
        uint16_t b = w.create_item(395, 3);  // same kind (frames 0-5)
        w.item(b)->quality = 4;
        CHECK(w.move_to_container(b, pack));
        uint16_t into = GameMode::auto_merge(w, b);
        if (v212) {
            CHECK_EQ(into, a);
            u.frames(2);  // (the emptied stack goes through a destroy process)
            CHECK(w.item(b) == nullptr);
            CHECK_EQ(w.item(a)->quality, 9);
            CHECK_EQ(w.item(a)->frame, 0);  // the receiving stack keeps its frame
        } else {
            CHECK_EQ(into, 0);
            CHECK_EQ(w.item(a)->quality, 5);
        }
        // A wooden stick never joins the dead man's elbow (v2.12 #11).
        uint16_t stick = w.create_item(395, 17);
        w.item(stick)->quality = 2;
        CHECK(w.move_to_container(stick, pack));
        CHECK_EQ(GameMode::auto_merge(w, stick), 0);
        CHECK_EQ(w.item(elbow)->quality, 1);
        // 666 at most: the rest stays a stack of its own.
        if (v212) {
            uint16_t c = u.give(395, 7, 660, pack);  // (past what he may carry: no weight check here)
            uint16_t e = w.create_item(395, 6);
            w.item(e)->quality = 10;
            CHECK(w.move_to_container(e, pack));
            CHECK_EQ(GameMode::auto_merge(w, e), c);
            CHECK_EQ(w.item(c)->quality, 666);
            CHECK(w.item(e) && w.item(e)->quality == 4);
        }
        std::printf("  v2.12 %s: merged into %u\n", v212 ? "on " : "off", into);
    }
    v212_changes() = true;
}

// --- The targeted jump (Classic) and its range ------------------------------------------------
// Both buttons on a ground spot ~ (sdx, sdy) screen pixels from the Avatar; returns the action
// played and where he ended.
struct JumpResult { unsigned action = 0; int moved = 0, miss = 0; bool target_ok = false; };
JumpResult both_buttons(const GameData& d, bool responsive, int str, int sdx, int sdy) {
    Ui u(d);
    World& w = u.w();
    w.controls.responsive = responsive;
    w.avatar()->str = int8_t(str);
    u.frames(10);
    int x0 = w.avatar()->x, y0 = w.avatar()->y;
    u.move(160 + sdx, 114 + sdy);
    u.frames(2);
    int tp[3];
    std::copy(w.input.cursor_point, w.input.cursor_point + 3, tp);
    JumpResult r;
    r.target_ok = w.input.cursor_item != 0;
    u.button(1, true);
    u.button(3, true);
    unsigned seen = 0;
    for (int f = 0; f < 60; ++f) {
        u.frames(1);
        if (f == 1) { u.button(1, false); u.button(3, false); }  // a click of both, not a held walk
        unsigned la = w.avatar()->last_anim;
        if (!seen && (la == kJumpUp || la == kShakeHead || la == kJump)) seen = la;
    }
    r.action = seen;
    r.moved = int(std::hypot(double(w.avatar()->x - x0), double(w.avatar()->y - y0)));
    // (the jump aims his footprint's middle at the cursor point)
    Box bx = w.box_of(*w.avatar());
    r.miss = int(std::hypot(double(bx.x - bx.xd / 2 - tp[0]), double(bx.y - bx.yd / 2 - tp[1])));
    return r;
}

void test_targeted_jump(const GameData& d) {
    std::printf("targeted jump: Classic + v2.12 jumps to the spot (STR x 32 range), v2.10 plain jump, Responsive forward\n");
    v212_changes() = true;
    JumpResult a = both_buttons(d, false, 20, 50, 20);
    std::printf("  Classic v2.12, STR 20, in range: action %u, moved %d, %d from the spot\n", a.action, a.moved, a.miss);
    CHECK_EQ(a.action, kJumpUp);
    CHECK(a.moved > 60);
    CHECK(a.miss < 48);
    JumpResult b = both_buttons(d, false, 2, 50, 20);  // range 64: the spot is out of reach
    std::printf("  Classic v2.12, STR 2, out of range: action %u, moved %d\n", b.action, b.moved);
    CHECK_EQ(b.action, kShakeHead);
    CHECK(b.moved < 8);
    v212_changes() = false;
    JumpResult c = both_buttons(d, false, 20, 50, 20);
    std::printf("  Classic v2.10: action %u, moved %d\n", c.action, c.moved);
    CHECK_EQ(c.action, kJump);
    v212_changes() = true;
    JumpResult e = both_buttons(d, true, 2, 50, 20);  // Responsive: forward, never the head shake
    std::printf("  Responsive: action %u, moved %d\n", e.action, e.moved);
    CHECK(e.action != kShakeHead);
    CHECK(e.action == kJumpUp);
}

// --- The help book pages ---------------------------------------------------------------------
void test_help(const GameData& d) {
    std::printf("help: every section fits on its pages; the new keys are listed\n");
    Ui u(d);
    int n = u.gm.help_pages();
    std::printf("  %d pages\n", n);
    CHECK(n > 4);
    CHECK_EQ(u.gm.help_overflows(), 0);
    u.gm.open_help();
    for (int p = 0; p < n; ++p) {
        u.frames(1);
        u.shot("v212_help_" + std::to_string(p + 1));
        u.key(kKeyRight);
    }
    u.frames(1);
    CHECK(u.gm.help_open());  // wrapped round to the first page
    u.key(kKeyEscape);
    CHECK(!u.gm.help_open());
}

// --- The undead invasion ---------------------------------------------------------------------
void test_invasion_trigger(const GameData& d) {
    std::printf("invasion: G[58:1] set by the Heart of Earth in the inventory, cleared once absorbed / placed\n");
    for (bool on : {false, true}) {
        restored_invasion() = on;
        Ui u(d);
        World& w = u.w();
        u.frames(20);
        CHECK_EQ(u.g(58), 0);
        // The real Heart: on map 57 at (25280, 29096, 0), taken into a bag in the backpack.
        u.warp(30495, 28127, 48, 57);
        uint16_t heart = 0;
        for (unsigned i = 1; i < w.objects().slots(); ++i)
            if (Item* it = w.item(uint16_t(i)); it && it->shape == 442 && it->frame == 1) heart = uint16_t(i);
        CHECK(heart != 0);
        uint16_t bag = u.give(637);
        CHECK(w.move_to_container(heart, bag));
        u.frames(20);
        std::printf("  option %s: Heart %u in the bag, G[58:1] = %u\n", on ? "on " : "off", heart, u.g(58));
        CHECK_EQ(u.g(58), on ? 1 : 0);
        if (!on) continue;
        // Dropped again, the flag stays (the theft has happened).
        w.move_to(heart, w.avatar()->x + 64, w.avatar()->y, w.avatar()->z);
        u.frames(20);
        CHECK_EQ(u.g(58), 1);
        CHECK(w.move_to_container(heart, bag));
        // Lithos absorbed: the Heart becomes 793:1; cleared, and not set again.
        w.item(heart)->shape = 793;
        u.frames(20);
        CHECK_EQ(u.g(58), 0);
        u.frames(40);
        CHECK_EQ(u.g(58), 0);
        // Placed on the pentagram (G[569:1]) with a 442:1 still carried (a stray copy): not set.
        w.item(heart)->shape = 442;
        u.set(569, 1, 1);
        u.frames(20);
        CHECK_EQ(u.g(58), 0);
        u.set(569, 1, 0);
        u.frames(20);
        CHECK_EQ(u.g(58), 1);
        // Switching the option Off clears it at once (through the options row).
        MenuGump* m = w.open_menu(MenuGump::kOptions);
        std::vector<MenuGump::OptionRow> rows = m->option_rows();
        int row = -1;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].label == "Undead invasion:") row = int(i);
        CHECK(row >= 0);
        if (row >= 0) {
            CHECK(rows[size_t(row)].group == "Game");
            u.shot("invasion_option");
            m->change_option(row, 1);
            CHECK(!restored_invasion());
            CHECK_EQ(u.g(58), 0);
            u.frames(20);
            CHECK_EQ(u.g(58), 0);
            m->change_option(row, 1);
            CHECK(restored_invasion());
            u.frames(20);
            CHECK_EQ(u.g(58), 1);  // still carrying it: set again
        }
        w.gumps.close_all();
        w.gumps.reap();
        // A save with the flag, loaded with the option Off: cleared on load.
        CHECK(u.gm.save_slot(50, "invasion"));
        Config::global().set_bool("restore_invasion", false);
        CHECK(u.gm.load_slot(50));
        CHECK_EQ(u.g(58), 0);
        Config::global().set_bool("restore_invasion", true);
        restored_invasion() = true;
        CHECK(u.gm.load_slot(50));
        CHECK_EQ(u.g(58), 1);
    }
    restored_invasion() = true;
}

// The spawning eggs: guards and peasants (G[58] = 0) or ghouls (G[58] = 1).
struct EggVisit { unsigned map; int x, y, z; };
void count_spawns(Ui& u, const std::vector<EggVisit>& eggs, int& guards, int& ghouls, int& peasants, int& hostile) {
    World& w = u.w();
    guards = ghouls = peasants = hostile = 0;
    for (const EggVisit& e : eggs) {
        u.warp(e.x + 64, e.y + 64, e.z, e.map);
        u.frames(20);
        for (unsigned i = 2; i < 256; ++i) {
            const Actor* a = w.actor(uint16_t(i));
            if (!a || a->chunk < 0 || a->is(Actor::kDead)) continue;
            if (std::abs(a->x - e.x) > 200 || std::abs(a->y - e.y) > 200) continue;
            if (!(a->flags & 0x80) && a->shape != 142) continue;  // FAST_ONLY: spawned by the egg
            if (a->shape == 574) ++guards;
            if (a->shape == 142) { ++ghouls; if (a->is(Actor::kInCombat)) ++hostile; }
            if (a->shape == 836) ++peasants;
        }
        // (away, so the egg can fire again on the next visit)
        u.warp(e.x + 3000 < 32000 ? e.x + 3000 : e.x - 3000, e.y, e.z, e.map);
    }
}

void test_invasion_eggs(const GameData& d) {
    std::printf("invasion: Tenebrae's guard eggs make ghouls, the peasant eggs nothing\n");
    const std::vector<EggVisit> eggs = {
        {39, 18015, 5727, 64}, {39, 14600, 7136, 48}, {39, 12063, 10335, 64}, {39, 15135, 11807, 64}, {39, 10271, 12447, 64},
        {39, 16991, 13631, 64}, {39, 12415, 15295, 64}, {39, 18388, 15412, 48}, {39, 15487, 15999, 64},
        {41, 17343, 4191, 48}, {41, 22044, 8204, 48}, {41, 17972, 9700, 48}, {41, 13951, 11903, 48}, {41, 17404, 11788, 48},
        {41, 20415, 16863, 48}, {41, 14975, 18367, 48},
        {41, 13306, 3102, 48}, {41, 19519, 5183, 48}, {41, 21714, 10478, 48}, {41, 14143, 13375, 48}, {41, 12822, 16722, 48},
        {41, 15282, 16854, 48},
    };
    int gu[2], gh[2], pe[2], ho[2];
    for (int on = 0; on < 2; ++on) {
        Ui u(d);
        u.set(58, 1, unsigned(on));
        count_spawns(u, eggs, gu[on], gh[on], pe[on], ho[on]);
        std::printf("  G[58:1] = %d: %d guards, %d ghouls (%d fighting), %d peasants at %zu eggs\n", on, gu[on], gh[on], ho[on],
                    pe[on], eggs.size());
        // A ghoul in the streets, for the shot: the eggs again until one is on the screen.
        if (on && gh[on] > 0 && !g_shots.empty()) {
            bool shot = false;
            for (int round = 0; round < 12 && !shot; ++round)
                for (const EggVisit& e : eggs) {
                    u.w().avatar()->hp = 60;
                    u.warp(e.x + 64, e.y + 64, e.z, e.map);
                    u.frames(15);
                    for (unsigned i = 2; i < 256 && !shot; ++i)
                        if (const Actor* a = u.w().actor(uint16_t(i)); a && a->shape == 142 && a->chunk >= 0 &&
                            std::abs(a->x - u.w().avatar()->x) < 300 && std::abs(a->y - u.w().avatar()->y) < 300) {
                            u.w().move_to(1, a->x + 96, a->y + 8, a->z);  // (not on top of it)
                            u.frames(2);
                            u.shot("invasion_ghoul");
                            shot = true;
                        }
                    if (shot) break;
                    u.warp(e.x + 3000 < 32000 ? e.x + 3000 : e.x - 3000, e.y, e.z, e.map);
                }
            std::printf("  ghoul on screen: %s\n", shot ? "yes" : "no");
        }
    }
    CHECK(gu[0] > 0);
    CHECK(pe[0] > 0);
    CHECK_EQ(gh[0], 0);
    CHECK_EQ(gu[1], 0);
    CHECK_EQ(pe[1], 0);
    CHECK(gh[1] > 0);
}

// The conversations: who refuses, who still talks; the critical-path NPCs after the Heart.
struct Talk { uint16_t npc; const char* name; const char* line; bool menu; };

void test_invasion_talk(const GameData& d) {
    std::printf("invasion: the townsfolk's lines with G[58:1] set; Devon, Vividos and Mythran still talk\n");
    const Talk talks[] = {
        {2, "Devon", "ghouls who seem to have begun wandering", true},
        {8, "Tarna", "walking dead march upon the city", false},
        {13, "Aramina", "another dead one", false},
        {19, "Shaana", "living dead walk the streets", false},
        {33, "Mythran", "", true},
        {5, "Darion", "emergency situation", true},
        {24, "Jenna", "", true},
        {11, "Guard 11", "", false}, {45, "Guard 45", "", false}, {46, "Guard 46", "", false}, {47, "Guard 47", "", false},
        {48, "Guard 48", "", false}, {49, "Guard 49", "", false}, {50, "Guard 50", "", false}, {51, "Guard 51", "", false},
        {59, "Guard 59", "", false},
    };
    for (const Talk& t : talks) {
        Ui u(d);
        World& w = u.w();
        u.set(58, 1, 1);
        u.set(89, 1, 1);  // past the execution, and (on the road, map 3) past the procession:
        u.set(86, 1, 1);  // the town's usual talk
        const Actor* a = w.actor(t.npc);
        if (!a) { std::printf("  %s: no such NPC\n", t.name); CHECK(false); continue; }
        // In the middle of Central Tenebrae (at the start many stand on the road for the
        // execution, where they have other lines).
        u.warp(13038, 15042, 48, 40);
        w.teleport_actor(t.npc, w.avatar()->x + 96, w.avatar()->y, w.avatar()->z, 40);
        u.frames(5);
        w.stasis = false;
        w.call_event(t.npc, kEvUse);
        bool menu = false;
        std::string said = u.listen(240, &menu);
        std::vector<std::string> ans = u.menu();
        std::printf("  %-8s (map %u): %s%s\n           \"%.150s\"\n", t.name, unsigned(a->mapnum), menu ? "menu: " : "no menu",
                    menu && !ans.empty() ? ans.front().c_str() : "", said.c_str());
        if (*t.line) CHECK(contains(said, t.line));
        if (t.menu) CHECK(menu);
        if (t.npc == 2) u.shot("invasion_devon");
        // Jenna's short exchange: does it reach her bar?
        if (t.npc == 24 && menu) {
            w.choose_answer(0);
            bool again = false;
            std::string more = u.listen(200, &again);
            std::vector<std::string> ans2 = u.menu();
            std::printf("           then: %s \"%.120s\" menu: %s\n", again ? "menu" : "no menu", more.c_str(),
                        ans2.empty() ? "" : ans2.front().c_str());
        }
    }
    // Vividos: his usual conversation, then the accusation.
    {
        Ui u(d);
        World& w = u.w();
        u.set(58, 1, 1);
        const Actor* a = w.actor(21);
        CHECK(a != nullptr);
        if (a) {
            u.warp(a->x + 96, a->y, a->z, a->mapnum);
            w.auto_ui = true;
            World::auto_ui_given().clear();
            w.call_event(21, kEvUse);
            std::string said = u.listen(3000);
            std::printf("  Vividos: %s\n", contains(said, "You stole the Heart of Earth") ? "accuses the Avatar" : "no accusation");
            CHECK(contains(said, "stole the Heart of Earth"));
            w.auto_ui = false;
        }
    }
}

// The critical path's Tenebrae steps a player may still have open after taking the Heart
// (critical_path.md 81-85) go through Devon and Mythran: talked through with the flag clear and
// set, in several story states, they offer the same answers (auto_ui asks every one of them).
void test_invasion_critical(const GameData& d) {
    std::printf("invasion: Devon's and Mythran's conversations are the same with G[58:1] set\n");
    for (uint16_t npc : {uint16_t(2), uint16_t(33), uint16_t(21)})
        for (int story = 0; story < 3; ++story) {
            std::vector<std::string> given[2];
            for (int on = 0; on < 2; ++on) {
                Ui u(d);
                World& w = u.w();
                u.set(89, 1, 1);
                if (story >= 1) { u.set(86, 1, 1); u.set(65, 1, 1); }
                if (story >= 2) u.set(304, 1, 1);
                u.warp(13038, 15042, 48, 40);
                w.teleport_actor(npc, w.avatar()->x + 96, w.avatar()->y, w.avatar()->z, 40);
                u.set(58, 1, unsigned(on));
                u.frames(5);
                w.auto_ui = true;
                World::auto_ui_given().clear();
                w.stasis = false;
                w.call_event(npc, kEvUse);
                u.listen(3000);
                given[on] = World::auto_ui_given();
                w.auto_ui = false;
            }
            std::printf("  npc %u, story state %d: %zu / %zu answers\n", unsigned(npc), story, given[0].size(), given[1].size());
            CHECK(!given[0].empty());
            CHECK(given[0] == given[1]);
        }
}

void test_invasion_quake(const GameData& d) {
    std::printf("invasion: earthquakes in Tenebrae now and then; never saved\n");
    Ui u(d);
    World& w = u.w();
    u.warp(13038, 15042, 48, 40);
    u.set(58, 1, 1);
    int quake_frames = 0, max_run = 0, run = 0;
    bool saved = false;
    for (int f = 0; f < invasion::kQuakePeriod + 200; ++f) {
        u.frames(1);
        if (w.quake > 0) { ++quake_frames; ++run; max_run = std::max(max_run, run); } else run = 0;
        if (w.quake > 0 && run == 20 && !saved) {
            u.shot("invasion_quake");
            SaveWriter out;
            w.save_game(out, "quake");
            CHECK(u.gm.save_slot(51, "quake"));
            saved = true;
        }
    }
    std::printf("  %d quake frames in %d (longest %d)\n", quake_frames, invasion::kQuakePeriod + 200, max_run);
    CHECK(quake_frames > 0);
    CHECK(max_run <= invasion::kQuakeFrames);
    CHECK(saved);
    CHECK(u.gm.load_slot(51));
    CHECK_EQ(w.quake, 0);
    // Not outside Tenebrae, not with the flag clear.
    Ui v(d);
    v.set(58, 1, 0);
    v.warp(13038, 15042, 48, 40);
    int q = 0;
    for (int f = 0; f < invasion::kQuakePeriod + 10; ++f) { v.frames(1); q += v.w().quake > 0; }
    CHECK_EQ(q, 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_v212_saves";
    std::string only = argc > 5 ? argv[5] : "";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    std::string err;
    CHECK(load_story_patches(d.usecode, g_data + "/patches", err));
    if (!err.empty()) std::printf("patches: %s\n", err.c_str());
    register_variant_options();
    auto run = [&](const char* n, auto f) { if (only.empty() || only == n) f(); };
    run("quicksave", [&] { test_quicksave(d); });
    run("keys", [&] { test_item_keys(d); });
    run("recall", [&] { test_recall(d); });
    run("stacking", [&] { test_stacking(d); });
    run("jump", [&] { test_targeted_jump(d); });
    run("help", [&] { test_help(d); });
    run("trigger", [&] { test_invasion_trigger(d); });
    run("eggs", [&] { test_invasion_eggs(d); });
    run("talk", [&] { test_invasion_talk(d); });
    run("critical", [&] { test_invasion_critical(d); });
    run("quake", [&] { test_invasion_quake(d); });
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
