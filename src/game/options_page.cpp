// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The options page of the menu book (MenuGump kOptions; gumps.md 2.3 and the engine's extra
// settings). The rows come from one table (option_rows(): the original seven, the volumes,
// zoom, and every row other features add through World::extra_option_rows()), grouped by
// their `group` and laid out on the book's pages: a group title in the book font, then one
// line per row in the diary font with the label on the left and the value right-aligned.
// The page text areas are found in the book picture itself (the parchment), every line is
// fitted to its page (labels shortened rather than overflowing) and drawn clipped to it.
// When the rows do not fit one spread, the dog-ears (shape 34) turn to the next one.
#include <algorithm>
#include <map>

#include "core/config.h"
#include "data/gamedata.h"
#include "data/shape.h"
#include "game/gumps.h"
#include "game/world.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "platform/input.h"

namespace pg {

namespace {

// Page geometry, measured from the book picture once (fallback: the v2.10 values).
struct BookPages {
    int x[2] = {35, 156}, y[2] = {4, 4}, r[2] = {142, 267}, b[2] = {138, 138};  // inclusive
};

// The parchment of each page: the colours that fill the middle of the two pages, then the
// widest run of columns (and rows) around each page's middle that is at least 90% parchment.
BookPages measure_pages(const ShapeFrame* f) {
    BookPages p;
    if (!f || f->w < 100 || f->h < 60) return p;
    auto px = [f](int x, int y) { return int(f->pixels[size_t(y) * size_t(f->w) + size_t(x)]); };
    int counts[256] = {};
    const int W = f->w, H = f->h, R = 24;
    for (int side = 0; side < 2; ++side) {
        int sx = side == 0 ? W / 4 : 3 * W / 4;
        for (int y = H / 2 - R; y < H / 2 + R; ++y)
            for (int x = sx - R; x < sx + R; ++x) ++counts[px(x, y)];
    }
    bool paper[256] = {};
    for (int i = 0; i < 256; ++i) paper[i] = i != ShapeFrame::kTransparent && counts[i] >= 8;
    for (int side = 0; side < 2; ++side) {
        int sx = side == 0 ? W / 4 : 3 * W / 4, sy = H / 2;
        auto col_ok = [&](int x) {
            int n = 0, y0 = H / 4, y1 = 3 * H / 4;
            for (int y = y0; y < y1; ++y) n += paper[px(x, y)];
            return n * 10 >= (y1 - y0) * 9;
        };
        int x0 = sx, x1 = sx;
        while (x0 > 0 && col_ok(x0 - 1)) --x0;
        while (x1 < W - 1 && col_ok(x1 + 1)) ++x1;
        auto row_ok = [&](int y) {
            int n = 0;
            for (int x = x0; x <= x1; ++x) n += paper[px(x, y)];
            return n * 10 >= (x1 - x0 + 1) * 9;
        };
        int y0 = sy, y1 = sy;
        while (y0 > 0 && row_ok(y0 - 1)) --y0;
        while (y1 < H - 1 && row_ok(y1 + 1)) ++y1;
        if (x1 - x0 < 60 || y1 - y0 < 60) return BookPages{};  // not a book: keep the fallback
        p.x[side] = x0; p.r[side] = x1; p.y[side] = y0; p.b[side] = y1;
    }
    return p;
}

// Margins inside the parchment.
constexpr int kMarginX = 5, kMarginTop = 4, kMarginBottom = 3;
// Rows: the diary font (6 pixels high) on a 9-pixel pitch; a 4-pixel gap after each group.
constexpr int kRowPad = 3, kGroupGap = 4, kTitleGap = 2, kIndent = 3, kValueGap = 5;
// The selected row's band: a darker parchment.
constexpr uint8_t kSelectColour = 162;
// The hint under the rows ('~' breaks the line).
const char* const kHint = "Click or Enter: change~Esc: back to the menu";

// Group order on the pages; groups not listed come after these, "Extras" always last.
const char* const kGroupOrder[] = {"Game", "Sound", "Controls", "Display", "Party"};

std::string group_title(const std::string& g) { return g == "Extras" ? "Extras & Debug" : g; }

// Rows registered without a group (the three-field form) by label.
std::string guess_group(const std::string& label) {
    static const std::map<std::string, std::string> k = {
        {"Music:", "Sound"}, {"Sound FX:", "Sound"}, {"Avatar Steps:", "Sound"}, {"Music Volume:", "Sound"},
        {"Sound Volume:", "Sound"}, {"Text Speed:", "Game"}, {"Animations:", "Game"}, {"Frame Skipping:", "Game"},
        {"Speed Limiting:", "Game"}, {"Autosave:", "Game"}, {"Controls:", "Controls"}, {"Move Assist:", "Controls"}, {"Easy Ledges:", "Controls"},
        {"Click Text:", "Controls"}, {"Smooth Scroll:", "Display"}, {"Zoom:", "Display"}, {"Companions:", "Party"},
        {"Party Bar:", "Party"}, {"Debug Mode:", "Extras"},
    };
    auto it = k.find(label);
    return it == k.end() ? "Extras" : it->second;
}

// Shortens a text to a width: drops characters from the end and ends it with '.'.
std::string fit(const ShapeFont& f, const std::string& s, int maxw) {
    if (maxw <= 0) return {};
    if (f.text_width(s) <= maxw) return s;
    std::string t = s;
    while (!t.empty()) {
        t.pop_back();
        while (!t.empty() && (t.back() == ' ' || t.back() == ':' || t.back() == '.')) t.pop_back();
        if (f.text_width(t + ".") <= maxw) return t + ".";
    }
    return {};
}

bool overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The rows

std::vector<MenuGump::OptionRow> MenuGump::option_rows() {
    World::Options& o = w_.options;
    using R = OptionRow;
    auto onoff = [](bool v) { return std::string(v ? "On" : "Off"); };
    auto vol = [](int v) { return "(" + std::to_string((v * 9 + 127) / 255) + ")"; };
    auto step_vol = [](int& v, int d) { int n = std::clamp((v * 9 + 127) / 255 + d, 0, 9); v = n * 255 / 9; };
    std::vector<OptionRow> r;
    // The original seven (gumps.md 2.3), in its order within their groups.
    // The volumes (an engine extra) sit right after their on/off rows.
    r.push_back({"Music:", [&o, onoff] { return onoff(o.music); }, [&o](int) { o.music = !o.music; }, "Sound"});
    r.push_back({"Music Volume:", [&o, vol] { return vol(o.music_volume); },
                 [&o, step_vol](int d) { step_vol(o.music_volume, d < 0 ? -1 : 1); }, "Sound", R::kSlider});
    r.push_back({"Sound FX:", [&o, onoff] { return onoff(o.sfx); }, [&o](int) { o.sfx = !o.sfx; }, "Sound"});
    r.push_back({"Sound Volume:", [&o, vol] { return vol(o.sfx_volume); },
                 [&o, step_vol](int d) { step_vol(o.sfx_volume, d < 0 ? -1 : 1); }, "Sound", R::kSlider});
    r.push_back({"Avatar Steps:", [&o, onoff] { return onoff(o.steps); }, [&o](int) { o.steps = !o.steps; }, "Sound"});
    r.push_back({"Animations:", [&o, onoff] { return onoff(o.animations); }, [&o](int) { o.animations = !o.animations; }, "Game"});
    r.push_back({"Text Speed:", [&o] { return "(" + std::to_string(o.text_speed) + ")"; },
                 [&o](int d) { o.text_speed = (o.text_speed + (d < 0 ? 9 : 1)) % 10; }, "Game", R::kSlider});
    r.push_back({"Frame Skipping:", [&o, onoff] { return onoff(o.frame_skip); }, [&o](int) { o.frame_skip = !o.frame_skip; }, "Game"});
    r.push_back({"Speed Limiting:", [&o, onoff] { return onoff(o.speed_limit); }, [&o](int) { o.speed_limit = !o.speed_limit; }, "Game"});
    // The engine's extras.
    World& w = w_;
    r.push_back({"Zoom:", [&w] { return w.zoom_value_ui ? w.zoom_value_ui() : std::string("100%"); },
                 [&w](int d) { if (w.zoom_change_ui) w.zoom_change_ui(d); }, "Display", R::kChoice,
                 [&w] { return bool(w.zoom_change_ui); }});
    for (const auto& extra : World::extra_option_rows()) {
        r.push_back(extra(w_));
        if (r.back().group.empty()) r.back().group = guess_group(r.back().label);
        if (r.back().label == "Controls:" && r.back().kind == R::kToggle) r.back().kind = R::kChoice;
    }
    return r;
}

void MenuGump::change_option(int i, int dir) {
    std::vector<OptionRow> rows = option_rows();
    if (i < 0 || i >= int(rows.size()) || !rows[size_t(i)].change) return;
    rows[size_t(i)].change(dir);
    w_.store_options();
}

// ---------------------------------------------------------------------------------------------
// Layout

void MenuGump::option_page_rect(int side, int& px, int& py, int& pw, int& ph) const {
    static const ShapeFrame* measured_for = nullptr;
    static BookPages pages;
    const ShapeFrame* f = gump_frame(35, 0);
    if (f != measured_for) { pages = measure_pages(f); measured_for = f; }
    side = side ? 1 : 0;
    px = pages.x[side] + kMarginX;
    py = pages.y[side] + kMarginTop;
    pw = pages.r[side] - pages.x[side] + 1 - 2 * kMarginX;
    ph = pages.b[side] - pages.y[side] + 1 - kMarginTop - kMarginBottom;
}

bool MenuGump::option_dogear(int which, int& dx, int& dy, int& dw, int& dh) const {
    dogear_box(which, dx, dy, dw, dh);
    return true;
}

MenuGump::OptionLayout MenuGump::option_layout() {
    OptionLayout L;
    std::vector<OptionRow> rows = option_rows();
    const ShapeFont& f = w_.font(kOptionFont);
    const ShapeFont& tf = w_.font(kOptionTitleFont);
    const int pitch = f.height() + kRowPad, title_h = tf.height();
    // Groups in page order, each with its visible rows (in table order).
    std::vector<std::string> order(std::begin(kGroupOrder), std::end(kGroupOrder));
    for (const OptionRow& r : rows)
        if (r.group != "Extras" && std::find(order.begin(), order.end(), r.group) == order.end()) order.push_back(r.group);
    order.push_back("Extras");
    // Flow: a group starts on the next page when its title and two rows (or all of a shorter
    // group) do not fit; a group that runs over repeats its title on the next page.
    int page = 0, cy = 0, top = 0, bottom = 0;
    auto start_page = [&](int p) {
        int px, py, pw, ph;
        option_page_rect(p % 2, px, py, pw, ph);
        page = p;
        top = cy = py;
        bottom = py + ph;
    };
    auto add = [&](int row, const std::string& label, const std::string& value, int h) {
        if (row >= 0 && page % 2 == 0 && page >= 2) {
            int dx, dy, dw, dh, px, py, pw, ph;
            option_dogear(0, dx, dy, dw, dh);
            option_page_rect(0, px, py, pw, ph);
            if (overlaps(px, cy, pw, h, dx, dy, dw, dh)) cy = dy + dh;
        }
        OptionLine ln;
        ln.row = row;
        ln.page = page;
        int px, py, pw, ph;
        option_page_rect(page % 2, px, py, pw, ph);
        ln.x = px; ln.y = cy; ln.w = pw; ln.h = h;
        ln.label = label; ln.value = value;
        L.lines.push_back(ln);
        cy += h;
    };
    start_page(0);
    for (const std::string& g : order) {
        std::vector<int> idx;
        for (int i = 0; i < int(rows.size()); ++i)
            if (rows[size_t(i)].group == g && (!rows[size_t(i)].visible || rows[size_t(i)].visible())) idx.push_back(i);
        if (idx.empty()) continue;
        int need = title_h + kTitleGap + std::min<int>(2, int(idx.size())) * pitch;
        if (cy != top && cy + need > bottom) start_page(page + 1);
        add(-1, group_title(g), "", title_h + kTitleGap);
        for (int i : idx) {
            if (cy + pitch > bottom) {
                start_page(page + 1);
                add(-1, group_title(g), "", title_h + kTitleGap);
            }
            add(i, rows[size_t(i)].label, rows[size_t(i)].value ? rows[size_t(i)].value() : std::string(), pitch);
        }
        cy += kGroupGap;
    }
    L.pages = page + 1;
    // The hint at the bottom of each spread's right page, or its left page, where there is room.
    const std::vector<TextPage> hint = layout_text(f, kHint, 200, 200);
    for (int sp = 0; sp < L.spreads() && !hint.empty(); ++sp)
        for (int side = 1; side >= 0; --side) {
            int px, py, pw, ph;
            option_page_rect(side, px, py, pw, ph);
            int used = py;
            for (const OptionLine& ln : L.lines)
                if (ln.page == 2 * sp + side) used = std::max(used, ln.y + ln.h);
            const int n = int(hint[0].lines.size()), adv = f.line_advance() + 1;
            int hy = py + ph - n * adv;
            if (hy < used + kGroupGap) continue;
            for (int i = 0; i < n; ++i) {
                OptionLine ln;
                ln.row = -2;
                ln.page = 2 * sp + side;
                ln.x = px; ln.y = hy + i * adv; ln.w = pw; ln.h = adv;
                ln.label = hint[0].lines[size_t(i)];
                L.lines.push_back(ln);
            }
            break;
        }
    // Keep clear of the dog-ears shown on each spread, then fit every text to its line.
    const int spreads = L.spreads();
    for (OptionLine& ln : L.lines) {
        int sp = ln.page / 2, side = ln.page % 2;
        for (int which = 0; which < 2; ++which) {
            bool shown = which == 0 ? sp > 0 : sp < spreads - 1;
            if (!shown || which != side) continue;
            // The corner's drawn columns in the line's rows (the box is half empty: a line
            // beside its thin tip keeps its full width).
            int dx, dy, dw, dh;
            option_dogear(which, dx, dy, dw, dh);
            const ShapeFrame* fr = gump_frame(34, which ? 1 : 0);
            int lo = dx + dw, hi = dx - 1;
            for (int yy = std::max(ln.y, dy); yy < std::min(ln.y + ln.h, dy + dh); ++yy)
                for (int xx = 0; xx < dw; ++xx)
                    if (!fr || fr->opaque(xx, yy - dy)) { lo = std::min(lo, dx + xx); hi = std::max(hi, dx + xx); }
            if (lo > hi || hi < ln.x || lo >= ln.x + ln.w) continue;
            int right = ln.x + ln.w;
            if (which == 0) ln.x = std::max(ln.x, hi + 3);
            else right = std::min(right, lo - 2);
            ln.w = std::max(0, right - ln.x);
        }
        if (ln.row < 0) {
            ln.label = fit(ln.row == -1 ? tf : f, ln.label, ln.w);
            continue;
        }
        // Both fit, or the value keeps what the label leaves (at least two fifths of the line).
        int avail = ln.w - kIndent;
        int lw = f.text_width(ln.label), vw = f.text_width(ln.value);
        if (lw + kValueGap + vw > avail) {
            ln.value = fit(f, ln.value, std::max(avail - kValueGap - lw, std::min(vw, avail * 2 / 5)));
            vw = f.text_width(ln.value);
        }
        ln.label = fit(f, ln.label, avail - vw - (vw ? kValueGap : 0));
        ln.value_x = ln.x + ln.w - vw;
    }
    return L;
}

// ---------------------------------------------------------------------------------------------
// Painting

void MenuGump::paint_options(Surface8& s) {
    OptionLayout L = option_layout();
    option_spread = std::clamp(option_spread, 0, std::max(0, L.spreads() - 1));
    if (option_spread > 0) draw_dogear(s, 0);
    if (option_spread < L.spreads() - 1) draw_dogear(s, 1);
    const ShapeFont& f = w_.font(kOptionFont);
    const ShapeFont& tf = w_.font(kOptionTitleFont);
    for (const OptionLine& ln : L.lines) {
        if (ln.page / 2 != option_spread) continue;
        int px, py, pw, ph;
        option_page_rect(ln.page % 2, px, py, pw, ph);
        s.set_clip(x + px, y + py, pw, ph);
        if (ln.row == -1) {
            int tx = ln.x + (ln.w - tf.text_width(ln.label)) / 2;
            tf.draw_top(s, x + tx, y + ln.y, ln.label);
        } else if (ln.row == -2) {
            f.draw_top(s, x + ln.x + (ln.w - f.text_width(ln.label)) / 2, y + ln.y, ln.label);
        } else {
            if (ln.row == option_sel_) s.fill_rect(x + ln.x, y + ln.y, ln.w, ln.h - 1, kSelectColour);
            int ty = ln.y + (ln.h - 1 - f.height() + 1) / 2;
            f.draw_top(s, x + ln.x + kIndent, y + ty, ln.label);
            f.draw_top(s, x + ln.value_x, y + ty, ln.value);
        }
        s.reset_clip();
    }
}

// ---------------------------------------------------------------------------------------------
// Input

int MenuGump::option_at(int lx, int ly) {
    OptionLayout L = option_layout();
    for (const OptionLine& ln : L.lines)
        if (ln.row >= 0 && ln.page / 2 == option_spread && lx >= ln.x && lx < ln.x + ln.w && ly >= ln.y &&
            ly < ln.y + ln.h)
            return ln.row;
    return -1;
}

void MenuGump::select_option(int row) {
    OptionLayout L = option_layout();
    for (const OptionLine& ln : L.lines)
        if (ln.row >= 0 && (row < 0 || ln.row == row)) {
            option_sel_ = ln.row;
            option_spread = ln.page / 2;
            return;
        }
}

void MenuGump::turn_options(int dir) {
    OptionLayout L = option_layout();
    int to = std::clamp(option_spread + dir, 0, std::max(0, L.spreads() - 1));
    if (to == option_spread) return;
    option_spread = to;
    // Select the first row of the new spread (the last when going back).
    int pick = -1;
    for (const OptionLine& ln : L.lines)
        if (ln.row >= 0 && ln.page / 2 == to && (pick < 0 || dir < 0)) pick = ln.row;
    if (pick >= 0) option_sel_ = pick;
}

bool MenuGump::options_press(int button, int lx, int ly) {
    down_button_ = 0;
    OptionLayout L = option_layout();
    for (int which = 0; which < 2; ++which) {
        bool shown = which == 0 ? option_spread > 0 : option_spread < L.spreads() - 1;
        if (shown && dogear_hit(which, lx, ly)) { down_button_ = 1 + which; return true; }
    }
    int row = option_at(lx, ly);
    if (row >= 0) down_button_ = 100 + row;
    return true;
}

bool MenuGump::options_release(int button, int lx, int ly) {
    int down = down_button_;
    down_button_ = 0;
    if (down == 1 || down == 2) {
        if (dogear_hit(down - 1, lx, ly)) turn_options(down == 1 ? -1 : 1);
        return true;
    }
    if (down >= 100 && option_at(lx, ly) == down - 100 && (button == 1 || button == 3)) {
        option_sel_ = down - 100;
        change_option(option_sel_, button == 1 ? 1 : -1);
    }
    return true;
}

void MenuGump::wheel(int dir) {
    if (mode_ != kOptions || !dir) return;
    std::vector<OptionRow> rows = option_rows();
    int r = option_hover_ >= 0 ? option_hover_ : option_sel_;
    if (r >= 0 && r < int(rows.size()) && rows[size_t(r)].kind != OptionRow::kToggle) {
        option_sel_ = r;
        change_option(r, dir > 0 ? 1 : -1);
        return;
    }
    options_key(dir > 0 ? kKeyUp : kKeyDown);
}

bool MenuGump::options_key(int key) {
    OptionLayout L = option_layout();
    std::vector<int> order;  // rows in page order
    for (const OptionLine& ln : L.lines)
        if (ln.row >= 0) order.push_back(ln.row);
    if (order.empty()) return true;
    int at = int(std::find(order.begin(), order.end(), option_sel_) - order.begin());
    bool has = at < int(order.size());
    int n = int(order.size());
    switch (key) {
    case kKeyUp: select_option(order[size_t(has ? (at + n - 1) % n : n - 1)]); break;
    case kKeyDown: select_option(order[size_t(has ? (at + 1) % n : 0)]); break;
    case kKeyHome: select_option(order.front()); break;
    case kKeyEnd: select_option(order.back()); break;
    case kKeyPageUp: turn_options(-1); break;
    case kKeyPageDown: turn_options(1); break;
    case kKeyLeft: case kKeyRight: case kKeyReturn: case kKeySpace:
        if (has) change_option(option_sel_, key == kKeyLeft ? -1 : 1);
        else if (key == kKeyLeft || key == kKeyRight) turn_options(key == kKeyLeft ? -1 : 1);
        break;
    default: break;
    }
    return true;
}

}  // namespace pg
