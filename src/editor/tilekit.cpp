// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/tilekit.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include "editor/json_util.h"

namespace pg {

namespace {
const char* const kRoleNames[] = {
    "none", "floor", "water", "lava", "roof", "wall_ew", "wall_ns", "post", "door", "trim", "stairs",
    "light", "plant", "tree", "decoration", "clutter", "furniture", "container", "egg", "npc", "effect",
};

using J = OJson;

J piece_j(const KitPiece& p) {
    double w = std::round(p.weight * 100) / 100;
    return J::array({p.shape, p.frame, w});
}
J pieces_j(const std::vector<KitPiece>& v) {
    J a = J::array();
    for (const KitPiece& p : v) a.push_back(piece_j(p));
    return a;
}
KitPiece piece_from(const J& j) {
    KitPiece p;
    if (j.is_array() && j.size() >= 2) {
        p.shape = j[0].get<uint16_t>();
        p.frame = j[1].get<uint16_t>();
        p.weight = j.size() > 2 ? j[2].get<double>() : 1.0;
    } else if (j.is_object()) {
        p.shape = j.value("shape", 0);
        p.frame = j.value("frame", 0);
        p.weight = j.value("weight", 1.0);
    }
    return p;
}
std::vector<KitPiece> pieces_from(const J& j) {
    std::vector<KitPiece> v;
    if (j.is_array())
        for (const J& e : j) v.push_back(piece_from(e));
    return v;
}
J shares_j(const std::vector<std::pair<std::string, double>>& v) {
    J o = J::object();
    for (auto& [k, w] : v) o[k] = std::round(w * 1000) / 1000;
    return o;
}
std::vector<std::pair<std::string, double>> shares_from(const J& j) {
    std::vector<std::pair<std::string, double>> v;
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it) v.push_back({it.key(), it.value().get<double>()});
    return v;
}
const char* const kSides[4] = {"n", "e", "s", "w"};
}  // namespace

const char* role_name(Role r) {
    unsigned i = unsigned(r);
    return i < sizeof(kRoleNames) / sizeof(kRoleNames[0]) ? kRoleNames[i] : "none";
}
Role role_from_name(const std::string& s) {
    for (unsigned i = 0; i < sizeof(kRoleNames) / sizeof(kRoleNames[0]); ++i)
        if (s == kRoleNames[i]) return Role(i);
    return Role::kNone;
}

const FloorSet* TileKit::floor(const std::string& id) const {
    for (const FloorSet& f : floors)
        if (f.id == id) return &f;
    return nullptr;
}
const WallStyle* TileKit::wall(const std::string& id) const {
    for (const WallStyle& w : walls)
        if (w.id == id) return &w;
    return nullptr;
}
const Prefab* TileKit::prefab(const std::string& id) const {
    for (const Prefab& p : prefabs)
        if (p.id == id) return &p;
    return nullptr;
}
const ThemeKit* TileKit::theme(const std::string& name) const {
    auto it = themes.find(name);
    if (it != themes.end()) return &it->second;
    // Unknown theme: a theme whose name contains it (or the other way), else cave, else the first.
    for (auto& [n, t] : themes)
        if (!name.empty() && (n.find(name) != std::string::npos || name.find(n) != std::string::npos)) return &t;
    it = themes.find("cave");
    if (it != themes.end()) return &it->second;
    return themes.empty() ? nullptr : &themes.begin()->second;
}

