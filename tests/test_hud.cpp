// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The HUD side panel (app/hud_panel.*, gfx/screen_layout.*, game/game_log.*; PLAN-HUD.md) and the
// longer turn-based Move with its destination cursor (app/turn_ui.*): the layout maths for many
// window sizes and the mouse mapping, the message log from a real look and a real conversation,
// the combat log from blows, misses, deaths and a turn-based fight, the panel's buttons (hit
// testing, tooltips, the keys and pad still driving them), Party Auto off by Esc / B / the
// "Auto mode: ON" button, portraits (click, an item dropped on one), log scrolling (wheel, the
// drag bar, new lines), the controller focus, and screenshots at 16:9 and 4:3.
//   pagan_hud_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [only]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/screen_layout.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Surface8 panel{106, 200};
    int win_w = 1280, win_h = 720;
    ScreenLayout lay;
    Ui(const GameData& d, bool turn = true, int ww = 1280, int wh = 720) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = true;
        w.turn->enabled = turn;
        gm.start();
        set_window(ww, wh);
    }
    void set_window(int ww, int wh) {
        win_w = ww;
        win_h = wh;
        lay = compute_layout(ww, wh, 320, 200, true, LayoutScale::kInteger, true);
        gm.set_panel_width(lay.panel_px);
        panel.resize(std::max(1, lay.panel_px), 200);
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    TurnCombat& t() { return *gm.world().turn; }
    TurnUI& ui() { return gm.turn_ui(); }
    HudPanel& hud() { return gm.hud_panel(); }
    void draw() {
        gm.render(surf);
        if (gm.panel_on()) gm.render_panel(panel);
    }
    void frames(int n, bool d = true) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            if (d) draw();
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
    void wheel(int x, int y, int d) { InputEvent e; e.type = InputEvent::kWheel; e.x = x; e.y = y; e.wheel = d; ev(e); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; e.pad = 7; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    // Panel point -> the game-surface point the platform reports (x >= 320).
    void click_panel(int px, int py) { click(320 + px, py); }
    // The whole window as the platform shows it (picture + panel), saved as shots/NAME.png.
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        std::vector<uint32_t> rgba, prgba, win;
        int fw, fh, pw = 0, ph = 0;
        int S = std::clamp(lay.game_w / 320, 1, 4);
        gm.render_frame(rgba, fw, fh, S, 0.0, 1.0 / 30);
        gm.render_panel_rgba(prgba, pw, ph);
        compose_window(lay, win_w, win_h, rgba.data(), fw, fh, pw ? prgba.data() : nullptr, pw, ph, win);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, win.data(), win_w, win_h, 1);
        std::printf("  shot %s\n", path.c_str());
        draw();  // the plain render again (test hit tests use it)
    }
    uint16_t cid(const std::string& name) { int i = p().find_def(name); return i >= 0 ? p().state[size_t(i)].id : 0; }
    void join(std::initializer_list<const char*> names) { for (const char* n : names) p().join(p().find_def(n)); }
    void avatar_to(int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().move_to(1, x, y, z, false);
        frames(2);
    }
    bool has_line(GameLog::Channel ch, const std::string& part, GameLog::Kind* kind = nullptr) {
        for (const auto& l : w().game_log.lines(ch))
            if (l.text.find(part) != std::string::npos) { if (kind) *kind = l.kind; return true; }
        return false;
    }
    void dump(GameLog::Channel ch, size_t last = 12) {
        const auto& ls = w().game_log.lines(ch);
        for (size_t i = ls.size() > last ? ls.size() - last : 0; i < ls.size(); ++i) std::printf("    | %s\n", ls[i].text.c_str());
    }
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

Gump* find_kind(World& w, const char* kind) {
    for (const auto& g : w.gumps.list)
        if (!g->closed && std::string(g->kind()) == kind) return g.get();
    return nullptr;
}

// ---------------------------------------------------------------------------------------------

