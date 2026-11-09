// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/dresser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include "editor/json_util.h"

namespace pg {

namespace {

// ---------------------------------------------------------------------------------------------
// Deterministic random numbers: a splitmix64 stream per (seed, stage, a, b), so one stage's
// choices never shift another's.

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return uint32_t((z ^ (z >> 31)) >> 32);
    }
    double uniform() { return next() / 4294967296.0; }
    int range(int n) { return n <= 0 ? 0 : int(next() % uint32_t(n)); }
};
Rng rng_at(uint32_t seed, uint32_t stage, int a = 0, int b = 0) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : {uint64_t(seed), uint64_t(stage), uint64_t(uint32_t(a)), uint64_t(uint32_t(b))}) {
        h ^= v;
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    Rng r(h);
    r.next();
    return r;
}
enum Stage : uint32_t { kStFloor = 1, kStWall, kStPost, kStDoor, kStRoof, kStLedge, kStPoi, kStScatter, kStLight, kStWater };

const KitPiece* pick(const std::vector<KitPiece>& v, Rng& r, const KitPiece* avoid = nullptr) {
    if (v.empty()) return nullptr;
    double tot = 0;
    for (const KitPiece& p : v) tot += std::max(0.0, p.weight) * (avoid && p.shape == avoid->shape && p.frame == avoid->frame && v.size() > 1 ? 0.1 : 1.0);
    if (tot <= 0) return &v[size_t(r.range(int(v.size())))];
    double x = r.uniform() * tot;
    for (const KitPiece& p : v) {
        x -= std::max(0.0, p.weight) * (avoid && p.shape == avoid->shape && p.frame == avoid->frame && v.size() > 1 ? 0.1 : 1.0);
        if (x < 0) return &p;
    }
    return &v.back();
}

bool floorlike(char c) { return c == '.' || c == '#' || c == '+' || c == '/' || c == '='; }
bool line_cell(char c) { return c == '#' || c == '+' || c == '/'; }
bool is_void(char c) { return c == ' ' || c == 'x'; }

std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s + " ") {
        if (std::isalnum(unsigned(c))) w += char(std::tolower(unsigned(c)));
        else if (!w.empty()) {
            out.push_back(w);
            w.clear();
        }
    }
    return out;
}
std::string stem(std::string w) {
    if (w.size() > 4 && w.compare(w.size() - 3, 3, "ies") == 0) return w.substr(0, w.size() - 3) + "y";
    if (w.size() > 3 && w.back() == 's' && w[w.size() - 2] != 's') w.pop_back();
    return w;
}

// Item boxes from the kit's shape table (stored corner = max x, max y, min z).
struct Box {
    int x0, y0, z0, x1, y1, z1;  // inclusive x/y, z1 = top (z0 + zd)
};
Box box_of(const TileKit& kit, const DressedItem& it) {
    const ShapeEntry* e = kit.shape(it.shape);
    int xd = e ? e->fp[0] : 32, yd = e ? e->fp[1] : 32, zd = e ? e->fp[2] : 0;
    if (it.flags & kItemFlipped) std::swap(xd, yd);
    return {it.x - xd + 1, it.y - yd + 1, it.z, it.x, it.y, it.z + zd};
}
uint16_t tflags_of(const TileKit& kit, unsigned shape) {
    const ShapeEntry* e = kit.shape(shape);
    return e ? e->tflags : 0;
}
constexpr uint16_t kTfSolid = 0x0002, kTfLand = 0x0008, kTfRoof = 0x0400, kTfDamaging = 0x0040;
bool volumes_overlap(const Box& a, const Box& b) {
    if (a.z1 <= a.z0 || b.z1 <= b.z0) return false;  // flat items have no volume
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1 && a.z0 < b.z1 && b.z0 < a.z1;
}

// A spatial hash of placed items for overlap checks.
struct ItemIndex {
    const TileKit& kit;
    std::unordered_map<uint64_t, std::vector<size_t>> cells;
    const std::vector<DressedItem>* items = nullptr;
    explicit ItemIndex(const TileKit& k) : kit(k) {}
    static uint64_t key(int cx, int cy) { return (uint64_t(uint32_t(cx + 1000)) << 32) | uint32_t(cy + 1000); }
    void add(size_t i) {
        Box b = box_of(kit, (*items)[i]);
        for (int cy = b.y0 / 128; cy <= b.y1 / 128; ++cy)
            for (int cx = b.x0 / 128; cx <= b.x1 / 128; ++cx) cells[key(cx, cy)].push_back(i);
    }
    // Any solid item whose volume intersects box b (b itself treated as solid)?
    bool solid_overlap(const Box& b, size_t* hit = nullptr) const {
        if (b.z1 <= b.z0) return false;
        for (int cy = b.y0 / 128; cy <= b.y1 / 128; ++cy)
            for (int cx = b.x0 / 128; cx <= b.x1 / 128; ++cx) {
                auto f = cells.find(key(cx, cy));
                if (f == cells.end()) continue;
                for (size_t i : f->second) {
                    const DressedItem& o = (*items)[i];
                    if (!(tflags_of(kit, o.shape) & (kTfSolid | kTfDamaging))) continue;
                    if (volumes_overlap(b, box_of(kit, o))) {
                        if (hit) *hit = i;
                        return true;
                    }
                }
            }
        return false;
    }
};

struct Dresser {
    const PaintedArea& A;
    const TileKit& K;
    uint32_t seed;
    DressOptions opt;
    DressResult R;
    ItemIndex index;
    std::vector<const ThemeKit*> theme;   // per cell
    std::vector<uint8_t> interior;        // per cell: enclosed by walls
    std::vector<int> poi_used;            // per cell: POI index + 1 occupying it
    std::vector<int> wall_room;           // cells bordering a room (roofs)
    std::map<std::string, ThemeKit> overridden;  // themes with the area's "styles" applied

    Dresser(const PaintedArea& a, const TileKit& k, uint32_t s, const DressOptions& o) : A(a), K(k), seed(s), opt(o), index(k) {
        index.items = &R.items;
    }
    int idx(int x, int y) const { return y * A.w + x; }

    size_t add(const DressedItem& it) {
        R.items.push_back(it);
        index.add(R.items.size() - 1);
        return R.items.size() - 1;
    }
    DressedItem make(const KitPiece& p, int x, int y, int z, const char* role, int cx, int cy) const {
        DressedItem it;
        it.shape = p.shape;
        it.frame = p.frame;
        it.x = x;
        it.y = y;
        it.z = z;
        it.role = role;
        it.cx = cx;
        it.cy = cy;
        return it;
    }
    // Adds unless a solid volume would overlap something solid already placed.
    bool try_add(const DressedItem& it) {
        Box b = box_of(K, it);
        if ((tflags_of(K, it.shape) & (kTfSolid | kTfDamaging)) && index.solid_overlap(b)) return false;
        add(it);
        return true;
    }

    const WallStyle* style(const ThemeKit* t, bool ledge = false) const {
        const WallStyle* w = t ? K.wall(ledge ? t->ledge : t->wall) : nullptr;
        if (!w && t) w = K.wall(t->wall);
        if (!w && !K.walls.empty()) w = &K.walls[0];
        return w;
    }
    // Pieces of length l (32-unit steps) along x (ew) or y; falls back to other styles.
    const std::vector<KitPiece>* pieces(const WallStyle* w, bool ew, int l) const {
        if (w) {
            const auto& v = ew ? w->ew[size_t(l)] : w->ns[size_t(l)];
            if (!v.empty()) return &v;
        }
        // Fallback: the most used style with this piece and the same height.
        for (const WallStyle& o : K.walls) {
            if (w && o.height != w->height) continue;
            const auto& v = ew ? o.ew[size_t(l)] : o.ns[size_t(l)];
            if (!v.empty()) return &v;
        }
        return nullptr;
    }

    void run();
    void themes_and_rooms();
    void floors();
    void walls();
    void roofs();
    void ledges();
    void pois();
    void scatter();
};

