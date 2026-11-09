// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/map_patches.h"
#include "world/atlas_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

#include "data/gamedata.h"
#include "data/mapfile.h"
#include "json/json.hpp"
#include "world/current_map.h"

namespace pg {

namespace {

// Names given in the specs (music.md, startup.md); everything else is "Map N".
const char* known_name(unsigned m) {
    switch (m) {
    case 3: return "Tenebrae docks";
    case 40: return "Central Tenebrae";
    case 6: return "Cemetery";
    case 49: case 50: return "Upper catacombs";
    case 46: return "Plane of Air";
    default: return nullptr;
    }
}

std::vector<ItemRecord> top_level_records(const GameData& d, unsigned m) {
    std::vector<ItemRecord> out = parse_item_records(d.fixed.entry(m));
    apply_map_fixes(m, out);
    std::vector<ItemRecord> nonfixed = parse_item_records(d.nonfixed.entry(m));
    map_patches::apply_moves(m, nonfixed);
    for (const MapEntry& e : build_item_tree(nonfixed, d.typeflags))
        if (e.parent < 0) out.push_back(e.rec);
    return out;
}

// Every map with fixed items (items.md: 42 in FIXED.DAT; map 0 is not a place).
std::vector<unsigned> atlas_maps(const GameData& d) {
    std::vector<unsigned> out;
    for (unsigned m = 1; m < d.fixed.count(); ++m)
        if (d.fixed.entry(m).size >= 16) out.push_back(m);
    return out;
}

long long cell_key(int x, int y) { return (long long)(x + 100000) * 1000000LL + (y + 100000); }

int round_chunks(int v) { return int(std::lround(v / 512.0)); }

// A chunk with this many records has real content (filler land is one ground glob or two).
constexpr int kDense = 3;
// A teleport offset is accepted as is when the maps would share at most this many dense
// chunks (seams between neighbouring outdoor maps share a few) and at most 60% of the
// smaller map's chunks overall. Otherwise the map is an interior, a level above or below,
// or a variant of the same place, and goes beside its entrance instead.
constexpr int kMaxDenseOverlap = 8;
// Neighbouring outdoor maps repeat a strip of each other's border (identical fixed items).
// Votes of identical items (same shape, frame and z) near the teleport offset find the
// exact offset; this many votes confirm a seam.
constexpr int kSeamVotes = 40, kSeamSearch = 768;

// Best offset of b's fixed items onto a's near (ox, oy), and its votes.
int seam_votes(const std::vector<ItemRecord>& a, const std::vector<ItemRecord>& b, int ox, int oy, int& bx, int& by) {
    std::unordered_multimap<uint32_t, const ItemRecord*> idx;
    for (const ItemRecord& r : a) idx.emplace((uint32_t(r.shape) << 8) | r.frame, &r);
    std::map<std::pair<int, int>, int> votes;
    for (const ItemRecord& r : b) {
        auto rg = idx.equal_range((uint32_t(r.shape) << 8) | r.frame);
        for (auto it = rg.first; it != rg.second; ++it) {
            int dx = it->second->x - r.x, dy = it->second->y - r.y;
            if (it->second->z == r.z && std::abs(dx - ox) <= kSeamSearch && std::abs(dy - oy) <= kSeamSearch)
                ++votes[{dx, dy}];
        }
    }
    int best = 0;
    for (auto& [k, v] : votes)
        if (v > best) { best = v; bx = k.first; by = k.second; }
    return best;
}

struct Placer {
    std::unordered_map<long long, int> used;  // atlas chunk cell -> most records placed there

