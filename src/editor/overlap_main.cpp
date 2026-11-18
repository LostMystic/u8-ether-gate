// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// pagan_overlaps: where the maps of the stitched world's layout overlap, and how.
//   pagan_overlaps GAME LAYOUT.json OUTDIR [--no-pictures] [--cells FILE] [--roads FILE] [--box MAP X0 Y0 X1 Y1]
//
// The original maps each carry copies of their neighbours (Tenebrae's districts repeat large
// parts of each other), so placed together they cover the same ground twice. For every pair
// of maps in different groups on one layer, every 128-unit world cell that both maps put items
// in is classed:
//   same     - the same items (shape, frame, world x / y / z) in both: a harmless duplicate
//   extra    - one map has everything the other has there, plus more (a superset)
//   conflict - each has items the other lacks: two different versions of that ground
// Conflicting cells are grouped into clusters (touching cells), each described (what each map
// has there) and, unless --no-pictures, pictured as the editor draws it: map A alone, map B
// alone, both. For a walkable seam the report also checks a cut line (the line through the
// original crossing points): one map owns each side, so a conflict away from the line is
// settled by the cut and one at the line would still show. For a pair with no seam it gives
// how far one map would have to move to clear the other's own content.
// Output: OUTDIR/overlaps.md, overlaps.json, cells_<a>_<b>.png, cluster_<a>_<b>_<n>.png,
// overview_layer<L>.png.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <array>
#include <bitset>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "app/rgba_draw.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "editor/edit_mode.h"
#include "editor/editor_storage.h"
#include "gfx/font.h"
#include "json/json.hpp"
#include "world/atlas_layout.h"
#include "world/current_map.h"
#include "world/map_patches.h"
#include "world/world_layout.h"

namespace pg {
bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale);
}

using namespace pg;
using nlohmann::json;

namespace {

constexpr int kCell = 128;  // a ground tile's size: anchors of 128-unit tiles land in every cell

struct WItem {
    int x, y, z;
    uint16_t shape;
    uint8_t frame;
    uint8_t kind;  // 0 ground, 1 water, 2 wall / solid, 3 roof, 4 other
    bool nonfixed = false;  // from the nonfixed list (things that are picked up, moved, used)
    bool operator<(const WItem& o) const {
        return std::tie(x, y, z, shape, frame) < std::tie(o.x, o.y, o.z, o.shape, o.frame);
    }
    bool operator==(const WItem& o) const { return x == o.x && y == o.y && z == o.z && shape == o.shape && frame == o.frame; }
};
const char* kKindName[] = {"ground", "water", "wall/solid", "roof", "other"};

long long ck(int cx, int cy) { return ((long long)(cx + (1 << 20)) << 22) | (long long)(cy + (1 << 20)); }
int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

struct MapItems {
    unsigned map = 0;
    int group = -1, layer = 0, dx = 0, dy = 0, dz = 0;
    std::unordered_map<long long, std::vector<WItem>> cells;  // sorted per cell
    std::unordered_map<long long, int> eggs;  // eggs per cell (monster, teleport, usecode: the map's own life there)
    double sx = 0, sy = 0;
    size_t n = 0;
};

uint8_t kind_of(const ShapeInfo& si) {
    if (si.is(ShapeInfo::kRoof)) return 3;
    if (si.is(ShapeInfo::kSea)) return 1;
    if (si.is(ShapeInfo::kLand)) return 0;
    if (si.is(ShapeInfo::kSolid)) return 2;
    return 4;
}

// `mask`: the map's chunks this group places (a piece of a map, or the map without its pieces).
MapItems load_map(const GameData& d, unsigned m, const AtlasGroup& G, int gi, const std::bitset<64 * 64>& mask) {
    MapItems mi;
    mi.map = m;
    mi.group = gi;
    mi.layer = G.layer;
    mi.dx = G.dx;
    mi.dy = G.dy;
    mi.dz = G.dz;
    bool nf = false;
    auto add = [&](const ItemRecord& r) {
        const ShapeInfo& si = d.typeflags.get(r.shape);
        if (r.x >= 32768 || r.y >= 32768) return;
        if (!mask[size_t(r.y >> 9) * 64 + size_t(r.x >> 9)]) return;
        const unsigned fam = si.family();
        if (fam == kFamMonsterEgg || fam == kFamTeleportEgg || fam == kFamUsecodeEgg) {
            ++mi.eggs[ck(floordiv(int(r.x) + G.dx, kCell), floordiv(int(r.y) + G.dy, kCell))];
            return;
        }
        if (si.is(ShapeInfo::kEditor)) return;
        WItem w{int(r.x) + G.dx, int(r.y) + G.dy, int(r.z) + G.dz, r.shape, r.frame, kind_of(si), nf};
        mi.cells[ck(floordiv(w.x, kCell), floordiv(w.y, kCell))].push_back(w);
        mi.sx += w.x;
        mi.sy += w.y;
        ++mi.n;
    };
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(m));
    apply_map_fixes(m, fixed);
    for (const ItemRecord& r : fixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape;
                q.frame = g.frame;
                q.x = uint16_t(bx + 2 * g.x + 1);
                q.y = uint16_t(by + 2 * g.y + 1);
                q.z = uint8_t(std::min(255, r.z + g.z));
                add(q);
            }
        } else {
            add(r);
        }
    }
    std::vector<ItemRecord> nonfixed = parse_item_records(d.nonfixed.entry(m));
    map_patches::apply_moves(m, nonfixed);
    nf = true;
    for (const MapEntry& e : build_item_tree(nonfixed, d.typeflags))
        if (e.parent < 0 && d.typeflags.get(e.rec.shape).family() != kFamGlobEgg) add(e.rec);
    for (auto& [k, v] : mi.cells) std::sort(v.begin(), v.end());
    if (mi.n) { mi.sx /= double(mi.n); mi.sy /= double(mi.n); }
    return mi;
}

enum Cls : uint8_t { kNone = 0, kOwn, kSame, kExtraA, kExtraB, kRetiled, kConflict };

struct CellRes {
    int cx, cy;
    Cls cls;
    int na, nb, matched, retiled;
    int kinds_a[5], kinds_b[5];  // items left unpaired, by kind
    int zmin_a, zmax_a, zmin_b, zmax_b;
};

// Exact matches first (shape, frame, x, y, z); then the rest pair up as "retiled" when they are
// the same kind of thing at the same height (within 8) - ground with ground, water with water,
// roof with roof anywhere in the cell, walls / other things only within 32 units of each other:
// the same place drawn with other tiles. What is left on both sides is a conflict.
CellRes compare_cell(int cx, int cy, const std::vector<WItem>& A, const std::vector<WItem>& B) {
    CellRes r{};
    r.cx = cx;
    r.cy = cy;
    r.na = int(A.size());
    r.nb = int(B.size());
    r.zmin_a = r.zmin_b = 9999;
    r.zmax_a = r.zmax_b = -9999;
    std::vector<const WItem*> ra, rb;
    size_t i = 0, j = 0;
    while (i < A.size() || j < B.size()) {
        if (j >= B.size() || (i < A.size() && A[i] < B[j])) ra.push_back(&A[i++]);
        else if (i >= A.size() || B[j] < A[i]) rb.push_back(&B[j++]);
        else { ++r.matched; ++i; ++j; }
    }
    std::vector<bool> used(rb.size(), false);
    std::vector<const WItem*> la;
    for (const WItem* x : ra) {
        int best = -1;
        for (size_t q = 0; q < rb.size(); ++q) {
            const WItem* y = rb[q];
            if (used[q] || y->kind != x->kind || std::abs(y->z - x->z) > 8) continue;
            bool flat = x->kind <= 1 || x->kind == 3;
            if (!flat && (std::abs(y->x - x->x) > 32 || std::abs(y->y - x->y) > 32)) continue;
            best = int(q);
            break;
        }
        if (best >= 0) { used[size_t(best)] = true; ++r.retiled; } else la.push_back(x);
    }
    int left_b = 0;
    for (size_t q = 0; q < rb.size(); ++q)
        if (!used[q]) {
            ++left_b;
            ++r.kinds_b[rb[q]->kind];
            r.zmin_b = std::min(r.zmin_b, rb[q]->z);
            r.zmax_b = std::max(r.zmax_b, rb[q]->z);
        }
    for (const WItem* x : la) {
        ++r.kinds_a[x->kind];
        r.zmin_a = std::min(r.zmin_a, x->z);
        r.zmax_a = std::max(r.zmax_a, x->z);
    }
    int left_a = int(la.size());
    if (left_a && left_b) r.cls = kConflict;
    else if (r.retiled) r.cls = kRetiled;
    else r.cls = left_a == 0 && left_b == 0 ? kSame : left_b == 0 ? kExtraA : kExtraB;
    return r;
}

struct Cluster {
    int id = 0;
    std::vector<int> cells;  // indices into the pair's results
    int cx0 = 1 << 30, cy0 = 1 << 30, cx1 = -(1 << 30), cy1 = -(1 << 30);
    int ua = 0, ub = 0, matched = 0;
    int kinds_a[5] = {0}, kinds_b[5] = {0};
    int zmin_a = 9999, zmax_a = -9999, zmin_b = 9999, zmax_b = -9999;
    double wx = 0, wy = 0;  // centre (world)
    int at_cut = 0;         // cells within kCutBand of the cut line
    std::string side;       // "a", "b", "line" (seams)
    std::string picture;
};

struct PairRes {
    const MapItems* a = nullptr;
    const MapItems* b = nullptr;
    std::vector<CellRes> cells;
    int n_same = 0, n_extra_a = 0, n_extra_b = 0, n_retiled = 0, n_conflict = 0;
    // Alignment: the offset of b (world units, from where it is now) at which the most identical
    // items line up, and how many (0: no identical items anywhere near).
    int align_dx = 0, align_dy = 0, align_dz = 0, align_votes = 0, here_votes = 0;
    std::map<std::tuple<int, int, int>, int> votes;  // a point - b point of identical items
    std::vector<Cluster> clusters;
    const WorldSeam* seam = nullptr;
    // Cut line (seams): a point and the unit direction from a's side to b's.
    bool has_cut = false;
    double px = 0, py = 0, ux = 0, uy = 0;
    int conflicts_at_cut = 0, conflicts_a_side = 0, conflicts_b_side = 0;
    std::string cut_basis;
    double best_t = 0;  // the best cut's position along the axis (world units)
    int best_at = 0;    // conflicting cells near the best cut
    double line_cells = 0;  // the shared ground's width across the axis (cells)
    // Separation (no seam): shift of b in whole chunks along an axis that clears a's own content.
    int sep_chunks = -1;
    std::string sep_axis;
    std::string cells_png;
};

constexpr double kCutBand = 256;
constexpr double kSeamCutSlack = 1024;  // how far a seam's cut may move from its crossing points
constexpr int kAlignWindow = 640;  // the offsets searched for a better alignment (a declared seam is at most a few hundred off)  // within this of the cut line a conflict still shows at the join

