// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6 tests (PLAN-M6.md): Responsive vs Classic controls (instant look, direction
// changes within a tick, instant turns, buffered jumps, latency numbers), the smooth camera
// (sub-tick positions, snapping on teleports), zoom picking, the atlas from the game,
// controller input through the game mode, click-through for barks, movement assist, the edge
// guard and doors without collision when open.
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "check.h"
#include "play.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "game/ask_nav.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/assist.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

namespace {

enum : unsigned { kWalk = 0, kRun = 1, kStand = 2, kJumpUp = 3, kJump = 17, kRunJump = 10, kShakeHead = 11 };

std::vector<uint8_t> snap(const World& w) {
    SaveWriter out;
    w.save_game(out, "m6");
    return out.data();
}

bool is_jump(unsigned a) { return a == 3 || a == 10 || a == 17 || a == 18 || (a >= 19 && a <= 26); }

// A Play world prepared once (past the opening, at the camp) and copied for each scenario
// through a savegame, so every run starts from the same state.
struct Base {
    const GameData& d;
    std::vector<uint8_t> save;
    explicit Base(const GameData& data) : d(data) {
        Play p(d);
        p.settle();
        save = snap(p.w);
    }
};

struct Run {
    World w;
    int frames = 0;
    Run(const Base& b, bool responsive) : w(b.d) {
        w.auto_ui = true;
        w.new_game();
        SaveReader in(b.save.data(), b.save.size());
        w.load_game(in);
        w.controls.responsive = responsive;
        w.controls.assist = responsive;
        frames = int(w.kernel().frame_number());
    }
    void step() {
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    void run(int n) { for (int i = 0; i < n; ++i) step(); }
    Actor& av() { return *w.avatar(); }
    void settle(int max = 300) {
        for (int i = 0; i < max; ++i) {
            if (!w.kernel().count_exact(1, kProcActorAnim) && !av().is(Actor::kAnimLock)) return;
            step();
        }
    }
    void press_right(int dir, int len) {
        w.input.cursor_dir = dir;
        w.input.cursor_len = len;
        w.input.button[2].press(w.input.now_ms, 0, 0);
    }
    void release_right() { w.input.button[2].down = false; w.input.button[2].handled = true; }
};

// --- Latency (ticks from the input to the first visible change) ----------------------------

struct Latency { int start_walk, reverse, turn, jump_walking, buffered_jump; };

Latency measure(const Base& b, bool responsive) {
    Latency L{-1, -1, -1, -1, -1};
    // 1. Start walking from standing.
    {
        Run r(b, responsive);
        r.settle();
        int x0 = r.av().x, y0 = r.av().y;
        r.press_right(0, 1);
        for (int t = 1; t <= 60; ++t) {
            r.step();
            if (r.av().x != x0 || r.av().y != y0) { L.start_walk = t; break; }
        }
        // 2. Reverse direction mid-walk: first tick on which he moves the other way.
        r.run(17);
        int py = r.av().y;
        r.w.input.cursor_dir = 4;
        for (int t = 1; t <= 90; ++t) {
            r.step();
            int dy = r.av().y - py;
            py = r.av().y;
            if (dy > 0) { L.reverse = t; break; }  // dir 0 is -y, dir 4 is +y
        }
        r.release_right();
    }
    // 3. Turning on the spot (the Left key).
    {
        Run r(b, responsive);
        r.settle();
        int d0 = r.av().dir;
        r.w.input.key_turn_left = true;
        for (int t = 1; t <= 60; ++t) {
            r.step();
            if (r.av().dir != d0) { L.turn = t; break; }
        }
        r.w.input.key_turn_left = false;
    }
    // 4. A jump while walking (Space pressed and held).
    {
        Run r(b, responsive);
        r.settle();
        r.press_right(0, 1);
        r.run(20);
        r.w.input.key_jump = true;
        r.w.input.jump_buffer_ms = r.w.input.now_ms;
        for (int t = 1; t <= 60; ++t) {
            r.step();
            if (is_jump(r.av().last_anim)) { L.jump_walking = t; break; }
        }
        r.w.input.key_jump = false;
        r.release_right();
    }
    // 5. A jump tapped (pressed and released at once) during an animation that cannot be
    // interrupted (shaking the head): Responsive keeps it for 300 ms.
    {
        Run r(b, responsive);
        r.settle();
        r.w.do_anim(1, kShakeHead, -1);
        r.run(3);
        r.w.input.key_jump = true;
        r.w.input.jump_buffer_ms = r.w.input.now_ms;
        r.step();
        r.w.input.key_jump = false;
        for (int t = 1; t <= 60; ++t) {
            r.step();
            if (is_jump(r.av().last_anim)) { L.buffered_jump = t; break; }
        }
    }
    return L;
}

void test_latency(const Base& b) {
    Latency c = measure(b, false), r = measure(b, true);
    std::printf("  latency in world ticks (33 ms), Classic / Responsive: start walking %d / %d, reverse mid-walk %d / %d,"
                " turn %d / %d, jump while walking %d / %d, tapped jump during an animation %d / %d (-1: never)\n",
                c.start_walk, r.start_walk, c.reverse, r.reverse, c.turn, r.turn, c.jump_walking, r.jump_walking,
                c.buffered_jump, r.buffered_jump);
    CHECK(r.start_walk >= 1 && r.start_walk <= 2);
    CHECK(r.reverse >= 1 && r.reverse <= 2);        // a direction change takes effect within a tick
    CHECK(c.reverse > r.reverse);
    CHECK(r.turn == 1);                              // turning is instant
    CHECK(c.turn > 1 || c.turn == -1);
    CHECK(r.jump_walking >= 1 && r.jump_walking <= 2);
    CHECK(r.buffered_jump > 0);                      // the tapped jump fires after the animation
    CHECK(c.buffered_jump == -1);                    // Classic forgets it
}

// --- Responsive single click: look at once -------------------------------------------------

struct UI {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit UI(const GameData& d, const Base& b, bool responsive) : gm(d) {
        gm.world().auto_ui = true;
        gm.world().ask_name = false;
        gm.start();
        SaveReader in(b.save.data(), b.save.size());
        gm.world().load_game(in);
        gm.world().controls.responsive = responsive;
        gm.world().controls.assist = responsive;
        frames(3);
    }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; gm.handle(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b;
        e.x = e.y = -9999;
        gm.handle(e);
    }
    void key(int k, bool down = true) { InputEvent e; e.type = down ? InputEvent::kKeyDown : InputEvent::kKeyUp; e.key = k; gm.handle(e); }
    void pad(int b, bool down) { InputEvent e; e.type = down ? InputEvent::kPadDown : InputEvent::kPadUp; e.button = b; gm.handle(e); }
    // A controller acts only after its first button press (that press only wakes it).
    void wake() { pad(kPadGuide, true); pad(kPadGuide, false); }
    void axis(int a, int v) { InputEvent e; e.type = InputEvent::kPadAxis; e.axis = a; e.value = v; gm.handle(e); }
    // Screen point of an item in the last frame.
    bool point_of(uint16_t id, int& x, int& y) {
        for (int yy = 0; yy < 200; ++yy)
            for (int xx = 0; xx < 320; ++xx)
                if (gm.pick(xx, yy) == id) { x = xx; y = yy; return true; }
        return false;
    }
};

int look_delay(const GameData& d, const Base& b, bool responsive) {
    UI u(d, b, responsive);
    World& w = u.gm.world();
    // Something with a look event near the camp: Devon's camp items; take whatever item is
    // drawn just above-left of the Avatar that has a look handler.
    uint16_t target = 0;
    int tx = 0, ty = 0;
    for (int yy = 40; yy < 170 && !target; yy += 3)
        for (int xx = 40; xx < 280 && !target; xx += 3) {
            uint16_t id = u.gm.pick(xx, yy);
            const Item* it = w.item(id);
            if (!it || id == 1 || it->as_actor()) continue;
            int cls = w.usecode_class(*it);
            if (cls >= 0 && d.usecode.event_entry(unsigned(cls), kEvLook)) { target = id; tx = xx; ty = yy; }
        }
    if (!target) return -2;
    uint64_t before = w.events_started;
    u.move(tx, ty);
    u.frames(1);
    u.button(1, true);
    u.frames(1);
    u.button(1, false);
    for (int t = 0; t <= 30; ++t) {
        if (w.events_started > before) return t;
        u.frames(1);
    }
    return -1;
}

void test_click(const GameData& d, const Base& b) {
    int c = look_delay(d, b, false), r = look_delay(d, b, true);
    std::printf("  look after a single click: Classic %d ticks, Responsive %d ticks\n", c, r);
    CHECK(r >= 0 && r <= 1);
    CHECK(c >= 11);
}

// --- The smooth camera ----------------------------------------------------------------------

void test_camera(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    w.controls.smooth_scroll = true;
    std::vector<uint32_t> px;
    int fw, fh;
    // Walk north and look at the camera between ticks.
    w.input.cursor_dir = 0;
    w.input.cursor_len = 1;
    u.move(160, 30);
    u.button(3, true);
    double s0x = 0, s0y = 0, s1x = 0, s1y = 0, smx = 0, smy = 0;
    int sub = 0, tries = 0;
    for (int i = 0; i < 60; ++i) {
        u.gm.tick();
        u.gm.render_frame(px, fw, fh, 3, 0.0, 1.0 / 60);
        u.gm.view_camera(s0x, s0y);
        u.gm.render_frame(px, fw, fh, 3, 0.5, 1.0 / 60);
        u.gm.view_camera(smx, smy);
        u.gm.render_frame(px, fw, fh, 3, 0.999, 1.0 / 60);
        u.gm.view_camera(s1x, s1y);
        if (i > 10) {
            ++tries;
            bool between = (smy != s0y || smx != s0x) && std::abs(smy - s0y) < std::abs(s1y - s0y) + 1e-9;
            if (between) ++sub;
        }
    }
    u.button(3, false);
    std::printf("  smooth camera: %d of %d walking ticks have a different camera half way to the next tick (frame %dx%d)\n",
                sub, tries, fw, fh);
    CHECK(sub > tries / 3);
    CHECK_EQ(fw, 960);
    // A teleport snaps: right after it the camera is on the Avatar.
    Actor* a = w.avatar();
    w.teleport_actor(1, a->x + 3000, a->y + 2000, a->z, w.map().map_number());
    u.gm.tick();
    u.gm.render_frame(px, fw, fh, 3, 0.0, 1.0 / 60);
    double cx, cy;
    u.gm.view_camera(cx, cy);
    int ax = a->x, ay = a->y, az = a->z + 20;
    double ex = (ax - ay) / 4, ey = (ax + ay) / 8 - az;
    std::printf("  teleport: camera %.1f,%.1f, the Avatar's point %.1f,%.1f\n", cx, cy, ex, ey);
    CHECK(std::abs(cx - ex) < 1 && std::abs(cy - ey) < 1);
}

// --- Zoom: picking at every level ---------------------------------------------------------

void test_zoom(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    std::vector<uint32_t> px;
    int fw, fh;
    // Without debug mode the view goes out to twice the normal at most; with it, all the way.
    for (int S : {2, 3, 4}) {
        u.gm.debug().force(false);
        u.gm.debug().set_enabled(false);
        w.controls.zoom_step = 0;
        u.gm.render_frame(px, fw, fh, S, 0.0, 1.0 / 30);
        for (int i = 0; i < 12; ++i) u.gm.zoom_by(1);
        int n, dd;
        u.gm.zoom_fraction(n, dd);
        std::printf("  zoom limit at %dx: %d/%d without debug mode\n", S, n, dd);
        CHECK(2 * n >= dd);
        CHECK(n < dd);
        u.gm.debug().force(true);
        for (int i = 0; i < 12; ++i) u.gm.zoom_by(1);
        u.gm.zoom_fraction(n, dd);
        std::printf("  ... with debug mode: %d/%d\n", n, dd);
        CHECK(2 * n < dd);
        u.gm.debug().force(false);
    }
    u.gm.debug().force(true);  // the far zoom levels below
    for (int step = 0; step <= 3 + GameMode::kFarSteps; ++step) {
        w.controls.zoom_step = step;
        u.gm.tick();
        u.gm.render_frame(px, fw, fh, 4, 0.0, 1.0 / 30);
        int n, dd;
        u.gm.zoom_fraction(n, dd);
        // The Avatar's own screen point (through the view's mapping) picks him; so do points
        // over his body.
        int sx, sy;
        w.screen_point(*w.avatar(), sx, sy);
        int hits = 0, tries = 0;
        for (int dy = 1; dy <= 14; dy += 2) {
            ++tries;
            // Points over his body: 14 native pixels up at this zoom (at least one sample at far zoom).
            float yy = sy - std::max(0.26f, dy * float(n) / dd);
            if (u.gm.pick_f(sx + 0.5f, yy) == 1 || u.gm.pick_f(sx + 0.25f, yy) == 1) ++hits;
        }
        // And the world point under the cursor matches the item picked.
        u.move(sx, sy - 4);
        u.gm.tick();
        std::printf("  zoom %d%% (frame %dx%d, world surface for %d%%): the Avatar picked at %d of %d points above his feet\n",
                    100 * n / dd, fw, fh, 100 * n / dd, hits, tries);
        CHECK(hits >= tries / 2);
        if (step <= 3) CHECK(w.view_w >= 320 * dd / n);
        else std::printf("    far zoom: world %.2f ms, %d tiles still to build\n", u.gm.world_ms(), u.gm.far_missing());
    }
    w.controls.zoom_step = 0;
}

// --- The atlas from the game --------------------------------------------------------------

void test_atlas(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    u.gm.set_atlas_layout_path("data/atlas_layout.json");
    u.frames(2);
    std::vector<uint8_t> before = snap(w);
    int f0 = u.gm.frames();
    u.key('m');
    CHECK(u.gm.atlas_open());
    std::vector<uint32_t> px;
    int fw, fh;
    for (int i = 0; i < 20; ++i) {
        u.gm.tick();
        u.gm.render_frame(px, fw, fh, 3, 0, 1.0 / 30);
    }
    CHECK_EQ(fw, 640);
    // Clicks and drags only pan; they do not teleport.
    u.move(100, 100);
    u.button(1, true);
    u.button(1, false);
    u.move(120, 90);
    u.button(1, true);
    u.move(160, 100);
    u.button(1, false);
    u.key('=');
    u.frames(3);
    u.key(kKeyTab);
    CHECK(!u.gm.atlas_open());
    std::vector<uint8_t> after = snap(w);
    CHECK(before == after);
    CHECK_EQ(u.gm.frames(), f0);
    std::printf("  atlas from the game: opened with M, panned and zoomed, closed with Tab: the world is byte-identical (%zu bytes)\n",
                after.size());
}

// --- The controller through the game mode -------------------------------------------------

void test_pad(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    InputEvent add;
    add.type = InputEvent::kPadAdded;
    std::strcpy(add.name, "Test pad");
    u.gm.handle(add);
    u.wake();
    CHECK(u.gm.pad_connected());
    w.teleport_actor(1, 16300, 12300, 48, 39);
    w.camera_follow(1);
    u.frames(30);
    Actor* a = w.avatar();
    int x0 = a->x, y0 = a->y;
    // Left stick up (screen north = world direction 7 on this projection).
    u.axis(kAxisLY, -20000);
    u.frames(30);
    int walked = std::abs(a->x - x0) + std::abs(a->y - y0);
    unsigned walk_anim = a->last_anim;
    u.axis(kAxisLY, -32767);
    unsigned run_anim = 0;
    for (int i = 0; i < 30; ++i) { u.frames(1); if (a->last_anim == kRun) run_anim = kRun; }
    u.axis(kAxisLY, 0);
    u.frames(20);
    std::printf("  pad: half stick (up = world direction %d) walked %d units (anim %u), full stick anim %u\n", w.input.pad_dir, walked, walk_anim, run_anim);
    CHECK(walked > 40);
    CHECK_EQ(walk_anim, kWalk);
    CHECK_EQ(run_anim, kRun);
    // X: jump.
    u.pad(kPadX, true);
    u.frames(2);
    u.pad(kPadX, false);
    bool jumped = is_jump(a->last_anim);
    u.frames(40);
    CHECK(jumped);
    // RB: combat on, again: off.
    u.pad(kPadRB, true); u.pad(kPadRB, false);
    u.frames(30);
    CHECK(a->is(Actor::kInCombat));
    u.pad(kPadRB, true); u.pad(kPadRB, false);
    u.frames(30);
    CHECK(!a->is(Actor::kInCombat));
    // D-pad Right cycles the auto-target; X uses it.
    u.frames(2);
    size_t cands = u.gm.target_candidates().size();
    uint16_t t0 = u.gm.auto_target();
    u.pad(kPadRight, true); u.pad(kPadRight, false);
    u.frames(1);
    uint16_t t1 = u.gm.auto_target();
    std::printf("  pad: %zu auto-target candidates, first %u, after D-pad Right %u\n", cands, t0, t1);
    CHECK(cands >= 1);
    if (cands >= 2) CHECK(t1 != t0);
    // A is the left button on the auto-target: a tap looks at it at once (the pointer jumps
    // onto it), a second tap uses it.
    uint64_t ev = w.events_started;
    u.pad(kPadA, true); u.pad(kPadA, false);
    u.frames(1);
    bool looked = w.events_started > ev;
    CHECK(u.gm.pick(u.gm.mouse_x(), u.gm.mouse_y()) == t1);
    uint64_t ev2 = w.events_started;
    u.pad(kPadA, true); u.pad(kPadA, false);
    u.frames(2);
    // (Out of reach: the red cross, or a walk there first, then the use: app/auto_walk.cpp.)
    bool walk_first = u.gm.auto_walk_pending();
    for (int f = 0; f < 300 && u.gm.auto_walk_pending(); ++f) u.frames(1);
    bool used = w.events_started > ev2 || w.red_cross_frames > 0;
    std::printf("  pad A on the target %u: tap looked %d, second tap used %d (walked there first %d)\n", t1, looked, used, walk_first);
    CHECK(looked);
    CHECK(used);
    u.frames(20);
    w.gumps.close_item_dependent();
    u.frames(15);
    // Y: the backpack window; then the mini-stats window. B closes the window under the
    // pointer first (the older backpack), then the newest (the stats).
    u.pad(kPadY, true); u.pad(kPadY, false);
    u.frames(10);
    ContainerGump* pack = w.gumps.find<ContainerGump>();
    CHECK(pack != nullptr);
    w.gumps.add(std::make_unique<MiniStatsGump>(w));
    u.frames(2);
    if (pack) {
        int px = -1, py = -1;
        for (int yy = pack->y + 2; yy < pack->y + pack->h - 2 && px < 0; yy += 3)
            for (int xx = pack->x + 2; xx < pack->x + pack->w - 6 && px < 0; xx += 3)
                if (w.gumps.at(xx, yy) == pack && w.gumps.at(xx + 4, yy) == pack) { px = xx; py = yy; }
        u.move(px, py);
        u.axis(kAxisRX, 9000); u.frames(1); u.axis(kAxisRX, 0);  // the right stick: the pointer is in use
        u.frames(1);
        bool under = w.gumps.at(u.gm.mouse_x(), u.gm.mouse_y()) == pack;
        u.pad(kPadB, true); u.pad(kPadB, false);
        u.frames(1);
        bool pack_closed = w.gumps.find<ContainerGump>() == nullptr, stats_open = w.gumps.find<MiniStatsGump>() != nullptr;
        u.move(300, 10);  // the real mouse: the pointer is idle
        u.pad(kPadB, true); u.pad(kPadB, false);
        u.frames(1);
        bool stats_closed = w.gumps.find<MiniStatsGump>() == nullptr;
        std::printf("  pad B: pointer over the backpack (%d) closed it %d (stats kept %d); next B closed the stats %d\n",
                    under, pack_closed, stats_open, stats_closed);
        CHECK(under && pack_closed && stats_open && stats_closed);
    }
    // Start: the menu; B closes it.
    u.pad(kPadStart, true); u.pad(kPadStart, false);
    u.frames(2);
    CHECK(w.gumps.find<MenuGump>() != nullptr);
    u.pad(kPadB, true); u.pad(kPadB, false);
    u.frames(2);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    // D-pad Down: zoom out (needs a composed frame of scale >= 2 to know the limit).
    std::vector<uint32_t> px;
    int fw, fh;
    u.gm.render_frame(px, fw, fh, 4, 0, 1.0 / 30);
    u.pad(kPadDown, true); u.pad(kPadDown, false);
    CHECK_EQ(w.controls.zoom_step, 1);
    u.pad(kPadUp, true); u.pad(kPadUp, false);
    CHECK_EQ(w.controls.zoom_step, 0);
    // Back: the atlas; Back again closes it. Holding Back: help.
    u.pad(kPadBack, true); u.frames(2); u.pad(kPadBack, false);
    CHECK(u.gm.atlas_open());
    u.pad(kPadBack, true); u.pad(kPadBack, false);
    CHECK(!u.gm.atlas_open());
    u.pad(kPadBack, true);
    u.frames(35);
    CHECK(u.gm.help_open());
    u.pad(kPadBack, false);
    u.pad(kPadB, true); u.pad(kPadB, false);
    CHECK(!u.gm.help_open());
    // The right stick moves the pointer; while it is active A clicks.
    int mx = u.gm.mouse_x();
    u.axis(kAxisRX, 30000);
    u.frames(5);
    u.axis(kAxisRX, 0);
    CHECK(u.gm.mouse_x() > mx + 10);
    CHECK(u.gm.pointer_active());
    // Unplugging.
    InputEvent rem;
    rem.type = InputEvent::kPadRemoved;
    u.gm.handle(rem);
    CHECK(!u.gm.pad_connected());
}

// --- Click-through ---------------------------------------------------------------------------

struct OpeningResult { int frames = 0, ticks = 0; unsigned skipped = 0; std::vector<uint8_t> globals; bool released = false; };

OpeningResult opening(const GameData& d, bool click) {
    OpeningResult r;
    GameMode gm(d);
    World& w = gm.world();
    w.auto_ui = true;  // answers the menus
    w.ask_name = false;
    w.controls.click_text = true;
    gm.start();
    Surface8 s;
    int free_run = 0;
    bool asked = false;
    for (int i = 0; i < 12000; ++i) {
        if (click && w.stasis && !w.barks.empty() && i % 3 == 0) {
            InputEvent e;
            e.type = InputEvent::kMouseDown;
            e.button = 1;
            e.x = 200; e.y = 40;
            gm.handle(e);
            e.type = InputEvent::kMouseUp;
            gm.handle(e);
        }
        gm.tick();
        if (i % 50 == 0) gm.render(s);
        ++r.ticks;
        // The scene is over when the conversation has been held and then nothing has been
        // said or asked, and the Avatar has been free, for 10 seconds.
        if (w.ask_menu) asked = true;
        free_run = (w.stasis || w.ask_menu || !w.barks.empty()) ? 0 : free_run + 1;
        if (asked && free_run >= 300) { r.released = true; break; }
    }
    r.frames = int(w.kernel().frame_number()) - 300;
    r.skipped = gm.pages_skipped();
    // The usecode globals at the end of the scene.
    SaveWriter out;
    w.vm().save(out);
    r.globals = out.data();
    return r;
}

void test_click_through(const GameData& d) {
    OpeningResult a = opening(d, false), b = opening(d, true);
    std::printf("  opening scene: %d world frames without clicks, %d with clicks (%u pages ended early); "
                "VM state %s\n", a.frames, b.frames, b.skipped, a.globals == b.globals ? "identical" : "differs");
    CHECK(a.released && b.released);
    CHECK(b.frames < a.frames * 2 / 3);
    CHECK(b.skipped >= 3);
    CHECK(a.globals == b.globals);
}

// --- The click-through guard: 0.5 s per page, one advance per press, hold after the guard ----

void test_click_guard(const GameData& d) {
    GameMode gm(d);
    World& w = gm.world();
    w.auto_ui = false;
    w.ask_name = false;
    w.skip_opening = true;  // parts 1-3: a speech made here; part 4: the opening's menus
    gm.start();
    Surface8 s;
    auto frame = [&]() { gm.tick(); gm.render(s); };
    auto click = [&](bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = 1;
        e.x = 300; e.y = 20;
        gm.handle(e);
    };
    // A long speech in a scene (stasis): many pages.
    for (int i = 0; i < 200; ++i) frame();
    std::string text;
    for (int k = 0; k < 40; ++k) text += "These are the words of a long speech, page after page of them. ";
    auto long_bark = [&]() {
        w.stasis = true;
        w.close_bark(1);
        w.bark(1, text);
        for (int i = 0; i < 20; ++i) frame();  // the first page is older than the guard
    };
    long_bark();
    std::printf("  click guard: a speech of %zu pages\n", w.barks.empty() ? 0 : w.barks[0].pages.size());
    CHECK(!w.barks.empty() && w.barks[0].pages.size() >= 8);
    // 1. Rapid clicks (every 2 frames for 2 s): advances at least 15 frames apart.
    std::vector<int> at;
    unsigned before = gm.pages_skipped();
    for (int f = 0; f < 60 && !w.ask_menu; ++f) {
        if (f % 2 == 0) { click(true); click(false); }
        unsigned now = gm.pages_skipped();
        if (now != before) { at.push_back(f); before = now; }
        frame();
    }
    int min_gap = 1000;
    for (size_t i = 1; i < at.size(); ++i) min_gap = std::min(min_gap, at[i] - at[i - 1]);
    std::printf("  click guard: rapid clicks for 2 s advanced %zu pages, at least %d frames apart\n", at.size(),
                at.size() > 1 ? min_gap : 0);
    CHECK(!at.empty());
    if (at.size() > 1) CHECK(min_gap >= GameMode::kTextGuardFrames);
    // 2. A double-click on an old page advances one page.
    long_bark();
    if (!w.barks.empty() && !w.ask_menu) {
        unsigned b0 = gm.pages_skipped();
        click(true); click(false);
        frame(); frame();
        click(true); click(false);
        frame();
        std::printf("  click guard: a double-click advanced %u page(s)\n", gm.pages_skipped() - b0);
        CHECK_EQ(gm.pages_skipped() - b0, 1u);
    }
    // 3. Holding: the press advances one page; nothing more until the hold passes 0.5 s.
    long_bark();
    if (!w.barks.empty() && !w.ask_menu) {
        unsigned b0 = gm.pages_skipped();
        click(true);
        for (int i = 0; i < GameMode::kTextGuardFrames - 2; ++i) frame();
        unsigned early = gm.pages_skipped() - b0;
        for (int i = 0; i < 40 && !w.ask_menu; ++i) frame();
        unsigned late = gm.pages_skipped() - b0;
        click(false);
        std::printf("  click guard: holding advanced %u page(s) in its first 0.5 s, %u after 1.8 s\n", early, late);
        CHECK(early <= 1);
        CHECK(late > early || w.ask_menu);
    }
    // 4. The answer menu ignores clicks for 0.5 s: in the opening conversation, answer the first
    // menu with a key, then click the next menu's first answer as soon as it appears.
    GameMode gm2(d);
    World& w2 = gm2.world();
    w2.auto_ui = false;
    w2.ask_name = false;
    gm2.start();
    auto frame2 = [&]() { gm2.tick(); gm2.render(s); };
    for (int i = 0; i < 6000 && !w2.ask_menu; ++i) frame2();
    uint16_t first = w2.ask_menu ? w2.ask_menu->pid : 0;
    InputEvent k;
    k.type = InputEvent::kKeyDown; k.key = '1';
    gm2.handle(k);
    for (int i = 0; i < 6000 && !(w2.ask_menu && w2.ask_menu->pid != first); ++i) {
        if (w2.stasis && !w2.barks.empty() && i % 20 == 10) {
            InputEvent e;
            e.type = InputEvent::kKeyDown; e.key = kKeyReturn;
            gm2.handle(e);
            e.type = InputEvent::kKeyUp;
            gm2.handle(e);
        }
        frame2();
    }
    {
        GameMode& gm = gm2;
        World& w = w2;
        auto frame = frame2;
    CHECK(w.ask_menu != nullptr);
    if (w.ask_menu) {
        uint16_t pid = w.ask_menu->pid;
        int bx, by;
        frame();
        CHECK(gm.ask_button_point(0, bx, by));
        InputEvent mv;
        mv.type = InputEvent::kMouseMove; mv.x = bx; mv.y = by;
        gm.handle(mv);
        InputEvent e;
        e.type = InputEvent::kMouseDown; e.button = 1; e.x = bx; e.y = by;
        std::printf("    menu: frames_open %d before the click (button at %d,%d)\n", w.ask_menu ? w.ask_menu->frames_open : -1, bx, by);
        gm.handle(e);
        e.type = InputEvent::kMouseUp;
        gm.handle(e);
        std::printf("    menu: frames_open %d at the click, menu %s right after\n", w.ask_menu ? w.ask_menu->frames_open : -1,
                    w.ask_menu ? "open" : "gone");
        frame();
        bool kept = w.ask_menu && w.ask_menu->pid == pid;
        for (int i = 0; i < GameMode::kTextGuardFrames; ++i) frame();
        e.type = InputEvent::kMouseDown;
        gm.handle(e);
        e.type = InputEvent::kMouseUp;
        gm.handle(e);
        frame();
        bool chosen = !w.ask_menu || w.ask_menu->pid != pid;
        std::printf("  click guard: a click on an answer at once %s, after 0.5 s %s\n", kept ? "ignored" : "CHOSE",
                    chosen ? "chose it" : "ignored");
        CHECK(kept);
        CHECK(chosen);
    }
    }
}

// --- Doors without collision when open, opening through the Avatar, no trapping ------------

bool inside_solid(World& w) {
    const Actor* a = w.avatar();
    Box b = w.box_of(*a);
    return !w.can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, w.shape_flags(*a), 1).valid;
}

void test_doors(const Base& b) {
    int opened[2] = {0, 0}, tried = 0, stuck = 0, trapped = 0, refused_close = 0, moved_out = 0;
    for (int mode = 0; mode < 2; ++mode) {
        Run r(b, mode == 1);
        r.w.teleport_actor(1, 16300, 12300, 48, 39);
        r.run(20);
        std::vector<uint16_t> all;
        r.w.area_search(16384, 12000, 2600, false, all);
        int n = 0;
        for (uint16_t id : all) {
            Item* door = r.w.item(id);
            if (!door || !r.w.is_door_shape(door->shape) || r.w.door_is_open(*door) || (door->quality & 0xFF)) continue;
            if (++n > 8) break;
            Box db = r.w.box_of(*door);
            // Stand in the middle of the door's swing: the square beside its hinge corner (the
            // open leaf turns about the item's max corner, observed: the shape swaps NS <-> EW
            // with the same corner).
            int ax = db.x - 8, ay = db.y - 8;
            bool ew = db.xd > db.yd;
            int sx = ew ? ax : db.x - 40, sy = ew ? db.y - 40 : ay;
            if (mode == 0) ++tried;
            r.w.move_to(1, sx + 32, sy + 32, door->z, false);
            r.run(2);
            r.w.call_event(id, kEvUse);
            r.run(240);
            door = r.w.item(id);
            if (door && r.w.door_is_open(*door)) ++opened[mode];
            else {
                std::string bk;
                for (const Bark& k : r.w.barks) if (!k.pages.empty() && !k.pages[0].lines.empty()) bk += k.pages[0].lines[0] + "|";
                std::printf("    door %u (%u/%u q %u at %d,%d,%d box %dx%d) not opened, mode %d, avatar at %d,%d: %s\n", id,
                            door ? door->shape : 0, door ? door->frame : 0, door ? door->quality : 0, db.x, db.y, db.z, db.xd,
                            db.yd, mode, r.av().x, r.av().y, bk.c_str());
            }
            if (mode == 1 && inside_solid(r.w)) ++stuck;
            // Close it with the Avatar standing in the doorway (the closed door's place).
            if (door && r.w.door_is_open(*door) && mode == 1) {
                r.w.move_to(1, db.x - db.xd / 2 + 32, db.y - db.yd / 2 + 32, door->z, false);
                r.run(2);
                int bx = r.av().x, by = r.av().y;
                r.w.call_event(id, kEvUse);
                r.run(60);
                door = r.w.item(id);
                if (inside_solid(r.w)) ++trapped;
                if (door && r.w.door_is_open(*door)) ++refused_close;
                else if (r.av().x != bx || r.av().y != by) ++moved_out;
                if (door && r.w.door_is_open(*door)) { r.w.call_event(id, kEvUse); r.run(60); }
            }
        }
    }
    std::printf("  doors with the Avatar in the swing: %d of %d opened in Classic, %d in Responsive; Responsive: %d left him"
                " inside something; closing on him: %d trapped, %d refused, %d moved him out\n",
                opened[0], tried, opened[1], stuck, trapped, refused_close, moved_out);
    CHECK(tried >= 5);
    CHECK(opened[1] >= tried - 1);
    CHECK(opened[0] < opened[1]);
    CHECK_EQ(stuck, 0);
    CHECK_EQ(trapped, 0);
}

// The open leaf of door 8367 (Tenebrae, map 39): walking through its area.
void test_door_walk(const Base& b) {
    for (int mode = 0; mode < 2; ++mode) {
        Run r(b, mode == 1);
        r.w.teleport_actor(1, 16300, 12300, 48, 39);
        r.run(20);
        uint16_t door = 0;
        std::vector<uint16_t> all;
        r.w.area_search(16191, 11807, 64, false, all);
        for (uint16_t id : all) if (const Item* it = r.w.item(id); it && r.w.is_door_shape(it->shape)) door = id;
        CHECK(door != 0);
        if (!door) return;
        r.w.move_to(1, 16180, 11950, 48, false);
        r.run(2);
        r.w.call_event(door, kEvUse);
        r.run(60);
        // Walk north along the hinge side, right through where the open leaf stands.
        r.w.move_to(1, 16190, 11900, 48, false);
        r.run(2);
        r.press_right(0, 1);
        int f = 0;
        for (; f < 150 && r.av().y > 11650; ++f) r.step();
        r.release_right();
        std::printf("  door 8367 open, walking through its leaf on the hinge side (%s): y %d after %d frames\n",
                    mode ? "Responsive" : "Classic", r.av().y, f);
        if (mode == 1) CHECK(r.av().y <= 11650);
    }
}

// Movement assist: approaching the open doorway of door 8367 with the body overlapping the
// wall by 6, 20 and 40 units.
void test_assist(const Base& b) {
    for (int off : {6, 20, 40}) {
        for (int mode = 0; mode < 2; ++mode) {
            Run r(b, mode == 1);
            r.w.teleport_actor(1, 16300, 12300, 48, 39);
            r.run(20);
            uint16_t door = 0;
            std::vector<uint16_t> all;
            r.w.area_search(16191, 11807, 64, false, all);
            for (uint16_t id : all) if (const Item* it = r.w.item(id); it && r.w.is_door_shape(it->shape)) door = id;
            if (!door) { CHECK(door != 0); return; }
            const Item* d0 = r.w.item(door);
            int x0 = d0->x - 128;  // the doorway's west edge (the closed door spans 128 in x)
            r.w.move_to(1, 16000, 11950, 48, false);
            r.run(2);
            r.w.call_event(door, kEvUse);
            r.run(60);
            // The Avatar's box (64 wide) straight south of the doorway, `off` units over its west
            // wall: x max = west edge + 64 - off.
            r.w.move_to(1, x0 + 64 - off, 11920, 48, false);
            r.run(3);
            r.press_right(0, 1);
            r.w.debug_assist = off == 40 && mode == 1;
            int f = 0;
            for (; f < 240 && r.av().y > 11700; ++f) { r.step(); if (r.w.debug_assist && f < 60) std::fprintf(stderr, "AS f %d at %d,%d anim %u\n", f, r.av().x, r.av().y, r.av().last_anim); }
            r.release_right();
            std::printf("  assist, doorway %d units off (%s): %s after %d frames (x %d)\n", off,
                        mode ? "Responsive" : "Classic", r.av().y <= 11700 ? "through" : "not through", f, r.av().x);
            if (mode == 1) CHECK(r.av().y <= 11700);
        }
    }
}

// The edge guard: pushing toward the water from the shore near the camp stops at the edge; a
// jump still goes in.
void test_edge(const Base& b) {
    for (int mode = 0; mode < 2; ++mode) {
        Run r(b, mode == 1);
        r.settle();
        // The nearest water.
        std::vector<uint16_t> near;
        r.w.area_search(r.av().x, r.av().y, 1200, false, near);
        int best = 1 << 30;
        int wx = 0, wy = 0;
        for (uint16_t id : near) {
            const Item* it = r.w.item(id);
            if (!it || !(r.w.data().typeflags.get(it->shape).flags() & ShapeInfo::kSea)) continue;
            int dd = std::abs(it->x - r.av().x) + std::abs(it->y - r.av().y);
            if (dd < best) { best = dd; wx = it->x - 64; wy = it->y - 64; }
        }
        CHECK(best < (1 << 30));
        int dir = direction_to(wx - r.av().x, wy - r.av().y);
        unsigned hp0 = r.av().hp;
        r.press_right(dir, 1);
        bool wet = false;
        int f = 0;
        for (; f < 400; ++f) {
            r.w.input.cursor_dir = dir;
            r.step();
            if (r.av().chunk < 0 || r.av().is(Actor::kDead) || r.av().hp < hp0 ||
                assist::dangerous(r.w, r.av(), r.av().x, r.av().y, r.av().z)) { wet = true; break; }
        }
        r.release_right();
        std::printf("  edge guard (%s): walking toward the water at %d,%d (direction %d): %s after %d frames at %d,%d\n",
                    mode ? "Responsive" : "Classic", wx, wy, dir, wet ? "in the water" : "stopped at the edge", f,
                    r.av().x, r.av().y);
        if (mode == 1) {
            CHECK(!wet);
            // A deliberate jump goes in.
            r.settle();
            // The controller's A with the stick toward the water: a jump that way.
            r.w.input.pad_move = true;
            r.w.input.pad_dir = dir;
            r.w.input.pad_len = 1;
            r.w.input.pad_jump = true;
            r.w.input.jump_buffer_ms = std::max<uint32_t>(1, r.w.input.now_ms);
            bool in = false;
            for (int k = 0; k < 120 && !in; ++k) {
                r.step();
                if (k == 8) r.w.input.pad_move = false;
                in = r.av().chunk < 0 || r.av().is(Actor::kDead) || r.av().hp < hp0 ||
                     assist::dangerous(r.w, r.av(), r.av().x, r.av().y, r.av().z);
            }
            std::printf("  edge guard: a jump toward the water %s\n", in ? "goes in" : "does not go in");
            CHECK(in);
        }
    }
}

// --- Responsive jumps: forward, and landing magnetism ---------------------------------------

// Jumps with the controller's A: `walk` frames of stick in `dir` first (0 = standing, len 2 =
// running). Returns the take-off and the resting point.
struct JumpResult { int x0, y0, x1, y1, z1; unsigned action; bool wet; uint16_t on; };

JumpResult pad_jump(Run& r, int dir, int walk, int len) {
    JumpResult j{};
    PlayerInput& in = r.w.input;
    if (walk > 0) {
        in.pad_move = true;
        in.pad_dir = dir;
        in.pad_len = len;
        r.run(walk);
    } else {
        // Face the direction first.
        r.av().dir = uint8_t(dir);
        r.run(2);
    }
    j.x0 = r.av().x;
    j.y0 = r.av().y;
    in.pad_jump = true;
    in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
    j.action = 999;
    for (int k = 0; k < 6; ++k) {
        r.step();
        unsigned a = r.av().last_anim;
        if (j.action == 999 && is_jump(a)) j.action = a;
    }
    in.pad_move = false;
    in.pad_jump = false;
    r.settle(200);
    r.run(10);
    r.settle(200);
    j.x1 = r.av().x; j.y1 = r.av().y; j.z1 = r.av().z;
    j.wet = r.av().chunk < 0 || r.av().is(Actor::kDead) || assist::dangerous(r.w, r.av(), j.x1, j.y1, j.z1);
    int top; uint16_t f;
    assist::surface_under(r.w, j.x1 - 32, j.y1 - 32, j.z1, 1, top, f, j.on);
    return j;
}

int dist(const JumpResult& j) { return int(std::hypot(double(j.x1 - j.x0), double(j.y1 - j.y0))); }

// Over the water east of the camp: the shore where the edge guard stops him.
struct Shore { int x, y, z; };

Shore shore(const Base& b) {
    Run r(b, true);
    r.settle();
    r.press_right(2, 1);
    for (int f = 0; f < 200; ++f) { r.w.input.cursor_dir = 2; r.step(); }
    r.release_right();
    r.settle();
    return {r.av().x, r.av().y, r.av().z};
}

uint16_t place_floor(Run& r, unsigned shape, int cx, int cy, int z) {
    uint16_t p = r.w.create_item(shape, 0);
    int xd, yd, zd;
    r.w.footprint(*r.w.item(p), xd, yd, zd);
    r.w.move_to(p, cx + xd / 2, cy + yd / 2, z, false);
    return p;
}

// A raised course 160 above the ground west of the camp's shore (a drop the edge guard calls
// deadly): a runway of shape-70 floors (128 x 128) from x0 to x1 along y, and the Avatar on
// its west end.
struct Course {
    int y, z, x0, x1;
};
Course build_runway(Run& r, const Shore& s, int len) {
    Course c;
    c.y = s.y - 32 - 600;
    c.z = s.z + 160;
    c.x1 = s.x - 400;
    c.x0 = c.x1 - len;
    for (int x = c.x0; x < c.x1; x += 128) place_floor(r, 70, x + 64, c.y, c.z);
    r.w.move_to(1, c.x0 + 64 + 32, c.y + 32, c.z + 8, false);
    r.run(3);
    r.settle();
    return c;
}

// Runs (len 2) or walks (len 1) east along the runway and jumps with A at x >= at (0: jump at
// once, standing). Returns the take-off and the rest point.
JumpResult course_jump(Run& r, int at, int len) {
    PlayerInput& in = r.w.input;
    if (at > 0) {
        in.pad_move = true;
        in.pad_dir = 2;
        in.pad_len = len;
        for (int f = 0; f < 200 && r.av().x < at; ++f) r.step();
    } else {
        r.av().dir = 2;
    }
    return pad_jump(r, 2, 0, len);
}

void test_jumps(const Base& b, const Shore& s) {
    // Standing, walking and running jumps east along a long runway (flat, open, safe).
    int d[2][3] = {};
    unsigned act[2][3] = {};
    for (int mode = 0; mode < 2; ++mode)
        for (int kind = 0; kind < 3; ++kind) {
            Run r(b, mode == 1);
            Course c = build_runway(r, s, 128 * 14);
            JumpResult j = course_jump(r, kind == 0 ? 0 : c.x0 + (kind == 1 ? 260 : 500), kind == 2 ? 2 : 1);
            d[mode][kind] = dist(j);
            act[mode][kind] = j.action;
        }
    std::printf("  jumps on a flat runway (take-off to rest, units), Classic / Responsive: standing %d (action %u) / %d (%u),"
                " walking %d (%u) / %d (%u), running %d (%u) / %d (%u)\n",
                d[0][0], act[0][0], d[1][0], act[1][0], d[0][1], act[0][1], d[1][1], act[1][1], d[0][2], act[0][2], d[1][2], act[1][2]);
    CHECK(d[1][0] >= 64);                  // a standing jump goes forward
    CHECK(d[0][0] < 16);                   // Classic: straight up
    CHECK(d[1][1] > d[1][0]);
    CHECK(d[1][2] > d[1][1]);
}

void test_jump_magnet(const Base& b, const Shore& s) {
    // A standing hop from the end of a short runway (one floor): nothing ahead, a platform
    // (shape 524, 96 x 96) at 0 / 20 / 40 degrees.
    auto hop = [&](int deg, int at, bool place, uint16_t& plat) {
        Run r(b, true);
        Course c = build_runway(r, s, 128);
        plat = 0;
        int cx = c.x0 + 64, cy = c.y;  // the floor's centre (he stands on it)
        if (place) {
            double a = deg * M_PI / 180;
            plat = place_floor(r, 524, cx + int(at * std::cos(a)), cy + int(at * std::sin(a)), c.z);
        }
        r.w.debug_assist = true;
        JumpResult j = course_jump(r, 0, 1);
        r.w.debug_assist = false;
        j.z1 -= c.z;
        return j;
    };
    uint16_t plat;
    JumpResult none = hop(0, 0, false, plat);
    std::printf("  hop off a floor 160 up with nothing ahead: came down %d below it, %d units away\n", -none.z1, dist(none));
    CHECK(none.z1 < 0);  // no platform: the jump stays as it is
    // (A 96-unit platform centred 40 degrees off at 150 units still reaches into the 30 degree
    // arc with its near corner, so the 40 degree cases are measured at 176 and 45 at 150.)
    const int cases[][2] = {{0, 150}, {0, 176}, {20, 150}, {20, 176}, {40, 176}, {45, 150}};
    for (const auto& cs : cases) {
        JumpResult j = hop(cs[0], cs[1], true, plat);
        bool on = j.on == plat;
        std::printf("  hop, platform %d degrees off at %d units: %s\n", cs[0], cs[1], on ? "landed on it" : "missed");
        if (cs[0] <= 20) CHECK(on);
        else CHECK(!on);
    }
    // A running jump from the runway's end with a small platform at 55% of the natural
    // distance: the natural landing (beyond it, down on the ground) is deadly, so he lands on it.
    int natural = 624;  // the running jump's travel (measured on the runway above)
    for (int with : {0, 1}) {
        Run r(b, true);
        Course c = build_runway(r, s, 128 * 6);
        uint16_t p = 0;
        if (with) p = place_floor(r, 524, c.x1 - 70 - 32 + natural * 55 / 100, c.y, c.z);
        JumpResult j = course_jump(r, c.x1 - 70, 2);
        std::printf("  running jump off the runway's end %s: %d units, came down %d below, %s\n",
                    with ? "with a platform at 55%" : "(nothing ahead)", dist(j), c.z - j.z1 + 0,
                    with ? (j.on == p ? "on the platform" : "missed it") : "");
        if (with) CHECK(j.on == p);
    }
}

// --- Ledges (Responsive): hold jump to climb or to jump, grab and pull up ----------------------

enum LedgeCase { kClimb56, kGrab96, kFallGrab, kFallRelease };

int ledge_test(const Base& b, const Shore& s, LedgeCase c, bool responsive, int& z_ground) {
    Run r(b, responsive);
    // In the air above the camp ground, clear of everything: a floor of shape-70 tiles 240 up
    // (two tiles: the Avatar's and the block's), the block east of him on the second tile.
    int fz = s.z + 100;  // (z is 0..255)
    int fx = s.x - 900, fy = s.y - 900;  // the first tile's centre
    for (int k = 0; k < 2; ++k) place_floor(r, 70, fx + 128 * k, fy, fz);
    int g = fz + 8;
    z_ground = g;
    auto put = [&](unsigned shape, int z) { place_floor(r, shape, fx + 128 + 8, fy, z); };
    if (c == kClimb56) put(740, g);
    else { put(193, g); put(740, g + 40); }
    // The Avatar's box flush with the block's west side (8 units gap).
    int ax = fx + 64 - 8 + 8, ay = fy + 32;
    r.w.move_to(1, ax, ay, (c == kFallGrab || c == kFallRelease) ? g + 90 : g, false);
    r.av().dir = 2;
    r.run(2);
    std::printf("    ledge case %d: start at %d,%d,%d (floor top %d)\n", int(c), r.av().x, r.av().y, r.av().z, g);
    r.w.debug_assist = true;
    PlayerInput& in = r.w.input;
    in.key_jump = true;
    in.jump_held = true;
    in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
    // (Falling: jump is only held, never pressed, so no buffered press either.)
    if (c == kFallGrab || c == kFallRelease) { in.key_jump = false; in.jump_buffer_ms = 0; r.w.fall(1); }
    bool hung = false;
    for (int f = 0; f < 150; ++f) {
        r.step();
        if (c != kFallGrab && c != kFallRelease) in.key_jump = f < 2;  // a press, then held
        if (r.av().last_anim == 45 && !hung) {
            std::printf("    ledge case %d: hanging at z %d after %d frames\n", int(c), r.av().z - g, f);
            hung = true;
        }
        in.jump_held = !(c == kFallRelease && hung);
    }
    in.key_jump = in.jump_held = false;
    r.settle();
    r.run(20);
    return r.av().z - g;
}

void test_ledges(const Base& b, const Shore& s) {
    int g;
    int climb = ledge_test(b, s, kClimb56, true, g);
    int grab = ledge_test(b, s, kGrab96, true, g);
    int fall = ledge_test(b, s, kFallGrab, true, g);
    int rel = ledge_test(b, s, kFallRelease, true, g);
    int classic = ledge_test(b, s, kGrab96, false, g);
    std::printf("  ledges (height reached): hold jump at a 56 ledge %d, at a 96 ledge %d (jump, grab, pull up), falling past "
                "a 96 ledge holding jump %d, released while hanging %d; Classic at the 96 ledge %d\n",
                climb, grab, fall, rel, classic);
    CHECK_EQ(climb, 56);
    CHECK(grab < 96);  // too high since Easy Ledges' limit came down to 72 (ledge::kMaxRise)
    CHECK_EQ(fall, 96);
    CHECK(rel < 56);  // he let go and dropped to the floor
    CHECK(classic < 96);
}

// --- The party bar on the controller: LB + D-pad picks a member, A opens his windows ----------

void test_party_pad(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    std::string err;
    bool loaded = false;
    for (const char* f : {"data/companions.json", "../data/companions.json", "../../data/companions.json",
                          "engine/data/companions.json"})
        if (!loaded)
            if (std::FILE* fp = std::fopen(f, "rb")) { std::fclose(fp); loaded = w.party->load_file(f, err); }
    if (!loaded) { std::printf("  party pad: companions.json not found, skipped\n"); return; }
    w.party->set_enabled(true);
    w.party->spawn_here();
    w.party->join(0);
    w.party->join(1);
    u.frames(10);
    InputEvent add;
    add.type = InputEvent::kPadAdded;
    u.gm.handle(add);
    u.wake();
    int n = int(w.party->bar_ids().size());
    // LB held + D-pad Right: the first companion; again: the next.
    u.pad(kPadLB, true);
    u.pad(kPadRight, true); u.pad(kPadRight, false);
    int h1 = w.party->pad_highlight;
    u.pad(kPadRight, true); u.pad(kPadRight, false);
    int h2 = w.party->pad_highlight;
    u.pad(kPadLB, false);
    u.frames(1);
    CHECK(w.gumps.container_gump(1) == nullptr);  // LB used with the D-pad: no paperdoll
    uint16_t who = h2 >= 0 && h2 < n ? w.party->bar_ids()[size_t(h2)] : 0;
    u.pad(kPadA, true); u.pad(kPadA, false);
    u.frames(5);
    bool opened = who && w.gumps.container_gump(who) != nullptr;
    std::printf("  party pad: %d bar slots, LB + Right -> slot %d, again -> %d (actor %u); A opened his paperdoll: %s\n", n, h1,
                h2, who, opened ? "yes" : "no");
    CHECK(n >= 3);
    CHECK_EQ(h1, 1);
    CHECK_EQ(h2, 2);
    CHECK(opened);
    CHECK_EQ(w.party->pad_highlight, -1);
    // A plain LB tap: the Avatar's paperdoll.
    w.gumps.close_item_dependent();
    u.frames(2);
    u.pad(kPadLB, true); u.pad(kPadLB, false);
    u.frames(3);
    CHECK(w.gumps.container_gump(1) != nullptr);
}

// --- Dragging at every zoom level (mouse) and with the controller's A -------------------------

struct ZoomUI {
    UI& u;
    int S = 4;
    std::vector<uint32_t> px;
    int fw = 0, fh = 0;
    void frames(int n) { for (int i = 0; i < n; ++i) { u.gm.tick(); u.gm.render_frame(px, fw, fh, S, 0, 1.0 / 30); } }
    // A screen point (with sub-pixel precision) that picks `id`, scanning the whole screen.
    bool find(uint16_t id, float& x, float& y, float margin = 0) {
        auto ok = [&](float xx, float yy) { return u.gm.pick_f(xx, yy) == id && !u.gm.world().gumps.at(int(xx), int(yy)); };
        for (float yy = 0.25f; yy < 200; yy += 0.5f)
            for (float xx = 0.25f; xx < 320; xx += 0.5f)
                if (ok(xx, yy) && (margin == 0 || (ok(xx - margin, yy) && ok(xx + margin, yy) && ok(xx, yy - margin) &&
                                                   ok(xx, yy + margin)))) {
                    x = xx; y = yy;
                    return true;
                }
        return false;
    }
    void move(float x, float y) {
        InputEvent e;
        e.type = InputEvent::kMouseMove;
        e.x = int(x); e.y = int(y); e.fx = x; e.fy = y;
        u.gm.handle(e);
    }
};

// Drags the bedroll from the ground into the open backpack window (to == 0) or onto the
// Avatar (to == 1) at zoom step `step`. True if it ended in the backpack.
bool drag_bedroll(const GameData& d, const Base& b, int step, int to, std::string& why) {
    UI u(d, b, true);
    World& w = u.gm.world();
    ZoomUI z{u};
    std::vector<uint16_t> near;
    w.area_search(w.avatar()->x, w.avatar()->y, 2000, false, near);
    uint16_t bed = 0;
    for (uint16_t id : near) if (const Item* it = w.item(id); it && (it->shape == 534 || it->shape == 535) && it->chunk >= 0) bed = id;
    if (!bed) {
        for (uint16_t id : near)
            if (const Item* it = w.item(id); it && std::abs(it->x - w.avatar()->x) + std::abs(it->y - w.avatar()->y) < 500 && !d.typeflags.get(it->shape).is(ShapeInfo::kFixed))
                std::printf("    near: %u shape %u %s at %d,%d chunk %d parent %u\n", id, it->shape, d.usecode.class_name(it->shape).c_str(), it->x, it->y, it->chunk, it->parent);
        why = "no bedroll";
        return false;
    }
    const Item* bi = w.item(bed);
    w.move_to(1, bi->x + 70, bi->y + 40, bi->z, false);
    w.camera_follow(1);
    w.controls.zoom_step = step;
    z.frames(10);
    uint16_t pack = w.equipped(1, 7);
    u.key('i'); u.key('i', false);
    z.frames(10);
    Gump* pg = w.gumps.container_gump(pack);
    if (!pg) { why = "no backpack window"; return false; }
    // Out of the way, in the bottom right corner (a player would move it there).
    if (auto* cg = dynamic_cast<ContainerGump*>(pg)) { cg->off_x += 318 - pg->w - cg->x; cg->off_y += 198 - pg->h - cg->y; }
    pg->x = 318 - pg->w;
    pg->y = 198 - pg->h;
    z.frames(2);
    float sx, sy;
    if (!z.find(bed, sx, sy)) { why = "bedroll not on screen"; return false; }
    z.move(sx, sy);
    z.frames(1);
    u.button(1, true);
    z.frames(1);
    z.move(sx + 3, sy + 3);
    z.frames(1);
    if (!u.gm.dragging()) { why = "drag did not start"; u.button(1, false); return false; }
    {
        const ShapeInfo& si = d.typeflags.get(w.item(bed)->shape);
        char buf[160];
        std::snprintf(buf, sizeof buf, "(reach %d, weight %u, fixed %d, avatar %d,%d bed %d,%d)", w.can_reach(1, bed, 128),
                      si.weight(), si.is(ShapeInfo::kFixed), w.avatar()->x, w.avatar()->y, w.item(bed)->x, w.item(bed)->y);
        why = buf;
    }
    float tx, ty;
    if (to == 0) { tx = pg->x + pg->w / 2.0f; ty = pg->y + pg->h / 2.0f; }
    else if (!z.find(1, tx, ty)) { why = "Avatar not found"; return false; }
    z.move(tx, ty);
    z.frames(1);
    u.button(1, false);
    z.frames(3);
    const Item* after = w.item(bed);
    bool in = after && after->parent == pack;
    if (!in) why = "dropped elsewhere " + why;
    return in;
}

void test_zoom_drag(const GameData& d, const Base& b) {
    for (int step = 0; step <= 5; ++step)
        for (int to : {0, 1}) {
            std::string why;
            bool ok = drag_bedroll(d, b, step, to, why);
            std::printf("  zoom step %d: bedroll dragged %s: %s %s\n", step, to ? "onto the Avatar" : "into the backpack window",
                        ok ? "in the backpack" : "FAILED", why.c_str());
            CHECK(ok);
        }
}

// The controller: the pointer on the bedroll, A held, the right stick to the backpack window,
// A released.
void test_pad_drag(const GameData& d, const Base& b) {
    UI u(d, b, true);
    World& w = u.gm.world();
    ZoomUI z{u, 3};
    InputEvent add;
    add.type = InputEvent::kPadAdded;
    u.gm.handle(add);
    u.wake();
    std::vector<uint16_t> near;
    w.area_search(w.avatar()->x, w.avatar()->y, 2000, false, near);
    uint16_t bed = 0;
    for (uint16_t id : near) if (const Item* it = w.item(id); it && (it->shape == 534 || it->shape == 535) && it->chunk >= 0) bed = id;
    CHECK(bed != 0);
    if (!bed) return;
    const Item* bi = w.item(bed);
    w.move_to(1, bi->x + 70, bi->y + 40, bi->z, false);
    z.frames(10);
    u.pad(kPadY, true); u.pad(kPadY, false);
    z.frames(10);
    uint16_t pack = w.equipped(1, 7);
    Gump* pg = w.gumps.container_gump(pack);
    CHECK(pg != nullptr);
    if (!pg) return;
    if (auto* cg = dynamic_cast<ContainerGump*>(pg)) { cg->off_x += 318 - pg->w - cg->x; cg->off_y += 198 - pg->h - cg->y; }
    pg->x = 318 - pg->w;
    pg->y = 198 - pg->h;
    z.frames(2);
    // Steer the pointer with the right stick.
    auto stick_to = [&](float tx, float ty) {
        for (int i = 0; i < 200; ++i) {
            float dx = tx - (u.gm.mouse_x() + 0.5f), dy = ty - (u.gm.mouse_y() + 0.5f);
            float m = std::sqrt(dx * dx + dy * dy);
            if (m < 2.5f) break;
            int v = m > 30 ? 32767 : m > 8 ? 18000 : 10500;
            u.axis(kAxisRX, int(v * dx / m));
            u.axis(kAxisRY, int(v * dy / m));
            z.frames(1);
        }
        u.axis(kAxisRX, 0);
        u.axis(kAxisRY, 0);
        z.frames(1);
    };
    float sx, sy;
    CHECK(z.find(bed, sx, sy, 3.5f) || z.find(bed, sx, sy, 2.5f) || z.find(bed, sx, sy));
    stick_to(sx, sy);
    bool over = u.gm.pick(u.gm.mouse_x(), u.gm.mouse_y()) == bed;
    std::printf("    pad drag: bedroll at %.1f,%.1f, pointer at %d,%d picks %u\n", sx, sy, u.gm.mouse_x(), u.gm.mouse_y(),
                u.gm.pick(u.gm.mouse_x(), u.gm.mouse_y()));
    u.pad(kPadA, true);
    z.frames(1);
    stick_to(pg->x + pg->w / 2.0f, pg->y + pg->h / 2.0f);
    bool dragging = u.gm.dragging();
    u.pad(kPadA, false);
    z.frames(3);
    const Item* after = w.item(bed);
    bool in = after && after->parent == pack;
    std::printf("  pad drag: pointer on the bedroll %d, dragging with A held %d, dropped into the backpack %d\n", over, dragging, in);
    CHECK(over && dragging && in);
}

// Screenshots for NOTES.md (only with PAGAN_M6_SHOTS=dir).
void m6_shots(const GameData& d, const Base& b, const std::string& dir) {
    UI u(d, b, true);
    World& w = u.gm.world();
    u.gm.set_atlas_layout_path("data/atlas_layout.json");
    w.controls.smooth_scroll = true;
    std::vector<uint32_t> px;
    int fw, fh;
    auto shot = [&](const std::string& name, int S) {
        u.gm.render_frame(px, fw, fh, S, 0, 1.0 / 30);
        save_png(dir + "/" + name + ".png", px.data(), fw, fh, 1);
        std::printf("  shot %s (%dx%d)\n", name.c_str(), fw, fh);
    };
    w.teleport_actor(1, 16130, 11960, 48, 39);
    w.camera_follow(1);
    u.frames(40);
    for (int z = 0; z <= 3; ++z) {
        w.controls.zoom_step = z;
        u.gm.tick();
        u.gm.render_frame(px, fw, fh, 4, 0, 1.0 / 30);
        int n, dd;
        u.gm.zoom_fraction(n, dd);
        shot("m6_zoom_" + std::to_string(100 * n / dd), 4);
    }
    w.controls.zoom_step = 0;
    InputEvent add;
    add.type = InputEvent::kPadAdded;
    std::strcpy(add.name, "Xbox Controller");
    u.gm.handle(add);
    u.wake();
    u.frames(8);
    shot("m6_autotarget", 3);
    u.pad(kPadRight, true); u.pad(kPadRight, false);
    u.frames(2);
    shot("m6_autotarget_next", 3);
    u.pad(kPadY, true); u.pad(kPadY, false);
    u.frames(10);
    u.axis(kAxisRX, -26000);
    u.axis(kAxisRY, 20000);
    u.frames(6);
    u.axis(kAxisRX, 0);
    u.axis(kAxisRY, 0);
    u.frames(2);
    shot("m6_virtual_pointer", 3);
    w.gumps.close_item_dependent();
    u.frames(2);
    w.open_menu(MenuGump::kMenu);
    u.frames(2);
    shot("m6_main_menu", 3);
    w.open_menu(MenuGump::kOptions);
    u.frames(2);
    shot("m6_options", 3);
    u.key(kKeyEscape); u.key(kKeyEscape);
    u.frames(2);
    for (int pg = 0; pg < u.gm.help_pages(); ++pg) {
        u.gm.open_help();
        for (int k = 0; k < pg; ++k) u.key(kKeyRight);
        u.frames(1);
        shot("m6_help_" + std::to_string(pg + 1), 3);
        u.key(kKeyEscape);
    }
    u.key('m');
    for (int i = 0; i < 30; ++i) { u.gm.tick(); u.gm.render_frame(px, fw, fh, 3, 0, 1.0 / 30); }
    for (int i = 0; i < 40 && u.gm.atlas_open(); ++i) u.gm.render_frame(px, fw, fh, 3, 0, 1.0 / 30);
    shot("m6_atlas_from_game", 3);
    u.key('m');
    // The assist probes at the doorway of door 8367.
    uint16_t door = 0;
    std::vector<uint16_t> all;
    w.area_search(16191, 11807, 64, false, all);
    for (uint16_t id : all) if (const Item* it = w.item(id); it && w.is_door_shape(it->shape)) door = id;
    if (door) {
        w.call_event(door, kEvUse);
        u.frames(60);
        w.move_to(1, 16063 + 64 - 40, 11920, 48, false);
        w.debug_assist = true;
        u.frames(3);
        assist::Candidate c;
        assist::find(w, *w.avatar(), 0, 0, c);
        shot("m6_assist_probes", 3);
        w.debug_assist = false;
    }
}

}  // namespace

