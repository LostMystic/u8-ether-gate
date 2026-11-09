// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "game/debug_menu.h"

#include <algorithm>
#include <cstdlib>

#include "data/gamedata.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"

namespace pg {

namespace {
constexpr int kW = 300, kH = 186;
constexpr int kFont = 0;
constexpr int kTop = 16;     // first row
constexpr int kBottom = 14;  // status line height

uint8_t nearest(const Palette& p, int r, int g, int b) {
    int best = 1, bd = 1 << 30;
    for (int i = 1; i < 256; ++i) {
        int dr = p.rgb8[size_t(i)][0] - r, dg = p.rgb8[size_t(i)][1] - g, db = p.rgb8[size_t(i)][2] - b;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}
}  // namespace

DebugMenuGump::DebugMenuGump(World& w, DebugPage root) : Gump(w) {
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHand;
    this->w = kW;
    h = kH;
    x = (320 - kW) / 2;
    y = (200 - kH) / 2;
    const Palette& p = w.data().palette;
    col_bg_ = nearest(p, 40, 28, 16);
    col_hi_ = nearest(p, 100, 70, 30);
    col_edge_ = nearest(p, 200, 160, 80);
    col_dark_ = nearest(p, 12, 8, 4);
    col_title_ = nearest(p, 70, 48, 24);
    pages_.push_back(std::move(root));
    sel_stack_.push_back(0);
    rebuild();
    move_sel(0);
}

void DebugMenuGump::rebuild() {
    rows_ = pages_.back().build ? pages_.back().build() : std::vector<DebugRow>{};
    if (rows_.empty()) sel_ = 0;
    else sel_ = std::clamp(sel_, 0, int(rows_.size()) - 1);
}

int DebugMenuGump::visible_rows() const {
    int adv = w_.font(kFont).line_advance() + 1;
    return std::max(1, (kH - kTop - kBottom) / adv);
}

void DebugMenuGump::keep_visible() {
    int n = visible_rows();
    if (sel_ < top_) top_ = sel_;
    // Scrolling up to a row shows its section title too (a header right above it).
    if (sel_ == top_ && top_ > 0 && sel_ < int(rows_.size()) && rows_[size_t(top_ - 1)].header) --top_;
    if (sel_ >= top_ + n) top_ = sel_ - n + 1;
    top_ = std::clamp(top_, 0, std::max(0, int(rows_.size()) - n));
}

void DebugMenuGump::move_sel(int d) {
    int n = int(rows_.size());
    if (n == 0) return;
    auto hdr = [&](int i) { return rows_[size_t(i)].header; };
    int s = std::clamp(sel_, 0, n - 1);
    if (d == 1 || d == -1) {
        for (int k = 0; k < n; ++k) {  // wraps at the ends, skips section titles
            s = (s + d + n) % n;
            if (!hdr(s)) break;
        }
    } else {
        s = std::clamp(s + d, 0, n - 1);
        int dir = d < 0 ? -1 : 1;
        int t = s;
        while (t >= 0 && t < n && hdr(t)) t += dir;
        if (t < 0 || t >= n) { t = s; while (t >= 0 && t < n && hdr(t)) t -= dir; }
        s = std::clamp(t, 0, n - 1);
    }
    sel_ = s;
    keep_visible();
}

bool DebugMenuGump::select(const std::string& label) {
    rebuild();
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!rows_[i].header && rows_[i].label.rfind(label, 0) == 0) {
            sel_ = int(i);
            keep_visible();
            return true;
        }
    return false;
}

void DebugMenuGump::push(DebugPage p) {
    sel_stack_.back() = sel_;
    pages_.push_back(std::move(p));
    sel_stack_.push_back(0);
    sel_ = 0;
    top_ = 0;
    digits_.clear();
    status.clear();
    rebuild();
    move_sel(0);
}

void DebugMenuGump::back() {
    digits_.clear();
    status.clear();
    if (pages_.size() <= 1) { close(); return; }
    pages_.pop_back();
    sel_stack_.pop_back();
    sel_ = sel_stack_.back();
    rebuild();
    keep_visible();
}

void DebugMenuGump::activate(int dir) {
    if (sel_ < 0 || sel_ >= int(rows_.size())) return;
    DebugRow r = rows_[size_t(sel_)];  // the action may rebuild the rows
    if (r.header) return;
    if (dir == 0 && !digits_.empty() && r.number) {
        r.number(std::atoi(digits_.c_str()));
        digits_.clear();
        rebuild();
        return;
    }
    digits_.clear();
    if (!r.act) return;
    if (dir != 0 && !r.value) return;  // Left / Right only change values
    r.act(dir);
    if (r.closes && dir == 0) close();
    if (!closed) rebuild();
}

int DebugMenuGump::row_at(int ly) const {
    int adv = w_.font(kFont).line_advance() + 1;
    if (ly < kTop) return -1;
    int i = top_ + (ly - kTop) / adv;
    if (i >= top_ + visible_rows() || i >= int(rows_.size())) return -1;
    return i;
}

void DebugMenuGump::hover(int lx, int ly) {
    int i = row_at(ly);
    if (i >= 0 && !rows_[size_t(i)].header) sel_ = i;
}

bool DebugMenuGump::press(int button, int lx, int ly) {
    if (button == 3) { back(); return true; }
    if (button != 1) return true;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) { close(); return true; }  // outside: close
    int i = row_at(ly);
    if (i < 0 || rows_[size_t(i)].header) return true;
    sel_ = i;
    // A click on the value's right half steps it down / up like Left / Right.
    const DebugRow& r = rows_[size_t(i)];
    if (r.value && r.act && lx > w - 70) activate(lx > w - 35 ? 1 : -1);
    else activate(0);
    return true;
}

