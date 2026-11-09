// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 6: a controller through the real SDL GameController path. A virtual controller
// (SDL_JoystickAttachVirtualEx) is plugged in, its buttons and sticks are set, and the events
// that Platform::poll turns them into drive the game mode: hot-plug, walking and running with
// the left stick, A jump, RB combat, Start menu / B close, D-pad zoom, Back atlas, answer menus
// (spatial D-pad / stick, A, B = bye / back; Devon and a companion), unplugging.
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/game_mode.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/platform.h"

using namespace pg;

int g_pass = 0, g_fail = 0;
#define CHECK_EQ_PAD(a, b) CHECK((a) == (b))
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {
struct Rig {
    Platform plat;
    GameMode gm;
    Surface8 surf{320, 200};
    std::vector<uint32_t> px;
    int fw = 0, fh = 0;
    int added = 0, removed = 0, pad_events = 0;
    int vid = -1000;
    explicit Rig(const GameData& d) : gm(d) {}
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            std::vector<InputEvent> evs;
            plat.poll(evs);
            for (const InputEvent& e : evs) {
                // Only the virtual controller drives the test (a real one may be in someone's hands).
                if (e.type == InputEvent::kPadAdded && std::string(e.name) == "Pagan virtual pad") vid = e.pad;
                if (e.type >= InputEvent::kPadDown && e.type <= InputEvent::kPadAxis && e.pad != vid) continue;
                if (e.type == InputEvent::kPadAdded) ++added;
                if (e.type == InputEvent::kPadRemoved) ++removed;
                if (e.type >= InputEvent::kPadDown && e.type <= InputEvent::kPadAxis) ++pad_events;
                gm.handle(e);
            }
            gm.tick();
            gm.render_frame(px, fw, fh, 4, 0, 1.0 / 30);
        }
    }
    void tap(int b) { plat.virtual_pad_button(b, true); frames(1); plat.virtual_pad_button(b, false); frames(1); }
};
}  // namespace