// Answer menus (game/ask_nav.h): spatial moves on a wrapped layout, the exit answer from the
// game's own strings and the companions', and the arrow keys + Enter on Devon's first menus.
void test_ask_nav(const GameData& d) {
    std::printf("answer menu navigation\n");
    // Two rows: [0 1 2] over [3 4]; 3 sits under 0-1, 4 under 2.
    std::vector<AskMenu::Button> b = {{0, 0, 40, 10}, {44, 0, 40, 10}, {88, 0, 60, 10}, {10, 10, 60, 10}, {90, 10, 50, 10}};
    CHECK_EQ(ask_move(b, 0, kAskDown), 3);
    CHECK_EQ(ask_move(b, 1, kAskDown), 3);
    CHECK_EQ(ask_move(b, 2, kAskDown), 4);
    CHECK_EQ(ask_move(b, 4, kAskUp), 2);
    CHECK_EQ(ask_move(b, 3, kAskUp), 0);
    CHECK_EQ(ask_move(b, 3, kAskDown), 0);   // wraps to the top row
    CHECK_EQ(ask_move(b, 2, kAskRight), 3);  // the end of a row: the next row
    CHECK_EQ(ask_move(b, 3, kAskLeft), 2);
    CHECK_EQ(ask_move(b, 4, kAskRight), 0);
    CHECK_EQ(ask_move(b, -1, kAskDown), 0);
    std::vector<AskMenu::Button> one = {{0, 0, 40, 10}, {44, 0, 40, 10}};
    CHECK_EQ(ask_move(one, 1, kAskDown), 1);  // one row: nothing below
    CHECK_EQ(ask_exit_index({"Who are you? ", "Goodbye. "}), 1);
    CHECK_EQ(ask_exit_index({"name", "bye", "join me"}), 1);
    CHECK_EQ(ask_exit_index({"necromancy", "the dead", "back"}), 2);
    CHECK_EQ(ask_exit_index({"Tell me of Tenebrae. ", "Enough about Pagan. ", "bye "}), 1);  // back before bye
    CHECK_EQ(ask_exit_index({"I wish to buy reagents. ", "Nothing, thank you. "}), 1);
    CHECK_EQ(ask_exit_index({"Yes. ", "No. "}), -1);
    CHECK_EQ(ask_exit_index({"Nevermind that. What do you do? ", "Who are you? "}), -1);
    CHECK_EQ(ask_exit_kind("Farewell. "), 1);
    CHECK_EQ(ask_exit_kind("GOODBYE! "), 1);
    CHECK_EQ(ask_exit_kind("Good day to you, Sir. "), 1);
    CHECK_EQ(ask_exit_kind("Never mind. "), 2);
    CHECK_EQ(ask_exit_kind("That's it. I've had it. Goodbye! "), 0);
    // The keyboard on the opening conversation: the first arrow shows the highlight, the next
    // moves it, Enter answers it.
    GameMode gm(d);
    World& w = gm.world();
    w.ask_name = false;
    gm.start();
    Surface8 surf(320, 200);
    auto key = [&](int k) { InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e); e.type = InputEvent::kKeyUp; gm.handle(e); };
    for (int i = 0; i < 6000 && !w.ask_menu; ++i) {
        gm.tick();
        gm.render(surf);
        if (!w.barks.empty() && i % 20 == 10) key(kKeyReturn);
    }
    CHECK(w.ask_menu != nullptr);
    if (!w.ask_menu) return;
    for (int i = 0; i < 20; ++i) { gm.tick(); gm.render(surf); }
    int before = gm.ask_highlight();
    key(kKeyRight);
    int shown = gm.ask_highlight();
    key(kKeyRight);
    int moved = gm.ask_highlight();
    std::string want = w.vm().string(w.ask_menu->answers[size_t(moved >= 0 ? moved : 0)]);
    std::printf("  keys: highlight %d, first Right %d, second Right %d (\"%s\"), Enter answers it\n", before, shown, moved, want.c_str());
    CHECK_EQ(before, -1);
    CHECK_EQ(shown, 0);
    CHECK_EQ(moved, 1);
    key(kKeyReturn);
    CHECK_EQ(gm.last_pad_answer(), 1);
    CHECK(!w.ask_menu || w.vm().string(w.ask_menu->answers[0]) != want);
}

