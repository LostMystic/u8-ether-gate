// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/tile_cache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "data/gamedata.h"
#include "world/current_map.h"

namespace pg {

namespace {
// Screen occupancy grid cell (native pixels) and the area it spans.
constexpr int kCell = 128;
constexpr int kGX0 = -9216, kGY0 = -1024, kGW = 18432 / kCell, kGH = 10240 / kCell;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Average of four premultiplied ARGB pixels, per channel.
inline uint32_t avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t rb = (a & 0xFF00FFu) + (b & 0xFF00FFu) + (c & 0xFF00FFu) + (d & 0xFF00FFu);
    uint32_t ag = ((a >> 8) & 0xFF00FFu) + ((b >> 8) & 0xFF00FFu) + ((c >> 8) & 0xFF00FFu) + ((d >> 8) & 0xFF00FFu);
    return ((rb >> 2) & 0xFF00FFu) | (((ag >> 2) & 0xFF00FFu) << 8);
}

// dst = src over dst, premultiplied.
inline uint32_t over(uint32_t s, uint32_t d) {
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    uint32_t ia = 255 - a;
    uint32_t rb = ((d & 0xFF00FFu) * ia + 0x800080u) >> 8 & 0xFF00FFu;
    uint32_t ag = (((d >> 8) & 0xFF00FFu) * ia + 0x800080u) >> 8 & 0xFF00FFu;
    return s + (rb | (ag << 8));
}
}  // namespace

struct TileCache::MapState {
    bool ready = false;
    std::bitset<64 * 64> excluded;
    std::vector<uint8_t> grid;  // kGW x kGH, 1 = something may be drawn there
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    bool any = false;
};

TileCache::TileCache(const GameData& data) : data_(data), view_(data) {
#ifdef __EMSCRIPTEN__
    budget_ = size_t(160) << 20;
#else
    budget_ = size_t(512) << 20;
#endif
    opt_.roof_hiding = false;
    for (int i = 0; i < 256; ++i) {
        const auto& c = data.palette.rgb8[size_t(i)];
        lut_[i] = i == 0 ? 0u : 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
}

TileCache::~TileCache() = default;

double TileCache::now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void TileCache::clear() {
    tiles_.clear();
    bytes_ = 0;
}

void TileCache::set_show_editor(bool on) {
    if (opt_.show_editor == on) return;
    opt_.show_editor = on;
    clear();
}

void TileCache::clear_map(unsigned map) {
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if ((it->first >> 56) == map) {
            bytes_ -= it->second.px.size() * 4;
            it = tiles_.erase(it);
        } else ++it;
    }
    if (loaded_ && loaded_->map_number() == map) loaded_dirty_ = true;
}

void TileCache::set_exclusion(unsigned map, const std::bitset<64 * 64>& chunks) {
    auto& p = maps_[map];
    if (!p) p = std::make_unique<MapState>();
    if (p->excluded == chunks) return;
    p->excluded = chunks;
    p->ready = false;  // the occupancy grid changes
    clear_map(map);
}

size_t TileCache::excluded_chunks(unsigned map) const {
    auto it = maps_.find(map);
    return it == maps_.end() || !it->second ? 0 : it->second->excluded.count();
}

TileCache::MapState& TileCache::state(unsigned map) {
    auto& p = maps_[map];
    if (!p) p = std::make_unique<MapState>();
    MapState& s = *p;
    if (s.ready) return s;
    s.ready = true;
    s.grid.assign(size_t(kGW) * kGH, 0);
    MapFootprint f = compute_footprint(data_, map);
    s.sx0 = s.sy0 = 1 << 30;
    s.sx1 = s.sy1 = -(1 << 30);
    for (int cy = f.cy0; cy <= f.cy1; ++cy)
        for (int cx = f.cx0; cx <= f.cx1; ++cx) {
            if (!f.has(cx, cy) || s.excluded[size_t(cy) * 64 + cx]) continue;
            // The chunk's ground diamond, plus room for shapes standing up to z 255 and
            // frames reaching past their anchor.
            int l = (cx - cy - 1) * 128 - 192, r = (cx - cy + 1) * 128 + 192;
            int t = (cx + cy) * 64 - 255 - 320, b = (cx + cy + 2) * 64 + 64;
            s.sx0 = std::min(s.sx0, l); s.sx1 = std::max(s.sx1, r);
            s.sy0 = std::min(s.sy0, t); s.sy1 = std::max(s.sy1, b);
            s.any = true;
            int gx0 = std::max(0, (l - kGX0) / kCell), gx1 = std::min(kGW - 1, (r - kGX0) / kCell);
            int gy0 = std::max(0, (t - kGY0) / kCell), gy1 = std::min(kGH - 1, (b - kGY0) / kCell);
            for (int gy = gy0; gy <= gy1; ++gy)
                std::memset(&s.grid[size_t(gy) * kGW + gx0], 1, size_t(gx1 - gx0 + 1));
        }
    return s;
}

