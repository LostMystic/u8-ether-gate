// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/world_space.h"

#include <algorithm>
#include <chrono>

#include "core/log.h"
#include "data/gamedata.h"
#include "world/current_map.h"

namespace pg {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void add_static(const GameData& d, MapStatics& s, const ItemRecord& r) {
    if (r.x >= 32768 || r.y >= 32768) return;
    const ShapeInfo& si = d.typeflags.get(r.shape);
    if (si.is(ShapeInfo::kEditor)) return;
    s.chunks[size_t(r.y / 512) * 64 + r.x / 512].push_back(r);
}

}  // namespace

MapCoverage compute_coverage(const GameData& d, unsigned map) {
    MapCoverage c;
    c.map = map;
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(map));
    apply_map_fixes(map, fixed);
    auto mark = [&](const ItemRecord& r) {
        const ShapeInfo& si = d.typeflags.get(r.shape);
        if (si.is(ShapeInfo::kEditor)) return;
        int xd, yd, zd;
        si.footprint(r.flags & kItemFlipped, xd, yd, zd);
        uint8_t bit = uint8_t(1 | (si.is(ShapeInfo::kLand) || si.is(ShapeInfo::kRoof) ? 2 : 0));
        int x0 = std::max(0, (int(r.x) - std::max(xd, 1) + 1) / MapCoverage::kCell), x1 = std::min(MapCoverage::kN - 1, int(r.x) / MapCoverage::kCell);
        int y0 = std::max(0, (int(r.y) - std::max(yd, 1) + 1) / MapCoverage::kCell), y1 = std::min(MapCoverage::kN - 1, int(r.y) / MapCoverage::kCell);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) c.bits[size_t(y) * MapCoverage::kN + size_t(x)] |= bit;
    };
    for (const ItemRecord& r : fixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape; q.frame = g.frame;
                q.x = uint16_t(bx + 2 * g.x + 1); q.y = uint16_t(by + 2 * g.y + 1); q.z = uint8_t(std::min(255, r.z + g.z));
                mark(q);
            }
        } else if (r.x < 32768 && r.y < 32768) {
            mark(r);
        }
    }
    return c;
}

// --- WorldSpace ----------------------------------------------------------------------------

bool WorldSpace::load(const WorldLayoutFile& l) {
    layout_ = l;
    for (Region& r : region_) r = Region{};
    for (size_t g = 0; g < l.atlas.groups.size(); ++g) {
        const AtlasGroup& G = l.atlas.groups[g];
        for (unsigned m : G.maps) {
            if (m >= 256) continue;
            Region& r = region_[m];
            r.placed = true; r.group = int(g); r.dx = G.dx; r.dy = G.dy; r.dz = G.dz; r.layer = G.layer;
        }
    }
    for (const WorldSeam& s : l.seams) {
        if (s.a >= 256 || s.b >= 256) continue;
        region_[s.a].partners.push_back(s.b);
        region_[s.b].partners.push_back(s.a);
    }
    // FNV-1a over the placement and the seams (saves record which layout they were made with).
    uint32_t h = 2166136261u;
    auto mix = [&](int v) { for (int k = 0; k < 4; ++k) { h ^= uint8_t(v >> (8 * k)); h *= 16777619u; } };
    for (const AtlasGroup& G : l.atlas.groups) {
        for (unsigned m : G.maps) mix(int(m));
        mix(G.dx); mix(G.dy); mix(G.dz); mix(G.layer);
    }
    for (const WorldSeam& s : l.seams) { mix(int(s.a)); mix(int(s.b)); mix(s.dx); mix(s.dy); }
    hash_ = h;
    return !l.atlas.groups.empty();
}

bool WorldSpace::load_file(const std::string& path, std::string& err) {
    WorldLayoutFile f;
    if (!f.load(path, err)) return false;
    path_ = path;
    if (!load(f)) { err = "no groups"; return false; }
    return true;
}

void WorldSpace::offset(unsigned map, int& dx, int& dy, int& dz) const {
    dx = dy = dz = 0;
    if (map >= 256) return;
    dx = region_[map].dx; dy = region_[map].dy; dz = region_[map].dz;
}