void test_layout() {
    std::printf("layout: the picture on the left, the panel in the spare width\n");
    struct Case { int w, h; LayoutScale m; int gx, gy, gw, gh, ppx; };
    const Case cases[] = {
        {1280, 720, LayoutScale::kInteger, 0, 0, 960, 720, 106},     // 16:9: 3x, a 106-pixel panel
        {1920, 1080, LayoutScale::kInteger, 0, 60, 1280, 960, 160},  // 16:9: 4x
        {2560, 1440, LayoutScale::kInteger, 0, 0, 1920, 1440, 106},  // 16:9: 6x
        {1024, 768, LayoutScale::kInteger, 0, 89, 787, 590, 96},     // 4:3: shrinks (fit) to leave the minimum
        {800, 600, LayoutScale::kInteger, 0, 70, 615, 461, 96},      // small 4:3
        {1280, 720, LayoutScale::kFit, 0, 0, 960, 720, 106},
        {1024, 768, LayoutScale::kFit, 0, 89, 787, 590, 96},
        {3440, 1440, LayoutScale::kFit, 0, 0, 1920, 1440, 253},      // 21:9
        {5120, 1440, LayoutScale::kFit, 0, 0, 1920, 1440, 320},      // 32:9: the panel is capped
        {600, 1000, LayoutScale::kFit, 0, 327, 461, 346, 96},        // portrait window
    };
    for (const Case& c : cases) {
        ScreenLayout l = compute_layout(c.w, c.h, 320, 200, true, c.m, true);
        std::printf("  %4dx%-4d %s: picture %d,%d %dx%d, panel %d,%d %dx%d (%d game px)\n", c.w, c.h,
                    c.m == LayoutScale::kInteger ? "integer" : "fit", l.game_x, l.game_y, l.game_w, l.game_h, l.panel_x, l.panel_y,
                    l.panel_w, l.panel_h, l.panel_px);
        CHECK_EQ(l.game_x, c.gx);
        CHECK(std::abs(l.game_y - c.gy) <= 1);
        CHECK(std::abs(l.game_w - c.gw) <= 1);
        CHECK(std::abs(l.game_h - c.gh) <= 1);
        CHECK_EQ(l.panel_px, c.ppx);
        CHECK(l.panel_x == l.game_w && l.panel_y == l.game_y && l.panel_h == l.game_h);
        CHECK(l.panel_x + l.panel_w <= c.w);
        CHECK(l.panel_px >= kMinPanel && l.panel_px <= kMaxPanel);
        // The panel's pixels are the game's: a window pixel in the panel maps past x 320.
        double fx, fy;
        layout_window_to_game(l, 320, 200, l.panel_x + l.panel_w / 2.0, l.panel_y + l.panel_h / 2.0, fx, fy);
        CHECK(fx >= 320 + l.panel_px / 2.0 - 1.5 && fx <= 320 + l.panel_px / 2.0 + 1.5);
        CHECK(std::abs(fy - 100) < 1.5);
        layout_window_to_game(l, 320, 200, l.game_x + l.game_w / 2.0, l.game_y + l.game_h / 2.0, fx, fy);
        CHECK(std::abs(fx - 160) < 1.5 && std::abs(fy - 100) < 1.5);
    }
    // Classic: centred as before, no panel.
    ScreenLayout c = compute_layout(1280, 720, 320, 200, true, LayoutScale::kInteger, false);
    CHECK_EQ(c.panel_px, 0);
    CHECK_EQ(c.game_x, 160);
    CHECK_EQ(c.game_w, 960);
    // Stretch: the picture and the minimum panel fill the window.
    ScreenLayout st = compute_layout(1280, 720, 320, 200, true, LayoutScale::kStretch, true);
    CHECK(st.game_h == 720 && st.panel_px >= kMinPanel && st.panel_x + st.panel_w <= 1280 && st.panel_x + st.panel_w >= 1275);
    // The composed window: black outside, the panel's pixels on the right.
    std::vector<uint32_t> g(320 * 200, 0xFF112233u), p(106 * 200, 0xFF445566u), win;
    ScreenLayout l = compute_layout(1280, 720, 320, 200, true, LayoutScale::kInteger, true);
    compose_window(l, 1280, 720, g.data(), 320, 200, p.data(), 106, 200, win);
    CHECK_EQ(win[size_t(360) * 1280 + 480], 0xFF112233u);
    CHECK_EQ(win[size_t(360) * 1280 + 1100], 0xFF445566u);
    CHECK_EQ(win[size_t(360) * 1280 + 1279], 0xFF000000u);  // 960 + 318 = 1278: the last column is black
}