std::string tilekit_to_json(const TileKit& kit) {
    J root = J::object();
    root["format"] = "pagan-tilekit";
    root["version"] = kit.version;
    root["doc"] = J::array({
        "Tile kit for the map dresser (engine/src/editor/dresser.*, docs/area_format.md).",
        "Mined from the original maps by pagan_tilekit; safe to edit by hand (the dresser only reads it).",
        "A piece is [shape, frame, weight]; weight = how often the original maps use it (relative).",
        "shapes: per shape role, footprint fp [x, y, z] in world units, typeflags tflags (SOLID 2, LAND 8, ROOF 0x400),",
        "  uses, most common z, frames used and the themes that use it.",
        "floors: floor materials. tiles = fill candidates (accents = tiles whose colour stands out, not used for fills);",
        "  pairs [a, b, h, v] = tile b east (+x) of a h times,",
        "  south (+y) of a v times; edge[mask] = tiles seen with that same-material neighbour mask (bit N1 E2 S4 W8).",
        "walls: wall styles. ew/ns pieces by length in 32-unit steps; post = corner/T/end pieces; jamb = posts",
        "  next to doors; door_ew/door_ns = the door used in that style's walls (the original door classes).",
        "themes: what each kind of place is made of; floor/interior_floor/water/lava/roof are floor ids,",
        "  shore[mask] = tiles laid in water cells next to land (mask bit N1 E2 S4 W8 = that neighbour is water),",
        "  wall/ledge are wall ids; scatter = share of open floor cells given a decoration.",
        "prefabs: point-of-interest object groups lifted from the original maps; items [shape, frame, dx, dy, dz,",
        "  flags, quality, npcnum, mapnum] relative to the group's box (min x, min y, floor z); size [x, y, z].",
        "adjacency: the most common piece-to-piece offsets (kind, a, b, dx, dy, dz, count)."});
    root["cell"] = kit.cell;
    root["source"] = kit.source;
    J counts = J::object();
    for (auto& [k, v] : kit.counts) counts[k] = v;
    root["counts"] = counts;

    J themes = J::object();
    for (auto& [name, t] : kit.themes) {
        J o = J::object();
        o["label"] = t.label;
        o["maps"] = t.maps;
        J am = J::object();
        for (auto& [m, n] : t.area_maps) am[std::to_string(m)] = n;
        o["area_maps"] = am;
        o["areas"] = t.areas;
        o["floor"] = t.floor;
        o["interior_floor"] = t.interior_floor;
        o["floors_alt"] = t.floors_alt;
        o["water"] = t.water;
        o["lava"] = t.lava;
        o["roof"] = t.roof;
        o["wall"] = t.wall;
        o["ledge"] = t.ledge;
        o["scatter"] = std::round(t.scatter * 1000) / 1000;
        o["lights"] = pieces_j(t.lights);
        o["plants"] = pieces_j(t.plants);
        o["trees"] = pieces_j(t.trees);
        o["decor"] = pieces_j(t.decor);
        o["clutter"] = pieces_j(t.clutter);
        o["furniture"] = pieces_j(t.furniture);
        J tv = J::object();
        for (int s = 0; s < 4; ++s) tv[kSides[s]] = pieces_j(t.trim_void[s]);
        o["trim_void"] = tv;
        J sh = J::object();
        for (int m = 0; m < 16; ++m)
            if (!t.shore[size_t(m)].empty()) sh[std::to_string(m)] = pieces_j(t.shore[size_t(m)]);
        o["shore"] = sh;
        o["fallback"] = t.fallback;
        themes[name] = o;
    }
    root["themes"] = themes;

    J floors = J::array();
    for (const FloorSet& f : kit.floors) {
        J o = J::object();
        o["id"] = f.id;
        o["kind"] = f.kind;
        o["label"] = f.label;
        o["count"] = f.count;
        o["z"] = f.z;
        o["footprint"] = f.footprint;
        o["roof_fraction"] = std::round(f.roof_fraction * 1000) / 1000;
        o["tiles"] = pieces_j(f.tiles);
        o["accents"] = pieces_j(f.accents);
        J pairs = J::array();
        for (auto& p : f.pairs) pairs.push_back(J::array({p[0], p[1], p[2], p[3]}));
        o["pairs"] = pairs;
        J edge = J::object();
        for (int m = 0; m < 16; ++m)
            if (!f.edge[m].empty()) edge[std::to_string(m)] = pieces_j(f.edge[m]);
        o["edge"] = edge;
        floors.push_back(o);
    }
    root["floors"] = floors;

    J walls = J::array();
    for (const WallStyle& w : kit.walls) {
        J o = J::object();
        o["id"] = w.id;
        o["label"] = w.label;
        o["height"] = w.height;
        o["count"] = w.count;
        o["land_top"] = w.land_top;
        J ew = J::object(), ns = J::object();
        for (int l = 1; l <= 4; ++l) {
            if (!w.ew[l].empty()) ew[std::to_string(l)] = pieces_j(w.ew[l]);
            if (!w.ns[l].empty()) ns[std::to_string(l)] = pieces_j(w.ns[l]);
        }
        o["ew"] = ew;
        o["ns"] = ns;
        o["post"] = pieces_j(w.post);
        o["jamb"] = pieces_j(w.jamb);
        o["door_ew"] = piece_j(w.door_ew);
        o["door_ns"] = piece_j(w.door_ns);
        o["door_quality"] = w.door_quality;
        o["themes"] = shares_j(w.themes);
        walls.push_back(o);
    }
    root["walls"] = walls;

    J prefabs = J::array();
    for (const Prefab& p : kit.prefabs) {
        J o = J::object();
        o["id"] = p.id;
        o["label"] = p.label;
        o["keywords"] = p.keywords;
        o["theme"] = p.theme;
        o["map"] = p.map;
        o["at"] = J::array({p.x, p.y, p.z});
        o["size"] = J::array({p.sx, p.sy, p.sz});
        J items = J::array();
        for (const PrefabItem& i : p.items)
            items.push_back(J::array({i.shape, i.frame, i.dx, i.dy, i.dz, i.flags, i.quality, i.npcnum, i.mapnum}));
        o["items"] = items;
        prefabs.push_back(o);
    }
    root["prefabs"] = prefabs;

    J shapes = J::object();
    for (auto& [s, e] : kit.shapes) {
        J o = J::object();
        o["role"] = role_name(e.role);
        if (!e.name.empty()) o["name"] = e.name;
        o["fp"] = J::array({e.fp[0], e.fp[1], e.fp[2]});
        o["tflags"] = e.tflags;
        o["family"] = e.family;
        o["uses"] = e.count;
        o["nonfixed"] = e.nonfixed;
        o["z"] = e.z;
        o["frames"] = pieces_j(e.frames);
        o["themes"] = shares_j(e.themes);
        shapes[std::to_string(s)] = o;
    }
    root["shapes"] = shapes;

    J adj = J::array();
    for (const AdjStat& a : kit.adjacency) adj.push_back(J::array({a.kind, a.a, a.b, a.dx, a.dy, a.dz, a.count}));
    root["adjacency"] = adj;
    return json_pretty(root) + "\n";
}

