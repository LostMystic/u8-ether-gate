// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Virtue popups (game/virtues.h; NOTES.md "Virtue popups"): each gain shows as "Humility +4" over
// the Avatar's head once the moment is calm (the end of the conversation that paid it, usually),
// rising slowly for 2.8 s and dithering away over its last 1.2 s. The engine's own design.
#include <algorithm>
#include <cstdio>

#include "data/gamedata.h"
#include "game/gumps.h"
#include "game/leveling.h"
#include "game/movie.h"
#include "game/virtues.h"
#include "game/world.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "kernel/kernel.h"

namespace pg {

std::string Virtues::popup_text(const Popup& p) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s +%d", name(p.virtue), p.points);
    return buf;
}

bool Virtues::popup_moment() {
    if (!w_) return false;
    World& w = *w_;
    const Actor* av = w.avatar();
    if (!av || av->chunk < 0 || av->is(Actor::kDead) || w.game_over || w.stasis || w.in_conversation()) return false;
    if (w.gumps.top_modal() || w.gumps.find<ReaderGump>() || w.gumps.find<MovieGump>()) return false;
    if (w.fade_pid) {
        const Process* p = w.kernel().get(w.fade_pid);
        if (p && p->live()) return false;
    }
    for (const Bark& b : w.barks)  // the Avatar saying something
        if (b.speaker == 1) return false;
    if (w.leveling)  // "Level up" / "Dodged" over his head first
        for (const Leveling::FloatText& f : w.leveling->floats)
            if (f.actor == 1) return false;
    return true;
}

void Virtues::popup_frame() {
    if (!w_ || w_->kernel().paused()) return;
    for (Popup& p : popups) ++p.age;
    popups.erase(std::remove_if(popups.begin(), popups.end(), [](const Popup& p) { return p.age >= kPopupFrames; }), popups.end());
    if (!enabled) {
        popups_waiting.clear();
        popups.clear();
    }
    if (popups_waiting.empty() || !popup_moment()) {
        popup_calm_ = 0;
        return;
    }
    if (popup_calm_ < kPopupCalm) {
        ++popup_calm_;
        return;
    }
    if (!popups.empty() && popups.back().age < kPopupGap) return;
    popups.push_back(popups_waiting.front());
    popups_waiting.erase(popups_waiting.begin());
}

