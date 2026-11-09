// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 4 tests: savegame round trips (save, load, and the loaded world runs on exactly
// like the original), gumps (containers, paperdoll, slider, readers, the menu book) and drag
// and drop between the world and windows, driven through the game mode's input handling.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "play.h"
#include "app/game_mode.h"
#include "core/serial.h"
#include "platform/platform.h"
#include "core/config.h"
#include "data/gamedata.h"
#include "game/processes.h"
#include "game/savegame.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"

using namespace pg;

namespace {

std::vector<uint8_t> snapshot(const World& w) {
    SaveWriter out;
    w.save_game(out, "test");
    return out.data();
}

void run(World& w, int frames, int& clock) {
    for (int i = 0; i < frames; ++i) {
        w.input.now_ms = uint32_t(clock * 1000 / 30);
        w.frame();
        ++clock;
    }
}

size_t first_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return a.size() == b.size() ? size_t(-1) : n;
}

void test_save_roundtrip(const GameData& d, int at, int after) {
    World w(d);
    w.auto_ui = true;
    w.new_game();
    int clock = 0;
    run(w, at, clock);
    std::vector<uint8_t> a = snapshot(w);
    CHECK(a.size() > 1000);
    // Loading and saving again gives the same bytes.
    World w2(d);
    w2.auto_ui = true;
    w2.new_game();
    SaveReader in(a.data(), a.size());
    CHECK(w2.load_game(in));
    std::vector<uint8_t> a2 = snapshot(w2);
    CHECK_EQ(a2.size(), a.size());
    CHECK(first_diff(a, a2) == size_t(-1));
    // Both worlds run on identically (all processes, VM state, objects, random numbers).
    int clock2 = clock;
    unsigned procs = unsigned(w.kernel().size());
    run(w, after, clock);
    run(w2, after, clock2);
    std::vector<uint8_t> b = snapshot(w), b2 = snapshot(w2);
    size_t diff = first_diff(b, b2);
    if (diff != size_t(-1)) std::printf("  round trip: worlds differ at byte %zu of %zu\n", diff, b.size());
    CHECK(diff == size_t(-1));
    CHECK_EQ(w2.avatar()->x, w.avatar()->x);
    CHECK_EQ(w2.kernel().size(), w.kernel().size());
    std::printf("  round trip at frame %d: save %zu bytes, %u processes, %zu objects; equal after %d frames\n", at,
                a.size(), procs, w.objects().live_count(), after);
}

// The first story gate (specs/world/map_index.md 3): the execution on the dock destroys the
// portcullis, then the way into Tenebrae is open.
void test_tenebrae_gate(const GameData& d) {
    Play p(d);
    auto portcullis = [&]() {
        std::vector<uint16_t> near;
        p.w.area_search(11775, 2015, 256, false, near);
        for (uint16_t k : near)
            if (const Item* it = p.w.item(k); it && it->shape == 633) return true;
        return false;
    };
    CHECK(portcullis());
    CHECK_EQ(p.w.vm().get_global(89, 1), 0);
    p.warp(11732, 5844, 48, 3);
    bool started = false;
    int f = 0;
    for (; f < 9000; ++f) {
        p.step();
        if (p.w.stasis) started = true;
        if (started && !p.w.stasis) break;
    }
    CHECK(started);
    CHECK_EQ(p.w.vm().get_global(89, 1), 1);
    CHECK(!portcullis());
    std::printf("  execution scene: %d frames, portcullis gone: %s\n", f, portcullis() ? "no" : "yes");
    // Walk to the city teleporter behind the gate (the guard may talk first).
    p.warp(11620, 2350, 48, 3);
    for (int tries = 0; tries < 6 && p.w.map().map_number() == 3; ++tries) {
        p.walk_to(11516, 1660, 1500, true);
        for (int k = 0; k < 3000 && p.w.stasis; ++k) p.step();
        p.run(30);
    }
    CHECK_EQ(p.w.map().map_number(), 40u);
    std::printf("  into Tenebrae: map %u at %d,%d,%d by frame %d\n", p.w.map().map_number(), p.av().x, p.av().y,
                p.av().z, p.frames);
}