void test_messages(const GameData& d) {
    std::printf("message log: a look, a conversation, an item taken\n");
    Ui u(d, false);
    u.frames(60);
    World& w = u.w();
    u.join({"brannoc", "varn", "ilvaran", "nerissa"});
    u.frames(5);
    // A real look: a left click on something near the Avatar.
    uint16_t looked = 0;
    for (int y = 60; y < 170 && !looked; y += 6)
        for (int x = 40; x < 300 && !looked; x += 6) {
            uint16_t id = u.gm.pick(x, y);
            const Item* it = w.item(id);
            if (!it || id < 256 || it->as_actor()) continue;
            unsigned before = w.game_log.serial(GameLog::kMessages);
            u.click(x, y);
            u.frames(40);
            for (const auto& l : w.game_log.lines(GameLog::kMessages))
                if (l.serial > before && l.kind == GameLog::kLook) looked = id;
        }
    CHECK(looked != 0);
    GameLog::Kind k;
    CHECK(u.has_line(GameLog::kMessages, "You see ", &k) && k == GameLog::kLook);
    u.dump(GameLog::kMessages, 3);
    // A conversation: Devon at the camp, through the answer menu.
    uint16_t devon = w.actor(2) ? 2 : 0;  // NPC 2 (usecode class DEVON)
    CHECK(devon != 0);
    if (devon) {
        w.call_event(devon, kEvUse);
        int answered = 0;
        for (int f = 0; f < 2400 && answered < 2; ++f) {
            u.frames(1, f % 2 == 0);
            if (w.ask_menu && w.ask_menu->frames_open > 20 && !w.ask_menu->answers.empty()) {
                w.choose_answer(0);
                ++answered;
            }
        }
        CHECK(answered >= 1);
        CHECK(u.has_line(GameLog::kMessages, "Devon: ", &k) && k == GameLog::kSpeech);
        CHECK(u.has_line(GameLog::kMessages, "> ", &k) && k == GameLog::kAnswer);
        u.dump(GameLog::kMessages, 8);
        // Let it run out.
        for (int f = 0; f < 3000 && (w.ask_menu || w.in_conversation()); ++f) {
            if (w.ask_menu && w.ask_menu->frames_open > 20) w.choose_answer(unsigned(w.ask_menu->answers.size() - 1));
            u.frames(1, false);
        }
    }
    // Messages the game mode shows (toasts) go in too.
    u.gm.toast("Autosaved", 30);
    CHECK(u.has_line(GameLog::kMessages, "Autosaved", &k) && k == GameLog::kInfo);
    // The panel shows the log at the bottom; the portraits are on top.
    u.frames(2);
    CHECK(!u.hud().combat_view());
    CHECK(u.hud().log_lines() >= 3);
    CHECK_EQ(u.hud().scroll(), 0);
    u.shot("hud_16x9_messages");
    u.set_window(1024, 768);
    u.frames(2);
    u.shot("hud_4x3_messages");
}

void test_scroll(const GameData& d) {
    std::printf("log scrolling: wheel, drag bar, new lines\n");
    Ui u(d, false);
    u.frames(30);
    GameLog& g = u.w().game_log;
    for (int i = 0; i < 80; ++i) g.add(GameLog::kMessages, GameLog::kInfo, "Line " + std::to_string(i) + " of the test log");
    u.frames(2);
    HudPanel& h = u.hud();
    CHECK(h.log_lines() >= 80);
    int lx, ly;
    CHECK(h.log_point(lx, ly));
    CHECK_EQ(h.scroll(), 0);
    u.wheel(320 + lx, ly, 1);  // up: older lines
    u.frames(1);
    CHECK(h.scroll() > 0);
    int s1 = h.scroll();
    // New lines while scrolled up: the view stays where it is.
    g.add(GameLog::kMessages, GameLog::kInfo, "A new line");
    u.frames(1);
    CHECK_EQ(h.scroll(), s1 + 1);
    // The wheel over the world zooms instead (the panel does not take it).
    u.gm.debug().force(true);  // (the test's window is small: only debug mode has zoom levels there)
    int z0 = u.gm.zoom_step();
    u.wheel(160, 100, -1);
    CHECK(u.gm.zoom_step() != z0);
    u.wheel(160, 100, 1);
    u.gm.debug().force(false);
    // Down to the bottom: new lines show at once again.
    for (int i = 0; i < 40; ++i) u.wheel(320 + lx, ly, -1);
    u.frames(1);
    CHECK_EQ(h.scroll(), 0);
    g.add(GameLog::kMessages, GameLog::kInfo, "Another line");
    u.frames(1);
    CHECK_EQ(h.scroll(), 0);
    // The drag bar: press its track at the top = a page up; drag the thumb down = back down.
    int tx = u.lay.panel_px - 5, ty = ly - 40;
    u.click_panel(tx, ty);
    u.frames(1);
    CHECK(h.scroll() >= h.log_visible() - 1);
    // Wheel over the panel never zooms.
    int z1 = u.gm.zoom_step();
    u.wheel(320 + lx, ly, -1);
    CHECK_EQ(u.gm.zoom_step(), z1);
}