bool TileCache::content_bounds(unsigned map, int& sx0, int& sy0, int& sx1, int& sy1) {
    MapState& s = state(map);
    sx0 = s.sx0; sy0 = s.sy0; sx1 = s.sx1; sy1 = s.sy1;
    return s.any;
}

bool TileCache::area_maybe_drawn(unsigned map, int sx0, int sy0, int w, int h) {
    MapState& s = state(map);
    if (!s.any) return false;
    int gx0 = floor_div(sx0 - kGX0, kCell), gx1 = floor_div(sx0 + w - 1 - kGX0, kCell);
    int gy0 = floor_div(sy0 - kGY0, kCell), gy1 = floor_div(sy0 + h - 1 - kGY0, kCell);
    gx0 = std::max(gx0, 0); gy0 = std::max(gy0, 0);
    gx1 = std::min(gx1, kGW - 1); gy1 = std::min(gy1, kGH - 1);
    for (int gy = gy0; gy <= gy1; ++gy)
        for (int gx = gx0; gx <= gx1; ++gx)
            if (s.grid[size_t(gy) * kGW + gx]) return true;
    return false;
}

TileCache::Tile& TileCache::put(uint64_t k) {
    Tile& t = tiles_[k];
    bytes_ -= t.px.size() * 4;
    t.px.clear();
    t.used = ++clock_;
    return t;
}

const TileCache::Tile* TileCache::get(unsigned map, int level, int tx, int ty) {
    auto it = tiles_.find(key(map, level, tx, ty));
    if (it == tiles_.end()) return nullptr;
    it->second.used = ++clock_;
    return &it->second;
}

void TileCache::render_block(unsigned map, int bx, int by) {
    double t0 = now_ms();
    if (!loaded_) loaded_ = std::make_unique<CurrentMap>(data_);
    if (loaded_->map_number() != map || loaded_dirty_) {
        loaded_->load(map);
        loaded_dirty_ = false;
        if (fixed_only_)
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx) {
                    std::vector<uint16_t> ids = loaded_->chunk(cx, cy);
                    for (uint16_t id : ids) {
                        const Item& it = loaded_->item(id);
                        if (it.origin != Item::kFixed && it.origin != Item::kGlob) loaded_->unlink(id);
                    }
                }
        // Atlas overlaps: take the items of excluded chunks out of the chunk lists.
        const auto& ex = state(map).excluded;
        if (ex.any())
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx)
                    if (ex[size_t(cy) * 64 + cx]) {
                        std::vector<uint16_t> ids = loaded_->chunk(cx, cy);
                        for (uint16_t id : ids) loaded_->unlink(id);
                    }
    }
    // Camera with screen point (Sx, Sy) at the surface centre and z = 0.
    int Sx = bx * kBlock + kBlock / 2, Sy = by * kBlock + kBlock / 2;
    int cam[3] = {2 * Sx + 4 * Sy, 4 * Sy - 2 * Sx, 0};
    // Fast area for this block alone, taller than the block so high items below it count.
    loaded_->update_fast_area(cam, cam, kBlock + 256, kBlock + 512);
    surf_.reset_clip();
    surf_.clear(0);
    view_.render(*loaded_, surf_, cam, cam, opt_);
    ++renders_;

    // Level 1: the 2x2 box filter of the block. Level 0 tiles are kept only when zoomed in
    // that far (render_l0_).
    const uint8_t* px = surf_.data();
    Tile& l1 = put(key(map, 1, bx, by));
    bool any = false;
    for (int i = 0; i < kBlock * kBlock && !any; ++i) any = px[i] != 0;
    l1.empty = !any;
    if (any) {
        l1.px.resize(size_t(kTile) * kTile);
        for (int y = 0; y < kTile; ++y) {
            const uint8_t* r0 = px + size_t(2 * y) * kBlock;
            const uint8_t* r1 = r0 + kBlock;
            uint32_t* o = &l1.px[size_t(y) * kTile];
            for (int x = 0; x < kTile; ++x)
                o[x] = avg4(lut_[r0[2 * x]], lut_[r0[2 * x + 1]], lut_[r1[2 * x]], lut_[r1[2 * x + 1]]);
        }
        bytes_ += l1.px.size() * 4;
    }
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            if (!want_l0_) break;
            Tile& t = put(key(map, 0, bx * 2 + i, by * 2 + j));
            t.empty = !any;
            if (!any) continue;
            t.px.resize(size_t(kTile) * kTile);
            bool some = false;
            for (int y = 0; y < kTile; ++y) {
                const uint8_t* r = px + size_t(j * kTile + y) * kBlock + i * kTile;
                uint32_t* o = &t.px[size_t(y) * kTile];
                for (int x = 0; x < kTile; ++x) { o[x] = lut_[r[x]]; some |= r[x] != 0; }
            }
            if (!some) { t.empty = true; t.px.clear(); t.px.shrink_to_fit(); }
            else bytes_ += t.px.size() * 4;
        }
    render_ms_ += now_ms() - t0;
    evict();
}

