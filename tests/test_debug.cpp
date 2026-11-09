// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Debug menu, critical path and autosave tests (app/debug_tools.*, game/critpath.*,
// game/debug_menu.*): the critical-path evaluator on the sample file with a fake world
// (conditions, order, debug overrides, routing through map links, NPC targets followed live);
// the debug menu through the game mode's input (only in debug mode; keys, mouse, controller;
// toggles, pause / step / speed, warps, give, heal, time, noclip); the real data file if it is
// there; a headless play through the execution with the autosave it triggers; autosave
// rotation, the Off setting, debug-forced steps; death -> the death menu -> load last autosave.
//   pagan_debug_tests GAME_DIR SAMPLE_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR]
#include <array>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/critpath.h"
#include "game/debug_menu.h"
#include "game/game_log.h"
#include "game/savegame.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_sample, g_data;

// ---------------------------------------------------------------------------------------------
// A fake world for the evaluator.

struct FakeCtx : CritContext {
    std::map<unsigned, int> g;
    std::map<std::pair<unsigned, int>, int> items;  // (shape, frame) -> count
    std::set<unsigned> equip, visited, dead;
    std::map<std::string, int> gone;
    unsigned map = 3;
    int ax = 14527, ay = 5887, az = 8;
    std::map<unsigned, std::array<int, 4>> npcs;  // map, x, y, z
    int global(unsigned pos, unsigned n) const override { auto it = g.find(pos); return it == g.end() ? 0 : it->second; }
    int item_count(unsigned shape, int frame, int quality) const override {
        int n = 0;
        for (const auto& kv : items)
            if (kv.first.first == shape && (frame < 0 || kv.first.second == frame)) n += kv.second;
        return n;
    }
    bool equipped(unsigned shape) const override { return equip.count(shape) != 0; }
    unsigned current_map() const override { return map; }
    bool visited_map(unsigned m) const override { return visited.count(m) != 0; }
    bool npc_dead(unsigned n) const override { return dead.count(n) != 0; }
    int item_gone(unsigned shape, unsigned m, int x, int y, int z) const override {
        auto it = gone.find(std::to_string(shape));
        return it == gone.end() ? 0 : it->second;
    }
    bool spell_known(unsigned) const override { return false; }
    bool npc_location(unsigned n, unsigned& m, int& x, int& y, int& z) const override {
        auto it = npcs.find(n);
        if (it == npcs.end()) return false;
        m = unsigned(it->second[0]); x = it->second[1]; y = it->second[2]; z = it->second[3];
        return true;
    }
    bool avatar_location(unsigned& m, int& x, int& y, int& z) const override {
        m = map; x = ax; y = ay; z = az;
        return true;
    }
};

