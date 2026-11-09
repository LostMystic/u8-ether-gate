// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The HUD side panel: see hud_panel.h.
#include "app/hud_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/game_mode.h"
#include "app/hud_icons.h"
#include "app/turn_ui.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/creatures.h"
#include "game/companion_magic.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "gfx/text_layout.h"
#include "platform/input.h"

namespace pg {

namespace {

constexpr int kFont = 1;   // the game's small black font (9 high, about 4 wide), drawn tinted
constexpr int kLineH = 8;  // its line pitch in the logs

// Tints of the panel text (remaps of font 1's ink).
enum Tint { kTDefault, kTPale, kTGold, kTGreen, kTWhite, kTGrey, kTRed, kTYellow, kTints };

int kind_tint(GameLog::Kind k) {
    switch (k) {
    case GameLog::kLook: return kTPale;
    case GameLog::kSpeech: return kTDefault;
    case GameLog::kAnswer: return kTGold;
    case GameLog::kInfo: return kTGreen;
    case GameLog::kHit: return kTWhite;
    case GameLog::kSpell: return kTYellow;
    case GameLog::kMiss: case GameLog::kBlock: return kTGrey;
    case GameLog::kHeal: return kTGreen;
    case GameLog::kDeath: return kTRed;
    case GameLog::kTurn: return kTPale;
    }
    return kTDefault;
}

void put(Surface8& s, int x, int y, uint8_t c) {
    if (x >= 0 && y >= 0 && x < s.width() && y < s.height()) s.row(y)[x] = c;
}

void frame_rect(Surface8& s, int x, int y, int w, int h, uint8_t c) {
    s.fill_rect(x, y, w, 1, c);
    s.fill_rect(x, y + h - 1, w, 1, c);
    s.fill_rect(x, y, 1, h, c);
    s.fill_rect(x + w - 1, y, 1, h, c);
}

const char* const kPartyAutoHelp = "Everyone in the party acts by himself; you watch. Esc, B or the Auto mode button turn it off.";

}  // namespace

HudPanel::HudPanel(GameMode& gm) : gm_(gm) {
    messages_.ch = GameLog::kMessages;
    combat_.ch = GameLog::kCombat;
}

uint8_t HudPanel::nearest(int r, int g, int b) const {
    const GameData& d = gm_.world().data();
    int best = 0, bd = 1 << 30;
    for (int i = 16; i < 256; ++i) {
        if (d.xform.loaded && d.xform.is_xform(uint8_t(i))) continue;
        int dr = d.palette.rgb8[size_t(i)][0] - r, dg = d.palette.rgb8[size_t(i)][1] - g, db = d.palette.rgb8[size_t(i)][2] - b;
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; }
    }
    return uint8_t(best);
}

void HudPanel::ensure_colours() {
    if (have_colours_) return;
    have_colours_ = true;
    c_bg_ = nearest(46, 40, 34);       // the panel's stone
    c_box_ = nearest(18, 16, 14);      // log boxes
    c_edge_ = nearest(150, 140, 120);  // frames (as the party bar's)
    c_light_ = nearest(190, 180, 150);
    c_dark_ = nearest(8, 8, 8);
    c_hl_ = nearest(255, 230, 60);
    c_band_ = nearest(92, 72, 40);
    c_hp_ = 39;                        // the party bar's HP red
    c_mana_ = 139;
    c_foe_ = nearest(120, 30, 20);
    c_grey_ = nearest(110, 110, 110);
    c_btn_ = nearest(70, 62, 52);
    c_btn_on_ = nearest(150, 120, 50);
    c_tip_ = nearest(222, 206, 160);
    std::vector<int> used;
    const ShapeFont& f = gm_.world().font(kFont);
    (void)f;
    const GameData& d = gm_.world().data();
    if (const Shape* sh = d.fonts.shape(kFont))
        for (unsigned i = 0; i < sh->frame_count(); ++i)
            if (const ShapeFrame* fr = sh->frame(i))
                for (uint8_t c : fr->pixels)
                    if (c != ShapeFrame::kTransparent && std::find(used.begin(), used.end(), c) == used.end()) used.push_back(c);
    auto lum = [&](int i) { return d.palette.rgb8[size_t(i)][0] * 3 + d.palette.rgb8[size_t(i)][1] * 6 + d.palette.rgb8[size_t(i)][2]; };
    static const int kRGB[kTints][3] = {{225, 210, 170}, {170, 195, 225}, {235, 190, 80}, {130, 215, 120},
                                        {245, 245, 240}, {140, 140, 140}, {235, 85, 60}, {255, 235, 70}};
    for (int t = 0; t < kTints; ++t) {
        for (int i = 0; i < 256; ++i) tints_[t][i] = uint8_t(i);
        uint8_t main = nearest(kRGB[t][0], kRGB[t][1], kRGB[t][2]);
        uint8_t shade = nearest(kRGB[t][0] * 2 / 3, kRGB[t][1] * 2 / 3, kRGB[t][2] * 2 / 3);
        // Font 1 is plain dark ink (made for parchment): on the panel's dark boxes the ink takes
        // the tint (any lighter edge pixels its shade).
        for (int c : used) tints_[t][c] = lum(c) < 300 ? main : shade;
    }
    static const struct { char ch; int r, g, b; } kIconCols[] = {
        {'#', 16, 12, 8}, {'w', 235, 235, 225}, {'s', 160, 165, 175}, {'S', 90, 95, 105}, {'y', 235, 195, 60},
        {'n', 140, 90, 45}, {'r', 205, 40, 30}, {'g', 70, 195, 70}, {'b', 60, 95, 205}, {'c', 140, 210, 245},
        {'p', 230, 185, 145}};
    for (const auto& k : kIconCols) icon_cols_[k.ch] = nearest(k.r, k.g, k.b);
}