// --- Themes per cell and enclosed rooms ---------------------------------------------------------

void Dresser::themes_and_rooms() {
    theme.assign(size_t(A.w * A.h), nullptr);
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            const std::string& t = A.at(x, y).theme;
            const ThemeKit* tk = K.theme(t.empty() ? A.theme : t);
            if (tk && !A.styles.empty()) {
                auto f = overridden.find(tk->name);
                if (f == overridden.end()) {
                    ThemeKit o = *tk;
                    for (auto& [k, v] : A.styles) {
                        bool wall = k == "wall" || k == "ledge";
                        if (wall ? !K.wall(v) : !K.floor(v)) {
                            R.warnings.push_back("styles: unknown " + k + " '" + v + "'");
                            continue;
                        }
                        if (k == "wall") o.wall = v;
                        else if (k == "ledge") o.ledge = v;
                        else if (k == "floor") o.floor = v;
                        else if (k == "interior_floor") o.interior_floor = v;
                        else if (k == "water") o.water = v;
                        else if (k == "roof") o.roof = v;
                        else R.warnings.push_back("styles: unknown key '" + k + "'");
                    }
                    f = overridden.emplace(tk->name, o).first;
                }
                tk = &f->second;
            }
            theme[size_t(idx(x, y))] = tk;
            if (!K.themes.count(t.empty() ? A.theme : t)) {
                std::string w = "unknown theme '" + (t.empty() ? A.theme : t) + "', using " + (tk ? tk->name : "none");
                if (std::find(R.warnings.begin(), R.warnings.end(), w) == R.warnings.end()) R.warnings.push_back(w);
            }
        }
    // Flood from outside the grid through everything but wall lines; unreached floor = interior.
    std::vector<uint8_t> seen(size_t((A.w + 2) * (A.h + 2)), 0);
    auto sidx = [&](int x, int y) { return size_t((y + 1) * (A.w + 2) + (x + 1)); };
    std::vector<std::pair<int, int>> stack = {{-1, -1}};
    seen[sidx(-1, -1)] = 1;
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            int nx = x + dx[d], ny = y + dy[d];
            if (nx < -1 || ny < -1 || nx > A.w || ny > A.h || seen[sidx(nx, ny)]) continue;
            char c = A.inside(nx, ny) ? A.kind(nx, ny) : ' ';
            if (c == '#' || c == '+') continue;
            seen[sidx(nx, ny)] = 1;
            stack.push_back({nx, ny});
        }
    }
    interior.assign(size_t(A.w * A.h), 0);
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x)
            if (!seen[sidx(x, y)] && !line_cell(A.kind(x, y)) && !is_void(A.kind(x, y))) interior[size_t(idx(x, y))] = 1;
    // Wall cells take the interior floor when all their open neighbours are interior.
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (!line_cell(A.kind(x, y))) continue;
            int in = 0, out = 0;
            const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                int nx = x + dx[d], ny = y + dy[d];
                if (!A.inside(nx, ny) || is_void(A.kind(nx, ny))) { ++out; continue; }
                if (line_cell(A.kind(nx, ny))) continue;
                (interior[size_t(idx(nx, ny))] ? in : out)++;
            }
            if (in > 0 && out == 0) interior[size_t(idx(x, y))] = 2;
        }
}

// --- Floors: weighted, neighbour-aware fills ---------------------------------------------------

struct FillModel {
    const FloorSet* fs = nullptr;
    std::map<std::pair<int, int>, std::array<int, 2>> pair;  // (a, b) -> (h, v)
    std::vector<double> hsum, vsum;
    void build(const FloorSet* f) {
        fs = f;
        hsum.assign(f->tiles.size(), 0);
        vsum.assign(f->tiles.size(), 0);
        for (auto& p : f->pairs) {
            pair[{p[0], p[1]}] = {p[2], p[3]};
            hsum[size_t(p[0])] += p[2];
            vsum[size_t(p[0])] += p[3];
        }
    }
    int find(const DressedItem* it) const {
        if (!it) return -1;
        for (size_t i = 0; i < fs->tiles.size(); ++i)
            if (fs->tiles[i].shape == it->shape && fs->tiles[i].frame == it->frame) return int(i);
        return -1;
    }
};

void Dresser::floors() {
    std::map<std::string, FillModel> models;
    auto model = [&](const std::string& id) -> const FillModel* {
        if (id.empty()) return nullptr;
        auto it = models.find(id);
        if (it != models.end()) return it->second.fs ? &it->second : nullptr;
        const FloorSet* f = K.floor(id);
        FillModel& m = models[id];
        if (f && !f->tiles.empty()) m.build(f);
        return m.fs ? &m : nullptr;
    };
    std::vector<long> placed(size_t(A.w * A.h), -1);  // item index of the cell's tile
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            char c = A.kind(x, y);
            if (is_void(c)) continue;
            const ThemeKit* t = theme[size_t(idx(x, y))];
            if (!t) continue;
            bool water = c == '~';
            std::string set = water ? t->water : (interior[size_t(idx(x, y))] && !t->interior_floor.empty() ? t->interior_floor : t->floor);
            const FillModel* m = model(set);
            if (!m) {
                // Water without a water set: use lava (lava themes), else skip with a warning.
                if (water) m = model(t->lava);
                if (!m) {
                    R.warnings.push_back("no floor set for cell " + std::to_string(x) + "," + std::to_string(y));
                    continue;
                }
            }
            int z = A.z(x, y);
            const FloorSet& fs = *m->fs;
            // Neighbours already placed: west and north (same set and height).
            const DressedItem* west = nullptr;
            const DressedItem* north = nullptr;
            auto same = [&](int nx, int ny) -> const DressedItem* {
                if (!A.inside(nx, ny) || placed[size_t(idx(nx, ny))] < 0) return nullptr;
                const DressedItem& o = R.items[size_t(placed[size_t(idx(nx, ny))])];
                return o.z == z && m->find(&o) >= 0 ? &o : nullptr;
            };
            west = same(x - 1, y);
            north = same(x, y - 1);
            Rng r = rng_at(seed, kStFloor, x, y);
            const std::vector<KitPiece>* cand = &fs.tiles;
            // Water and lava edges: the tiles the original uses with this neighbour mask.
            std::vector<KitPiece> edge;
            if (fs.kind == "water" || fs.kind == "lava") {
                int mask = 0;
                const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
                for (int s = 0; s < 4; ++s) {
                    int nx = x + dx[s], ny = y + dy[s];
                    bool same_kind = !A.inside(nx, ny) || A.kind(nx, ny) == c || is_void(A.kind(nx, ny));
                    if (same_kind) mask |= 1 << s;
                }
                if (mask != 15 && !fs.edge[size_t(mask)].empty()) {
                    edge = fs.edge[size_t(mask)];
                    cand = &edge;
                }
            }
            // Shores: water next to land takes the theme's shore tiles for its wet-neighbour mask.
            if (water) {
                int mask = 0;
                const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
                for (int s = 0; s < 4; ++s) {
                    int nx = x + dx[s], ny = y + dy[s];
                    if (!A.inside(nx, ny) || A.kind(nx, ny) == '~' || is_void(A.kind(nx, ny))) mask |= 1 << s;
                }
                if (mask != 15 && !t->shore[size_t(mask)].empty()) {
                    const KitPiece* k = pick(t->shore[size_t(mask)], r);
                    placed[size_t(idx(x, y))] = long(add(make(*k, x * A.cell + 127, y * A.cell + 127, z, "water", x, y)));
                    continue;
                }
            }
            const KitPiece* chosen = nullptr;
            if (cand == &fs.tiles) {
                // Score = weight^0.5 x P(b | west) x P(b | north); identical neighbours are avoided.
                int wi = m->find(west), ni = m->find(north);
                std::vector<double> sc(fs.tiles.size());
                double tot = 0;
                for (size_t i = 0; i < fs.tiles.size(); ++i) {
                    double s = std::sqrt(std::max(1e-6, fs.tiles[i].weight));
                    if (wi >= 0) {
                        auto f = m->pair.find({wi, int(i)});
                        s *= (f != m->pair.end() ? f->second[0] : 0) + 0.05 * (1 + m->hsum[size_t(wi)] / fs.tiles.size());
                    }
                    if (ni >= 0) {
                        auto f = m->pair.find({ni, int(i)});
                        s *= (f != m->pair.end() ? f->second[1] : 0) + 0.05 * (1 + m->vsum[size_t(ni)] / fs.tiles.size());
                    }
                    if ((wi == int(i) || ni == int(i)) && fs.tiles.size() > 1) s *= 0.04;
                    sc[i] = s;
                    tot += s;
                }
                double v = r.uniform() * tot;
                for (size_t i = 0; i < sc.size(); ++i) {
                    v -= sc[i];
                    if (v < 0) { chosen = &fs.tiles[i]; break; }
                }
                if (!chosen) chosen = &fs.tiles.back();
            } else {
                const DressedItem* prev = west ? west : north;
                KitPiece avoid{prev ? prev->shape : uint16_t(0), prev ? prev->frame : uint16_t(0), 1};
                chosen = pick(*cand, r, prev ? &avoid : nullptr);
            }
            int fp = fs.footprint;
            DressedItem it = make(*chosen, x * A.cell + fp - 1, y * A.cell + fp - 1, z, water ? "water" : "floor", x, y);
            placed[size_t(idx(x, y))] = long(add(it));
        }
}