int main(int argc, char** argv) {
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    GameFS fs;
    GameData d;
    if (root.empty() || !fs.open(root) || !d.load(fs)) { std::printf("game data not found\n"); return 1; }
    Rig r(d);
    PlatformConfig cfg;
    cfg.headless = true;
    cfg.audio = false;
    CHECK(r.plat.init(cfg));
    World& w = r.gm.world();
    w.auto_ui = true;
    w.ask_name = false;
    w.skip_opening = true;
    w.controls.responsive = true;
    r.gm.start();
    r.frames(10);
    w.teleport_actor(1, 16300, 12300, 48, 39);
    w.camera_follow(1);
    r.frames(20);
    // Plug in (a real controller may be connected too: count from here). Device events can be
    // late on a loaded machine, so wait for them in real time rather than a frame count.
    for (int i = 0; i < 50; ++i) { r.frames(1); SDL_Delay(10); }
    int real = r.added;
    if (real) std::printf("  a real controller is connected as well (%d)\n", real);
    bool att = r.plat.attach_virtual_pad();
    CHECK(att);
    if (!att) { std::printf("no virtual controller: %s\n", SDL_GetError()); return 1; }
    for (int i = 0; i < 200 && r.added < real + 1; ++i) { r.frames(1); SDL_Delay(5); }
    CHECK(r.added == real + 1);
    CHECK(r.plat.controllers() == real + 1);
    CHECK(r.gm.pad_connected());
    // The first button press only wakes the controller (a phantom pad must not act).
    r.tap(SDL_CONTROLLER_BUTTON_GUIDE);
    // Left stick up half way: walk; all the way: run.
    Actor* a = w.avatar();
    int x0 = a->x, y0 = a->y;
    r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTY, -20000);
    for (int i = 0; i < 100 && !w.input.pad_move; ++i) { r.frames(1); SDL_Delay(2); }  // the axis has arrived
    bool walked = false, ran = false;
    for (int i = 0; i < 25; ++i) { r.frames(1); walked |= a->last_anim == 0; }
    r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTY, -32767);
    for (int i = 0; i < 25; ++i) { r.frames(1); ran |= a->last_anim == 1; }
    r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    r.frames(20);
    int moved = std::abs(a->x - x0) + std::abs(a->y - y0);
    std::printf("  virtual pad: %d pad events; walked %d, ran %d, moved %d units\n", r.pad_events, walked, ran, moved);
    CHECK(walked && ran && moved > 100);
    // X: jump.
    bool jumped = false;
    r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_X, true);
    for (int i = 0; i < 8; ++i) { r.frames(1); unsigned la = a->last_anim; jumped |= la == 3 || la == 10 || la == 17 || la == 18 || (la >= 19 && la <= 26); }
    r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_X, false);
    r.frames(40);
    CHECK(jumped);
    // RB: combat mode.
    r.tap(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    r.frames(20);
    CHECK(a->is(Actor::kInCombat));
    r.tap(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    r.frames(20);
    CHECK(!a->is(Actor::kInCombat));
    // Start: menu; B closes it.
    r.tap(SDL_CONTROLLER_BUTTON_START);
    CHECK(w.gumps.find<MenuGump>() != nullptr);
    r.tap(SDL_CONTROLLER_BUTTON_B);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    // D-pad Down / Up: zoom out and in.
    r.tap(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(w.controls.zoom_step == 1);
    r.tap(SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(w.controls.zoom_step == 0);
    // Back: the atlas (opens on release), B closes it; holding Back: the help page.
    r.tap(SDL_CONTROLLER_BUTTON_BACK);
    CHECK(r.gm.atlas_open());
    r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 30000);  // pans the atlas
    r.frames(10);
    r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    r.frames(1);
    r.tap(SDL_CONTROLLER_BUTTON_B);
    CHECK(!r.gm.atlas_open());
    r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_BACK, true);
    r.frames(35);
    CHECK(r.gm.help_open());
    r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_BACK, false);
    r.frames(1);
    r.tap(SDL_CONTROLLER_BUTTON_B);
    CHECK(!r.gm.help_open());
    // Talking with the controller: the opening conversation with Devon. The D-pad and the
    // stick move the highlight by where the answers are drawn (down = the row below), A
    // answers the highlighted one, B the menu's "Goodbye." / "back"; a press right as the menu
    // appears is ignored, a later one works.
    auto rows_of = [](const AskMenu& m) {
        std::vector<int> ys;
        for (const auto& b : m.buttons)
            if (std::find(ys.begin(), ys.end(), b.y) == ys.end()) ys.push_back(b.y);
        return int(ys.size());
    };
    auto answer_text = [&](int i) { return w.ask_menu ? w.vm().string(w.ask_menu->answers[size_t(i)]) : std::string(); };
    auto wait_menu = [&](const AskMenu* old, int n) {
        for (int i = 0; i < n && (!w.ask_menu || w.ask_menu.get() == old); ++i) {
            r.frames(1);
            if (w.stasis && !w.barks.empty() && !w.ask_menu && i % 20 == 10) r.tap(SDL_CONTROLLER_BUTTON_A);  // click through the lines
        }
        return w.ask_menu != nullptr && w.ask_menu.get() != old;
    };
    {
        w.skip_opening = false;
        w.auto_ui = false;
        w.new_game();
        CHECK(wait_menu(nullptr, 6000));
        if (w.ask_menu) {
            // A the moment the menu appears: ignored (a press meant for the line before).
            int open = w.ask_menu->frames_open;
            const AskMenu* m0 = w.ask_menu.get();
            r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_A, true);
            r.frames(1);
            r.plat.virtual_pad_button(SDL_CONTROLLER_BUTTON_A, false);
            r.frames(1);
            bool kept = w.ask_menu.get() == m0 && r.gm.last_pad_answer() < 0;
            // Later: Right moves along the row, Left back; Down in a one-row menu stays; A answers.
            r.frames(6);
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            int right = r.gm.ask_highlight();
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            int left = r.gm.ask_highlight();
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            int down1 = r.gm.ask_highlight();
            int rows = rows_of(*w.ask_menu);
            std::string first = answer_text(0);
            r.tap(SDL_CONTROLLER_BUTTON_A);
            int chosen = r.gm.last_pad_answer();
            std::printf("  virtual pad talk: A %d frame(s) after the menu appeared %s; Right -> %d, Left -> %d, Down (%d row) -> %d, A chose %d \"%s\"\n",
                        open, kept ? "ignored" : "ANSWERED", right, left, rows, down1, chosen, first.c_str());
            CHECK(kept);
            CHECK_EQ_PAD(right, 1);
            CHECK_EQ_PAD(left, 0);
            CHECK(rows != 1 || down1 == 0);
            CHECK_EQ_PAD(chosen, 0);
        }
        // On to a menu of two rows or more (answering the first answer each time).
        const AskMenu* last = w.ask_menu.get();
        bool multi = false;
        for (int k = 0; k < 8 && !multi; ++k) {
            if (!wait_menu(last, 3000)) break;
            last = w.ask_menu.get();
            if (rows_of(*w.ask_menu) >= 2) { multi = true; break; }
            r.frames(20);
            r.tap(SDL_CONTROLLER_BUTTON_A);
        }
        CHECK(multi);
        if (multi && w.ask_menu) {
            const AskMenu& m = *w.ask_menu;
            // B at once: ignored (the 0.5 s guard).
            r.tap(SDL_CONTROLLER_BUTTON_B);
            CHECK(w.ask_menu.get() == &m);
            r.frames(20);
            // Down: the answer nearest below the first one (by centre).
            const auto& b0 = m.buttons[0];
            int want = -1, best = 1 << 30, below_y = 1 << 30;
            for (const auto& b : m.buttons) if (b.y > b0.y) below_y = std::min(below_y, b.y);
            for (size_t i = 0; i < m.buttons.size(); ++i) {
                const auto& b = m.buttons[i];
                int dist = std::abs((2 * b.x + b.w) - (2 * b0.x + b0.w));
                if (b.y == below_y && dist < best) { best = dist; want = int(i); }
            }
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            int down = r.gm.ask_highlight();
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_UP);
            int up = r.gm.ask_highlight();
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            int right = r.gm.ask_highlight();
            r.tap(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            std::printf("  multi-row menu (%d rows, %zu answers): Down -> %d (expected %d, y %d -> %d), Up -> %d, Right -> %d\n",
                        rows_of(m), m.answers.size(), down, want, b0.y, down >= 0 ? m.buttons[size_t(down)].y : -1, up, right);
            CHECK(down == want && down >= 0 && m.buttons[size_t(down)].y > b0.y);
            CHECK_EQ_PAD(up, 0);
            CHECK_EQ_PAD(right, 1);
            // The stick: down moves one row at once; held, it repeats after 300 ms, then every 150 ms.
            CHECK_EQ_PAD(r.gm.ask_highlight(), 0);
            r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 30000);
            int f0 = -1;
            std::vector<int> changes;
            int prev = r.gm.ask_highlight();
            int stick_first = -1;
            for (int i = 0; i < 200 && changes.size() < 4; ++i) {
                r.frames(1);
                if (f0 < 0 && r.gm.ask_highlight() != prev) { f0 = r.gm.frames(); stick_first = r.gm.ask_highlight(); }
                if (r.gm.ask_highlight() != prev) { changes.push_back(r.gm.frames()); prev = r.gm.ask_highlight(); }
                if (f0 < 0) SDL_Delay(2);  // the axis event may take a moment to arrive
            }
            r.plat.virtual_pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
            r.frames(3);
            std::printf("  stick down: first move to %d, moves at frames", stick_first);
            for (int c : changes) std::printf(" %d", c);
            std::printf("\n");
            CHECK(stick_first == want);
            CHECK(changes.size() >= 3 && changes[1] - changes[0] >= 8 && changes[1] - changes[0] <= 11 &&
                  changes[2] - changes[1] >= 4 && changes[2] - changes[1] <= 6);
            // PAGAN_M6_SHOTS=dir: the menu with the highlight and the B hint.
            if (const char* dir = std::getenv("PAGAN_M6_SHOTS")) {
                r.frames(1);
                save_png(std::string(dir) + "/pad_answer_menu.png", r.px.data(), r.fw, r.fh, 1);
            }
            // B: the menu's "Goodbye.".
            int bye = -1;
            for (size_t i = 0; i < m.answers.size(); ++i) if (w.vm().string(m.answers[i]) == "Goodbye. ") bye = int(i);
            r.tap(SDL_CONTROLLER_BUTTON_B);
            std::printf("  B chose %d (Goodbye. is %d)\n", r.gm.last_pad_answer(), bye);
            CHECK(bye >= 0 && r.gm.last_pad_answer() == bye);
            CHECK(w.ask_menu.get() != &m);
        }
    }
    // A companion's conversation: B in a sub-menu takes its "back", in the main menu its "bye".
    if (argc > 2) {
        std::string err;
        CHECK(w.party->load_file(std::string(argv[2]) + "/companions.json", err));
        w.party->enabled = true;
        w.party->beach_always = true;  // the companions on the beach, as this test expects
        w.skip_opening = true;
        w.new_game();
        for (int i = 0; i < 300 && !w.party->spawned_once; ++i) r.frames(1);
        CHECK(w.party->spawned_once);
        Party& p = *w.party;
        int mi = p.find_def("morvain");
        r.frames(60);
        if (const Actor* ma = w.actor(p.state[size_t(mi)].id)) {  // the Avatar next to Morvain
            w.kernel().kill(1, kProcAny);
            w.move_to(1, ma->x + 80, ma->y - 40, ma->z, false);
            w.camera_follow(1);
            r.frames(2);
        }
        p.start_talk(mi);
        auto has = [&](const char* s) {
            if (!w.ask_menu) return -1;
            for (size_t i = 0; i < w.ask_menu->answers.size(); ++i) if (w.vm().string(w.ask_menu->answers[i]) == s) return int(i);
            return -1;
        };
        CHECK(wait_menu(nullptr, 900));
        int lith = has("Lithos");
        CHECK(lith >= 0);
        const AskMenu* main_menu = w.ask_menu.get();
        if (lith >= 0) {
            InputEvent e; e.type = InputEvent::kKeyDown; e.key = '1' + lith; r.gm.handle(e);
            e.type = InputEvent::kKeyUp; r.gm.handle(e);
        }
        CHECK(wait_menu(main_menu, 900));
        bool sub = has("back") >= 0 && has("necromancy") >= 0;
        r.frames(20);
        if (const char* dir = std::getenv("PAGAN_M6_SHOTS")) {
            r.frames(1);
            save_png(std::string(dir) + "/pad_answer_back.png", r.px.data(), r.fw, r.fh, 1);
        }
        const AskMenu* sm = w.ask_menu.get();
        r.tap(SDL_CONTROLLER_BUTTON_B);
        int took = r.gm.last_pad_answer();
        CHECK(wait_menu(sm, 900));
        bool back_main = has("join me") >= 0 && has("bye") >= 0;
        std::printf("  companion: sub-menu with back %s, B took %d, back in the main menu %s\n", sub ? "yes" : "NO", took,
                    back_main ? "yes" : "NO");
        CHECK(sub);
        CHECK(back_main);
        r.frames(20);
        r.tap(SDL_CONTROLLER_BUTTON_B);
        for (int i = 0; i < 900 && p.talking(); ++i) { r.frames(1); if (!w.barks.empty() && i % 20 == 10) r.tap(SDL_CONTROLLER_BUTTON_A); }
        std::printf("  companion: B in the main menu ends the talk: %s\n", p.talking() ? "NO" : "yes");
        CHECK(!p.talking());
    }
    // Unplug.
    r.plat.detach_virtual_pad();
    for (int i = 0; i < 200 && r.removed < 1; ++i) { r.frames(1); SDL_Delay(5); }
    std::printf("  virtual pad: added %d, removed %d\n", r.added, r.removed);
    CHECK(r.removed == 1);
    CHECK(r.gm.pad_connected() == (real > 0));
    r.plat.shutdown();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
