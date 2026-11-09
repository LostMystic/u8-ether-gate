// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/tilekit_miner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

#include "data/gamedata.h"
#include "world/map_patches.h"
#include "world/current_map.h"

namespace pg {

namespace {

// ---------------------------------------------------------------------------------------------
// Helpers

uint64_t key3(int a, int b, int c) {
    return (uint64_t(uint32_t(a) & 0xFFFFF) << 40) | (uint64_t(uint32_t(b) & 0xFFFFF) << 20) | uint64_t(uint32_t(c) & 0xFFFFF);
}
uint32_t sf(unsigned shape, unsigned frame) { return (uint32_t(shape) << 16) | frame; }
unsigned sf_shape(uint32_t k) { return k >> 16; }
unsigned sf_frame(uint32_t k) { return k & 0xFFFF; }

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(unsigned(c)));
    return s;
}

// Sorted (descending weight) piece list from counts, capped.
std::vector<KitPiece> top_pieces(const std::map<uint32_t, double>& counts, size_t cap, double min_share = 0.0) {
    std::vector<KitPiece> v;
    double total = 0;
    for (auto& [k, n] : counts) total += n;
    for (auto& [k, n] : counts) v.push_back({uint16_t(sf_shape(k)), uint16_t(sf_frame(k)), n});
    std::stable_sort(v.begin(), v.end(), [](const KitPiece& a, const KitPiece& b) { return a.weight > b.weight; });
    std::vector<KitPiece> out;
    for (const KitPiece& p : v) {
        if (out.size() >= cap) break;
        if (total > 0 && p.weight / total < min_share && !out.empty()) break;
        out.push_back(p);
    }
    return out;
}

// Theme seeds: map numbers by the place names of specs/world/map_index.md.
struct ThemeSeed {
    const char* name;
    const char* label;
    std::vector<unsigned> maps;
};
const std::vector<ThemeSeed>& theme_seeds() {
    static const std::vector<ThemeSeed> s = {
        {"tenebrae", "Tenebrae town: timber houses, cobbled streets, gardens (maps 39-41)", {39, 40, 41}},
        {"beach", "Shore and docks: sand, rock, sea (maps 3, 4 Fisherman's Reef, 21 Stone Cove, 15 Treasure Cove)", {3, 4, 21, 15}},
        {"valley", "Open grassland and roads (maps 5 East Road, 7 Herdsman's Valley)", {5, 7}},
        {"cemetery", "Cemetery: graves, crypts, dead trees (map 6)", {6}},
        {"plateau", "Plateau: high rocky ground, Mythran's house (map 8)", {8}},
        {"cave", "Caves: rough rock walls, underground water (maps 31 Hall of the Mountain King, 57 Pit of the Dead, 62 Ancient Necromancers)", {31, 57, 62}},
        {"lava", "Lava: lava rivers and fire (maps 12 Daemon's Crag, 24/25 Lava River, 44 Plane of Fire)", {12, 24, 25, 44}},
        {"catacombs", "Catacombs: dressed stone walls and flagstones (maps 49, 50, 51)", {49, 50, 51}},
        {"fortress", "Obsidian Fortress (map 14)", {14}},
        {"lake", "Carthax Lake (maps 16, 26, 28)", {16, 26, 28}},
        {"argentrock", "Argentrock Isle: the sorcerers' island (map 37)", {37}},
        {"shrine", "Shrine of the Ancient Ones (map 54)", {54}},
        {"ether", "The elemental planes and the ethereal void (maps 43, 45, 46, 47)", {43, 45, 46, 47}},
    };
    return s;
}

// Usecode class names (the game's own names for shapes) to roles.
bool name_in(const std::string& n, std::initializer_list<const char*> list) {
    for (const char* s : list)
        if (n == s) return true;
    return false;
}
bool is_light_name(const std::string& n) {
    return name_in(n, {"LAMPPOST", "LAMP1", "LAMP2", "LAMP3", "LIGHT", "CANDLE", "CANDLBRA", "TALLCAND", "SKULCAND",
                       "FIREPIT", "CAMPFIRE", "NSLAMPTP", "EWLAMPTP", "FIREPLAC", "FIREPLEW", "FIREPLNS", "ENDLAMP",
                       "INTERN", "BURNDOUT"});
}
bool is_plant_name(const std::string& n) {
    return name_in(n, {"BRANCHES", "MUSHROOM", "MUSHCAP", "LITLMUSH", "MUSHGRUP", "SHORTMSH", "SMALMUSH", "POTPLANT",
                       "EWCROPS", "NSCROPS", "FIRESHRO"});
}
bool is_tree_name(const std::string& n) { return name_in(n, {"TREE", "GREENTRE", "BIGUGLY"}); }

// Share of clearly green pixels in a shape's first frame (plants, bushes, trees).
double green_share(const GameData& d, unsigned shape) {
    const ShapeFrame* f = d.shapes.frame(shape, 0);
    if (!f || f->pixels.empty()) return 0;
    int n = 0, g = 0;
    for (uint8_t p : f->pixels) {
        if (p == ShapeFrame::kTransparent) continue;
        ++n;
        const auto& c = d.palette.rgb8[p];
        if (c[1] > c[0] + 12 && c[1] > c[2] + 12) ++g;
    }
    return n ? double(g) / n : 0;
}

// A colour word for a frame's mean colour (labels in the kit, for people reading it).
std::string colour_word(const GameData& d, unsigned shape, unsigned frame) {
    const ShapeFrame* f = d.shapes.frame(shape, frame);
    double c[3] = {0, 0, 0}, n = 0;
    if (f)
        for (uint8_t p : f->pixels) {
            if (p == ShapeFrame::kTransparent) continue;
            for (int k = 0; k < 3; ++k) c[k] += d.palette.rgb8[p][size_t(k)];
            ++n;
        }
    if (n == 0) return "empty";
    for (double& v : c) v /= n;
    double mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    std::string dark = mx < 70 ? "dark " : mx > 170 ? "light " : "";
    if (mx - mn < 22) return dark + "grey";
    if (c[2] >= c[0] && c[2] >= c[1]) return dark + (c[0] > c[1] + 15 ? "purple" : "blue");
    if (c[1] >= c[0] && c[1] >= c[2]) return dark + "green";
    if (c[1] > c[0] * 0.8) return dark + (mx > 140 ? "sand" : "yellow-brown");
    if (c[1] > c[0] * 0.5) return dark + "brown";
    return dark + "red";
}

// Frames that are fit to place: some frames in the data are empty (invisible blockers) or flat
// one-colour placeholders. For structure pieces a frame must also cover at least 60% of the
// shape's fullest frame, so end caps and broken bits are not used as fill.
struct FrameCheck {
    const GameData& d;
    std::map<unsigned, int> full;  // shape -> opaque pixels of its fullest frame
    explicit FrameCheck(const GameData& dd) : d(dd) {}
    static int opaque(const ShapeFrame* f) {
        int n = 0;
        if (f)
            for (uint8_t p : f->pixels) n += p != ShapeFrame::kTransparent;
        return n;
    }
    bool ok(unsigned shape, unsigned frame, bool structure) {
        const ShapeFrame* f = d.shapes.frame(shape, frame);
        int n = opaque(f);
        if (n < 12) return false;
        std::map<uint8_t, int> col;
        for (uint8_t p : f->pixels)
            if (p != ShapeFrame::kTransparent) col[p]++;
        int top = 0;
        for (auto& [c, k] : col) top = std::max(top, k);
        if (top * 10 > n * 6) return false;  // a one-colour placeholder
        if (!structure) return true;
        auto it = full.find(shape);
        if (it == full.end()) {
            int m = 0;
            const Shape* s = d.shapes.shape(shape);
            for (unsigned k = 0; s && k < s->frame_count(); ++k) m = std::max(m, opaque(d.shapes.frame(shape, k)));
            it = full.emplace(shape, m).first;
        }
        return n * 10 >= it->second * 6;
    }
    void filter(std::vector<KitPiece>& v, bool structure) {
        std::vector<KitPiece> out;
        for (const KitPiece& p : v)
            if (ok(p.shape, p.frame, structure)) out.push_back(p);
        v.swap(out);
    }
};

// ---------------------------------------------------------------------------------------------
// Per-shape statistics and roles

struct ShapeStat {
    int count = 0, nonfixed = 0;
    std::map<int, int> z;
    std::map<unsigned, int> frames;
    std::map<unsigned, int> maps;
    int aligned128 = 0, aligned256 = 0;  // floor tiles on the 128 / 256 grid
    int has_floor_below = 0;             // ROOF flats: a floor 24..120 below at the same cell
};