// --- Walls: posts at junctions and ends, straight runs between, doors and openings --------------
//
// The wall line of a row of cells runs through 32-unit slot 1 of each cell (cell x covers slots
// 4x .. 4x+3): a cell's post stands at slot 4x+1 (x 128x+32 .. 128x+63) and a link to the east
// neighbour fills slots 4x+2 .. 4x+4, up to that neighbour's post. Columns are the same in y.

void Dresser::walls() {
    enum Slot : uint8_t { kNone, kWall, kPost, kDoor, kJamb, kOpen };
    auto link = [&](int x, int y, int dx, int dy) {
        return A.inside(x, y) && A.inside(x + dx, y + dy) && line_cell(A.kind(x, y)) && line_cell(A.kind(x + dx, y + dy));
    };
    // Per line: slot kinds; lines indexed by row (horizontal) or column (vertical).
    std::vector<std::vector<uint8_t>> hl(size_t(A.h), std::vector<uint8_t>(size_t(A.w * 4 + 8), kNone));
    std::vector<std::vector<uint8_t>> vl(size_t(A.w), std::vector<uint8_t>(size_t(A.h * 4 + 8), kNone));
    std::vector<uint8_t> post(size_t(A.w * A.h), 0);  // 1 post, 2 jamb
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            char c = A.kind(x, y);
            if (!line_cell(c)) continue;
            bool e = link(x, y, 1, 0), w = link(x, y, -1, 0), s = link(x, y, 0, 1), n = link(x, y, 0, -1);
            bool h = e || w, v = n || s;
            auto& H = hl[size_t(y)];
            auto& V = vl[size_t(x)];
            if (e)
                for (int k = 2; k <= 4; ++k) H[size_t(4 * x + k)] = kWall;
            if (s)
                for (int k = 2; k <= 4; ++k) V[size_t(4 * y + k)] = kWall;
            bool straight = (e && w && !v) || (n && s && !h);
            if (h) H[size_t(4 * x + 1)] = kWall;
            if (v) V[size_t(4 * y + 1)] = kWall;
            if (!straight) post[size_t(idx(x, y))] = 1;  // corner, T, cross, end or lone post
        }
    // Doors and passages cut the lines.
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            char c = A.kind(x, y);
            if (c != '+' && c != '/') continue;
            bool e = link(x, y, 1, 0), w = link(x, y, -1, 0), s = link(x, y, 0, 1), n = link(x, y, 0, -1);
            bool horiz = (e && w) || ((e || w) && !(n && s));
            bool vert = !horiz && (n || s);
            R.entrances.push_back({x, y});
            if (!horiz && !vert) {
                R.warnings.push_back(std::string(c == '+' ? "door" : "passage") + " at " + std::to_string(x) + "," +
                                     std::to_string(y) + " is not in a wall line");
                post[size_t(idx(x, y))] = 0;
                continue;
            }
            auto& L = horiz ? hl[size_t(y)] : vl[size_t(x)];
            int p = horiz ? x : y;
            post[size_t(idx(x, y))] = 0;
            if (c == '+') {
                // Door over slots 4p+1 .. 4p+4, jambs at 4p and 4p+5.
                for (int k = 1; k <= 4; ++k) L[size_t(4 * p + k)] = kDoor;
                L[size_t(4 * p)] = kJamb;
                L[size_t(4 * p + 5)] = kJamb;
                int ex = horiz ? x + 1 : x, ey = horiz ? y : y + 1;
                if (A.inside(ex, ey) && line_cell(A.kind(ex, ey))) post[size_t(idx(ex, ey))] = 2;
                else R.warnings.push_back("door at " + std::to_string(x) + "," + std::to_string(y) + " has no wall on its far side");
                if (!(horiz ? w : n)) R.warnings.push_back("door at " + std::to_string(x) + "," + std::to_string(y) + " has no wall on its near side");
            } else {
                for (int k = 0; k <= 4; ++k) L[size_t(4 * p + k)] = kOpen;
                // The wall ends either side get end posts.
                int ax = horiz ? x - 1 : x, ay = horiz ? y : y - 1, bx = horiz ? x + 1 : x, by = horiz ? y : y + 1;
                if (A.inside(ax, ay) && A.kind(ax, ay) == '#') post[size_t(idx(ax, ay))] = 1;
                if (A.inside(bx, by) && A.kind(bx, by) == '#') post[size_t(idx(bx, by))] = 1;
            }
            // The cross direction of the door cell keeps nothing.
            auto& C = horiz ? vl[size_t(x)] : hl[size_t(y)];
            int q = horiz ? y : x;
            if (C[size_t(4 * q + 1)] == kWall) C[size_t(4 * q + 1)] = kNone;
        }
    // Posts and jambs.
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            uint8_t p = post[size_t(idx(x, y))];
            if (!p) continue;
            const WallStyle* ws = style(theme[size_t(idx(x, y))]);
            if (!ws) continue;
            Rng r = rng_at(seed, kStPost, x, y);
            const std::vector<KitPiece>& pv = (p == 2 && !ws->jamb.empty()) ? ws->jamb : ws->post;
            const std::vector<KitPiece>* use = &pv;
            if (use->empty()) use = pieces(ws, true, 1);
            if (!use || use->empty()) continue;
            const KitPiece* k = pick(*use, r);
            add(make(*k, 128 * x + 63, 128 * y + 63, A.z(x, y), p == 2 ? "jamb" : "post", x, y));
            hl[size_t(y)][size_t(4 * x + 1)] = kPost;
            vl[size_t(x)][size_t(4 * y + 1)] = kPost;
        }
    // Doors and jambs on the slots (the jamb at 4p is placed here; the far one is the next post).
    auto place_line = [&](bool horiz, int line) {
        auto& L = horiz ? hl[size_t(line)] : vl[size_t(line)];
        int n = int(L.size());
        auto cell_of = [&](int slot, int& cx, int& cy) {
            int c = std::clamp(slot / 4, 0, (horiz ? A.w : A.h) - 1);
            cx = horiz ? c : line;
            cy = horiz ? line : c;
        };
        auto put = [&](const KitPiece& k, int a, int b, const char* role) {
            // Slots a..b along the line.
            int cx, cy;
            cell_of(a, cx, cy);
            int z = A.z(cx, cy);
            int fixed = 128 * line + 63;
            int along = 32 * b + 31;
            DressedItem it = make(k, horiz ? along : fixed, horiz ? fixed : along, z, role, cx, cy);
            if (!try_add(it)) R.warnings.push_back(std::string(role) + " piece at " + std::to_string(cx) + "," + std::to_string(cy) + " overlaps");
        };
        for (int a = 0; a < n;) {
            uint8_t k = L[size_t(a)];
            if (k == kDoor) {
                int b = a;
                while (b + 1 < n && L[size_t(b + 1)] == kDoor) ++b;
                int cx, cy;
                cell_of(a, cx, cy);
                const WallStyle* ws = style(theme[size_t(idx(cx, cy))]);
                KitPiece d = ws ? (horiz ? ws->door_ew : ws->door_ns) : KitPiece{uint16_t(horiz ? 69 : 68), 0, 1};
                if (!d.shape) d = {uint16_t(horiz ? 69 : 68), 0, 1};
                DressedItem it = make(d, horiz ? 32 * b + 31 : 128 * line + 63, horiz ? 128 * line + 63 : 32 * b + 31, A.z(cx, cy), "door", cx, cy);
                it.quality = 0;  // unlocked, closed
                if (!try_add(it)) R.warnings.push_back("door at " + std::to_string(cx) + "," + std::to_string(cy) + " overlaps");
                a = b + 1;
                continue;
            }
            if (k == kJamb) {
                int cx, cy;
                cell_of(a, cx, cy);
                const WallStyle* ws = style(theme[size_t(idx(cx, cy))]);
                Rng r = rng_at(seed, kStDoor, line * 2 + horiz, a);
                const std::vector<KitPiece>* jv = ws && !ws->jamb.empty() ? &ws->jamb : ws && !ws->post.empty() ? &ws->post : pieces(ws, horiz, 1);
                if (jv && !jv->empty()) put(*pick(*jv, r), a, a, "jamb");
                ++a;
                continue;
            }
            if (k != kWall) {
                ++a;
                continue;
            }
            // A run of wall slots, split where the floor height changes.
            int b = a;
            int cx0, cy0;
            cell_of(a, cx0, cy0);
            while (b + 1 < n && L[size_t(b + 1)] == kWall) {
                int cx, cy;
                cell_of(b + 1, cx, cy);
                if (A.z(cx, cy) != A.z(cx0, cy0)) break;
                ++b;
            }
            const WallStyle* ws = style(theme[size_t(idx(cx0, cy0))]);
            Rng r = rng_at(seed, kStWall, line * 2 + horiz, a);
            const KitPiece* last = nullptr;
            int s = a;
            while (s <= b) {
                int left = b - s + 1;
                int l = std::min(4, left);
                // Prefer full pieces; leave room so the remainder exists in the style.
                while (l > 1 && !pieces(ws, horiz, l)) --l;
                const std::vector<KitPiece>* pv = pieces(ws, horiz, l);
                if (!pv && ws && !ws->post.empty()) pv = &ws->post, l = 1;
                if (!pv) break;
                const KitPiece* kp = pick(*pv, r, last);
                put(*kp, s, s + l - 1, "wall");
                last = kp;
                s += l;
            }
            a = b + 1;
        }
    };
    for (int y = 0; y < A.h; ++y) place_line(true, y);
    for (int x = 0; x < A.w; ++x) place_line(false, x);
}