bool parse_tilekit(const std::string& text, TileKit& kit, std::string& err) {
    J root;
    try {
        root = J::parse(text);
    } catch (const std::exception& e) {
        err = std::string("tilekit: bad JSON: ") + e.what();
        return false;
    }
    try {
        if (root.value("format", "") != "pagan-tilekit") {
            err = "tilekit: not a pagan-tilekit file";
            return false;
        }
        kit = TileKit{};
        kit.version = root.value("version", 1);
        kit.cell = root.value("cell", 128);
        kit.source = root.value("source", "");
        if (root.contains("counts"))
            for (auto it = root["counts"].begin(); it != root["counts"].end(); ++it) kit.counts[it.key()] = it.value().get<int>();
        if (root.contains("themes"))
            for (auto it = root["themes"].begin(); it != root["themes"].end(); ++it) {
                const J& o = it.value();
                ThemeKit t;
                t.name = it.key();
                t.label = o.value("label", "");
                if (o.contains("maps")) t.maps = o["maps"].get<std::vector<unsigned>>();
                if (o.contains("area_maps"))
                    for (auto a = o["area_maps"].begin(); a != o["area_maps"].end(); ++a)
                        t.area_maps.push_back({unsigned(std::stoi(a.key())), a.value().get<int>()});
                t.areas = o.value("areas", 0);
                t.floor = o.value("floor", "");
                t.interior_floor = o.value("interior_floor", "");
                if (o.contains("floors_alt")) t.floors_alt = o["floors_alt"].get<std::vector<std::string>>();
                t.water = o.value("water", "");
                t.lava = o.value("lava", "");
                t.roof = o.value("roof", "");
                t.wall = o.value("wall", "");
                t.ledge = o.value("ledge", "");
                t.scatter = o.value("scatter", 0.05);
                t.lights = pieces_from(o.value("lights", J::array()));
                t.plants = pieces_from(o.value("plants", J::array()));
                t.trees = pieces_from(o.value("trees", J::array()));
                t.decor = pieces_from(o.value("decor", J::array()));
                t.clutter = pieces_from(o.value("clutter", J::array()));
                t.furniture = pieces_from(o.value("furniture", J::array()));
                if (o.contains("trim_void"))
                    for (int s = 0; s < 4; ++s) t.trim_void[s] = pieces_from(o["trim_void"].value(kSides[s], J::array()));
                if (o.contains("shore"))
                    for (auto it = o["shore"].begin(); it != o["shore"].end(); ++it) {
                        int m = std::stoi(it.key());
                        if (m >= 0 && m < 16) t.shore[size_t(m)] = pieces_from(it.value());
                    }
                if (o.contains("fallback")) t.fallback = o["fallback"].get<std::vector<std::string>>();
                kit.themes[t.name] = t;
            }
        if (root.contains("floors"))
            for (const J& o : root["floors"]) {
                FloorSet f;
                f.id = o.value("id", "");
                f.kind = o.value("kind", "floor");
                f.label = o.value("label", "");
                f.count = o.value("count", 0);
                f.z = o.value("z", 0);
                f.footprint = o.value("footprint", 128);
                f.roof_fraction = o.value("roof_fraction", 0.0);
                f.tiles = pieces_from(o.value("tiles", J::array()));
                f.accents = pieces_from(o.value("accents", J::array()));
                if (o.contains("pairs"))
                    for (const J& p : o["pairs"]) f.pairs.push_back({p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()});
                if (o.contains("edge"))
                    for (auto it = o["edge"].begin(); it != o["edge"].end(); ++it) {
                        int m = std::stoi(it.key());
                        if (m >= 0 && m < 16) f.edge[m] = pieces_from(it.value());
                    }
                kit.floors.push_back(f);
            }
        if (root.contains("walls"))
            for (const J& o : root["walls"]) {
                WallStyle w;
                w.id = o.value("id", "");
                w.label = o.value("label", "");
                w.height = o.value("height", 40);
                w.count = o.value("count", 0);
                w.land_top = o.value("land_top", false);
                for (int l = 1; l <= 4; ++l) {
                    if (o.contains("ew")) w.ew[l] = pieces_from(o["ew"].value(std::to_string(l), J::array()));
                    if (o.contains("ns")) w.ns[l] = pieces_from(o["ns"].value(std::to_string(l), J::array()));
                }
                w.post = pieces_from(o.value("post", J::array()));
                w.jamb = pieces_from(o.value("jamb", J::array()));
                if (o.contains("door_ew")) w.door_ew = piece_from(o["door_ew"]);
                if (o.contains("door_ns")) w.door_ns = piece_from(o["door_ns"]);
                w.door_quality = o.value("door_quality", 0);
                w.themes = shares_from(o.value("themes", J::object()));
                kit.walls.push_back(w);
            }
        if (root.contains("prefabs"))
            for (const J& o : root["prefabs"]) {
                Prefab p;
                p.id = o.value("id", "");
                p.label = o.value("label", "");
                if (o.contains("keywords")) p.keywords = o["keywords"].get<std::vector<std::string>>();
                p.theme = o.value("theme", "");
                p.map = o.value("map", 0);
                if (o.contains("at") && o["at"].size() >= 3) {
                    p.x = o["at"][0];
                    p.y = o["at"][1];
                    p.z = o["at"][2];
                }
                if (o.contains("size") && o["size"].size() >= 3) {
                    p.sx = o["size"][0];
                    p.sy = o["size"][1];
                    p.sz = o["size"][2];
                }
                if (o.contains("items"))
                    for (const J& i : o["items"]) {
                        if (!i.is_array() || i.size() < 5) continue;
                        PrefabItem it;
                        it.shape = i[0];
                        it.frame = i[1];
                        it.dx = i[2];
                        it.dy = i[3];
                        it.dz = i[4];
                        if (i.size() > 5) it.flags = i[5];
                        if (i.size() > 6) it.quality = i[6];
                        if (i.size() > 7) it.npcnum = i[7];
                        if (i.size() > 8) it.mapnum = i[8];
                        p.items.push_back(it);
                    }
                kit.prefabs.push_back(p);
            }
        if (root.contains("shapes"))
            for (auto it = root["shapes"].begin(); it != root["shapes"].end(); ++it) {
                const J& o = it.value();
                ShapeEntry e;
                e.shape = unsigned(std::stoi(it.key()));
                e.role = role_from_name(o.value("role", "none"));
                e.name = o.value("name", "");
                if (o.contains("fp") && o["fp"].size() >= 3)
                    for (int k = 0; k < 3; ++k) e.fp[k] = o["fp"][k];
                e.tflags = o.value("tflags", 0);
                e.family = o.value("family", 0);
                e.count = o.value("uses", 0);
                e.nonfixed = o.value("nonfixed", 0);
                e.z = o.value("z", 0);
                e.frames = pieces_from(o.value("frames", J::array()));
                e.themes = shares_from(o.value("themes", J::object()));
                kit.shapes[e.shape] = e;
            }
        if (root.contains("adjacency"))
            for (const J& a : root["adjacency"]) {
                if (!a.is_array() || a.size() < 7) continue;
                AdjStat s;
                s.kind = a[0];
                s.a = a[1];
                s.b = a[2];
                s.dx = a[3];
                s.dy = a[4];
                s.dz = a[5];
                s.count = a[6];
                kit.adjacency.push_back(s);
            }
    } catch (const std::exception& e) {
        err = std::string("tilekit: bad field: ") + e.what();
        return false;
    }
    return true;
}

bool load_tilekit(const std::string& path, TileKit& kit, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "tilekit: cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_tilekit(ss.str(), kit, err);
}

}  // namespace pg