    // Number of chunks of f (shifted by whole chunks sx, sy) that hit a used cell within
    // `dilate` chunks.
    int overlap(const MapFootprint& f, int sx, int sy, int dilate, int stop_after = 1 << 30) const {
        int n = 0;
        for (int cy = f.cy0; cy <= f.cy1; ++cy)
            for (int cx = f.cx0; cx <= f.cx1; ++cx) {
                if (!f.has(cx, cy)) continue;
                bool hit = false;
                for (int oy = -dilate; oy <= dilate && !hit; ++oy)
                    for (int ox = -dilate; ox <= dilate && !hit; ++ox)
                        hit = used.count(cell_key(cx + sx + ox, cy + sy + oy)) != 0;
                if (hit && ++n >= stop_after) return n;
            }
        return n;
    }
    // Chunks where both f and the placed maps have real content (kDense+ records), not
    // just filler such as one ground glob.
    int dense_overlap(const MapFootprint& f, int sx, int sy) const {
        int n = 0;
        for (int cy = f.cy0; cy <= f.cy1; ++cy)
            for (int cx = f.cx0; cx <= f.cx1; ++cx) {
                if (f.count[size_t(cy) * 64 + cx] < kDense) continue;
                auto it = used.find(cell_key(cx + sx, cy + sy));
                if (it != used.end() && it->second >= kDense) ++n;
            }
        return n;
    }
    void add(const MapFootprint& f, int sx, int sy) {
        for (int cy = f.cy0; cy <= f.cy1; ++cy)
            for (int cx = f.cx0; cx <= f.cx1; ++cx)
                if (f.has(cx, cy)) {
                    int& c = used[cell_key(cx + sx, cy + sy)];
                    c = std::max(c, int(f.count[size_t(cy) * 64 + cx]));
                }
    }
    // Nearest free whole-chunk shift to (sx, sy) where f does not come within 2 chunks of
    // anything (a square spiral).
    void free_spot(const MapFootprint& f, int& sx, int& sy) const {
        if (!overlap(f, sx, sy, 2, 1)) return;
        for (int r = 1; r < 400; ++r)
            for (int k = -r; k < r; ++k) {
                const int cand[4][2] = {{k, -r}, {r, k}, {-k, r}, {-r, -k}};
                for (auto& c : cand)
                    if (!overlap(f, sx + c[0], sy + c[1], 2, 1)) { sx += c[0]; sy += c[1]; return; }
            }
    }
};

}  // namespace

int AtlasLayout::group_of(unsigned map) const {
    for (size_t i = 0; i < groups.size(); ++i)
        for (unsigned m : groups[i].maps)
            if (m == map) return int(i);
    return -1;
}

MapFootprint compute_footprint(const GameData& d, unsigned m) {
    MapFootprint f;
    f.map = m;
    std::vector<int> zs;
    auto add = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || x >= 32768 || y >= 32768) return;
        int cx = x / 512, cy = y / 512;
        if (f.count[size_t(cy) * 64 + cx] < 65535) ++f.count[size_t(cy) * 64 + cx];
        if (!f.occ[size_t(cy) * 64 + cx]) { f.occ[size_t(cy) * 64 + cx] = true; ++f.chunks; }
        f.cx0 = std::min(f.cx0, cx); f.cy0 = std::min(f.cy0, cy);
        f.cx1 = std::max(f.cx1, cx); f.cy1 = std::max(f.cy1, cy);
        zs.push_back(z);
    };
    for (const ItemRecord& r : top_level_records(d, m)) add(r.x, r.y, r.z);
    for (const NpcRecord& n : d.npcs)
        if (n.mapnum == m && n.number != 0) add(n.x, n.y, n.z);
    if (!zs.empty()) {
        std::nth_element(zs.begin(), zs.begin() + long(zs.size() / 2), zs.end());
        f.median_z = zs[zs.size() / 2];
    }
    return f;
}

std::vector<MapFootprint> compute_footprints(const GameData& d) {
    std::vector<MapFootprint> out;
    for (unsigned m : atlas_maps(d)) {
        MapFootprint f = compute_footprint(d, m);
        if (f.chunks) out.push_back(f);
    }
    return out;
}