void test_evaluator() {
    std::printf("critpath evaluator (sample file)\n");
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(g_sample + "/critpath_sample.json", err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    CHECK_EQ(cp.steps.size(), 6);
    CHECK_EQ(cp.links.size(), 5);
    CHECK(cp.steps[0].id == "execution");
    CHECK(cp.steps[0].done_when.kind == CritCond::kGlobal);
    CHECK(cp.steps[3].target.type == CritTarget::kNpc);
    CHECK(!cp.links[1].has_condition);
    CHECK(cp.links[0].has_condition);
    FakeCtx c;
    c.npcs[2] = {3, 14431, 5983, 8};
    // New game: the first step, its target on this map.
    CHECK_EQ(cp.current(c), 0);
    CritGuide g = cp.guide(c);
    CHECK(g.valid && g.have_point && g.x == 11732 && g.y == 5844 && g.link == -1);
    CHECK(g.line1 == "Next: Watch the execution at the dock");
    // The execution happened: next is Tenebrae, through the gate link (the nearer east-road
    // link leads nowhere useful, the side door's condition is false).
    c.g[89] = 1;
    CHECK_EQ(cp.current(c), 1);
    g = cp.guide(c);
    CHECK_EQ(g.link, 0);
    CHECK(g.have_point && g.x == 11516 && g.y == 1660);
    CHECK(g.line2.find("Central Tenebrae") != std::string::npos && g.line2.find("city gate") != std::string::npos);
    std::printf("  %s / %s\n", g.line1.c_str(), g.line2.c_str());
    // Link conditions: without the gate flag (step forced done) there is no way; the side door opens one.
    c.g[89] = 0;
    cp.forced_done.insert(0);
    g = cp.guide(c);
    CHECK_EQ(g.step, 1);
    CHECK(g.unreachable);
    c.g[130] = 1;
    g = cp.guide(c);
    CHECK_EQ(g.link, 2);
    cp.forced_done.clear();
    c.g[130] = 0;
    c.g[89] = 1;
    // Visited 40: East Tenebrae next, two hops away; the first hop is still the gate.
    c.visited.insert(40);
    CHECK_EQ(cp.current(c), 2);
    g = cp.guide(c);
    CHECK_EQ(g.link, 0);
    CHECK_EQ(g.route.size(), 3);
    CHECK(g.route.size() == 3 && g.route[0] == 3 && g.route[1] == 40 && g.route[2] == 41);
    CHECK(cp.route(c, 41, 3).empty());  // no link out of 41 in the sample
    // In Tenebrae the street link to 41.
    c.map = 40;
    c.ax = 13038; c.ay = 15042;
    g = cp.guide(c);
    CHECK_EQ(g.link, 4);
    // "any" with "ge".
    c.g[120] = 1;
    CHECK_EQ(cp.current(c), 2);
    c.g[120] = 2;
    CHECK_EQ(cp.current(c), 3);
    c.g[120] = 0;
    c.map = 41;
    CHECK_EQ(cp.current(c), 3);  // on_map 41 also does it
    // NPC target followed live (and through links when he is on another map).
    c.map = 3;
    c.ax = 14527; c.ay = 5887;
    c.g[120] = 2;
    g = cp.guide(c);
    CHECK(g.have_point && g.x == 14431 && g.y == 5983);
    c.npcs[2] = {3, 12000, 6000, 48};
    g = cp.guide(c);
    CHECK(g.x == 12000 && g.y == 6000);
    c.npcs[2] = {40, 13000, 14000, 48};
    g = cp.guide(c);
    CHECK_EQ(g.link, 0);
    CHECK_EQ(g.target_map, 40);
    // all + not: the key and Devon alive.
    c.items[{79, 0}] = 1;
    CHECK_EQ(cp.current(c), 4);
    c.dead.insert(2);
    CHECK_EQ(cp.current(c), 3);
    c.dead.clear();
    // Equipped, or reagents of one frame with a quantity.
    c.items[{395, 6}] = 4;
    CHECK_EQ(cp.current(c), 4);
    c.items[{395, 6}] = 5;
    CHECK_EQ(cp.current(c), 5);
    c.items[{395, 6}] = 0;
    c.equip.insert(1300);
    CHECK_EQ(cp.current(c), 5);
    // Out of order (critical_path.md 2): a later milestone of the same chain closes the steps
    // before it; a milestone of a chain that comes after closes the earlier chain.
    c.equip.clear();
    c.gone["633"] = 1;
    CHECK_EQ(cp.current(c), -1);
    CHECK(cp.guide(c).all_done);
    {
        FakeCtx f;
        f.visited.insert(40);  // milestone 1 of chain a: step 0 is closed, step 2 is next
        CHECK_EQ(cp.current(f), 2);
        f.visited.clear();
        f.items[{79, 0}] = 1;  // a non-milestone of chain b closes nothing
        CHECK_EQ(cp.current(f), 0);
        f.gone["633"] = 1;     // chain b's milestone: chain a (before it) and b's earlier steps
        CHECK_EQ(cp.current(f), -1);
        CHECK(cp.chain_follows("b", "a") && !cp.chain_follows("a", "b"));
    }
    // Debug jump.
    FakeCtx fresh;
    cp.jump_to = 3;
    CHECK_EQ(cp.current(fresh), 3);
    cp.jump_to = -1;
    // Bad input.
    CritPath bad;
    CHECK(!bad.load_text("{ not json", err));
    CHECK(!bad.load_text("{\"x\": 1}", err));
    CHECK(bad.load_text("[{\"from_map\": 1, \"to_map\": 2}]", err) && bad.links.size() == 1);
    CHECK(cp.steps[3].done_when.describe().find("not(npc 2 dead)") != std::string::npos);
    CHECK(map_name(40) == "Central Tenebrae");
}

// ---------------------------------------------------------------------------------------------
// The game mode

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, bool skip = true, bool sample = true) : gm(d) {
        gm.world().skip_opening = skip;
        gm.world().ask_name = false;
        gm.world().auto_ui = !skip;
        gm.saves().set_dir(g_saves);
        if (sample) gm.debug().load_crit({g_sample + "/critpath_sample.json"});
        gm.start();
        frames(40);
    }
    World& w() { return gm.world(); }
    DebugTools& dt() { return gm.debug(); }
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
    void key(int k, unsigned mods = 0) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; e.mods = mods; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    void shot(const std::string& name, bool move_cursor = true) {
        if (g_shots.empty()) return;
        if (move_cursor) move(316, 196);
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 2);
        std::printf("  shot %s\n", path.c_str());
    }
};

