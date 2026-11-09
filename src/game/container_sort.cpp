// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Container sort (container_sort.h): merge, group, and a layout where every item can be
// clicked. Everything here is the engine's own design.
#include "game/story_fixes.h"
#include "game/container_sort.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/gumps.h"
#include "game/world.h"

namespace pg {

// ---------------------------------------------------------------------------------------------
// Categories

const char* sort_category_name(SortCategory c) {
    static const char* n[] = {"weapons", "armour", "reagents", "magic", "potions", "keys",
                              "food", "money", "books", "containers", "misc"};
    return unsigned(c) < unsigned(SortCategory::kCount) ? n[unsigned(c)] : "?";
}

namespace {
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool ends(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}  // namespace

// Rules, first match wins (TYPEFLAG bytes from specs/formats/typeflags.md; class names are the
// 8-letter names of EUSECODE.FLX, specs/formats/usecode_file.md):
//  - equip type 6 = weapon, 1..5 = armour (shield, arm guards, helmet, body armour, leggings);
//  - family 2 (quantity: coins) = money;
//  - family 9, or a class named *REAG (the three reagent shapes) = reagents;
//  - a class named *KEY* (KEY, KEYONEC, KEYRING) = keys;
//  - a family 6 shape (container: bags, baskets, boxes) = containers;
//  - POTION = potions; *FOCUS / *ITEM (the foci and spell items), GEMOFPRO, RECALL = magic;
//  - *BOOK*, *SCRL, *SCROL, GRIMOIRE = books and scrolls;
//  - *FOOD*, *FOO, MOREFISH = food;
//  - the thrown weapons DART, GRENADE, FGRENADE, FIREBOMB = weapons;
//  - anything else = misc.
SortCategory sort_category(const GameData& d, unsigned shape) {
    const ShapeInfo& si = d.typeflags.get(shape);
    unsigned eq = si.equip_type();
    if (eq == 6) return SortCategory::kWeapon;
    if (eq >= 1 && eq <= 5) return SortCategory::kArmour;
    const std::string n = d.usecode.class_name(shape);
    if (si.family() == kFamQuantity) return SortCategory::kMoney;
    if (si.family() == kFamReagent || ends(n, "REAG")) return SortCategory::kReagent;
    if (has(n, "KEY")) return SortCategory::kKey;
    if (si.family() == kFamContainer) return SortCategory::kContainer;
    if (n == "POTION") return SortCategory::kPotion;
    if (ends(n, "FOCUS") || ends(n, "ITEM") || n == "GEMOFPRO" || n == "RECALL") return SortCategory::kMagic;
    if (has(n, "BOOK") || ends(n, "SCRL") || ends(n, "SCROL") || n == "GRIMOIRE") return SortCategory::kBook;
    if (has(n, "FOOD") || ends(n, "FOO") || n == "MOREFISH") return SortCategory::kFood;
    if (n == "DART" || n == "GRENADE" || n == "FGRENADE" || n == "FIREBOMB") return SortCategory::kWeapon;
    return SortCategory::kMisc;
}

// ---------------------------------------------------------------------------------------------
// Merging

bool sort_is_stack(const World& w, const Item& it) {
    unsigned fam = w.data().typeflags.get(it.shape).family();
    return fam == kFamQuantity || fam == kFamReagent;
}

bool sort_can_merge(const World& w, const Item& a, const Item& b) {
    if (a.shape != b.shape || a.id == b.id) return false;
    if ((a.flags & kItemOwned) != (b.flags & kItemOwned)) return false;
    unsigned fam = w.data().typeflags.get(a.shape).family();
    if (fam == kFamQuantity) return true;
    if (fam != kFamReagent) return false;
    auto kind = [&](unsigned f) -> int {
        // Reagent frames counted as one kind (world/items.md "Stacks").
        static const int necro[][2] = {{0, 5}, {6, 7}, {10, 12}, {14, 15}, {16, 20}};
        static const int sorc[][2] = {{0, 1}, {2, 5}, {6, 9}, {10, 13}, {14, 17}, {18, 20}};
        if (a.shape == 395)
            for (int i = 0; i < 5; ++i) if (int(f) >= necro[i][0] && int(f) <= necro[i][1]) return 100 + i;
        if (a.shape == 398)
            for (int i = 0; i < 6; ++i) if (int(f) >= sorc[i][0] && int(f) <= sorc[i][1]) return 200 + i;
        return int(f);
    };
    if (necro_elbow_apart(a.shape, a.frame, b.frame)) return false;  // Vividos' elbow (game/story_fixes.h)
    return kind(a.frame) == kind(b.frame);
}

// ---------------------------------------------------------------------------------------------
// Layout

SortBox sort_box(const ShapeFrame* f, bool flipped) {
    SortBox b;
    b.frame = f;
    b.flipped = flipped;
    if (!f) return b;
    b.w = f->w;
    b.h = f->h;
    // Surface8::draw: column c goes to x - xoff + c, or x + xoff - c when mirrored.
    b.left_off = flipped ? f->xoff - (f->w - 1) : -f->xoff;
    b.top_off = -f->yoff;
    return b;
}

bool SortBox::opaque(int bx, int by) const {
    return frame && frame->opaque(flipped ? w - 1 - bx : bx, by);
}

const char* sort_mode_name(SortLayout::Mode m) {
    switch (m) {
    case SortLayout::kShelf: return "shelf";
    case SortLayout::kFirstFit: return "first-fit";
    case SortLayout::kDense: return "dense";
    case SortLayout::kOverlap: return "overlap";
    }
    return "?";
}

namespace {

struct Rect { int x, y, w, h; };

bool apart(const Rect& a, const Rect& b, int gap) {
    return a.x + a.w + gap <= b.x || b.x + b.w + gap <= a.x || a.y + a.h + gap <= b.y || b.y + b.h + gap <= a.y;
}

// Shelf rows in reading order.
bool shelf(const std::vector<SortBox>& it, int W, int H, int gap, std::vector<Rect>& out) {
    out.assign(it.size(), Rect{0, 0, 0, 0});
    int x = 0, y = 0, row = 0;
    for (size_t i = 0; i < it.size(); ++i) {
        const SortBox& b = it[i];
        if (b.w > W || b.h > H) return false;
        if (x > 0 && x + b.w > W) { y += row + gap; x = 0; row = 0; }
        if (y + b.h > H) return false;
        out[i] = {x, y, b.w, b.h};
        x += b.w + gap;
        row = std::max(row, b.h);
    }
    return true;
}

// Each item at the first free spot scanning rows top to bottom, left to right (columns left
// to right, top to bottom when `columns`).
bool first_fit(const std::vector<SortBox>& it, const std::vector<size_t>& order, int W, int H, int gap,
               bool columns, std::vector<Rect>& out) {
    out.assign(it.size(), Rect{0, 0, 0, 0});
    std::vector<Rect> placed;
    for (size_t i : order) {
        const SortBox& b = it[i];
        if (b.w > W || b.h > H) return false;
        bool done = false;
        int outer = columns ? W - b.w : H - b.h, inner = columns ? H - b.h : W - b.w;
        for (int a = 0; a <= outer && !done; ++a)
            for (int c = 0; c <= inner; ++c) {
                Rect r = columns ? Rect{a, c, b.w, b.h} : Rect{c, a, b.w, b.h};
                bool ok = true;
                for (const Rect& p : placed)
                    if (!apart(r, p, gap)) {
                        ok = false;
                        c = std::max(c, (columns ? p.y + p.h : p.x + p.w) + gap - 1);  // skip past it
                        break;
                    }
                if (ok) { out[i] = r; placed.push_back(r); done = true; break; }
            }
        if (!done) return false;
    }
    return true;
}

// MaxRects with the best-short-side-fit rule: the tightest packer here (groups are not kept
// in reading order).
bool max_rects(const std::vector<SortBox>& it, const std::vector<size_t>& order, int W, int H, int gap,
               std::vector<Rect>& out) {
    out.assign(it.size(), Rect{0, 0, 0, 0});
    std::vector<Rect> free{{0, 0, W + gap, H + gap}};
    for (size_t i : order) {
        int w = it[i].w + gap, h = it[i].h + gap;
        int best = -1, bs = 1 << 30, bl = 1 << 30;
        for (size_t f = 0; f < free.size(); ++f) {
            const Rect& r = free[f];
            if (w > r.w || h > r.h) continue;
            int s = std::min(r.w - w, r.h - h), l = std::max(r.w - w, r.h - h);
            if (s < bs || (s == bs && l < bl) ||
                (s == bs && l == bl && best >= 0 && (r.y < free[size_t(best)].y || (r.y == free[size_t(best)].y && r.x < free[size_t(best)].x)))) {
                best = int(f); bs = s; bl = l;
            }
        }
        if (best < 0) return false;
        Rect p{free[size_t(best)].x, free[size_t(best)].y, w, h};
        out[i] = {p.x, p.y, it[i].w, it[i].h};
        std::vector<Rect> next;
        for (const Rect& r : free) {
            if (p.x >= r.x + r.w || p.x + p.w <= r.x || p.y >= r.y + r.h || p.y + p.h <= r.y) { next.push_back(r); continue; }
            if (p.x > r.x) next.push_back({r.x, r.y, p.x - r.x, r.h});
            if (p.x + p.w < r.x + r.w) next.push_back({p.x + p.w, r.y, r.x + r.w - p.x - p.w, r.h});
            if (p.y > r.y) next.push_back({r.x, r.y, r.w, p.y - r.y});
            if (p.y + p.h < r.y + r.h) next.push_back({r.x, p.y + p.h, r.w, r.y + r.h - p.y - p.h});
        }
        free.clear();
        for (size_t a = 0; a < next.size(); ++a) {
            bool inside = false;
            for (size_t b = 0; b < next.size() && !inside; ++b) {
                if (a == b) continue;
                const Rect &x = next[a], &y = next[b];
                bool contained = x.x >= y.x && x.y >= y.y && x.x + x.w <= y.x + y.w && x.y + x.h <= y.y + y.h;
                if (contained && (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h || a > b)) inside = true;
            }
            if (!inside) free.push_back(next[a]);
        }
    }
    return true;
}

// The fallback when nothing fits side by side. Items are drawn back to front largest first
// (small ones on top stay whole), each near its spot in a squeezed shelf layout (keeps the
// groups together). A spot is scored, in this order, by: how many items would keep less than
// half of their opaque pixels or no clear 8x8 patch (fewer is better), the smallest visible
// fraction of any item (larger is better), then the pixels it covers plus the distance from
// its home spot. Items are placed greedily, then each is moved again while that improves the
// score (a few rounds).
void overlap_layout(const std::vector<SortBox>& it, int W, int H, SortLayout& L) {
    const size_t n = it.size();
    // Home spots: shelf rows over an unbounded height, squeezed into H.
    std::vector<int> hx(n), hy(n);
    {
        int x = 0, y = 0, row = 0;
        for (size_t i = 0; i < n; ++i) {
            const SortBox& b = it[i];
            if (x > 0 && x + b.w > W) { y += row + 1; x = 0; row = 0; }
            hx[i] = x;
            hy[i] = y;
            x += b.w + 1;
            row = std::max(row, b.h);
        }
        int total = std::max(1, y + row);
        for (size_t i = 0; i < n; ++i) {
            hx[i] = std::clamp(hx[i], 0, std::max(0, W - it[i].w));
            hy[i] = std::clamp(total > H ? hy[i] * std::max(0, H - it[i].h) / std::max(1, total - it[i].h) : hy[i], 0,
                               std::max(0, H - it[i].h));
        }
    }
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return it[a].w * it[a].h > it[b].w * it[b].h; });
    std::vector<int> rank(n);
    for (size_t r = 0; r < n; ++r) rank[order[r]] = int(r);
    std::vector<std::vector<std::pair<int, int>>> px(n);  // opaque pixels per item
    std::vector<int> tot(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for (int y = 0; y < it[i].h; ++y)
            for (int x = 0; x < it[i].w; ++x)
                if (it[i].opaque(x, y)) px[i].push_back({x, y});
        tot[i] = int(px[i].size());
    }
    L.left.assign(n, 0);
    L.top.assign(n, 0);
    std::vector<bool> placed(n, false);
    std::vector<int> owner(size_t(W) * size_t(H), -1), vis(n, 0);
    // The top-most opaque item per pixel and the visible pixels of each, for the placed items
    // except `skip`.
    auto paint = [&](size_t skip) {
        std::fill(owner.begin(), owner.end(), -1);
        std::fill(vis.begin(), vis.end(), 0);
        for (size_t k : order) {
            if (!placed[k] || k == skip) continue;
            for (auto [x0, y0] : px[k]) {
                int ax = L.left[k] + x0, ay = L.top[k] + y0;
                if (ax < 0 || ay < 0 || ax >= W || ay >= H) continue;
                int& o = owner[size_t(ay) * size_t(W) + size_t(ax)];
                if (o >= 0) --vis[size_t(o)];
                o = int(k);
                ++vis[k];
            }
        }
    };
    auto top_at = [&](int ax, int ay) { return ax < 0 || ay < 0 || ax >= W || ay >= H ? -2 : owner[size_t(ay) * size_t(W) + size_t(ax)]; };
    auto bad_frac = [&](int v, int t) { return t > 0 && v * 2 < t; };
    // A clear patch of item o (kw x kh, min 8 x 8) holding >= 1/3 of o's own visible pixels,
    // with candidate k at (cx, cy) on top of the painted map (k not painted; k = n: none).
    // o may be k.
    std::vector<int> blk, own;
    auto patch_ok = [&](size_t o, size_t k, int cx, int cy) {
        const SortBox& b = it[o];
        int ox = o == k ? cx : L.left[o], oy = o == k ? cy : L.top[o];
        int kw = std::min(8, b.w), kh = std::min(8, b.h), bw = b.w, bh = b.h;
        size_t sz = size_t(bw + 1) * size_t(bh + 1);
        blk.assign(sz, 0);
        own.assign(sz, 0);
        auto at = [&](std::vector<int>& v, int x, int y) -> int& { return v[size_t(y) * size_t(bw + 1) + size_t(x)]; };
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                int ax = ox + x, ay = oy + y;
                int t = top_at(ax, ay);
                bool blocked = t == -2 || (t >= 0 && size_t(t) != o && rank[size_t(t)] > rank[o]);
                if (k < n && o != k && rank[k] > rank[o] && it[k].opaque(ax - cx, ay - cy) && ax - cx < it[k].w && ay - cy < it[k].h && ax >= cx && ay >= cy)
                    blocked = true;
                bool mine = !blocked && b.opaque(x, y);
                at(blk, x + 1, y + 1) = int(blocked) + at(blk, x, y + 1) + at(blk, x + 1, y) - at(blk, x, y);
                at(own, x + 1, y + 1) = int(mine) + at(own, x, y + 1) + at(own, x + 1, y) - at(own, x, y);
            }
        for (int y = 0; y + kh <= bh; ++y)
            for (int x = 0; x + kw <= bw; ++x) {
                if (at(blk, x + kw, y + kh) - at(blk, x, y + kh) - at(blk, x + kw, y) + at(blk, x, y)) continue;
                int m = at(own, x + kw, y + kh) - at(own, x, y + kh) - at(own, x + kw, y) + at(own, x, y);
                if (m * 3 >= kw * kh) return true;
            }
        return false;
    };
    struct Score {
        int bad = 0;          // items under half visible or without a clear patch
        double minf = 1.0;    // smallest visible fraction
        int cost = 0;         // covered pixels * 4 + distance from home
        int x = 0, y = 0;
        bool operator<(const Score& o) const {
            if (bad != o.bad) return bad < o.bad;
            if (minf != o.minf) return minf > o.minf;
            if (cost != o.cost) return cost < o.cost;
            if (y != o.y) return y < o.y;
            return x < o.x;
        }
    };
    std::vector<int> cover(n, 0), touched;
    // Best spot for k against the painted map of the others.
    auto place = [&](size_t k, bool have_current) {
        int base_bad = 0;
        double base_min = 1.0;
        for (size_t o = 0; o < n; ++o)
            if (placed[o] && o != k) {
                if (bad_frac(vis[o], tot[o])) ++base_bad;
                if (tot[o]) base_min = std::min(base_min, double(vis[o]) / tot[o]);
            }
        const SortBox& b = it[k];
        auto eval = [&](int x, int y) {
            Score s;
            s.x = x;
            s.y = y;
            touched.clear();
            int over = 0, mine = 0;
            for (auto [x0, y0] : px[k]) {
                int t = top_at(x + x0, y + y0);
                if (t == -2) continue;
                if (t >= 0 && rank[size_t(t)] > rank[k]) continue;  // hidden under a smaller item
                ++mine;
                if (t < 0) continue;
                ++over;
                if (cover[size_t(t)]++ == 0) touched.push_back(t);
            }
            s.bad = base_bad;
            s.minf = base_min;
            for (int o : touched) {
                int before = vis[size_t(o)], after = before - cover[size_t(o)];
                if (bad_frac(after, tot[size_t(o)]) && !bad_frac(before, tot[size_t(o)])) ++s.bad;
                if (tot[size_t(o)]) s.minf = std::min(s.minf, double(after) / tot[size_t(o)]);
                cover[size_t(o)] = 0;
            }
            if (bad_frac(mine, tot[k])) ++s.bad;
            if (tot[k]) s.minf = std::min(s.minf, double(mine) / tot[k]);
            s.cost = over * 4 + std::abs(x - hx[k]) + std::abs(y - hy[k]);
            return s;
        };
        std::vector<Score> c;
        int xmax = std::max(0, W - b.w), ymax = std::max(0, H - b.h);
        for (int y = 0; y <= ymax; ++y)
            for (int x = 0; x <= xmax; ++x) c.push_back(eval(x, y));
        Score current;
        if (have_current) current = eval(L.left[k], L.top[k]);
        std::sort(c.begin(), c.end());
        // Clear patches, for the best few spots (and the current one).
        auto patches = [&](Score& s) {
            touched.clear();
            for (size_t o = 0; o < n; ++o)
                if (placed[o] && o != k) {
                    const SortBox& ob = it[o];
                    bool inter = !(s.x >= L.left[o] + ob.w || L.left[o] >= s.x + b.w || s.y >= L.top[o] + ob.h || L.top[o] >= s.y + b.h);
                    if (inter && !patch_ok(o, k, s.x, s.y)) ++s.bad;
                }
            if (!patch_ok(k, k, s.x, s.y)) ++s.bad;
        };
        Score best = c.front();
        bool first = true;
        for (size_t i = 0; i < c.size() && i < 32; ++i) {
            Score s = c[i];
            if (!first && s.bad >= best.bad && !(s.bad == best.bad && s.minf > best.minf)) break;
            patches(s);
            if (first || s < best) best = s;
            first = false;
        }
        if (have_current) {
            patches(current);
            if (!(best < current)) return false;
        }
        L.left[k] = best.x;
        L.top[k] = best.y;
        return true;
    };
    // Greedy, back to front.
    for (size_t k : order) {
        paint(k);
        place(k, false);
        placed[k] = true;
    }
    // Then move items while it helps.
    for (int round = 0; round < 3; ++round) {
        bool moved = false;
        for (size_t k : order) {
            paint(k);
            if (place(k, true)) moved = true;
        }
        if (!moved) break;
    }
    L.order = order;
    // Final check of every item.
    paint(n);
    L.covered = 0;
    L.min_visible = 1.0;
    L.all_clickable = true;
    for (size_t o = 0; o < n; ++o) {
        if (tot[o] == 0) continue;
        if (vis[o] < tot[o]) ++L.covered;
        double f = double(vis[o]) / double(tot[o]);
        L.min_visible = std::min(L.min_visible, f);
        if (f < 0.5 || (vis[o] < tot[o] && !patch_ok(o, n, 0, 0))) L.all_clickable = false;
    }
}

}  // namespace