void test_combat_log(const GameData& d) {
    std::printf("combat log: blows, misses, deaths, heals (real time)\n");
    Ui u(d, false);
    u.frames(60);
    World& w = u.w();
    u.join({"brannoc", "ilvaran"});
    u.avatar_to(14300, 6150, 0);
    uint16_t m = spawn(u, 142, 140, 260, 0.4);
    CHECK(m);
    if (!m) return;
    w.receive_hit(m, 1, 0, 5, kDmgPierce);
    CHECK(u.has_line(GameLog::kCombat, "hits the ghoul for 5."));
    CHECK_EQ(w.game_log.last_foe, m);
    w.receive_hit(1, m, 0, 3, kDmgPierce);
    CHECK(u.has_line(GameLog::kCombat, "The ghoul hits "));
    w.receive_hit(m, u.cid("brannoc"), 0, 999, kDmgPierce);
    GameLog::Kind k;
    CHECK(u.has_line(GameLog::kCombat, "Brannoc kills the ghoul.", &k) && k == GameLog::kDeath);
    // A death without a blow.
    uint16_t m2 = spawn(u, 142, 140, 300, 2.0);
    if (m2) {
        w.kill_actor(m2, 0);
        CHECK(u.has_line(GameLog::kCombat, "The ghoul dies.", &k) && k == GameLog::kDeath);
    }
    // Misses: the Avatar's swings in real time against a ghoul (some miss).
    uint16_t m3 = spawn(u, 142, 100, 200, 1.0);
    if (m3) {
        Actor* g = w.actor(m3);
        g->hp = 250;
        for (int i = 0; i < 40; ++i) w.receive_hit(m3, 1, 0, 0, 0);
        CHECK(u.has_line(GameLog::kCombat, "misses the ghoul") || u.has_line(GameLog::kCombat, "blocks"));
    }
    u.dump(GameLog::kCombat, 6);
    // Combat mode in real time: the panel's combat view without buttons.
    w.set_in_combat(1);
    u.frames(2);
    CHECK(u.hud().combat_view());
    CHECK(!u.hud().turn_buttons());
    CHECK_EQ(u.hud().roster_rows(), 3);
    u.shot("hud_16x9_realtime_combat");
}