std::vector<TeleportLink> teleport_links(const GameData& d) {
    struct Egg { unsigned map; ItemRecord r; };
    std::vector<Egg> from, dest;
    for (unsigned m : atlas_maps(d))
        for (const ItemRecord& r : top_level_records(d, m)) {
            if (d.typeflags.get(r.shape).family() != kFamTeleportEgg) continue;
            (r.frame == 1 ? dest : from).push_back({m, r});
        }
    std::vector<TeleportLink> out;
    for (const Egg& f : from) {
        unsigned to = f.r.mapnum, id = f.r.quality & 0xFF;
        if (to == f.map) continue;  // a teleporter within one map says nothing about placement
        for (const Egg& t : dest)
            if (t.map == to && (t.r.quality & 0xFF) == id) {
                TeleportLink l;
                l.from_map = f.map; l.to_map = to; l.id = id;
                l.fx = f.r.x; l.fy = f.r.y; l.fz = f.r.z;
                l.tx = t.r.x; l.ty = t.r.y; l.tz = t.r.z;
                out.push_back(l);
                break;
            }
    }
    return out;
}

AtlasLayout generate_atlas_layout(const GameData& d) {
    std::vector<MapFootprint> fps = compute_footprints(d);
    std::vector<TeleportLink> links = teleport_links(d);
    std::map<unsigned, size_t> idx;
    for (size_t i = 0; i < fps.size(); ++i) idx[fps[i].map] = i;

    // Relative offset of b in a's coordinates for each linked pair: the teleporter in a and
    // the arrival point in b are (nearly) the same spot, so b + off = a there. Both
    // directions are averaged.
    struct Rel { double sx = 0, sy = 0; int n = 0; std::string why; };
    std::map<std::pair<unsigned, unsigned>, Rel> rel;  // key (a, b) with a < b: offset of b
    for (const TeleportLink& l : links) {
        if (!idx.count(l.from_map) || !idx.count(l.to_map)) continue;
        unsigned a = std::min(l.from_map, l.to_map), b = std::max(l.from_map, l.to_map);
        int ox = l.fx - l.tx, oy = l.fy - l.ty;  // offset of to_map in from_map's coordinates
        if (l.from_map != a) { ox = -ox; oy = -oy; }
        Rel& r = rel[{a, b}];
        r.sx += ox; r.sy += oy; ++r.n;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%sid %u: map %u egg at %d,%d,%d -> map %u at %d,%d,%d", r.why.empty() ? "" : "; ",
                      l.id, l.from_map, l.fx, l.fy, l.fz, l.to_map, l.tx, l.ty, l.tz);
        r.why += buf;
    }
    auto offset_of = [&](unsigned a, unsigned b, int& ox, int& oy, std::string& why) {
        // Offset of b relative to a.
        auto it = rel.find({std::min(a, b), std::max(a, b)});
        if (it == rel.end()) return false;
        ox = int(std::lround(it->second.sx / it->second.n));
        oy = int(std::lround(it->second.sy / it->second.n));
        if (a > b) { ox = -ox; oy = -oy; }
        why = it->second.why;
        return true;
    };

    // Maps linked with (almost) no offset that do not cover the same chunks share one
    // coordinate space: they become one group.
    std::vector<int> parent(fps.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = find(parent[size_t(i)]); };
    std::map<int, std::string> share_why;
    for (auto& [k, r] : rel) {
        double ox = r.sx / r.n, oy = r.sy / r.n;
        if (std::abs(ox) > 300 || std::abs(oy) > 300) continue;
        const MapFootprint &fa = fps[idx[k.first]], &fb = fps[idx[k.second]];
        int common = int((fa.occ & fb.occ).count());
        int ra = find(int(idx[k.first])), rb = find(int(idx[k.second]));
        if (ra != rb) parent[size_t(rb)] = ra;
        char buf[200];
        std::snprintf(buf, sizeof buf, "maps %u and %u share coordinates (teleport offset %.0f,%.0f, %d common chunks)",
                      k.first, k.second, ox, oy, common);
        share_why[ra] += (share_why[ra].empty() ? "" : "; ") + std::string(buf);
    }

    struct G { std::vector<unsigned> maps; MapFootprint fp; bool placed = false; int dx = 0, dy = 0;
               std::string why; bool data = false; };
    std::vector<G> gs;
    std::map<unsigned, int> gof;  // map -> group index
    std::map<int, int> root_to_g;
    for (size_t i = 0; i < fps.size(); ++i) {
        int r = find(int(i));
        if (!root_to_g.count(r)) { root_to_g[r] = int(gs.size()); gs.emplace_back(); }
        G& g = gs[size_t(root_to_g[r])];
        g.maps.push_back(fps[i].map);
        gof[fps[i].map] = root_to_g[r];
        if (g.maps.size() == 1) g.fp = fps[i];
        else {
            g.fp.occ |= fps[i].occ;
            g.fp.cx0 = std::min(g.fp.cx0, fps[i].cx0); g.fp.cy0 = std::min(g.fp.cy0, fps[i].cy0);
            g.fp.cx1 = std::max(g.fp.cx1, fps[i].cx1); g.fp.cy1 = std::max(g.fp.cy1, fps[i].cy1);
            g.fp.chunks = int(g.fp.occ.count());
            for (size_t c = 0; c < g.fp.count.size(); ++c)
                g.fp.count[c] = uint16_t(std::min(65535, g.fp.count[c] + fps[i].count[c]));
        }
        if (share_why.count(r)) g.why = share_why[r];
    }

    // Group-level edges: (neighbour, offset of neighbour relative to this group, text).
    struct E { int to; int ox, oy; std::string why; int votes; };
    std::vector<std::vector<E>> edges(gs.size());
    for (auto& [k, r] : rel) {
        int ga = gof[k.first], gb = gof[k.second];
        if (ga == gb) continue;
        int ox, oy;
        std::string why;
        offset_of(k.first, k.second, ox, oy, why);
        int vx = 0, vy = 0;
        int votes = seam_votes(parse_item_records(d.fixed.entry(k.first)), parse_item_records(d.fixed.entry(k.second)), ox, oy,
                               vx, vy);
        if (votes >= kSeamVotes) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "; seam: %d identical border items line up at offset %d,%d", votes, vx, vy);
            why += buf;
            ox = vx; oy = vy;
        } else votes = 0;
        char head[64];
        std::snprintf(head, sizeof head, "teleport link map %u <-> map %u (", k.first, k.second);
        edges[size_t(ga)].push_back({gb, ox, oy, head + why + ")", votes});
        edges[size_t(gb)].push_back({ga, -ox, -oy, head + why + ")", votes});
    }

    Placer pl;
    auto place = [&](int gi, int dx, int dy, const std::string& why, bool data) {
        G& g = gs[size_t(gi)];
        g.placed = true; g.dx = dx; g.dy = dy; g.data = data;
        g.why = g.why.empty() ? why : why + "; " + g.why;
        pl.add(g.fp, round_chunks(dx), round_chunks(dy));
    };
    // Best first: of all links from placed to unplaced groups, take the one whose offset
    // lands on the least content; place it if it fits (kMaxDenseOverlap), repeat.
    auto spread_exact = [&]() {
        for (;;) {
            int best_g = -1, best_d = 1 << 30, bdx = 0, bdy = 0;
            const E* best_e = nullptr;
            for (size_t gi = 0; gi < gs.size(); ++gi) {
                if (!gs[gi].placed) continue;
                for (const E& e : edges[gi]) {
                    G& h = gs[size_t(e.to)];
                    if (h.placed) continue;
                    int dx = gs[gi].dx + e.ox, dy = gs[gi].dy + e.oy;
                    int sx = round_chunks(dx), sy = round_chunks(dy);
                    int dense = pl.dense_overlap(h.fp, sx, sy);
                    if (std::getenv("PAGAN_ATLAS_DEBUG"))
                        std::fprintf(stderr, "candidate map %u from map %u: dense %d, overlap %d of %d\n", h.maps[0],
                                     gs[gi].maps[0], dense, pl.overlap(h.fp, sx, sy, 0), h.fp.chunks);
                    // A confirmed seam always fits (its overlap is the repeated border);
                    // seams go first, most votes first.
                    if (e.votes) dense = -e.votes;
                    else if (dense > kMaxDenseOverlap || pl.overlap(h.fp, sx, sy, 0) * 10 > h.fp.chunks * 6) continue;
                    if (dense < best_d) { best_d = dense; best_g = e.to; best_e = &e; bdx = dx; bdy = dy; }
                }
            }
            if (best_g < 0) return;
            char buf[64];
            if (best_d >= 0) std::snprintf(buf, sizeof buf, " [%d content chunks shared]", best_d);
            else buf[0] = 0;
            place(best_g, bdx, bdy, "data: " + best_e->why + buf, true);
        }
    };
    // Places one unplaced neighbour of a placed group beside its entrance; true if any.
    auto place_beside = [&]() {
        for (size_t gi = 0; gi < gs.size(); ++gi) {
            if (!gs[gi].placed) continue;
            for (const E& e : edges[gi]) {
                G& h = gs[size_t(e.to)];
                if (h.placed) continue;
                int dx = gs[gi].dx + e.ox, dy = gs[gi].dy + e.oy;
                int sx = round_chunks(dx), sy = round_chunks(dy);
                int ox = sx, oy = sy;
                pl.free_spot(h.fp, sx, sy);
                char buf[96];
                std::snprintf(buf, sizeof buf, "rule: beside its entrance, moved %d,%d chunks off the teleport offset; ",
                              sx - ox, sy - oy);
                place(e.to, dx + (sx - ox) * 512, dy + (sy - oy) * 512, buf + e.why, false);
                return true;
            }
        }
        return false;
    };

    // Root: the group with central Tenebrae (map 40) stays at its own coordinates.
    int root = gof.count(40) ? gof[40] : 0;
    place(root, 0, 0, "root: original coordinates", true);
    for (;;) {
        spread_exact();
        if (place_beside()) continue;
        // Unlinked groups (reached only by usecode teleports, or not at all): largest first,
        // at the free spot nearest the middle of what is placed so far.
        int best = -1;
        for (size_t gi = 0; gi < gs.size(); ++gi)
            if (!gs[gi].placed && (best < 0 || gs[gi].fp.chunks > gs[size_t(best)].fp.chunks)) best = int(gi);
        if (best < 0) break;
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (const G& g : gs)
            if (g.placed) {
                x0 = std::min(x0, g.fp.cx0 + round_chunks(g.dx)); x1 = std::max(x1, g.fp.cx1 + round_chunks(g.dx));
                y0 = std::min(y0, g.fp.cy0 + round_chunks(g.dy)); y1 = std::max(y1, g.fp.cy1 + round_chunks(g.dy));
            }
        const MapFootprint& f = gs[size_t(best)].fp;
        int sx = (x0 + x1) / 2 - (f.cx0 + f.cx1) / 2, sy = (y0 + y1) / 2 - (f.cy0 + f.cy1) / 2;
        pl.free_spot(f, sx, sy);
        place(best, sx * 512, sy * 512, "guess: no teleport link to the placed maps; packed at the nearest free spot", false);
    }

    AtlasLayout out;
    out.comment =
        "Pagan world atlas layout. Each group's maps are drawn with every item moved by offset [dx, dy] "
        "(world units; +x is screen right-down, +y is screen left-down). Edit offsets freely; "
        "'derived' says where each came from. Regenerate with: pagan --write-atlas-layout FILE";
    for (const G& g : gs) {
        AtlasGroup a;
        a.maps = g.maps;
        std::sort(a.maps.begin(), a.maps.end());
        a.name = "map";
        for (size_t i = 0; i < a.maps.size(); ++i) a.name += (i ? "_" : "") + std::to_string(a.maps[i]);
        const char* n = known_name(a.maps[0]);
        a.label = std::string(a.maps.size() > 1 ? "Maps " : "Map ");
        for (size_t i = 0; i < a.maps.size(); ++i) a.label += (i ? ", " : "") + std::to_string(a.maps[i]);
        if (n) a.label += std::string(" ") + n;
        a.dx = g.dx; a.dy = g.dy;
        a.derived = g.why;
        a.from_data = g.data;
        out.groups.push_back(a);
    }
    std::sort(out.groups.begin(), out.groups.end(), [](const AtlasGroup& a, const AtlasGroup& b) { return a.maps[0] < b.maps[0]; });
    return out;
}