// Drives the real game mode: input events in, a rendered frame per tick (picking needs it).
struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, const std::string& saves) : gm(d) {
        gm.world().skip_opening = true;
        gm.world().ask_name = false;
        gm.saves().set_dir(saves);
        gm.start();
        frames(40);
    }
    World& w() { return gm.world(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
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
    void type(const std::string& t) {
        InputEvent e; e.type = InputEvent::kText;
        std::strncpy(e.text, t.c_str(), sizeof(e.text) - 1);
        ev(e);
    }
    // A single left click (delivered after the double-click time).
    void click(int x, int y) { move(x, y); button(1, true); frames(1); button(1, false); frames(16); }
    void double_click(int x, int y) { move(x, y); button(1, true); button(1, false); frames(2); button(1, true); button(1, false); frames(16); }
    // Left-drag from one screen point to another (a few moves on the way).
    void drag(int x0, int y0, int x1, int y1) {
        frames(15);  // not a double-click with the previous press
        move(x0, y0); button(1, true); frames(1);
        for (int k = 1; k <= 4; ++k) { move(x0 + (x1 - x0) * k / 4, y0 + (y1 - y0) * k / 4); frames(1); }
        button(1, false); frames(2);
    }
    // A screen pixel where gump g shows item id (0,0 if none).
    bool find_item(Gump* g, uint16_t id, int& sx, int& sy) {
        for (int y = 0; y < g->h; ++y)
            for (int x = 0; x < g->w; ++x)
                if (g->trace_item(x, y) == id) { sx = g->x + x; sy = g->y + y; return true; }
        return false;
    }
    ContainerGump* backpack_gump() {
        uint16_t pack = w().equipped(1, 7);
        return dynamic_cast<ContainerGump*>(w().gumps.container_gump(pack));
    }
};

void test_gumps(const GameData& d, const std::string& saves) {
    Ui u(d, saves);
    World& w = u.w();
    CHECK(!w.stasis);
    uint16_t pack = w.equipped(1, 7);
    CHECK(pack != 0);
    // --- I opens the backpack through its usecode (openGump), Backspace closes it.
    u.key('i');
    u.frames(5);
    ContainerGump* bp = u.backpack_gump();
    CHECK(bp != nullptr);
    CHECK(w.item(pack)->flags & kItemGumpOpen);
    if (bp) { CHECK_EQ(bp->shape, 2u); CHECK(bp->x >= 0 && bp->y >= 0 && bp->x + bp->w <= 320); }
    u.key(kKeyBackspace);
    u.frames(2);
    CHECK(u.backpack_gump() == nullptr);
    CHECK(!(w.item(pack)->flags & kItemGumpOpen));

    // --- Items put in get random places inside the item area; a helm goes on by dragging it
    // from the backpack onto the paperdoll.
    uint16_t helm = w.create_item(531, 0);
    CHECK(w.give_to_avatar(helm));
    uint16_t coins = w.create_item(143, 0);
    w.item(coins)->quality = 10;
    CHECK(w.give_to_avatar(coins));
    u.key('z');  // the paperdoll (the Avatar's use event)
    u.frames(5);
    auto* pd = dynamic_cast<PaperdollGump*>(w.gumps.container_gump(1));
    CHECK(pd != nullptr);
    u.key('i');
    u.frames(5);
    bp = u.backpack_gump();
    CHECK(bp != nullptr);
    if (!bp || !pd) return;
    // Move the backpack window aside by dragging its frame.
    int bx = bp->x, by = bp->y, gx = -1, gy = -1;
    for (int y = 0; y < bp->h && gx < 0; ++y)
        for (int x = 0; x < bp->w; ++x)
            if (bp->hit(x, y) && !bp->trace_item(x, y) && w.gumps.at(bp->x + x, bp->y + y) == bp) { gx = x; gy = y; break; }
    CHECK(gx >= 0);
    u.drag(bp->x + gx, bp->y + gy, bp->x + gx + 120, bp->y + gy + 60);
    CHECK_EQ(bp->x, std::min(bx + 120, bp->x));
    CHECK(bp->x != bx || bp->y != by);
    u.frames(2);
    int sx, sy;
    CHECK(u.find_item(bp, helm, sx, sy));
    const Item* hi = w.item(helm);
    int hx = hi->y & 0xFF, hy = hi->y >> 8;
    CHECK(hx >= 0 && hx < bp->area_w && hy >= 0 && hy < bp->area_h);
    // Drop it on the paperdoll's figure (not the backpack hot spot).
    u.drag(sx, sy, pd->x + 7 + 40, pd->y + 9 + 30);
    CHECK_EQ(w.equipped(1, 3), helm);
    CHECK(w.item(helm)->flags & kItemEquipped);
    CHECK_EQ(w.item(helm)->parent, 1);
    std::printf("  helm dragged from the backpack onto the paperdoll: slot 3 = %u\n", w.equipped(1, 3));

    // --- A stack dragged out to the world asks how many (slider); 7 of 10 are dropped.
    CHECK(u.find_item(bp, coins, sx, sy));
    int wx = 175, wy = 128;  // the ground just in front of the Avatar
    u.drag(sx, sy, wx, wy);
    SliderGump* sl = w.gumps.find<SliderGump>();
    CHECK(sl != nullptr);
    CHECK(w.kernel().paused());
    if (sl) {
        CHECK_EQ(sl->value(), 10);
        u.key(kKeyLeft); u.key(kKeyLeft); u.key(kKeyLeft);
        CHECK_EQ(sl->value(), 7);
        u.key(kKeyReturn);
        u.frames(3);
    }
    CHECK(w.gumps.find<SliderGump>() == nullptr);
    CHECK(!w.kernel().paused());
    CHECK_EQ(w.item(coins)->quality, 3);
    CHECK_EQ(w.item(coins)->parent, pack);
    uint16_t dropped = 0;
    {
        std::vector<uint16_t> near;
        w.area_search(w.avatar()->x, w.avatar()->y, 256, false, near);
        for (uint16_t k : near)
            if (const Item* it = w.item(k); it && it->shape == 143 && !it->parent) dropped = k;
    }
    CHECK(dropped != 0);
    if (dropped) CHECK_EQ(w.item(dropped)->quality, 7);
    std::printf("  stack split by the slider: %u left in the pack, %u on the ground (item %u)\n",
                w.item(coins)->quality, dropped ? w.item(dropped)->quality : 0, dropped);
    // --- Dragging the ground stack back onto the pack's stack merges them (slider again).
    u.frames(30);
    if (dropped) {
        int px = 0, py = 0;
        w.screen_point(*w.item(dropped), px, py);
        bool found = false;
        for (int dy = -6; dy <= 2 && !found; ++dy)
            for (int dx = -6; dx <= 6 && !found; ++dx) {
                u.move(px + dx, py + dy);
                u.frames(1);
                if (w.input.cursor_item == dropped) { px += dx; py += dy; found = true; }
            }
        CHECK(found);
        CHECK(u.find_item(bp, coins, sx, sy));
        u.drag(px, py, sx, sy);
        if ((sl = w.gumps.find<SliderGump>())) { u.key(kKeyReturn); u.frames(3); }
        CHECK(w.item(dropped) == nullptr);
        CHECK_EQ(w.item(coins)->quality, 10);
    }
    // --- The helm back off the paperdoll into the backpack window.
    int hpx = 0, hpy = 0;
    CHECK(u.find_item(pd, helm, hpx, hpy));
    u.drag(hpx, hpy, bp->x + bp->area_l + bp->area_w / 2, bp->y + bp->area_t + bp->area_h / 2);
    CHECK_EQ(w.item(helm)->parent, pack);
    CHECK(!(w.item(helm)->flags & kItemEquipped));
    CHECK_EQ(w.equipped(1, 3), 0);
    // --- Double-clicking the paperdoll's backpack picture toggles the backpack window.
    u.frames(20);

    // --- Readers are modal: the world pauses; Space turns pages, Escape closes.
    u.w().gumps.close_item_dependent();
    uint16_t pid = w.open_reader(ReaderGump::kBook, 0, 6, "Page one*Page two*Page three*Page four*Page five");
    u.frames(2);
    ReaderGump* rg = w.reader();
    CHECK(rg != nullptr);
    CHECK(w.kernel().paused());
    if (rg) {
        CHECK_EQ(rg->pages.size(), 5u);
        u.key(kKeySpace);
        CHECK_EQ(rg->page, 2u);
        // A single click (held over a frame) turns the page after the double-click time.
        u.frames(20);
        u.click(rg->x + rg->w / 2, rg->y + rg->h / 2);
        CHECK_EQ(rg->page, 4u);
        u.key(kKeyEscape);
        u.frames(2);
    }
    CHECK(w.reader() == nullptr);
    CHECK(!w.kernel().paused());
    CHECK(w.kernel().get(pid) == nullptr || !w.kernel().get(pid)->live());
    // A plaque closes on any click.
    w.open_reader(ReaderGump::kPlaque, 0, 16, "A SIGN");
    u.frames(2);
    CHECK(w.reader() != nullptr);
    u.click(100, 100);
    CHECK(w.reader() == nullptr);

    // --- The slider intrinsic suspends the caller and wakes it with the value.
    {
        auto g = std::make_unique<SliderGump>(w, 0, 20, 2, 0);
        SliderGump* s2 = g.get();
        w.gumps.add(std::move(g));
        u.frames(1);
        u.key(kKeyRight); u.key(kKeyRight);
        CHECK_EQ(s2->value(), 4);
        // The knob follows the mouse while held.
        u.move(s2->x + 70 + 4, s2->y + 20); u.button(1, true); u.frames(1);
        u.move(s2->x + 130, s2->y + 20); u.frames(1);
        u.button(1, false); u.frames(1);
        CHECK(s2->value() > 4);
        u.key(kKeyReturn);
        u.frames(2);
        CHECK(w.gumps.find<SliderGump>() == nullptr);
    }

    // --- Escape opens the menu book; Write Diary, pick entry 2, type, Enter saves.
    w.gumps.close_item_dependent();
    int ax = w.avatar()->x, ay = w.avatar()->y;
    u.key(kKeyEscape);
    u.frames(2);
    MenuGump* menu = w.gumps.find<MenuGump>();
    CHECK(menu != nullptr);
    CHECK(w.kernel().paused());
    if (!menu) return;
    u.key('3');
    CHECK(menu->mode() == MenuGump::kSave);
    menu->spread = 0;
    // Entry 2 = column 0, row 1 of the first spread.
    u.click(menu->x + (307 - 220) / 2 + 20, menu->y + 6 + 43 + 15);
    CHECK_EQ(menu->editing, 2);
    u.key(kKeyBackspace); u.key(kKeyBackspace); u.key(kKeyBackspace);
    for (int k = 0; k < 20; ++k) u.key(kKeyBackspace);
    u.type("Gump ");
    u.type("test");
    u.key(kKeyReturn);
    u.frames(3);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    CHECK(u.gm.saves().exists(2));
    CHECK(u.gm.saves().description(2) == "Gump test");
    // Walk away, then Read Diary entry 2 restores the saved position.
    w.teleport_actor(1, ax + 200, ay, w.avatar()->z, w.map().map_number());
    u.frames(5);
    CHECK(w.avatar()->x != ax);
    u.key(kKeyEscape);
    u.frames(1);
    menu = w.gumps.find<MenuGump>();
    CHECK(menu != nullptr);
    if (!menu) return;
    u.key('2');
    CHECK(menu->mode() == MenuGump::kLoad);
    CHECK_EQ(menu->spread, 0);
    u.click(menu->x + (307 - 220) / 2 + 20, menu->y + 6 + 43 + 15);
    u.frames(2);
    CHECK_EQ(w.avatar()->x, ax);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    CHECK(!w.kernel().paused());
    // --- Quit: Alt+X, then Y.
    u.key('x', kModAlt);
    u.frames(1);
    CHECK(w.gumps.find<QuitGump>() != nullptr);
    u.key('n');
    u.frames(1);
    CHECK(w.gumps.find<QuitGump>() == nullptr);
    CHECK(!u.gm.wants_quit());
    u.key('x', kModAlt);
    u.key('y');
    u.frames(1);
    CHECK(u.gm.wants_quit());
}

// Death (gumps.md 2.13): the gravestone, then the menu, which cannot be closed while dead;
// entry 1 of Read Diary starts a new game.
void test_death_menu(const GameData& d, const std::string& saves) {
    Ui u(d, saves);
    World& w = u.w();
    w.kill_actor(1, 1);
    bool grave = false;
    for (int f = 0; f < 600 && !w.gumps.find<MenuGump>(); ++f) {
        if (w.reader()) {
            grave = true;
            if (f % 60 == 59) u.key(kKeySpace);  // any key closes the gravestone
        }
        u.frames(1);
    }
    CHECK(grave);
    MenuGump* m = w.gumps.find<MenuGump>();
    CHECK(m != nullptr);
    if (!m) return;
    u.key(kKeyEscape);
    u.frames(2);
    CHECK(w.gumps.find<MenuGump>() != nullptr);  // refused while dead
    u.key('2');
    CHECK(m->mode() == MenuGump::kLoad);
    m->spread = 0;
    u.click(m->x + (307 - 220) / 2 + 20, m->y + 6 + 3 + 15);  // entry 1: The Beginning...
    u.frames(5);
    CHECK(!w.avatar()->is(Actor::kDead));
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    CHECK_EQ(w.avatar()->x, 14527);
}

// The menu book answers a single click at once (it used to wait for a double-click: the
// press was marked used while the button was held in a modal window), keys and the controller
// move a highlight, and a double-click does not act twice.
void test_menu_clicks(const GameData& d, const std::string& saves) {
    Ui u(d, saves);
    World& w = u.w();
    u.key(kKeyEscape);
    u.frames(2);
    MenuGump* m = w.gumps.find<MenuGump>();
    CHECK(m != nullptr);
    if (!m) return;
    auto entry = [&](int i, int& sx, int& sy) { sx = m->x + 167 + 8; sy = m->y + 18 + 14 * i + 5; };
    int sx, sy;
    // A single click on "4.Options", held for a few frames like a real click: acts on release.
    entry(3, sx, sy);
    u.move(sx, sy); u.frames(1);
    u.button(1, true); u.frames(3);
    CHECK(m->mode() == MenuGump::kMenu);
    u.button(1, false);
    CHECK(m->mode() == MenuGump::kOptions);
    // One click on an options row changes it at once.
    bool anim = w.options.animations;
    MenuGump::OptionLayout L = m->option_layout();
    for (const MenuGump::OptionLine& ln : L.lines)
        if (ln.row >= 0 && ln.page / 2 == m->option_spread && ln.label.rfind("Animations", 0) == 0) {
            u.frames(15);
            u.move(m->x + ln.x + 4, m->y + ln.y + ln.h / 2); u.frames(1);
            u.button(1, true); u.frames(2); u.button(1, false);
            break;
        }
    CHECK(w.options.animations != anim);
    w.options.animations = anim;
    w.store_options();  // the settings are global: later tests expect the defaults
    u.key(kKeyEscape);
    CHECK(m->mode() == MenuGump::kMenu);
    // A double-click on "2.Read Diary" opens the diary and stays there.
    u.frames(20);
    entry(1, sx, sy);
    u.double_click(sx, sy);
    CHECK(m->mode() == MenuGump::kLoad);
    CHECK(w.gumps.find<MenuGump>() == m);
    // The diary by keys: Down highlights the next entry, Esc goes back.
    int before = m->diary_sel;
    u.key(kKeyDown);
    CHECK(m->diary_sel != before);
    u.key(kKeyEscape);
    CHECK(m->mode() == MenuGump::kMenu);
    // Keys: Down highlights the first entry, Down again the second, Enter chooses it.
    u.key(kKeyDown);
    CHECK_EQ(m->selected_item(), 0);
    u.key(kKeyDown); u.key(kKeyDown); u.key(kKeyDown);
    CHECK_EQ(m->selected_item(), 3);
    u.key(kKeyReturn);
    CHECK(m->mode() == MenuGump::kOptions);
    u.key(kKeyEscape);
    // "6.Quit" opens the quit box on one click; one click on No closes it.
    u.frames(20);
    entry(5, sx, sy);
    u.click(sx, sy);
    QuitGump* q = w.gumps.find<QuitGump>();
    CHECK(q != nullptr);
    if (q) {
        u.frames(20);
        u.click(q->x + q->w - 16 - 5, q->y + 38 + 5);
        CHECK(w.gumps.find<QuitGump>() == nullptr);
        CHECK(!u.gm.wants_quit());
    }
    // A double-click on the book's empty page closes it.
    u.frames(20);
    u.double_click(m->x + 30, m->y + 140);
    u.frames(2);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    std::printf("  menu: single clicks, keys and double-clicks ok\n");
}

// The start menu (NOTES.md "Start menu"): with a save, the launch loads the most recent one
// and opens the menu over it; Continue / Esc play on, New Game starts afresh.
void test_start_menu(const GameData& d, const std::string& saves) {
    int sx = 0;
    {
        SaveStore old(saves + "/start");
        for (int sl = 2; sl <= SaveStore::kMaxSlot; ++sl) std::remove(old.path(sl).c_str());
        Ui u(d, saves + "/start");
        CHECK_EQ(u.gm.launch(true), 0);  // no saves yet: a plain new game
        CHECK(u.w().gumps.find<MenuGump>() == nullptr);
        u.frames(20);
        u.w().teleport_actor(1, 11700, 2200, 48, 3);
        u.frames(20);
        sx = u.w().avatar()->x;

        CHECK(u.gm.save_slot(4, "Start menu test"));
    }
    for (int pass = 0; pass < 3; ++pass) {
        Ui u(d, saves + "/start");
        World& w = u.w();
        CHECK_EQ(u.gm.launch(true), 4);
        MenuGump* m = w.gumps.find<MenuGump>();
        CHECK(m != nullptr && m->start_menu);
        if (!m) return;
        CHECK_EQ(m->selected_item(), int(MenuGump::kItemContinue));
        CHECK_EQ(w.avatar()->x, sx);  // the backdrop is the saved world
        CHECK(w.kernel().paused());
        u.frames(5);
        if (pass == 0) {
            u.key(kKeyReturn);  // Continue
        } else if (pass == 1) {
            u.key(kKeyEscape);  // Esc also plays on (the world is loaded)
        } else {
            // New Game, by one click on its line.
            int bx = 0;
            for (int lx = 0; lx < m->w && !bx; ++lx)
                if (m->menu_item_at(lx, 137) == MenuGump::kItemNewGame) bx = lx;
            CHECK(bx > 0);
            u.click(m->x + bx + 3, m->y + 137);
            u.frames(3);
            CHECK(w.gumps.find<MenuGump>() == nullptr);
            CHECK_EQ(w.avatar()->x, 14527);  // the start of a new game
            continue;
        }
        u.frames(2);
        CHECK(w.gumps.find<MenuGump>() == nullptr);
        CHECK(!w.kernel().paused());
        CHECK_EQ(w.avatar()->x, sx);
    }
    std::printf("  start menu: continue, esc and new game ok\n");
}

// Books with pages (NOTES.md "Start menu, menu clicks and the icon"): Write Diary opens on the
// first free entry after the newest one, ready to type; Read Diary on the newest save; the
// controller's LB / RB turn the pages of every paged book; the right-hand dog-ear is the mirror
// image of the left one, on the right page's corner, and turns the page on one click where it
// is drawn.
void test_books(const GameData& d, const std::string& saves, const std::string& shots) {
    std::string dir = saves + "/books";
    {
        SaveStore clean(dir);
        for (int sl = 2; sl <= SaveStore::kMaxSlot; ++sl) std::remove(clean.path(sl).c_str());
    }
    Config::global().set_int("save_last", 0);
    Ui u(d, dir);
    World& w = u.w();
    auto pad = [&](int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; e.pad = 7; u.ev(e);
        e.type = InputEvent::kPadUp; u.ev(e);
    };
    auto open = [&](int key) -> MenuGump* {
        u.key(kKeyEscape);
        u.frames(2);
        MenuGump* m = w.gumps.find<MenuGump>();
        if (!m) return nullptr;
        u.key(key);
        u.frames(1);
        return m;
    };
    auto close = [&]() {
        for (int k = 0; k < 4 && w.gumps.find<MenuGump>(); ++k) { u.key(kKeyEscape); u.frames(2); }
    };
    auto shot = [&](const std::string& name) {
        if (shots.empty()) return;
        u.frames(1);
        std::vector<uint32_t> rgba;
        u.surf.to_rgba(w.data().palette, u.gm.palette(), rgba);
        mkdir(shots.c_str(), 0755);
        save_png(shots + "/" + name + ".png", rgba.data(), 320, 200, 3);
        std::printf("  shot %s/%s.png\n", shots.c_str(), name.c_str());
    };

    // --- Where the diary opens.
    MenuGump* m = open('3');
    CHECK(m && m->mode() == MenuGump::kSave);
    if (!m) return;
    CHECK_EQ(m->spread, 0);
    CHECK_EQ(m->diary_sel, 2);   // no saves yet: the first entry after The Beginning
    CHECK_EQ(m->editing, 2);     // ready to type
    close();
    CHECK(u.gm.save_slot(2, "two"));
    CHECK(u.gm.save_slot(3, "three"));
    CHECK(u.gm.save_slot(4, "four"));
    m = open('3');
    CHECK_EQ(m->spread, 0);
    CHECK_EQ(m->diary_sel, 5);
    CHECK_EQ(m->editing, 5);
    shot("books_write_diary");
    close();
    m = open('2');
    CHECK(m->mode() == MenuGump::kLoad);
    CHECK_EQ(m->spread, 0);
    CHECK_EQ(m->diary_sel, 4);   // the newest save
    close();
    CHECK(u.gm.save_slot(5, "five"));
    CHECK(u.gm.save_slot(6, "six"));
    m = open('3');
    CHECK_EQ(m->spread, 1);      // entry 7 is on the second spread
    CHECK_EQ(m->diary_sel, 7);
    close();
    // Overwriting an old entry: the next free one after it (5 and 6 are taken).
    CHECK(u.gm.save_slot(9, "nine"));
    CHECK(u.gm.save_slot(4, "four again"));
    m = open('3');
    CHECK_EQ(m->diary_sel, 7);
    CHECK_EQ(m->spread, 1);
    close();
    m = open('2');
    CHECK_EQ(m->diary_sel, 4);
    CHECK_EQ(m->spread, 0);
    close();
    // Typing at once writes the offered entry (not the "3" that opened the page).
    u.key(kKeyEscape);
    u.frames(2);
    m = w.gumps.find<MenuGump>();
    u.key('3');
    u.type("3");  // SDL sends the text of a key with it, in the same frame
    u.frames(1);
    CHECK(m->mode() == MenuGump::kSave);
    CHECK(m->edit_text.empty());
    u.type("Typed");
    u.key(kKeyReturn);
    u.frames(3);
    CHECK(w.gumps.find<MenuGump>() == nullptr);
    CHECK(u.gm.saves().description(7) == "Typed");
    // Up / Down move off the offered entry; typing then writes the highlighted one.
    m = open('3');
    CHECK_EQ(m->diary_sel, 8);
    u.key(kKeyDown);
    CHECK_EQ(m->editing, 0);
    CHECK_EQ(m->diary_sel, 9);
    u.frames(1);
    u.type("x");
    CHECK_EQ(m->editing, 9);
    u.key(kKeyEscape);           // cancel the edit (the old text stays)
    CHECK_EQ(m->editing, 0);
    CHECK(u.gm.saves().description(9) == "nine");
    close();
    // An autosave is the newest save: Read Diary opens on the autosaves with it highlighted.
    // Write Diary stays with the diary (it used to open on the last page after an autosave).
    CHECK(u.gm.save_slot(SaveStore::kAutoFirst, "auto"));
    u.gm.saves().last_slot = SaveStore::kAutoFirst;
    m = open('2');
    CHECK_EQ(m->spread, -1);
    CHECK_EQ(m->diary_sel, SaveStore::kAutoFirst);
    close();
    m = open('3');
    CHECK(m->spread >= 0 && m->spread < 2);
    CHECK(m->diary_sel >= 2 && !u.gm.saves().exists(m->diary_sel));
    close();

    // --- The dog-ears: the right one mirrors the left one, on the right page's corner.
    m = open('2');
    m->spread = 1;
    u.frames(1);
    shot("books_dogears");
    int lx, ly, lw, lh, rx, ry, rw, rh;
    m->dogear_box(0, lx, ly, lw, lh);
    m->dogear_box(1, rx, ry, rw, rh);
    CHECK_EQ(lx, 34);
    CHECK(rx + rw <= m->w - 38);  // on the page, not over its gilt edge and the cover
    {
        Surface8 page(320, 200);  // the two corners alone (the entries' text overlaps the left one)
        page.fill_rect(0, 0, 320, 200, 0);
        m->draw_dogear(page, 0);
        m->draw_dogear(page, 1);
        int same = 0, diff = 0;
        const ShapeFrame* f0 = w.data().gumps.frame(34, 0);
        for (int j = 0; j < lh; ++j)
            for (int i = 0; i < lw; ++i) {
                if (!f0 || !f0->opaque(i, j)) continue;
                uint8_t a = page.data()[size_t(m->y + ly + j) * 320 + size_t(m->x + lx + i)];
                uint8_t b = page.data()[size_t(m->y + ry + j) * 320 + size_t(m->x + rx + rw - 1 - i)];
                (a == b ? same : diff)++;
            }
        std::printf("  dog-ears: left at %d, right at %d (w %d); mirrored pixels equal %d, differ %d\n", lx, rx, m->w, same, diff);
        CHECK(same > 100 && diff == 0);  // the corners are mirror images
    }
    // One click on the drawn corner turns the page; the old place (beside the page) and the
    // empty part of the corner's box do not.
    const ShapeFrame* f1 = w.data().gumps.frame(34, 1);
    CHECK(f1 != nullptr);
    if (f1) {
        int ox = -1, oy = -1, ex = -1, ey = -1;
        for (int j = 0; j < f1->h; ++j)
            for (int i = 0; i < f1->w; ++i) {
                bool near = false;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) near |= f1->opaque(i + dx, j + dy);
                if (f1->opaque(i, j) && ox < 0 && j > 4 && i > 4 && i < 18) { ox = i; oy = j; }
                if (!near && ex < 0) { ex = i; ey = j; }
            }
        CHECK(ox >= 0 && ex >= 0);
        u.frames(20);
        u.click(m->x + rx + ox, m->y + ry + oy);
        CHECK_EQ(m->spread, 2);
        u.click(m->x + m->w - 38 + 14, m->y + 3 + 10);  // where it used to be drawn
        CHECK_EQ(m->spread, 2);
        if (ex >= 0) u.click(m->x + rx + ex, m->y + ry + ey);
        CHECK_EQ(m->spread, 2);
        // The left corner turns back.
        const ShapeFrame* f0 = w.data().gumps.frame(34, 0);
        u.click(m->x + lx + (f0 ? f0->w - 1 - ox : 10), m->y + ly + oy);
        CHECK_EQ(m->spread, 1);
    }
    // --- LB / RB turn the pages: Read Diary, Write Diary, the options page, books, scrolls.
    pad(kPadRB);
    CHECK_EQ(m->spread, 2);
    pad(kPadLB);
    pad(kPadLB);
    CHECK_EQ(m->spread, 0);
    u.key(kKeyPageDown);
    CHECK_EQ(m->spread, 1);
    close();
    m = open('3');
    int sp = m->spread;
    pad(kPadRB);
    CHECK_EQ(m->spread, sp + 1);
    CHECK_EQ(m->editing, 0);
    pad(kPadLB);
    CHECK_EQ(m->spread, sp);
    close();
    m = open('4');
    CHECK(m->mode() == MenuGump::kOptions);
    int spreads = m->option_layout().spreads();
    pad(kPadRB);
    CHECK_EQ(m->option_spread, std::min(1, spreads - 1));
    pad(kPadLB);
    CHECK_EQ(m->option_spread, 0);
    close();
    w.open_reader(ReaderGump::kBook, 0, 6, "Page one*Page two*Page three*Page four*Page five");
    u.frames(2);
    ReaderGump* rg = w.reader();
    CHECK(rg != nullptr);
    if (rg) {
        pad(kPadRB);
        CHECK_EQ(rg->page, 2u);
        pad(kPadRB);
        CHECK_EQ(rg->page, 4u);
        pad(kPadRB);                 // the last spread: stays open
        CHECK_EQ(rg->page, 4u);
        CHECK(w.reader() == rg);
        pad(kPadLB);
        CHECK_EQ(rg->page, 2u);
        u.key(kKeyPageUp);
        CHECK_EQ(rg->page, 0u);
        u.key(kKeyEscape);
        u.frames(2);
    }
    w.open_reader(ReaderGump::kScroll, 0, 19, "Scroll one*Scroll two");
    u.frames(2);
    rg = w.reader();
    CHECK(rg != nullptr);
    if (rg) {
        CHECK_EQ(rg->pages.size(), 2u);
        pad(kPadRB);
        CHECK_EQ(rg->page, 1u);
        pad(kPadLB);
        CHECK_EQ(rg->page, 0u);
        u.key(kKeyEscape);
        u.frames(2);
    }
    // Outside a book RB is still combat mode.
    CHECK(w.gumps.top_modal() == nullptr);
    pad(kPadRB);
    u.frames(2);
    CHECK(w.avatar()->is(Actor::kInCombat));
    pad(kPadRB);
    u.frames(2);
    CHECK(!w.avatar()->is(Actor::kInCombat));
    std::printf("  books: diary opens on the next free / newest entry, LB / RB turn pages, mirrored dog-ear ok\n");
}

}  // namespace

void test_milestone4(const GameData& d, const std::string& tmp) {
    test_books(d, tmp, tmp + "/books_shots");
    if (std::getenv("PAGAN_ONLY_BOOKS")) return;
    test_menu_clicks(d, tmp);
    test_start_menu(d, tmp);
    test_death_menu(d, tmp);
    test_gumps(d, tmp);
    test_tenebrae_gate(d);
    test_save_roundtrip(d, 200, 600);    // the opening scene: Devon walking, usecode waiting
    test_save_roundtrip(d, 3000, 1500);  // the conversation
    test_save_roundtrip(d, 8400, 600);   // after the opening, the world running
}
