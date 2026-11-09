// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Walk to act (app/auto_walk.cpp, NOTES.md "Walk to act"): a double-click use or a drag into
// the pack on an item out of reach walks the Avatar there over safe ground and then acts; water
// in the way, or too far, gives the red cross; any input cancels the walk.
// Each scenario runs on a raised test platform (shape-70 floors 160 above the camp), so the
// ground is known.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "play.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/gumps.h"
#include "game/pathfinder.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

int g_fail = 0, g_pass = 0;
using namespace pg;

namespace {

std::string g_shots;

struct Base {
    const GameData& d;
    std::vector<uint8_t> save;
    explicit Base(const GameData& data) : d(data) {
        Play p(d);
        SaveWriter out;
        p.w.save_game(out, "autowalk");
        save = out.data();
    }
};

struct UI {
    GameMode gm;
    Surface8 surf{320, 200};
    UI(const GameData& d, const Base& b) : gm(d) {
        gm.world().auto_ui = true;
        gm.world().ask_name = false;
        gm.start();
        SaveReader in(b.save.data(), b.save.size());
        gm.world().load_game(in);
        frames(3);
    }
    World& w() { return gm.world(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; gm.handle(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b;
        e.x = e.y = -9999;
        gm.handle(e);
    }
    void key(int k, bool down) { InputEvent e; e.type = down ? InputEvent::kKeyDown : InputEvent::kKeyUp; e.key = k; gm.handle(e); }
    bool point_of(uint16_t id, int& x, int& y) {
        for (int yy = 0; yy < 200; ++yy)
            for (int xx = 0; xx < 320; ++xx)
                if (gm.pick(xx, yy) == id && !w().gumps.at(xx, yy)) { x = xx; y = yy; return true; }
        return false;
    }
    bool double_click(uint16_t id) {
        int x, y;
        if (!point_of(id, x, y)) return false;
        move(x, y);
        frames(1);
        for (int i = 0; i < 2; ++i) { button(1, true); frames(1); button(1, false); frames(1); }
        return true;
    }
    // Drags `id` from the world onto `onto` (an item drawn in the world). False if it did not start.
    bool drag(uint16_t id, uint16_t onto) {
        int x, y, tx, ty;
        if (!point_of(id, x, y)) return false;
        move(x, y);
        frames(1);
        button(1, true);
        frames(1);
        move(x + 3, y + 3);
        frames(1);
        bool started = gm.dragging();
        if (!point_of(onto, tx, ty) && onto == 1) { tx = 160; ty = 100; }
        move(tx, ty);
        frames(1);
        button(1, false);
        frames(1);
        return started;
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        std::vector<uint32_t> px;
        int fw, fh;
        gm.render_frame(px, fw, fh, 2, 0, 1.0 / 30);
        save_png(g_shots + "/" + name + ".png", px.data(), fw, fh, 1);
    }
};

// The first flat 128 x 128 sea (not land) shape: water to put in the way.
unsigned water_shape(const GameData& d) {
    for (unsigned s = 0; s < d.typeflags.count(); ++s) {
        const ShapeInfo& si = d.typeflags.get(s);
        if (si.is(ShapeInfo::kSea) && !si.is(ShapeInfo::kLand) && si.x() == 4 && si.y() == 4 && si.z() == 0) return s;
    }
    return 0;
}

uint16_t place(World& w, unsigned shape, int cx, int cy, int z) {
    uint16_t p = w.create_item(shape, 0);
    int xd, yd, zd;
    w.footprint(*w.item(p), xd, yd, zd);
    w.move_to(p, cx + xd / 2, cy + yd / 2, z, false);
    return p;
}

// A platform of `len` floors along x (two wide), 160 above the Avatar; floor `water_at` (>= 0)
// is water instead. The Avatar stands on the west end; returns the platform's west x, its
// centre-line y and top z. (Along x: down and right on the screen; keep targets within ~450.)
struct Deck { int x0, y, z; };
Deck build(UI& u, int len, int water_at, unsigned water) {
    World& w = u.w();
    const Actor* av = w.avatar();
    Deck dk{av->x - 64, av->y, av->z + 160};
    for (int i = 0; i < len; ++i)
        for (int j : {-1, 0})
            place(w, i == water_at ? water : 70u, dk.x0 + i * 128 + 64, dk.y + j * 128 + 64,
                  i == water_at ? dk.z + 8 : dk.z);  // (floor 70 is 8 high, water and lava flat)
    dk.z += 8;  // the top
    w.move_to(1, dk.x0 + 64 + 32, dk.y + 16, dk.z, false);
    w.camera_follow(1);
    u.frames(15);
    return dk;
}

// A use target (a bag: using it opens its window) `dist` east of the Avatar on the platform.
uint16_t bag_at(UI& u, const Deck& dk, int dist) {
    World& w = u.w();
    uint16_t bag = place(w, 529, w.avatar()->x + dist, dk.y, dk.z);
    u.frames(2);
    return bag;
}

int run_until(UI& u, int max, const std::function<bool()>& done) {
    for (int f = 0; f < max; ++f) {
        if (done()) return f;
        u.frames(1);
    }
    return done() ? max : -1;
}

void test_use_after_walk(const GameData& d, const Base& b) {
    UI u(d, b);
    World& w = u.w();
    Deck dk = build(u, 5, -1, 0);
    uint16_t bag = bag_at(u, dk, 6 * 48);  // 288 units: well out of reach (128), within range (512)
    int x0 = w.avatar()->x;
    CHECK(!w.can_reach(1, bag, 128));
    CHECK(u.double_click(bag));
    bool pending = u.gm.auto_walk_pending();
    bool cross = w.red_cross_frames > 0;
    u.shot("autowalk_1_start");
    int n = 0;
    int f = run_until(u, 400, [&] {
        if (n % 3 == 0 && n < 60) { char nm[48]; std::snprintf(nm, sizeof nm, "autowalk_walk_%02d", n / 3); u.shot(nm); }
        ++n;
        return w.gumps.container_gump(bag) != nullptr;
    });
    u.frames(4);
    u.shot("autowalk_2_used");
    std::printf("  use after walk: pending %d, red cross %d, used after %d frames, walked %d units, reach %d\n", pending,
                cross, f, w.avatar()->x - x0, w.can_reach(1, bag, 128));
    CHECK(pending);
    CHECK(!cross);
    CHECK(f >= 0);
    CHECK(w.avatar()->x - x0 > 64);
    CHECK(!u.gm.auto_walk_pending());
}

// Water or lava (`shape`) across the whole width of the platform between him and the bag.
void test_hazard_cross(const GameData& d, const Base& b, unsigned shape, const char* what) {
    std::printf("  %s shape %u\n", what, shape);
    CHECK(shape != 0);
    if (!shape) return;
    UI u(d, b);
    World& w = u.w();
    Deck dk = build(u, 4, 1, shape);  // floor 0, the hazard, floors 2-3
    uint16_t bag = place(w, 529, dk.x0 + 2 * 128 + 64, dk.y, dk.z);
    u.frames(2);
    // Is there a way for a walker that does not mind it (a companion standing in it would)?
    Pathfinder plain(w);
    plain.set_actor(1);
    plain.set_target_item(bag, false);
    std::vector<PathStep> path;
    bool plain_ok = plain.plan(path);
    int x0 = w.avatar()->x;
    CHECK(u.double_click(bag));
    bool pending = u.gm.auto_walk_pending();
    bool cross = w.red_cross_frames > 0;
    u.shot(std::string("autowalk_3_") + what + "_cross");
    u.frames(90);
    std::printf("  across %s: path ignoring hazards %d, walk %d, red cross %d, bag used %d, moved %d\n", what, plain_ok,
                pending, cross, w.gumps.container_gump(bag) != nullptr, w.avatar()->x - x0);
    CHECK(!pending);
    CHECK(cross);
    CHECK(!w.gumps.container_gump(bag));
    CHECK(std::abs(w.avatar()->x - x0) < 16);
}

void test_too_far(const GameData& d, const Base& b) {
    UI u(d, b);
    World& w = u.w();
    Deck dk = build(u, 7, -1, 0);
    u.gm.auto_walk_range = 256;
    uint16_t bag = bag_at(u, dk, 420);
    CHECK(u.double_click(bag));
    std::printf("  too far (range 256, bag 420 away): pending %d, red cross %d\n", u.gm.auto_walk_pending(),
                w.red_cross_frames > 0);
    CHECK(!u.gm.auto_walk_pending());
    CHECK(w.red_cross_frames > 0);
}

void test_cancel(const GameData& d, const Base& b) {
    for (int how = 0; how < 3; ++how) {
        UI u(d, b);
        World& w = u.w();
        Deck dk = build(u, 5, -1, 0);
        uint16_t bag = bag_at(u, dk, 288);
        CHECK(u.double_click(bag));
        CHECK(u.gm.auto_walk_pending());
        u.frames(8);
        const char* what = "";
        if (how == 0) { what = "a key"; u.key('w', true); u.frames(1); u.key('w', false); }
        else if (how == 1) { what = "a left click"; u.move(20, 20); u.frames(1); u.button(1, true); u.frames(1); u.button(1, false); }
        else { what = "combat mode"; u.key('c', true); u.frames(1); u.key('c', false); }
        bool pending = u.gm.auto_walk_pending();
        u.frames(200);
        bool walking = w.kernel().count_exact(1, kProcPathfind) > 0;
        bool used = w.gumps.container_gump(bag) != nullptr;
        std::printf("  cancel by %s: pending after %d, still pathfinding %d, bag used %d\n", what, pending, walking, used);
        CHECK(!pending);
        CHECK(!walking);
        CHECK(!used);
    }
}

void test_pickup(const GameData& d, const Base& b) {
    UI u(d, b);
    World& w = u.w();
    Deck dk = build(u, 5, -1, 0);
    uint16_t bed = place(w, 534, w.avatar()->x + 288, dk.y, dk.z);  // a bedroll
    u.frames(2);
    uint16_t pack = w.equipped(1, 7);
    CHECK(!w.can_reach(1, bed, 128));
    // Dropped on the ground somewhere: no walk (only a pack takes a far item).
    {
        int x, y;
        CHECK(u.point_of(bed, x, y));
        u.move(x, y); u.frames(1); u.button(1, true); u.frames(1); u.move(x + 3, y + 3); u.frames(1);
        bool started = u.gm.dragging();
        u.move(x + 40, y - 30); u.frames(1); u.button(1, false); u.frames(1);
        std::printf("  far drag onto the ground: started %d, walk %d, bedroll still on the floor %d\n", started,
                    u.gm.auto_walk_pending(), w.item(bed)->parent == 0);
        CHECK(started);
        CHECK(!u.gm.auto_walk_pending());
        CHECK(w.item(bed)->parent == 0);
    }
    u.frames(20);
    bool started = u.drag(bed, 1);
    bool pending = u.gm.auto_walk_pending();
    int f = run_until(u, 400, [&] { return w.item(bed) && w.item(bed)->parent == pack; });
    u.shot("autowalk_4_picked_up");
    std::printf("  pick up into the pack: drag started %d, walk %d, in the backpack after %d frames\n", started, pending, f);
    CHECK(started);
    CHECK(pending);
    CHECK(f >= 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_shots = argc > 2 ? argv[2] : "";
    if (!g_shots.empty()) std::filesystem::create_directories(g_shots);
    Config::global().load((g_shots.empty() ? std::string("/tmp/pagan") : g_shots) + "_autowalk_test.cfg");  // never the player's
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    Base base(d);
    test_use_after_walk(d, base);
    test_hazard_cross(d, base, water_shape(d), "water");
    test_hazard_cross(d, base, 409, "lava");
    test_too_far(d, base);
    test_cancel(d, base);
    test_pickup(d, base);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
