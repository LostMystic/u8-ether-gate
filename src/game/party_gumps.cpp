// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The party bar (engine extra, PLAN-COMPANIONS.md): a row of small portraits along the top of
// the screen - the Avatar and each companion in the party, cropped from their standing frame,
// with hit point and mana bars. A click opens that member's paperdoll. Not saved with the game
// (it is rebuilt from the party every frame).
#include <algorithm>
#include <cmath>

#include "data/gamedata.h"
#include "core/log.h"
#include "data/picture.h"
#include "gfx/surface.h"
#include "game/party.h"
#include "game/world.h"

namespace pg {

class PartyBarGump : public Gump {
public:
    static constexpr int kCellW = 26, kCellH = 33, kPortraitH = 25;
    explicit PartyBarGump(World& w) : Gump(w) {
        x = 4;
        y = 3;
        layer = kLayerNormal;
    }
    const char* kind() const override { return "partybar"; }

    std::vector<uint16_t> ids() const {
        if (const Party* p = w_.party.get()) return p->bar_ids();
        return {1};
    }
    int slot_at(int lx, int ly) const {
        if (ly < 0 || ly >= kCellH || lx < 0) return -1;
        int i = lx / (kCellW + 2);
        if (lx - i * (kCellW + 2) >= kCellW) return -1;
        return i < int(ids().size()) ? i : -1;
    }
    bool hit(int lx, int ly) const override { return slot_at(lx, ly) >= 0; }
    bool press(int button, int lx, int ly) override { return slot_at(lx, ly) >= 0; }
    void click(int button, int lx, int ly) override { open(lx, ly); }
    void double_click(int button, int lx, int ly) override { open(lx, ly); }
    void open(int lx, int ly) {
        int i = slot_at(lx, ly);
        if (i < 0) return;
        if (Party* p = w_.party.get()) p->open_bar_slot(i);
    }
    void tick() override {
        size_t n = ids().size();
        w = int(n) * (kCellW + 2);
        h = kCellH;
    }

    void paint(Surface8& s) override {
        if (!cols_) find_colours();
        const Party* p = w_.party.get();
        std::vector<uint16_t> list = ids();
        for (size_t i = 0; i < list.size(); ++i) {
            const Actor* a = w_.actor(list[i]);
            if (!a) continue;
            int cx = x + int(i) * (kCellW + 2), cy = y;
            s.fill_rect(cx, cy, kCellW, kCellH, edge_);
            s.fill_rect(cx + 1, cy + 1, kCellW - 2, kPortraitH, bg_);
            // A painted portrait (art.portrait, data/portraits/) fills the box as it is.
            if (const ShapeFrame* pf = p ? p->bar_portrait(list[i]) : nullptr) {
                s.set_clip(cx + 1, cy + 1, kCellW - 2, kPortraitH);
                s.draw(*pf, cx + 1 + (kCellW - 2 - pf->w) / 2, cy + 1 + (kPortraitH - pf->h) / 2);
                s.reset_clip();
            } else {
                // The standing frame facing the viewer, its top (head and shoulders) in the box.
                unsigned frame = a->frame;
                if (const AnimSequence* seq = w_.data().anim.get(a->shape, 2)) frame = seq->at(3, 0).frame;
                if (const ShapeFrame* f = w_.data().shapes.frame(a->shape, frame)) {
                    s.set_clip(cx + 1, cy + 1, kCellW - 2, kPortraitH);
                    const uint8_t* rm = p ? p->remap(list[i]) : nullptr;
                    std::vector<uint8_t> px;
                    // Remap by drawing a recoloured copy of the frame.
                    ShapeFrame copy;
                    const ShapeFrame* draw = f;
                    if (rm) {
                        copy = *f;
                        for (uint8_t& c : copy.pixels)
                            if (c != ShapeFrame::kTransparent) c = rm[c];
                        draw = &copy;
                    }
                    int top = std::clamp(draw->h - 46, 0, 16);  // tall frames (a spear over the head): look lower
                    s.draw(*draw, cx + kCellW / 2 - draw->w / 2 + draw->xoff, cy + 2 - top + draw->yoff);
                    s.reset_clip();
                }
            }
            int mh = list[i] == 1 ? std::max(1, 2 * int(a->str)) : (p ? p->max_hp(*a) : 1);
            int mm = list[i] == 1 ? std::max(0, 2 * int(a->intel)) : (p ? p->max_mana(*a) : 0);
            int bw = kCellW - 2;
            s.fill_rect(cx + 1, cy + kPortraitH + 1, bw, 3, dark_);
            if (!a->is(Actor::kDead))
                s.fill_rect(cx + 1, cy + kPortraitH + 1, std::clamp(int(a->hp) * bw / std::max(1, mh), 0, bw), 3, 39);
            s.fill_rect(cx + 1, cy + kPortraitH + 5, bw, 2, dark_);
            if (mm > 0) s.fill_rect(cx + 1, cy + kPortraitH + 5, std::clamp(int(a->mana) * bw / mm, 0, bw), 2, 139);
            if (a->is(Actor::kInCombat)) s.fill_rect(cx, cy, kCellW, 1, 39);
            // The slot picked with the controller (LB + D-pad): a bright frame.
            if (p && p->pad_highlight == int(i)) {
                uint8_t hl = hl_;
                s.fill_rect(cx - 1, cy - 1, kCellW + 2, 1, hl);
                s.fill_rect(cx - 1, cy + kCellH, kCellW + 2, 1, hl);
                s.fill_rect(cx - 1, cy - 1, 1, kCellH + 2, hl);
                s.fill_rect(cx + kCellW, cy - 1, 1, kCellH + 2, hl);
            }
        }
    }

private:
    void find_colours() {
        const Palette& pal = w_.data().palette;
        auto nearest = [&](int r, int g, int b) {
            int best = 0, bd = 1 << 30;
            for (int i = 16; i < 256; ++i) {
                if (w_.data().xform.loaded && w_.data().xform.is_xform(uint8_t(i))) continue;
                int dr = pal.rgb8[size_t(i)][0] - r, dg = pal.rgb8[size_t(i)][1] - g, db = pal.rgb8[size_t(i)][2] - b;
                int d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = i; }
            }
            return uint8_t(best);
        };
        edge_ = nearest(150, 140, 120);
        bg_ = nearest(40, 36, 32);
        dark_ = nearest(10, 10, 10);
        hl_ = nearest(255, 230, 60);
        cols_ = true;
    }
    bool cols_ = false;
    uint8_t edge_ = 0, bg_ = 0, dark_ = 0, hl_ = 0;
};