const uint8_t* HudPanel::tint(int which) { return tints_[std::clamp(which, 0, kTints - 1)]; }

bool HudPanel::combat_view() const {
    World& w = gm_.world();
    if (w.turn && w.turn->active()) return true;
    const Actor* av = w.avatar();
    return av && !av->is(Actor::kDead) && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
}

bool HudPanel::turn_buttons() const {
    World& w = gm_.world();
    return w.turn && w.turn->enabled && w.turn->active();
}

std::vector<uint16_t> HudPanel::party_ids() const {
    World& w = gm_.world();
    if (w.party && w.party->enabled) return w.party->bar_ids();
    return {1};
}

int HudPanel::max_hp_of(uint16_t id) {
    World& w = gm_.world();
    const Actor* a = w.actor(id);
    if (!a) return 1;
    if (id == 1) return std::max(1, 2 * int(a->str));
    if (w.party && w.party->is_companion(id)) return std::max(1, w.party->max_hp(*a));
    int& m = foe_max_[id];
    m = std::max(m, int(a->hp));
    return std::max(1, m);
}

// ---------------------------------------------------------------------------------------------
// Layout

void HudPanel::layout(int pw) {
    pw_ = pw;
    portraits_.clear();
    buttons_.clear();
    roster_ = monster_r_ = title_r_ = Rect{};
    roster_rows_ = 0;
    std::vector<uint16_t> ids = party_ids();
    const int n = int(ids.size());
    if (!combat_view()) {
        // One row of portraits, shrunk to fit: cells at most 26 wide, as the party bar's.
        const int gap = 2;
        int cw = std::clamp((pw - 4 - gap * (n - 1)) / std::max(1, n), 6, 26);
        int ph = (cw - 2) * 25 / 24;
        int chh = ph + 2 + 5;
        int total = n * cw + (n - 1) * gap;
        int x0 = std::max(2, (pw - total) / 2);
        for (int i = 0; i < n; ++i) portraits_.push_back({Rect{x0 + i * (cw + gap), 3, cw, chh}, ids[size_t(i)]});
        int ly = 3 + chh + 4;
        messages_.r = Rect{2, ly, pw - 4, 198 - ly};
        return;
    }
    // Combat: the roster (one line each), the monster slot, the buttons, the combat log.
    title_r_ = Rect{2, 2, pw - 4, 8};
    const int rh = 11;
    int y = 11;
    roster_ = Rect{2, y, pw - 4, rh * n};
    for (int i = 0; i < n; ++i) portraits_.push_back({Rect{2, y + i * rh, pw - 4, rh}, ids[size_t(i)]});
    roster_rows_ = n;
    y += rh * n + 2;
    monster_r_ = Rect{2, y, pw - 4, rh};
    y += rh + 3;
    if (turn_buttons()) {
        int cols = (pw - 4) >= 10 * 16 + 9 * 2 ? 10 : 5;
        cols_ = cols;
        int bs = std::min(22, ((pw - 4) - (cols - 1) * 2) / cols);
        int rows = (kButtons + cols - 1) / cols;
        int x0 = 2 + ((pw - 4) - (cols * bs + (cols - 1) * 2)) / 2;
        for (int b = 0; b < kButtons; ++b)
            buttons_.push_back({Rect{x0 + (b % cols) * (bs + 2), y + (b / cols) * (bs + 2), bs, bs}, b});
        y += rows * (bs + 2) + 1;
    }
    combat_.r = Rect{2, y, pw - 4, std::max(16, 198 - y)};
}

int HudPanel::visible(const LogBox& b) const { return std::max(1, (b.r.h - 4) / kLineH); }

