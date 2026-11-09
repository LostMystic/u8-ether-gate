// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Options page tests (MenuGump kOptions, game/options_page.cpp; gumps.md 2.3 + the engine's
// extras): the page text areas found in the book picture; every line of every spread inside
// them (boxes, and the pixels actually drawn); Debug Mode on the page; every row changes and
// is still changed in a new game mode loaded from the settings file; keyboard, mouse (click,
// right click, wheel, dog-ears) and controller (D-pad, A, B, LB / RB); pagination with many
// rows; screenshots of every spread.
//   pagan_options_tests GAME_DIR [SHOT_DIR] [SAVE_DIR]
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "data/shape.h"
#include "game/party.h"
#include "game/world.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {
std::string g_shots, g_saves;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d) : gm(d) {
        gm.world().skip_opening = true;
        gm.world().ask_name = false;
        gm.saves().set_dir(g_saves);
        gm.start();
        gm.world().load_options();
        if (Party* p = gm.world().party.get()) {  // as main() reads the companions settings
            p->enabled = Config::global().get_bool("companions", true);
            p->show_bar = Config::global().get_bool("party_bar", true);
        }
        frames(10);
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
    void click(int b) { button(b, true); button(b, false); }
    void wheel(int d) { InputEvent e; e.type = InputEvent::kWheel; e.wheel = d; e.x = gm.mouse_x(); e.y = gm.mouse_y(); ev(e); }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    void save(const std::string& name) {
        if (g_shots.empty()) return;
        move(316, 196);
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 3);
        std::printf("  shot %s\n", path.c_str());
    }
};

std::string file_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Rect { int x, y, w, h; };
bool inside(const Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

// Every line of every spread inside its page, clear of the dog-ears; the drawn pixels too.
void check_layout(Ui& u, MenuGump* book, const std::string& shot_prefix) {
    MenuGump::OptionLayout L = book->option_layout();
    const ShapeFont& f = u.w().font(MenuGump::kOptionFont);
    const ShapeFont& tf = u.w().font(MenuGump::kOptionTitleFont);
    Rect page[2];
    for (int s = 0; s < 2; ++s) book->option_page_rect(s, page[s].x, page[s].y, page[s].w, page[s].h);
    CHECK(L.pages >= 1);
    int bad_box = 0, bad_text = 0, bad_dog = 0;
    for (const auto& ln : L.lines) {
        const Rect& p = page[ln.page % 2];
        bool box_in = ln.x >= p.x && ln.y >= p.y && ln.x + ln.w <= p.x + p.w && ln.y + ln.h <= p.y + p.h;
        if (!box_in) { ++bad_box; std::printf("  line outside its page: %s (%d,%d %dx%d)\n", ln.label.c_str(), ln.x, ln.y, ln.w, ln.h); }
        // The text boxes: label from x + indent, value ending at the right edge, not overlapping.
        if (ln.row >= 0) {
            int lx0 = ln.x + 3, lx1 = lx0 + f.text_width(ln.label);
            int vx0 = ln.value_x, vx1 = vx0 + f.text_width(ln.value);
            bool ok = lx0 >= p.x && vx1 <= p.x + p.w && vx1 <= ln.x + ln.w && (ln.value.empty() || lx1 < vx0) &&
                      ln.y + f.height() <= p.y + p.h;
            if (!ok) { ++bad_text; std::printf("  text overflow: %s %s\n", ln.label.c_str(), ln.value.c_str()); }
        } else if ((ln.row == -1 ? tf : f).text_width(ln.label) > ln.w) {
            ++bad_text;
        }
        int sp = ln.page / 2;
        for (int which = 0; which < 2; ++which) {
            bool shown = which == 0 ? sp > 0 : sp < L.spreads() - 1;
            int dx, dy, dw, dh;
            book->option_dogear(which, dx, dy, dw, dh);
            // No drawn pixel of the corner inside the line's box (its box is half empty).
            const ShapeFrame* fr = u.w().data().gumps.frame(34, unsigned(which));
            bool hit = false;
            for (int yy = std::max(ln.y, dy); shown && yy < std::min(ln.y + ln.h, dy + dh); ++yy)
                for (int xx = std::max(ln.x, dx); xx < std::min(ln.x + ln.w, dx + dw); ++xx)
                    hit |= !fr || fr->opaque(xx - dx, yy - dy);
            if (hit) ++bad_dog;
        }
    }
    CHECK_EQ(bad_box, 0);
    CHECK_EQ(bad_text, 0);
    CHECK_EQ(bad_dog, 0);
    // The pixels: the page as painted against the bare book (with the dog-ears).
    int saved = book->option_spread;
    for (int sp = 0; sp < L.spreads(); ++sp) {
        book->option_spread = sp;
        Surface8 a(320, 200), b(320, 200);
        a.clear(0);
        b.clear(0);
        book->Gump::paint(a);
        for (int which = 0; which < 2; ++which) {
            bool shown = which == 0 ? sp > 0 : sp < L.spreads() - 1;
            int dx, dy, dw, dh;
            book->option_dogear(which, dx, dy, dw, dh);
            if (const ShapeFrame* f = u.w().data().gumps.frame(34, unsigned(which)); f && shown)
                a.draw(*f, book->x + dx + f->xoff, book->y + dy + f->yoff);
        }
        book->paint(b);
        int outside = 0, drawn = 0;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 320; ++x) {
                if (a.row(y)[x] == b.row(y)[x]) continue;
                ++drawn;
                int lx = x - book->x, ly = y - book->y;
                if (!inside(page[0], lx, ly) && !inside(page[1], lx, ly)) ++outside;
            }
        std::printf("  spread %d: %d pixels of text, %d outside the pages\n", sp + 1, drawn, outside);
        CHECK(drawn > 200);
        CHECK_EQ(outside, 0);
        if (!shot_prefix.empty()) u.save(shot_prefix + std::to_string(sp + 1));
    }
    book->option_spread = saved;
}