std::string fmt(const char* f, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string kinds_text(const int k[5]) {
    std::string s;
    for (int i = 0; i < 5; ++i)
        if (k[i]) s += (s.empty() ? "" : ", ") + std::to_string(k[i]) + " " + kKindName[i];
    return s.empty() ? "nothing" : s;
}

std::string map_label(const WorldLayoutFile& l, unsigned m) {
    int g = l.atlas.group_of(m);
    std::string lab = g >= 0 ? l.atlas.groups[size_t(g)].label : "";
    // "Map 40 Central Tenebrae" -> "Central Tenebrae (40)"
    std::string pre = "Map " + std::to_string(m) + " ";
    if (lab.rfind(pre, 0) == 0) return lab.substr(pre.size()) + " (" + std::to_string(m) + ")";
    return lab.empty() ? "map " + std::to_string(m) : lab;
}

// --- Pictures ---------------------------------------------------------------------------------

struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    Img(int W, int H, uint32_t bg) : w(W), h(H), px(size_t(W) * H, bg) {}
    void put(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) px[size_t(y) * w + x] = c;
    }
    void blit(const std::vector<uint32_t>& s, int sw, int sh, int ox, int oy) {
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) put(ox + x, oy + y, s[size_t(y) * sw + x]);
    }
};

const uint32_t kColRetiled = 0xFF2A8A90u, kColSame = 0xFF2F6B3Au, kColExtra = 0xFFB89A2Cu, kColConflict = 0xFFE0302Au, kColBg = 0xFF101418u;

// An isometric cell map (as the editor and the game see the world): cell (cx, cy) as a small
// diamond, k world units per pixel across.
struct IsoMap {
    int x0, y0, x1, y1;  // cell bounds
    int k = 16;          // world units per horizontal pixel
    int w = 0, h = 0, ou = 0, ov = 0;
    void fit() {
        double wx0 = x0 * kCell, wy0 = y0 * kCell, wx1 = (x1 + 1) * kCell, wy1 = (y1 + 1) * kCell;
        double umin = (wx0 - wy1) / k, umax = (wx1 - wy0) / k, vmin = (wx0 + wy0) / (2.0 * k), vmax = (wx1 + wy1) / (2.0 * k);
        ou = int(std::floor(umin)) - 8;
        ov = int(std::floor(vmin)) - 8;
        w = int(std::ceil(umax)) - ou + 8;
        h = int(std::ceil(vmax)) - ov + 8;
    }
    void to_px(double wx, double wy, int& u, int& v) const {
        u = int(std::lround((wx - wy) / k)) - ou;
        v = int(std::lround((wx + wy) / (2.0 * k))) - ov;
    }
    template <class F> void raster(Img& im, F color_at) const {
        for (int v = 0; v < im.h; ++v)
            for (int u = 0; u < im.w; ++u) {
                double U = (u + ou) * double(k), V = (v + ov) * 2.0 * k;
                double wx = (V + U) / 2, wy = (V - U) / 2;
                uint32_t c = color_at(floordiv(int(std::floor(wx)), kCell), floordiv(int(std::floor(wy)), kCell));
                if (c) im.put(u, v, c);
            }
    }
};

void line(Img& im, int x0, int y0, int x1, int y1, uint32_t c) {
    int n = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    for (int i = 0; i <= n; ++i) im.put(x0 + (x1 - x0) * i / std::max(1, n), y0 + (y1 - y0) * i / std::max(1, n), c);
}

struct Fonts {
    std::vector<ShapeFont> f;
    const GameData* d = nullptr;
    void text(Img& im, int x, int y, const std::string& s, int scale = 1) const {
        if (f.empty()) return;
        RgbaCanvas c{im.px.data(), im.w, im.h, &d->palette};
        c.text(f.size() > 6 ? f[6] : f[0], x, y, s, scale);
    }
    void label(Img& im, int x, int y, const std::string& s, int scale = 1) const {
        if (f.empty()) return;
        RgbaCanvas c{im.px.data(), im.w, im.h, &d->palette};
        int tw = int(s.size()) * 7 * scale;
        c.shade(x - 3, y - 2, tw + 6, 14 * scale, 0xFF000000u, 170);
        c.text(f.size() > 6 ? f[6] : f[0], x, y, s, scale);
    }
};

// The editor, set to draw just the maps (picture mode) from the layout being checked.
struct Painter {
    EditMode m;
    Painter(const GameData& d, const std::string& layout_text) : m(d) {
        auto st = std::make_unique<MemoryStorage>();
        st->save_layout(layout_text, 0, [](const StorageResult&) {});
        m.set_storage(std::move(st));
        m.set_budget(-1);
        m.load_all();
        m.picture = true;
        m.show_grid = false;
    }
    // A picture of world point (x, y) on `layer` with only `maps`, 2^level native pixels per
    // canvas pixel; S output pixels per game pixel (canvas 320*S x 200*S).
    void shot(int x, int y, int layer, const std::set<unsigned>& maps, int level, int S, std::vector<uint32_t>& out, int& w, int& h) {
        m.only_maps = maps;
        m.show_world_point(x, y, level);
        m.layer = layer;
        m.tick();
        m.render(out, w, h, S);
    }
};

// --- Which map's version is the real one (--owners) ------------------------------------------
//
// Where two or more maps of one layer put items in the same 128-unit cell, one of them owns the
// cell and the others hide their scenery there in the stitched world (world_layout.json
// "hidden"). The owner is the map whose version is its own place rather than a copy of a
// neighbour's: the original maps carry a strip of their neighbours around their edges (seen past
// the teleporters, never walked on), drawn more plainly - bare ground and a wall where the real
// map has its houses, its furniture and its eggs. Per map and cell:
//   content  = the items of that map in the cell and the eight around it, weighted: ground or
//              water 0.25, wall / solid 0.6, other 1, roof 1.5, +2 for a nonfixed item (things
//              that are used, picked up, moved), 4 for an egg (monsters, teleporters, scripts);
//   distance = cells from the map's own ground (cells no other map of the layer covers),
//              walking through the map's covered cells;
//   score    = content - kOwnDistance x distance.
// Each cell goes to a map so that the total of (best score - its map's score) over the cells plus
// kOwnSeam per pair of touching cells kept by different maps is low (sweeps of the cheapest choice
// per cell): borders run where the maps agree. A map's owned cells that are not joined to its own ground (an island inside the
// other map's town) go to the map around them, so each map keeps one piece and crossings happen
// at its edge.
// --- Where a player can walk in a map (the original game) ------------------------------------
//
// A 32-unit grid over the map's own coordinates. Ground: the top of the highest land item over
// each square (fixed items, glob contents included). Blocked: a solid, non-land fixed item
// standing on that ground (base below ground + 32, top above ground + 8), and the trigger area of
// every teleporter (a step in there takes the player to another map, so what lies past it is never
// walked on in this map). The walk starts at the map's teleporter arrival eggs and at the arrival
// points of the layout's links into the map, and steps to a side neighbour with ground, unblocked,
// at most 24 up or down. Doors are nonfixed and open, so they never block. Returned: the reached
// squares as world 128-unit cells with how many of their 16 squares were reached.
struct MapReach {
    std::unordered_map<long long, int> cells;  // world cell -> reached 32-unit squares (1..16), walls around them included
    std::unordered_map<long long, int> ground;  // world cell -> 32-unit squares with this map's ground (0..16)
    int seeds = 0, squares = 0;
};

MapReach reach_map(const GameData& d, const WorldLayoutFile& L, const MapItems& mi) {
    constexpr int N = 1024, Q = 32;
    std::vector<int16_t> ground(size_t(N) * N, -1);
    std::vector<uint8_t> blocked(size_t(N) * N, 0);
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(mi.map));
    apply_map_fixes(mi.map, fixed);
    std::vector<ItemRecord> all;
    for (const ItemRecord& r : fixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape; q.frame = g.frame;
                q.x = uint16_t(bx + 2 * g.x + 1); q.y = uint16_t(by + 2 * g.y + 1);
                q.z = uint8_t(std::min(255, r.z + g.z));
                all.push_back(q);
            }
        } else {
            all.push_back(r);
        }
    }
    std::vector<ItemRecord> nonfixed = parse_item_records(d.nonfixed.entry(mi.map));
    map_patches::apply_moves(mi.map, nonfixed);
    auto each_square = [&](const ItemRecord& r, auto f) {
        const ShapeInfo& si = d.typeflags.get(r.shape);
        int xd = 0, yd = 0, zd = 0;
        si.footprint(r.flags & kItemFlipped, xd, yd, zd);
        int x0 = std::max(0, int(r.x) - std::max(xd, 1) + 1) / Q, x1 = std::min(N - 1, int(r.x) / Q);
        int y0 = std::max(0, int(r.y) - std::max(yd, 1) + 1) / Q, y1 = std::min(N - 1, int(r.y) / Q);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) f(size_t(y) * N + size_t(x), zd);
    };
    for (const ItemRecord& r : all) {
        if (r.x >= 32768 || r.y >= 32768) continue;
        const ShapeInfo& si = d.typeflags.get(r.shape);
        if (!si.is(ShapeInfo::kLand) || si.is(ShapeInfo::kEditor)) continue;
        each_square(r, [&](size_t i, int zd) { ground[i] = int16_t(std::max<int>(ground[i], r.z + zd)); });
    }
    for (const ItemRecord& r : all) {
        if (r.x >= 32768 || r.y >= 32768) continue;
        const ShapeInfo& si = d.typeflags.get(r.shape);
        if (!si.is(ShapeInfo::kSolid) || si.is(ShapeInfo::kLand) || si.is(ShapeInfo::kEditor)) continue;
        each_square(r, [&](size_t i, int zd) {
            if (ground[i] >= 0 && r.z < ground[i] + 32 && r.z + zd > ground[i] + 8) blocked[i] = 1;
        });
    }
    MapReach out;
    std::vector<size_t> q;
    std::vector<uint8_t> seen(size_t(N) * N, 0);
    auto seed = [&](int x, int y) {
        int gx = std::clamp(x / Q, 0, N - 1), gy = std::clamp(y / Q, 0, N - 1);
        // The nearest walkable square within 3 (an egg may sit on a wall's edge).
        for (int r = 0; r <= 3; ++r)
            for (int oy = -r; oy <= r; ++oy)
                for (int ox = -r; ox <= r; ++ox) {
                    int sx = gx + ox, sy = gy + oy;
                    if (sx < 0 || sy < 0 || sx >= N || sy >= N) continue;
                    size_t i = size_t(sy) * N + size_t(sx);
                    if (ground[i] < 0 || blocked[i] || seen[i]) continue;
                    seen[i] = 1;
                    q.push_back(i);
                    ++out.seeds;
                    return;
                }
    };
    auto teleporter_area = [&](const ItemRecord& r) {
        if (d.typeflags.get(r.shape).family() != kFamTeleportEgg || r.frame == 1) return;
        int rx = (r.npcnum >> 4) * Q, ry = (r.npcnum & 15) * Q;
        for (int y = std::max(0, (int(r.y) - ry) / Q); y <= std::min(N - 1, (int(r.y) + ry) / Q); ++y)
            for (int x = std::max(0, (int(r.x) - rx) / Q); x <= std::min(N - 1, (int(r.x) + rx) / Q); ++x) blocked[size_t(y) * N + size_t(x)] = 2;
    };
    for (const ItemRecord& r : fixed) teleporter_area(r);
    for (const ItemRecord& r : nonfixed) teleporter_area(r);
    auto arrival = [&](const ItemRecord& r) {
        if (d.typeflags.get(r.shape).family() == kFamTeleportEgg && r.frame == 1) seed(r.x, r.y);
    };
    for (const ItemRecord& r : fixed) arrival(r);
    for (const ItemRecord& r : nonfixed) arrival(r);
    for (const WorldLink& l : L.links)
        if (l.to_map == mi.map) seed(l.to[0], l.to[1]);
    for (size_t h = 0; h < q.size(); ++h) {
        const size_t i = q[h];
        const int x = int(i % N), y = int(i / N);
        for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            int nx = x + ox, ny = y + oy;
            if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
            size_t n = size_t(ny) * N + size_t(nx);
            if (seen[n] || ground[n] < 0 || blocked[n] || std::abs(ground[n] - ground[i]) > 24) continue;
            seen[n] = 1;
            q.push_back(n);
        }
    }
    out.squares = int(q.size());
    // The walls that bound the walked ground are this map's too (two squares into anything blocked
    // that stands on ground): a wall is never walked on, but it belongs to the side it encloses.
    {
        std::vector<size_t> edge;
        std::vector<size_t> front = q;
        for (int step = 0; step < 2; ++step) {
            std::vector<size_t> next;
            for (size_t i : front) {
                const int x = int(i % N), y = int(i / N);
                for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    int nx = x + ox, ny = y + oy;
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                    size_t n = size_t(ny) * N + size_t(nx);
                    if (seen[n] || ground[n] < 0 || blocked[n] != 1) continue;
                    seen[n] = 1;
                    next.push_back(n);
                }
            }
            edge.insert(edge.end(), next.begin(), next.end());
            front = std::move(next);
        }
        q.insert(q.end(), edge.begin(), edge.end());
    }
    for (size_t i = 0; i < size_t(N) * N; ++i)
        if (ground[i] >= 0) {
            int wx = int(i % N) * Q + Q / 2 + mi.dx, wy = int(i / N) * Q + Q / 2 + mi.dy;
            ++out.ground[ck(floordiv(wx, kCell), floordiv(wy, kCell))];
        }
    for (size_t i : q) {
        int wx = int(i % N) * Q + Q / 2 + mi.dx, wy = int(i / N) * Q + Q / 2 + mi.dy;
        ++out.cells[ck(floordiv(wx, kCell), floordiv(wy, kCell))];
    }
    return out;
}