void HudPanel::rewrap(LogBox& b) {
    const GameLog& g = gm_.world().game_log;
    int width = b.r.w - 4 - 5;
    if (width == b.width && g.serial(b.ch) == b.serial) return;
    size_t before = b.lines.size();
    bool fresh = width != b.width;
    b.lines.clear();
    const ShapeFont& f = gm_.world().font(kFont);
    for (const GameLog::Line& l : g.lines(b.ch)) {
        std::vector<TextPage> pages = layout_text(f, l.text, std::max(20, width - 3), 0);
        bool first = true;
        for (const TextPage& p : pages)
            for (const std::string& s : p.lines) {
                b.lines.push_back({s, l.kind, first});
                first = false;
            }
    }
    // New lines show at once unless the player has scrolled up: then the view stays put.
    if (!fresh && b.scroll > 0 && b.lines.size() > before) b.scroll += int(b.lines.size() - before);
    b.width = width;
    b.serial = g.serial(b.ch);
    b.scroll = std::clamp(b.scroll, 0, std::max(0, int(b.lines.size()) - visible(b)));
}

void HudPanel::scroll_by(LogBox& b, int d) {
    b.scroll = std::clamp(b.scroll + d, 0, std::max(0, int(b.lines.size()) - visible(b)));
}

int HudPanel::scroll() const { return shown_log().scroll; }
int HudPanel::log_lines() const { return int(shown_log().lines.size()); }
int HudPanel::log_visible() const { return visible(shown_log()); }

// ---------------------------------------------------------------------------------------------
// Drawing

void HudPanel::bevel(Surface8& s, const Rect& r, bool sunk) {
    s.fill_rect(r.x, r.y, r.w, 1, sunk ? c_dark_ : c_light_);
    s.fill_rect(r.x, r.y, 1, r.h, sunk ? c_dark_ : c_light_);
    s.fill_rect(r.x, r.y + r.h - 1, r.w, 1, sunk ? c_edge_ : c_dark_);
    s.fill_rect(r.x + r.w - 1, r.y, 1, r.h, sunk ? c_edge_ : c_dark_);
}

void HudPanel::draw_portrait(Surface8& s, uint16_t id, int x, int y, int w, int h) {
    World& wd = gm_.world();
    const Actor* a = wd.actor(id);
    if (!a || w <= 0 || h <= 0) return;
    // The party bar's picture (24 x 25): a painted portrait, else the standing frame's top.
    Surface8 tmp(24, 25);
    tmp.clear(c_box_);
    const Party* p = wd.party.get();
    if (const ShapeFrame* pf = p ? p->bar_portrait(id) : nullptr) {
        tmp.draw(*pf, (24 - pf->w) / 2, (25 - pf->h) / 2);
    } else {
        unsigned frame = a->frame;
        if (const AnimSequence* seq = wd.data().anim.get(a->shape, 2)) frame = seq->at(3, 0).frame;
        if (const ShapeFrame* f = wd.data().shapes.frame(a->shape, frame)) {
            const uint8_t* rm = p ? p->remap(id) : nullptr;
            int top = std::clamp(f->h - 46, 0, 16);
            tmp.draw(*f, 12 - f->w / 2 + f->xoff, 1 - top + f->yoff, false, nullptr, nullptr, rm);
        }
    }
    // Shrunk (or cropped to the head for the roster's tiny picture) with nearest sampling.
    bool head = w <= 12;
    int sx0 = head ? 3 : 0, sy0 = 0, sw = head ? 18 : 24, sh = head ? 18 : 25;
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx) {
            int sxp = sx0 + dx * sw / w, syp = sy0 + dy * sh / h;
            put(s, x + dx, y + dy, tmp.row(std::min(24, syp))[std::min(23, sxp)]);
        }
    if (a->is(Actor::kDead))
        for (int i = 0; i < std::min(w, h); ++i) { put(s, x + i, y + i, c_hp_); put(s, x + w - 1 - i, y + i, c_hp_); }
}

void HudPanel::draw_actor_mini(Surface8& s, uint16_t id, int x, int y, int w, int h) {
    World& wd = gm_.world();
    const Actor* a = wd.actor(id);
    if (!a) return;
    unsigned shape = a->shape, frame = a->frame;
    WorldView::Options::Look lk;
    if (wd.creatures && wd.creatures->appearance(id, lk) && !lk.hide) { shape = lk.shape; frame = lk.frame; }  // game/creatures.h
    const ShapeFrame* f = wd.data().shapes.frame(shape, frame);
    if (!f || f->w <= 0 || f->h <= 0) return;
    Surface8 tmp(f->w, f->h);
    tmp.clear(ShapeFrame::kTransparent);
    tmp.draw(*f, f->xoff, f->yoff, false);
    double k = std::min({1.0, double(w) / f->w, double(h) / f->h});
    int ow = std::max(1, int(f->w * k)), oh = std::max(1, int(f->h * k));
    int ox = x + (w - ow) / 2, oy = y + (h - oh) / 2;
    for (int dy = 0; dy < oh; ++dy)
        for (int dx = 0; dx < ow; ++dx) {
            uint8_t c = tmp.row(std::min(f->h - 1, int(dy / k)))[std::min(f->w - 1, int(dx / k))];
            if (c != ShapeFrame::kTransparent) put(s, ox + dx, oy + dy, c);
        }
}