std::vector<uint16_t> Party::bar_ids() const {
    std::vector<uint16_t> out{1};
    for (int m : members) out.push_back(state[size_t(m)].id);
    return out;
}

void Party::open_bar_slot(int i) {
    std::vector<uint16_t> list = bar_ids();
    if (i < 0 || i >= int(list.size())) return;
    uint16_t id = list[size_t(i)];
    if (id == 1) {
        if (!w_.gumps.container_gump(1)) w_.call_event(1, kEvUse);
        return;
    }
    int ci = index_of(id);
    if (ci >= 0) open_inventory(ci);
}

bool Party::bar_wanted() const {
    return enabled && show_bar && spawned_once && !members.empty() && !hud_panel;
}

void Party::update_bar() {
    PartyBarGump* g = w_.gumps.find<PartyBarGump>();
    bool want = bar_wanted() && !w_.gumps.top_modal();
    if (want && !g) {
        auto ng = std::make_unique<PartyBarGump>(w_);
        ng->tick();
        w_.gumps.add(std::move(ng));
    } else if (!want && g && !w_.gumps.top_modal()) {
        g->close();
        w_.gumps.reap();
    }
}

// ---------------------------------------------------------------------------------------------
// A companion's own paperdoll picture.

namespace {

// EPX / Scale2x: doubles a frame, rounding diagonal edges instead of making blocks.
ShapeFrame scale2x(const ShapeFrame& f) {
    ShapeFrame o;
    o.w = f.w * 2;
    o.h = f.h * 2;
    o.xoff = f.xoff * 2;
    o.yoff = f.yoff * 2;
    o.pixels.assign(size_t(o.w) * o.h, ShapeFrame::kTransparent);
    auto p = [&](int x, int y) {
        x = std::clamp(x, 0, f.w - 1);
        y = std::clamp(y, 0, f.h - 1);
        return f.pixels[size_t(y) * f.w + x];
    };
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            uint8_t c = p(x, y), a = p(x, y - 1), b = p(x + 1, y), cc = p(x - 1, y), d = p(x, y + 1);
            uint8_t e0 = c, e1 = c, e2 = c, e3 = c;
            if (cc == a && cc != d && a != b) e0 = a;
            if (a == b && a != cc && b != d) e1 = b;
            if (d == cc && d != b && cc != a) e2 = cc;
            if (b == d && b != a && d != cc) e3 = d;
            o.pixels[size_t(2 * y) * o.w + 2 * x] = e0;
            o.pixels[size_t(2 * y) * o.w + 2 * x + 1] = e1;
            o.pixels[size_t(2 * y + 1) * o.w + 2 * x] = e2;
            o.pixels[size_t(2 * y + 1) * o.w + 2 * x + 1] = e3;
        }
    return o;
}

}  // namespace