void DebugMenuGump::wheel(int dir) {
    int n = visible_rows();
    top_ = std::clamp(top_ + (dir > 0 ? -3 : 3), 0, std::max(0, int(rows_.size()) - n));
    if (sel_ < top_) sel_ = top_;
    if (sel_ >= top_ + n) sel_ = top_ + n - 1;
    if (sel_ >= 0 && sel_ < int(rows_.size()) && rows_[size_t(sel_)].header) move_sel(1);
}

bool DebugMenuGump::key(int k, unsigned mods) {
    switch (k) {
    case kKeyUp: move_sel(-1); break;
    case kKeyDown: move_sel(1); break;
    case kKeyPageUp: move_sel(-visible_rows()); break;
    case kKeyPageDown: move_sel(visible_rows()); break;
    case kKeyHome: sel_ = 0; move_sel(0); break;
    case kKeyEnd: sel_ = int(rows_.size()) - 1; keep_visible(); break;
    case kKeyLeft: activate(-1); break;
    case kKeyRight: activate(1); break;
    case kKeyReturn: case kKeySpace: activate(0); break;
    case kKeyEscape: case kKeyBackspace:
        if (k == kKeyBackspace && !digits_.empty()) digits_.pop_back();
        else back();
        break;
    case kKeyF10: close(); break;
    default:
        if (k >= '0' && k <= '9' && sel_ >= 0 && sel_ < int(rows_.size()) && rows_[size_t(sel_)].number && digits_.size() < 6)
            digits_ += char(k);
        else if (k == '-' && digits_.empty() && sel_ >= 0 && sel_ < int(rows_.size()) && rows_[size_t(sel_)].number)
            digits_ = "-";
        break;
    }
    (void)mods;
    return true;
}

void DebugMenuGump::tick() {
    if (!closed) rebuild();
}

void DebugMenuGump::paint(Surface8& s) {
    const ShapeFont& f = w_.font(kFont);
    // The frame: a dark edge, a light gold line, the brown body (like the game's wooden frames).
    s.fill_rect(x, y, w, h, col_dark_);
    s.fill_rect(x + 1, y + 1, w - 2, h - 2, col_edge_);
    s.fill_rect(x + 2, y + 2, w - 4, h - 4, col_dark_);
    s.fill_rect(x + 3, y + 3, w - 6, h - 6, col_bg_);
    s.fill_rect(x + 3, y + 3, w - 6, kTop - 5, col_title_);
    std::string head = pages_.back().title;
    if (pages_.size() > 1) head = "< " + head;
    f.draw_top(s, x + 7, y + 4, head);
    std::string help = pages_.size() > 1 ? "Esc back" : "F10, Esc: close";
    f.draw_top(s, x + w - 7 - f.text_width(help), y + 4, help);
    int adv = f.line_advance() + 1;
    int n = visible_rows();
    for (int i = top_; i < std::min(int(rows_.size()), top_ + n); ++i) {
        const DebugRow& r = rows_[size_t(i)];
        int ry = y + kTop + (i - top_) * adv;
        if (r.header) {
            s.fill_rect(x + 6, ry + adv - 2, w - 12, 1, col_edge_);
            f.draw_top(s, x + 7, ry, r.label);
            continue;
        }
        if (i == sel_) s.fill_rect(x + 5, ry - 1, w - 10, adv, col_hi_);
        f.draw_top(s, x + 10, ry, r.label);
        if (!r.shortcut.empty()) {
            // A long label pushes the shortcut to the right edge (rows without a value).
            int sx = x + 172;
            if (!r.value && x + 10 + f.text_width(r.label) + 6 > sx) sx = x + w - 9 - f.text_width(r.shortcut);
            f.draw_top(s, sx, ry, r.shortcut);
        }
        std::string v = r.value ? r.value() : "";
        if (i == sel_ && !digits_.empty()) v = digits_ + "_";
        if (!v.empty()) {
            if (r.act && r.value && i == sel_ && digits_.empty()) v = "< " + v + " >";
            f.draw_top(s, x + w - 9 - f.text_width(v), ry, v);
        }
    }
    // Scroll marks.
    if (top_ > 0) f.draw_top(s, x + w / 2, y + kTop - 7, "^");
    if (top_ + n < int(rows_.size())) f.draw_top(s, x + w / 2, y + h - kBottom - 4, "v");
    s.fill_rect(x + 6, y + h - kBottom, w - 12, 1, col_edge_);
    if (!status.empty()) f.draw_top(s, x + 7, y + h - kBottom + 2, status);
}

}  // namespace pg