void test_menu(const GameData& d) {
    std::printf("debug menu\n");
    Config::global().set_bool("debug_mode", false);
    Ui u(d);
    DebugTools& dt = u.dt();
    World& w = u.w();
    // Off: F10 and the shortcuts do nothing.
    CHECK(!dt.enabled());
    u.key(kKeyF10);
    CHECK(dt.menu() == nullptr);
    u.key('g', kModCtrl);
    CHECK(!dt.god);
    // The options page has the two rows.
    MenuGump* book = w.open_menu(MenuGump::kOptions);
    int dbg_row = -1, auto_row = -1;
    std::vector<MenuGump::OptionRow> rows = book->option_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].label == "Debug Mode:") dbg_row = int(i);
        if (rows[i].label == "Autosave:") auto_row = int(i);
    }
    CHECK(dbg_row >= 0 && auto_row >= 0);
    if (dbg_row >= 0) {
        CHECK(rows[size_t(dbg_row)].value() == "Off");
        book->change_option(dbg_row, 1);  // on, and written to the settings file
        CHECK(dt.enabled());
        Config c2;
        c2.load(Config::global().path());
        CHECK(c2.get_bool("debug_mode", false));
    }
    if (auto_row >= 0) CHECK(rows[size_t(auto_row)].value() == "On");
    u.key(kKeyEscape);
    u.key(kKeyEscape);
    u.frames(3);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    // On: F10 opens the menu (modal, the world paused), F10 closes it.
    u.key(kKeyF10);
    DebugMenuGump* m = dt.menu();
    CHECK(m != nullptr);
    if (!m) return;
    u.frames(1);
    uint32_t f0 = w.kernel().frame_number();
    u.frames(5);
    CHECK(w.kernel().paused());
    CHECK_EQ(w.kernel().frame_number(), f0);
    u.shot("debug_menu", false);
    // Keyboard: select a row and act on it.
    CHECK(m->select("Noclip"));
    u.key(kKeyReturn);
    CHECK(w.debug_noclip);
    u.key(kKeyReturn);
    CHECK(!w.debug_noclip);
    // Mouse: hover a row, click it.
    m->select("Status line");
    int adv = w.font(0).line_advance() + 1;
    int sel = m->selected();
    u.move(m->x + 60, m->y + 16 + (sel + 1) * adv + 2);  // the next row: collision boxes
    CHECK_EQ(m->selected(), sel + 1);
    u.button(1, true);
    u.button(1, false);
    CHECK(dt.show_boxes);
    // Controller: D-pad down + A on the next row (eggs and editor items).
    u.pad(kPadDown);
    u.pad(kPadA);
    CHECK_EQ(m->selected(), sel + 2);
    // Sub-pages: steps; Right marks a step done, B goes back.
    m->select("Crit path: list steps");
    u.key(kKeyReturn);
    CHECK_EQ(m->depth(), 2);
    {
        // the first step row is right after "Clear debug overrides"
        m->select("Clear");
        u.key(kKeyDown);
        u.key(kKeyRight);
        CHECK(dt.crit.forced_done.count(0) == 1);
        u.key(kKeyRight);
        CHECK(dt.crit.forced_done.count(0) == 0);
    }
    u.pad(kPadB);
    CHECK_EQ(m->depth(), 1);
    // Numbers: the hour row.
    unsigned h0 = dt.hour();
    m->select("Time of day");
    u.key(kKeyRight);
    CHECK_EQ(dt.hour(), h0 + 1);
    u.key('7');
    u.key(kKeyReturn);
    CHECK_EQ(dt.hour() % 24, 7);
    // Wheel scrolls; Esc closes.
    InputEvent wh; wh.type = InputEvent::kWheel; wh.wheel = -1;
    u.ev(wh);
    u.key(kKeyF10);
    u.frames(2);
    CHECK(dt.menu() == nullptr);
    CHECK(!w.kernel().paused());
    // Shortcuts.
    u.key('g', kModCtrl);
    CHECK(dt.god);
    u.frames(2);
    CHECK(w.avatar()->is(Actor::kInvincible));
    u.key('g', kModCtrl);
    u.frames(2);
    CHECK(!w.avatar()->is(Actor::kInvincible));
    w.avatar()->hp = 1;
    u.key('l', kModCtrl);
    CHECK_EQ(w.avatar()->hp, 2 * w.avatar()->str);
    w.avatar()->mana = 0;
    u.key('m', kModCtrl);
    u.frames(1);
    CHECK_EQ(w.avatar()->mana, 2 * w.avatar()->intel);
    u.key('m', kModCtrl);
    // Pause, frame step, speed.
    uint32_t a = w.kernel().frame_number();
    u.frames(1);
    uint32_t per = w.kernel().frame_number() - a;
    CHECK(per >= 1);
    u.key('z', kModCtrl);
    a = w.kernel().frame_number();
    u.frames(10);
    CHECK_EQ(w.kernel().frame_number(), a);
    u.key('s', kModCtrl);
    u.frames(3);
    CHECK_EQ(w.kernel().frame_number(), a + per);
    u.key('z', kModCtrl);
    CHECK(!dt.paused);
    u.key('d', kModCtrl);
    CHECK_EQ(dt.speed, 2);
    a = w.kernel().frame_number();
    u.frames(5);
    CHECK_EQ(w.kernel().frame_number() - a, 10 * per);
    u.key('d', kModCtrl);
    CHECK_EQ(dt.speed, 4);
    u.key('d', kModCtrl);
    CHECK_EQ(dt.speed, 1);
    // Noclip: a solid item's place is open to the Avatar (above its feet), not to others.
    {
        const Actor* av = w.avatar();
        int xd, yd, zd;
        w.footprint(*av, xd, yd, zd);
        uint16_t blocker = 0;
        for (int dx = -512; dx <= 512 && !blocker; dx += 32)
            for (int dy = -512; dy <= 512 && !blocker; dy += 32) {
                World::Placement p = w.can_exist(av->x + dx, av->y + dy, av->z, xd, yd, zd, w.shape_flags(*av), 1);
                if (!p.valid) {
                    blocker = p.blocker;
                    w.debug_noclip = true;
                    World::Placement q = w.can_exist(av->x + dx, av->y + dy, av->z, xd, yd, zd, w.shape_flags(*av), 1);
                    World::Placement o = w.can_exist(av->x + dx, av->y + dy, av->z, xd, yd, zd, w.shape_flags(*av), 2);
                    const Item* b = w.item(blocker);
                    Box bb = b ? w.box_of(*b) : Box{};
                    if (bb.z + bb.zd > av->z) { CHECK(q.valid); CHECK(!o.valid); }
                    w.debug_noclip = false;
                }
            }
        CHECK(blocker != 0);
    }
    // Give, warp, teleport to an NPC, kill.
    WorldCritContext wc(w);
    int keys = wc.item_count(79, -1);
    CHECK(dt.give(79, 0, 1));
    CHECK_EQ(wc.item_count(79, -1), keys + 1);
    CHECK(dt.warp_to(40, 13038, 15042, 48));
    u.frames(5);
    CHECK_EQ(w.map().map_number(), 40);
    CHECK(dt.teleport_to_npc(2));  // Devon, back on map 3
    u.frames(5);
    CHECK_EQ(w.map().map_number(), 3);
    {
        const Actor* dv = w.actor(2);
        int dx = dv->x - w.avatar()->x, dy = dv->y - w.avatar()->y;
        CHECK(dx * dx + dy * dy < 300 * 300);
    }
    CHECK(dt.kill(2));
    CHECK(w.actor(2)->is(Actor::kDead));
    CHECK(!dt.kill(1));  // never the Avatar
    // The atlas warp: a click in the atlas jumps there.
    dt.toggle("warp_atlas");
    CHECK(u.gm.atlas_open());
    int at[3] = {11700, 5800, 48};
    CHECK(dt.atlas_jump(3, at));
    CHECK(!dt.atlas_warp);
    u.gm.close_atlas();
    // Snapshot round trip (deferred until the menu has closed).
    dt.toggle("snapshot");
    CHECK(u.gm.saves().exists(SaveStore::kSnapshotSlot));
    int sx = w.avatar()->x;
    dt.warp_to(3, 12500, 4000, 48);
    dt.toggle("load_snapshot");
    CHECK_EQ(w.avatar()->x, sx);
    // A debug script action.
    CHECK(u.gm.set_script("0:debug:routes"));
    bool r0 = dt.show_routes;
    u.frames(1);
    CHECK(dt.show_routes != r0);
    dt.set_enabled(false);
    u.key(kKeyF10);
    CHECK(dt.menu() == nullptr);
}