const ShapeFrame* Party::doll_picture(uint16_t actor) const {
    int i = index_of(actor);
    if (i < 0) return nullptr;
    // Own figure (Nerissa), or the Avatar's paperdoll body in the companion's colours with a
    // painted head (art.paperdoll_head) over his.
    const bool own_fig = defs[size_t(i)].own_doll;
    if (!own_fig && defs[size_t(i)].doll_head.empty()) return nullptr;
    auto it = doll_pictures_.find(i);
    if (it != doll_pictures_.end()) return it->second.get();
    const GameData& d = w_.data();
    const ShapeFrame* g = d.gumps.frame(1, 0);
    const AnimSequence* stand = own_fig ? d.anim.get(defs[size_t(i)].shape, 2) : nullptr;
    // Facing east (towards the screen's lower right), as the Avatar's paperdoll body does.
    const ShapeFrame* body = stand ? d.shapes.frame(defs[size_t(i)].shape, stand->at(2, 0).frame) : nullptr;
    if (!g || (own_fig && !body) || g->w < 80 || g->h < 104) return nullptr;
    auto pic = std::make_unique<ShapeFrame>(*g);
    // The body panel (gump pixels x 15..71, y 15..100; measured on gump 1): stone texture again.
    // The panel's own stone is palette 22 with a sprinkle of 21; the left bevel is taken from a
    // row the Avatar's body does not reach.
    auto px = [&](int x, int y) -> uint8_t& { return pic->pixels[size_t(y) * pic->w + x]; };
    auto stone = [&](int x, int y) -> uint8_t {
        if (x <= 19) return g->pixels[size_t(96) * g->w + x];
        if (y == 100) return 21;
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return (h % 100) < 18 ? 21 : 22;
    };
    std::vector<uint8_t> fig(pic->pixels.size(), 0);  // the figure's own pixels
    const uint8_t* rm = doll_remap(actor);
    if (!own_fig) {
        // The Avatar's body stays: its pixels are everything in the panel but the stone (21, 22)
        // and the left bevel; the whole picture takes the companion's colours, as the plain
        // recoloured paperdoll is drawn.
        for (int y = 15; y <= 100; ++y)
            for (int x = 20; x <= 71; ++x) {
                uint8_t c = g->pixels[size_t(y) * g->w + x];
                if (c != 21 && c != 22 && c != ShapeFrame::kTransparent) fig[size_t(y) * pic->w + x] = 1;
            }
        if (rm)
            for (uint8_t& c : pic->pixels)
                if (c != ShapeFrame::kTransparent) c = rm[c];
    }
    if (own_fig) {
    for (int y = 15; y <= 100; ++y)
        for (int x = 15; x <= 71; ++x) px(x, y) = stone(x, y);
    // The figure: doubled with Scale2x, then fitted to the panel's height (nearest sampling).
    ShapeFrame big = scale2x(*body);
    const int top = 17, bottom = 99, left = 20, right = 71;
    float k = std::min(1.0f, std::min(float(bottom - top + 1) / big.h, float(right - left + 1) / big.w));
    int ow = int(big.w * k), oh = int(big.h * k);
    int ox = (left + right + 1) / 2 - ow / 2 + 2, oy = bottom + 1 - oh;
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            int sx = std::min(big.w - 1, int((x + 0.5f) / k)), sy = std::min(big.h - 1, int((y + 0.5f) / k));
            uint8_t c = big.pixels[size_t(sy) * big.w + sx];
            if (c == ShapeFrame::kTransparent) continue;
            if (rm) c = rm[c];  // art.paperdoll_recolour (e.g. her hair as dark as the painted head's)
            int tx = ox + x, ty = oy + y;
            if (tx >= 15 && tx <= 71 && ty >= 13 && ty <= 100) {
                px(tx, ty) = c;
                fig[size_t(ty) * pic->w + tx] = 1;
            }
        }
    }
    // A painted head (art.paperdoll_head): over the figure's head, centred on its face.
    const ShapeFrame* hd = defs[size_t(i)].doll_head.empty() ? nullptr : picture(defs[size_t(i)].doll_head);
    if (hd) {
        int fig_top = -1;
        long fx = 0, fn = 0;
        for (int y = 13; y <= 100 && (fig_top < 0 || y < fig_top + 16); ++y)
            for (int x = 15; x <= 71; ++x) {
                if (!fig[size_t(y) * pic->w + x]) continue;
                if (fig_top < 0) fig_top = y;
                uint8_t c = own_fig ? px(x, y) : g->pixels[size_t(y) * g->w + x];  // before the recolour
                if (c >= 200 && c <= 210) { fx += x; ++fn; }  // the skin ramp (the face)
            }
        if (!own_fig) {
            // The Avatar's paperdoll body: his head (hair top to chin, about 11 x 15) is centred at
            // x 49 with its top at y 21 of gump 1 (measured; his raised arm is skin too, so the
            // skin search below would land on his fist).
            fig_top = 20;
            fx = 49;
            fn = 1;
        }
        if (fig_top >= 0 && fn > 0) {
            int hx = int(fx / fn) - hd->w / 2 + defs[size_t(i)].doll_head_dx;
            int hy = fig_top + defs[size_t(i)].doll_head_dy;
            auto inside = [&](int x, int y) { return x >= 15 && x <= 71 && y >= 13 && y <= 100; };
            auto head_at = [&](int x, int y) {
                int u = x - hx, v = y - hy;
                return u >= 0 && v >= 0 && u < hd->w && v < hd->h && hd->pixels[size_t(v) * hd->w + u] != ShapeFrame::kTransparent;
            };
            if (!own_fig) {
                doll_head_at_[i] = {hx, hy};  // drawn again over the equipment (gumps.cpp)
                // His own head (x 43..56, y 20..33, clear of the raised arm) where the new one
                // does not cover it: back to stone, so no blond hair peeks out beside a hood.
                for (int y = 20; y <= 33; ++y)
                    for (int x = 43; x <= 56; ++x)
                        if (inside(x, y) && fig[size_t(y) * pic->w + x] && !head_at(x, y)) {
                            px(x, y) = stone(x, y);
                            fig[size_t(y) * pic->w + x] = 0;
                        }
            }
            // The old head goes: figure pixels in the head's rows, beside the new head, back to stone
            // (her own figure's head; the Avatar's body keeps its shoulders, his head is under the new one).
            for (int y = hy - 1; own_fig && y < hy + hd->h * 3 / 4; ++y)
                for (int x = hx - 5; x < hx + hd->w + 5; ++x)
                    if (inside(x, y) && fig[size_t(y) * pic->w + x] && !head_at(x, y)) {
                        px(x, y) = stone(x, y);
                        fig[size_t(y) * pic->w + x] = 0;
                    }
            for (int v = 0; v < hd->h; ++v)
                for (int u = 0; u < hd->w; ++u)
                    if (head_at(hx + u, hy + v) && inside(hx + u, hy + v)) px(hx + u, hy + v) = hd->pixels[size_t(v) * hd->w + u];
            // The game's dark edge round the new head where it meets the stone.
            for (int y = hy - 1; y <= hy + hd->h; ++y)
                for (int x = hx - 1; x <= hx + hd->w; ++x) {
                    if (!inside(x, y) || head_at(x, y) || fig[size_t(y) * pic->w + x]) continue;
                    if (head_at(x - 1, y) || head_at(x + 1, y) || head_at(x, y - 1) || head_at(x, y + 1)) px(x, y) = 0;
                }
        }
    }
    const ShapeFrame* out = pic.get();
    doll_pictures_[i] = std::move(pic);
    return out;
}