void HudPanel::draw_icon(Surface8& s, int b, int x, int y, bool on) {
    if (b < 0 || b >= kButtons) return;
    for (int r = 0; r < kIconSize; ++r)
        for (int c = 0; c < kIconSize; ++c) {
            char ch = kHudIcons[b][r][c];
            if (ch == '.') continue;
            auto it = icon_cols_.find(ch);
            uint8_t col = it == icon_cols_.end() ? c_light_ : it->second;
            if (!on && ch != '#') col = c_grey_;
            put(s, x + c, y + r, col);
        }
}

std::string HudPanel::label_short(uint16_t id, int max_w) const {
    const ShapeFont& f = gm_.world().font(kFont);
    std::string n = GameLog::name_of(gm_.world(), id, false);
    if (!n.empty()) n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
    if (f.text_width(n) > max_w && n.find(' ') != std::string::npos) n = n.substr(0, n.find(' '));
    while (n.size() > 1 && f.text_width(n) > max_w) n.pop_back();
    return n;
}

void HudPanel::draw_log(Surface8& s, LogBox& b, const char* title) {
    rewrap(b);
    const ShapeFont& f = gm_.world().font(kFont);
    s.fill_rect(b.r.x, b.r.y, b.r.w, b.r.h, c_box_);
    bevel(s, b.r, true);
    int vis = visible(b);
    int total = int(b.lines.size());
    int last = total - b.scroll;  // one past the last line shown
    int first = std::max(0, last - vis);
    s.set_clip(b.r.x + 1, b.r.y + 1, b.r.w - 6, b.r.h - 2);
    int y = b.r.y + 2 + (vis - (last - first)) * kLineH;  // the newest line at the bottom
    if (total == 0 && title) f.draw_top(s, b.r.x + 3, b.r.y + 3, title, tint(kTGrey));
    for (int i = first; i < last; ++i, y += kLineH) {
        const Wrapped& l = b.lines[size_t(i)];
        f.draw_top(s, b.r.x + 3 + (l.first ? 0 : 3), y, l.text, tint(kind_tint(l.kind)));
    }
    s.reset_clip();
    // The drag bar.
    b.track = Rect{b.r.x + b.r.w - 5, b.r.y + 2, 3, b.r.h - 4};
    s.fill_rect(b.track.x, b.track.y, b.track.w, b.track.h, c_bg_);
    int th = b.track.h, ty = b.track.y;
    if (total > vis) {
        th = std::max(4, b.track.h * vis / total);
        int range = total - vis;
        ty = b.track.y + (b.track.h - th) * (range - b.scroll) / range;
    }
    b.thumb = Rect{b.track.x, ty, 3, th};
    s.fill_rect(b.thumb.x, b.thumb.y, b.thumb.w, b.thumb.h, b.dragging ? c_hl_ : c_edge_);
    if (b.scroll > 0) {
        // Scrolled up: a small marker that there is more below.
        int mx = b.r.x + b.r.w - 12, my = b.r.y + b.r.h - 5;
        for (int i = 0; i < 3; ++i) s.fill_rect(mx - i, my - i, 1 + 2 * i, 1, c_hl_);
    }
}