// --- Roofs over enclosed rooms -------------------------------------------------------------------

void Dresser::roofs() {
    if (!opt.roofs) return;
    // Roof cells: rooms, and the wall cells around them.
    std::vector<uint8_t> want(size_t(A.w * A.h), 0);
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (interior[size_t(idx(x, y))]) { want[size_t(idx(x, y))] = 1; continue; }
            if (!line_cell(A.kind(x, y))) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (A.inside(x + dx, y + dy) && interior[size_t(idx(x + dx, y + dy))] == 1) want[size_t(idx(x, y))] = 1;
        }
    // One roof tile per building (a connected roof region), so a roof keeps one colour.
    std::vector<int> region(size_t(A.w * A.h), -1);
    int regions = 0;
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (!want[size_t(idx(x, y))] || region[size_t(idx(x, y))] >= 0) continue;
            const ThemeKit* t = theme[size_t(idx(x, y))];
            const FloorSet* rs = t ? K.floor(t->roof) : nullptr;
            const WallStyle* ws = style(t);
            int id = regions++;
            std::vector<std::pair<int, int>> st = {{x, y}}, cells;
            region[size_t(idx(x, y))] = id;
            while (!st.empty()) {
                auto [cx, cy] = st.back();
                st.pop_back();
                cells.push_back({cx, cy});
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + dx[d], ny = cy + dy[d];
                    if (!A.inside(nx, ny) || !want[size_t(idx(nx, ny))] || region[size_t(idx(nx, ny))] >= 0) continue;
                    region[size_t(idx(nx, ny))] = id;
                    st.push_back({nx, ny});
                }
            }
            if (!rs || rs->tiles.empty() || rs->footprint != 128 || !ws) continue;
            Rng r = rng_at(seed, kStRoof, x, y);
            std::vector<KitPiece> top(rs->tiles.begin(), rs->tiles.begin() + long(std::min<size_t>(4, rs->tiles.size())));
            KitPiece k = *pick(top, r);
            int z = 0;
            for (auto& [cx, cy] : cells) z = std::max(z, A.z(cx, cy) + ws->height);
            if (z > 255) continue;
            std::sort(cells.begin(), cells.end(), [](auto& a, auto& b) { return std::make_pair(a.second, a.first) < std::make_pair(b.second, b.first); });
            for (auto& [cx, cy] : cells) add(make(k, 128 * cx + 127, 128 * cy + 127, z, "roof", cx, cy));
        }
}

// --- Height changes: risers (ledges) along the higher cell's edge --------------------------------

void Dresser::ledges() {
    const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            char c = A.kind(x, y);
            if (!floorlike(c) && c != '~') continue;
            int z = A.z(x, y);
            for (int s = 0; s < 4; ++s) {
                int nx = x + dx[s], ny = y + dy[s];
                int nz;
                if (!A.inside(nx, ny) || is_void(A.kind(nx, ny))) continue;  // edges over void stay open
                nz = A.z(nx, ny);
                int diff = z - nz;
                if (diff <= 8) continue;
                if (A.kind(x, y) == '=' || A.kind(nx, ny) == '=') continue;  // stairs handle their own
                const WallStyle* own = style(theme[size_t(idx(x, y))], true);
                bool ew = s == 0 || s == 2;  // a riser on the N/S edge runs along x
                // Riser heights that add up to the drop: the theme's ledge style first, then any
                // other style with full-length pieces (fewest pieces; exact fits only).
                std::vector<const WallStyle*> cand;
                if (own && pieces(own, ew, 4)) cand.push_back(own);
                for (const WallStyle& o : K.walls)
                    if (&o != own && !o.ew[4].empty() && !o.ns[4].empty() && o.height >= 8) {
                        bool dup = false;
                        for (const WallStyle* c2 : cand) dup |= c2->height == o.height;
                        if (!dup) cand.push_back(&o);
                    }
                std::vector<int> best(size_t(diff + 1), 1 << 20), from(size_t(diff + 1), -1);
                best[0] = 0;
                for (int hgt = 1; hgt <= diff; ++hgt)
                    for (size_t k = 0; k < cand.size(); ++k) {
                        int hh = cand[k]->height;
                        if (hh <= hgt && best[size_t(hgt - hh)] + 1 < best[size_t(hgt)]) best[size_t(hgt)] = best[size_t(hgt - hh)] + 1, from[size_t(hgt)] = int(k);
                    }
                int fill = diff;
                while (fill > 0 && from[size_t(fill)] < 0) fill -= 8;  // the closest exact height below
                if (fill <= 0) continue;
                Rng r = rng_at(seed, kStLedge, idx(x, y), s);
                // Slot on the shared edge, inside the higher cell; stacked so the top is flush.
                int fx = s == 1 ? 128 * x + 127 : s == 3 ? 128 * x + 31 : 128 * x + 127;
                int fy = s == 2 ? 128 * y + 127 : s == 0 ? 128 * y + 31 : 128 * y + 127;
                int top = z;
                for (int hgt = fill; hgt > 0;) {
                    const WallStyle* w = cand[size_t(from[size_t(hgt)])];
                    const std::vector<KitPiece>* pv = pieces(w, ew, 4);
                    if (!pv) break;
                    DressedItem it = make(*pick(*pv, r), fx, fy, top - w->height, "ledge", x, y);
                    try_add(it);
                    top -= w->height;
                    hgt -= w->height;
                }
                if (fill < diff) R.warnings.push_back("drop at " + std::to_string(x) + "," + std::to_string(y) + " only partly filled");
            }
        }
}