int row_of(MenuGump* book, const std::string& label) {
    std::vector<MenuGump::OptionRow> rows = book->option_rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].label == label) return int(i);
    return -1;
}

std::string value_of(MenuGump* book, int row) {
    std::vector<MenuGump::OptionRow> rows = book->option_rows();
    return row >= 0 && row < int(rows.size()) && rows[size_t(row)].value ? rows[size_t(row)].value() : "";
}

// The screen centre of a row's line (on the current spread).
bool row_point(MenuGump* book, int row, int& sx, int& sy) {
    MenuGump::OptionLayout L = book->option_layout();
    for (const auto& ln : L.lines)
        if (ln.row == row && ln.page / 2 == book->option_spread) {
            sx = book->x + ln.x + ln.w / 2;
            sy = book->y + ln.y + ln.h / 2;
            return true;
        }
    return false;
}

void test_layout(const GameData& d) {
    std::printf("layout\n");
    Ui u(d);
    MenuGump* book = u.w().open_menu(MenuGump::kOptions);
    u.frames(2);
    // The page areas found in the book picture: two pages left and right of the gutter.
    int px[2], py[2], pw[2], ph[2];
    for (int s = 0; s < 2; ++s) book->option_page_rect(s, px[s], py[s], pw[s], ph[s]);
    std::printf("  pages (gump-local): left %d,%d %dx%d, right %d,%d %dx%d\n", px[0], py[0], pw[0], ph[0], px[1], py[1],
                pw[1], ph[1]);
    CHECK(px[0] > 20 && px[0] + pw[0] < book->w / 2 && px[1] > book->w / 2 && px[1] + pw[1] < book->w - 20);
    CHECK(pw[0] > 80 && pw[1] > 80 && ph[0] > 110 && ph[1] > 110);
    // Debug Mode is on the page, under the Extras & Debug title.
    MenuGump::OptionLayout L = book->option_layout();
    int dbg = row_of(book, "Debug Mode:");
    CHECK(dbg >= 0);
    bool title = false, shown = false;
    std::string last_title;
    for (const auto& ln : L.lines) {
        if (ln.row < 0) last_title = ln.label;
        if (ln.row == dbg) { shown = ln.label == "Debug Mode:"; title = last_title == "Extras & Debug"; }
    }
    CHECK(shown);
    CHECK(title);
    // Every row is on some page (all are visible here, the Zoom row too).
    std::vector<MenuGump::OptionRow> rows = book->option_rows();
    int placed = 0;
    int hints = 0;
    for (const auto& ln : L.lines) { placed += ln.row >= 0; hints += ln.row == -2; }
    std::printf("  hint lines: %d\n", hints);
    CHECK_EQ(placed, int(rows.size()));
    std::printf("  %d rows, %d pages\n", int(rows.size()), L.pages);
    for (const auto& ln : L.lines)
        std::printf("    p%d %-16s %s\n", ln.page + 1, (ln.row < 0 ? "[" + ln.label + "]" : ln.label).c_str(), ln.value.c_str());
    book->select_option(row_of(book, "Text Speed:"));
    check_layout(u, book, "options_page");
    // Many more rows (another feature's long list, long labels and values): pagination.
    size_t before = World::extra_option_rows().size();
    for (int i = 0; i < 24; ++i)
        World::extra_option_rows().push_back([i](World&) {
            return MenuGump::OptionRow{"Test Option Number " + std::to_string(i) + ":",
                                       [] { return std::string("A Very Long Value Indeed"); }, [](int) {},
                                       i < 12 ? "Testing" : ""};
        });
    L = book->option_layout();
    std::printf("  with 24 more rows: %d pages, %d spreads\n", L.pages, L.spreads());
    CHECK(L.spreads() >= 2);
    check_layout(u, book, "options_page_paged");
    // Dog-ear and PgDn / PgUp turns.
    book->option_spread = 0;
    int dx, dy, dw, dh;
    book->option_dogear(1, dx, dy, dw, dh);
    u.move(book->x + dx + dw / 2, book->y + dy + dh / 2);
    u.click(1);
    CHECK_EQ(book->option_spread, 1);
    book->option_dogear(0, dx, dy, dw, dh);
    u.move(book->x + dx + dw / 2, book->y + dy + dh / 2);
    u.click(1);
    CHECK_EQ(book->option_spread, 0);
    u.key(kKeyPageDown);
    CHECK_EQ(book->option_spread, 1);
    u.pad(kPadLB);
    CHECK_EQ(book->option_spread, 0);
    u.pad(kPadRB);
    CHECK_EQ(book->option_spread, 1);
    // Down through every row walks every spread.
    book->select_option(-1);
    int max_spread = 0;
    for (int i = 0; i < placed + 24; ++i) { u.key(kKeyDown); max_spread = std::max(max_spread, book->option_spread); }
    CHECK_EQ(max_spread, L.spreads() - 1);
    World::extra_option_rows().resize(before);
    u.key(kKeyEscape);
    CHECK(book->mode() == MenuGump::kMenu);
}