void WorldSpace::to_world(unsigned map, int x, int y, int z, int32_t out[3]) const {
    int dx, dy, dz;
    offset(map, dx, dy, dz);
    out[0] = x + dx; out[1] = y + dy; out[2] = z + dz;
}

void WorldSpace::to_local(unsigned map, const int32_t w[3], int out[3]) const {
    int dx, dy, dz;
    offset(map, dx, dy, dz);
    out[0] = w[0] - dx; out[1] = w[1] - dy; out[2] = w[2] - dz;
}

bool WorldSpace::map_at(int32_t x, int32_t y, int lay, unsigned& map,
                        const std::function<unsigned(const WorldVariant&)>& pick) const {
    bool found = false;
    size_t best_n = 0;
    for (const AtlasGroup& G : layout_.atlas.groups) {
        if (G.layer != lay) continue;
        std::vector<unsigned> maps = G.maps;
        for (const WorldVariant& v : layout_.variants)
            if (v.group == G.name) maps = {pick ? pick(v) : v.when_zero};
        for (unsigned m : maps) {
            int lx = x - G.dx, ly = y - G.dy;
            if (lx < 0 || ly < 0 || lx >= 32768 || ly >= 32768) continue;
            WorldSpace& me = const_cast<WorldSpace&>(*this);
            if (!me.coverage(m).land(lx, ly)) continue;
            // Several maps cover it (a seam's strip): the one with the most scenery in that chunk.
            size_t n = me.statics(m).chunks[size_t(ly / 512) * 64 + size_t(lx / 512)].size();
            if (!found || n > best_n) { found = true; best_n = n; map = m; }
        }
    }
    return found;
}

bool WorldSpace::is_portal(unsigned map, unsigned to_map, unsigned id) const {
    (void)id;  // every teleporter between two seam maps is a crossing point
    return map != to_map && layout_.seam(map, to_map) != nullptr && placed(map) && placed(to_map);
}

const MapCoverage& WorldSpace::coverage(unsigned map) {
    auto it = coverage_.find(map);
    if (it == coverage_.end()) it = coverage_.emplace(map, std::make_unique<MapCoverage>(compute_coverage(data_, map))).first;
    return *it->second;
}

const MapStatics& WorldSpace::statics(unsigned map) {
    auto it = statics_.find(map);
    if (it != statics_.end()) return *it->second;
    auto s = std::make_unique<MapStatics>();
    s->map = map;
    std::vector<ItemRecord> fixed = parse_item_records(data_.fixed.entry(map));
    apply_map_fixes(map, fixed);
    for (const MapEntry& e : build_item_tree(fixed, data_.typeflags)) {
        if (e.parent >= 0) continue;
        const ItemRecord& r = e.rec;
        if (data_.typeflags.get(r.shape).family() == kFamGlobEgg) {
            int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : data_.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape; q.frame = g.frame; q.flags = kItemDisposable | kItemFastOnly;
                q.x = uint16_t(bx + 2 * g.x + 1); q.y = uint16_t(by + 2 * g.y + 1); q.z = uint8_t(std::min(255, r.z + g.z));
                add_static(data_, *s, q);
            }
        } else {
            add_static(data_, *s, r);
        }
    }
    return *statics_.emplace(map, std::move(s)).first->second;
}

// --- NeighborStream ------------------------------------------------------------------------

void NeighborStream::clear() {
    chunks_.clear();
    items_.clear();
    active_ = 0xFFFF;
    chunks_live = 0;
}