// The critical-path line: near the dock with the target on screen, and from the camp (off screen).
void test_crit_line(const GameData& d) {
    std::printf("critical-path line\n");
    Ui u(d);
    DebugTools& dt = u.dt();
    dt.force(true);
    dt.show_crit = true;
    dt.warp_to(3, 12100, 5560, 48);
    u.frames(20);
    CHECK(dt.guide.valid && dt.guide.have_point);
    CHECK_EQ(dt.guide.step, 0);
    CHECK(dt.guide.line1.find("execution") != std::string::npos);
    u.shot("critpath_line");
    // The off-screen arrow from the camp.
    dt.warp_to(3, 14527, 5887, 8);
    u.frames(20);
    u.shot("critpath_arrow");
    // Collision boxes and routes draw without trouble.
    dt.show_boxes = dt.show_routes = true;
    u.frames(3);
    u.shot("debug_boxes");
}

// Headless play through the execution: the tracker sees the step completed, and exactly one
// autosave is taken afterwards at a safe moment with the step's title.
void test_execution_autosave(const GameData& d, bool real) {
    std::printf("execution and its autosave (%s data)\n", real ? "real" : "sample");
    Config::global().set_bool("autosave", true);
    Config::global().set("autosave_last", "0");
    for (int s = SaveStore::kAutoFirst; s < SaveStore::kAutoFirst + SaveStore::kAutoSlots; ++s) std::remove(SaveStore(g_saves).path(s).c_str());
    Ui u(d, false, !real);
    DebugTools& dt = u.dt();
    World& w = u.w();
    if (real) dt.load_crit({g_data + "/critpath.json", g_data + "/map_links.json"});
    u.frames(8400, false);  // the opening (auto-answered) ends
    int first = dt.crit.current(dt.ctx);
    std::string first_title = first >= 0 ? dt.crit.steps[size_t(first)].title : "";
    std::string exec_title;  // the step that is exactly "G[89:1] = 1" (the execution)
    for (const CritStep& st : dt.crit.steps)
        if (st.done_when.describe() == "G[89:1]==1") { exec_title = st.title; break; }
    CHECK(!exec_title.empty());
    std::printf("  current step at the start: %d \"%s\" (%s)\n", first, first_title.c_str(),
                first >= 0 ? dt.crit.steps[size_t(first)].done_when.describe().c_str() : "");
    CHECK(first >= 0);
    bool uses89 = first >= 0 && dt.crit.steps[size_t(first)].done_when.describe().find("G[89:1]") != std::string::npos;
    CHECK(uses89);
    CHECK_EQ(w.vm().get_global(89, 1), 0);
    CHECK_EQ(dt.autosaves_written, 0);
    // Down the road to the dock (the same spot as the m4 gate test): the execution plays.
    u.gm.set_script(std::to_string(u.gm.frames()) + ":warp:11732,5844,48,3");
    bool started = false;
    int f = 0;
    for (; f < 9000; ++f) {
        u.frames(1, false);
        if (w.stasis) started = true;
        if (started && !w.stasis) break;
    }
    CHECK(started);
    CHECK_EQ(w.vm().get_global(89, 1), 1);
    std::printf("  the scene ended at frame %d\n", u.gm.frames());
    int during = dt.autosaves_written;
    // Wait for the autosave (a safe moment comes soon after the scene).
    for (int k = 0; k < 2000 && dt.autosaves_written == during; ++k) u.frames(1, false);
    CHECK_EQ(dt.autosaves_written, 1);
    CHECK(dt.last_autosave_safe);
    CHECK(dt.last_autosave_label.find("Autosave - " + exec_title + " (") == 0);
    int now = dt.crit.current(dt.ctx);
    CHECK(now != first);
    std::printf("  after the execution: step %d \"%s\"; autosave slot %d \"%s\" at frame %d\n", now,
                now >= 0 ? dt.crit.steps[size_t(now)].title.c_str() : "", dt.last_autosave_slot,
                dt.last_autosave_label.c_str(), dt.last_autosave_frame);
    u.frames(600, false);
    CHECK_EQ(dt.autosaves_written, 1);  // exactly one
    SaveStore st(g_saves);
    st.refresh();
    CHECK(st.exists(dt.last_autosave_slot));
    CHECK_EQ(st.latest_autosave(), dt.last_autosave_slot);
    CHECK(w.visited_maps.count(3) == 1);
    if (real) {
        // A cross-map route: Bentic (NPC 12, East Tenebrae) from the docks goes through the
        // gate's teleporter into Central Tenebrae first.
        int bentic = dt.crit.step_index("ask_bentic");
        CHECK(bentic >= 0);
        if (bentic >= 0) {
            dt.crit.jump_to = bentic;
            CritGuide g = dt.crit.guide(dt.ctx);
            std::printf("  route to Bentic:");
            for (unsigned m : g.route) std::printf(" %u", m);
            std::printf("; %s / %s\n", g.line1.c_str(), g.line2.c_str());
            CHECK(g.route.size() >= 2 && g.route.front() == 3 && g.route.back() == 41);
            CHECK(g.link >= 0 && g.have_point);
            if (g.link >= 0) CHECK_EQ(dt.crit.links[size_t(g.link)].from_map, 3);
            dt.crit.jump_to = -1;
        }
        // Visited maps survive a save and load.
        CHECK(u.gm.save_slot(50, "visited"));
        w.visited_maps.insert(77);
        CHECK(u.gm.load_slot(50));
        CHECK(w.visited_maps.count(77) == 0 && w.visited_maps.count(3) == 1);
        return;
    }

    // --- Death -> the death menu -> Load last autosave (sample run only).
    std::vector<uint8_t> file;
    CHECK(st.read(dt.last_autosave_slot, file));
    SaveHeader hdr;
    SaveHeader::read(file, hdr);
    int slot = dt.last_autosave_slot;
    int ax = 0, ay = 0;
    {
        // What the autosave holds: load it in a second game mode.
        GameMode g2(d);
        g2.saves().set_dir(g_saves);
        g2.start();
        CHECK(g2.load_slot(slot));
        ax = g2.world().avatar()->x;
        ay = g2.world().avatar()->y;
    }
    u.frames(200, false);  // walk on a little (the state now differs from the autosave)
    w.auto_ui = false;
    w.kill_actor(1, 0);
    MenuGump* mg = nullptr;
    for (int k = 0; k < 3000 && !mg; ++k) {
        u.frames(1, false);
        if (w.gumps.find<ReaderGump>()) u.key(kKeyEscape);
        mg = w.gumps.find<MenuGump>();
    }
    CHECK(mg != nullptr);
    if (!mg) return;
    int ds;
    CHECK(mg->death_autosave(ds) && ds == slot);
    u.shot("death_menu_autosave");
    u.key(kKeyReturn);  // the default choice
    u.frames(1);
    CHECK(!w.avatar()->is(Actor::kDead));
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    CHECK_EQ(w.avatar()->x, ax);
    CHECK_EQ(w.avatar()->y, ay);
    CHECK_EQ(w.vm().get_global(89, 1), 1);
    // Byte for byte: the loaded world (one frame on) equals the autosave loaded elsewhere and
    // run one frame.
    {
        GameMode g2(d);
        g2.saves().set_dir(g_saves);
        g2.start();
        g2.load_slot(slot);
        g2.tick();
        SaveWriter a1, a2;
        w.save_game(a1, "x");
        g2.world().save_game(a2, "x");
        std::printf("  loaded state %s the autosave's\n", a1.data() == a2.data() ? "is byte-identical to" : "differs from");
        CHECK(a1.data() == a2.data());
    }
    // The diary's load page opens on the autosaves.
    MenuGump* book = w.open_menu(MenuGump::kLoad);
    CHECK_EQ(book->spread, -1);
    CHECK_EQ(book->autosave_order().size(), 1);
    u.shot("diary_autosaves");
    u.key(kKeyRight);
    CHECK_EQ(book->spread, 0);
    u.key(kKeyLeft);
    CHECK_EQ(book->spread, -1);
    u.key(kKeyEscape);
    u.key(kKeyEscape);
    u.frames(2);
}