std::vector<std::pair<unsigned, std::bitset<64 * 64>>> atlas_exclusions(const AtlasLayout& layout,
                                                                        const std::vector<MapFootprint>& fps) {
    std::map<unsigned, const MapFootprint*> fp;
    for (const MapFootprint& f : fps) fp[f.map] = &f;
    struct Owner { int count = -1; unsigned map = 0; };
    std::unordered_map<long long, Owner> owner;
    auto each = [&](auto fn) {
        for (const AtlasGroup& g : layout.groups)
            for (unsigned m : g.maps) {
                auto it = fp.find(m);
                if (it == fp.end()) continue;
                const MapFootprint& f = *it->second;
                int sx = round_chunks(g.dx), sy = round_chunks(g.dy);
                // Layers (the stitched world) never compete: the key carries the layer.
                long long lk = (long long)g.layer * 1000000000000LL;
                for (int cy = f.cy0; cy <= f.cy1; ++cy)
                    for (int cx = f.cx0; cx <= f.cx1; ++cx)
                        if (f.has(cx, cy)) fn(m, cx, cy, lk + cell_key(cx + sx, cy + sy), int(f.count[size_t(cy) * 64 + cx]));
            }
    };
    std::map<unsigned, std::bitset<64 * 64>> ex;
    if (!layout.seam_pairs.empty()) {
        // A stitched layout: per chunk, a map gives way only to a seam partner with more there.
        std::set<std::pair<unsigned, unsigned>> partner;
        for (auto [a, b] : layout.seam_pairs) { partner.insert({a, b}); partner.insert({b, a}); }
        std::unordered_map<long long, std::vector<std::pair<unsigned, int>>> at;
        each([&](unsigned m, int, int, long long k, int c) { at[k].push_back({m, c}); });
        each([&](unsigned m, int cx, int cy, long long k, int c) {
            for (auto [o, oc] : at[k])
                if (o != m && partner.count({m, o}) && (oc > c || (oc == c && o < m))) {
                    ex[m][size_t(cy) * 64 + cx] = true;
                    break;
                }
        });
        return {ex.begin(), ex.end()};
    }
    each([&](unsigned m, int, int, long long k, int c) {
        Owner& o = owner[k];
        if (c > o.count) { o.count = c; o.map = m; }
    });
    each([&](unsigned m, int cx, int cy, long long k, int) {
        if (owner[k].map != m) ex[m][size_t(cy) * 64 + cx] = true;
    });
    return {ex.begin(), ex.end()};
}

