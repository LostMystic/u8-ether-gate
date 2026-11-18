// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The Ether Gate credits (game/ether_credits.h, NOTES.md "Ether Gate credits"): opened from the
// menu's Credits entry; every person of the game's credits, the photos, each effect (shots), the
// layout inside the screen; the Original Credits button, Back, Esc, the right button, the pad, the
// wheel; the roll's end back at the menu.
//   pagan_credits_tests GAME_DIR DATA_DIR [SHOT_DIR]
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/credits.h"
#include "game/ether_credits.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {
std::string g_shots, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d) : gm(d) {
        gm.world().skip_opening = true;
        gm.world().ask_name = false;
        gm.world().credits_dir = g_data + "/credits";
        gm.start();
        for (int i = 0; i < 10; ++i) gm.tick();
    }
    World& w() { return gm.world(); }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void click(int b) {
        InputEvent e;
        e.type = InputEvent::kMouseDown; e.button = b; e.x = gm.mouse_x(); e.y = gm.mouse_y(); ev(e);
        e.type = InputEvent::kMouseUp; ev(e);
    }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    void wheel(int d) { InputEvent e; e.type = InputEvent::kWheel; e.wheel = d; e.x = gm.mouse_x(); e.y = gm.mouse_y(); ev(e); }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 3);
    }
    EtherCreditsGump* ec() { return dynamic_cast<EtherCreditsGump*>(w().gumps.top_modal()); }
    CreditsGump* oc() { return dynamic_cast<CreditsGump*>(w().gumps.top_modal()); }
    EtherCreditsGump* open() {
        MenuGump* m = w().open_menu(MenuGump::kMenu);
        m->choose(5);
        for (int i = 0; i < 2; ++i) gm.tick();
        return ec();
    }
    bool menu_up() { return w().gumps.find<MenuGump>() != nullptr; }
};

void test_roll(const GameData& d) {
    std::printf("the roll: people, photos, effects\n");
    Ui u(d);
    EtherCreditsGump* g = u.open();
    CHECK(g != nullptr);
    if (!g) return;
    CHECK(g->loaded());
    int people = 0, with_photo = 0, prev_y = -1, bad_order = 0, too_wide = 0;
    const ShapeFont& nf = u.w().font(8);
    for (const auto& e : g->entries()) {
        if (e.y < prev_y) ++bad_order;
        prev_y = e.y;
        if (e.type != EtherCreditsGump::Entry::kPerson) continue;
        ++people;
        with_photo += e.photo != nullptr;
        const int room = e.photo ? 320 - 2 * 30 - 46 - 12 : 256;
        if (nf.text_width(e.name) > room) { ++too_wide; std::printf("  too wide: %s\n", e.name.c_str()); }
    }
    std::printf("  %d people (%d with a photo), %d different photos, strip %d px\n", people, with_photo, g->photos_loaded(), g->strip_height());
    CHECK_EQ(people, 109);       // every credit line of the game's credits, one per person
    CHECK_EQ(g->photos_loaded(), 43);  // 38 portraits, 5 group photos
    CHECK_EQ(with_photo, 57);   // some people in several sections
    CHECK_EQ(bad_order, 0);
    CHECK_EQ(too_wide, 0);
    u.shot("credits_opening");
    // Each effect at a moment it shows (the person, and the frames since they rose in).
    struct Moment { const char* name; int age; const char* shot; };
    const Moment moments[] = {
        {"Richard Garriott", 34, "credits_glow_garriott"}, {"Tony Zurovec", 30, "credits_fade_zurovec"},
        {"Herman Miller", 14, "credits_type_miller"},      {"Denis Loubet", 10, "credits_paint_loubet"},
        {"Micael Priest", 60, "credits_art_priest"},       {"Matt Sheffield", 70, "credits_group_sheffield"},
        {"Sheri L. Graner", 16, "credits_runes_graner"},
        {"Mark Vittek", 12, "credits_build_vittek"},       {"Randy Buck", 14, "credits_wave_buck"},
        {"Kirk Winterrowd", 70, "credits_group_winterrowd"},
        {"Don Derouen", 20, "credits_stamp_derouen"},
        {"Dee Starns", 70, "credits_group_starns"},      {"Bill Johnson", 8, "credits_echo_johnson"},
        {"Starr Long", 60, "credits_starr_long"},          {"Rolf D. Busch", 60, "credits_busch"},
    };
    int ticks = 0;
    for (const Moment& m : moments) {
        bool seen = false;
        for (; ticks < 30000 && u.ec(); ++ticks) {
            const EtherCreditsGump::Entry* hit = nullptr;
            for (const auto& e : u.ec()->entries())
                if (e.type == EtherCreditsGump::Entry::kPerson && e.name == m.name && e.age >= m.age) { hit = &e; break; }
            if (hit) { seen = true; break; }
            u.gm.tick();
        }
        CHECK(seen);
        if (seen) u.shot(m.shot);
        if (!seen) std::printf("  not seen: %s\n", m.name);
    }
    std::printf("  %d ticks to Rolf D. Busch (%.0f s)\n", ticks, ticks / 30.0);
    // The section heading follows the roll.
    for (int i = 0; i < 10; ++i) u.gm.tick();
    CHECK(u.ec() && u.ec()->section() == "German Version");
    // The mod author near the end.
    bool author = false;
    for (int t = 0; t < 20000 && u.ec() && !author; ++t) {
        u.gm.tick();
        if (!u.ec()) break;
        for (const auto& e : u.ec()->entries())
            if (e.type == EtherCreditsGump::Entry::kText && !e.text_lines.empty() && e.text_lines[0] == "Mark Barazzuol" && e.age >= 70) author = true;
    }
    CHECK(author);
    u.shot("credits_mod_author");
    // To the end: it holds, then goes back to the menu by itself.
    int end = 0;
    for (; end < 30000 && u.ec(); ++end) {
        u.gm.tick();
        if (u.ec() && u.ec()->strip_height() - u.ec()->scroll() <= EtherCreditsGump::kWinH + 40 && end % 30 == 0) {}
        if (u.ec() && u.ec()->strip_height() - u.ec()->scroll() == EtherCreditsGump::kWinH + 1) u.shot("credits_end");
    }
    std::printf("  ended %d ticks later\n", end);
    CHECK(!u.ec() && u.menu_up());
}