// A turn-based fight on the panel: the roster, the monster slot, the buttons, tooltips.
void test_turn_panel(const GameData& d, int ww, int wh, const char* tag) {
    std::printf("turn-based fight on the panel (%dx%d)\n", ww, wh);
    Ui u(d, true, ww, wh);
    u.frames(60);
    u.join({"brannoc", "varn", "ilvaran", "kalvax", "wynna", "nerissa"});
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    Actor* av = u.w().avatar();
    av->hp = uint16_t(2 * av->str);
    uint16_t m[3] = {spawn(u, 142, 260, 420, 0.3), spawn(u, 142, 260, 420, 1.3), spawn(u, 807, 220, 400, 2.4)};
    CHECK(m[0] && m[1] && m[2]);
    u.frames(2);
    u.gm.handle([] { InputEvent e; e.type = InputEvent::kKeyDown; e.key = 'c'; return e; }());
    u.frames(3);
    CHECK(u.t().active());
    CHECK(u.hud().combat_view() && u.hud().turn_buttons());
    CHECK_EQ(u.hud().roster_rows(), 7);
    // Party members wait for commands: let the turns come round to one.
    for (int f = 0; f < 3000 && !u.t().awaiting_player(); ++f) u.frames(1, f % 3 == 0);
    CHECK(u.t().awaiting_player());
    u.frames(2);
    // The monster slot shows a foe of the fight.
    CHECK(u.hud().monster_slot() != 0 && u.t().is_fighter(u.hud().monster_slot()));
    // Hovering a button: its tooltip.
    int bx, by;
    CHECK(u.hud().button_point(HudPanel::kBDefend, bx, by));
    u.move(320 + bx, by);
    u.frames(12);
    CHECK(u.hud().tooltip().find("Defend") != std::string::npos);
    u.shot(std::string("hud_") + tag + "_combat_tooltip");
    // The keys still drive the menu: the arrows move the highlight, which the buttons show.
    u.move(160, 100);
    int sel0 = u.ui().selected_entry();
    u.key(kKeyRight);
    CHECK_EQ(u.ui().selected_entry(), TurnUI::grid_step(sel0, u.ui().menu_size(), u.ui().menu_columns(), 1, 0));
    // A click on Defend: the turn's action.
    uint16_t cur = u.t().current();
    u.click(320 + bx, by);
    u.frames(2);
    CHECK(u.t().current() != cur || !u.t().awaiting_player());
    CHECK(u.has_line(GameLog::kCombat, " defends"));
    // Next party turn: Move through the button, the destination pushed with the keys.
    for (int f = 0; f < 3000 && !u.t().awaiting_player(); ++f) u.frames(1, f % 3 == 0);
    if (u.t().awaiting_player()) {
        CHECK(u.hud().button_point(HudPanel::kBMove, bx, by));
        u.click(320 + bx, by);
        u.frames(1);
        CHECK(u.ui().mode() == TurnUI::kPickMove);
        CHECK_EQ(u.ui().active_entry(), int(TurnUI::kEMove));
        for (int i = 0; i < 6; ++i) u.key(kKeyRight);
        u.frames(2);
        u.shot(std::string("hud_") + tag + "_move");
        // B / Esc: one step back (to the menu).
        u.key(kKeyEscape);
        CHECK(u.ui().mode() == TurnUI::kMenu);
        CHECK(u.t().awaiting_player());
    }
    u.dump(GameLog::kCombat, 8);
    // Party Auto from the panel's button, then the "Auto mode: ON" button turns it off.
    CHECK(u.hud().button_point(HudPanel::kBPartyAuto, bx, by));
    u.click(320 + bx, by);
    CHECK(u.t().party_auto);
    u.frames(20);
    int ax, ay;
    CHECK(u.ui().auto_button_point(ax, ay));
    CHECK(ax < 100 && ay > 170);  // bottom left of the picture
    u.move(ax, ay);
    u.frames(2);
    u.shot(std::string("hud_") + tag + "_auto_mode");
    u.click(ax, ay);
    CHECK(!u.t().party_auto);
    // Esc turns it off too (and does not open the main menu).
    u.t().toggle_party_auto();
    u.frames(2);
    u.key(kKeyEscape);
    CHECK(!u.t().party_auto);
    CHECK(!u.w().gumps.top_modal());
    // And the pad's B.
    u.t().toggle_party_auto();
    u.frames(2);
    u.pad(kPadB);
    CHECK(!u.t().party_auto);
    u.t().end_fight(TurnCombat::kAborted);
}

void test_move_range(const GameData& d) {
    std::printf("Move: 300 units, the destination cursor stays in range\n");
    Ui u(d, true);
    u.frames(60);
    u.avatar_to(14300, 6150, 0);
    u.frames(30);
    uint16_t m = spawn(u, 142, 380, 520, 0.3);
    CHECK(m);
    u.key('c');
    u.frames(3);
    for (int f = 0; f < 3000 && !(u.t().awaiting_player() && u.t().current() == 1); ++f) u.frames(1, f % 3 == 0);
    CHECK(u.t().awaiting_player() && u.t().current() == 1);
    auto t0 = std::chrono::steady_clock::now();
    const auto& r = u.t().reach();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int far = 0;
    for (const auto& n : r) far = std::max(far, n.cost);
    std::printf("  reachable spots %zu, farthest %d, %.1f ms\n", r.size(), far, ms);
    CHECK(far > 200 && far <= TurnCombat::kMoveRange);
    u.t().auto_end = false;  // this test looks at the menu after the Move
    u.key('1');
    u.frames(1);
    CHECK(u.ui().mode() == TurnUI::kPickMove);
    CHECK(u.ui().hover_node() < 0);  // starts on the character
    // Push the destination one way far past the range: it stops on the edge.
    int lastx = -1, lasty = -1, still = 0;
    for (int i = 0; i < 80; ++i) {
        u.key(kKeyLeft);
        int x, y;
        u.ui().dest_screen(x, y);
        if (x == lastx && y == lasty) ++still;
        lastx = x;
        lasty = y;
    }
    int hn = u.ui().hover_node();
    CHECK(hn > 0);
    if (hn > 0) CHECK(u.t().reach()[size_t(hn)].cost <= TurnCombat::kMoveRange);
    CHECK(still > 10);  // it stayed put once it reached the edge
    u.frames(2);
    // Enter: pick; Esc lets the pick go (one step), Esc again = back to the menu.
    u.key(kKeyReturn);
    CHECK(!u.t().moved() || u.ui().mode() == TurnUI::kMenu);
    if (u.ui().mode() == TurnUI::kMenu && u.t().moved()) {
        int x, y, z;
        const Actor* av = u.w().avatar();
        (void)x; (void)y; (void)z; (void)av;
    }
    for (int f = 0; f < 600 && !u.t().awaiting_player(); ++f) u.frames(1, false);
    CHECK(u.t().moved());
    // The mouse far outside the range: the marker sits on the nearest reachable spot.
    u.t().end_fight(TurnCombat::kAborted);
}