void NeighborStream::update(WorldSpace& ws, unsigned active, const int cam[3], int w, int h,
                            const std::function<const std::vector<MapEntry>*(unsigned)>& stored) {
    auto t0 = std::chrono::steady_clock::now();
    if (active != active_) {
        chunks_.clear();  // the dedupe depends on the active map; stored items changed
        active_ = active;
    }
    ++tick_;
    items_.clear();
    if (!ws.placed(active) || ws.seam_partners(active).empty()) { chunks_live = 0; update_ms_last = ms_since(t0); return; }
    int ax, ay, az;
    ws.offset(active, ax, ay, az);
    const MapCoverage& acov = ws.coverage(active);
    // The view in screen space (active-local), with a chunk of margin like the fast area.
    int left = (cam[0] - cam[1]) / 4 - (w / 2 + 256), right = (cam[0] - cam[1]) / 4 + (w / 2 + 256);
    int top = (cam[0] + cam[1]) / 8 - cam[2] - (h / 2 + 256), bottom = (cam[0] + cam[1]) / 8 - cam[2] + (h / 2 + 256);
    for (unsigned p : ws.seam_partners(active)) {
        int px, py, pz;
        ws.offset(p, px, py, pz);
        const int ddx = px - ax, ddy = py - ay, ddz = pz - az;  // p's point + dd = the active map's point
        const MapStatics& st = ws.statics(p);
        const std::vector<MapEntry>* nonfixed = stored ? stored(p) : nullptr;
        for (int cy = 0; cy < 64; ++cy)
            for (int cx = 0; cx < 64; ++cx) {
                int X = cx * 512 + ddx, Y = cy * 512 + ddy;
                int scx = (X - Y) / 4, scy = (X + Y) / 8 - ddz;
                if (scx + 128 <= left || scx - 128 >= right || scy + 128 <= top || scy - 256 >= bottom) continue;
                const int ci = cy * 64 + cx;
                auto key = std::make_pair(p, ci);
                auto it = chunks_.find(key);
                if (it == chunks_.end()) {
                    auto tb = std::chrono::steady_clock::now();
                    Chunk c;
                    auto add = [&](const ItemRecord& r, Item::Origin o) {
                        int x = r.x + ddx, y = r.y + ddy;
                        // The active map has its own ground anywhere under this item: its copy
                        // (the duplicated strip) or its own content wins; only what lies wholly
                        // beyond the active map's ground is drawn from the neighbour.
                        int xd, yd, zd;
                        data_.typeflags.get(r.shape).footprint(r.flags & kItemFlipped, xd, yd, zd);
                        for (int sy = y - std::max(yd - 1, 0);; sy = std::min(sy + MapCoverage::kCell, y)) {
                            for (int sx = x - std::max(xd - 1, 0);; sx = std::min(sx + MapCoverage::kCell, x)) {
                                if (acov.land(sx, sy)) return;
                                if (sx >= x) break;
                            }
                            if (sy >= y) break;
                        }
                        Item it2;
                        it2.shape = r.shape; it2.frame = r.frame; it2.flags = uint16_t(r.flags & ~kItemFastArea);
                        it2.x = x; it2.y = y; it2.z = r.z + ddz; it2.quality = r.quality; it2.origin = o;
                        it2.mapnum = uint8_t(p);
                        c.items.push_back(it2);
                    };
                    for (const ItemRecord& r : st.chunks[size_t(ci)]) add(r, Item::kFixed);
                    if (nonfixed)
                        for (const MapEntry& e : *nonfixed) {
                            if (e.parent >= 0 || e.rec.x >= 32768 || e.rec.y >= 32768) continue;
                            if (int(e.rec.y / 512) * 64 + int(e.rec.x / 512) != ci) continue;
                            if (data_.typeflags.get(e.rec.shape).is(ShapeInfo::kEditor)) continue;
                            add(e.rec, Item::kNonFixed);
                        }
                    double ms = ms_since(tb);
                    ++chunk_builds;
                    build_ms_total += ms;
                    build_ms_max = std::max(build_ms_max, ms);
                    it = chunks_.emplace(key, std::move(c)).first;
                }
                it->second.last_used = tick_;
                for (const Item& x : it->second.items) items_.push_back(&x);
            }
    }
    // Chunks out of view for 10 s (300 frames) are dropped.
    for (auto it = chunks_.begin(); it != chunks_.end();)
        if (tick_ - it->second.last_used > 300) it = chunks_.erase(it);
        else ++it;
    chunks_live = unsigned(chunks_.size());
    update_ms_last = ms_since(t0);
}

}  // namespace pg
