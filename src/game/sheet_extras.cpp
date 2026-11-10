// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The character sheet's engine extras (PLAN-VIRTUES.md 2; NOTES.md "Virtues"): the level line with
// its XP bar under the stats button on everyone's sheet, and the virtue octagon in the bottom-left
// corner of the Avatar's. Both are drawn by the paperdoll (game/gumps.cpp) after its own picture;
// a left click on either tells its value in a small note on the sheet (and in the message log).
#include <algorithm>
#include <cmath>

#include "data/gamedata.h"
#include "game/game_log.h"
#include "game/gumps.h"
#include "game/leveling.h"
#include "game/virtues.h"
#include "game/world.h"
#include "gfx/surface.h"

namespace pg {

namespace {

// The stats button (gump shape 38) sits at 81,84 and is 70 x 15; the level line's plaque goes
// directly under it, over the sheet's bottom rope frame (the sheet keeps its size, 168 x 113).
constexpr int kLineX = 83, kLineY = 100, kLineW = 66, kLineH = 8;
constexpr int kLineFont = 3;  // the small capitals (5 pixels high)
constexpr int kBarW = 26, kBarH = 4;
constexpr int kBarX = kLineX + kLineW - 3 - kBarW, kBarY = kLineY + (kLineH - kBarH) / 2;
constexpr int kBarSlack = 2;  // a click this close to the bar counts

// The octagon: the bottom-left corner of the figure's panel (inner area about 19..72 x 14..98).
constexpr int kOctX = 30, kOctY = 86, kOctR = 9;
constexpr int kGemHit = 3;  // a click within this many pixels of a gem's middle
constexpr int kGlowFromPct = 80;  // a point gem glows from this share of its maximum
constexpr int kNoteTicks = 180;

struct Rgb { int r, g, b; };
// Bright (full) colours, virtues.h order, then the centre gem.
const Rgb kBright[9] = {
    {70, 120, 255},   // Honesty: blue
    {255, 225, 50},   // Compassion: yellow
    {240, 40, 30},    // Valor: red
    {40, 205, 70},    // Justice: green
    {255, 135, 25},   // Sacrifice: orange
    {175, 70, 235},   // Honor: purple
    {250, 250, 250},  // Spirituality: white
    {205, 210, 222},  // Humility: silver
    {255, 214, 120},  // the centre: the Avatar's pale gold
};
const Rgb kHumilityDim = {44, 44, 48};  // "black": a dark grey that turns silver

Rgb mix(Rgb a, Rgb b, int pct) {
    return {a.r + (b.r - a.r) * pct / 100, a.g + (b.g - a.g) * pct / 100, a.b + (b.b - a.b) * pct / 100};
}
Rgb dim_of(int gem) {
    if (gem == kHumility) return kHumilityDim;
    const Rgb& c = kBright[gem];
    return {c.r * 28 / 100, c.g * 28 / 100, c.b * 28 / 100};
}

uint8_t nearest(const Palette& p, Rgb c) {
    int best = 1, bd = 1 << 30;
    for (int i = 1; i < 255; ++i) {
        int dr = p.rgb8[size_t(i)][0] - c.r, dg = p.rgb8[size_t(i)][1] - c.g, db = p.rgb8[size_t(i)][2] - c.b;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}

const std::vector<uint8_t>& blend_table(const World& w) {
    static std::vector<uint8_t> t;
    if (t.empty()) t = build_ghost_table(w.data().palette);
    return t;
}

// Blends one pixel 50% toward colour c (twice: 25% more of the background stays... 75% toward c).
void blend_px(Surface8& s, int x, int y, uint8_t c, const std::vector<uint8_t>& t, int times = 1) {
    if (x < 0 || y < 0 || x >= s.width() || y >= s.height()) return;
    uint8_t& p = s.row(y)[x];
    for (int k = 0; k < times; ++k) p = t[size_t(p) * 256 + c];
}
void put_px(Surface8& s, int x, int y, uint8_t c) {
    if (x < 0 || y < 0 || x >= s.width() || y >= s.height()) return;
    s.row(y)[x] = c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The level line

bool PaperdollGump::level_line_shown() const { return w_.leveling && w_.leveling->active() && w_.actor(owner); }

bool PaperdollGump::xp_bar_rect(int& bx, int& by, int& bw, int& bh) const {
    if (!level_line_shown()) return false;
    bx = kBarX; by = kBarY; bw = kBarW; bh = kBarH;
    return true;
}

int PaperdollGump::xp_bar_fill(int width) const {
    if (!w_.leveling) return 0;
    const int lv = w_.leveling->level_of(owner), xp = w_.leveling->xp_of(owner);
    if (lv >= kMaxLevel) return width;  // the top level: full
    const int lo = kLevelXp[std::max(1, lv) - 1], hi = kLevelXp[std::max(1, lv)];
    if (hi <= lo) return width;
    return std::clamp(int((long long)(xp - lo) * width / (hi - lo)), 0, width);
}

void PaperdollGump::paint_level_line(Surface8& s) {
    if (!level_line_shown()) return;
    const Palette& pal = w_.data().palette;
    static uint8_t dark = 0, edge = 0, hole = 0, fill = 0, fill_hi = 0;
    if (!dark) {
        dark = nearest(pal, {22, 18, 14});
        edge = nearest(pal, {92, 88, 84});
        hole = nearest(pal, {52, 46, 40});
        fill = nearest(pal, {214, 168, 40});
        fill_hi = nearest(pal, {255, 226, 120});
    }
    // The plaque: a dark inset with a light lower edge, like the panels above it.
    s.fill_rect(x + kLineX, y + kLineY, kLineW, kLineH, edge);
    s.fill_rect(x + kLineX, y + kLineY, kLineW - 1, kLineH - 1, dark);
    const ShapeFont& f = w_.font(kLineFont);
    f.draw_top(s, x + kLineX + 3, y + kLineY + (kLineH - 1 - f.height()) / 2 + 1, "Level " + std::to_string(w_.leveling->level_of(owner)));
    // The XP bar: a sunken trough, gold as far as this level's XP goes. No number on it.
    s.fill_rect(x + kBarX, y + kBarY, kBarW, kBarH, hole);
    const int n = xp_bar_fill(kBarW - 2);
    if (n > 0) {
        s.fill_rect(x + kBarX + 1, y + kBarY + 1, n, kBarH - 2, fill);
        s.fill_rect(x + kBarX + 1, y + kBarY + 1, n, 1, fill_hi);
    }
}

// ---------------------------------------------------------------------------------------------
// The virtue octagon

bool PaperdollGump::octagon_shown() const { return owner == 1 && w_.virtues && w_.virtues->enabled; }

void PaperdollGump::gem_centre(int gem, int& cx, int& cy) {
    if (gem < 0 || gem >= kVirtueCount) { cx = kOctX; cy = kOctY; return; }
    // Clockwise from the top: Honesty at the top, Compassion top-right, ... Humility top-left.
    const double a = (-90.0 + 45.0 * gem) * 3.14159265358979 / 180.0;
    cx = kOctX + int(std::lround(kOctR * std::cos(a)));
    cy = kOctY + int(std::lround(kOctR * std::sin(a)));
}

int PaperdollGump::gem_at(int lx, int ly) const {
    if (!octagon_shown()) return -1;
    int best = -1, bd = 1 << 30;
    for (int g = 0; g <= kVirtueCount; ++g) {
        int cx, cy;
        gem_centre(g, cx, cy);
        // Middles of a 4-pixel gem are between pixels: cx-2..cx+1.
        const int dx = lx - cx, dy = ly - cy;
        if (dx < -kGemHit - 1 || dx > kGemHit || dy < -kGemHit - 1 || dy > kGemHit) continue;
        const int d = (2 * dx + 1) * (2 * dx + 1) + (2 * dy + 1) * (2 * dy + 1);
        if (d < bd) { bd = d; best = g; }
    }
    return best;
}

void PaperdollGump::paint_octagon(Surface8& s) {
    if (!octagon_shown()) return;
    const Virtues& v = *w_.virtues;
    const Palette& pal = w_.data().palette;
    const std::vector<uint8_t>& t = blend_table(w_);
    static uint8_t black = 0, outline = 0;
    if (!black) { black = nearest(pal, {0, 0, 0}); outline = nearest(pal, {14, 10, 8}); }
    // A soft dark socket under the gems, so dim gems read on the light panel and glows show.
    for (int dy = -kOctR - 4; dy <= kOctR + 3; ++dy)
        for (int dx = -kOctR - 4; dx <= kOctR + 3; ++dx) {
            const double fx = dx + 0.5, fy = dy + 0.5;
            const double r = std::max(std::fabs(fx), std::fabs(fy)) * 0.6 + (std::fabs(fx) + std::fabs(fy)) * 0.4;  // an octagon-ish round
            if (r > kOctR + 3.1) continue;
            blend_px(s, x + kOctX + dx, y + kOctY + dy, black, t, r > kOctR + 2.1 ? 1 : 2);
        }
    // Pulse: 0..100 over about 1.3 s.
    const int pulse = int(50 + 50 * std::sin(double(anim_) * 2 * 3.14159265358979 / 80.0));
    struct G { int cx, cy, pct, glow; bool centre; };
    G gems[kVirtueCount + 1];
    for (int g = 0; g <= kVirtueCount; ++g) {
        G& e = gems[g];
        gem_centre(g, e.cx, e.cy);
        e.centre = g == kVirtueCount;
        const int mx = std::max(1, e.centre ? v.total_max() : v.max_score);
        const int val = e.centre ? v.total() : v.get(g);
        e.pct = std::clamp(val * 100 / mx, 0, 100);
        // Glow 0..100: the points from kGlowFromPct of their maximum, the centre only when all are full.
        if (e.centre) e.glow = v.all_max() ? 100 : 0;
        else e.glow = e.pct >= kGlowFromPct ? (e.pct - kGlowFromPct) * 100 / (100 - kGlowFromPct) : 0;
        e.glow = std::max(e.glow, e.glow > 0 ? 25 : 0);
    }
    // Halos first (they may spread over a neighbour's halo, never over its gem).
    for (int g = 0; g <= kVirtueCount; ++g) {
        const G& e = gems[g];
        if (!e.glow) continue;
        const uint8_t hc = nearest(pal, mix(kBright[g], {255, 255, 255}, 40));
        const int strength = e.glow * (60 + 40 * pulse / 100) / 100;  // 15..100
        const int half = e.centre ? 3 : 2;  // the gem spans cx-half..cx+half-1 (+1 for the centre)
        const int lo = -half - 2, hi = half + 1 + (e.centre ? 1 : 0);
        for (int dy = lo; dy <= hi; ++dy)
            for (int dx = lo; dx <= hi; ++dx) {
                const bool corner = (dx == lo || dx == hi) && (dy == lo || dy == hi);
                if (corner) continue;
                const bool outer = dx == lo || dx == hi || dy == lo || dy == hi;
                if (outer && strength < 45) continue;
                blend_px(s, x + e.cx + dx, y + e.cy + dy, hc, t, outer ? (strength >= 85 ? 2 : 1) : (strength >= 60 ? 3 : 2));
            }
    }
    // The gems: a dark rim, the body from dim to bright with the score, a highlight and a shade.
    for (int g = 0; g <= kVirtueCount; ++g) {
        const G& e = gems[g];
        Rgb body = mix(dim_of(g), kBright[g], e.pct);
        if (e.glow) body = mix(body, {255, 255, 255}, e.glow * pulse / 100 / 5);  // the pulse lifts it a little
        const uint8_t cb = nearest(pal, body);
        const uint8_t chl = nearest(pal, mix(body, {255, 255, 255}, e.pct >= 10 ? 55 : 25));
        const uint8_t csh = nearest(pal, mix(body, {0, 0, 0}, 35));
        const int n = e.centre ? 5 : 4;      // gem size
        const int x0 = x + e.cx - 2, y0 = y + e.cy - 2;
        for (int dy = -1; dy <= n; ++dy)
            for (int dx = -1; dx <= n; ++dx) {
                const bool rim = dx < 0 || dy < 0 || dx >= n || dy >= n;
                const bool cut = (dx <= 0 || dx >= n - 1) && (dy <= 0 || dy >= n - 1);  // rounded corners
                if (rim) {
                    if ((dx < 0 || dx >= n) && (dy < 0 || dy >= n)) continue;  // the rim's corners
                    put_px(s, x0 + dx, y0 + dy, outline);
                    continue;
                }
                if (cut && !((dx == 0 || dx == n - 1) && (dy == 0 || dy == n - 1))) {
                    put_px(s, x0 + dx, y0 + dy, cb);
                    continue;
                }
                if (cut) { put_px(s, x0 + dx, y0 + dy, outline); continue; }
                uint8_t c = cb;
                if (dx == 1 && dy == 1) c = chl;
                else if (dx + dy >= 2 * (n - 1) - 1) c = csh;
                put_px(s, x0 + dx, y0 + dy, c);
            }
    }
}

// ---------------------------------------------------------------------------------------------
// Clicks and the note

std::string PaperdollGump::click_message(int lx, int ly) const {
    int bx, by, bw, bh;
    if (xp_bar_rect(bx, by, bw, bh) && lx >= bx - kBarSlack && ly >= by - kBarSlack && lx < bx + bw + kBarSlack &&
        ly < by + bh + kBarSlack) {
        const int lv = w_.leveling->level_of(owner), xp = w_.leveling->xp_of(owner);
        std::string m = "XP " + std::to_string(xp);
        if (lv >= kMaxLevel) return m + ", the top level";
        return m + ", " + std::to_string(std::max(0, kLevelXp[lv] - xp)) + " to level " + std::to_string(lv + 1);
    }
    const int g = gem_at(lx, ly);
    if (g >= 0 && g < kVirtueCount) return std::string(Virtues::name(g)) + " " + std::to_string(w_.virtues->get(g));
    return std::string();
}

void PaperdollGump::click(int button, int lx, int ly) {
    if (button == 1) {
        const int mx = map_x(lx);  // the narrow sheet's place (a portrait sheet is wider)
        const std::string m = click_message(mx, ly);
        if (!m.empty()) {
            note = m;
            note_ticks = kNoteTicks;
            note_x = mx;
            note_y = ly;
            w_.game_log.add(GameLog::kMessages, GameLog::kInfo, m, owner);
            return;
        }
        if (gem_at(mx, ly) >= 0) return;  // the centre gem: nothing
    }
    ContainerGump::click(button, lx, ly);  // trace_item maps again
}

void PaperdollGump::tick() {
    ContainerGump::tick();
    ++anim_;
    if (note_ticks > 0 && --note_ticks == 0) note.clear();
}

void PaperdollGump::paint_note(Surface8& s) {
    if (note.empty()) return;
    static uint8_t bg = 0, edge = 0;
    if (!bg) { bg = nearest(w_.data().palette, {28, 24, 20}); edge = nearest(w_.data().palette, {150, 140, 120}); }
    const ShapeFont& f = w_.font(0);
    const int tw = f.text_width(note), bw = tw + 8, bh = f.height() + 3;
    const int bx = std::clamp(note_x - bw / 2, 2 - widen(), std::max(2, w - widen() - 2 - bw));  // x is shifted by widen() here
    const int by = std::clamp(note_y - bh - 5, 2, std::max(2, h - 2 - bh));
    s.fill_rect(x + bx, y + by, bw, bh, edge);
    s.fill_rect(x + bx + 1, y + by + 1, bw - 2, bh - 2, bg);
    f.draw_top(s, x + bx + 4, y + by + 2, note);
}

}  // namespace pg