void TileCache::evict() {
    if (bytes_ <= budget_) return;
    std::vector<std::pair<uint64_t, uint64_t>> cand;  // (used, key)
    for (auto& [k, t] : tiles_)
        if (!t.px.empty() && !(t.used >= frame_ && int((k >> 48) & 0xFF) == display_level_)) cand.emplace_back(t.used, k);
    std::sort(cand.begin(), cand.end());
    for (auto& [u, k] : cand) {
        if (bytes_ <= budget_ * 8 / 10) break;
        bytes_ -= tiles_[k].px.size() * 4;
        tiles_.erase(k);
    }
}

bool TileCache::build(unsigned map, int level, int tx, int ty, double deadline) {
    if (get(map, level, tx, ty)) return true;
    const int span = kTile << level;
    if (!area_maybe_drawn(map, tx * span, ty * span, span, span)) {
        put(key(map, level, tx, ty)).empty = true;
        return true;
    }
    if (level <= 1) {
        if (now_ms() > deadline) return false;
        want_l0_ = level == 0;
        render_block(map, level == 0 ? floor_div(tx, 2) : tx, level == 0 ? floor_div(ty, 2) : ty);
        want_l0_ = false;
        return get(map, level, tx, ty) != nullptr;
    }
    // Children first; stop at the deadline (finished children stay cached).
    const Tile* ch[4];
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
            if (!build(map, level - 1, tx * 2 + i, ty * 2 + j, deadline)) return false;
    bool any = false;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            ch[j * 2 + i] = get(map, level - 1, tx * 2 + i, ty * 2 + j);
            if (!ch[j * 2 + i]) return false;  // evicted meanwhile; try again later
            any |= !ch[j * 2 + i]->empty;
        }
    Tile t;
    t.empty = !any;
    if (any) {
        t.px.assign(size_t(kTile) * kTile, 0);
        for (int q = 0; q < 4; ++q) {
            const Tile* c = ch[q];
            if (c->empty) continue;
            int ox = (q & 1) * (kTile / 2), oy = (q >> 1) * (kTile / 2);
            for (int y = 0; y < kTile / 2; ++y) {
                const uint32_t* r0 = &c->px[size_t(2 * y) * kTile];
                const uint32_t* r1 = r0 + kTile;
                uint32_t* o = &t.px[size_t(oy + y) * kTile + ox];
                for (int x = 0; x < kTile / 2; ++x) o[x] = avg4(r0[2 * x], r0[2 * x + 1], r1[2 * x], r1[2 * x + 1]);
            }
        }
        bytes_ += t.px.size() * 4;
    }
    Tile& dst = put(key(map, level, tx, ty));
    dst.empty = t.empty;
    dst.px = std::move(t.px);
    evict();
    return true;
}