void HudPanel::draw(Surface8& s) {
    ensure_colours();
    World& w = gm_.world();
    layout(s.width());
    s.reset_clip();
    s.clear(c_bg_);
    // A thin carved edge against the picture.
    s.fill_rect(0, 0, 1, s.height(), c_dark_);
    s.fill_rect(1, 0, 1, s.height(), c_edge_);
    const ShapeFont& f = w.font(kFont);
    const Party* party = w.party.get();
    TurnCombat* tc = w.turn.get();
    const bool fight = tc && tc->active();
    const uint16_t cur = fight ? tc->current() : 0;
    int focus_portrait = focus_ ? focus_item_ : -1;
    int np = int(portraits_.size());
    if (!combat_view()) {
        for (int i = 0; i < np; ++i) {
            const Portrait& p = portraits_[size_t(i)];
            const Actor* a = w.actor(p.id);
            if (!a) continue;
            const Rect& r = p.r;
            s.fill_rect(r.x, r.y, r.w, r.h, c_edge_);
            int ph = r.h - 7;
            draw_portrait(s, p.id, r.x + 1, r.y + 1, r.w - 2, ph);
            int mh = max_hp_of(p.id);
            int mm = p.id == 1 ? std::max(0, 2 * int(a->intel)) : (party ? party->max_mana(*a) : 0);
            int bw = r.w - 2;
            s.fill_rect(r.x + 1, r.y + ph + 2, bw, 2, c_dark_);
            if (!a->is(Actor::kDead)) s.fill_rect(r.x + 1, r.y + ph + 2, std::clamp(int(a->hp) * bw / mh, 0, bw), 2, c_hp_);
            s.fill_rect(r.x + 1, r.y + ph + 4, bw, 2, c_dark_);
            if (mm > 0) s.fill_rect(r.x + 1, r.y + ph + 4, std::clamp(int(a->mana) * bw / mm, 0, bw), 2, c_mana_);
            if (const Leveling* lv = w.leveling.get(); lv && lv->active() && r.w >= 14) {  // the level, in the corner
                std::string l = std::to_string(lv->level_of(p.id));
                s.fill_rect(r.x + 1, r.y + 1, f.text_width(l) + 2, f.height(), c_dark_);
                f.draw_top(s, r.x + 2, r.y + 1, l, tint(kTGold));
            }
            bool hl = focus_portrait == i || (party && party->pad_highlight == i) || (hover_what_ == 100 + i);
            if (hl) frame_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, focus_portrait == i || (party && party->pad_highlight == i) ? c_hl_ : c_light_);
        }
        draw_log(s, messages_, "Messages");
        if (focus_ && focus_item_ == np) frame_rect(s, messages_.r.x - 1, messages_.r.y - 1, messages_.r.w + 2, messages_.r.h + 2, c_hl_);
        draw_tooltip(s);
        return;
    }
    // --- Combat: the title, the roster, the monster slot, the buttons, the log.
    char buf[64];
    if (fight) std::snprintf(buf, sizeof buf, "Round %d", tc->round());
    else std::snprintf(buf, sizeof buf, "Combat");
    f.draw_top(s, title_r_.x + 2, title_r_.y, buf, tint(kTGold));
    if (tc && tc->enabled && tc->party_auto) {
        const char* pa = "AUTO";
        f.draw_top(s, title_r_.x + title_r_.w - f.text_width(pa) - 2, title_r_.y, pa, tint((frame_ / 15) % 2 ? kTYellow : kTGold));
    }
    for (int i = 0; i < np; ++i) {
        const Portrait& p = portraits_[size_t(i)];
        const Actor* a = w.actor(p.id);
        if (!a) continue;
        const Rect& r = p.r;
        bool now = p.id == cur && cur;
        if (now) s.fill_rect(r.x, r.y, r.w, r.h, c_band_);
        s.fill_rect(r.x, r.y, 11, 11, c_edge_);
        draw_portrait(s, p.id, r.x + 1, r.y + 1, 9, 9);
        int mh = max_hp_of(p.id);
        char hp[32];
        std::snprintf(hp, sizeof hp, "%d/%d", a->is(Actor::kDead) ? 0 : int(a->hp), mh);
        int hw = f.text_width(hp);
        std::string name = label_short(p.id, r.w - 13 - hw - 4);
        int ty = r.y + 1;
        bool dead = a->is(Actor::kDead);
        f.draw_top(s, r.x + 13, ty, name, tint(dead ? kTGrey : now ? kTYellow : kTDefault));
        int low = int(a->hp) * 4 < mh ? kTRed : int(a->hp) * 2 < mh ? kTGold : kTGreen;
        f.draw_top(s, r.x + r.w - hw - 1, ty, hp, tint(dead ? kTGrey : low));
        if (fight && (tc->keep_auto(p.id) || tc->party_auto)) put(s, r.x + 11, r.y + 1, icon_cols_['c']);
        if (focus_portrait == i || (party && party->pad_highlight == i)) frame_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, c_hl_);
        else if (hover_what_ == 100 + i) frame_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, c_light_);
    }
    // The monster slot: the monster acting now (turn-based), else the latest foe in a blow.
    if (fight) {
        if (cur && !tc->is_party_fighter(cur)) monster_ = cur;
        const Actor* m = w.actor(monster_);
        if (!m || m->is(Actor::kDead) || !tc->is_fighter(monster_)) {
            monster_ = 0;  // the next foe of the fight still standing
            for (const auto& g : tc->order())
                if (!g.party && w.actor(g.id) && !w.actor(g.id)->is(Actor::kDead)) { monster_ = g.id; break; }
        }
    } else {
        monster_ = w.game_log.last_foe;
    }
    const Rect& mr = monster_r_;
    s.fill_rect(mr.x, mr.y - 2, mr.w, 1, c_edge_);
    bool foe_now = fight && monster_ && monster_ == cur;
    if (foe_now) s.fill_rect(mr.x, mr.y, mr.w, mr.h, c_foe_);
    if (const Actor* m = w.actor(monster_)) {
        s.fill_rect(mr.x, mr.y, 11, 11, c_dark_);
        draw_actor_mini(s, monster_, mr.x + 1, mr.y + 1, 9, 9);
        char hp[32];
        if (m->is(Actor::kDead)) std::snprintf(hp, sizeof hp, "dead");
        else std::snprintf(hp, sizeof hp, "%d/%d", int(m->hp), max_hp_of(monster_));
        int hw = f.text_width(hp);
        f.draw_top(s, mr.x + 13, mr.y + 1, label_short(monster_, mr.w - 13 - hw - 4), tint(foe_now ? kTYellow : kTRed));
        f.draw_top(s, mr.x + mr.w - hw - 1, mr.y + 1, hp, tint(kTGrey));
    } else {
        f.draw_top(s, mr.x + 13, mr.y + 1, "No foe", tint(kTGrey));
    }
    // The action buttons.
    TurnUI& ui = gm_.turn_ui();
    int sel = ui.selected_entry(), act = ui.active_entry();
    for (const Btn& bt : buttons_) {
        bool on = button_enabled(bt.b);
        bool lit = bt.b == act || (bt.b == kBPartyAuto && tc && tc->party_auto) ||
                   (bt.b == kBKeepAuto && tc && cur && tc->keep_auto(cur));
        const Rect& r = bt.r;
        s.fill_rect(r.x, r.y, r.w, r.h, lit ? c_btn_on_ : c_btn_);
        bevel(s, r, lit);
        draw_icon(s, bt.b, r.x + (r.w - kIconSize) / 2 + (lit ? 1 : 0), r.y + (r.h - kIconSize) / 2 + (lit ? 1 : 0), on);
        bool focus_here = focus_ && focus_item_ == np + 1 + bt.b;
        if (focus_here || (bt.b == sel && ui.awaiting())) frame_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, c_hl_);
        else if (hover_what_ == bt.b) frame_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, c_light_);
    }
    draw_log(s, combat_, "Combat log");
    if (focus_ && focus_item_ == np) frame_rect(s, combat_.r.x - 1, combat_.r.y - 1, combat_.r.w + 2, combat_.r.h + 2, c_hl_);
    draw_tooltip(s);
}