const ShapeFrame* Party::picture(const std::string& file) const {
    auto it = pictures_.find(file);
    if (it != pictures_.end()) return it->second.get();
    auto f = std::make_unique<ShapeFrame>();
    std::string err;
    std::string path = (data_dir.empty() ? std::string("data") : data_dir) + "/" + file;
    if (!load_picture(path, w_.data().palette, w_.data().xform, *f, &err)) {
        PG_LOG("party: picture %s: %s", file.c_str(), err.c_str());
        f.reset();
    }
    const ShapeFrame* out = f.get();
    pictures_[file] = std::move(f);
    return out;
}

const ShapeFrame* Party::doll_head_over(uint16_t actor, int& x, int& y) const {
    int i = index_of(actor);
    if (i < 0 || !doll_picture(actor)) return nullptr;
    auto it = doll_head_at_.find(i);
    if (it == doll_head_at_.end()) return nullptr;
    x = it->second.first;
    y = it->second.second;
    return picture(defs[size_t(i)].doll_head);
}

const ShapeFrame* Party::bar_portrait(uint16_t actor) const {
    int i = index_of(actor);
    if (i < 0 || defs[size_t(i)].portrait.empty()) return nullptr;
    return picture(defs[size_t(i)].portrait);
}

}  // namespace pg