int atlas_seam_votes(const GameData& d, unsigned a, unsigned b, int ox, int oy, int& bx, int& by) {
    return seam_votes(parse_item_records(d.fixed.entry(a)), parse_item_records(d.fixed.entry(b)), ox, oy, bx, by);
}

std::vector<ItemRecord> atlas_top_level_records(const GameData& d, unsigned m) { return top_level_records(d, m); }

std::string AtlasLayout::to_json() const {
    using nlohmann::json;
    std::ostringstream o;
    o << "{\n  \"comment\": " << json(comment).dump() << ",\n  \"groups\": [\n";
    for (size_t i = 0; i < groups.size(); ++i) {
        const AtlasGroup& g = groups[i];
        o << "    {\"name\": " << json(g.name).dump() << ", \"label\": " << json(g.label).dump() << ", \"maps\": "
          << json(g.maps).dump() << ", \"offset\": [" << g.dx << ", " << g.dy;
        if (g.dz) o << ", " << g.dz;  // [dx, dy] or [dx, dy, dz]
        o << "]";
        if (g.layer) o << ", \"layer\": " << g.layer;
        if (!g.extra.empty()) {
            json x = json::parse(g.extra, nullptr, false);
            if (x.is_object())
                for (auto& [k, v] : x.items()) o << ", " << json(k).dump() << ": " << v.dump();
        }
        o << ",\n     \"source\": \""
          << (g.from_data ? "data" : "manual") << "\", \"derived\": " << json(g.derived).dump() << "}"
          << (i + 1 < groups.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

bool AtlasLayout::from_json(const std::string& text, std::string& err) {
    using nlohmann::json;
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not a JSON object"; return false; }
    if (!j.contains("groups") || !j["groups"].is_array()) { err = "no \"groups\" array"; return false; }
    AtlasLayout l;
    l.comment = j.value("comment", "");
    std::set<unsigned> seen;
    for (const json& g : j["groups"]) {
        if (!g.is_object()) { err = "a group is not an object"; return false; }
        AtlasGroup a;
        a.name = g.value("name", "");
        a.label = g.value("label", a.name);
        a.derived = g.value("derived", "");
        a.from_data = g.value("source", "") == "data";
        if (!g.contains("maps") || !g["maps"].is_array() || g["maps"].empty()) {
            err = "group \"" + a.name + "\" has no maps";
            return false;
        }
        for (const json& m : g["maps"]) {
            if (!m.is_number_unsigned() || m.get<unsigned>() > 255) { err = "group \"" + a.name + "\": bad map number"; return false; }
            unsigned mn = m.get<unsigned>();
            if (!seen.insert(mn).second) { err = "map " + std::to_string(mn) + " is in two groups"; return false; }
            a.maps.push_back(mn);
        }
        if (g.contains("offset")) {
            const json& o = g["offset"];
            if (!o.is_array() || o.size() < 2 || o.size() > 3 || !o[0].is_number_integer() || !o[1].is_number_integer() ||
                (o.size() == 3 && !o[2].is_number_integer())) {
                err = "group \"" + a.name + "\": offset must be [dx, dy] or [dx, dy, dz]";
                return false;
            }
            a.dx = o[0].get<int>();
            a.dy = o[1].get<int>();
            if (o.size() == 3) a.dz = o[2].get<int>();
        }
        if (g.contains("layer")) {
            if (!g["layer"].is_number_integer()) { err = "group \"" + a.name + "\": layer must be an integer"; return false; }
            a.layer = g["layer"].get<int>();
        }
        json x = json::object();
        for (auto& [k, v] : g.items())
            if (k != "name" && k != "label" && k != "maps" && k != "offset" && k != "layer" && k != "source" && k != "derived")
                x[k] = v;
        if (!x.empty()) a.extra = x.dump();
        l.groups.push_back(std::move(a));
    }
    if (j.contains("seams") && j["seams"].is_array())
        for (const json& sm : j["seams"])
            if (sm.is_object() && sm.contains("maps") && sm["maps"].is_array() && sm["maps"].size() == 2 &&
                sm.value("mode", std::string("walk")) != "teleport")
                l.seam_pairs.push_back({sm["maps"][0].get<unsigned>(), sm["maps"][1].get<unsigned>()});
    *this = std::move(l);
    return true;
}

bool AtlasLayout::load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str(), err);
}

bool AtlasLayout::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << to_json();
    return bool(f);
}

}  // namespace pg