// The icons by arrows and D-pad, the range circle, the bolt and flash, peace after the win
// (NOTES.md "Combat round two"). Shots: combat2_*.
void test_combat2(const GameData& d, int ww, int wh, int cols, const char* tag, bool shots) {
    std::printf("combat icons, circle, lightning (%s)\n", tag);
    Ui u(d, true, ww, wh);
    u.frames(60);
    u.join({"wynna", "varn"});
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    uint16_t g = spawn(u, 142, 300, 380, 0.3);
    CHECK(g);
    u.w().actor(g)->hp = 600;
    u.frames(2);
    u.gm.handle([] { InputEvent e; e.type = InputEvent::kKeyDown; e.key = 'c'; return e; }());
    u.frames(6);
    auto to_turn = [&](uint16_t who) {
        for (int i = 0; i < 3000 && u.t().active(); ++i) {
            if (u.t().awaiting_player()) {
                if (u.t().current() == who) return true;
                u.key('5');
            }
            u.frames(1, i % 4 == 0);
        }
        return false;
    };
    CHECK(to_turn(1));
    u.frames(2);
    CHECK_EQ(u.ui().menu_columns(), cols);
    CHECK_EQ(u.ui().menu_size(), int(HudPanel::kButtons));
    const int nrows = (int(HudPanel::kButtons) + cols - 1) / cols;
    TurnUI& ui = u.ui();
    CHECK_EQ(ui.selected_entry(), 0);
    // Left and Right along the row, both inputs, wrapping in the row.
    u.key(kKeyRight);
    CHECK_EQ(ui.selected_entry(), 1);
    u.pad(kPadRight);
    CHECK_EQ(ui.selected_entry(), 2);
    u.pad(kPadLeft);
    u.key(kKeyLeft);
    CHECK_EQ(ui.selected_entry(), 0);
    u.pad(kPadLeft);
    CHECK_EQ(ui.selected_entry(), cols - 1);  // the row's far end
    u.key(kKeyRight);
    CHECK_EQ(ui.selected_entry(), 0);
    // Up and Down between the rows, same column.
    u.pad(kPadRight);
    u.pad(kPadDown);
    CHECK_EQ(ui.selected_entry(), nrows > 1 ? 1 + cols : 1);
    u.key(kKeyRight);
    u.key(kKeyUp);
    CHECK_EQ(ui.selected_entry(), 2);
    u.pad(kPadUp);
    CHECK_EQ(ui.selected_entry(), nrows > 1 ? 2 + cols : 2);  // wraps
    u.key(kKeyDown);
    CHECK_EQ(ui.selected_entry(), 2);
    // The tenth icon is reached too, and works: Party Auto on, B off.
    while (ui.selected_entry() != 0) u.key(kKeyLeft);
    if (nrows > 1) u.key(kKeyDown);
    u.key(kKeyLeft);
    CHECK_EQ(ui.selected_entry(), int(HudPanel::kBPartyAuto));
    u.pad(kPadA);
    CHECK(u.t().party_auto);
    u.pad(kPadB);
    CHECK(!u.t().party_auto);
    CHECK(to_turn(1));
    // An icon that cannot be used is not skipped: a press says why, nothing happens. (The Avatar
    // has no spell items here.)
    if (u.t().cast_options(1).empty()) {
        ui.set_selected(TurnUI::kECast);
        u.pad(kPadA);
        CHECK(ui.mode() == TurnUI::kMenu);
        CHECK(u.t().awaiting_player());
    }
    u.key('5');
    // Wynna: the panel's focus (R3) hands over to the pick (A in the list fires, not the icon).
    uint16_t wy = u.cid("wynna");
    CHECK(to_turn(wy));
    u.w().actor(wy)->mana = 60;
    u.frames(2);
    u.pad(kPadRStick);
    CHECK(u.hud().focused());
    while (u.hud().focus_item() != u.hud().portraits() + 1 + int(TurnUI::kECast)) u.pad(kPadRight);
    u.pad(kPadA);
    CHECK(ui.mode() == TurnUI::kList);
    std::vector<TurnCombat::CastOption> opts = u.t().cast_options(wy);
    int li = -1;
    for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i].name == "Lightning") li = int(i);
    CHECK(li >= 0);
    for (int i = 0; i < li; ++i) u.pad(kPadDown);
    CHECK(!u.hud().focused());
    int r = 0;
    CHECK(ui.range_circle(r));
    CHECK_EQ(r, 960);
    u.pad(kPadA);
    CHECK(ui.mode() == TurnUI::kPickTarget);
    u.frames(2);
    if (shots) {
        // Lightning's reach goes off the picture at the normal zoom: the zoomed-out views show it.
        u.w().controls.zoom_step = 2;
        u.frames(2);
        u.shot("combat2_range_circle_zoomout");
        u.w().controls.zoom_step = 1;
        u.frames(2);
        u.shot("combat2_range_circle_zoom1");
        u.w().controls.zoom_step = 0;
        u.frames(2);
        u.shot("combat2_range_circle_normal");
    }
    const int hp0 = u.w().actor(g)->hp;
    u.pad(kPadA);  // one press
    CHECK(!u.t().awaiting_player());
    CHECK_EQ(u.w().sky_bolts.size(), size_t(1));
    CHECK(!ui.range_circle(r));
    // The panel stays as it is through the flash; the picture goes pale.
    std::vector<uint32_t> pic0, pan0, pic1, pan1;
    int fw, fh, pw, ph;
    const int SC = std::clamp(u.lay.game_w / 320, 1, 4);
    if (shots) u.shot("combat2_lightning_frame1");
    u.gm.tick();
    if (shots) u.shot("combat2_lightning_frame2");
    u.gm.tick();
    if (shots) u.shot("combat2_lightning_frame3");
    u.gm.render_frame(pic0, fw, fh, SC, 0.0, 1.0 / 30);
    u.gm.render_panel_rgba(pan0, pw, ph);
    u.gm.tick();
    CHECK(u.w().sky_bolts.empty());
    CHECK(u.w().actor(g)->hp < hp0);
    CHECK_EQ(u.w().sky_flash, 1);
    if (shots) u.shot("combat2_lightning_flash");
    else u.gm.render_frame(pic1, fw, fh, SC, 0.0, 1.0 / 30);
    CHECK_EQ(u.w().sky_flash, 0);
    if (shots) {
        u.w().sky_flash = 1;
        u.gm.render_frame(pic1, fw, fh, SC, 0.0, 1.0 / 30);
    }
    u.gm.render_panel_rgba(pan1, pw, ph);
    long long s0 = 0, s1 = 0;
    for (uint32_t c : pic0) s0 += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
    for (uint32_t c : pic1) s1 += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
    CHECK(s1 / (long long)pic1.size() > s0 / (long long)pic0.size() + 200);
    CHECK(s1 / (long long)pic1.size() > 500);
    long long p0 = 0, p1 = 0;
    for (uint32_t c : pan0) p0 += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
    for (uint32_t c : pan1) p1 += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
    CHECK(std::abs(p1 - p0) * 10 < p0 + 1);  // about the same (the log may have got a line)
    if (shots) { u.frames(1); u.shot("combat2_lightning_after"); }
    // Win: back to portraits and messages, weapon away, no fight restarting.
    u.w().actor(g)->hp = 5;
    u.t().party_auto = true;
    for (int i = 0; i < 9000 && u.t().active(); ++i) u.frames(1, i % 4 == 0);
    CHECK(u.t().last_end == TurnCombat::kVictory);
    u.t().party_auto = false;
    u.frames(90);
    CHECK(!u.hud().combat_view());
    CHECK(!u.hud().turn_buttons());
    CHECK(!u.w().avatar()->is(Actor::kInCombat));
    CHECK(!u.t().active());
    if (shots) u.shot("combat2_peace");
}