void HudPanel::draw_tooltip(Surface8& s) {
    tip_.clear();
    int what = -1;
    Rect anchor;
    int np = int(portraits_.size());
    if (focus_ && focus_item_ > np && focus_item_ - np - 1 < int(buttons_.size())) {
        what = focus_item_ - np - 1;
    } else if (focus_ && focus_item_ < np) {
        what = 100 + focus_item_;
    } else if (hover_what_ >= 0 && hover_frames_ >= 8) {
        what = hover_what_;
    }
    if (what < 0) return;
    std::string title, body;
    if (what >= 100) {
        int i = what - 100;
        if (i >= np) return;
        anchor = portraits_[size_t(i)].r;
        uint16_t id = portraits_[size_t(i)].id;
        const Actor* a = gm_.world().actor(id);
        if (!a) return;
        title = label_short(id, 1000);
        const Party* p = gm_.world().party.get();
        int mm = id == 1 ? std::max(0, 2 * int(a->intel)) : (p ? p->max_mana(*a) : 0);
        char buf[96];
        std::snprintf(buf, sizeof buf, "HP %d/%d  Mana %d/%d. Click: %s", a->is(Actor::kDead) ? 0 : int(a->hp), max_hp_of(id),
                      int(a->mana), mm, id == 1 ? "paperdoll" : "inventory");
        body = buf;
        if (const Leveling* lv = gm_.world().leveling.get(); lv && lv->active()) {
            const int level = lv->level_of(id), next = Leveling::next_level_xp(level);
            body = "Level " + std::to_string(level) + ", XP " + std::to_string(lv->xp_of(id)) +
                   (next ? "/" + std::to_string(next) : std::string()) + ". " + body;
            // Stage 5: each spell with its uses left, or why it is locked.
            const int ci = p && id != 1 ? p->index_of(id) : -1;
            if (ci >= 0 && lv->magic->loaded) {
                const std::string sm = lv->magic->summary(ci);
                if (!sm.empty()) body += " Spells: " + sm + ".";
            }
        }
        if (combat_view()) return;  // the roster line already says it
    } else {
        if (what >= int(buttons_.size())) return;
        anchor = buttons_[size_t(what)].r;
        // Below the whole grid (over the log), so no button is covered.
        for (const Btn& bt : buttons_) anchor.h = std::max(anchor.h, bt.r.y + bt.r.h - anchor.y);
        int b = buttons_[size_t(what)].b;
        if (b == kBPartyAuto) {
            title = "Party Auto  (A, RB)";
            body = kPartyAutoHelp;
        } else {
            title = std::string(TurnUI::entry_name(b)) + "  (" + std::to_string(b + 1) + ")";
            body = TurnUI::entry_help(b);
        }
        if (!button_enabled(b)) body += " Not now.";
    }
    tip_ = title + ": " + body;
    const ShapeFont& f = gm_.world().font(kFont);
    int w = pw_ - 4;
    std::vector<TextPage> pages = layout_text(f, body, w - 6, 0);
    std::vector<std::string> lines;
    for (const auto& p : pages)
        for (const auto& l : p.lines) lines.push_back(l);
    int h = kLineH * int(lines.size() + 1) + 5;
    int y = anchor.y + anchor.h + 3;
    if (y + h > 198) y = std::max(1, anchor.y - h - 3);
    Rect r{2, y, w, h};
    s.fill_rect(r.x, r.y, r.w, r.h, c_dark_);
    frame_rect(s, r.x, r.y, r.w, r.h, c_hl_);
    f.draw_top(s, r.x + 3, r.y + 3, title, tint(kTYellow));
    for (size_t i = 0; i < lines.size(); ++i) f.draw_top(s, r.x + 3, r.y + 3 + kLineH * int(i + 1), lines[i], tint(kTDefault));
}