// --- Points of interest --------------------------------------------------------------------------

void Dresser::pois() {
    poi_used.assign(size_t(A.w * A.h), 0);
    for (size_t pi = 0; pi < A.pois.size(); ++pi) {
        const PaintedPoi& P = A.pois[pi];
        PoiResult res;
        res.id = P.id.empty() ? "poi" + std::to_string(pi + 1) : P.id;
        res.note = P.note;
        res.x = P.x;
        res.y = P.y;
        if (!A.inside(P.x, P.y) || !floorlike(A.kind(P.x, P.y))) {
            R.warnings.push_back("POI " + res.id + " is not on a floor cell");
            R.pois.push_back(res);
            continue;
        }
        const ThemeKit* t = theme[size_t(idx(P.x, P.y))];
        // Score every prefab: note words +1 (+2 on the label), tags count double, theme +0.5.
        std::vector<std::string> note_words, tag_words;
        for (auto& w : words_of(P.note)) note_words.push_back(stem(w));
        for (auto& tg : P.tags)
            for (auto& w : words_of(tg)) tag_words.push_back(stem(w));
        std::vector<std::pair<double, const Prefab*>> scored;
        for (const Prefab& pf : K.prefabs) {
            if (!P.prefab.empty() && pf.id != P.prefab) continue;
            std::set<std::string> kw, lw;
            for (auto& k : pf.keywords) kw.insert(stem(k));
            for (auto& w : words_of(pf.label)) lw.insert(stem(w));
            double s = 0;
            for (auto& w : note_words) s += (lw.count(w) ? 2 : 0) + (kw.count(w) ? 1 : 0);
            for (auto& w : tag_words) s += 2 * ((lw.count(w) ? 2 : 0) + (kw.count(w) ? 1 : 0));
            if (t && pf.theme == t->name && s > 0) s += 1.5;
            if (!P.prefab.empty()) s += 100;
            if (s > 0.5 || !P.prefab.empty()) scored.push_back({s, &pf});
        }
        std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        // Candidates within 1 point of the best, in a seeded order; try to place each.
        std::vector<const Prefab*> order;
        Rng r = rng_at(seed, kStPoi, int(pi));
        if (!scored.empty()) {
            double best = scored[0].first;
            std::vector<const Prefab*> top, rest;
            for (auto& [s, pf] : scored) (s >= best - 1.0 ? top : rest).push_back(pf);
            for (size_t i = top.size(); i > 1; --i) std::swap(top[i - 1], top[size_t(r.range(int(i)))]);
            order = top;
            order.insert(order.end(), rest.begin(), rest.end());
        }
        int radius = std::max(0, P.radius);
        int z = A.z(P.x, P.y);
        bool done = false;
        for (const Prefab* pf : order) {
            if (done) break;
            int ncx = std::max(1, (pf->sx + 127) / 128), ncy = std::max(1, (pf->sy + 127) / 128);
            // Placements nearest the marker first.
            for (int rr = 0; rr <= radius + 1 && !done; ++rr)
                for (int oy = -rr; oy <= rr && !done; ++oy)
                    for (int ox = -rr; ox <= rr && !done; ++ox) {
                        if (std::max(std::abs(ox), std::abs(oy)) != rr) continue;
                        int cx0 = P.x - (ncx - 1) / 2 + ox, cy0 = P.y - (ncy - 1) / 2 + oy;
                        bool ok = true;
                        for (int yy = cy0; yy < cy0 + ncy && ok; ++yy)
                            for (int xx = cx0; xx < cx0 + ncx && ok; ++xx)
                                ok = A.inside(xx, yy) && A.kind(xx, yy) == '.' && A.z(xx, yy) == z && !poi_used[size_t(idx(xx, yy))];
                        if (!ok) continue;
                        // Centre the prefab in its cells, on the 32 grid.
                        int bx = 128 * cx0 + (((ncx * 128 - pf->sx) / 2) & ~31);
                        int by = 128 * cy0 + (((ncy * 128 - pf->sy) / 2) & ~31);
                        std::vector<DressedItem> its;
                        for (const PrefabItem& pi2 : pf->items) {
                            DressedItem it;
                            it.shape = pi2.shape;
                            it.frame = pi2.frame;
                            it.x = bx + pi2.dx;
                            it.y = by + pi2.dy;
                            it.z = z + pi2.dz;
                            it.flags = pi2.flags;
                            it.quality = pi2.quality;
                            it.npcnum = pi2.npcnum;
                            it.mapnum = pi2.mapnum;
                            it.role = "poi";
                            it.cx = P.x;
                            it.cy = P.y;
                            it.poi = res.id;
                            if (it.z > 255) { ok = false; break; }
                            Box b = box_of(K, it);
                            if ((tflags_of(K, it.shape) & (kTfSolid | kTfDamaging)) && index.solid_overlap(b)) {
                                ok = false;
                                break;
                            }
                            its.push_back(it);
                        }
                        if (!ok || its.empty()) continue;
                        for (const DressedItem& it : its) add(it);
                        for (int yy = cy0; yy < cy0 + ncy; ++yy)
                            for (int xx = cx0; xx < cx0 + ncx; ++xx) poi_used[size_t(idx(xx, yy))] = int(pi) + 1;
                        res.prefab = pf->id;
                        res.label = pf->label;
                        res.placed = true;
                        res.box[0] = bx;
                        res.box[1] = by;
                        res.box[2] = bx + pf->sx - 1;
                        res.box[3] = by + pf->sy - 1;
                        for (auto& [s, p2] : scored)
                            if (p2 == pf) res.score = s;
                        done = true;
                    }
        }
        if (!done) {
            // Nothing matched or fitted: a light (or decoration) of the theme marks the spot.
            const std::vector<KitPiece>* v = t && !t->lights.empty() ? &t->lights : t ? &t->decor : nullptr;
            if (v && !v->empty() && !poi_used[size_t(idx(P.x, P.y))]) {
                const KitPiece* k = pick(*v, r);
                const ShapeEntry* e = K.shape(k->shape);
                int xd = e ? e->fp[0] : 32, yd = e ? e->fp[1] : 32;
                // Centred in the cell, on the 32 grid.
                auto centre = [](int c, int d) { return std::min(128 * c + std::max(127, d - 1), 128 * c + ((128 + d) / 2 + 31) / 32 * 32 - 1); };
                DressedItem it = make(*k, centre(P.x, xd), centre(P.y, yd), z, "poi", P.x, P.y);
                it.poi = res.id;
                if (try_add(it)) {
                    poi_used[size_t(idx(P.x, P.y))] = int(pi) + 1;
                    res.placed = true;
                    res.label = "marker (" + std::string(e && !e->name.empty() ? e->name : "decoration") + ")";
                    Box b = box_of(K, it);
                    res.box[0] = b.x0;
                    res.box[1] = b.y0;
                    res.box[2] = b.x1;
                    res.box[3] = b.y1;
                }
            }
            if (!res.placed) R.warnings.push_back("POI " + res.id + ": nothing fitted");
            else R.warnings.push_back("POI " + res.id + ": no prefab matched \"" + P.note + "\", placed a marker");
        }
        R.pois.push_back(res);
    }
}

// --- Loose decorations, plants and lights on open floor ------------------------------------------