// A on an NPC picked as the auto-target works like the mouse on him: one tap is a click (look:
// his name over his head), two taps within the double-click time a double-click (use: he talks).
// Each tap snaps the pointer onto him again, so it still works after he has walked a little.
void test_pad_talk(const GameData& d, const Base& b, bool responsive) {
    UI u(d, b, responsive);
    World& w = u.gm.world();
    InputEvent add;
    add.type = InputEvent::kPadAdded;
    std::strcpy(add.name, "Test pad");
    u.gm.handle(add);
    u.wake();
    const uint16_t npc = 2;  // Devon
    Actor* g = w.actor(npc);
    CHECK(g != nullptr);
    if (!g) return;
    w.teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
    w.camera_follow(1);
    u.frames(30);
    for (size_t i = 0; i < 12 && u.gm.auto_target() != npc; ++i) { u.pad(kPadRight, true); u.pad(kPadRight, false); u.frames(1); }
    CHECK_EQ(u.gm.auto_target(), npc);
    auto barking = [&] {
        for (const Bark& bk : w.barks)
            if (bk.speaker == npc) return true;
        return false;
    };
    auto talking = [&] { return w.ask_menu || w.in_conversation(); };
    // One tap: a click on him (the look comes at once in Responsive mode, after the
    // double-click time in Classic, as with the mouse).
    u.pad(kPadA, true); u.pad(kPadA, false);
    bool named = false;
    for (int f = 0; f < 20 && !named; ++f) { u.frames(1); named = barking(); }
    bool on_him = u.gm.pick(u.gm.mouse_x(), u.gm.mouse_y()) == npc;
    u.frames(20);
    bool talked_after_one = talking();
    // He steps aside (the pointer is still where he was), then two quick taps.
    w.teleport_actor(npc, g->x - 48, g->y + 32, g->z, g->mapnum);
    u.frames(5);
    CHECK_EQ(u.gm.auto_target(), npc);
    u.pad(kPadA, true); u.pad(kPadA, false);
    u.frames(2);
    u.pad(kPadA, true); u.pad(kPadA, false);
    bool on_him2 = u.gm.pick(u.gm.mouse_x(), u.gm.mouse_y()) == npc;
    bool talks = false;
    int when = -1;
    for (int f = 0; f < 600 && !talks; ++f) { u.frames(1); talks = talking(); if (talks) when = f; }
    std::printf("  pad A on Devon (%s): one tap named him %d (pointer on him %d, talking %d); after he moved, "
                "double tap pointer on him %d, conversation %d (after %d frames)\n",
                responsive ? "Responsive" : "Classic", named, on_him, talked_after_one, on_him2, talks, when);
    CHECK(named);
    CHECK(on_him);
    CHECK(!talked_after_one);
    CHECK(on_him2);
    CHECK(talks);
}

void test_milestone6(const GameData& d) {
    Base base(d);
    test_latency(base);
    test_click(d, base);
    test_camera(d, base);
    test_zoom(d, base);
    test_atlas(d, base);
    test_pad(d, base);
    test_pad_talk(d, base, true);
    test_pad_talk(d, base, false);
    test_ask_nav(d);
    test_click_through(d);
    test_click_guard(d);
    test_doors(base);
    test_door_walk(base);
    test_assist(base);
    test_edge(base);
    Shore sh = shore(base);
    test_jumps(base, sh);
    test_jump_magnet(base, sh);
    test_ledges(base, sh);
    test_party_pad(d, base);
    test_zoom_drag(d, base);
    test_pad_drag(d, base);
    if (const char* dir = std::getenv("PAGAN_M6_SHOTS")) m6_shots(d, base, dir);
}