// Rotation, Off, debug-forced steps.
void test_autosave_rules(const GameData& d) {
    std::printf("autosave rotation and settings\n");
    Config::global().set_bool("autosave", true);
    Ui u(d);
    DebugTools& dt = u.dt();
    World& w = u.w();
    u.frames(20, false);
    int before = dt.autosaves_written;
    for (int i = 1; i <= 4; ++i) CHECK(dt.autosave_now("R" + std::to_string(i)));
    CHECK_EQ(dt.autosaves_written, before + 4);
    SaveStore st(g_saves);
    st.refresh();
    int n = 0;
    for (int s = SaveStore::kAutoFirst; s < SaveStore::kAutoFirst + SaveStore::kAutoSlots; ++s) n += st.exists(s);
    CHECK_EQ(n, 3);
    CHECK_EQ(st.latest_autosave(), dt.last_autosave_slot);
    CHECK(st.description(st.latest_autosave()).find("Autosave - R4") == 0);
    MenuGump* book = w.open_menu(MenuGump::kLoad);
    std::vector<int> order = book->autosave_order();
    CHECK_EQ(order.size(), 3);
    if (order.size() == 3) {
        CHECK(u.gm.saves().description(order[0]).find("R4") != std::string::npos);
        CHECK(u.gm.saves().description(order[1]).find("R3") != std::string::npos);
        CHECK(u.gm.saves().description(order[2]).find("R2") != std::string::npos);
    }
    // Player saves are untouched by the rotation.
    for (int s = 2; s <= SaveStore::kSlots; ++s) if (st.exists(s)) CHECK(st.description(s).find("Autosave") == std::string::npos);
    book->close();
    u.frames(2, false);
    // Off: a completed step schedules nothing.
    dt.set_autosave(false);
    int w0 = dt.autosaves_written;
    w.vm().set_global(120, 2, 2);  // the sample's "east_town" step
    u.frames(40, false);
    CHECK(!dt.autosave_pending());
    CHECK_EQ(dt.autosaves_written, w0);
    // On again: another step completed in play is saved.
    dt.set_autosave(true);
    w.vm().set_global(89, 1, 1);  // the sample's first step
    u.frames(60, false);
    CHECK_EQ(dt.autosaves_written, w0 + 1);
    CHECK(dt.last_autosave_label.find("Watch the execution") != std::string::npos);
    // A step marked done in the debug menu is not autosaved.
    dt.crit.forced_done.insert(3);
    u.frames(60, false);
    CHECK_EQ(dt.autosaves_written, w0 + 1);
    // A load is not a completion: loading the snapshot from before a step does not autosave.
    dt.save_snapshot();
    w.vm().set_global(89, 1, 0);
    u.frames(30, false);
    w.vm().set_global(89, 1, 1);
    u.frames(30, false);
    CHECK_EQ(dt.autosaves_written, w0 + 2);
    u.gm.load_slot(SaveStore::kSnapshotSlot);
    u.frames(60, false);
    CHECK_EQ(dt.autosaves_written, w0 + 2);
    // Unsafe moments wait: in combat nothing is written until he leaves it.
    w.set_in_combat(1);
    w.vm().set_global(89, 1, 0);
    u.frames(30, false);
    w.vm().set_global(89, 1, 1);
    u.frames(60, false);
    CHECK(dt.autosave_pending());
    CHECK_EQ(dt.autosaves_written, w0 + 2);
    w.clear_in_combat(1);
    u.frames(30, false);
    CHECK_EQ(dt.autosaves_written, w0 + 3);
}