int draw_tiles(TileCache& cache, const std::vector<PlacedMap>& maps, int zoom, int cx, int cy, uint32_t* out, int w, int h,
               double deadline, uint32_t background) {
    const int K = TileCache::kTile;
    const int s = 1 << zoom, span = K << zoom;
    const int vx0 = cx - (w / 2) * s, vy0 = cy - (h / 2) * s;  // native atlas point of out(0, 0)
    std::fill(out, out + size_t(w) * h, background);
    cache.next_frame(zoom);
    struct Want { size_t m; int tx, ty; long d; };
    std::vector<Want> missing;
    // Pass 1: find visible tiles, build the missing ones nearest the centre first.
    for (size_t mi = 0; mi < maps.size(); ++mi) {
        const PlacedMap& pm = maps[mi];
        int lx0 = vx0 - pm.ox, ly0 = vy0 - pm.oy;
        int tx0 = floor_div(lx0, span), tx1 = floor_div(lx0 + w * s - 1, span);
        int ty0 = floor_div(ly0, span), ty1 = floor_div(ly0 + h * s - 1, span);
        int bx0, by0, bx1, by1;
        if (!cache.content_bounds(pm.map, bx0, by0, bx1, by1)) continue;
        tx0 = std::max(tx0, floor_div(bx0, span)); tx1 = std::min(tx1, floor_div(bx1, span));
        ty0 = std::max(ty0, floor_div(by0, span)); ty1 = std::min(ty1, floor_div(by1, span));
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
                if (!cache.get(pm.map, zoom, tx, ty)) {
                    long dx = long(pm.ox) + long(tx) * span + span / 2 - cx, dy = long(pm.oy) + long(ty) * span + span / 2 - cy;
                    missing.push_back({mi, tx, ty, dx * dx + dy * dy});
                }
    }
    std::sort(missing.begin(), missing.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
    int still = 0;
    for (const Want& m : missing)
        if (TileCache::now_ms() > deadline || !cache.build(maps[m.m].map, zoom, m.tx, m.ty, deadline)) ++still;

    // Pass 2: composite, in map order (later maps on top). A missing tile shows a coarser
    // ancestor magnified if one is cached.
    for (const PlacedMap& pm : maps) {
        int lx0 = vx0 - pm.ox, ly0 = vy0 - pm.oy;
        int tx0 = floor_div(lx0, span), tx1 = floor_div(lx0 + w * s - 1, span);
        int ty0 = floor_div(ly0, span), ty1 = floor_div(ly0 + h * s - 1, span);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                // Output position of the tile's top-left pixel.
                int u0 = floor_div(pm.ox + tx * span - vx0, s), v0 = floor_div(pm.oy + ty * span - vy0, s);
                const TileCache::Tile* t = cache.get(pm.map, zoom, tx, ty);
                int mag = 0, sub_x = 0, sub_y = 0;
                if (!t) {
                    for (int k = 1; k <= 4 && zoom + k <= TileCache::kMaxLevel && !t; ++k) {
                        t = cache.get(pm.map, zoom + k, floor_div(tx, 1 << k), floor_div(ty, 1 << k));
                        if (t) {
                            mag = k;
                            sub_x = (tx - floor_div(tx, 1 << k) * (1 << k)) * (K >> k);
                            sub_y = (ty - floor_div(ty, 1 << k) * (1 << k)) * (K >> k);
                        }
                    }
                }
                if (!t || t->empty) continue;
                int y0 = std::max(0, v0), y1 = std::min(h, v0 + K), x0 = std::max(0, u0), x1 = std::min(w, u0 + K);
                for (int y = y0; y < y1; ++y) {
                    const uint32_t* src = &t->px[size_t(sub_y + ((y - v0) >> mag)) * K + sub_x];
                    uint32_t* dst = out + size_t(y) * w;
                    for (int x = x0; x < x1; ++x) dst[x] = over(src[(x - u0) >> mag], dst[x]);
                }
            }
    }
    return still;
}

}  // namespace pg