SortLayout sort_layout(const std::vector<SortBox>& items, int W, int H) {
    SortLayout L;
    const size_t n = items.size();
    std::vector<size_t> reading(n);
    std::iota(reading.begin(), reading.end(), 0);
    std::vector<Rect> r;
    auto done = [&](SortLayout::Mode m, int gap, const std::vector<size_t>& order) {
        L.mode = m;
        L.gap = gap;
        L.left.resize(n);
        L.top.resize(n);
        for (size_t i = 0; i < n; ++i) { L.left[i] = r[i].x; L.top[i] = r[i].y; }
        L.order = order;
        return L;
    };
    for (int gap : {2, 1})
        if (shelf(items, W, H, gap, r)) return done(SortLayout::kShelf, gap, reading);
    for (int gap : {1, 0})
        for (bool columns : {false, true})
            if (first_fit(items, reading, W, H, gap, columns, r)) return done(SortLayout::kFirstFit, gap, reading);
    // Tight packing, first in the group order, then largest first by several measures (area,
    // height, width, longest side).
    for (int gap : {1, 0})
        if (max_rects(items, reading, W, H, gap, r)) return done(SortLayout::kDense, gap, reading);
    auto measure = [&](int k, size_t i) {
        const SortBox& b = items[i];
        return k == 0 ? b.w * b.h : k == 1 ? b.h * 1000 + b.w : k == 2 ? b.w * 1000 + b.h : std::max(b.w, b.h) * 1000 + std::min(b.w, b.h);
    };
    for (int gap : {1, 0})
        for (int k = 0; k < 4; ++k) {
            std::vector<size_t> big = reading;
            std::stable_sort(big.begin(), big.end(), [&](size_t a, size_t b) { return measure(k, a) > measure(k, b); });
            if (max_rects(items, big, W, H, gap, r) || first_fit(items, big, W, H, gap, false, r))
                return done(SortLayout::kDense, gap, reading);
        }
    L.mode = SortLayout::kOverlap;
    overlap_layout(items, W, H, L);
    return L;
}