void test_rows(const GameData& d) {
    std::printf("rows change and persist\n");
    std::vector<std::string> labels, changed;
    {
        Ui u(d);
        std::vector<uint32_t> rgba;
        int fw, fh;
        MenuGump* book = u.w().open_menu(MenuGump::kOptions);
        u.frames(1);
        u.gm.render_frame(rgba, fw, fh, 4, 0.0, 1.0 / 60);  // a 4x window: zoom levels exist
        std::vector<MenuGump::OptionRow> rows = book->option_rows();
        for (int i = 0; i < int(rows.size()); ++i) {
            std::string v0 = value_of(book, i);
            std::string cfg0 = file_text(Config::global().path());
            book->change_option(i, 1);
            if (value_of(book, i) == v0) book->change_option(i, -1);  // a slider at its top
            std::string v1 = value_of(book, i);
            bool saved = file_text(Config::global().path()) != cfg0;
            std::printf("  %-16s %s -> %s%s\n", rows[size_t(i)].label.c_str(), v0.c_str(), v1.c_str(), saved ? "" : "  (not saved!)");
            CHECK(v1 != v0);
            CHECK(saved);
            labels.push_back(rows[size_t(i)].label);
            changed.push_back(v1);
        }
        u.key(kKeyEscape);
        u.key(kKeyEscape);
    }
    // A new game mode reads the settings file again: every row still shows its new value.
    Config::global().load(Config::global().path());
    {
        Ui u(d);
        std::vector<uint32_t> rgba;
        int fw, fh;
        MenuGump* book = u.w().open_menu(MenuGump::kOptions);
        u.gm.render_frame(rgba, fw, fh, 4, 0.0, 1.0 / 60);
        for (size_t i = 0; i < labels.size(); ++i) {
            int r = row_of(book, labels[i]);
            std::string v = value_of(book, r);
            if (v != changed[i]) std::printf("  not kept: %s %s (expected %s)\n", labels[i].c_str(), v.c_str(), changed[i].c_str());
            CHECK(v == changed[i]);
        }
        // Back to the defaults for the other tests (debug mode off).
        Config::global().set_bool("debug_mode", false);
        Config::global().save();
    }
}