constexpr double kOwnSeam = 4.0;     // points a border costs per cell side
constexpr double kOwnReach = 60.0;   // a player walks here in this map (plus 2 per walked square of 16)
constexpr double kOwnNear = 6.0;     // per cell from the map's walked ground
constexpr double kOwnContent = 0.3;  // per content point (a tie-break)

struct OwnCell {
    std::vector<int> maps;      // indices into `maps` covering the cell
    std::vector<double> score;  // per entry of maps
    int owner = -1;             // index into `maps`
    double margin = 0;
};

double item_weight(const WItem& w) {
    static const double kw[5] = {0.25, 0.25, 0.6, 1.5, 1.0};
    return kw[w.kind] + (w.nonfixed ? 2.0 : 0.0);
}

int run_owners(const GameData& d, const WorldLayoutFile& L, std::vector<MapItems>& maps, const Fonts& fonts,
               const std::string& out, const std::string& file, int only_layer, bool pictures) {
    WorldLayoutFile proposed = L;  // with the hidden cells found (the after pictures)
    proposed.hidden.clear();
    struct Spot { double x, y; int layer; std::string what; };
    std::vector<Spot> spots;
    json res = json::object();
    json hidden = json::array();
    json pairs_j = json::array();
    std::set<int> layers;
    for (const MapItems& m : maps) layers.insert(m.layer);
    for (int layer : layers) {
        if (only_layer != 9999 && layer != only_layer) continue;
        std::vector<int> mi;
        for (size_t i = 0; i < maps.size(); ++i)
            if (maps[i].layer == layer) mi.push_back(int(i));
        // Cells and who covers them.
        std::unordered_map<long long, std::vector<int>> cover;
        for (int i : mi)
            for (const auto& [k, v] : maps[size_t(i)].cells) cover[k].push_back(i);
        for (int i : mi)
            for (const auto& [k, n] : maps[size_t(i)].eggs)
                if (maps[size_t(i)].cells.count(k) == 0) cover[k].push_back(i);
        std::unordered_map<long long, OwnCell> shared;
        for (auto& [k, v] : cover)
            if (v.size() > 1) shared[k].maps = v;
        if (shared.empty()) continue;
        auto key_xy = [](long long k, int& cx, int& cy) {
            cx = int((k >> 22) - (1 << 20));
            cy = int((k & ((1 << 22) - 1)) - (1 << 20));
        };
        // Distance of each map's cells from its own ground (BFS through its covered cells).
        std::map<int, std::unordered_map<long long, int>> dist;
        for (int i : mi) {
            std::unordered_map<long long, int>& D = dist[i];
            std::vector<long long> q;
            const MapItems& m = maps[size_t(i)];
            auto covered = [&](long long k) { return m.cells.count(k) || m.eggs.count(k); };
            for (const auto& [k, v] : m.cells)
                if (!shared.count(k)) { D[k] = 0; q.push_back(k); }
            for (size_t h = 0; h < q.size(); ++h) {
                int cx, cy;
                key_xy(q[h], cx, cy);
                int dd = D[q[h]];
                for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    long long n = ck(cx + ox, cy + oy);
                    if (!covered(n) || D.count(n)) continue;
                    D[n] = dd + 1;
                    q.push_back(n);
                }
            }
        }
        // Where a player walks in each map (reach_map), and how far each cell is from that.
        std::map<int, MapReach> reach;
        std::map<int, std::unordered_map<long long, int>> rdist;
        for (int i : mi) {
            reach[i] = reach_map(d, L, maps[size_t(i)]);
            std::printf("owners: map %u: %d squares walkable from %d arrival points (%zu cells)\n", maps[size_t(i)].map,
                        reach[i].squares, reach[i].seeds, reach[i].cells.size());
            std::unordered_map<long long, int>& D = rdist[i];
            std::vector<long long> q;
            for (const auto& [k, n] : reach[i].cells) { D[k] = 0; q.push_back(k); }
            for (size_t h = 0; h < q.size(); ++h) {
                int cx, cy;
                key_xy(q[h], cx, cy);
                int dd = D[q[h]];
                if (dd >= 48) continue;
                for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    long long n = ck(cx + ox, cy + oy);
                    if (D.count(n)) continue;
                    D[n] = dd + 1;
                    q.push_back(n);
                }
            }
        }
        // Scores: a map a player walks in this cell beats one where nobody does (kOwnReach);
        // among those, the nearer walked ground (kOwnNear per cell), then the content.
        for (auto& [k, c] : shared) {
            int cx, cy;
            key_xy(k, cx, cy);
            for (int i : c.maps) {
                const MapItems& m = maps[size_t(i)];
                double content = 0;
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) {
                        long long n = ck(cx + ox, cy + oy);
                        if (auto it = m.cells.find(n); it != m.cells.end())
                            for (const WItem& w : it->second) content += item_weight(w);
                        if (auto it = m.eggs.find(n); it != m.eggs.end()) content += 4.0 * it->second;
                    }
                auto rc = reach[i].cells.find(k);
                const int walked = rc == reach[i].cells.end() ? 0 : rc->second;
                auto rd = rdist[i].find(k);
                const int near = rd == rdist[i].end() ? 60 : rd->second;
                c.score.push_back((walked ? kOwnReach + 2.0 * walked : 0.0) - kOwnNear * near + kOwnContent * content);
            }
            int best = 0;
            for (size_t j = 1; j < c.score.size(); ++j)
                if (c.score[j] > c.score[size_t(best)]) best = int(j);
            double second = -1e9;
            for (size_t j = 0; j < c.score.size(); ++j)
                if (int(j) != best) second = std::max(second, c.score[j]);
            c.owner = c.maps[size_t(best)];
            c.margin = (c.score[size_t(best)] - second) / std::max(1.0, std::fabs(c.score[size_t(best)]));
        }
        // Smoothing: each cell pays what its map's version is short of the best one there
        // (content points), plus kOwnSeam for each of its four neighbours kept by another map.
        // Repeated sweeps (each cell takes its cheapest map, the others fixed) settle the borders
        // where the maps agree - plain ground both draw the same - and never through a wall or a
        // house only one of them has (a border inside a wall showed pieces of both walls).
        auto label_at = [&](long long k, int& lab) {
            auto it = shared.find(k);
            if (it != shared.end()) { lab = it->second.owner; return true; }
            auto cv = cover.find(k);
            if (cv != cover.end() && cv->second.size() == 1) { lab = cv->second[0]; return true; }
            return false;
        };
        for (int sweep = 0; sweep < 30; ++sweep) {
            int changed = 0;
            for (auto& [k, c] : shared) {
                int cx, cy;
                key_xy(k, cx, cy);
                double best_s = *std::max_element(c.score.begin(), c.score.end());
                double best_e = 1e18;
                int best_m = c.owner;
                for (size_t j2 = 0; j2 < c.maps.size(); ++j2) {
                    double e = best_s - c.score[j2];
                    for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                        int lab;
                        if (label_at(ck(cx + ox, cy + oy), lab) && lab != c.maps[j2]) e += kOwnSeam;
                    }
                    if (e < best_e - 1e-9 || (std::fabs(e - best_e) < 1e-9 && c.maps[j2] == c.owner)) { best_e = e; best_m = c.maps[j2]; }
                }
                if (best_m != c.owner) { c.owner = best_m; ++changed; }
            }
            if (!changed) break;
        }
        // Islands: owned cells not joined to the owner's own ground go to the map around them.
        for (int round = 0; round < 6; ++round) {
            int changed = 0;
            std::unordered_map<long long, int> comp;
            for (auto& [k0, c0] : shared) {
                if (comp.count(k0)) continue;
                const int own = c0.owner;
                std::vector<long long> q{k0};
                comp[k0] = 1;
                bool joined = false;
                std::map<int, int> around;
                for (size_t h = 0; h < q.size(); ++h) {
                    int cx, cy;
                    key_xy(q[h], cx, cy);
                    for (auto [ox, oy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                        long long n = ck(cx + ox, cy + oy);
                        auto it = shared.find(n);
                        if (it != shared.end()) {
                            if (it->second.owner == own) {
                                if (!comp.count(n)) { comp[n] = 1; q.push_back(n); }
                            } else {
                                ++around[it->second.owner];
                            }
                            continue;
                        }
                        auto cv = cover.find(n);
                        if (cv == cover.end()) continue;
                        if (std::find(cv->second.begin(), cv->second.end(), own) != cv->second.end()) joined = true;
                        else ++around[cv->second[0]];
                    }
                }
                if (joined) continue;
                for (long long k : q) {
                    OwnCell& c = shared[k];
                    int bo = -1, bn = 0;
                    for (auto [o, n] : around)
                        if (n > bn && std::find(c.maps.begin(), c.maps.end(), o) != c.maps.end()) { bo = o; bn = n; }
                    if (bo >= 0 && bo != c.owner) { c.owner = bo; ++changed; }
                }
            }
            if (!changed) break;
        }
        // Hidden cells per map (its own 128-unit grid: a local cell hides when its middle lies in a
        // cell another map owns), and what of the map's own life is in them.
        std::map<int, std::set<std::pair<int, int>>> hide;
        std::map<int, std::set<std::pair<int, int>>> keep_ground;  // hidden, but its floor stays (the owner has none there)
        std::map<std::pair<int, int>, std::array<int, 4>> pair_stats;  // kept by a, kept by b, close calls, -
        for (auto& [k, c] : shared) {
            int cx, cy;
            key_xy(k, cx, cy);
            const long long kk = k;
            auto gr = [&](int i) {
                auto it = reach[i].ground.find(kk);
                return it == reach[i].ground.end() ? 0 : it->second;
            };
            for (int i : c.maps)
                if (i != c.owner) {
                    const MapItems& m = maps[size_t(i)];
                    int lx = floordiv(cx * kCell + kCell / 2 - m.dx, kCell), ly = floordiv(cy * kCell + kCell / 2 - m.dy, kCell);
                    hide[i].insert({lx, ly});
                    if (gr(c.owner) < 14 && gr(i) > gr(c.owner)) keep_ground[i].insert({lx, ly});
                }
            for (size_t a = 0; a < c.maps.size(); ++a)
                for (size_t b = a + 1; b < c.maps.size(); ++b) {
                    int ia = c.maps[a], ib = c.maps[b];
                    unsigned ma = maps[size_t(ia)].map, mb = maps[size_t(ib)].map;
                    if (ma > mb) { std::swap(ia, ib); std::swap(ma, mb); }
                    auto& st = pair_stats[{int(ma), int(mb)}];
                    if (c.owner == ia) ++st[0];
                    else if (c.owner == ib) ++st[1];
                    if (c.margin < 0.25) ++st[2];
                }
        }
        for (auto& [i, cells] : hide) {
            const MapItems& m = maps[size_t(i)];
            json rows = json::array();
            std::map<int, std::vector<int>> by_row;
            for (auto [x, y] : cells) by_row[y].push_back(x);
            int n_nonfixed = 0, n_eggs = 0;
            json kept_things = json::array();
            for (auto& [y, xs] : by_row) {
                std::sort(xs.begin(), xs.end());
                size_t a = 0;
                while (a < xs.size()) {
                    size_t b = a;
                    while (b + 1 < xs.size() && xs[b + 1] == xs[b] + 1) ++b;
                    rows.push_back({y, xs[a], xs[b]});
                    a = b + 1;
                }
                for (int x : xs) {
                    long long wk = ck(floordiv(x * kCell + kCell / 2 + m.dx, kCell), floordiv(y * kCell + kCell / 2 + m.dy, kCell));
                    if (auto it = m.cells.find(wk); it != m.cells.end())
                        for (const WItem& w : it->second)
                            if (w.nonfixed) {
                                ++n_nonfixed;
                                if (kept_things.size() < 40) kept_things.push_back({w.shape, w.frame, w.x - m.dx, w.y - m.dy, w.z - m.dz});
                            }
                    if (auto it = m.eggs.find(wk); it != m.eggs.end()) n_eggs += it->second;
                }
            }
            {
                WorldHidden wh;
                wh.map = m.map;
                for (const auto& r : rows) wh.rows.push_back({r[0].get<int>(), r[1].get<int>(), r[2].get<int>()});
                std::map<int, std::vector<int>> kg;
                for (auto [x, y] : keep_ground[i]) kg[y].push_back(x);
                for (auto& [y, xs] : kg) {
                    std::sort(xs.begin(), xs.end());
                    size_t a = 0;
                    while (a < xs.size()) {
                        size_t b = a;
                        while (b + 1 < xs.size() && xs[b + 1] == xs[b] + 1) ++b;
                        wh.ground_rows.push_back({y, xs[a], xs[b]});
                        a = b + 1;
                    }
                }
                wh.derived = "owners: a neighbour's version is the real one here (pagan_overlaps --owners)";
                proposed.hidden.push_back(std::move(wh));
            }
            hidden.push_back({{"map", m.map}, {"rows", rows}, {"cells", cells.size()}, {"nonfixed_left", n_nonfixed},
                              {"eggs_left", n_eggs}, {"nonfixed_sample", kept_things}});
            std::printf("owners: map %u hides %zu cells (%d nonfixed items and %d eggs stay there)\n", m.map, cells.size(), n_nonfixed, n_eggs);
        }
        for (auto& [ab, st] : pair_stats) {
            pairs_j.push_back({{"a", ab.first}, {"b", ab.second}, {"layer", layer}, {"kept_a", st[0]}, {"kept_b", st[1]}, {"close", st[2]}});
            std::printf("owners: %d/%d: %d cells kept by %d, %d by %d, %d close calls\n", ab.first, ab.second, st[0], ab.first, st[1],
                        ab.second, st[2]);
        }
        // Spots for the before / after pictures: per hiding map, its biggest pieces of hidden ground
        // where it drew something the owner does not (what the hiding changes on screen).
        for (int i : mi) {
            const MapItems& m = maps[size_t(i)];
            std::set<long long> h;
            for (auto& [k, c] : shared) {
                if (c.owner == i || std::find(c.maps.begin(), c.maps.end(), i) == c.maps.end()) continue;
                auto it = m.cells.find(k);
                auto ot = maps[size_t(c.owner)].cells.find(k);
                if (it == m.cells.end()) continue;
                if (ot != maps[size_t(c.owner)].cells.end()) {
                    CellRes r = compare_cell(0, 0, it->second, ot->second);
                    if (r.cls == kSame || r.cls == kExtraB) continue;  // nothing of its own would go
                }
                h.insert(k);
            }
            std::set<long long> seen;
            std::vector<std::pair<int, Spot>> comps;
            for (long long k0 : h) {
                if (seen.count(k0)) continue;
                std::vector<long long> q{k0};
                seen.insert(k0);
                double sx = 0, sy = 0;
                std::map<unsigned, int> owners_here;
                for (size_t a = 0; a < q.size(); ++a) {
                    int cx, cy;
                    key_xy(q[a], cx, cy);
                    sx += cx;
                    sy += cy;
                    ++owners_here[maps[size_t(shared[q[a]].owner)].map];
                    for (int oy = -2; oy <= 2; ++oy)
                        for (int ox = -2; ox <= 2; ++ox) {
                            long long n = ck(cx + ox, cy + oy);
                            if (h.count(n) && !seen.count(n)) { seen.insert(n); q.push_back(n); }
                        }
                }
                unsigned bo = 0;
                int bn = 0;
                for (auto [o, n] : owners_here)
                    if (n > bn) { bo = o; bn = n; }
                Spot sp{(sx / q.size() + 0.5) * kCell, (sy / q.size() + 0.5) * kCell, layer,
                        map_label(L, m.map) + " hides " + std::to_string(q.size()) + " cells for " + map_label(L, bo)};
                comps.push_back({int(q.size()), sp});
            }
            std::sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t c = 0; c < comps.size() && c < 4; ++c) {
                if (comps[c].first < 6) break;
                bool near = false;
                for (const Spot& o : spots)
                    if (std::hypot(o.x - comps[c].second.x, o.y - comps[c].second.y) < 1200) near = true;
                if (!near) spots.push_back(comps[c].second);
            }
        }
        // A picture of the layer's shared ground by owner.
        IsoMap im;
        im.x0 = im.y0 = 1 << 30;
        im.x1 = im.y1 = -(1 << 30);
        for (auto& [k, c] : shared) {
            int cx, cy;
            key_xy(k, cx, cy);
            im.x0 = std::min(im.x0, cx - 16); im.y0 = std::min(im.y0, cy - 16);
            im.x1 = std::max(im.x1, cx + 16); im.y1 = std::max(im.y1, cy + 16);
        }
        int span = std::max(im.x1 - im.x0, im.y1 - im.y0) * kCell;
        im.k = std::max(8, span / 1400);
        im.fit();
        Img pic(im.w, im.h + 24 + 16 * int(mi.size() / 4 + 1), kColBg);
        auto colour = [&](int i) -> uint32_t {
            static const uint32_t pal[] = {0xFF5AB4F0u, 0xFF78F0A0u, 0xFFF0C850u, 0xFFC8A0FFu, 0xFFFF8CC8u, 0xFFFFFF78u, 0xFFFF6E5Au,
                                           0xFF5AE6E6u, 0xFFC8C8C8u, 0xFFB4DC78u, 0xFFE6A064u, 0xFF8C8CFFu};
            size_t pos = size_t(std::find(mi.begin(), mi.end(), i) - mi.begin());
            return pal[pos % 12];  // (ABGR in memory: as the other pictures)
        };
        auto dim = [](uint32_t c, int f) {
            uint32_t r = (c & 0xFF) * f / 100, g = ((c >> 8) & 0xFF) * f / 100, b = ((c >> 16) & 0xFF) * f / 100;
            return 0xFF000000u | (b << 16) | (g << 8) | r;
        };
        im.raster(pic, [&](int cx, int cy) -> uint32_t {
            long long k = ck(cx, cy);
            auto it = shared.find(k);
            if (it != shared.end()) return it->second.margin < 0.25 ? dim(colour(it->second.owner), 75) : colour(it->second.owner);
            auto cv = cover.find(k);
            if (cv != cover.end()) return dim(colour(cv->second[0]), 35);
            return 0;
        });
        int lx = 6, ly = im.h + 4;
        fonts.label(pic, 6, ly, "Shared ground by the map kept there (bright; darker = close call). Dim: a map's own ground.");
        ly += 18;
        for (int i : mi) {
            const MapItems& m = maps[size_t(i)];
            bool any = false;
            for (auto& [k, c] : shared)
                if (std::find(c.maps.begin(), c.maps.end(), i) != c.maps.end()) { any = true; break; }
            if (!any) continue;
            std::string t = map_label(L, m.map);
            for (int yy = 0; yy < 10; ++yy)
                for (int xx = 0; xx < 10; ++xx) pic.put(lx + xx, ly + yy, colour(i));
            fonts.label(pic, lx + 14, ly - 1, t);
            lx += 14 + int(t.size()) * 7 + 18;
            if (lx > pic.w - 260) { lx = 6; ly += 16; }
        }
        std::string name = fmt("owners_layer%d.png", layer);
        save_png(out + "/" + name, pic.px.data(), pic.w, pic.h, 1);
        std::printf("wrote %s/%s\n", out.c_str(), name.c_str());
    }
    res["hidden"] = hidden;
    res["pairs"] = pairs_j;
    if (pictures && !spots.empty()) {
        // Before: as the layout has it now; after: with the hidden cells (a fresh editor for each, so
        // nothing drawn is cached across).
        std::vector<std::vector<uint32_t>> before(spots.size());
        int w = 0, h = 0;
        const int S = 2;
        std::set<unsigned> all;
        for (const MapItems& m : maps) all.insert(m.map);
        {
            L.activate_hidden(nullptr);
            Painter pb(d, L.to_json());
            for (size_t i = 0; i < spots.size(); ++i)
                pb.shot(int(spots[i].x), int(spots[i].y), spots[i].layer, all, 1, S, before[i], w, h);
        }
        {
            Painter pa(d, proposed.to_json());
            json shots = json::array();
            for (size_t i = 0; i < spots.size(); ++i) {
                std::vector<uint32_t> after;
                int w2 = 0, h2 = 0;
                pa.shot(int(spots[i].x), int(spots[i].y), spots[i].layer, all, 1, S, after, w2, h2);
                Img row(2 * w + 4, h + 22, kColBg);
                row.blit(before[i], w, h, 0, 22);
                row.blit(after, w2, h2, w + 4, 22);
                fonts.label(row, 6, 4, "Now: " + spots[i].what);
                fonts.label(row, w + 10, 4, "With the hidden cells");
                std::string name = fmt("owners_spot_%02zu.png", i + 1);
                save_png(out + "/" + name, row.px.data(), row.w, row.h, 1);
                shots.push_back({{"picture", name}, {"world", {int(spots[i].x), int(spots[i].y)}}, {"what", spots[i].what}});
            }
            res["pictures"] = shots;
        }
        L.activate_hidden(nullptr);
        std::printf("wrote %zu before / after pictures\n", spots.size());
    }
    {
        std::string lp = out + "/world_layout.owners.json";
        std::ofstream(lp) << proposed.to_json();
        std::printf("wrote %s (the layout with the hidden cells)\n", lp.c_str());
    }
    res["rule"] = "content (items around the cell, weighted) - 0.5 x cells from the map's own ground; close calls follow their "
                  "neighbours; islands go to the map around them";
    std::ofstream(file) << res.dump(1) << "\n";
    std::printf("wrote %s\n", file.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: pagan_overlaps GAME LAYOUT.json OUTDIR [--no-pictures] [--cells FILE] [--roads FILE] [--box MAP X0 Y0 X1 Y1]\n");
        return 2;
    }
    bool pictures = true;
    for (int i = 4; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-pictures") == 0) pictures = false;
    std::string out = argv[3];
    mkdir(out.c_str(), 0755);
    GameFS fs;
    if (!fs.open(argv[1])) { std::fprintf(stderr, "game folder not found: %s\n", argv[1]); return 2; }
    GameData d;
    if (!d.load(fs)) { std::fprintf(stderr, "load: %s\n", d.error().c_str()); return 2; }
    std::string layout_text;
    {
        std::ifstream f(argv[2], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        layout_text = ss.str();
    }
    WorldLayoutFile L;
    std::string err;
    if (!L.from_json(layout_text, err)) { std::fprintf(stderr, "%s: %s\n", argv[2], err.c_str()); return 2; }
    // --roads FILE: the stitched world's roads (data/patches/world_roads.json) as part of their maps,
    // on as the game has them (the layout's road seams).
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--roads") == 0) {
            std::string e;
            if (!map_patches::load_roads_file(argv[i + 1], e)) { std::fprintf(stderr, "%s: %s\n", argv[i + 1], e.c_str()); return 2; }
            map_patches::set_active_roads(L.roads());
            std::printf("roads on: %zu\n", L.roads().size());
        }

    // Every placed map's items in world cells. A two-state place (16/26, 24/25) counts once: the
    // map shown first (its other state lies on the same spot by design).
    std::set<unsigned> second_state;
    for (const WorldVariant& v : L.variants) second_state.insert(v.when_nonzero);
    std::vector<MapItems> maps;
    for (size_t g = 0; g < L.atlas.groups.size(); ++g) {
        const AtlasGroup& G = L.atlas.groups[g];
        if (G.extra.find("\"unplaced\"") != std::string::npos) continue;
        for (unsigned m : G.maps)
            if (!second_state.count(m) && m < d.fixed.count()) maps.push_back(load_map(d, m, G, int(g), L.atlas.chunk_mask(g, m)));
    }
    std::printf("%zu maps loaded\n", maps.size());
    // --view X Y LAYER MAPS FILE [LEVEL]: one picture per map set (MAPS: "4;39;4,39"), side by side.
    for (int i = 4; i + 5 < argc; ++i)
        if (std::strcmp(argv[i], "--view") == 0) {
            const int vx = std::atoi(argv[i + 1]), vy = std::atoi(argv[i + 2]), vl = std::atoi(argv[i + 3]);
            const int lev = i + 6 < argc && argv[i + 6][0] != '-' ? std::atoi(argv[i + 6]) : 1;
            std::vector<std::set<unsigned>> sets;
            std::stringstream ss(argv[i + 4]);
            std::string part;
            while (std::getline(ss, part, ';')) {
                std::set<unsigned> st;
                std::stringstream ps(part);
                std::string n;
                while (std::getline(ps, n, ',')) st.insert(unsigned(std::atoi(n.c_str())));
                sets.push_back(st);
            }
            Fonts f;
            f.d = &d;
            for (unsigned q = 0; q < d.fonts.count(); ++q) f.f.emplace_back(d.fonts.shape(q), int(q));
            Painter pt(d, layout_text);
            std::vector<uint32_t> buf;
            int w = 0, h = 0;
            Img row(int(sets.size()) * 644, 422, kColBg);
            for (size_t k = 0; k < sets.size(); ++k) {
                pt.shot(vx, vy, vl, sets[k], lev, 2, buf, w, h);
                row.blit(buf, w, h, int(k) * (w + 4), 22);
                std::string t;
                for (unsigned m : sets[k]) t += (t.empty() ? "" : " + ") + map_label(L, m);
                f.label(row, int(k) * (w + 4) + 6, 4, t);
            }
            save_png(argv[i + 5], row.px.data(), row.w, row.h, 1);
            std::printf("wrote %s\n", argv[i + 5]);
            return 0;
        }
    // --owners FILE [--layer L]: which map keeps each shared cell (run_owners); nothing else.
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--owners") == 0) {
            int only = 9999;
            for (int j = 4; j + 1 < argc; ++j)
                if (std::strcmp(argv[j], "--layer") == 0) only = std::atoi(argv[j + 1]);
            // --maps A,B,...: only these maps take part (the others are left as they are).
            for (int j = 4; j + 1 < argc; ++j)
                if (std::strcmp(argv[j], "--maps") == 0) {
                    std::set<unsigned> keep;
                    std::stringstream ms(argv[j + 1]);
                    std::string n;
                    while (std::getline(ms, n, ',')) keep.insert(unsigned(std::atoi(n.c_str())));
                    maps.erase(std::remove_if(maps.begin(), maps.end(), [&](const MapItems& m) { return !keep.count(m.map); }), maps.end());
                }
            Fonts f;
            f.d = &d;
            for (unsigned q = 0; q < d.fonts.count(); ++q) f.f.emplace_back(d.fonts.shape(q), int(q));
            return run_owners(d, L, maps, f, out, argv[i + 1], only, pictures);
        }
    // --eggs MAP: the map's teleport and usecode eggs (local points), from the fixed and nonfixed lists.
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--eggs") == 0) {
            const unsigned em = unsigned(std::atoi(argv[i + 1]));
            for (int list = 0; list < 2; ++list) {
                std::vector<ItemRecord> v = parse_item_records(list ? d.nonfixed.entry(em) : d.fixed.entry(em));
                for (const ItemRecord& r : v) {
                    const unsigned fam = d.typeflags.get(r.shape).family();
                    if (fam != kFamTeleportEgg && fam != kFamUsecodeEgg) continue;
                    std::printf("eggs map %u %s: %s shape %u frame %u q %u npcnum %u mapnum %u at %d %d %d chunk %d,%d\n", em,
                                list ? "nonfixed" : "fixed", fam == kFamTeleportEgg ? "teleport" : "usecode", r.shape, r.frame,
                                r.quality, r.npcnum, r.mapnum, r.x, r.y, r.z, r.x / 512, r.y / 512);
                }
            }
            return 0;
        }
    // --box MAP X0 Y0 X1 Y1: list a map's visible items inside a world box (shape, frame, world and
    // local x / y / z, kind), for fitting new content to it.
    for (int i = 4; i + 5 < argc; ++i)
        if (std::strcmp(argv[i], "--box") == 0) {
            unsigned bm = unsigned(std::atoi(argv[i + 1]));
            int bx0 = std::atoi(argv[i + 2]), by0 = std::atoi(argv[i + 3]), bx1 = std::atoi(argv[i + 4]), by1 = std::atoi(argv[i + 5]);
            for (const MapItems& m : maps) {
                if (m.map != bm) continue;
                std::vector<WItem> v;
                for (const auto& [k, items] : m.cells)
                    for (const WItem& w : items)
                        if (w.x >= bx0 && w.x <= bx1 && w.y >= by0 && w.y <= by1) v.push_back(w);
                std::sort(v.begin(), v.end(), [](const WItem& a, const WItem& b) { return std::tie(a.y, a.x, a.z) < std::tie(b.y, b.x, b.z); });
                for (const WItem& w : v)
                    std::printf("box map %u: shape %4u frame %3u world %6d %6d %4d local %5d %5d %3d %s\n", bm, w.shape, w.frame, w.x, w.y,
                                w.z, w.x - m.dx, w.y - m.dy, w.z - m.dz, kKindName[w.kind]);
            }
        }
    // --cells FILE: every placed map's visible items per world cell (128 units), for planning moves
    // (a move by whole chunks shifts these cells by 4 per chunk).
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--cells") == 0) {
            json cj = json::object();
            for (const MapItems& m : maps) {
                json arr = json::array();
                for (const auto& [k, v] : m.cells)
                    arr.push_back({int((k >> 22) - (1 << 20)), int((k & ((1 << 22) - 1)) - (1 << 20)), v.size()});
                cj[std::to_string(m.map)] = {{"layer", m.layer}, {"offset", {m.dx, m.dy}}, {"cells", arr}};
            }
            std::ofstream(argv[i + 1]) << cj.dump() << "\n";
            std::printf("wrote %s\n", argv[i + 1]);
        }

    // Pairs: different groups, one layer, at least one shared cell.
    std::vector<PairRes> pairs;
    for (size_t i = 0; i < maps.size(); ++i)
        for (size_t j = i + 1; j < maps.size(); ++j) {
            const MapItems& A = maps[i];
            const MapItems& B = maps[j];
            if (A.group == B.group || A.layer != B.layer) continue;
            PairRes p;
            p.a = &A;
            p.b = &B;
            const MapItems& S = A.cells.size() < B.cells.size() ? A : B;
            const MapItems& O = &S == &A ? B : A;
            for (const auto& [k, v] : S.cells) {
                auto it = O.cells.find(k);
                if (it == O.cells.end()) continue;
                int cx = int((k >> 22) - (1 << 20)), cy = int((k & ((1 << 22) - 1)) - (1 << 20));
                const std::vector<WItem>& va = &S == &A ? v : it->second;
                const std::vector<WItem>& vb = &S == &A ? it->second : v;
                CellRes r = compare_cell(cx, cy, va, vb);
                p.cells.push_back(r);
                if (r.cls == kSame) ++p.n_same;
                else if (r.cls == kExtraA) ++p.n_extra_a;
                else if (r.cls == kExtraB) ++p.n_extra_b;
                else if (r.cls == kRetiled) ++p.n_retiled;
                else ++p.n_conflict;
            }
            if (p.cells.empty()) continue;
            if (const char* dm = std::getenv("PG_OVL_DUMP")) {
                unsigned da = 0, db = 0;
                if (std::sscanf(dm, "%u,%u", &da, &db) == 2 && da == A.map && db == B.map) {
                    int shown = 0;
                    for (const CellRes& r : p.cells) {
                        if (r.cls != kConflict || (shown++ % 400) != 0) continue;
                        std::printf("cell %d,%d: a %d b %d matched %d\n", r.cx, r.cy, r.na, r.nb, r.matched);
                        for (int side = 0; side < 2; ++side) {
                            const std::vector<WItem>& v = (side ? B : A).cells.at(ck(r.cx, r.cy));
                            for (const WItem& w : v) std::printf("   %c %5d %5d %3d  shape %4u frame %3u kind %s\n", side ? 'B' : 'A', w.x, w.y, w.z, w.shape, w.frame, kKindName[w.kind]);
                        }
                    }
                }
            }
            std::sort(p.cells.begin(), p.cells.end(), [](const CellRes& x, const CellRes& y) { return std::tie(x.cy, x.cx) < std::tie(y.cy, y.cx); });
            p.seam = L.seam(std::min(A.map, B.map), std::max(A.map, B.map));
            pairs.push_back(std::move(p));
        }
    std::sort(pairs.begin(), pairs.end(), [](const PairRes& x, const PairRes& y) {
        if (x.a->layer != y.a->layer) return x.a->layer > y.a->layer ? false : true;
        return x.cells.size() > y.cells.size();
    });

    // Alignment of each pair: identical items (shape, frame) of b near the shared ground voting
    // for the offset (dx, dy, dz) that would line them up with a's. Ground tiles that are
    // everywhere are left out (noise).
    for (PairRes& p : pairs) {
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (const CellRes& r : p.cells) { x0 = std::min(x0, r.cx); y0 = std::min(y0, r.cy); x1 = std::max(x1, r.cx); y1 = std::max(y1, r.cy); }
        x0 -= 16; y0 -= 16; x1 += 16; y1 += 16;
        auto gather = [&](const MapItems& m) {
            std::vector<const WItem*> v;
            for (const auto& [k, items] : m.cells) {
                int cx = int((k >> 22) - (1 << 20)), cy = int((k & ((1 << 22) - 1)) - (1 << 20));
                if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
                for (const WItem& w : items) v.push_back(&w);
            }
            return v;
        };
        std::vector<const WItem*> va = gather(*p.a), vb = gather(*p.b);
        std::unordered_map<uint32_t, std::vector<const WItem*>> idx;
        for (const WItem* w : va) idx[(uint32_t(w->shape) << 8) | w->frame].push_back(w);
        std::map<std::tuple<int, int, int>, int> votes;
        for (const WItem* w : vb) {
            auto it = idx.find((uint32_t(w->shape) << 8) | w->frame);
            if (it == idx.end() || it->second.size() > 200) continue;
            for (const WItem* q : it->second) {
                int ddx = q->x - w->x, ddy = q->y - w->y, ddz = q->z - w->z;
                if (std::abs(ddx) > kAlignWindow || std::abs(ddy) > kAlignWindow || std::abs(ddz) > 64) continue;
                ++votes[{ddx, ddy, ddz}];
            }
        }
        for (auto& [k, n] : votes)
            if (n > p.align_votes) { p.align_votes = n; std::tie(p.align_dx, p.align_dy, p.align_dz) = k; }
        if (std::getenv("PG_OVL_ALIGN")) {
            std::vector<std::pair<int, std::tuple<int, int, int>>> top;
            for (auto& [k, n] : votes) top.push_back({n, k});
            std::sort(top.rbegin(), top.rend());
            std::printf("align %u/%u:", p.a->map, p.b->map);
            for (size_t i = 0; i < top.size() && i < 6; ++i)
                std::printf("  %d@%+d,%+d,%+d", top[i].first, std::get<0>(top[i].second), std::get<1>(top[i].second), std::get<2>(top[i].second));
            std::printf("\n");
        }
        auto h = votes.find({0, 0, 0});
        p.here_votes = h == votes.end() ? 0 : h->second;
        p.votes = std::move(votes);
    }

    // Which maps are off, and by how much. A map's votes are summed over its pairs (moving it by
    // the offset lines its copies up with each neighbour's). Content that repeats every chunk
    // (fences, walls, tiled ground) votes for offsets 512 apart as well, so the pick is the
    // smallest offset among the strong ones with the best one's remainder (mod 512); a remainder
    // of 0 is that repetition and never counts. Greedy: the map with the fewest votes for where
    // it is now goes first; once it is moved, its pairs no longer count against its neighbours.
    struct MapMove { unsigned map; int dx, dy, dz, votes, here; std::string why; };
    std::vector<MapMove> moves;
    {
        std::set<unsigned> fixed_maps;  // accepted moves
        auto mod512 = [](int v) { int r = ((v % 512) + 512) % 512; return r > 256 ? r - 512 : r; };
        for (int round = 0; round < 8; ++round) {
            std::map<unsigned, std::map<std::tuple<int, int, int>, int>> mv;
            for (const PairRes& p : pairs) {
                if (fixed_maps.count(p.a->map) || fixed_maps.count(p.b->map)) continue;
                for (auto& [k, n] : p.votes) {
                    auto [ddx, ddy, ddz] = k;
                    mv[p.b->map][{ddx, ddy, ddz}] += n;
                    mv[p.a->map][{-ddx, -ddy, -ddz}] += n;
                }
            }
            MapMove pick{0, 0, 0, 0, 0, 1 << 30, ""};
            for (auto& [m, v] : mv) {
                if (fixed_maps.count(m)) continue;
                int best = 0;
                std::tuple<int, int, int> bk{0, 0, 0};
                for (auto& [k, n] : v)
                    if (n > best) { best = n; bk = k; }
                int rx = mod512(std::get<0>(bk)), ry = mod512(std::get<1>(bk)), rz = std::get<2>(bk);
                if (rx == 0 && ry == 0 && rz == 0) continue;  // repetition, or already lined up
                auto h = v.find({0, 0, 0});
                int here = h == v.end() ? 0 : h->second;
                // The smallest strong offset with the same remainder.
                std::tuple<int, int, int> ck2 = bk;
                int cv = best;
                double cmag = std::hypot(std::get<0>(bk), std::get<1>(bk));
                for (auto& [k, n] : v) {
                    if (n * 10 < best * 6 || mod512(std::get<0>(k)) != rx || mod512(std::get<1>(k)) != ry || std::get<2>(k) != rz) continue;
                    double mag = std::hypot(std::get<0>(k), std::get<1>(k));
                    if (mag < cmag) { cmag = mag; ck2 = k; cv = n; }
                }
                if (cv < 100 || cv < 2 * here) continue;
                if (here < pick.here || (here == pick.here && cv > pick.votes))
                    pick = MapMove{m, std::get<0>(ck2), std::get<1>(ck2), std::get<2>(ck2), cv, here, ""};
            }
            if (!pick.map) break;
            std::string partners;
            for (const PairRes& p : pairs) {
                if (p.a->map != pick.map && p.b->map != pick.map) continue;
                if (fixed_maps.count(p.a->map) || fixed_maps.count(p.b->map)) continue;
                int sgn = p.b->map == pick.map ? 1 : -1;
                auto it = p.votes.find({sgn * pick.dx, sgn * pick.dy, sgn * pick.dz});
                if (it != p.votes.end() && it->second >= 20)
                    partners += (partners.empty() ? "" : ", ") + map_label(L, p.a->map == pick.map ? p.b->map : p.a->map) + " " +
                                std::to_string(it->second);
            }
            pick.why = partners;
            moves.push_back(pick);
            fixed_maps.insert(pick.map);
            std::printf("misaligned: %s off by %+d, %+d, %+d (%d identical items line up there, %d where it is; with %s)\n",
                        map_label(L, pick.map).c_str(), pick.dx, pick.dy, pick.dz, pick.votes, pick.here, partners.c_str());
        }
    }

    // The layout with those maps moved (and the seams to them re-derived from the new places):
    // OUTDIR/world_layout.aligned.json, to check and adopt.
    std::string aligned_file;
    if (!moves.empty()) {
        json j = json::parse(layout_text, nullptr, false, true);
        std::map<unsigned, std::pair<int, int>> goff;
        for (auto& g : j["groups"]) {
            for (const MapMove& mm : moves)
                for (auto& m : g["maps"])
                    if (m.get<unsigned>() == mm.map) {
                        g["offset"][0] = g["offset"][0].get<int>() + mm.dx;
                        g["offset"][1] = g["offset"][1].get<int>() + mm.dy;
                        g["derived"] = g.value("derived", "") +
                                       fmt(" Moved by %+d, %+d by the overlap report (pagan_overlaps): %d identical items line up with its "
                                           "neighbours there, the maps' own chunk grid.", mm.dx, mm.dy, mm.votes);
                        g["source"] = "data";
                    }
            for (auto& m : g["maps"]) goff[m.get<unsigned>()] = {g["offset"][0].get<int>(), g["offset"][1].get<int>()};
        }
        if (j.contains("seams"))
            for (auto& sm : j["seams"]) {
                unsigned a = sm["maps"][0].get<unsigned>(), b = sm["maps"][1].get<unsigned>();
                bool touched = false;
                for (const MapMove& mm : moves) touched |= mm.map == a || mm.map == b;
                if (!touched || !goff.count(a) || !goff.count(b)) continue;
                sm["offset"][0] = goff[b].first - goff[a].first;
                sm["offset"][1] = goff[b].second - goff[a].second;
                sm["derived"] = sm.value("derived", "") + " Offset re-derived after the overlap report's alignment.";
            }
        aligned_file = "world_layout.aligned.json";
        std::ofstream(out + "/" + aligned_file) << j.dump(1) << "\n";
    }

    // A cell is a map's "own" content when no other map on its layer has the same or more there
    // (not a copy of someone else's ground).
    std::map<std::pair<unsigned, long long>, bool> copied;  // (map, cell) -> another map covers it at least as fully
    for (const PairRes& p : pairs)
        for (const CellRes& r : p.cells) {
            long long k = ck(r.cx, r.cy);
            if (r.cls == kSame) { copied[{p.a->map, k}] = true; copied[{p.b->map, k}] = true; }
            if (r.cls == kExtraA) copied[{p.b->map, k}] = true;
            if (r.cls == kExtraB) copied[{p.a->map, k}] = true;
        }
    auto own_cells = [&](const MapItems& m) {
        std::set<long long> s;
        for (const auto& [k, v] : m.cells)
            if (!copied.count({m.map, k})) s.insert(k);
        return s;
    };

    Fonts fonts;
    fonts.d = &d;
    for (unsigned i = 0; i < d.fonts.count(); ++i) fonts.f.emplace_back(d.fonts.shape(i), int(i));
    std::unique_ptr<Painter> painter;
    if (pictures) painter = std::make_unique<Painter>(d, layout_text);

    for (PairRes& p : pairs) {
        const MapItems& A = *p.a;
        const MapItems& B = *p.b;
        // Clusters of conflicting cells (cells within 2 of each other join).
        std::map<long long, int> idx;
        for (size_t i = 0; i < p.cells.size(); ++i)
            if (p.cells[i].cls == kConflict) idx[ck(p.cells[i].cx, p.cells[i].cy)] = int(i);
        std::set<int> seen;
        for (auto& [k0, i0] : idx) {
            if (seen.count(i0)) continue;
            Cluster c;
            std::vector<int> st{i0};
            seen.insert(i0);
            while (!st.empty()) {
                int i = st.back();
                st.pop_back();
                c.cells.push_back(i);
                const CellRes& r = p.cells[size_t(i)];
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        auto it = idx.find(ck(r.cx + dx, r.cy + dy));
                        if (it != idx.end() && !seen.count(it->second)) { seen.insert(it->second); st.push_back(it->second); }
                    }
            }
            for (int i : c.cells) {
                const CellRes& r = p.cells[size_t(i)];
                c.cx0 = std::min(c.cx0, r.cx); c.cy0 = std::min(c.cy0, r.cy);
                c.cx1 = std::max(c.cx1, r.cx); c.cy1 = std::max(c.cy1, r.cy);
                c.ua += r.na - r.matched; c.ub += r.nb - r.matched; c.matched += r.matched;
                for (int q = 0; q < 5; ++q) { c.kinds_a[q] += r.kinds_a[q]; c.kinds_b[q] += r.kinds_b[q]; }
                c.zmin_a = std::min(c.zmin_a, r.zmin_a); c.zmax_a = std::max(c.zmax_a, r.zmax_a);
                c.zmin_b = std::min(c.zmin_b, r.zmin_b); c.zmax_b = std::max(c.zmax_b, r.zmax_b);
                c.wx += (r.cx + 0.5) * kCell; c.wy += (r.cy + 0.5) * kCell;
            }
            c.wx /= double(c.cells.size());
            c.wy /= double(c.cells.size());
            p.clusters.push_back(std::move(c));
        }
        std::sort(p.clusters.begin(), p.clusters.end(), [](const Cluster& x, const Cluster& y) { return x.ua + x.ub > y.ua + y.ub; });
        for (size_t i = 0; i < p.clusters.size(); ++i) p.clusters[i].id = int(i) + 1;

        // A cut line: one map owns the ground on its side of it, the other the rest. It runs
        // across the main axis between the two maps' own content (Tenebrae's districts join along
        // the world axes). A seam's cut goes through its crossing points (the original
        // teleporters); for every pair the report also finds the position along the axis with the
        // fewest conflicting cells near the line (best_cut).
        {
            auto centre = [&](const MapItems& m, double& cx, double& cy) {
                std::set<long long> own = own_cells(m);
                double n = 0;
                cx = cy = 0;
                for (long long k : own) {
                    cx += double(int((k >> 22) - (1 << 20))) + 0.5;
                    cy += double(int((k & ((1 << 22) - 1)) - (1 << 20))) + 0.5;
                    ++n;
                }
                if (n > 0) { cx = cx / n * kCell; cy = cy / n * kCell; } else { cx = m.sx; cy = m.sy; }
            };
            double ax, ay, bx, by;
            centre(A, ax, ay);
            centre(B, bx, by);
            double vx = bx - ax, vy = by - ay;
            if (std::fabs(vx) >= std::fabs(vy)) { p.ux = vx > 0 ? 1 : -1; p.uy = 0; } else { p.ux = 0; p.uy = vy > 0 ? 1 : -1; }
            p.has_cut = std::hypot(vx, vy) > 0;
            auto along = [&](const CellRes& r) { return ((r.cx + 0.5) * kCell) * p.ux + ((r.cy + 0.5) * kCell) * p.uy; };
            auto at_line = [&](double t) {
                int n = 0;
                for (const CellRes& r : p.cells)
                    if (r.cls == kConflict && std::fabs(along(r) - t) <= kCutBand) ++n;
                return n;
            };
            double tmin = 1e18, tmax = -1e18;
            for (const CellRes& r : p.cells) { tmin = std::min(tmin, along(r)); tmax = std::max(tmax, along(r)); }
            if (p.seam && !p.seam->portals.empty()) {
                double sx = 0, sy = 0;
                for (const WorldPortal& w : p.seam->portals) {
                    const MapItems& M = w.map == A.map ? A : B;
                    sx += w.x + M.dx;
                    sy += w.y + M.dy;
                }
                p.px = sx / double(p.seam->portals.size());
                p.py = sy / double(p.seam->portals.size());
                p.cut_basis = "the crossing points";
            }
            // The best cut: a seam's stays near its crossing points (where the game switches maps:
            // further in, one map's copy would stand over the other's live streets); any other
            // pair's stays in the middle half of the shared ground.
            {
                double lo = std::floor((tmin + (tmax - tmin) * 0.25) / kCell) * kCell, hi = tmax - (tmax - tmin) * 0.25;
                if (p.seam && !p.seam->portals.empty()) {
                    double t0 = p.px * p.ux + p.py * p.uy;
                    lo = t0 - kSeamCutSlack;
                    hi = t0 + kSeamCutSlack;
                }
                p.best_t = lo;
                p.best_at = 1 << 30;
                for (double t = lo; t <= hi; t += kCell / 2.0) {
                    int n = at_line(t);
                    if (n < p.best_at) { p.best_at = n; p.best_t = t; }
                }
            }
            if (!(p.seam && !p.seam->portals.empty())) {
                // Through the best position, at the shared ground's middle across the axis.
                double mx = 0, my = 0;
                for (const CellRes& r : p.cells) { mx += (r.cx + 0.5) * kCell; my += (r.cy + 0.5) * kCell; }
                mx /= double(p.cells.size());
                my /= double(p.cells.size());
                p.px = p.ux != 0 ? p.best_t * p.ux : mx;
                p.py = p.uy != 0 ? p.best_t * p.uy : my;
                p.cut_basis = "the fewest conflicts";
            }
            {
                double lo = 1e18, hi = -1e18;
                for (const CellRes& r : p.cells) {
                    double w = r.cx * -p.uy + r.cy * p.ux;
                    lo = std::min(lo, w);
                    hi = std::max(hi, w);
                }
                p.line_cells = hi - lo + 1;
            }
            double t0 = p.px * p.ux + p.py * p.uy;
            for (Cluster& c : p.clusters) {
                int ln = 0, sa = 0, sb = 0;
                for (int i : c.cells) {
                    double dd = along(p.cells[size_t(i)]) - t0;
                    if (std::fabs(dd) <= kCutBand) ++ln; else if (dd < 0) ++sa; else ++sb;
                }
                c.at_cut = ln;
                c.side = ln ? "line" : sa >= sb ? "a" : "b";
                p.conflicts_at_cut += ln;
                p.conflicts_a_side += sa;
                p.conflicts_b_side += sb;
            }
        }
        if (!p.seam) {
            // How far apart the two would have to move (whole chunks, along the axis between them)
            // for their own content (cells that are not a copy of another map) to stop meeting.
            std::set<long long> oa = own_cells(A), ob = own_cells(B);
            const char* name = p.ux > 0 ? "x" : p.ux < 0 ? "x" : "y";
            int best = -1;
            for (int n = 0; n <= 64 && best < 0; ++n) {
                int sh = n * 512 / kCell;
                bool hit = false;
                for (long long k : ob) {
                    int cx = int((k >> 22) - (1 << 20)) + int(p.ux) * sh, cy = int((k & ((1 << 22) - 1)) - (1 << 20)) + int(p.uy) * sh;
                    if (oa.count(ck(cx, cy))) { hit = true; break; }
                }
                if (!hit) best = n;
            }
            p.sep_chunks = best;
            p.sep_axis = name;
        }

        // The cell map of the pair.
        IsoMap im;
        im.x0 = im.y0 = 1 << 30;
        im.x1 = im.y1 = -(1 << 30);
        for (const CellRes& r : p.cells) {
            im.x0 = std::min(im.x0, r.cx - 24); im.y0 = std::min(im.y0, r.cy - 24);
            im.x1 = std::max(im.x1, r.cx + 24); im.y1 = std::max(im.y1, r.cy + 24);
        }
        int span = std::max(im.x1 - im.x0, im.y1 - im.y0) * kCell;
        im.k = std::max(8, span / 700);
        im.fit();
        Img pic(im.w, im.h + 40, kColBg);
        std::unordered_map<long long, Cls> cls;
        for (const CellRes& r : p.cells) cls[ck(r.cx, r.cy)] = r.cls;
        im.raster(pic, [&](int cx, int cy) -> uint32_t {
            long long k = ck(cx, cy);
            auto it = cls.find(k);
            if (it != cls.end()) return it->second == kSame ? kColSame : it->second == kConflict ? kColConflict : it->second == kRetiled ? kColRetiled : kColExtra;
            bool ia = A.cells.count(k), ib = B.cells.count(k);
            if (ia) return 0xFF34506Eu;  // a only: blue-grey
            if (ib) return 0xFF5E4A6Au;  // b only: violet-grey
            return 0;
        });
        if (p.has_cut) {
            // The cut line, 6000 units each way.
            double nx = -p.uy, ny = p.ux;
            int u0, v0, u1, v1;
            im.to_px(p.px - nx * 9000, p.py - ny * 9000, u0, v0);
            im.to_px(p.px + nx * 9000, p.py + ny * 9000, u1, v1);
            line(pic, u0, v0, u1, v1, 0xFFFFFFFFu);
            line(pic, u0 + 1, v0, u1 + 1, v1, 0xFFFFFFFFu);
        }
        for (const Cluster& c : p.clusters) {
            if (c.id > 12) break;
            int u, v;
            im.to_px(c.wx, c.wy, u, v);
            fonts.label(pic, u + 6, v - 6, std::to_string(c.id));
        }
        fonts.label(pic, 6, im.h + 4, map_label(L, A.map) + " = blue,  " + map_label(L, B.map) + " = violet");
        fonts.label(pic, 6, im.h + 20, "green same   teal retiled   yellow one has more   red conflict" + std::string(p.has_cut ? "   white: cut line" : ""));
        p.cells_png = fmt("cells_%u_%u.png", A.map, B.map);
        save_png(out + "/" + p.cells_png, pic.px.data(), pic.w, pic.h, 1);

        // Pictures of the biggest conflicts: a alone, b alone, both (as the editor draws them).
        if (painter) {
            int shots = 0;
            for (Cluster& c : p.clusters) {
                if (shots >= 6 || c.ua + c.ub < 6) break;
                ++shots;
                int span_c = std::max(c.cx1 - c.cx0, c.cy1 - c.cy0) * kCell;
                int level = span_c > 1500 ? 1 : 0;
                const int S = 2;
                std::vector<uint32_t> buf;
                int w = 0, h = 0;
                Img row(3 * 640 + 8, 400 + 22, kColBg);
                const std::set<unsigned> sets[3] = {{A.map}, {B.map}, {A.map, B.map}};
                const std::string names[3] = {map_label(L, A.map) + " alone", map_label(L, B.map) + " alone", "both (as stitched)"};
                for (int s = 0; s < 3; ++s) {
                    painter->shot(int(std::lround(c.wx)), int(std::lround(c.wy)), A.layer, sets[s], level, S, buf, w, h);
                    row.blit(buf, w, h, s * (w + 4), 22);
                    fonts.label(row, s * (w + 4) + 6, 4, names[s]);
                }
                c.picture = fmt("cluster_%u_%u_%d.png", A.map, B.map, c.id);
                save_png(out + "/" + c.picture, row.px.data(), row.w, row.h, 1);
            }
        }
        std::printf("%s / %s (layer %d): %zu shared cells: %d same, %d+%d extra, %d retiled, %d conflict in %zu clusters%s; best align %+d,%+d,%+d "
                    "(%d votes, %d here)\n",
                    map_label(L, A.map).c_str(), map_label(L, B.map).c_str(), A.layer, p.cells.size(), p.n_same, p.n_extra_a, p.n_extra_b,
                    p.n_retiled, p.n_conflict, p.clusters.size(), p.seam ? " (seam)" : "", p.align_dx, p.align_dy, p.align_dz, p.align_votes,
                    p.here_votes);
    }

    // Overview per layer: every map's cells, the overlaps coloured.
    std::set<int> layers;
    for (const MapItems& m : maps) layers.insert(m.layer);
    for (int lay : layers) {
        std::unordered_map<long long, int> cover;  // cell -> number of maps
        IsoMap im;
        im.x0 = im.y0 = 1 << 30;
        im.x1 = im.y1 = -(1 << 30);
        for (const MapItems& m : maps) {
            if (m.layer != lay) continue;
            for (const auto& [k, v] : m.cells) {
                ++cover[k];
                int cx = int((k >> 22) - (1 << 20)), cy = int((k & ((1 << 22) - 1)) - (1 << 20));
                im.x0 = std::min(im.x0, cx); im.y0 = std::min(im.y0, cy);
                im.x1 = std::max(im.x1, cx); im.y1 = std::max(im.y1, cy);
            }
        }
        if (cover.empty()) continue;
        std::unordered_map<long long, Cls> worst;
        for (const PairRes& p : pairs) {
            if (p.a->layer != lay) continue;
            for (const CellRes& r : p.cells) {
                Cls& w = worst[ck(r.cx, r.cy)];
                Cls c = r.cls == kExtraB ? kExtraA : r.cls;
                if (c > w) w = c;
            }
        }
        int span = std::max(im.x1 - im.x0, im.y1 - im.y0) * kCell;
        im.k = std::max(8, span / 1400);
        im.fit();
        Img pic(im.w, im.h + 24, kColBg);
        im.raster(pic, [&](int cx, int cy) -> uint32_t {
            long long k = ck(cx, cy);
            auto it = worst.find(k);
            if (it != worst.end()) return it->second == kSame ? kColSame : it->second == kConflict ? kColConflict : it->second == kRetiled ? kColRetiled : kColExtra;
            return cover.count(k) ? 0xFF46505Au : 0;
        });
        for (const MapItems& m : maps) {
            if (m.layer != lay) continue;
            int u, v;
            im.to_px(m.sx, m.sy, u, v);
            fonts.label(pic, u - 20, v - 6, map_label(L, m.map));
        }
        fonts.label(pic, 6, im.h + 6, fmt("Layer %d.  grey: one map   green: same   teal: retiled   yellow: one has more   red: conflict", lay));
        save_png(out + "/" + fmt("overview_layer%d.png", lay), pic.px.data(), pic.w, pic.h, 1);
    }

    // The report.
    std::ofstream md(out + "/overlaps.md");
    json J = json::object();
    J["layout"] = argv[2];
    J["cell"] = kCell;
    J["pairs"] = json::array();
    md << "# Map overlap report\n\n"
       << "Layout: `" << argv[2] << "`. Every 128-unit world cell where two maps (in different groups, on one layer) both put items: "
       << "**same** = identical items (a harmless copy), **extra** = one map has everything the other has there and more, "
       << "**retiled** = the same kind of thing at the same height drawn with other tiles (ground for ground, water for water), "
       << "**conflict** = each has things the other lacks (water here, ground there; a wall on one side only; another height). "
       << "Cells of 128 x 128 world units (a ground tile); a chunk is 4 x 4 cells (512 units). **Aligned** = identical items (shape and frame) "
       << "near the shared ground line up best where the maps are now; otherwise the offset (dx, dy, dz) at which more of them "
       << "would.\n\n"
       << "| Maps | Layer | Seam | Shared cells | Same | Extra | Retiled | Conflict | Clusters | Aligned | Verdict |\n"
       << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    auto align_text = [](const PairRes& p) {
        auto mod512 = [](int v) { int r = ((v % 512) + 512) % 512; return r > 256 ? r - 512 : r; };
        if (p.align_votes < 12) return fmt("no copies (%d identical items at best)", p.align_votes);
        if (mod512(p.align_dx) == 0 && mod512(p.align_dy) == 0 && p.align_dz == 0)
            return p.here_votes ? fmt("yes (%d identical items)", p.here_votes) : std::string("only repeating pieces match");
        return fmt("**off**: %d identical items at %+d, %+d, %+d (%d here)", p.align_votes, p.align_dx, p.align_dy, p.align_dz, p.here_votes);
    };
    // How rough a join a cut leaves: conflicting cells near the line, out of the cells in the band
    // along it (the shared ground's width across the axis, 2 x kCutBand / kCell cells deep).
    auto roughness = [](const PairRes& p, int at) { return double(at) / std::max(1.0, p.line_cells * (2 * kCutBand / kCell + 1)); };
    auto verdict = [&](const PairRes& p) {
        if (p.n_conflict == 0) return std::string("copies only: trimming them changes nothing you can see");
        double r = roughness(p, p.conflicts_at_cut);
        std::string s;
        if (r < 0.05) s = fmt("**cut** (%s): clean, %d cells to touch up", p.cut_basis.c_str(), p.conflicts_at_cut);
        else if (r < 0.2) s = fmt("**cut** (%s): %d cells at the join to rework (%.0f%%)", p.cut_basis.c_str(), p.conflicts_at_cut, r * 100);
        else s = fmt("**rough at the cut** (%s): %d cells (%.0f%%); pull apart and build a link", p.cut_basis.c_str(), p.conflicts_at_cut, r * 100);
        if (p.seam && p.best_at * 10 < p.conflicts_at_cut * 7)
            s += fmt("; a cut %+.0f units along %s would leave %d", p.best_t - (p.px * p.ux + p.py * p.uy) * 1.0, p.ux != 0 ? "x" : "y", p.best_at);
        if (!p.seam && p.sep_chunks > 0) s += fmt("; apart in %d chunks along %s", p.sep_chunks, p.sep_axis.c_str());
        return s;
    };
    for (const PairRes& p : pairs)
        md << "| " << map_label(L, p.a->map) << " / " << map_label(L, p.b->map) << " | " << p.a->layer << " | " << (p.seam ? "yes" : "no")
           << " | " << p.cells.size() << " | " << p.n_same << " | " << p.n_extra_a + p.n_extra_b << " | " << p.n_retiled << " | "
           << p.n_conflict << " | " << p.clusters.size() << " | " << align_text(p) << " | " << verdict(p) << " |\n";
    md << "\n";
    for (int lay : layers) md << "![Layer " << lay << "](overview_layer" << lay << ".png)\n\n";
    md << "## Alignment\n\n";
    if (moves.empty())
        md << "Every map's copies line up best where it is now.\n\n";
    else {
        md << "Identical items (same shape and frame) in the ground two maps share line up best with these maps moved. "
              "Their places came from the original teleporters (a declared seam), which mark the join only roughly; the "
              "corrected offsets fall on the maps' own 512-unit chunk grid. Content that repeats every chunk (fences, walls) also "
              "lines up 512 units apart, so the smallest strong move is taken.\n\n"
              "| Map | Move by (dx, dy, dz) | Identical items there | Where it is now | With |\n| --- | --- | --- | --- | --- |\n";
        for (const MapMove& mm : moves)
            md << "| " << map_label(L, mm.map) << " | " << mm.dx << ", " << mm.dy << ", " << mm.dz << " | " << mm.votes << " | " << mm.here << " | "
               << mm.why << " |\n";
        md << "\nThe layout with these moves: `" << aligned_file << "` (the seams to them re-derived).\n\n";
    }
    for (const PairRes& p : pairs) {
        const std::string an = map_label(L, p.a->map), bn = map_label(L, p.b->map);
        md << "## " << an << " / " << bn << "\n\n";
        md << "Layer " << p.a->layer << ", " << (p.seam ? "a walkable seam" : "no seam (they only overlap)") << ". " << p.cells.size()
           << " shared cells: " << p.n_same << " same, " << p.n_extra_a << " where " << an << " has more, " << p.n_extra_b << " where " << bn
           << " has more, " << p.n_retiled << " retiled, **" << p.n_conflict << " in conflict** (" << p.clusters.size() << " clusters). Aligned: "
           << align_text(p) << ".\n\n";
        if (p.has_cut) {
            const char* axis = p.ux != 0 ? "x" : "y";
            md << fmt("Cut line through %s at world (%.0f, %.0f), across the %s axis: %s owns the ground on its side, %s the rest. "
                      "Conflicting cells: %d on %s's side and %d on %s's side (settled by the cut), **%d within %d units of the line** (they "
                      "would still show at the join; %.0f%% of the band along the line). The best place for a cut along %s leaves %d.\n\n",
                      p.cut_basis.c_str(), p.px, p.py, axis, an.c_str(), bn.c_str(), p.conflicts_a_side, an.c_str(), p.conflicts_b_side,
                      bn.c_str(), p.conflicts_at_cut, int(kCutBand), roughness(p, p.conflicts_at_cut) * 100, axis, p.best_at);
        }
        if (!p.seam && p.sep_chunks >= 0)
            md << fmt("No seam. Moved %d chunks (%d units) further apart along %s, the two maps' own content (cells that are not a copy "
                      "of another map) would no longer meet.\n\n",
                      p.sep_chunks, p.sep_chunks * 512, p.sep_axis.c_str());
        md << "![cells](" << p.cells_png << ")\n\n";
        json jp = {{"a", p.a->map}, {"b", p.b->map}, {"layer", p.a->layer}, {"seam", p.seam != nullptr}, {"shared", p.cells.size()},
                   {"same", p.n_same}, {"extra_a", p.n_extra_a}, {"extra_b", p.n_extra_b}, {"retiled", p.n_retiled}, {"conflict", p.n_conflict},
                   {"align", {{"best", {p.align_dx, p.align_dy, p.align_dz}}, {"votes", p.align_votes}, {"here", p.here_votes}}},
                   {"verdict", verdict(p)}, {"cells_png", p.cells_png}, {"clusters", json::array()}};
        if (p.has_cut)
            jp["cut"] = {{"point", {std::lround(p.px), std::lround(p.py)}}, {"toward_b", {int(p.ux), int(p.uy)}}, {"at_line", p.conflicts_at_cut},
                         {"a_side", p.conflicts_a_side}, {"b_side", p.conflicts_b_side}, {"basis", p.cut_basis},
                         {"roughness", roughness(p, p.conflicts_at_cut)}, {"best_along", std::lround(p.best_t)}, {"best_at_line", p.best_at}};
        if (!p.seam) jp["separation"] = {{"chunks", p.sep_chunks}, {"axis", p.sep_axis}};
        if (!p.clusters.empty()) {
            md << "| # | Cells | World centre | " << an << " local | " << bn << " local | Only in " << an << " | Only in " << bn << " | Where |\n"
               << "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
        }
        for (const Cluster& c : p.clusters) {
            int wx = int(std::lround(c.wx)), wy = int(std::lround(c.wy));
            std::string za = c.zmin_a <= c.zmax_a ? fmt(" (z %d-%d)", c.zmin_a - p.a->dz, c.zmax_a - p.a->dz) : "";
            std::string zb = c.zmin_b <= c.zmax_b ? fmt(" (z %d-%d)", c.zmin_b - p.b->dz, c.zmax_b - p.b->dz) : "";
            std::string where = p.has_cut ? (c.side == "line" ? "**at the cut**" : c.side == "a" ? an + "'s side" : bn + "'s side") : "-";
            if (c.id <= 40)
                md << "| " << c.id << " | " << c.cells.size() << " | " << wx << ", " << wy << " | " << wx - p.a->dx << ", " << wy - p.a->dy << " | "
                   << wx - p.b->dx << ", " << wy - p.b->dy << " | " << kinds_text(c.kinds_a) << za << " | " << kinds_text(c.kinds_b) << zb << " | "
                   << where << " |\n";
            jp["clusters"].push_back({{"id", c.id}, {"cells", c.cells.size()}, {"world", {wx, wy}}, {"local_a", {wx - p.a->dx, wy - p.a->dy}},
                                      {"local_b", {wx - p.b->dx, wy - p.b->dy}}, {"box_cells", {c.cx0, c.cy0, c.cx1, c.cy1}},
                                      {"only_a", c.ua}, {"only_b", c.ub}, {"matched", c.matched}, {"only_a_kinds", kinds_text(c.kinds_a)},
                                      {"only_b_kinds", kinds_text(c.kinds_b)}, {"side", c.side}, {"picture", c.picture}});
        }
        if (p.clusters.size() > 40) md << "\n(" << p.clusters.size() - 40 << " smaller clusters in overlaps.json.)\n";
        md << "\n";
        for (const Cluster& c : p.clusters)
            if (!c.picture.empty()) md << "Cluster " << c.id << ":\n\n![cluster " << c.id << "](" << c.picture << ")\n\n";
        J["pairs"].push_back(jp);
    }
    J["alignment"] = json::array();
    for (const MapMove& mm : moves)
        J["alignment"].push_back({{"map", mm.map}, {"move", {mm.dx, mm.dy, mm.dz}}, {"votes", mm.votes}, {"here", mm.here}, {"with", mm.why}});
    if (!aligned_file.empty()) J["aligned_layout"] = aligned_file;
    std::ofstream(out + "/overlaps.json") << J.dump(1) << "\n";
    std::printf("wrote %s/overlaps.md, overlaps.json and the pictures\n", out.c_str());
    return 0;
}