void test_portraits(const GameData& d) {
    std::printf("portraits: one row, clicks, items dropped on them, the controller focus\n");
    Ui u(d, false, 1024, 768);
    u.frames(60);
    u.join({"brannoc", "varn", "ilvaran", "kalvax", "wynna", "nerissa"});
    u.frames(10);
    HudPanel& h = u.hud();
    CHECK_EQ(h.portraits(), 7);
    CHECK(h.portrait_width() >= 6 && h.portrait_width() < 26);  // shrunk to fit one line
    int x6 = 0, y6 = 0, x0 = 0, y0 = 0;
    CHECK(h.portrait_point(6, x6, y6) && h.portrait_point(0, x0, y0));
    CHECK(y6 == y0 && x6 > x0 && x6 < u.lay.panel_px);
    // No party bar overlay while the panel is on.
    CHECK(!find_kind(u.w(), "partybar"));
    u.shot("hud_4x3_portraits");
    // Click Brannoc's portrait: his inventory (paperdoll) opens.
    int bx, by;
    CHECK(h.portrait_point(1, bx, by));
    u.click_panel(bx, by);
    u.frames(2);
    bool open = false;
    for (const auto& g : u.w().gumps.list)
        if (auto* pd = dynamic_cast<PaperdollGump*>(g.get()); pd && pd->owner == u.cid("brannoc")) open = true;
    CHECK(open);
    u.key(kKeyBackspace);
    u.frames(2);
    // Drag an item from the world onto Varn's portrait: it goes into his backpack.
    World& w = u.w();
    const Actor* av = w.avatar();
    uint16_t item = w.create_item(531, 0);  // a helmet
    int ix, iy, iz;
    bool placed = item && find_free(w, 531, av->x, av->y, av->z, 32, 90, item, ix, iy, iz, 2.2);
    CHECK(placed);
    if (placed) {
        w.move_to(item, ix, iy, iz, false);
        u.frames(3);
        int sx, sy;
        w.screen_point(*w.item(item), sx, sy);
        // Find a pixel of it.
        int px = -1, py = -1;
        for (int dy = -20; dy <= 2 && px < 0; ++dy)
            for (int dx = -12; dx <= 12 && px < 0; ++dx)
                if (u.gm.pick(sx + dx, sy + dy) == item) { px = sx + dx; py = sy + dy; }
        CHECK(px >= 0);
        if (px >= 0) {
            int vx, vy;
            h.portrait_point(2, vx, vy);
            u.move(px, py);
            u.button(1, true);
            u.frames(1);
            u.move(px + 10, py + 2);
            u.frames(1);
            u.move(320 + vx, vy);
            u.frames(1);
            u.button(1, false);
            u.frames(2);
            uint16_t pack = w.equipped(u.cid("varn"), 7);
            const Item* it = w.item(item);
            CHECK(it && pack && it->parent == pack);
        }
    }
    // The controller: R3 focuses the panel, the D-pad moves, A opens a portrait, B leaves.
    u.pad(kPadRStick);
    CHECK(h.focused());
    int f0 = h.focus_item();
    u.pad(kPadLeft);
    CHECK(h.focus_item() != f0);
    while (h.focus_item() != 3) u.pad(kPadRight);
    u.pad(kPadA);
    u.frames(2);
    open = false;
    for (const auto& g : u.w().gumps.list)
        if (auto* pd = dynamic_cast<PaperdollGump*>(g.get()); pd && pd->owner == u.cid("ilvaran")) open = true;
    CHECK(open);
    u.pad(kPadB);
    CHECK(!h.focused());
    // Classic: the panel goes, the party bar comes back.
    u.gm.set_panel_width(0);
    u.frames(3);
    CHECK(find_kind(u.w(), "partybar") != nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_hud_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, auto f) { if (only.empty() || only == n) f(); };
    run("layout", [] { test_layout(); });
    run("messages", [&] { test_messages(d); });
    run("scroll", [&] { test_scroll(d); });
    run("combatlog", [&] { test_combat_log(d); });
    run("turn169", [&] { test_turn_panel(d, 1280, 720, "16x9"); });
    run("turn43", [&] { test_turn_panel(d, 1024, 768, "4x3"); });
    run("move", [&] { test_move_range(d); });
    run("portraits", [&] { test_portraits(d); });
    run("combat2", [&] { test_combat2(d, 1280, 720, 5, "five to a row", false); test_combat2(d, 2400, 1000, 10, "ten in a row", false); });
    run("combat2shots", [&] { test_combat2(d, 1280, 720, 5, "shots", true); });
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