// ---------------------------------------------------------------------------------------------
// Sorting a window

namespace {

bool shown(const Item& it) { return !(it.flags & (kItemEquipped | kItemInvisible)); }

void arrange(World& w, ContainerGump& g, SortResult& res) {
    Item* c = w.item(g.owner);
    if (!c) return;
    const GameData& d = w.data();
    std::vector<uint16_t> keep, items;
    for (uint16_t k : c->contents) {
        const Item* it = w.item(k);
        if (it && shown(*it) && d.shapes.frame(it->shape, it->frame)) items.push_back(k);
        else keep.push_back(k);
    }
    // Group, then largest first, then shape / frame / count / id for a stable result.
    struct Key { int cat, area; unsigned shape, frame, q; uint16_t id; };
    auto key = [&](uint16_t id) {
        const Item* it = w.item(id);
        const ShapeFrame* f = d.shapes.frame(it->shape, it->frame);
        return Key{int(sort_category(d, it->shape)), f->w * f->h, it->shape, it->frame, it->quality, id};
    };
    std::stable_sort(items.begin(), items.end(), [&](uint16_t a, uint16_t b) {
        Key x = key(a), y = key(b);
        if (x.cat != y.cat) return x.cat < y.cat;
        if (x.area != y.area) return x.area > y.area;
        if (x.shape != y.shape) return x.shape < y.shape;
        if (x.frame != y.frame) return x.frame < y.frame;
        if (x.q != y.q) return x.q > y.q;
        return x.id < y.id;
    });
    std::vector<SortBox> boxes;
    for (uint16_t k : items) {
        const Item* it = w.item(k);
        boxes.push_back(sort_box(d.shapes.frame(it->shape, it->frame), it->flags & kItemFlipped));
    }
    SortLayout L = sort_layout(boxes, g.area_w, g.area_h);
    std::vector<uint16_t> order = keep;
    for (size_t i : L.order) {
        Item* it = w.item(items[i]);
        int gx = std::clamp(L.left[i] - boxes[i].left_off, 0, 254);
        int gy = std::clamp(L.top[i] - boxes[i].top_off, 0, 254);
        it->y = int32_t((gy << 8) | gx);
        order.push_back(items[i]);
    }
    c->contents = order;
    res.items = int(items.size());
    res.mode = L.mode;
    res.covered = L.covered;
    res.all_clickable = L.all_clickable;
}

}  // namespace