void Dresser::scatter() {
    if (!opt.scatter) return;
    std::set<std::pair<int, int>> near_entrance;
    for (auto& e : R.entrances)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) near_entrance.insert({e[0] + dx, e[1] + dy});
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (A.kind(x, y) != '.' || poi_used[size_t(idx(x, y))] || near_entrance.count({x, y})) continue;
            // Open cells only: all 8 neighbours floor at the same height (corridors stay clear).
            bool open = true, by_wall = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (!A.inside(nx, ny)) { open = false; continue; }
                    char c = A.kind(nx, ny);
                    if (line_cell(c)) by_wall = true;
                    if (c != '.' || A.z(nx, ny) != A.z(x, y) || poi_used[size_t(idx(nx, ny))]) open = false;
                }
            const ThemeKit* t = theme[size_t(idx(x, y))];
            if (!t) continue;
            Rng r = rng_at(seed, kStScatter, x, y);
            const std::vector<KitPiece>* v = nullptr;
            double p = r.uniform();
            if (by_wall && !t->lights.empty() && p < 0.05) {
                v = &t->lights;
            } else if (open && p < std::min(0.10, t->scatter + 0.02)) {
                bool inside = interior[size_t(idx(x, y))];
                double q = r.uniform();
                if (!inside && !t->plants.empty() && q < 0.45) v = &t->plants;
                else if (!inside && !t->trees.empty() && q < 0.52) v = &t->trees;
                else if (!t->decor.empty() && q < 0.85) v = &t->decor;
                else if (!t->clutter.empty()) v = &t->clutter;
                else if (!t->decor.empty()) v = &t->decor;
            }
            if (!v || v->empty()) continue;
            // Under a roof only what fits below it (no lamp posts indoors).
            std::vector<KitPiece> low;
            if (interior[size_t(idx(x, y))]) {
                const WallStyle* ws = style(t);
                int hmax = (ws ? ws->height : 40) - 8;
                for (const KitPiece& c : *v) {
                    const ShapeEntry* e = K.shape(c.shape);
                    if (e && e->fp[2] <= hmax) low.push_back(c);
                }
                if (low.empty()) continue;
                v = &low;
            }
            const KitPiece* k = pick(*v, r);
            const ShapeEntry* e = K.shape(k->shape);
            if (!e) continue;
            int xd = e->fp[0], yd = e->fp[1];
            if (xd > 128 || yd > 128) {
                if (!open) continue;  // trees and big pieces need the open middle of a field
            }
            int sx = std::max(0, (128 - xd) / 32), sy = std::max(0, (128 - yd) / 32);
            int ox = 32 * r.range(sx + 1), oy = 32 * r.range(sy + 1);
            DressedItem it = make(*k, 128 * x + std::max(xd, 32) - 1 + ox, 128 * y + std::max(yd, 32) - 1 + oy, A.z(x, y),
                                  by_wall && p < 0.05 ? "light" : "decor", x, y);
            try_add(it);
        }
}

void Dresser::run() {
    R.seed = seed;
    R.size[0] = A.w * A.cell;
    R.size[1] = A.h * A.cell;
    themes_and_rooms();
    floors();
    walls();
    ledges();
    pois();
    scatter();
    roofs();
}

}  // namespace

// ---------------------------------------------------------------------------------------------

const PaintedCell& PaintedArea::at(int x, int y) const {
    static const PaintedCell kVoid;
    return inside(x, y) ? cells[size_t(y * w + x)] : kVoid;
}

PaintedArea make_area(const std::string& theme, const std::vector<std::string>& rows, const std::vector<std::string>& heights,
                      uint32_t seed) {
    PaintedArea a;
    a.theme = theme;
    a.seed = seed;
    a.h = int(rows.size());
    for (auto& r : rows) a.w = std::max(a.w, int(r.size()));
    a.cells.assign(size_t(a.w * a.h), PaintedCell{});
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < int(rows[size_t(y)].size()); ++x) {
            PaintedCell& c = a.cells[size_t(y * a.w + x)];
            c.kind = rows[size_t(y)][size_t(x)];
            if (y < int(heights.size()) && x < int(heights[size_t(y)].size())) {
                char h = char(std::tolower(unsigned(heights[size_t(y)][size_t(x)])));
                c.level = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'z' ? h - 'a' + 10 : 0;
            }
        }
    return a;
}

bool parse_area(const std::string& text, PaintedArea& area, std::string& err) {
    OJson j;
    try {
        j = OJson::parse(text);
    } catch (const std::exception& e) {
        err = std::string("area: bad JSON: ") + e.what();
        return false;
    }
    try {
        if (j.value("format", "") != "pagan-area") {
            err = "area: format is not \"pagan-area\"";
            return false;
        }
        if (j.value("version", 1) > 1) {
            // Later versions only add fields; read what we know.
        }
        if (!j.contains("rows") || !j["rows"].is_array()) {
            err = "area: no rows";
            return false;
        }
        std::vector<std::string> rows = j["rows"].get<std::vector<std::string>>();
        std::vector<std::string> heights;
        if (j.contains("heights") && j["heights"].is_array()) heights = j["heights"].get<std::vector<std::string>>();
        area = make_area(j.value("theme", "cave"), rows, heights, j.value("seed", 1u));
        area.name = j.value("name", "");
        area.cell = j.value("cell", 128);
        if (area.cell != 128) {
            err = "area: only cell 128 is supported";
            return false;
        }
        area.base_z = j.value("base_z", 0);
        area.level_z = j.value("level_z", 8);
        if (j.contains("origin") && j["origin"].is_array() && j["origin"].size() >= 2) {
            area.origin[0] = j["origin"][0];
            area.origin[1] = j["origin"][1];
            area.origin[2] = j["origin"].size() > 2 ? j["origin"][2].get<int>() : 0;
        }
        if (j.contains("themes") && j["themes"].is_array()) {
            std::map<char, std::string> legend;
            if (j.contains("theme_legend"))
                for (auto it = j["theme_legend"].begin(); it != j["theme_legend"].end(); ++it)
                    if (!it.key().empty()) legend[it.key()[0]] = it.value().get<std::string>();
            std::vector<std::string> tr = j["themes"].get<std::vector<std::string>>();
            for (int y = 0; y < area.h && y < int(tr.size()); ++y)
                for (int x = 0; x < area.w && x < int(tr[size_t(y)].size()); ++x) {
                    auto f = legend.find(tr[size_t(y)][size_t(x)]);
                    if (f != legend.end()) area.cells[size_t(y * area.w + x)].theme = f->second;
                }
        }
        for (PaintedCell& c : area.cells)
            if (!(c.kind == ' ' || c.kind == '.' || c.kind == '#' || c.kind == '+' || c.kind == '/' || c.kind == '~' ||
                  c.kind == '=' || c.kind == 'x'))
                c.kind = '.';
        if (j.contains("pois") && j["pois"].is_array())
            for (const OJson& p : j["pois"]) {
                PaintedPoi q;
                q.id = p.value("id", "");
                q.x = p.value("x", 0);
                q.y = p.value("y", 0);
                q.note = p.value("note", "");
                if (p.contains("tags") && p["tags"].is_array()) q.tags = p["tags"].get<std::vector<std::string>>();
                q.prefab = p.value("prefab", "");
                q.radius = p.value("radius", 2);
                area.pois.push_back(q);
            }
        if (j.contains("styles") && j["styles"].is_object())
            for (auto it = j["styles"].begin(); it != j["styles"].end(); ++it)
                if (it.value().is_string()) area.styles[it.key()] = it.value().get<std::string>();
    } catch (const std::exception& e) {
        err = std::string("area: bad field: ") + e.what();
        return false;
    }
    return true;
}

DressResult dress(const PaintedArea& area, const TileKit& kit, uint32_t seed, const DressOptions& opt) {
    Dresser d(area, kit, seed ? seed : area.seed, opt);
    d.run();
    if (opt.validate) d.R.validation = validate(d.R, area, kit);
    return std::move(d.R);
}

// ---------------------------------------------------------------------------------------------
// Validation