// ---------------------------------------------------------------------------------------------
// Input

void HudPanel::tick() {
    ++frame_;
    if (hover_what_ >= 0) ++hover_frames_;
    // The left stick on the focused log scrolls it.
    if (focus_ && stick_y_ != 0) {
        if (stick_repeat_ <= 0) { scroll_by(shown_log(), stick_y_ < 0 ? 1 : -1); stick_repeat_ = 4; }
        else --stick_repeat_;
    }
    if (focus_) focus_item_ = std::clamp(focus_item_, 0, std::max(0, focus_count() - 1));
}

bool HudPanel::button_enabled(int b) const {
    World& w = gm_.world();
    if (b == kBPartyAuto) return w.turn && w.turn->enabled && w.turn->active();
    return gm_.turn_ui().entry_on(b);
}

void HudPanel::press_button(int b) {
    World& w = gm_.world();
    if (b == kBPartyAuto) {
        if (w.turn && w.turn->enabled && w.turn->active()) w.turn->toggle_party_auto();
        return;
    }
    gm_.turn_ui().press_entry(b);
}

uint16_t HudPanel::portrait_actor(int x, int y) const {
    for (const Portrait& p : portraits_)
        if (p.r.in(x, y)) return p.id;
    return 0;
}

bool HudPanel::mouse_down(int button, int x, int y) {
    World& w = gm_.world();
    if (pw_ <= 0) return false;
    LogBox& b = shown_log();
    if (button == 1 && b.thumb.in(x, y) && b.lines.size() > size_t(visible(b))) {
        b.dragging = true;
        b.drag_y0 = y;
        b.drag_scroll0 = b.scroll;
        return true;
    }
    if (button == 1 && b.track.in(x, y)) {
        scroll_by(b, y < b.thumb.y ? visible(b) : -visible(b));
        return true;
    }
    if (w.gumps.top_modal()) return true;  // the menu book or a reader is up: only the log scrolls
    if (button == 1)
        for (const Btn& bt : buttons_)
            if (bt.r.in(x, y)) {
                if (button_enabled(bt.b)) press_button(bt.b);
                else if (w.turn) w.turn->status = std::string(bt.b == kBPartyAuto ? "Party Auto" : TurnUI::entry_name(bt.b)) + ": not now";
                hover_frames_ = 0;
                return true;
            }
    for (size_t i = 0; i < portraits_.size(); ++i)
        if (portraits_[i].r.in(x, y)) {
            if (button == 1 && w.party && w.party->enabled && !w.stasis) w.party->open_bar_slot(int(i));
            else if (button == 1 && !w.stasis && portraits_[i].id == 1) w.call_event(1, kEvUse);
            return true;
        }
    return true;  // the panel itself: nothing behind it
}

void HudPanel::mouse_up(int button, int x, int y) {
    (void)x;
    (void)y;
    if (button == 1) messages_.dragging = combat_.dragging = false;
}

void HudPanel::mouse_move(int x, int y) {
    hover_x_ = x;
    hover_y_ = y;
    LogBox& b = shown_log();
    if (b.dragging) {
        int range = int(b.lines.size()) - visible(b);
        int px = b.track.h - b.thumb.h;
        if (range > 0 && px > 0) b.scroll = std::clamp(b.drag_scroll0 - (y - b.drag_y0) * range / px, 0, range);
        return;
    }
    int what = -1;
    for (const Btn& bt : buttons_)
        if (bt.r.in(x, y)) what = bt.b;
    for (size_t i = 0; i < portraits_.size() && what < 0; ++i)
        if (portraits_[i].r.in(x, y)) what = 100 + int(i);
    if (what != hover_what_) { hover_what_ = what; hover_frames_ = 0; }
}

void HudPanel::leave() {
    hover_what_ = -1;
    hover_frames_ = 0;
    hover_x_ = hover_y_ = -1;
}