// Linked names: the pointer on one holds the roll and says where it leads; a click opens it.
void test_links(const GameData& d) {
    std::printf("links\n");
    Ui u(d);
    std::vector<std::string> opened;
    u.w().open_url = [&](const std::string& url) { opened.push_back(url); return true; };
    EtherCreditsGump* g = u.open();
    CHECK(g != nullptr);
    if (!g) return;
    int links = 0, bad = 0;
    for (const auto& e : g->entries())
        if (!e.link.empty()) { ++links; bad += e.link.rfind("https://", 0) != 0; }
    std::printf("  %d linked names\n", links);
    CHECK(links >= 10);
    CHECK_EQ(bad, 0);
    // The first linked name, once it is well up the window.
    const EtherCreditsGump::Entry* target = nullptr;
    for (int t = 0; t < 20000 && u.ec() && !target; ++t) {
        u.gm.tick();
        u.gm.render(u.surf);  // where each name is drawn is learnt while painting
        if (!u.ec()) break;
        for (const auto& e : u.ec()->entries())
            if (!e.link.empty() && e.age > 60 && e.nw > 0) { target = &e; break; }
    }
    CHECK(target != nullptr);
    if (!target) return;
    std::printf("  %s -> %s\n", target->name.c_str(), target->link.c_str());
    u.move(target->nx + target->nw / 2, target->ny + target->nh / 2);
    u.gm.tick();
    u.gm.render(u.surf);
    const int s0 = g->scroll();
    for (int i = 0; i < 60; ++i) u.gm.tick();
    CHECK_EQ(g->scroll(), s0);  // held while the pointer is on it
    CHECK(g->link_at(target->nx + 2, target->ny + 2) >= 0);
    u.shot("credits_link_hover");
    u.click(1);
    u.gm.tick();
    CHECK(opened.size() == 1 && opened[0] == target->link);
    CHECK(u.ec() != nullptr);  // the roll stays
    // Off the name, the roll goes on.
    u.move(160, 190);
    for (int i = 0; i < 30; ++i) u.gm.tick();
    CHECK(g->scroll() > s0);
}

void test_buttons(const GameData& d) {
    std::printf("buttons, keys, pad\n");
    Ui u(d);
    EtherCreditsGump* g = u.open();
    CHECK(g != nullptr);
    if (!g) return;
    // The wheel and the arrow keys scroll by hand.
    for (int i = 0; i < 90; ++i) u.gm.tick();
    int s0 = g->scroll();
    u.wheel(-1);
    CHECK(g->scroll() > s0);
    u.key(kKeyUp);
    CHECK(g->scroll() == s0);
    // Original Credits: the game's own screen, the new one gone.
    int bx, by, bw, bh;
    EtherCreditsGump::button_rect(0, bx, by, bw, bh);
    u.move(bx + bw / 2, by + bh / 2);
    u.gm.tick();
    u.shot("credits_button_hover");
    u.click(1);
    u.gm.tick();
    CHECK(u.oc() != nullptr);
    CHECK(u.w().gumps.find<EtherCreditsGump>() == nullptr);
    for (int i = 0; i < 120; ++i) u.gm.tick();
    u.shot("credits_original");
    // Out of the original: Esc asks, Yes leaves; the menu is there.
    u.key(kKeyEscape);
    for (int i = 0; i < 12; ++i) u.gm.tick();
    u.key('y');
    u.gm.tick();
    CHECK(!u.oc() && u.menu_up());
    // Back, Esc, the right button, the pad's B: back to the menu; the pad's Y: the original.
    g = u.open();
    EtherCreditsGump::button_rect(1, bx, by, bw, bh);
    u.move(bx + bw / 2, by + bh / 2);
    u.click(1);
    u.gm.tick();
    CHECK(g && !u.ec() && u.menu_up());
    g = u.open();
    u.key(kKeyEscape);
    u.gm.tick();
    CHECK(g && !u.ec() && u.menu_up());
    g = u.open();
    u.move(160, 100);
    u.click(3);
    u.gm.tick();
    CHECK(g && !u.ec() && u.menu_up());
    g = u.open();
    u.pad(kPadB);
    u.gm.tick();
    CHECK(g && !u.ec() && u.menu_up());
    g = u.open();
    u.pad(kPadY);
    u.gm.tick();
    CHECK(g && u.oc() != nullptr);
    // 'O' from the keyboard does the same.
    u.key(kKeyEscape);
    for (int i = 0; i < 12; ++i) u.gm.tick();
    u.key('y');
    u.gm.tick();
    g = u.open();
    u.key('o');
    u.gm.tick();
    CHECK(g && u.oc() != nullptr);
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    test_roll(d);
    test_buttons(d);
    test_links(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
