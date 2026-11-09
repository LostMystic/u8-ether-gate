// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// The spellbook window (game/spellbook.h, NOTES.md "Spellbook").
#include <algorithm>

#include "data/gamedata.h"
#include "data/shape.h"
#include "game/spellbook.h"
#include "game/world.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "platform/input.h"

namespace pg {

namespace {
constexpr unsigned kBookGump = 6;          // the book picture the readers use (gumps.md 2.11)
constexpr int kTitleFont = 9, kNameFont = 9, kInfoFont = 4;

uint8_t nearest(const Palette& p, int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 1; i < 255; ++i) {
        int dr = p.rgb8[size_t(i)][0] - r, dg = p.rgb8[size_t(i)][1] - g, db = p.rgb8[size_t(i)][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}
}  // namespace

SpellbookGump::SpellbookGump(World& w) : Gump(w) {
    shape = kBookGump;
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHand;
    const ShapeFrame* gf = gump_frame(shape, 0);
    this->w = gf ? gf->w : 280;
    h = gf ? gf->h : 140;
    x = (320 - this->w) / 2;
    y = (200 - h) / 2;
    const Palette& pal = w.data().palette;
    // Red text: each glyph colour tinted red at its own brightness (keeps the letters' shading).
    for (int i = 0; i < 256; ++i) {
        const int l = (pal.rgb8[size_t(i)][0] * 3 + pal.rgb8[size_t(i)][1] * 6 + pal.rgb8[size_t(i)][2]) / 10;
        red_[i] = uint8_t(i == ShapeFrame::kTransparent ? i : nearest(pal, std::min(255, l * 2 + 40), l / 3, l / 3));
    }
    ink_ = nearest(pal, 90, 60, 30);
    const ShapeFont& nf = w.font(kNameFont);
    const ShapeFont& inf = w.font(kInfoFont);
    int px, py, pw, ph;
    page_rect(0, px, py, pw, ph);
    const int body = ph - w.font(kTitleFont).line_advance() - 2 - inf.line_advance() - 1;
    const int entry_h = std::max(nf.line_advance() + 2 * inf.line_advance() + 1, 22);
    per_page = std::clamp(body / entry_h, 3, 4);
    build();
    // Open on the first page with something on it.
    for (size_t i = 0; i < pages.size(); ++i)
        if (!pages[i].spells.empty()) { spread = int(i / 2); selected = pages[i].spells.front()->key(); break; }
}

void SpellbookGump::page_rect(int side, int& px, int& py, int& pw, int& ph) const {
    px = side ? 150 : 9;
    py = 5;
    pw = 123;
    ph = std::max(60, h - 12);
}

void SpellbookGump::build() {
    Spellbook& sb = *w_.spellbook;
    const auto listed = sb.listed();
    pages.clear();
    for (int sc = 0; sc < Spellbook::kSchools; ++sc) {
        std::vector<const Spellbook::Spell*> v;
        for (const Spellbook::Spell* s : listed)
            if (s->school == sc) v.push_back(s);
        size_t n = 0;
        do {
            Page p;
            p.school = sc;
            for (size_t k = 0; k < size_t(per_page) && n < v.size(); ++k) p.spells.push_back(v[n++]);
            pages.push_back(std::move(p));
        } while (n < v.size());
    }
    spread = std::clamp(spread, 0, std::max(0, spreads() - 1));
    // Entry boxes of the spread on show.
    entries.clear();
    const ShapeFont& tf = w_.font(kTitleFont);
    for (int side = 0; side < 2; ++side) {
        const int pi = spread * 2 + side;
        if (pi >= int(pages.size())) break;
        int px, py, pw, ph;
        page_rect(side, px, py, pw, ph);
        const int top = py + tf.line_advance() + 2;
        const int body = ph - (top - py) - w_.font(kInfoFont).line_advance() - 1;
        const int eh = body / per_page;
        for (size_t k = 0; k < pages[size_t(pi)].spells.size(); ++k)
            entries.push_back({pages[size_t(pi)].spells[k], pi, px, top + int(k) * eh, pw, eh});
    }
}

std::string SpellbookGump::info_line(const Spellbook::Spell& s, const Spellbook::Status& st, bool& warn,
                                     std::string& line2) const {
    warn = !st.castable;
    line2.clear();
    if (w_.spellbook->debug_infinite) return "Unlimited (debug)";
    std::string l;
    const std::string mana = s.cost > 0 ? std::to_string(s.cost) + " mana" : "no mana";
    switch (s.school) {
    case Spellbook::kNecromancy:
        l = std::to_string(st.casts) + (st.casts == 1 ? " cast, " : " casts, ") + mana;
        break;
    case Spellbook::kTheurgy:
        l = st.focus ? "Focus, " + mana : "No focus";
        warn = !st.focus || !st.castable;
        break;
    case Spellbook::kSorcery:
        l = std::to_string(st.casts) + (st.casts == 1 ? " cast" : " casts");
        if (st.reagent_casts > 0) l += " (" + std::to_string(st.reagent_casts) + " by reagents)";
        break;
    case Spellbook::kThaumaturgy: l = std::to_string(st.casts) + " charged"; break;
    case Spellbook::kScrolls:
        l = std::to_string(st.casts) + (st.casts == 1 ? " scroll" : " scrolls");
        if (s.num == 1) line2 = "Not used up";
        break;
    default: break;
    }
    if (!st.castable) line2 = st.note == l ? std::string() : st.note;
    else if (st.mana_casts >= 0 && (st.casts < 0 || st.mana_casts < st.casts))
        line2 = "Mana for " + std::to_string(st.mana_casts);
    return l;
}

void SpellbookGump::paint(Surface8& s) {
    if (gump_frame(shape, 0)) Gump::paint(s);
    else s.fill_rect(x, y, w, h, ink_);
    Spellbook& sb = *w_.spellbook;
    const ShapeFont& tf = w_.font(kTitleFont);
    const ShapeFont& nf = w_.font(kNameFont);
    const ShapeFont& inf = w_.font(kInfoFont);
    for (int side = 0; side < 2; ++side) {
        const int pi = spread * 2 + side;
        int px, py, pw, ph;
        page_rect(side, px, py, pw, ph);
        if (pi < int(pages.size())) {
            const Page& p = pages[size_t(pi)];
            std::string title = Spellbook::school_name(p.school);
            tf.draw_top(s, x + px + (pw - tf.text_width(title)) / 2, y + py, title);
            if (p.spells.empty()) {
                const char* note = p.school == Spellbook::kTempest   ? "The water spells are yet to come."
                                   : p.school == Spellbook::kScrolls ? "No scrolls carried."
                                                                     : "No spells written here yet.";
                const int ty = py + tf.line_advance() + 12;
                inf.draw_top(s, x + px + (pw - inf.text_width(note)) / 2, y + ty, note);
            }
        }
        // Footer: the mana on the left page, messages and the page number on the right.
        const int fy = py + ph - inf.line_advance();
        if (side == 0) {
            const Actor* av = w_.avatar();
            std::string m = av ? "Mana " + std::to_string(av->mana) + "/" + std::to_string(2 * std::max<int>(0, av->intel)) : "";
            if (spread > 0) m = "< " + m;
            inf.draw_top(s, x + px, y + fy, m);
        } else {
            std::string m = message_frames > 0 ? message
                          : sb.debug_infinite ? "Debug: infinite spells"
                                              : "Double-click to cast";
            std::string pgno = std::to_string(spread + 1) + "/" + std::to_string(std::max(1, spreads()));
            if (spread + 1 < spreads()) pgno += " >";
            const int pnw = inf.text_width(pgno);
            inf.draw_top(s, x + px + pw - pnw, y + fy, pgno);
            const bool red = message_frames > 0;
            while (!m.empty() && inf.text_width(m) > pw - pnw - 4) m.pop_back();
            inf.draw_top(s, x + px, y + fy, m, red ? red_ : nullptr);
        }
    }
    for (const Entry& e : entries) {
        const Spellbook::Spell& sp = *e.spell;
        const Spellbook::Status st = sb.status(sp);
        if (sp.key() == selected) {
            s.fill_rect(x + e.x - 2, y + e.y, e.w + 3, 1, ink_);
            s.fill_rect(x + e.x - 2, y + e.y + e.h - 2, e.w + 3, 1, ink_);
        }
        if (const ShapeFrame* f = w_.data().shapes.frame(sp.icon_shape, sp.icon_frame)) {
            // The icon centred in a 22-pixel box, its picture clipped to the entry.
            const int bx = x + e.x + 11 - f->w / 2 + f->xoff, by = y + e.y + std::max(1, (e.h - f->h) / 2) + f->yoff;
            s.set_clip(x + e.x, y + e.y + 1, 22, e.h - 3);
            s.draw(*f, bx, by);
            s.reset_clip();
        }
        const int tx = x + e.x + 24;
        int ty = y + e.y + 1;
        std::string name = sp.name;
        nf.draw_top(s, tx, ty, name);
        ty += nf.line_advance();
        bool warn = false;
        std::string line2;
        std::string l1 = info_line(sp, st, warn, line2);
        inf.draw_top(s, tx, ty, l1, warn && line2.empty() ? red_ : nullptr);
        if (!line2.empty()) inf.draw_top(s, tx, ty + inf.line_advance(), line2, warn || st.mana_casts == 0 ? red_ : nullptr);
    }
}

bool SpellbookGump::arrow_hit(int which, int lx, int ly) const {
    int px, py, pw, ph;
    page_rect(which, px, py, pw, ph);
    const int fy = py + ph - w_.font(kInfoFont).line_advance() - 2;
    if (ly < fy || ly > py + ph + 4) return false;
    return which == 0 ? (lx >= px - 4 && lx < px + 14) : (lx >= px + pw - 14 && lx <= px + pw + 4);
}

bool SpellbookGump::turn(int dir) {
    const int to = spread + dir;
    if (to < 0 || to >= spreads()) return false;
    spread = to;
    build();
    selected = entries.empty() ? -1 : entries.front().spell->key();
    return true;
}

bool SpellbookGump::press(int button, int lx, int ly) {
    if (button == 3) { close(); return true; }
    if (button != 1) return true;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) { close(); return true; }
    if (arrow_hit(0, lx, ly) && turn(-1)) return true;
    if (arrow_hit(1, lx, ly) && turn(1)) return true;
    for (const Entry& e : entries)
        if (lx >= e.x && lx < e.x + e.w && ly >= e.y && ly < e.y + e.h) { selected = e.spell->key(); return true; }
    return true;
}

void SpellbookGump::double_click(int button, int lx, int ly) {
    if (button != 1) return;
    for (const Entry& e : entries)
        if (lx >= e.x && lx < e.x + e.w && ly >= e.y && ly < e.y + e.h) {
            selected = e.spell->key();
            cast_selected();
            return;
        }
}

bool SpellbookGump::cast_selected() {
    const Spellbook::Spell* s = Spellbook::find(selected);
    if (!s) return false;
    // The book closes first, so a target cursor or a scene can follow.
    std::string why;
    if (!w_.spellbook->cast(*s, why)) {
        message = why;
        message_frames = 90;
        return false;
    }
    close();
    return true;
}

void SpellbookGump::select_step(int d) {
    if (entries.empty()) return;
    int i = 0;
    for (size_t k = 0; k < entries.size(); ++k)
        if (entries[k].spell->key() == selected) i = int(k);
    const int to = i + d;
    if (to < 0) { if (turn(-1) && !entries.empty()) selected = entries.back().spell->key(); return; }
    if (to >= int(entries.size())) { turn(1); return; }
    selected = entries[size_t(to)].spell->key();
}

bool SpellbookGump::key(int key, unsigned mods) {
    (void)mods;
    if (key == kKeyEscape || key == 'g') close();
    else if (key == kKeyLeft || key == kKeyPageUp) turn(-1);
    else if (key == kKeyRight || key == kKeyPageDown) turn(1);
    else if (key == kKeyUp) select_step(-1);
    else if (key == kKeyDown) select_step(1);
    else if (key == kKeyReturn || key == kKeySpace) cast_selected();
    return true;
}

void SpellbookGump::tick() {
    ++frames_;
    if (message_frames > 0) --message_frames;
    build();
    if (w_.auto_ui && frames_ > 60) close();
}

}  // namespace pg