Role classify(const GameData& d, unsigned shape, const ShapeStat& st, const std::string& name) {
    const ShapeInfo& si = d.typeflags.get(shape);
    int xd = int(si.x()) * 32, yd = int(si.y()) * 32, zd = int(si.z()) * 8;
    unsigned fam = si.family();
    uint16_t f = si.flags();
    if (si.is(ShapeInfo::kEditor) || fam == kFamGlobEgg || fam == kFamUsecodeEgg || fam == kFamMonsterEgg ||
        fam == kFamTeleportEgg)
        return Role::kEgg;
    if (fam == kFamContainer && d.shapes.shape(shape) && d.shapes.shape(shape)->frame_count() > 40) return Role::kNpc;
    if (has(name, "DOOR")) return Role::kDoor;
    if (fam == kFamContainer) return Role::kContainer;
    bool flat = zd <= 8;
    bool fixed = f & ShapeInfo::kFixed;
    if ((f & ShapeInfo::kSea) && flat && xd >= 64) return Role::kWater;
    if (has(name, "WATER") && flat) return Role::kWater;
    if ((has(name, "LAVA") || ((f & ShapeInfo::kDamaging) && (f & ShapeInfo::kLand))) && flat && xd >= 64) return Role::kLava;
    if (is_light_name(name)) return Role::kLight;
    if (is_tree_name(name)) return Role::kTree;
    if (is_plant_name(name)) return Role::kPlant;
    if (flat && xd == yd && xd >= 64 && (f & (ShapeInfo::kLand | ShapeInfo::kRoof))) {
        if ((f & ShapeInfo::kRoof) && st.count > 0 && st.has_floor_below * 2 > st.count) return Role::kRoof;
        return Role::kFloor;
    }
    if ((f & ShapeInfo::kRoof) && fixed && zd <= 16) return Role::kRoof;  // roof edges and ridges
    int mn = std::min(xd, yd), mx = std::max(xd, yd);
    if (fixed && (f & ShapeInfo::kSolid) && zd >= 24 && mn == 32 && mx <= 128 && name.empty()) {
        if (mx == 32) return Role::kPost;
        return xd > yd ? Role::kWallEW : Role::kWallNS;
    }
    if (fixed && mn == 32 && mx >= 64 && zd <= 16 && name.empty()) return Role::kTrim;
    if (st.count > 0 && st.nonfixed * 2 > st.count) {
        if (si.weight() > 0 && mx <= 64 && zd <= 24) return Role::kClutter;
        return Role::kFurniture;
    }
    if (!name.empty() && !fixed) return Role::kFurniture;
    if (mx >= 160 && zd >= 32 && green_share(d, shape) > 0.25) return Role::kTree;
    if (green_share(d, shape) > 0.30) return Role::kPlant;
    return Role::kDecoration;
}

bool is_structure(Role r) {
    return r == Role::kFloor || r == Role::kWater || r == Role::kLava || r == Role::kRoof || r == Role::kWallEW ||
           r == Role::kWallNS || r == Role::kPost || r == Role::kTrim || r == Role::kDoor;
}

// ---------------------------------------------------------------------------------------------
// Prefab labels

struct LabelRule {
    const char* label;
    const char* id;
    std::vector<const char*> names;  // any member with one of these names (or a name containing it, '*')
    int min_hits;
    std::vector<const char*> keywords;
    bool single = false;  // a group of one or two items is enough (a grave, a well, a statue)
};
const std::vector<LabelRule>& label_rules() {
    static const std::vector<LabelRule> r = {
        {"campfire camp", "camp", {"CAMPFIRE", "FIREPIT", "BEDROLL", "OPNBDROL"}, 1, {"camp", "campfire", "fire", "rest", "bedroll", "wilderness"}},
        {"altar", "altar", {"ALTAR", "ALTAR_EW", "PENT", "PEDESTAL", "BIGDEMST"}, 1, {"altar", "shrine", "ritual", "temple", "sacrifice", "worship", "pentagram"}, true},
        {"shrine", "shrine", {"STATUE", "STATUE1", "LITHOS", "STRATOS", "PYROS", "HYDROS", "AMOSTAT", "ODISTAT", "APASTAT", "MORDSTAT", "STANSTON"}, 1, {"shrine", "statue", "idol", "monument", "titan", "worship", "stone"}, true},
        {"gargoyle statues", "gargoyles", {"GARGOYLE", "SGARGL"}, 1, {"gargoyle", "statue", "guardian", "gate", "monument"}, true},
        {"grave", "graves", {"GRAVE_NS", "GRAVE_EW", "GRAVEII"}, 1, {"grave", "graves", "cemetery", "graveyard", "burial", "tomb", "headstone"}, true},
        {"crypt", "crypt", {"CODNS", "CODEW", "DEADNS", "DEADEW", "LAYGHOUL"}, 1, {"crypt", "coffin", "tomb", "dead", "undead", "burial", "sarcophagus"}},
        {"remains", "remains", {"SKELETON", "BONES", "BONES2", "BONES3", "BONES4", "SKULZ", "LEGS", "TORSO", "DEADCLOZ"}, 2, {"bones", "skeleton", "remains", "dead", "corpse", "skull", "ossuary"}},
        {"bookcase corner", "books", {"NSSHELF", "EWSHELF", "BASEBOOK", "GRIMOIRE", "BOOKBLOO", "BASESCRL", "MAGSCROL", "DESKNS", "DESKEW"}, 2, {"books", "bookcase", "library", "study", "scholar", "scrolls", "desk"}},
        {"torture chamber", "torture", {"TORTRACK", "TORTCHAR", "CUFFS", "IRONMAN"}, 1, {"torture", "dungeon", "prison", "jail", "rack"}},
        {"smithy", "smithy", {"ANVIL", "BELLOWS", "CHOPBLK"}, 1, {"smithy", "forge", "anvil", "blacksmith", "workshop"}, true},
        {"kitchen", "kitchen", {"OVEN", "CAULDRON", "POTSPANS", "PLATEFOO", "FLOUR", "SPOUT"}, 1, {"kitchen", "cook", "food", "oven", "cauldron"}},
        {"market stall", "stall", {"AGWARE", "FOOD", "MOREFOOD", "BASKET", "MOREFISH", "FISHBONZ", "TABLWARE"}, 3, {"market", "stall", "shop", "vendor", "wares", "trader", "food"}},
        {"bedroom", "bedroom", {"BEDNS", "BEDEW", "KINGBDNS", "KINGBDEW", "CANOPYTP", "CANOPYEW", "NITSTAND", "WARDNS", "WARDEW"}, 1, {"bed", "bedroom", "sleep", "inn", "home", "house"}},
        {"table and chairs", "table", {"WTABLE", "WTABLEEW", "DTABLE", "OAKTBLNS", "OAKTBLEW", "FTABLENS", "FTABLEEW", "CHAIR", "SCHAIR", "WBENCH", "BENCHEW", "WTHRONE"}, 2, {"table", "chairs", "dining", "tavern", "inn", "meal", "hall"}},
        {"storage", "storage", {"BARREL", "KEGNS", "KEGEW", "CHEST_NS", "CHEST_EW", "LCHST_NS", "LCHST_EW", "JBOX", "BAG", "RAINBARL"}, 2, {"storage", "barrels", "chest", "supplies", "crates", "cellar", "loot", "treasure"}},
        {"lamp post", "lamp", {"LAMPPOST", "LAMP1", "LAMP2", "LAMP3"}, 1, {"lamp", "light", "street", "lantern", "post"}, true},
        {"well", "well", {"WELL"}, 1, {"well", "water", "village", "square"}, true},
        {"hearth", "hearth", {"FIREPLAC", "FIREPLEW", "FIREPLNS", "CHIMNEY", "SHCHIMNE"}, 1, {"fireplace", "hearth", "home", "warm"}},
        {"stable", "stable", {"TROUGHNS", "TROUGHEW", "HAY", "STRAW"}, 1, {"stable", "farm", "barn", "hay", "animals"}},
        {"weaver", "weaver", {"LOOM", "WOOL"}, 1, {"loom", "weaver", "wool", "cloth", "workshop"}},
        {"candles", "candles", {"CANDLE", "CANDLBRA", "TALLCAND", "SKULCAND"}, 2, {"candles", "light", "shrine", "ritual", "chapel"}},
        {"webs", "webs", {"COBWEB", "WEB", "BIGWEBEW", "BIGWEBNS", "LARWEBEW", "LARWEBNS", "LONGWEB", "EWBIGWEB", "NSBIGWEB"}, 1, {"spider", "webs", "cobwebs", "lair", "abandoned"}},
        {"mushrooms", "mushrooms", {"MUSHROOM", "MUSHCAP", "LITLMUSH", "MUSHGRUP", "SHORTMSH", "SMALMUSH", "FIRESHRO"}, 2, {"mushrooms", "fungus", "cave", "garden", "damp"}},
        {"grove", "grove", {"TREE", "GREENTRE", "BIGUGLY", "BRANCHES", "POTPLANT"}, 2, {"trees", "grove", "garden", "plants", "woods", "forest"}},
        {"fishing spot", "fishing", {"FISHNET", "FISHPOLE", "BOAT", "OAR"}, 1, {"fishing", "fisherman", "boat", "dock", "net", "pier"}},
        {"lever", "lever", {"LEVER", "WALLSWIT", "WINCHNS", "WINCHEW"}, 1, {"lever", "switch", "mechanism", "puzzle", "winch"}},
    };
    return r;
}

// ---------------------------------------------------------------------------------------------

struct Area {
    unsigned map = 0;
    int ax = 0, ay = 0;
    std::map<std::string, double> feat;  // feature counts
    double norm = 0;
    int theme = -1;
    int floors = 0;
};

double cosine(const std::map<std::string, double>& a, double na, const std::map<std::string, double>& b, double nb) {
    if (na <= 0 || nb <= 0) return 0;
    double s = 0;
    const auto& small = a.size() < b.size() ? a : b;
    const auto& big = a.size() < b.size() ? b : a;
    for (auto& [k, v] : small) {
        auto it = big.find(k);
        if (it != big.end()) s += v * it->second;
    }
    return s / (na * nb);
}
double norm_of(const std::map<std::string, double>& a) {
    double s = 0;
    for (auto& [k, v] : a) s += v * v;
    return std::sqrt(s);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

std::vector<unsigned> tilekit_maps() {
    return {3, 4, 5, 6, 7, 8, 12, 14, 15, 16, 21, 24, 25, 26, 28, 31, 37, 39, 40, 41, 43, 44, 45, 46, 47, 49, 50, 51, 54, 57, 62, 63};
}

std::vector<MinedItem> gather_map_items(const GameData& d, unsigned map) {
    std::vector<MinedItem> out;
    auto add = [&](unsigned m, int x, int y, int z, const ItemRecord& r, bool nonfixed) {
        MinedItem it;
        it.map = m;
        it.x = x;
        it.y = y;
        it.z = z;
        it.shape = r.shape;
        it.frame = r.frame;
        it.flags = uint16_t(r.flags & ~kItemFastArea);
        it.quality = r.quality;
        it.npcnum = r.npcnum;
        it.mapnum = r.mapnum;
        it.nonfixed = nonfixed;
        d.typeflags.get(r.shape).footprint(it.flags & kItemFlipped, it.xd, it.yd, it.zd);
        out.push_back(it);
    };
    std::vector<ItemRecord> fixed = parse_item_records(d.fixed.entry(map));
    apply_map_fixes(map, fixed);
    for (const ItemRecord& r : fixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord gr;
                gr.shape = g.shape;
                gr.frame = g.frame;
                add(map, (r.x & ~511) + 2 * g.x + 1, (r.y & ~511) + 2 * g.y + 1, r.z + g.z, gr, false);
            }
            continue;
        }
        add(map, r.x, r.y, r.z, r, false);
    }
    std::vector<ItemRecord> nonfixed = parse_item_records(d.nonfixed.entry(map));
    map_patches::apply_moves(map, nonfixed);
    std::vector<MapEntry> tree = build_item_tree(nonfixed, d.typeflags);
    for (const MapEntry& e : tree)
        if (e.parent < 0) add(map, e.rec.x, e.rec.y, e.rec.z, e.rec, true);
    return out;
}