SortResult sort_container(World& w, ContainerGump& g) {
    SortResult res;
    if (!g.can_sort()) { res.refused = "not sortable"; return res; }
    if (w.stasis) { res.refused = "stasis"; return res; }
    Item* c = w.item(g.owner);
    if (!c) { res.refused = "no container"; return res; }
    if (!w.can_reach(1, g.owner, 128)) { res.refused = "out of reach"; w.refuse("Too far away to sort it"); return res; }
    // 1. Merge stacks: each stack tops up the first earlier one of its kind (up to 666).
    std::vector<uint16_t> list = c->contents, changed, emptied;
    std::vector<unsigned> before;
    for (uint16_t k : list) before.push_back(w.item(k) ? w.item(k)->quality : 0);
    for (size_t i = 0; i < list.size(); ++i) {
        Item* a = w.item(list[i]);
        if (!a || !shown(*a) || !sort_is_stack(w, *a)) continue;
        for (size_t j = 0; j < i; ++j) {
            Item* t = w.item(list[j]);
            if (!t || !shown(*t) || std::find(emptied.begin(), emptied.end(), t->id) != emptied.end()) continue;
            if (!sort_can_merge(w, *t, *a) || t->quality >= kMaxStack) continue;
            unsigned qa = std::max<unsigned>(1, a->quality), qt = std::max<unsigned>(1, t->quality);
            unsigned moved = std::min(qa, kMaxStack - qt);
            t->quality = uint16_t(qt + moved);
            a->quality = uint16_t(qa - moved);
            if (std::find(changed.begin(), changed.end(), t->id) == changed.end()) changed.push_back(t->id);
            if (a->quality == 0) { emptied.push_back(a->id); break; }
        }
        // A stack that gave only part of itself changed too (its pile frame may differ).
        if (a->quality > 0 && a->quality != std::max<unsigned>(1, before[i]) &&
            std::find(changed.begin(), changed.end(), a->id) == changed.end())
            changed.push_back(a->id);
    }
    for (uint16_t id : emptied) {
        changed.erase(std::remove(changed.begin(), changed.end(), id), changed.end());
        w.destroy(id);
    }
    res.merged_away = int(emptied.size());
    // 2. The combine event on each stack whose count changed (as the engine does on a merge).
    g.sort_pids.clear();
    for (uint16_t id : changed)
        if (uint16_t pid = w.call_event(id, kEvCombine)) g.sort_pids.push_back(pid);
    g.sort_wait = g.sort_pids.empty() ? 0 : 60;
    // 3. Group and lay out.
    arrange(w, g, res);
    res.ok = true;
    PG_LOG("sort container %u: %d items, %d stacks merged away, layout %s%s", g.owner, res.items, res.merged_away,
           sort_mode_name(res.mode), res.mode == SortLayout::kOverlap ? (res.all_clickable ? " (all clickable)" : " (crowded)") : "");
    return res;
}

SortResult sort_container(World& w, uint16_t container) {
    if (auto* g = dynamic_cast<ContainerGump*>(w.gumps.container_gump(container)); g && g->can_sort())
        return sort_container(w, *g);
    SortResult r;
    r.refused = "no window";
    return r;
}

void sort_relayout(World& w, ContainerGump& g) {
    SortResult res;
    arrange(w, g, res);
}

ContainerGump* sort_target(World& w, int sx, int sy) {
    if (auto* g = dynamic_cast<ContainerGump*>(w.gumps.at(sx, sy)); g && g->can_sort()) return g;
    for (size_t i = w.gumps.list.size(); i-- > 0;)
        if (auto* g = dynamic_cast<ContainerGump*>(w.gumps.list[i].get()); g && !g->closed && g->can_sort()) return g;
    return nullptr;
}

}  // namespace pg