// The real data file, if the researcher's files have arrived.
void test_real_data(const GameData& d) {
    std::string f = g_data + "/critpath.json";
    std::ifstream in(f);
    if (!in) { std::printf("real data: %s not there yet (skipped)\n", f.c_str()); return; }
    std::printf("real data: %s\n", f.c_str());
    CritPath cp;
    std::string err;
    CHECK(cp.load_file(f, err));
    std::ifstream l(g_data + "/map_links.json");
    if (l) CHECK(cp.load_file(g_data + "/map_links.json", err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    std::printf("  %zu steps, %zu links\n", cp.steps.size(), cp.links.size());
    CHECK(!cp.steps.empty());
    int unknown = 0;
    std::function<void(const CritCond&)> walk = [&](const CritCond& c) {
        if (c.kind == CritCond::kUnknown) { ++unknown; std::printf("  unknown condition %s\n", c.text.c_str()); }
        for (const CritCond& s : c.sub) walk(s);
    };
    for (const CritStep& s : cp.steps) walk(s.done_when);
    CHECK_EQ(unknown, 0);
    test_execution_autosave(d, true);
}

}  // namespace


// The Give item page (Ctrl-I): one click on a row gives that item to the Avatar, into his
// backpack; money gives 500 a click as one stack; stacks merge; the result is said in the menu
// and the message log; Enter and the controller act the same; a full pack drops at his feet.
void test_give_page(const GameData& d) {
    std::printf("give item page\n");
    Config::global().set_bool("debug_mode", true);
    Ui u(d);
    DebugTools& dt = u.dt();
    World& w = u.w();
    u.key('i', kModCtrl);
    DebugMenuGump* m = dt.menu();
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->title() == "Give item");
    CHECK_EQ(m->depth(), 2u);
    const uint16_t pack = w.equipped(1, 7);
    CHECK(pack != 0);
    // Items of a shape in the Avatar's pack (stacks: their counts) and the number of stacks.
    auto in_pack = [&](unsigned shape, int frame = -1, int* stacks = nullptr, int* biggest = nullptr) {
        int n = 0, k = 0, big = 0;
        if (const Item* p = w.item(pack))
            for (uint16_t c : p->contents)
                if (const Item* it = w.item(c); it && it->shape == shape && (frame < 0 || it->frame == frame)) {
                    unsigned fam = d.typeflags.get(shape).family();
                    n += (fam == kFamQuantity || fam == kFamReagent) ? it->quality : 1;
                    ++k;
                    big = std::max<int>(big, it->quality);
                }
        if (stacks) *stacks = k;
        if (biggest) *biggest = big;
        return n;
    };
    // Clicks a row with the mouse: select it (scrolls it into view), find its line, click.
    int adv = w.font(0).line_advance() + 1;
    auto click_row = [&](const std::string& label) {
        if (!m->select(label)) { std::printf("  no row %s\n", label.c_str()); return false; }
        int want = m->selected(), line = want - m->top();
        u.move(m->x + 60, m->y + 16 + line * adv + 2);
        if (line >= 0 && m->selected() == want) {
            u.button(1, true);
            u.button(1, false);
            return true;
        }
        std::printf("  row %s not on screen\n", label.c_str());
        return false;
    };
    // Every list row is labelled with its item and shape:frame; the money row is first.
    int item_rows = 0;
    for (const DebugRow& r : m->rows())
        if (!r.header && !r.value && r.shortcut.find(':') != std::string::npos) ++item_rows;
    CHECK(item_rows >= 60);
    u.shot("give_page", false);

    // Money: 500 a click, one stack (spec: at most 666 in a stack, the rest a stack of its own).
    int stacks = 0, big = 0;
    int coins = in_pack(143);
    CHECK(click_row("Obsidian coins"));
    CHECK_EQ(in_pack(143, -1, &stacks, &big), coins + 500);
    CHECK(big >= 500 && big <= 666);
    CHECK(m->status.find("500 x Obsidian coins") != std::string::npos);
    CHECK(dt.menu() == m);  // the menu stays open for more clicks
    {
        const auto& lines = w.game_log.lines(GameLog::kMessages);
        CHECK(!lines.empty() && lines.back().text.find("500 x Obsidian coins") != std::string::npos);
    }
    u.shot("give_coins_clicked", false);
    CHECK(click_row("Obsidian coins"));
    CHECK(click_row("Obsidian coins"));
    int total = in_pack(143, -1, &stacks, &big);
    CHECK_EQ(total, coins + 1500);
    CHECK(big <= 666);
    CHECK_EQ(stacks, (total + 665) / 666);  // merged: only the last stack is not full

    // Any other item: one a click, the shape asked for, into the pack.
    int swords = in_pack(420);
    CHECK(click_row("Sword"));
    CHECK_EQ(in_pack(420), swords + 1);
    CHECK(m->status == "Gave Sword (" + std::to_string(swords + 1) + " carried)");
    // Reagents: one a click, joining the stack already there.
    int blood = in_pack(395, 5, &stacks);
    CHECK(click_row("Blood"));
    CHECK(click_row("Blood"));
    int bstacks = 0;
    CHECK_EQ(in_pack(395, 5, &bstacks), blood + 2);
    CHECK_EQ(bstacks, std::max(1, stacks));
    // Keyboard Enter and controller A act like a click.
    int daggers = in_pack(418);
    CHECK(m->select("Dagger"));
    u.key(kKeyReturn);
    CHECK_EQ(in_pack(418), daggers + 1);
    int maces = in_pack(417);
    CHECK(m->select("Mace"));
    u.pad(kPadA);
    CHECK_EQ(in_pack(417), maces + 1);
    // Given items work: the sword can be wielded.
    u.key(kKeyF10);
    u.frames(3);
    uint16_t sword = 0;
    if (const Item* p = w.item(pack))
        for (uint16_t c : p->contents)
            if (const Item* it = w.item(c); it && it->shape == 420) sword = c;
    CHECK(sword != 0 && w.equip(1, sword));
    // A full pack: the item goes to his feet (said so), not lost.
    u.key('i', kModCtrl);
    m = dt.menu();
    CHECK(m != nullptr);
    if (!m) return;
    bool dropped = false;
    for (int i = 0; i < 200 && !dropped; ++i) {
        CHECK(click_row("Bag"));
        dropped = m->status.find("Backpack full: Bag dropped at his feet") != std::string::npos;
    }
    CHECK(dropped);
    {
        const Actor* av = w.avatar();
        bool near = false;
        std::vector<uint16_t> ids;
        w.area_search(av->x, av->y, 128, false, ids);
        for (uint16_t k : ids)
            if (const Item* it = w.item(k); it && it->shape == 637 && it->parent == 0) near = true;
        CHECK(near);
    }
    u.shot("give_pack_full", false);
    u.key(kKeyF10);
    u.frames(2);
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_sample = argc > 2 ? argv[2] : "tests/data";
    g_data = argc > 3 ? argv[3] : "data";
    g_shots = argc > 4 ? argv[4] : "";
    g_saves = argc > 5 ? argv[5] : "/tmp/pagan_debug_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    test_evaluator();
    test_menu(d);
    test_give_page(d);
    test_crit_line(d);
    test_autosave_rules(d);
    test_execution_autosave(d, false);
    test_real_data(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