Validation validate(const DressResult& r, const PaintedArea& A, const TileKit& K) {
    Validation v;
    ItemIndex index(K);
    index.items = &r.items;
    for (size_t i = 0; i < r.items.size(); ++i) index.add(i);
    auto note = [&](const std::string& kind, const std::string& msg, int& counter) {
        if (counter++ < 4) v.problems.push_back(kind + ": " + msg);
    };
    int n_over = 0, n_sup = 0, n_floor = 0, n_wall = 0, n_poi = 0;

    // 1. Solid volumes must not intersect (POI items from one prefab keep their original layout).
    for (size_t i = 0; i < r.items.size(); ++i) {
        const DressedItem& a = r.items[i];
        if (!(tflags_of(K, a.shape) & (kTfSolid | kTfDamaging))) continue;
        Box ba = box_of(K, a);
        if (ba.z1 <= ba.z0) continue;
        std::set<size_t> seen;
        for (int cy = ba.y0 / 128; cy <= ba.y1 / 128; ++cy)
            for (int cx = ba.x0 / 128; cx <= ba.x1 / 128; ++cx) {
                auto f = index.cells.find(ItemIndex::key(cx, cy));
                if (f == index.cells.end()) continue;
                for (size_t j : f->second) {
                    if (j <= i || !seen.insert(j).second) continue;
                    const DressedItem& b = r.items[j];
                    if (!(tflags_of(K, b.shape) & (kTfSolid | kTfDamaging))) continue;
                    if (!a.poi.empty() && a.poi == b.poi) continue;
                    if (volumes_overlap(ba, box_of(K, b))) {
                        ++v.overlaps;
                        note("overlap", a.role + " " + std::to_string(a.shape) + " at " + std::to_string(a.cx) + "," +
                                            std::to_string(a.cy) + " and " + b.role + " " + std::to_string(b.shape),
                             n_over);
                    }
                }
            }
    }
    // Highest support top under a point / footprint.
    auto support_at = [&](int x0, int y0, int x1, int y1, int maxz, bool land_only) {
        int best = -1000;
        for (int cy = y0 / 128; cy <= y1 / 128; ++cy)
            for (int cx = x0 / 128; cx <= x1 / 128; ++cx) {
                auto f = index.cells.find(ItemIndex::key(cx, cy));
                if (f == index.cells.end()) continue;
                for (size_t j : f->second) {
                    const DressedItem& b = r.items[j];
                    uint16_t tf = tflags_of(K, b.shape);
                    if (!(tf & (land_only ? (kTfLand | kTfRoof) : (kTfSolid | kTfLand | kTfRoof)))) continue;
                    if (b.role == "roof") continue;
                    Box bb = box_of(K, b);
                    if (bb.x0 > x1 || x0 > bb.x1 || bb.y0 > y1 || y0 > bb.y1) continue;
                    if (bb.z1 <= maxz) best = std::max(best, bb.z1);
                }
            }
        return best;
    };
    // 2. Objects stand on something.
    for (const DressedItem& a : r.items) {
        if (a.role == "floor" || a.role == "water" || a.role == "roof" || a.role == "wall" || a.role == "post" ||
            a.role == "jamb" || a.role == "door" || a.role == "ledge" || a.role == "trim")
            continue;
        if (a.flags & kItemHanging) continue;
        Box b = box_of(K, a);
        if (b.z0 <= 0) continue;
        if (support_at(b.x0, b.y0, b.x1, b.y1, b.z0, false) != b.z0) {
            ++v.unsupported;
            note("unsupported", a.role + " " + std::to_string(a.shape) + " at " + std::to_string(a.cx) + "," + std::to_string(a.cy), n_sup);
        }
    }
    // 3. Every floor-like cell is covered by land at its height (4 sample points).
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (!floorlike(A.kind(x, y))) continue;
            ++v.floor_cells;
            int z = A.z(x, y);
            bool ok = true;
            for (int s = 0; s < 4 && ok; ++s) {
                int px = 128 * x + 32 + 64 * (s & 1), py = 128 * y + 32 + 64 * (s >> 1);
                ok = support_at(px, py, px, py, z, true) == z;
            }
            if (!ok) {
                ++v.uncovered_cells;
                note("floor", "cell " + std::to_string(x) + "," + std::to_string(y) + " not covered at z " + std::to_string(z), n_floor);
            }
        }
    // 4. Walls are closed: along every link between two wall-line cells, every 32-unit slot holds a
    //    solid piece (doors count; passages are meant to be open).
    auto solid_at = [&](int px, int py, int z) {
        for (int cy = py / 128; cy <= py / 128; ++cy)
            for (int cx = px / 128; cx <= px / 128; ++cx) {
                auto f = index.cells.find(ItemIndex::key(cx, cy));
                if (f == index.cells.end()) continue;
                for (size_t j : f->second) {
                    const DressedItem& b = r.items[j];
                    if (!(tflags_of(K, b.shape) & kTfSolid) || b.role == "floor") continue;
                    Box bb = box_of(K, b);
                    if (px >= bb.x0 && px <= bb.x1 && py >= bb.y0 && py <= bb.y1 && z >= bb.z0 && z < bb.z1) return true;
                }
            }
        return false;
    };
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            if (!line_cell(A.kind(x, y))) continue;
            for (int d = 0; d < 2; ++d) {
                int nx = x + (d == 0), ny = y + (d == 1);
                if (!A.inside(nx, ny) || !line_cell(A.kind(nx, ny))) continue;
                if (A.kind(x, y) == '/' || A.kind(nx, ny) == '/') continue;
                ++v.wall_links;
                int z = std::max(A.z(x, y), A.z(nx, ny)) + 4;
                bool gap = false;
                for (int k = 1; k <= 5 && !gap; ++k) {
                    int along = 128 * (d == 0 ? x : y) + 32 * k + 16;
                    int fixed = 128 * (d == 0 ? y : x) + 48;
                    gap = !solid_at(d == 0 ? along : fixed, d == 0 ? fixed : along, z);
                }
                if (gap) {
                    ++v.wall_gaps;
                    note("wall", "gap between " + std::to_string(x) + "," + std::to_string(y) + " and " + std::to_string(nx) +
                                     "," + std::to_string(ny), n_wall);
                }
            }
        }
    // 5. Walkability: avatar boxes (64 x 64 x 40) on a 32-unit grid; a box is valid if nothing
    //    solid but doors (they open) intersects it and land under its centre is at most 8 below.
    //    Moves go to the 8 neighbours with steps of at most 8. Starts: the entrances.
    const int W4 = A.w * 4, H4 = A.h * 4;
    std::vector<int> zpos(size_t(W4 * H4), -1000);
    auto valid_at = [&](int gx, int gy) -> int {
        // Stored corner (max x, max y) of the box at grid (gx, gy).
        int x1 = 32 * gx + 31, y1 = 32 * gy + 31, x0 = x1 - 63, y0 = y1 - 63;
        if (x0 < 0 || y0 < 0 || x1 >= A.w * 128 || y1 >= A.h * 128) return -1000;
        int ccx = (x0 + 32) / 128, ccy = (y0 + 32) / 128;
        if (!A.inside(ccx, ccy)) return -1000;
        int top = A.z(ccx, ccy) + 8;
        int z = support_at(x0, y0, x1, y1, top, false);
        if (z < -100) return -1000;
        int land = support_at(x0 + 31, y0 + 31, x0 + 32, y0 + 32, z, true);
        if (land < z - 8) return -1000;
        Box b{x0, y0, z, x1, y1, z + 40};
        for (int cy = y0 / 128; cy <= y1 / 128; ++cy)
            for (int cx = x0 / 128; cx <= x1 / 128; ++cx) {
                auto f = index.cells.find(ItemIndex::key(cx, cy));
                if (f == index.cells.end()) continue;
                for (size_t j : f->second) {
                    const DressedItem& o = r.items[j];
                    if (o.role == "door" || o.role == "roof") continue;
                    if (!(tflags_of(K, o.shape) & (kTfSolid | kTfDamaging))) continue;
                    if (volumes_overlap(b, box_of(K, o))) return -1000;
                }
            }
        return z;
    };
    for (int gy = 0; gy < H4; ++gy)
        for (int gx = 0; gx < W4; ++gx) {
            zpos[size_t(gy * W4 + gx)] = valid_at(gx, gy);
            if (zpos[size_t(gy * W4 + gx)] > -1000) ++v.positions;
        }
    std::vector<uint8_t> reach(size_t(W4 * H4), 0);
    std::vector<int> q;
    auto start_near = [&](int cx, int cy) {
        for (int gy = 4 * cy - 2; gy <= 4 * cy + 6; ++gy)
            for (int gx = 4 * cx - 2; gx <= 4 * cx + 6; ++gx) {
                if (gx < 0 || gy < 0 || gx >= W4 || gy >= H4) continue;
                size_t i = size_t(gy * W4 + gx);
                if (zpos[i] > -1000 && !reach[i]) reach[i] = 1, q.push_back(int(i));
            }
    };
    for (auto& e : r.entrances) start_near(e[0], e[1]);
    // No entrances: start from the area's edge (outside), else from any floor.
    for (int pass = 0; pass < 2 && q.empty(); ++pass)
        for (int y = 0; y < A.h && q.empty(); ++y)
            for (int x = 0; x < A.w && q.empty(); ++x) {
                bool edge = x == 0 || y == 0 || x == A.w - 1 || y == A.h - 1;
                if (A.kind(x, y) == '.' && (edge || pass == 1)) start_near(x, y);
            }
    for (size_t qi = 0; qi < q.size(); ++qi) {
        int i = q[qi], gx = i % W4, gy = i / W4;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = gx + dx, ny = gy + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= W4 || ny >= H4) continue;
                size_t j = size_t(ny * W4 + nx);
                if (reach[j] || zpos[j] <= -1000 || std::abs(zpos[j] - zpos[size_t(i)]) > 8) continue;
                if (dx && dy && (zpos[size_t(gy * W4 + nx)] <= -1000 || zpos[size_t(ny * W4 + gx)] <= -1000)) continue;
                reach[j] = 1;
                q.push_back(int(j));
            }
    }
    v.reachable = int(q.size());
    for (const PoiResult& p : r.pois) {
        if (!p.placed) continue;
        // Reachable if some reached box centre is within 96 units of the POI's box.
        bool ok = false;
        for (int gy = 0; gy < H4 && !ok; ++gy)
            for (int gx = 0; gx < W4 && !ok; ++gx) {
                if (!reach[size_t(gy * W4 + gx)]) continue;
                int cx = 32 * gx - 1, cy = 32 * gy - 1;
                int ddx = std::max({p.box[0] - cx, 0, cx - p.box[2]}), ddy = std::max({p.box[1] - cy, 0, cy - p.box[3]});
                ok = ddx * ddx + ddy * ddy <= 96 * 96;
            }
        if (!ok) {
            v.unreachable_pois.push_back(p.id);
            note("walk", "POI " + p.id + " cannot be reached from the entrances", n_poi);
        }
    }
    v.floor_covered = v.uncovered_cells == 0;
    v.walls_closed = v.wall_gaps == 0;
    v.walkable = v.unreachable_pois.empty() && v.reachable > 0;
    v.ok = v.floor_covered && v.walls_closed && v.walkable && v.overlaps == 0 && v.unsupported == 0;
    return v;
}