bool HudPanel::wheel(int dir, int x, int y) {
    (void)x;
    (void)y;
    if (!dir) return false;
    scroll_by(shown_log(), dir > 0 ? 2 : -2);
    return true;
}

int HudPanel::focus_count() const { return int(portraits_.size()) + 1 + int(buttons_.size()); }

void HudPanel::set_focus(bool on) {
    focus_ = on;
    stick_y_ = 0;
    if (on) {
        int np = int(portraits_.size());
        // In a fight, on the selected action (else the log); outside, on the log.
        int sel = gm_.turn_ui().selected_entry();
        focus_item_ = (!buttons_.empty() && sel >= 0) ? np + 1 + sel : np;
    }
}

bool HudPanel::pad(const InputEvent& ev) {
    if (!focus_) return false;
    World& w = gm_.world();
    // A pick, a list or a script's target cursor is open: the controller belongs to it (A fires).
    if ((turn_buttons() && gm_.turn_ui().mode() != TurnUI::kMenu) || w.target_pid) {
        set_focus(false);
        return false;
    }
    if (ev.type == InputEvent::kPadAxis) {
        if (ev.axis == kAxisLY) {
            stick_y_ = std::abs(ev.value) > 16000 ? ev.value : 0;
            if (!stick_y_) stick_repeat_ = 0;
            return true;
        }
        return ev.axis == kAxisLX;
    }
    if (ev.type != InputEvent::kPadDown) return ev.type == InputEvent::kPadUp && ev.button != kPadStart && ev.button != kPadBack;
    const int np = int(portraits_.size()), n = focus_count();
    const bool on_log = focus_item_ == np;
    // On the icons the D-pad goes by rows and columns, as it does without the focus; Up from the
    // top row reaches the log.
    const int on_button = focus_item_ > np ? focus_item_ - np - 1 : -1;
    if (on_button >= 0 && on_button < int(buttons_.size()) &&
        (ev.button == kPadUp || ev.button == kPadDown || ev.button == kPadLeft || ev.button == kPadRight)) {
        const int nb = int(buttons_.size());
        if (ev.button == kPadUp && on_button < cols_) { focus_item_ = np; return true; }
        focus_item_ = np + 1 + TurnUI::grid_step(on_button, nb, cols_, ev.button == kPadRight ? 1 : ev.button == kPadLeft ? -1 : 0,
                                                 ev.button == kPadDown ? 1 : ev.button == kPadUp ? -1 : 0);
        return true;
    }
    switch (ev.button) {
    case kPadB:
    case kPadRStick: set_focus(false); return true;
    case kPadLB: scroll_by(shown_log(), visible(shown_log())); return true;
    case kPadRB: scroll_by(shown_log(), -visible(shown_log())); return true;
    case kPadUp:
        if (on_log) scroll_by(shown_log(), 1);
        else focus_item_ = (focus_item_ + n - 1) % n;
        return true;
    case kPadDown:
        if (on_log) scroll_by(shown_log(), -1);
        else focus_item_ = (focus_item_ + 1) % n;
        return true;
    case kPadLeft: focus_item_ = (focus_item_ + n - 1) % n; return true;
    case kPadRight: focus_item_ = (focus_item_ + 1) % n; return true;
    case kPadA:
        if (focus_item_ < np) {
            if (!w.gumps.top_modal() && !w.stasis) {
                if (w.party && w.party->enabled) w.party->open_bar_slot(focus_item_);
                else w.call_event(1, kEvUse);
            }
        } else if (focus_item_ > np) {
            int b = focus_item_ - np - 1;
            if (b < int(buttons_.size()) && button_enabled(buttons_[size_t(b)].b)) press_button(buttons_[size_t(b)].b);
        }
        return true;
    case kPadStart:
    case kPadBack:
        return false;  // the main menu, the atlas
    default: return true;
    }
}

// ---------------------------------------------------------------------------------------------
// Test points

bool HudPanel::button_point(int b, int& x, int& y) const {
    for (const Btn& bt : buttons_)
        if (bt.b == b) { x = bt.r.x + bt.r.w / 2; y = bt.r.y + bt.r.h / 2; return true; }
    return false;
}

bool HudPanel::portrait_point(int i, int& x, int& y) const {
    if (i < 0 || size_t(i) >= portraits_.size()) return false;
    x = portraits_[size_t(i)].r.x + portraits_[size_t(i)].r.w / 2;
    y = portraits_[size_t(i)].r.y + portraits_[size_t(i)].r.h / 2;
    return true;
}

bool HudPanel::log_point(int& x, int& y) const {
    const LogBox& b = shown_log();
    if (!b.r.w) return false;
    x = b.r.x + b.r.w / 2;
    y = b.r.y + b.r.h / 2;
    return true;
}

}  // namespace pg