void test_input(const GameData& d) {
    std::printf("input\n");
    Ui u(d);
    u.w().options.text_speed = 5;
    u.w().options.music = true;
    MenuGump* book = u.w().open_menu(MenuGump::kOptions);
    u.frames(2);
    const int ts = row_of(book, "Text Speed:"), mus = row_of(book, "Music:"), dbg = row_of(book, "Debug Mode:");
    // Keyboard: the first row is selected; Down / Up move in page order; Right / Left / Enter.
    CHECK(book->option_selected() >= 0);
    book->select_option(ts);
    u.key(kKeyRight);
    CHECK_EQ(u.w().options.text_speed, 6);
    u.key(kKeyLeft);
    u.key(kKeyLeft);
    CHECK_EQ(u.w().options.text_speed, 4);
    u.key(kKeyReturn);
    CHECK_EQ(u.w().options.text_speed, 5);
    int s0 = book->option_selected();
    u.key(kKeyDown);
    CHECK(book->option_selected() != s0);
    u.key(kKeyUp);
    CHECK_EQ(book->option_selected(), s0);
    // Mouse: hover selects, a click changes, a right click goes back, the wheel steps a slider.
    int sx, sy;
    CHECK(row_point(book, mus, sx, sy));
    u.move(sx, sy);
    CHECK_EQ(book->option_selected(), mus);
    u.click(1);
    CHECK(!u.w().options.music);
    u.click(3);
    CHECK(u.w().options.music);
    CHECK(row_point(book, ts, sx, sy));
    u.move(sx, sy);
    u.wheel(1);
    CHECK_EQ(u.w().options.text_speed, 6);
    u.wheel(-1);
    CHECK_EQ(u.w().options.text_speed, 5);
    // Controller: D-pad moves, A changes, B goes back to the menu.
    u.move(316, 196);
    book->select_option(dbg);
    bool on = Config::global().get_bool("debug_mode", false);
    u.pad(kPadA);
    CHECK(Config::global().get_bool("debug_mode", false) != on);
    u.pad(kPadA);
    CHECK(Config::global().get_bool("debug_mode", false) == on);
    u.pad(kPadUp);
    CHECK(book->option_selected() != dbg);
    u.pad(kPadDown);
    CHECK_EQ(book->option_selected(), dbg);
    u.pad(kPadB);
    CHECK(book->mode() == MenuGump::kMenu);
}
}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_shots = argc > 2 ? argv[2] : "";
    g_saves = argc > 3 ? argv[3] : "/tmp/pagan_options_saves";
    mkdir(g_saves.c_str(), 0755);
    std::remove((g_saves + "/pagan_test.cfg").c_str());
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    Party::register_options();  // the companions rows, as main() registers them
    test_layout(d);
    test_input(d);
    test_rows(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