// ---------------------------------------------------------------------------------------------
// JSON out / in

std::string dressed_to_json(const PaintedArea& A, const DressResult& r) {
    OJson j = OJson::object();
    j["format"] = "pagan-dressed";
    j["version"] = 1;
    j["name"] = A.name;
    j["theme"] = A.theme;
    j["seed"] = r.seed;
    j["cell"] = A.cell;
    j["origin"] = OJson::array({A.origin[0], A.origin[1], A.origin[2]});
    j["size"] = OJson::array({r.size[0], r.size[1]});
    OJson items = OJson::array();
    for (const DressedItem& it : r.items) {
        OJson o = OJson::object();
        o["shape"] = it.shape;
        o["frame"] = it.frame;
        o["x"] = it.x;
        o["y"] = it.y;
        o["z"] = it.z;
        o["flags"] = it.flags;
        o["quality"] = it.quality;
        o["npcnum"] = it.npcnum;
        o["mapnum"] = it.mapnum;
        o["role"] = it.role;
        o["cell"] = OJson::array({it.cx, it.cy});
        o["poi"] = it.poi;
        items.push_back(o);
    }
    j["items"] = items;
    OJson pois = OJson::array();
    for (const PoiResult& p : r.pois) {
        OJson o = OJson::object();
        o["id"] = p.id;
        o["note"] = p.note;
        o["prefab"] = p.prefab;
        o["label"] = p.label;
        o["score"] = std::round(p.score * 100) / 100;
        o["placed"] = p.placed;
        o["cell"] = OJson::array({p.x, p.y});
        o["box"] = OJson::array({p.box[0], p.box[1], p.box[2], p.box[3]});
        pois.push_back(o);
    }
    j["pois"] = pois;
    OJson ent = OJson::array();
    for (auto& e : r.entrances) ent.push_back(OJson::array({e[0], e[1]}));
    j["entrances"] = ent;
    j["warnings"] = r.warnings;
    const Validation& v = r.validation;
    OJson val = OJson::object();
    val["ok"] = v.ok;
    val["floor_covered"] = v.floor_covered;
    val["walls_closed"] = v.walls_closed;
    val["overlaps"] = v.overlaps;
    val["unsupported"] = v.unsupported;
    val["walkable"] = v.walkable;
    val["unreachable_pois"] = v.unreachable_pois;
    val["floor_cells"] = v.floor_cells;
    val["uncovered_cells"] = v.uncovered_cells;
    val["wall_links"] = v.wall_links;
    val["wall_gaps"] = v.wall_gaps;
    val["positions"] = v.positions;
    val["reachable"] = v.reachable;
    val["problems"] = v.problems;
    j["validation"] = val;
    return json_pretty(j) + "\n";
}

bool parse_dressed(const std::string& text, std::vector<ItemRecord>& items, int origin[3], std::string& err) {
    OJson j;
    try {
        j = OJson::parse(text);
        if (j.value("format", "") != "pagan-dressed") {
            err = "dressed: format is not \"pagan-dressed\"";
            return false;
        }
        origin[0] = origin[1] = origin[2] = 0;
        if (j.contains("origin") && j["origin"].size() >= 3)
            for (int k = 0; k < 3; ++k) origin[k] = j["origin"][size_t(k)];
        items.clear();
        for (const OJson& o : j.value("items", OJson::array())) {
            ItemRecord r;
            int x = o.value("x", 0), y = o.value("y", 0), z = o.value("z", 0);
            if (x < 0 || y < 0 || x > 65535 || y > 65535 || z < 0 || z > 255) continue;
            r.x = uint16_t(x);
            r.y = uint16_t(y);
            r.z = uint8_t(z);
            r.shape = o.value("shape", 0);
            r.frame = o.value("frame", 0);
            r.flags = o.value("flags", 0);
            r.quality = o.value("quality", 0);
            r.npcnum = o.value("npcnum", 0);
            r.mapnum = o.value("mapnum", 0);
            items.push_back(r);
        }
    } catch (const std::exception& e) {
        err = std::string("dressed: ") + e.what();
        return false;
    }
    return true;
}

std::vector<ItemRecord> to_records(const DressResult& r, bool with_roofs) {
    std::vector<ItemRecord> out;
    for (const DressedItem& it : r.items) {
        if (!with_roofs && it.role == "roof") continue;
        ItemRecord rec;
        rec.x = uint16_t(std::clamp(it.x, 0, 65535));
        rec.y = uint16_t(std::clamp(it.y, 0, 65535));
        rec.z = uint8_t(std::clamp(it.z, 0, 255));
        rec.shape = it.shape;
        rec.frame = uint8_t(it.frame);
        rec.flags = it.flags;
        rec.quality = it.quality;
        rec.npcnum = it.npcnum;
        rec.mapnum = it.mapnum;
        out.push_back(rec);
    }
    return out;
}

}  // namespace pg