bool mine_tilekit(const GameData& d, TileKit& kit, std::string& report, const MineOptions& opt) {
    kit = TileKit{};
    kit.source = "original maps (FIXED.DAT with globs, NONFIXED.DAT), TYPEFLAG.DAT, usecode class names";
    std::vector<unsigned> maps = tilekit_maps();
    std::map<unsigned, std::vector<MinedItem>> items;
    size_t total = 0, fixed_total = 0;
    for (unsigned m : maps) {
        items[m] = gather_map_items(d, m);
        total += items[m].size();
        for (const MinedItem& it : items[m]) fixed_total += !it.nonfixed;
    }
    auto name_of = [&](unsigned s) { return d.usecode.class_name(s); };

    // --- Shape statistics. Floors under ROOF flats are found with a per-map cell index.
    std::map<unsigned, ShapeStat> stats;
    for (auto& [m, list] : items) {
        std::unordered_map<uint64_t, std::vector<int>> flat_at;  // (cx, cy) -> z of flat land tiles
        for (const MinedItem& it : list) {
            const ShapeInfo& si = d.typeflags.get(it.shape);
            if (it.zd <= 8 && it.xd == it.yd && it.xd >= 64 && (si.flags() & (ShapeInfo::kLand | ShapeInfo::kRoof)))
                flat_at[key3(it.x / 128, it.y / 128, 0)].push_back(it.z);
        }
        for (const MinedItem& it : list) {
            ShapeStat& s = stats[it.shape];
            s.count++;
            s.nonfixed += it.nonfixed;
            s.z[it.z]++;
            s.frames[it.frame]++;
            s.maps[m]++;
            if ((it.x + 1) % 128 == 0 && (it.y + 1) % 128 == 0) s.aligned128++;
            if ((it.x + 1) % 256 == 0 && (it.y + 1) % 256 == 0) s.aligned256++;
            const ShapeInfo& si = d.typeflags.get(it.shape);
            if (si.is(ShapeInfo::kRoof) && it.zd <= 8) {
                auto f = flat_at.find(key3(it.x / 128, it.y / 128, 0));
                if (f != flat_at.end())
                    for (int z : f->second)
                        if (z + 24 <= it.z && z + 120 >= it.z) {
                            s.has_floor_below++;
                            break;
                        }
            }
        }
    }
    std::map<unsigned, Role> role;
    for (auto& [s, st] : stats) role[s] = classify(d, s, st, name_of(s));

    // --- Floor tiles on the 128 and 256 grids; neighbour pairs; under-roof test.
    struct Tile {
        unsigned map;
        int cx, cy, z;
        uint32_t k;  // shape/frame
        bool roofed;
    };
    std::vector<Tile> tiles[2];  // [0] 128, [1] 256
    std::unordered_map<uint64_t, uint32_t> grid[2];  // per map: (cx, cy, z) -> sf (map in high bits of the hash)
    auto gkey = [](unsigned map, int cx, int cy, int z) { return key3(cx + int(map) * 300, cy, z); };
    std::map<unsigned, std::unordered_map<uint64_t, std::vector<int>>> roof_at;  // per map: 128-cell -> roof z
    for (auto& [m, list] : items)
        for (const MinedItem& it : list) {
            if (!d.typeflags.get(it.shape).is(ShapeInfo::kRoof)) continue;
            for (int cy = it.y0() / 128; cy <= it.y / 128; ++cy)
                for (int cx = it.x0() / 128; cx <= it.x / 128; ++cx) roof_at[m][key3(cx, cy, 0)].push_back(it.z);
        }
    for (auto& [m, list] : items)
        for (const MinedItem& it : list) {
            Role r = role[it.shape];
            if (r != Role::kFloor && r != Role::kWater && r != Role::kLava && r != Role::kRoof) continue;
            if (it.zd > 8 || it.xd != it.yd) continue;
            int g = it.xd == 128 ? 0 : it.xd == 256 ? 1 : -1;
            if (g < 0 || (it.x + 1) % it.xd || (it.y + 1) % it.yd) continue;
            Tile t{m, it.x / it.xd, it.y / it.yd, it.z, sf(it.shape, it.frame), false};
            if (r != Role::kRoof) {
                auto& ra = roof_at[m];
                for (int dy = 0; dy <= g && !t.roofed; ++dy)
                    for (int dx = 0; dx <= g && !t.roofed; ++dx) {
                        auto f = ra.find(key3(t.cx * (g + 1) + dx, t.cy * (g + 1) + dy, 0));
                        if (f == ra.end()) continue;
                        for (int z : f->second)
                            if (z > it.z + 16 && z <= it.z + 160) t.roofed = true;
                    }
            }
            grid[g][gkey(m, t.cx, t.cy, t.z)] = t.k;
            tiles[g].push_back(t);
        }

    // Shape-level union of floor shapes that intermix (same kind only).
    std::map<unsigned, unsigned> parent;
    std::function<unsigned(unsigned)> findp = [&](unsigned s) {
        while (parent[s] != s) s = parent[s] = parent[parent[s]];
        return s;
    };
    std::map<unsigned, int> fl_count;
    std::map<std::pair<unsigned, unsigned>, int> shape_pairs;
    for (int g = 0; g < 2; ++g)
        for (const Tile& t : tiles[g]) {
            unsigned s = sf_shape(t.k);
            parent[s] = s;
            fl_count[s]++;
            for (int dir = 0; dir < 2; ++dir) {
                auto f = grid[g].find(gkey(t.map, t.cx + (dir == 0), t.cy + (dir == 1), t.z));
                if (f == grid[g].end()) continue;
                unsigned s2 = sf_shape(f->second);
                if (s2 != s) shape_pairs[{std::min(s, s2), std::max(s, s2)}]++;
            }
        }
    for (auto& [p, n] : shape_pairs) {
        if (role[p.first] != role[p.second]) continue;
        if (d.typeflags.get(p.first).x() != d.typeflags.get(p.second).x()) continue;
        double amin = double(n) / std::max(1, std::min(fl_count[p.first], fl_count[p.second]));
        double amax = double(n) / std::max(1, std::max(fl_count[p.first], fl_count[p.second]));
        if (amin >= 0.5 && amax >= 0.08 && n >= 20) parent[findp(p.first)] = findp(p.second);
    }
    // Floor sets: one per union root.
    std::map<unsigned, int> set_of_root;
    std::map<unsigned, int> set_of_shape;
    for (auto& [s, n] : fl_count) {
        unsigned r = findp(s);
        if (!set_of_root.count(r)) {
            set_of_root[r] = int(kit.floors.size());
            FloorSet fs;
            Role rr = role[s];
            fs.kind = rr == Role::kWater ? "water" : rr == Role::kLava ? "lava" : rr == Role::kRoof ? "roof" : "floor";
            fs.footprint = int(d.typeflags.get(s).x()) * 32;
            kit.floors.push_back(fs);
        }
        set_of_shape[s] = set_of_root[r];
    }
    {
        struct Acc {
            std::map<uint32_t, double> tiles;
            std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> pairs;
            std::array<std::map<uint32_t, double>, 16> edge;
            int roofed = 0, count = 0;
            std::map<int, int> z;
            unsigned top_shape = 0;
        };
        std::vector<Acc> acc(kit.floors.size());
        for (int g = 0; g < 2; ++g)
            for (const Tile& t : tiles[g]) {
                int si = set_of_shape[sf_shape(t.k)];
                Acc& a = acc[size_t(si)];
                a.tiles[t.k]++;
                a.count++;
                a.roofed += t.roofed;
                a.z[t.z]++;
                int mask = 0;
                const int ddx[4] = {0, 1, 0, -1}, ddy[4] = {-1, 0, 1, 0};
                for (int s = 0; s < 4; ++s) {
                    auto f = grid[g].find(gkey(t.map, t.cx + ddx[s], t.cy + ddy[s], t.z));
                    if (f != grid[g].end() && set_of_shape[sf_shape(f->second)] == si) mask |= 1 << s;
                }
                a.edge[size_t(mask)][t.k]++;
                for (int dir = 0; dir < 2; ++dir) {
                    auto f = grid[g].find(gkey(t.map, t.cx + (dir == 0), t.cy + (dir == 1), t.z));
                    if (f == grid[g].end() || set_of_shape[sf_shape(f->second)] != si) continue;
                    a.pairs[{t.k, f->second}][size_t(dir)]++;
                }
            }
        for (size_t i = 0; i < kit.floors.size(); ++i) {
            FloorSet& fs = kit.floors[i];
            Acc& a = acc[i];
            fs.count = a.count;
            fs.roof_fraction = a.count ? double(a.roofed) / a.count : 0;
            int bz = 0, bn = -1;
            for (auto& [z, n] : a.z)
                if (n > bn) bz = z, bn = n;
            fs.z = bz;
            // Fill candidates: tiles seen fully surrounded by the same material, else all tiles.
            std::map<uint32_t, double> core = a.edge[15];
            if (core.size() < 1) core = a.tiles;
            fs.tiles = top_pieces(core, 48, 0.002);
            std::map<uint32_t, int> index;
            for (size_t k = 0; k < fs.tiles.size(); ++k) index[sf(fs.tiles[k].shape, fs.tiles[k].frame)] = int(k);
            for (auto& [p, hv] : a.pairs) {
                auto ia = index.find(p.first), ib = index.find(p.second);
                if (ia == index.end() || ib == index.end()) continue;
                fs.pairs.push_back({ia->second, ib->second, hv[0], hv[1]});
            }
            for (int m = 0; m < 15; ++m) fs.edge[size_t(m)] = top_pieces(a.edge[size_t(m)], 8, 0.02);
            unsigned top = fs.tiles.empty() ? 0 : fs.tiles[0].shape;
            fs.id = "fs" + std::to_string(top);
            std::string n = name_of(top);
            fs.label = (fs.tiles.empty() ? std::string() : colour_word(d, top, fs.tiles[0].frame) + " ") +
                       (!n.empty() ? lower(n) : fs.kind) + (fs.footprint != 128 ? " (256)" : "") +
                       (fs.roof_fraction > 0.5 ? ", indoors" : "");
        }
        // Drop empty sets; keep ids unique.
        std::set<std::string> ids;
        std::vector<FloorSet> keep;
        for (FloorSet& fs : kit.floors) {
            if (fs.tiles.empty()) continue;
            std::string id = fs.id;
            for (int k = 2; ids.count(id); ++k) id = fs.id + "_" + std::to_string(k);
            fs.id = id;
            ids.insert(id);
            keep.push_back(fs);
        }
        kit.floors.swap(keep);
    }
    std::map<unsigned, std::string> floor_id_of_shape;
    for (const FloorSet& fs : kit.floors)
        for (const KitPiece& p : fs.tiles) floor_id_of_shape.emplace(p.shape, fs.id);
    for (auto& [s, si] : set_of_shape) {
        (void)si;
        if (floor_id_of_shape.count(s)) continue;
        // Shapes whose tiles never made the candidate list: map them to the set of their root.
        unsigned r = findp(s);
        for (const FloorSet& fs : kit.floors)
            for (const KitPiece& p : fs.tiles)
                if (findp(p.shape) == r && !floor_id_of_shape.count(s)) floor_id_of_shape[s] = fs.id;
    }

    // --- Wall styles. Wall pieces touching each other at the same z.
    struct WallAcc {
        std::array<std::map<uint32_t, double>, 5> ew, ns;
        std::map<uint32_t, double> post, jamb, door_ew, door_ns, ns_partner;
        std::map<uint16_t, int> door_q;
        int count = 0;
    };
    std::map<unsigned, WallAcc> wacc;  // by EW-128 anchor shape
    std::map<std::tuple<unsigned, unsigned, int, int>, int> wall_join;  // (a, b, dx, dy)
    std::map<unsigned, std::map<unsigned, int>> ns_touch_ew;             // NS-128 shape -> EW-128 shape -> touches
    for (auto& [m, list] : items) {
        std::unordered_map<uint64_t, std::vector<int>> cells;  // 64-unit cells -> wall/door/post item indices
        std::vector<int> walls;
        for (size_t i = 0; i < list.size(); ++i) {
            Role r = role[list[i].shape];
            if (r != Role::kWallEW && r != Role::kWallNS && r != Role::kPost && r != Role::kDoor) continue;
            walls.push_back(int(i));
            const MinedItem& it = list[i];
            for (int cy = it.y0() / 64; cy <= it.y / 64; ++cy)
                for (int cx = it.x0() / 64; cx <= it.x / 64; ++cx) cells[key3(cx, cy, 0)].push_back(int(i));
        }
        for (int i : walls) {
            const MinedItem& a = list[size_t(i)];
            Role ra = role[a.shape];
            std::set<int> seen;
            for (int cy = (a.y0() - 1) / 64; cy <= (a.y + 1) / 64; ++cy)
                for (int cx = (a.x0() - 1) / 64; cx <= (a.x + 1) / 64; ++cx) {
                    auto f = cells.find(key3(cx, cy, 0));
                    if (f == cells.end()) continue;
                    for (int j : f->second) {
                        if (j == i || !seen.insert(j).second) continue;
                        const MinedItem& b = list[size_t(j)];
                        if (b.z != a.z) continue;
                        bool tx = a.x + 1 >= b.x0() && b.x + 1 >= a.x0();
                        bool ty = a.y + 1 >= b.y0() && b.y + 1 >= a.y0();
                        if (!tx || !ty) continue;
                        Role rb = role[b.shape];
                        if (ra != Role::kDoor && rb != Role::kDoor) wall_join[{a.shape, b.shape, b.x - a.x, b.y - a.y}]++;
                        if (rb == Role::kDoor && ra == Role::kPost) wacc[0].jamb[sf(a.shape, a.frame)]++;  // global jambs
                        if (ra == Role::kWallNS && a.yd == 128 && rb == Role::kWallEW && b.xd == 128) ns_touch_ew[a.shape][b.shape]++;
                        if (ra != Role::kWallEW || a.xd != 128) continue;
                        WallAcc& w = wacc[a.shape];
                        int lb = std::max(b.xd, b.yd) / 32;
                        bool colinear = b.y == a.y && b.yd == a.yd && (b.x0() == a.x + 1 || b.x + 1 == a.x0());
                        bool same_h = std::abs(b.zd - a.zd) <= 8;
                        if (rb == Role::kWallEW && colinear && same_h && lb >= 1 && lb <= 4) w.ew[size_t(lb)][sf(b.shape, b.frame)]++;
                        else if (rb == Role::kWallNS && same_h && lb >= 1 && lb <= 4) w.ns[size_t(lb)][sf(b.shape, b.frame)]++;
                        else if (rb == Role::kPost && same_h) w.post[sf(b.shape, b.frame)]++;
                        else if (rb == Role::kDoor) {
                            (b.xd > b.yd ? w.door_ew : w.door_ns)[sf(b.shape, b.frame)]++;
                            w.door_q[b.quality & ~0x100]++;
                        }
                    }
                }
            if (ra == Role::kWallEW && a.xd == 128) {
                WallAcc& w = wacc[a.shape];
                w.ew[4][sf(a.shape, a.frame)] += 0.25;  // the anchor's own frames
                w.count++;
            }
        }
    }
    // Second hop: NS pieces colinear with the NS partner, and posts touching it.
    // Simplified: the NS lengths of a style are the NS-shapes that touch its EW anchor; NS 128 partner
    // shapes that touch EW anchors of the style contribute their frames.
    std::map<unsigned, std::string> wall_style_of_shape;
    for (auto& [anchor, w] : wacc) {
        if (anchor == 0 || w.count < 12) continue;
        WallStyle ws;
        ws.id = "ws" + std::to_string(anchor);
        const ShapeInfo& si = d.typeflags.get(anchor);
        ws.height = int(si.z()) * 8;
        ws.land_top = si.is(ShapeInfo::kLand);
        ws.count = w.count;
        for (int l = 1; l <= 4; ++l) {
            ws.ew[size_t(l)] = top_pieces(w.ew[size_t(l)], 8, 0.03);
            ws.ns[size_t(l)] = top_pieces(w.ns[size_t(l)], 8, 0.03);
        }
        // NS 128: shapes that mostly touch this anchor.
        std::map<uint32_t, double> ns4 = w.ns[4];
        for (auto& [nss, m] : ns_touch_ew) {
            unsigned best = 0;
            int bn = 0;
            for (auto& [e, n] : m)
                if (n > bn) best = e, bn = n;
            if (best != anchor) continue;
            const ShapeStat& st = stats[nss];
            for (auto& [f, n] : st.frames) ns4[sf(nss, f)] += 0.25 * n;
        }
        ws.ns[4] = top_pieces(ns4, 8, 0.03);
        ws.post = top_pieces(w.post, 8, 0.03);
        if (!w.door_ew.empty()) ws.door_ew = top_pieces(w.door_ew, 1)[0];
        if (!w.door_ns.empty()) ws.door_ns = top_pieces(w.door_ns, 1)[0];
        int bq = -1;
        for (auto& [q, n] : w.door_q)
            if (n > bq) ws.door_quality = q, bq = n;
        ws.label = colour_word(d, anchor, ws.ew[4].empty() ? 0 : ws.ew[4][0].frame) + " wall, " + std::to_string(ws.height) +
                   " high" + (ws.land_top ? ", walkable top" : "");
        kit.walls.push_back(ws);
    }
    std::sort(kit.walls.begin(), kit.walls.end(), [](const WallStyle& a, const WallStyle& b) { return a.count > b.count; });
    // Pieces belong to the style of their own EW anchor first, then to the most used style that has them.
    for (const WallStyle& ws : kit.walls) wall_style_of_shape[unsigned(std::stoi(ws.id.substr(2)))] = ws.id;
    for (const WallStyle& ws : kit.walls) {
        for (int l = 1; l <= 4; ++l) {
            for (auto& p : ws.ew[size_t(l)]) wall_style_of_shape.emplace(p.shape, ws.id);
            for (auto& p : ws.ns[size_t(l)]) wall_style_of_shape.emplace(p.shape, ws.id);
        }
        for (auto& p : ws.post) wall_style_of_shape.emplace(p.shape, ws.id);
    }
    // Styles without a door borrow the most common door: DOOR_EW 69 / DOOR_NS 68 frame 0.
    std::map<uint32_t, double> jambs = wacc[0].jamb;
    for (WallStyle& ws : kit.walls) {
        std::map<uint32_t, double> j;
        for (const KitPiece& p : ws.post) {
            auto f = jambs.find(sf(p.shape, p.frame));
            if (f != jambs.end()) j[f->first] = f->second;
        }
        ws.jamb = top_pieces(j, 4);
        if (!ws.door_ew.shape) ws.door_ew = {69, 0, 1};
        if (!ws.door_ns.shape) ws.door_ns = {68, ws.door_ew.shape == 69 ? ws.door_ew.frame : uint16_t(0), 1};
        if (ws.door_ew.shape == 69 && ws.door_ns.shape == 0) ws.door_ns = {68, ws.door_ew.frame, 1};
    }

    // --- Areas (1024 x 1024) and themes.
    std::vector<Area> areas;
    std::map<std::tuple<unsigned, int, int>, size_t> area_index;
    for (auto& [m, list] : items)
        for (const MinedItem& it : list) {
            Role r = role[it.shape];
            std::string f;
            if (r == Role::kFloor || r == Role::kWater || r == Role::kLava) {
                auto fi = floor_id_of_shape.find(it.shape);
                f = fi != floor_id_of_shape.end() ? fi->second : "";
                if (f.empty()) continue;
            } else if (r == Role::kWallEW || r == Role::kWallNS || r == Role::kPost) {
                auto wi = wall_style_of_shape.find(it.shape);
                f = wi != wall_style_of_shape.end() ? wi->second : "";
                if (f.empty()) continue;
            } else if (r == Role::kTree || r == Role::kPlant || r == Role::kDecoration || r == Role::kLight) {
                f = "s" + std::to_string(it.shape);
            } else
                continue;
            auto key = std::make_tuple(m, it.x / 1024, it.y / 1024);
            auto ai = area_index.find(key);
            if (ai == area_index.end()) {
                ai = area_index.emplace(key, areas.size()).first;
                Area a;
                a.map = m;
                a.ax = it.x / 1024;
                a.ay = it.y / 1024;
                areas.push_back(a);
            }
            Area& a = areas[ai->second];
            double w = (r == Role::kFloor || r == Role::kWater || r == Role::kLava) ? 1.0 : r == Role::kDecoration ? 0.5 : 1.5;
            a.feat[f] += w;
            if (r == Role::kFloor) a.floors++;
        }
    {
        std::vector<Area> keep;
        for (Area& a : areas)
            if (a.floors >= 12) {
                for (auto& [k, v] : a.feat) v = std::sqrt(v);
                a.norm = norm_of(a.feat);
                keep.push_back(a);
            }
        areas.swap(keep);
    }
    const auto& seeds = theme_seeds();
    std::vector<std::map<std::string, double>> cent(seeds.size());
    std::vector<double> cnorm(seeds.size());
    auto seed_of = [&](unsigned map, size_t t) {
        return std::find(seeds[t].maps.begin(), seeds[t].maps.end(), map) != seeds[t].maps.end();
    };
    for (Area& a : areas)
        for (size_t t = 0; t < seeds.size(); ++t)
            if (seed_of(a.map, t)) a.theme = int(t);
    for (int iter = 0; iter < 8; ++iter) {
        for (auto& c : cent) c.clear();
        std::vector<int> n(seeds.size(), 0);
        for (const Area& a : areas) {
            if (a.theme < 0) continue;
            for (auto& [k, v] : a.feat) cent[size_t(a.theme)][k] += v / a.norm;
            n[size_t(a.theme)]++;
        }
        for (size_t t = 0; t < seeds.size(); ++t) cnorm[t] = norm_of(cent[t]);
        int changed = 0;
        for (Area& a : areas) {
            int best = a.theme;
            double bs = -1;
            for (size_t t = 0; t < seeds.size(); ++t) {
                double s = cosine(a.feat, a.norm, cent[t], cnorm[t]) + (seed_of(a.map, t) ? 0.45 : 0.0);
                if (s > bs) bs = s, best = int(t);
            }
            if (best != a.theme) changed++, a.theme = best;
        }
        if (!changed) break;
    }
    // Theme kits from their areas.
    std::map<std::tuple<unsigned, int, int>, int> area_theme;
    for (const Area& a : areas) area_theme[{a.map, a.ax, a.ay}] = a.theme;
    struct ThemeAcc {
        std::map<std::string, double> floor_use, floor_roofed, wall_use;
        std::map<uint32_t, double> lights, plants, trees, decor, clutter, furniture;
        std::array<std::map<uint32_t, double>, 4> trim_void;
        std::array<std::map<uint32_t, double>, 16> shore;
        int floor_cells = 0, scatter_items = 0;
        std::map<unsigned, int> area_maps;
        int areas = 0;
    };
    std::vector<ThemeAcc> tacc(seeds.size());
    for (const Area& a : areas) {
        tacc[size_t(a.theme)].areas++;
        tacc[size_t(a.theme)].area_maps[a.map]++;
    }
    auto theme_at = [&](unsigned m, int x, int y) {
        auto f = area_theme.find({m, x / 1024, y / 1024});
        return f == area_theme.end() ? -1 : f->second;
    };
    std::map<unsigned, std::map<int, double>> shape_theme_use;
    for (int g = 0; g < 2; ++g)
        for (const Tile& t : tiles[g]) {
            int th = theme_at(t.map, t.cx * (g ? 256 : 128), t.cy * (g ? 256 : 128));
            if (th < 0) continue;
            auto fi = floor_id_of_shape.find(sf_shape(t.k));
            if (fi == floor_id_of_shape.end()) continue;
            tacc[size_t(th)].floor_use[fi->second] += g ? 4 : 1;
            if (t.roofed) tacc[size_t(th)].floor_roofed[fi->second] += g ? 4 : 1;
            if (role[sf_shape(t.k)] == Role::kFloor) tacc[size_t(th)].floor_cells += g ? 4 : 1;
        }
    // Shores: 8-high floor tiles at the water's z next to water tiles, by wet-neighbour mask.
    {
        auto wet = [&](unsigned map, int cx, int cy, int z, bool shore_too) {
            auto f = grid[0].find(gkey(map, cx, cy, z));
            if (f == grid[0].end()) return false;
            unsigned s = sf_shape(f->second);
            if (role[s] == Role::kWater) return true;
            return shore_too && role[s] == Role::kFloor && d.typeflags.get(s).z() == 1;
        };
        const int ddx[4] = {0, 1, 0, -1}, ddy[4] = {-1, 0, 1, 0};
        for (const Tile& t : tiles[0]) {
            unsigned s = sf_shape(t.k);
            if (role[s] != Role::kFloor || d.typeflags.get(s).z() != 1) continue;
            int mask = 0, water = 0;
            for (int k = 0; k < 4; ++k) {
                if (wet(t.map, t.cx + ddx[k], t.cy + ddy[k], t.z, true)) mask |= 1 << k;
                water += wet(t.map, t.cx + ddx[k], t.cy + ddy[k], t.z, false);
            }
            if (!water || mask == 15) continue;
            int th = theme_at(t.map, t.cx * 128, t.cy * 128);
            if (th >= 0) tacc[size_t(th)].shore[size_t(mask)][t.k]++;
        }
    }
    for (auto& [m, list] : items) {
        // Floor presence per 128 cell (any z) for the void-edge trims.
        std::unordered_map<uint64_t, std::vector<int>> floor_z;
        for (const MinedItem& it : list) {
            Role r = role[it.shape];
            if ((r == Role::kFloor || r == Role::kWater || r == Role::kLava) && it.zd <= 8)
                for (int cy = it.y0() / 128; cy <= it.y / 128; ++cy)
                    for (int cx = it.x0() / 128; cx <= it.x / 128; ++cx) floor_z[key3(cx, cy, 0)].push_back(it.z);
        }
        auto floor_near = [&](int cx, int cy, int z) {
            auto f = floor_z.find(key3(cx, cy, 0));
            if (f == floor_z.end()) return false;
            for (int fz : f->second)
                if (std::abs(fz - z) <= 64) return true;
            return false;
        };
        for (const MinedItem& it : list) {
            int th = theme_at(m, it.x, it.y);
            Role r = role[it.shape];
            shape_theme_use[it.shape][th] += 1;
            if (th < 0) continue;
            ThemeAcc& ta = tacc[size_t(th)];
            uint32_t k = sf(it.shape, it.frame);
            switch (r) {
            case Role::kWallEW:
            case Role::kWallNS:
            case Role::kPost: {
                auto wi = wall_style_of_shape.find(it.shape);
                if (wi != wall_style_of_shape.end()) ta.wall_use[wi->second] += 1;
                break;
            }
            case Role::kLight: ta.lights[k]++; break;
            case Role::kPlant: ta.plants[k]++, ta.scatter_items++; break;
            case Role::kTree: ta.trees[k]++, ta.scatter_items++; break;
            case Role::kDecoration:
                if (it.xd <= 128 && it.yd <= 128) ta.decor[k]++, ta.scatter_items++;
                break;
            case Role::kClutter: ta.clutter[k]++; break;
            case Role::kFurniture: ta.furniture[k]++; break;
            case Role::kTrim: {
                // A strip along one edge of a floor cell whose neighbour on that side has no floor.
                int cx = it.x / 128, cy = it.y / 128;
                bool ew = it.xd > it.yd;
                int side = -1;
                if (ew && it.y0() % 128 == 0 && !floor_near(cx, cy - 1, it.z)) side = 0;
                else if (ew && (it.y + 1) % 128 == 0 && !floor_near(cx, cy + 1, it.z)) side = 2;
                else if (!ew && (it.x + 1) % 128 == 0 && !floor_near(cx + 1, cy, it.z)) side = 1;
                else if (!ew && it.x0() % 128 == 0 && !floor_near(cx - 1, cy, it.z)) side = 3;
                if (side >= 0 && floor_near(cx, cy, it.z)) ta.trim_void[size_t(side)][k]++;
                break;
            }
            default: break;
            }
        }
    }
    for (size_t t = 0; t < seeds.size(); ++t) {
        ThemeAcc& ta = tacc[t];
        ThemeKit tk;
        tk.name = seeds[t].name;
        tk.label = seeds[t].label;
        tk.maps = seeds[t].maps;
        for (auto& [m, n] : ta.area_maps) tk.area_maps.push_back({m, n});
        tk.areas = ta.areas;
        auto kind_of = [&](const std::string& id) {
            const FloorSet* f = kit.floor(id);
            return f ? f->kind : std::string();
        };
        std::vector<std::pair<double, std::string>> fl;
        for (auto& [id, n] : ta.floor_use) fl.push_back({n, id});
        std::sort(fl.rbegin(), fl.rend());
        double floor_total = 0;
        for (auto& [n, id] : fl)
            if (kind_of(id) == "floor") floor_total += n;
        for (auto& [n, id] : fl) {
            std::string k = kind_of(id);
            double roofed = ta.floor_roofed[id] / std::max(1.0, n);
            const FloorSet* fs = kit.floor(id);
            if (k == "floor") {
                if (fs && fs->footprint != 128) continue;
                if (roofed < 0.5 && tk.floor.empty()) tk.floor = id;
                else if (roofed >= 0.5 && tk.interior_floor.empty() && n >= 0.004 * floor_total) tk.interior_floor = id;
                else if (tk.floors_alt.size() < 4 && n >= 0.03 * floor_total && id != tk.floor) tk.floors_alt.push_back(id);
            } else if (k == "water" && tk.water.empty() && fs && fs->footprint == 128) tk.water = id;
            else if (k == "lava" && tk.lava.empty() && fs && fs->footprint == 128) tk.lava = id;
        }
        if (tk.floor.empty() && !fl.empty())
            for (auto& [n, id] : fl)
                if (kind_of(id) == "floor") { tk.floor = id; break; }
        // Roofs: the most used roof set in the theme's areas (by roof tiles).
        double best_roof = 0;
        for (const FloorSet& fs : kit.floors) {
            if (fs.kind != "roof" || fs.footprint != 128) continue;
            double n = 0;
            for (const KitPiece& p : fs.tiles) n += shape_theme_use[p.shape][int(t)];
            if (n > best_roof) best_roof = n, tk.roof = fs.id;
        }
        if (best_roof < 0.02 * ta.floor_cells) tk.roof.clear();  // caves and dungeons have no roofs
        std::vector<std::pair<double, std::string>> wl;
        for (auto& [id, n] : ta.wall_use) wl.push_back({n, id});
        std::sort(wl.rbegin(), wl.rend());
        for (auto& [n, id] : wl) {
            const WallStyle* ws = kit.wall(id);
            if (!ws) continue;
            if (tk.wall.empty() && !ws->ew[4].empty()) tk.wall = id;
            if (tk.ledge.empty() && ws->land_top && !ws->ew[4].empty()) tk.ledge = id;
        }
        if (tk.ledge.empty()) tk.ledge = tk.wall;
        tk.lights = top_pieces(ta.lights, 8, 0.02);
        tk.plants = top_pieces(ta.plants, 10, 0.02);
        tk.trees = top_pieces(ta.trees, 6, 0.03);
        tk.decor = top_pieces(ta.decor, 12, 0.02);
        tk.clutter = top_pieces(ta.clutter, 12, 0.02);
        tk.furniture = top_pieces(ta.furniture, 12, 0.02);
        for (int s = 0; s < 4; ++s) tk.trim_void[size_t(s)] = top_pieces(ta.trim_void[size_t(s)], 4, 0.05);
        for (int m = 0; m < 15; ++m) tk.shore[size_t(m)] = top_pieces(ta.shore[size_t(m)], 6, 0.03);
        tk.scatter = std::clamp(ta.floor_cells ? double(ta.scatter_items) / ta.floor_cells : 0.03, 0.01, 0.12);
        // Fallbacks: the two most similar themes by centroid.
        std::vector<std::pair<double, std::string>> sim;
        for (size_t u = 0; u < seeds.size(); ++u)
            if (u != t) sim.push_back({cosine(cent[t], cnorm[t], cent[u], cnorm[u]), seeds[u].name});
        std::sort(sim.rbegin(), sim.rend());
        for (size_t k = 0; k < sim.size() && k < 2; ++k) tk.fallback.push_back(sim[k].second);
        kit.themes[tk.name] = tk;
    }
    // Missing parts come from the fallbacks.
    for (auto& [name, tk] : kit.themes) {
        for (const std::string& fb : tk.fallback) {
            const ThemeKit& o = kit.themes[fb];
            if (tk.floor.empty()) tk.floor = o.floor;
            if (tk.wall.empty()) tk.wall = o.wall;
            if (tk.ledge.empty()) tk.ledge = o.ledge;
            if (tk.water.empty()) tk.water = o.water;
        }
        if (tk.wall.empty() && !kit.walls.empty()) tk.wall = kit.walls[0].id;
        if (tk.ledge.empty()) tk.ledge = tk.wall;
        if (tk.water.empty())
            for (const FloorSet& fs : kit.floors)
                if (fs.kind == "water" && fs.footprint == 128) { tk.water = fs.id; break; }
    }
    // Style and shape themes (shares).
    std::map<std::string, std::map<int, double>> style_theme;
    for (auto& [s, use] : shape_theme_use) {
        auto wi = wall_style_of_shape.find(s);
        if (wi != wall_style_of_shape.end())
            for (auto& [t, n] : use) style_theme[wi->second][t] += n;
    }
    auto shares = [&](const std::map<int, double>& use) {
        double tot = 0;
        for (auto& [t, n] : use)
            if (t >= 0) tot += n;
        std::vector<std::pair<double, std::string>> v;
        for (auto& [t, n] : use)
            if (t >= 0 && tot > 0) v.push_back({n / tot, seeds[size_t(t)].name});
        std::sort(v.rbegin(), v.rend());
        std::vector<std::pair<std::string, double>> out;
        for (auto& [w, n] : v)
            if (out.size() < 4 && w >= 0.05) out.push_back({n, w});
        return out;
    };
    for (WallStyle& ws : kit.walls) ws.themes = shares(style_theme[ws.id]);

    // --- Drop unusable frames from everything the dresser picks from.
    {
        FrameCheck fc(d);
        auto mean_rgb = [&](unsigned shape, unsigned frame, double out[3]) {
            const ShapeFrame* f = d.shapes.frame(shape, frame);
            double n = 0;
            out[0] = out[1] = out[2] = 0;
            if (f)
                for (uint8_t p : f->pixels) {
                    if (p == ShapeFrame::kTransparent) continue;
                    for (int c = 0; c < 3; ++c) out[c] += d.palette.rgb8[p][size_t(c)];
                    ++n;
                }
            for (int c = 0; c < 3; ++c) out[c] /= std::max(1.0, n);
        };
        for (FloorSet& fs : kit.floors) {
            // Accents: tiles whose mean colour is far from the material's (weighted) mean colour.
            std::vector<std::array<double, 3>> col(fs.tiles.size());
            double mean[3] = {0, 0, 0}, wsum = 0;
            for (size_t i = 0; i < fs.tiles.size(); ++i) {
                mean_rgb(fs.tiles[i].shape, fs.tiles[i].frame, col[i].data());
                for (int c = 0; c < 3; ++c) mean[c] += col[i][size_t(c)] * fs.tiles[i].weight;
                wsum += fs.tiles[i].weight;
            }
            for (int c = 0; c < 3; ++c) mean[c] /= std::max(1.0, wsum);
            // Keep the pair indices valid: rebuild them over the kept tiles.
            std::vector<int> remap(fs.tiles.size(), -1);
            std::vector<KitPiece> kept;
            fs.accents.clear();
            for (size_t i = 0; i < fs.tiles.size(); ++i) {
                if (!fc.ok(fs.tiles[i].shape, fs.tiles[i].frame, true)) continue;
                double dist = std::sqrt(std::pow(col[i][0] - mean[0], 2) + std::pow(col[i][1] - mean[1], 2) + std::pow(col[i][2] - mean[2], 2));
                if (dist > 45 && fs.kind == "floor") {
                    fs.accents.push_back(fs.tiles[i]);
                    continue;
                }
                remap[i] = int(kept.size()), kept.push_back(fs.tiles[i]);
            }
            if (kept.empty()) continue;  // keep a set that would be emptied as it is
            std::vector<std::array<int, 4>> pairs;
            for (auto& p : fs.pairs)
                if (remap[size_t(p[0])] >= 0 && remap[size_t(p[1])] >= 0) pairs.push_back({remap[size_t(p[0])], remap[size_t(p[1])], p[2], p[3]});
            fs.tiles.swap(kept);
            fs.pairs.swap(pairs);
            for (auto& e : fs.edge) fc.filter(e, false);
        }
        for (WallStyle& ws : kit.walls) {
            for (int l = 1; l <= 4; ++l) {
                fc.filter(ws.ew[size_t(l)], true);
                fc.filter(ws.ns[size_t(l)], true);
            }
            fc.filter(ws.post, true);
            fc.filter(ws.jamb, true);
        }
        for (auto& [n, t] : kit.themes)
            for (auto* v : {&t.lights, &t.plants, &t.trees, &t.decor, &t.clutter, &t.furniture}) fc.filter(*v, false);
    }

    // --- Shape entries.
    for (auto& [s, st] : stats) {
        ShapeEntry e;
        e.shape = s;
        e.role = role[s];
        e.name = name_of(s);
        const ShapeInfo& si = d.typeflags.get(s);
        e.fp[0] = int(si.x()) * 32;
        e.fp[1] = int(si.y()) * 32;
        e.fp[2] = int(si.z()) * 8;
        e.tflags = si.flags();
        e.family = si.family();
        e.count = st.count;
        e.nonfixed = st.nonfixed;
        int bz = 0, bn = -1;
        for (auto& [z, n] : st.z)
            if (n > bn) bz = z, bn = n;
        e.z = bz;
        std::map<uint32_t, double> fr;
        for (auto& [f, n] : st.frames) fr[sf(s, f)] = n;
        e.frames = top_pieces(fr, 64);
        e.themes = shares(shape_theme_use[s]);
        kit.shapes[s] = e;
    }
    // Shapes the dresser may place but the maps never use (the doors are always there).
    for (unsigned s : {68u, 69u, 340u, 342u})
        if (!kit.shapes.count(s)) {
            ShapeEntry e;
            e.shape = s;
            e.role = Role::kDoor;
            e.name = name_of(s);
            const ShapeInfo& si = d.typeflags.get(s);
            e.fp[0] = int(si.x()) * 32;
            e.fp[1] = int(si.y()) * 32;
            e.fp[2] = int(si.z()) * 8;
            e.tflags = si.flags();
            e.family = si.family();
            kit.shapes[s] = e;
        }

    // --- Prefabs: groups of object items standing together.
    std::map<std::string, int> per_label;
    std::set<std::string> signatures;
    struct Cand {
        Prefab p;
        std::string rule;
        double score;
    };
    std::vector<Cand> cands;
    int prefab_candidates = 0;
    for (auto& [m, list] : items) {
        std::vector<int> objs;
        for (size_t i = 0; i < list.size(); ++i) {
            const MinedItem& it = list[i];
            Role r = role[it.shape];
            if (is_structure(r) || r == Role::kEgg || r == Role::kNpc || r == Role::kEffect || r == Role::kNone) continue;
            if (it.xd > 384 || it.yd > 384) continue;
            if (it.flags & (kItemInvisible | kItemContained)) continue;
            objs.push_back(int(i));
        }
        // Single-linkage groups: XY gap <= 32, z ranges within 8.
        std::unordered_map<uint64_t, std::vector<int>> cells;
        for (size_t k = 0; k < objs.size(); ++k) {
            const MinedItem& it = list[size_t(objs[k])];
            for (int cy = it.y0() / 128; cy <= it.y / 128; ++cy)
                for (int cx = it.x0() / 128; cx <= it.x / 128; ++cx) cells[key3(cx, cy, 0)].push_back(int(k));
        }
        std::vector<int> up(objs.size());
        std::iota(up.begin(), up.end(), 0);
        std::function<int(int)> fp = [&](int a) {
            while (up[size_t(a)] != a) a = up[size_t(a)] = up[size_t(up[size_t(a)])];
            return a;
        };
        for (size_t k = 0; k < objs.size(); ++k) {
            const MinedItem& a = list[size_t(objs[k])];
            for (int cy = (a.y0() - 33) / 128; cy <= (a.y + 33) / 128; ++cy)
                for (int cx = (a.x0() - 33) / 128; cx <= (a.x + 33) / 128; ++cx) {
                    auto f = cells.find(key3(cx, cy, 0));
                    if (f == cells.end()) continue;
                    for (int j : f->second) {
                        if (size_t(j) <= k) continue;
                        const MinedItem& b = list[size_t(objs[size_t(j)])];
                        bool nx = a.x + 33 >= b.x0() && b.x + 33 >= a.x0();
                        bool ny = a.y + 33 >= b.y0() && b.y + 33 >= a.y0();
                        bool nz = a.z <= b.z + b.zd + 8 && b.z <= a.z + a.zd + 8;
                        if (nx && ny && nz) up[size_t(fp(int(k)))] = fp(j);
                    }
                }
        }
        std::map<int, std::vector<int>> linked;
        for (size_t k = 0; k < objs.size(); ++k) linked[fp(int(k))].push_back(objs[k]);
        // Groups too big for a point of interest are cut into 512 x 512 pieces.
        std::vector<std::vector<int>> groups;
        for (auto& [g, mem] : linked) {
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
            for (int i : mem) {
                const MinedItem& it = list[size_t(i)];
                x0 = std::min(x0, it.x0()), y0 = std::min(y0, it.y0()), x1 = std::max(x1, it.x), y1 = std::max(y1, it.y);
            }
            if (mem.size() <= 40 && x1 - x0 <= 768 && y1 - y0 <= 768) {
                groups.push_back(mem);
                continue;
            }
            std::map<std::pair<int, int>, std::vector<int>> parts;
            for (int i : mem) {
                const MinedItem& it = list[size_t(i)];
                parts[{(it.x - it.xd / 2) / 512, (it.y - it.yd / 2) / 512}].push_back(i);
            }
            for (auto& [k, v] : parts) groups.push_back(v);
        }
        // Floor z lookup for the group's floor level.
        std::unordered_map<uint64_t, std::vector<int>> floor_z;
        for (const MinedItem& it : list) {
            Role r = role[it.shape];
            if ((r == Role::kFloor || r == Role::kRoof) && it.zd <= 8)
                for (int cy = it.y0() / 128; cy <= it.y / 128; ++cy)
                    for (int cx = it.x0() / 128; cx <= it.x / 128; ++cx) floor_z[key3(cx, cy, 0)].push_back(it.z);
        }
        for (auto& members : groups) {
            if (members.empty() || members.size() > 40) continue;
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, zmin = 1 << 30, zmax = -1;
            for (int i : members) {
                const MinedItem& it = list[size_t(i)];
                x0 = std::min(x0, it.x0());
                y0 = std::min(y0, it.y0());
                x1 = std::max(x1, it.x);
                y1 = std::max(y1, it.y);
                zmin = std::min(zmin, it.z);
                zmax = std::max(zmax, it.z + it.zd);
            }
            if (x1 - x0 > 768 || y1 - y0 > 768) continue;
            ++prefab_candidates;
            // Floor level: the highest floor at or below the lowest item, near the middle.
            int fz = zmin;
            {
                auto f = floor_z.find(key3((x0 + x1) / 256, (y0 + y1) / 256, 0));
                int best = -1;
                if (f != floor_z.end())
                    for (int z : f->second)
                        if (z <= zmin && z > best) best = z;
                if (best >= 0 && zmin - best <= 8) fz = best;
            }
            // Keep only supported members: on the floor, or on top of another member.
            std::vector<int> kept(members.begin(), members.end());
            for (bool changed = true; changed;) {
                changed = false;
                for (size_t a = 0; a < kept.size(); ++a) {
                    const MinedItem& it = list[size_t(kept[a])];
                    if (it.z == fz || (it.flags & kItemHanging)) continue;
                    bool ok = false;
                    for (size_t b = 0; b < kept.size() && !ok; ++b) {
                        if (a == b) continue;
                        const MinedItem& o = list[size_t(kept[b])];
                        bool ov = it.x0() <= o.x && o.x0() <= it.x && it.y0() <= o.y && o.y0() <= it.y;
                        if (ov && o.z + o.zd == it.z) ok = true;
                    }
                    if (!ok) {
                        kept.erase(kept.begin() + long(a));
                        changed = true;
                        break;
                    }
                }
            }
            if (kept.empty()) continue;
            // Wall-mounted things (paintings, plaques, tapestries) need a wall behind them.
            std::vector<int> k2;
            for (int i : kept) {
                std::string n = name_of(list[size_t(i)].shape);
                if (has(n, "PAINT") || has(n, "PLAQUE") || has(n, "TAPEST") || has(n, "SHFSID") || has(n, "WEB") ||
                    has(n, "LAMPTP"))
                    continue;
                k2.push_back(i);
            }
            if (k2.empty()) continue;
            // Label.
            std::map<std::string, int> name_hits;
            for (int i : k2) name_hits[name_of(list[size_t(i)].shape)]++;
            const LabelRule* rule = nullptr;
            for (const LabelRule& r : label_rules()) {
                int hits = 0;
                for (const char* n : r.names) {
                    auto f = name_hits.find(n);
                    if (f != name_hits.end()) hits += f->second;
                }
                if (hits >= r.min_hits) { rule = &r; break; }
            }
            if (!rule || k2.size() < (rule->single ? 1u : 3u)) continue;
            // Signature: the multiset of shapes.
            std::vector<unsigned> sig;
            for (int i : k2) sig.push_back(list[size_t(i)].shape);
            std::sort(sig.begin(), sig.end());
            std::string s = rule->id;
            for (unsigned v : sig) s += "," + std::to_string(v);
            if (!signatures.insert(s).second) continue;
            Prefab p;
            p.label = rule->label;
            int th = theme_at(m, (x0 + x1) / 2, (y0 + y1) / 2);
            p.theme = th >= 0 ? seeds[size_t(th)].name : "";
            p.map = m;
            std::set<std::string> kw(rule->keywords.begin(), rule->keywords.end());
            if (!p.theme.empty()) kw.insert(p.theme);
            for (auto& [n, c] : name_hits)
                if (!n.empty()) kw.insert(lower(n));
            p.keywords.assign(kw.begin(), kw.end());
            x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, zmax = -1;
            for (int i : k2) {
                const MinedItem& it = list[size_t(i)];
                x0 = std::min(x0, it.x0());
                y0 = std::min(y0, it.y0());
                x1 = std::max(x1, it.x);
                y1 = std::max(y1, it.y);
                zmax = std::max(zmax, it.z + it.zd);
            }
            p.x = x0;
            p.y = y0;
            p.z = fz;
            p.sx = x1 - x0 + 1;
            p.sy = y1 - y0 + 1;
            p.sz = zmax - fz;
            for (int i : k2) {
                const MinedItem& it = list[size_t(i)];
                PrefabItem pi;
                pi.shape = it.shape;
                pi.frame = it.frame;
                pi.dx = it.x - x0;
                pi.dy = it.y - y0;
                pi.dz = it.z - fz;
                pi.flags = uint16_t(it.flags & (kItemFlipped | kItemHanging | kItemInvisible));
                pi.quality = it.quality;
                pi.npcnum = it.npcnum;
                pi.mapnum = it.mapnum;
                p.items.push_back(pi);
            }
            std::sort(p.items.begin(), p.items.end(), [](const PrefabItem& a, const PrefabItem& b) {
                return std::make_tuple(a.dz, a.dy, a.dx, a.shape, a.frame) < std::make_tuple(b.dz, b.dy, b.dx, b.shape, b.frame);
            });
            // Quality: a handful of items in a compact box, not mostly plants (unless a grove).
            int green = 0;
            for (int i : k2) {
                Role r = role[list[size_t(i)].shape];
                green += r == Role::kPlant || r == Role::kTree;
            }
            double sc = -std::abs(int(k2.size()) - 7) * 0.3 - double(p.sx) * p.sy / (256.0 * 256.0);
            if (std::string(rule->id) != "grove") sc -= 4.0 * green / double(k2.size());
            cands.push_back({p, rule->id, sc});
        }
    }
    // Keep up to 12 per label, at most 2 per theme on the first pass (variety across places).
    {
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.rule != b.rule ? a.rule < b.rule : a.score > b.score;
        });
        std::map<std::pair<std::string, std::string>, int> per_theme;
        std::vector<bool> taken(cands.size(), false);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < cands.size(); ++i) {
                Cand& c = cands[i];
                if (taken[i] || per_label[c.rule] >= 12) continue;
                if (pass == 0 && per_theme[{c.rule, c.p.theme}] >= 2) continue;
                taken[i] = true;
                per_theme[{c.rule, c.p.theme}]++;
                per_label[c.rule]++;
            }
        std::map<std::string, int> num;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (!taken[i]) continue;
            cands[i].p.id = cands[i].rule + "_" + std::to_string(++num[cands[i].rule]);
            kit.prefabs.push_back(cands[i].p);
        }
    }

    // Composed prefabs: the original graves stand alone, so a "grave row" is three of them in a
    // line (different headstones, 32 apart), and a "graveyard" two rows.
    {
        std::vector<const Prefab*> singles;
        for (const Prefab& p : kit.prefabs)
            if (p.label == "grave" && p.items.size() == 1) singles.push_back(&p);
        std::stable_sort(singles.begin(), singles.end(), [](const Prefab* a, const Prefab* b) { return a->items[0].frame < b->items[0].frame; });
        for (int rows = 1; rows <= 2 && !singles.empty(); ++rows) {
            Prefab row;
            row.id = rows == 1 ? "grave_row_1" : "graveyard_1";
            row.label = rows == 1 ? "grave row" : "graveyard";
            row.keywords = {"grave", "graves", "row", "cemetery", "graveyard", "burial", "tomb", "headstone", "dead"};
            row.theme = "cemetery";
            row.map = singles[0]->map;
            int x = 0, sy = 0;
            for (int r2 = 0; r2 < rows; ++r2) {
                x = 0;
                for (int k = 0; k < 3; ++k) {
                    const Prefab* g = singles[size_t(k) % singles.size()];
                    PrefabItem it = g->items[0];
                    it.dx += x;
                    it.dy += r2 * (g->sy + 64);
                    row.items.push_back(it);
                    x += g->sx + 32;
                    row.sz = std::max(row.sz, g->sz);
                    sy = std::max(sy, r2 * (g->sy + 64) + g->sy);
                }
            }
            row.sx = x - 32;
            row.sy = sy;
            kit.prefabs.push_back(row);
        }
    }

    // --- Adjacency statistics (top entries).
    {
        std::map<std::tuple<std::string, unsigned, unsigned, int, int, int>, int> adj;
        for (auto& [k, n] : wall_join) {
            auto [a, b, dx, dy] = k;
            if (a <= b) adj[{"wall_join", a, b, dx, dy, 0}] += n;
        }
        for (auto& [p, n] : shape_pairs) adj[{"floor_next", p.first, p.second, 128, 0, 0}] += n;
        // Items resting on top of other items (furniture on floors excluded).
        for (auto& [m, list] : items) {
            std::unordered_map<uint64_t, std::vector<int>> cells;
            for (size_t i = 0; i < list.size(); ++i) {
                Role r = role[list[i].shape];
                if (r == Role::kFloor || r == Role::kEgg || r == Role::kRoof || r == Role::kWater) continue;
                cells[key3(list[i].x / 128, list[i].y / 128, 0)].push_back(int(i));
            }
            for (auto& [c, v] : cells)
                for (int i : v)
                    for (int j : v) {
                        const MinedItem& a = list[size_t(i)];
                        const MinedItem& b = list[size_t(j)];
                        if (i == j || a.zd == 0 || b.z != a.z + a.zd) continue;
                        bool ov = a.x0() <= b.x && b.x0() <= a.x && a.y0() <= b.y && b.y0() <= a.y;
                        if (ov) adj[{"on_top", a.shape, b.shape, 0, 0, a.zd}]++;
                    }
        }
        std::vector<AdjStat> v;
        for (auto& [k, n] : adj) {
            auto& [kind, a, b, dx, dy, dz] = k;
            if (n >= 10) v.push_back({kind, a, b, dx, dy, dz, n});
        }
        std::sort(v.begin(), v.end(), [](const AdjStat& a, const AdjStat& b) { return a.count > b.count; });
        std::map<std::string, int> per;
        for (const AdjStat& a : v)
            if (per[a.kind]++ < 150) kit.adjacency.push_back(a);
    }

    // --- Counts and report.
    std::map<Role, int> role_shapes, role_uses;
    for (auto& [s, e] : kit.shapes) {
        role_shapes[e.role]++;
        role_uses[e.role] += e.count;
    }
    kit.counts["maps"] = int(maps.size());
    kit.counts["items"] = int(total);
    kit.counts["fixed_items_with_globs"] = int(fixed_total);
    kit.counts["shapes"] = int(kit.shapes.size());
    kit.counts["floor_sets"] = int(kit.floors.size());
    kit.counts["wall_styles"] = int(kit.walls.size());
    kit.counts["themes"] = int(kit.themes.size());
    kit.counts["areas"] = int(areas.size());
    kit.counts["prefabs"] = int(kit.prefabs.size());
    kit.counts["prefab_candidates"] = prefab_candidates;
    kit.counts["floor_tiles_128"] = int(tiles[0].size());
    kit.counts["floor_tiles_256"] = int(tiles[1].size());
    for (auto& [r, n] : role_shapes) kit.counts[std::string("role_") + role_name(r)] = n;

    char buf[512];
    report.clear();
    std::snprintf(buf, sizeof buf, "Tile kit mined from %zu maps: %zu items (%zu fixed incl. globs), %zu shapes.\n",
                  maps.size(), total, fixed_total, kit.shapes.size());
    report += buf;
    report += "\nRoles (shapes / uses):\n";
    for (auto& [r, n] : role_shapes) {
        std::snprintf(buf, sizeof buf, "  %-11s %4d shapes %7d uses\n", role_name(r), n, role_uses[r]);
        report += buf;
    }
    std::snprintf(buf, sizeof buf, "\nFloor tiles on the grid: %zu (128) + %zu (256); %zu floor sets.\n", tiles[0].size(),
                  tiles[1].size(), kit.floors.size());
    report += buf;
    for (size_t i = 0; i < kit.floors.size() && i < 400; ++i) {
        const FloorSet& f = kit.floors[i];
        if (f.count < 200) continue;
        std::snprintf(buf, sizeof buf, "  %-9s %-6s fp %3d  %6d tiles  %2zu fill tiles  roofed %.2f  z %d  %s\n", f.id.c_str(),
                      f.kind.c_str(), f.footprint, f.count, f.tiles.size(), f.roof_fraction, f.z, f.label.c_str());
        report += buf;
    }
    std::snprintf(buf, sizeof buf, "\nWall styles: %zu\n", kit.walls.size());
    report += buf;
    for (const WallStyle& w : kit.walls) {
        std::string th;
        for (auto& [n, s] : w.themes) th += n + " " + std::to_string(int(s * 100)) + "% ";
        std::snprintf(buf, sizeof buf, "  %-7s h %2d %5d pieces  ew %zu/%zu/%zu/%zu ns %zu/%zu/%zu/%zu posts %zu door %u:%u/%u:%u %s\n",
                      w.id.c_str(), w.height, w.count, w.ew[1].size(), w.ew[2].size(), w.ew[3].size(), w.ew[4].size(),
                      w.ns[1].size(), w.ns[2].size(), w.ns[3].size(), w.ns[4].size(), w.post.size(), w.door_ew.shape,
                      w.door_ew.frame, w.door_ns.shape, w.door_ns.frame, th.c_str());
        report += buf;
    }
    std::snprintf(buf, sizeof buf, "\nThemes: %zu (from %zu areas of 1024 x 1024)\n", kit.themes.size(), areas.size());
    report += buf;
    for (auto& [n, t] : kit.themes) {
        std::string am;
        for (auto& [m, c] : t.area_maps) am += std::to_string(m) + ":" + std::to_string(c) + " ";
        std::snprintf(buf, sizeof buf, "  %-10s %3d areas  floor %-8s inside %-8s water %-8s lava %-8s roof %-8s wall %-7s ledge %-7s maps %s\n",
                      n.c_str(), t.areas, t.floor.c_str(), t.interior_floor.c_str(), t.water.c_str(), t.lava.c_str(),
                      t.roof.c_str(), t.wall.c_str(), t.ledge.c_str(), am.c_str());
        report += buf;
    }
    std::snprintf(buf, sizeof buf, "\nPrefabs: %zu kept of %d groups\n", kit.prefabs.size(), prefab_candidates);
    report += buf;
    for (auto& [l, n] : per_label) {
        std::snprintf(buf, sizeof buf, "  %-10s %d\n", l.c_str(), n);
        report += buf;
    }
    std::snprintf(buf, sizeof buf, "\nAdjacency entries kept: %zu\n", kit.adjacency.size());
    report += buf;
    if (opt.verbose) std::fputs(report.c_str(), stdout);
    return true;
}

}  // namespace pg