namespace {

struct Rgb { int r, g, b; };
// The octagon's gem colours (game/sheet_extras.cpp), a little lighter so the text reads.
const Rgb kText[kVirtueCount] = {
    {120, 160, 255},  // Honesty: blue
    {255, 250, 90},   // Compassion: yellow
    {255, 90, 70},    // Valor: red
    {90, 225, 110},   // Justice: green
    {255, 160, 60},   // Sacrifice: orange
    {200, 120, 250},  // Honor: purple
    {250, 250, 250},  // Spirituality: white
    {200, 208, 225},  // Humility: silver
};

uint8_t nearest(const GameData& d, Rgb c) {
    const Palette& p = d.palette;
    int best = 16, bd = 1 << 30;
    for (int i = 16; i < 255; ++i) {  // not the transparency colours (8..14)
        const int dr = p.rgb8[size_t(i)][0] - c.r, dg = p.rgb8[size_t(i)][1] - c.g, db = p.rgb8[size_t(i)][2] - c.b;
        const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}

constexpr int kFont = 0;   // the barks' font
constexpr int kRise = 30;  // pixels risen over the whole life
constexpr int kFadeFrames = 36;
const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
// The plus, in the font's style (ink with a black edge), level with its digits.
constexpr int kPlusW = 7, kPlusTop = 2;
const char* const kPlus[7] = {"..ooo..", "..o#o..", "ooo#ooo", "o#####o", "ooo#ooo", "..o#o..", "..ooo.."};

struct Tints {
    bool built = false;
    uint8_t outline = 0;
    uint8_t main[kVirtueCount] = {};
    uint8_t remap[kVirtueCount][256];
};

const Tints& tints(const World& w) {
    static Tints t;
    if (t.built) return t;
    t.built = true;
    const GameData& d = w.data();
    const Palette& pal = d.palette;
    t.outline = 0;  // the font's own black edge
    auto lum = [&](int i) { return pal.rgb8[size_t(i)][0] * 3 + pal.rgb8[size_t(i)][1] * 6 + pal.rgb8[size_t(i)][2]; };
    // The font's own ink: its brighter pixels take the colour, the darker ones a shade of it, the
    // darkest (its shadow, if any) the outline.
    std::vector<int> used;
    int lo = 1 << 30, hi = 0;
    if (const Shape* sh = d.fonts.shape(kFont))
        for (unsigned i = 0; i < sh->frame_count(); ++i)
            if (const ShapeFrame* fr = sh->frame(i))
                for (uint8_t c : fr->pixels)
                    if (c != ShapeFrame::kTransparent && std::find(used.begin(), used.end(), c) == used.end()) {
                        used.push_back(c);
                        lo = std::min(lo, lum(c));
                        hi = std::max(hi, lum(c));
                    }
    for (int v = 0; v < kVirtueCount; ++v) {
        const Rgb c = kText[v];
        const uint8_t main = nearest(d, c);
        t.main[v] = main;
        const uint8_t shade = nearest(d, {c.r * 7 / 10, c.g * 7 / 10, c.b * 7 / 10});
        for (int i = 0; i < 256; ++i) t.remap[v][i] = uint8_t(i);
        for (int i : used) {
            const int l = hi > lo ? (lum(i) - lo) * 100 / (hi - lo) : 100;
            t.remap[v][i] = l >= 60 ? main : l >= 25 ? shade : t.outline;
        }
    }
    return t;
}

}  // namespace

void draw_virtue_popups(World& w, Surface8& s) {
    if (!w.virtues || w.virtues->popups.empty()) return;
    const Actor* av = w.avatar();
    if (!av || av->chunk < 0) return;
    const ShapeFont& f = w.font(kFont);
    if (!f.valid()) return;
    const Tints& t = tints(w);
    int ax, ay;
    w.screen_point(*av, ax, ay);
    ay -= int(w.data().typeflags.get(av->shape).z()) * 8 + 16;  // over the head, as the barks
    static Surface8 scratch(160, 24);
    for (const Virtues::Popup& p : w.virtues->popups) {
        if (p.virtue < 0 || p.virtue >= kVirtueCount) continue;
        // Font 0 has no '+' (an empty glyph): "Name ", a drawn plus, the number.
        const std::string head = std::string(Virtues::name(p.virtue)) + " ", num = std::to_string(p.points);
        const int tw = f.text_width(head) + kPlusW + f.text_width(num), th = f.height();
        if (scratch.width() < tw || scratch.height() < th) scratch.resize(std::max(tw, scratch.width()), std::max(th, scratch.height()));
        scratch.reset_clip();
        scratch.clear(ShapeFrame::kTransparent);
        const uint8_t* remap = t.remap[p.virtue];
        int pen = f.draw_top(scratch, 0, 0, head, remap);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                if (kPlus[y][x] != '.') scratch.fill_rect(pen + x, kPlusTop + y, 1, 1, kPlus[y][x] == '#' ? t.main[p.virtue] : t.outline);
        f.draw_top(scratch, pen + kPlusW, 0, num, remap);
        const int left = std::clamp(ax - tw / 2, 0, std::max(0, 320 - tw));
        const int top = std::clamp(ay - th - p.age * kRise / Virtues::kPopupFrames, 0, std::max(0, 200 - th));
        // Dithered fade: an ever larger share of the pixels left out (a 4x4 ordered pattern).
        const int left_frames = Virtues::kPopupFrames - p.age;
        const int keep = left_frames >= kFadeFrames ? 16 : 16 * left_frames / kFadeFrames;
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const int gx = left + x, gy = top + y;
                const uint8_t c = scratch.row(y)[x];
                if (c == ShapeFrame::kTransparent || kBayer[gy & 3][gx & 3] >= keep) continue;
                s.fill_rect(gx, gy, 1, 1, c);  // fill_rect: clipped, and marks the UI layer's mask
            }
    }
}

}  // namespace pg
