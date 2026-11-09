// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/world_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <functional>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

#include "data/gamedata.h"
#include "data/mapfile.h"
#include "json/json.hpp"
#include "world/world_space.h"

namespace pg {

using nlohmann::json;

namespace {

// Preferred layer and kind of every placed map (specs/world/map_index.md section 1).
struct MapKind { unsigned map; int layer; const char* kind; const char* name; };
const MapKind kKinds[] = {
    {3, 0, "surface", "Tenebrae docks and shore"}, {4, 0, "surface", "Fisherman's Reef"},
    {5, 0, "surface", "East Road"}, {6, 0, "surface", "Cemetery"}, {7, 0, "surface", "Herdsman's Valley"},
    {8, 0, "surface", "Plateau"}, {12, 1, "high place", "Daemon's Crag"}, {14, 1, "high place", "Obsidian Fortress"},
    {15, -1, "cave", "Treasure Cove"}, {16, 0, "surface", "Carthax Lake"}, {21, 0, "surface", "Stone Cove"},
    {24, -1, "tunnel", "Lava River tunnel"}, {25, -1, "tunnel", "Lava River tunnel"}, {26, 0, "surface", "Carthax Lake"},
    {28, -1, "interior", "Carthax Lake breaking ground"}, {31, -1, "cave", "Hall of the Mountain King"},
    {37, 0, "surface", "Argentrock Isle"}, {39, 0, "surface", "West Tenebrae"}, {40, 0, "surface", "Central Tenebrae"},
    {41, 0, "surface", "East Tenebrae"}, {43, 2, "plane", "Ethereal Plane"}, {44, 2, "plane", "Plane of Fire"},
    {45, 2, "plane", "Plane of Water"}, {46, 2, "plane", "Plane of Air"}, {47, 2, "plane", "Plane of Earth"},
    {49, -1, "catacombs", "Catacombs level"}, {50, -1, "catacombs", "Upper Catacombs"},
    {51, -2, "catacombs", "Lower Catacombs"}, {54, -2, "shrine", "Shrine of the Ancient Ones"},
    {57, -2, "catacombs", "Pit of the Dead"}, {62, -2, "catacombs", "Ancient Necromancers"},
    {63, -1, "interior", "Ghost armour area"},
};
constexpr int kAnyRecord = 1;

const MapKind* kind_of(unsigned m) {
    for (const MapKind& k : kKinds)
        if (k.map == m) return &k;
    return nullptr;
}

long long ckey(int layer, int x, int y) { return ((long long)(layer + 64) << 40) ^ ((long long)(x + 100000) << 20) ^ (y + 100000); }
int floor_div(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

// Occupied world chunks (512 grid, conservative for offsets that are not whole chunks).
struct Occupancy {
    std::unordered_map<long long, std::vector<int>> owner;  // cell -> the groups on it
    template <class F> static void cells(const MapFootprint& f, int dx, int dy, F fn) {
        for (int cy = f.cy0; cy <= f.cy1; ++cy)
            for (int cx = f.cx0; cx <= f.cx1; ++cx) {
                // Occupied = any record at all (fixed, nonfixed or an NPC), even one ground glob:
                // nothing of one group may lie on a chunk of another group of its layer.
                if (f.count[size_t(cy) * 64 + cx] < kAnyRecord) continue;
                int x0 = floor_div(cx * 512 + dx, 512), x1 = floor_div(cx * 512 + dx + 511, 512);
                int y0 = floor_div(cy * 512 + dy, 512), y1 = floor_div(cy * 512 + dy + 511, 512);
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) fn(x, y);
            }
    }
    // Groups whose chunks f would share on `layer` (excluding `allowed`), with the count of shared chunks.
    std::map<int, int> hits(const MapFootprint& f, int dx, int dy, int layer, const std::set<int>& allowed) const {
        std::map<int, int> out;
        std::set<long long> seen;
        cells(f, dx, dy, [&](int x, int y) {
            long long k = ckey(layer, x, y);
            if (!seen.insert(k).second) return;
            auto it = owner.find(k);
            if (it == owner.end()) return;
            for (int g : it->second)
                if (!allowed.count(g)) ++out[g];
        });
        return out;
    }
    void add(const MapFootprint& f, int dx, int dy, int layer, int g) {
        cells(f, dx, dy, [&](int x, int y) {
            std::vector<int>& v = owner[ckey(layer, x, y)];
            if (v.empty() || v.back() != g) v.push_back(g);
        });
    }
};

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace

const WorldSeam* WorldLayoutFile::seam(unsigned m1, unsigned m2) const {
    unsigned a = std::min(m1, m2), b = std::max(m1, m2);
    for (const WorldSeam& s : seams)
        if (s.a == a && s.b == b) return &s;
    return nullptr;
}

bool WorldLayoutFile::from_json(const std::string& text, std::string& err) {
    WorldLayoutFile w;
    if (!w.atlas.from_json(text, err)) return false;
    json j = json::parse(text, nullptr, false, true);
    if (j.contains("seams") && j["seams"].is_array())
        for (const json& s : j["seams"]) {
            if (!s.is_object() || !s.contains("maps") || !s["maps"].is_array() || s["maps"].size() != 2) {
                err = "a seam needs \"maps\": [a, b]";
                return false;
            }
            // A seam the editor broke (a segment moved off it): its crossings are plain teleports.
            if (s.value("mode", std::string("walk")) == "teleport") continue;
            WorldSeam m;
            m.a = s["maps"][0].get<unsigned>();
            m.b = s["maps"][1].get<unsigned>();
            if (s.contains("offset") && s["offset"].is_array() && s["offset"].size() >= 2) {
                m.dx = s["offset"][0].get<int>();
                m.dy = s["offset"][1].get<int>();
                if (s["offset"].size() >= 3) m.dz = s["offset"][2].get<int>();
            }
            if (m.a > m.b) { std::swap(m.a, m.b); m.dx = -m.dx; m.dy = -m.dy; m.dz = -m.dz; }
            m.votes = s.value("votes", 0);
            m.basis = s.value("basis", std::string("votes"));
            if (s.contains("strip") && s["strip"].is_array() && s["strip"].size() == 4)
                for (int k = 0; k < 4; ++k) m.strip[k] = s["strip"][size_t(k)].get<int>();
            if (s.contains("portals") && s["portals"].is_array())
                for (const json& p : s["portals"]) {
                    WorldPortal q;
                    q.map = p.value("map", 0u);
                    q.to_map = p.value("to_map", 0u);
                    q.id = p.value("id", 0u);
                    if (p.contains("at") && p["at"].is_array() && p["at"].size() == 3) {
                        q.x = p["at"][0].get<int>(); q.y = p["at"][1].get<int>(); q.z = p["at"][2].get<int>();
                    }
                    m.portals.push_back(q);
                }
            m.derived = s.value("derived", "");
            w.seams.push_back(m);
        }
    if (j.contains("links") && j["links"].is_array())
        for (const json& s : j["links"]) {
            if (!s.is_object()) continue;
            WorldLink l;
            l.from_map = s.value("from_map", 0u);
            l.to_map = s.value("to_map", 0u);
            l.kind = s.value("kind", "egg");
            l.id = s.value("id", -1);
            l.mode = s.value("mode", "teleport");
            if (s.contains("from") && s["from"].is_array() && s["from"].size() == 3) {
                l.has_from = true;
                for (int k = 0; k < 3; ++k) l.from[k] = s["from"][size_t(k)].get<int>();
            }
            if (s.contains("to") && s["to"].is_array() && s["to"].size() == 3)
                for (int k = 0; k < 3; ++k) l.to[k] = s["to"][size_t(k)].get<int>();
            if (s.contains("condition")) l.condition = s["condition"].dump();
            l.derived = s.value("derived", "");
            w.links.push_back(l);
        }
    if (j.contains("variants") && j["variants"].is_array())
        for (const json& s : j["variants"]) {
            if (!s.is_object()) continue;
            WorldVariant v;
            v.group = s.value("group", "");
            if (s.contains("global") && s["global"].is_array() && s["global"].size() == 2) {
                v.global_pos = s["global"][0].get<unsigned>();
                v.global_width = s["global"][1].get<unsigned>();
            }
            v.when_zero = s.value("when_zero", 0u);
            v.when_nonzero = s.value("when_nonzero", 0u);
            v.derived = s.value("derived", "");
            w.variants.push_back(v);
        }
    w.layout_revision = j.value("layout_revision", j.contains("seams") ? 1 : 0);
    if (j.contains("layers") && j["layers"].is_object())
        for (auto& [k, v] : j["layers"].items())
            if (v.is_string()) w.layer_names[std::atoi(k.c_str())] = v.get<std::string>();
    *this = std::move(w);
    return true;
}

bool WorldLayoutFile::load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str(), err);
}

std::string WorldLayoutFile::extra_json() const {
    // Written by hand (one entry per line) so the file stays readable and diffs well.
    std::ostringstream o;
    o << "{\"layout_revision\": " << layout_revision << ",\n  \"seams\": [";
    for (size_t i = 0; i < seams.size(); ++i) {
        const WorldSeam& s = seams[i];
        o << (i ? ",\n    " : "\n    ") << "{\"maps\": [" << s.a << ", " << s.b << "], \"offset\": [" << s.dx << ", " << s.dy
          << (s.dz ? ", " + std::to_string(s.dz) : std::string()) << "], \"votes\": " << s.votes << ", \"basis\": " << json(s.basis).dump() << ", \"strip\": [" << s.strip[0] << ", " << s.strip[1] << ", " << s.strip[2] << ", "
          << s.strip[3] << "], \"mode\": \"walk\", \"portals\": [";
        for (size_t k = 0; k < s.portals.size(); ++k) {
            const WorldPortal& p = s.portals[k];
            o << (k ? ", " : "") << "{\"map\": " << p.map << ", \"to_map\": " << p.to_map << ", \"id\": " << p.id << ", \"at\": ["
              << p.x << ", " << p.y << ", " << p.z << "]}";
        }
        o << "],\n     \"derived\": " << json(s.derived).dump() << "}";
    }
    o << "\n  ],\n  \"links\": [";
    for (size_t i = 0; i < links.size(); ++i) {
        const WorldLink& l = links[i];
        o << (i ? ",\n    " : "\n    ") << "{\"from_map\": " << l.from_map << ", \"to_map\": " << l.to_map << ", \"kind\": "
          << json(l.kind).dump();
        if (l.id >= 0) o << ", \"id\": " << l.id;
        if (l.has_from) o << ", \"from\": [" << l.from[0] << ", " << l.from[1] << ", " << l.from[2] << "]";
        o << ", \"to\": [" << l.to[0] << ", " << l.to[1] << ", " << l.to[2] << "], \"mode\": " << json(l.mode).dump();
        if (!l.condition.empty()) o << ", \"condition\": " << l.condition;
        o << ",\n     \"derived\": " << json(l.derived).dump() << "}";
    }
    o << "\n  ],\n  \"variants\": [";
    for (size_t i = 0; i < variants.size(); ++i) {
        const WorldVariant& v = variants[i];
        o << (i ? ",\n    " : "\n    ") << "{\"group\": " << json(v.group).dump() << ", \"global\": [" << v.global_pos << ", "
          << v.global_width << "], \"when_zero\": " << v.when_zero << ", \"when_nonzero\": " << v.when_nonzero
          << ", \"derived\": " << json(v.derived).dump() << "}";
    }
    o << "\n  ],\n  \"layers\": {";
    bool first = true;
    for (auto& [k, v] : layer_names) {
        o << (first ? "" : ", ") << "\"" << k << "\": " << json(v).dump();
        first = false;
    }
    o << "}}";
    return o.str();
}

std::string WorldLayoutFile::to_json() const {
    std::string base = atlas.to_json();
    while (!base.empty() && (base.back() == '\n' || base.back() == ' ')) base.pop_back();
    if (!base.empty() && base.back() == '}') base.pop_back();
    while (!base.empty() && (base.back() == '\n' || base.back() == ' ')) base.pop_back();
    std::string x = extra_json();
    // extra_json is "{...}": splice its members in.
    return base + ",\n  " + x.substr(1, x.size() - 2) + "\n}\n";
}

WorldLayoutFile generate_world_layout(const GameData& d, const std::string& links_text) {
    std::vector<MapFootprint> fps = compute_footprints(d);
    std::map<unsigned, const MapFootprint*> fp;
    for (const MapFootprint& f : fps) fp[f.map] = &f;
    std::vector<TeleportLink> eggs = teleport_links(d);

    // --- Groups: 49 / 50 share coordinates (their links sit at identical points); the two-state
    // places are one group each.
    std::vector<std::vector<unsigned>> groups;
    std::map<unsigned, int> gof;
    auto add_group = [&](std::vector<unsigned> maps) {
        std::vector<unsigned> have;
        for (unsigned m : maps)
            if (fp.count(m) && !gof.count(m)) have.push_back(m);
        if (have.empty()) return;
        for (unsigned m : have) gof[m] = int(groups.size());
        groups.push_back(have);
    };
    add_group({49, 50});
    add_group({16, 26});
    add_group({24, 25});
    for (const MapFootprint& f : fps) add_group({f.map});
    std::vector<MapFootprint> gfp(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        MapFootprint& u = gfp[g];
        u.map = groups[g][0];
        for (unsigned m : groups[g]) {
            const MapFootprint& f = *fp[m];
            u.occ |= f.occ;
            u.cx0 = std::min(u.cx0, f.cx0); u.cy0 = std::min(u.cy0, f.cy0);
            u.cx1 = std::max(u.cx1, f.cx1); u.cy1 = std::max(u.cy1, f.cy1);
            for (size_t c = 0; c < u.count.size(); ++c) u.count[c] = uint16_t(std::min(65535, u.count[c] + f.count[c]));
        }
        u.chunks = int(u.occ.count());
    }

    // --- Relations between maps of different groups. Offset (ox, oy) of b in a's coordinates.
    struct Rel { double sx = 0, sy = 0; int n = 0; std::string why; int prio = 2; };
    std::map<std::pair<unsigned, unsigned>, Rel> rel;
    for (const TeleportLink& l : eggs) {
        if (!fp.count(l.from_map) || !fp.count(l.to_map) || gof[l.from_map] == gof[l.to_map]) continue;
        unsigned a = std::min(l.from_map, l.to_map), b = std::max(l.from_map, l.to_map);
        int ox = l.fx - l.tx, oy = l.fy - l.ty;
        if (l.from_map != a) { ox = -ox; oy = -oy; }
        Rel& r = rel[{a, b}];
        r.sx += ox; r.sy += oy; ++r.n; r.prio = 1;
        r.why += fmt("%steleporter id %u: map %u at %d,%d,%d -> map %u at %d,%d,%d", r.why.empty() ? "" : "; ", l.id,
                     l.from_map, l.fx, l.fy, l.fz, l.to_map, l.tx, l.ty, l.tz);
    }
    // Usecode / MAPTELE teleports with a fixed source point (critical path's map_links.json).
    json links = json::parse(links_text.empty() ? std::string("{}") : links_text, nullptr, false, true);
    std::vector<WorldLink> out_links;
    if (links.is_object() && links.contains("links") && links["links"].is_array())
        for (const json& l : links["links"]) {
            if (!l.is_object()) continue;
            int fm = l.value("from_map", -1), tm = l.value("to_map", -1);
            if (tm < 0 || fm == tm) continue;
            WorldLink w;
            w.from_map = unsigned(std::max(fm, 0));
            w.to_map = unsigned(tm);
            w.kind = l.value("kind", "egg");
            w.id = l.contains("egg_id") && l["egg_id"].is_number_integer() ? l["egg_id"].get<int>() : -1;
            w.has_from = fm >= 0 && l.contains("x") && l["x"].is_number_integer();
            if (w.has_from) { w.from[0] = l["x"].get<int>(); w.from[1] = l["y"].get<int>(); w.from[2] = l["z"].get<int>(); }
            if (l.contains("to_x") && l["to_x"].is_number_integer()) {
                w.to[0] = l["to_x"].get<int>(); w.to[1] = l["to_y"].get<int>(); w.to[2] = l["to_z"].get<int>();
            }
            if (l.contains("condition")) w.condition = l["condition"].dump();
            w.derived = l.value("note", "");
            if (fm < 0) w.derived = "from any map: " + w.derived;
            out_links.push_back(w);
            if (w.kind == "egg" || !w.has_from || !fp.count(w.from_map) || !fp.count(w.to_map)) continue;
            if (gof[w.from_map] == gof[w.to_map]) continue;
            unsigned a = std::min(w.from_map, w.to_map), b = std::max(w.from_map, w.to_map);
            Rel& r = rel[{a, b}];
            if (r.prio == 1) continue;  // the teleporter eggs already place this pair
            int ox = w.from[0] - w.to[0], oy = w.from[1] - w.to[1];
            if (w.from_map != a) { ox = -ox; oy = -oy; }
            r.sx += ox; r.sy += oy; ++r.n; r.prio = 2;
            r.why += fmt("%s%s teleport: map %u at %d,%d,%d -> map %u at %d,%d,%d", r.why.empty() ? "" : "; ", w.kind.c_str(),
                         w.from_map, w.from[0], w.from[1], w.from[2], w.to_map, w.to[0], w.to[1], w.to[2]);
        }

    // Group edges with seam votes.
    struct Edge { int to; int ox, oy; int votes; int prio; std::string why; unsigned ma, mb; };
    std::vector<std::vector<Edge>> edges(groups.size());
    std::vector<WorldSeam> seams;
    for (auto& [k, r] : rel) {
        int ga = gof[k.first], gb = gof[k.second];
        int ox = int(std::lround(r.sx / r.n)), oy = int(std::lround(r.sy / r.n));
        int votes = 0, vx = 0, vy = 0;
        if (r.prio == 1) votes = atlas_seam_votes(d, k.first, k.second, ox, oy, vx, vy);
        std::string why = r.why;
        if (votes >= 40) {
            why += fmt("; seam: %d identical border items line up at offset %d,%d", votes, vx, vy);
            ox = vx; oy = vy;
            WorldSeam s;
            s.a = k.first; s.b = k.second; s.dx = ox; s.dy = oy; s.votes = votes;
            s.derived = fmt("seam votes: %d identical fixed items (same shape, frame and z) of map %u line up with map %u "
                            "at offset %d,%d (teleport offset %.0f,%.0f); ",
                            votes, k.second, k.first, vx, vy, r.sx / r.n, r.sy / r.n) + r.why;
            seams.push_back(s);
        } else votes = 0;
        edges[size_t(ga)].push_back({gb, ox, oy, votes, r.prio, why, k.first, k.second});
        edges[size_t(gb)].push_back({ga, -ox, -oy, votes, r.prio, why, k.second, k.first});
    }

    // --- Placement (revision 2, engine/NOTES.md "The stitched world"): the curated plan below. The
    // surface follows the whole-island fan map (screen top-right = north); seams sit at their exact
    // offsets; caves, catacombs and interiors are under the place they are entered from (or under
    // their spot on the fan map); the planes are above; maps nobody can reach are parked in an
    // "Unplaced" row south of the island. The offsets were found offline: the target spot, then the
    // nearest 512-unit step where the group overlaps nothing on its layer (the validator's rule,
    // with a chunk of gap). Groups with no text here are vote seams: the text says which.
    struct Plan { unsigned map; int dx, dy, layer, trim; const char* why; const char* unplaced_note; };
    static const Plan kPlan[] = {
    {40, 0, 0, 0, 0, "root: original coordinates (Central Tenebrae)", nullptr},
    {39, -8192, -512, 0, 0, nullptr, nullptr},
    {41, 2048, 0, 0, 0, nullptr, nullptr},
    {3, 2048, 12800, 0, 0, nullptr, nullptr},
    {4, -14336, 3072, 0, 0, nullptr, nullptr},
    {5, 19456, -12288, 0, 0, nullptr, nullptr},
    {7, -2025, -24057, 0, 40, "seam with map 40 (walkable; layer 0): Herdsman's Valley is the northern part of Tenebrae, the fields beyond the north wall (the fan map draws it there). Exact offset of the gate teleporters both ways (map 40 egg id 0 at 13054,2322 -> map 7 at 15071,26239; map 7 egg id 3 at 15103,26815 -> map 40 at 13070,2618; averaged): map 7's own copy of the north wall lines up with the city's, so walking north out of the gate is walking on into the valley; its ground (z 8) is trimmed up 40 to the city's (z 48)", nullptr},
    {6, 21505, -31280, 0, 40, "seam with map 5 (walkable; layer 0): the Cemetery is the far end of the East Road, east of Tenebrae (the fan map draws them as one coast). Exact offset of the 5 <-> 6 teleporters both ways (map 5 egg id 1 at 7423,10943 -> map 6 at 5375,29823; map 6 egg id 1 at 5375,30367 -> map 5 at 7424,11264; averaged): the road's wall runs on into the Cemetery's, so walking up the road walks on into the Cemetery; its ground (z 8) is trimmed up 40 to the road's (z 48)", nullptr},
    {8, -41344, -1920, 0, 0, "Plateau: west of Fisherman's Reef and north-west of Tenebrae, as on the fan map (its main plateau centred on the fan map's plateau, world -19221,14045), then the nearest free spot (20 chunks off) so it overlaps nothing on its layer; the 7 <-> 8 mountain path stays a teleport (at its exact offset the plateau's path piece would lie over map 7 and West Tenebrae)", nullptr},
    {16, 25408, -82624, 0, 0, "fan map: Carthax Lake: the grey isle with the storm in the east of the fan map, between Daemon's Crag (north) and Argentrock (south); its content centred there (layer 0); linked by its teleports", nullptr},
    {21, 43136, -19328, 0, 0, "fan map: Stone Cove: the walled cove with the pool on the south-east coast of the fan map, south-east of the Cemetery; its content centred there (layer 0), then the nearest free spot (1 chunk off) so it overlaps nothing on its layer; linked by its teleports", nullptr},
    {37, 50112, -58432, 0, 0, "fan map: Argentrock Isle: the island with the library off the east coast of the fan map; its content centred there (layer 0), then the nearest free spot (4 chunks off) so it overlaps nothing on its layer; linked by its teleports", nullptr},
    {12, 5504, -104768, 0, 0, "fan map: Daemon's Crag: the lava fields and the town on the volcano in the north-east of the fan map; its content centred there (layer 0); linked by its teleports", nullptr},
    {49, 19008, -47104, -1, 0, "fan map: the Upper Catacombs: the big grey maze in the middle of the fan map, under the mountains north-east of Tenebrae (entered from the Cemetery); its content centred there (layer -1); linked by its teleports", nullptr},
    {63, -5097, -26521, -1, 0, "Ghost armour area (layer -1): under Herdsman's Valley, at the exact offset of the 7 <-> 63 teleporters (map 7 egg id 5 at 13055,16415 -> map 63 at 16127,18879)", nullptr},
    {15, 15017, -7019, -1, 0, "Treasure Cove (layer -1): under the East Road, at the exact offset of its way out (map 15 egg id 10 at 8310,6762 -> map 5 at 3871,12031); the East Road's hidden pads lead down into it", nullptr},
    {14, -6528, -88384, -1, 0, "Obsidian Fortress (layer -1): inside the volcano, under Daemon's Crag at the exact offset of the 12 <-> 14 teleporters (map 12 egg id 1 at 3903,18687 -> map 14 at 15935,2303)", nullptr},
    {24, 6464, -122208, -1, 0, "Lava River tunnel (layer -1): under Daemon's Crag at the exact offset of the MAPTELE egg there (map 12 at 20223,13663 -> map 24/25 at 19263,31103); its other end leads to the catacombs", nullptr},
    {28, 28192, -72608, -1, 0, "Carthax Lake breaking ground (layer -1): under Carthax Lake at the exact offset of the 16 -> 28 teleporter (map 16 egg id 1 at 17764,18124 -> map 28 at 14980,8108)", nullptr},
    {31, -13056, -62784, -1, 0, "fan map: the Hall of the Mountain King: the caves with pools in the north of the fan map; its content centred there (layer -1), then the nearest free spot (1 chunk off) so it overlaps nothing on its layer; linked by its teleports", nullptr},
    {54, 14482, -50626, -2, 0, "Shrine of the Ancient Ones (layer -2): under the Upper Catacombs at the exact offset of the 50 -> 54 teleporter (map 50 egg id 0 at 11168,11264 -> map 54 at 15694,14786)", nullptr},
    {62, 41792, -58976, -2, 0, "Ancient Necromancers (layer -2): under the Upper Catacombs at the exact offset of the 50 -> 62 teleporter (map 50 egg id 5 at 24479,2783 -> map 62 at 1695,14655)", nullptr},
    {51, -4794, -38390, -2, 0, "Lower Catacombs (layer -2): under the Upper Catacombs, from the offset of the 50 -> 51 teleporter (map 50 egg id 7 at 11074,8910 -> map 51 at 28732,4804), then the nearest free spot (12 chunks off) so it overlaps nothing on its layer, placed together with its seam partner 57", nullptr},
    {57, -2746, -41974, -2, 0, nullptr, nullptr},
    {43, -45056, -45056, 1, 0, "the Ethereal Plane (layer 1, the planes): reached by the Ethereal Travel spell and the blackrock, not by walking; above Tenebrae", nullptr},
    {44, -58784, -65760, 1, 0, "elemental plane (layer 1, the planes): beside the Ethereal Plane, from the offset of its teleporter there, then the nearest free spot (11 chunks off) so it overlaps nothing on its layer", nullptr},
    {45, -61843, -35211, 1, 0, "elemental plane (layer 1, the planes): beside the Ethereal Plane, from the offset of its teleporter there, then the nearest free spot (20 chunks off) so it overlaps nothing on its layer", nullptr},
    {46, -35021, -42049, 1, 0, "elemental plane (layer 1, the planes): beside the Ethereal Plane, from the offset of its teleporter there, then the nearest free spot (18 chunks off) so it overlaps nothing on its layer", nullptr},
    {47, -68553, -44829, 1, 0, "elemental plane (layer 1, the planes): beside the Ethereal Plane, from the offset of its teleporter there, then the nearest free spot (33 chunks off) so it overlaps nothing on its layer", nullptr},
    {20, -40960, 45568, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (596 fixed items: a stone floor and ruins)"},
    {29, -17408, 46080, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (150 fixed items)"},
    {33, -10240, 21504, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (131 fixed items)"},
    {56, -7168, 44032, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (182 fixed items)"},
    {159, 15872, 45056, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "leftover map (a couple of stray items)"},
    {200, 24064, 46080, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "leftover map (a few items)"},
    {222, 31232, 46080, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "leftover map (scattered items)"},
    {223, 67072, 46080, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (a big walled green field)"},
    {243, 89600, 32256, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "leftover map (a few items)"},
    {255, 109568, 46080, 0, 0, "unplaced: no link in the data or the usecode (map_index.md 1); parked in the Unplaced row south of the island (in the sea beyond Fisherman's Reef), each its own movable group", "unused test map (a garden square)"},
    };
    struct Placed { bool done = false; int dx = 0, dy = 0, layer = 0, trim = 0; std::string why; bool data = false; std::string extra; };
    std::vector<Placed> pl(groups.size());
    for (const Plan& p : kPlan) {
        if (!gof.count(p.map)) continue;
        Placed& q = pl[size_t(gof[p.map])];
        q.done = true; q.dx = p.dx; q.dy = p.dy; q.layer = p.layer; q.trim = p.trim;
        q.data = !p.unplaced_note;
        if (p.why) q.why = p.why;
        if (p.unplaced_note) q.extra = json({{"unplaced", true}, {"note", p.unplaced_note}}).dump();
    }
    for (size_t g = 0; g < groups.size(); ++g) {
        Placed& q = pl[g];
        if (!q.done) {  // a map the plan does not know (other data): parked as unplaced
            q.done = true; q.layer = 0; q.dx = -40960 - int(g) * 4096; q.dy = 90112; q.data = false;
            q.why = "unplaced: not in the layout plan";
            q.extra = json({{"unplaced", true}, {"note", "not in the layout plan"}}).dump();
        }
        if (!q.why.empty()) continue;
        // A vote seam: the text of its seam edge to a group placed with a text.
        for (const Edge& e : edges[g]) {
            if (!e.votes) continue;
            const Placed& o = pl[size_t(e.to)];
            bool exact = q.dx - o.dx == -e.ox && q.dy - o.dy == -e.oy;
            q.why = fmt("seam with map %u (walkable; same layer, %s): ", e.mb, exact ? "exact vote offset" : "offset from the plan") +
                    fmt("link map %u <-> map %u (", e.ma, e.mb) + e.why + ")";
            if (exact) break;
        }
        if (q.why.empty()) q.why = "plan";
    }
    // Seams declared by the plan (no identical-item votes: the maps draw the shared strip each in
    // their own way): map 7 joins map 40 at the gate teleporters' offset.
    struct Declared { unsigned a, b; const char* why; };
    static const Declared kDeclared[] = {
        {7, 40, "declared seam: Herdsman's Valley (7) is Tenebrae beyond its north wall; the two gate teleporters (map 40 egg id 0 "
                "at 13054,2322 -> map 7 at 15071,26239; map 7 egg id 3 at 15103,26815 -> map 40 at 13070,2618) agree on the offset "
                "within 16 units, map 7's copy of the north wall lines up with the city's (the fan map shows them as one town), "
                "so walking out of the north gate walks on into the valley."},
        {5, 6, "declared seam: the Cemetery (6) is the far end of the East Road (5); the two teleporters (map 5 egg id 1 at "
               "7423,10943 -> map 6 at 5375,29823; map 6 egg id 1 at 5375,30367 -> map 5 at 7424,11264) agree on the offset "
               "within 223 units, and the road's coast wall runs on into the Cemetery's (one coast on the fan map), so walking "
               "up the road walks on into the Cemetery."},
    };
    for (const Declared& dcl : kDeclared) {
        if (!gof.count(dcl.a) || !gof.count(dcl.b)) continue;
        WorldSeam s;
        s.a = std::min(dcl.a, dcl.b); s.b = std::max(dcl.a, dcl.b);
        const Placed &A = pl[size_t(gof[s.a])], &B = pl[size_t(gof[s.b])];
        s.dx = B.dx - A.dx; s.dy = B.dy - A.dy; s.dz = B.trim - A.trim;
        s.votes = 0;
        s.basis = "teleport edge";
        s.derived = dcl.why;
        seams.push_back(s);
    }

    WorldLayoutFile out;
    out.layout_revision = kLayoutRevision;
    out.atlas.comment =
        "Pagan stitched world layout (PLAN-WORLD-EDITOR.md section 1; engine/NOTES.md 'The stitched world'). Each group's "
        "maps are placed in one world space: offset [dx, dy, dz] (world units, dz = 256 x layer) is added to every item. "
        "'derived' says how each was found. Seams are walkable joins, links are kept as teleports. Edit freely (also in "
        "the editor); regenerate with: pagan --write-world-layout FILE (keeps areas[] and unknown fields).";
    for (size_t g = 0; g < groups.size(); ++g) {
        AtlasGroup a;
        a.maps = groups[g];
        std::sort(a.maps.begin(), a.maps.end());
        a.name = "map";
        for (size_t i = 0; i < a.maps.size(); ++i) a.name += (i ? "_" : "") + std::to_string(a.maps[i]);
        const MapKind* k = kind_of(a.maps[0]);
        a.label = std::string(a.maps.size() > 1 ? "Maps " : "Map ");
        for (size_t i = 0; i < a.maps.size(); ++i) a.label += (i ? ", " : "") + std::to_string(a.maps[i]);
        if (k) a.label += std::string(" ") + k->name;
        if (!pl[g].data && pl[g].extra.find("unplaced") != std::string::npos) a.label += " (unplaced)";
        a.dx = pl[g].dx;
        a.dy = pl[g].dy;
        a.layer = pl[g].layer;
        a.dz = a.layer * kLayerZ + pl[g].trim;
        a.derived = pl[g].why;
        a.from_data = pl[g].data;
        a.extra = pl[g].extra;
        out.atlas.groups.push_back(a);
    }
    std::sort(out.atlas.groups.begin(), out.atlas.groups.end(),
              [](const AtlasGroup& a, const AtlasGroup& b) { return a.maps[0] < b.maps[0]; });

    // Seams: the strip both cover (64-unit cells, world coordinates of a) and the portals.
    std::map<unsigned, MapCoverage> cov;
    auto coverage = [&](unsigned m) -> const MapCoverage& {
        auto it = cov.find(m);
        if (it == cov.end()) it = cov.emplace(m, compute_coverage(d, m)).first;
        return it->second;
    };
    for (WorldSeam& s : seams) {
        const AtlasGroup& ga = out.atlas.groups[size_t(out.atlas.group_of(s.a))];
        const MapCoverage &ca = coverage(s.a), &cb = coverage(s.b);
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (int cy = 0; cy < MapCoverage::kN; ++cy)
            for (int cx = 0; cx < MapCoverage::kN; ++cx) {
                int x = cx * 64 + 32, y = cy * 64 + 32;
                if (!ca.land(x, y) || !cb.land(x - s.dx, y - s.dy)) continue;
                x0 = std::min(x0, cx * 64); y0 = std::min(y0, cy * 64);
                x1 = std::max(x1, cx * 64 + 64); y1 = std::max(y1, cy * 64 + 64);
            }
        if (x0 <= x1) {
            s.strip[0] = x0 + ga.dx; s.strip[1] = y0 + ga.dy; s.strip[2] = x1 + ga.dx; s.strip[3] = y1 + ga.dy;
        }
        for (const TeleportLink& l : eggs)
            if ((l.from_map == s.a && l.to_map == s.b) || (l.from_map == s.b && l.to_map == s.a))
                s.portals.push_back({l.from_map, l.to_map, l.id, l.fx, l.fy, l.fz});
        s.derived += fmt(" Crossing points (the original teleporters, now walked through without a jump): %zu.", s.portals.size());
    }
    std::sort(seams.begin(), seams.end(), [](const WorldSeam& a, const WorldSeam& b) { return std::make_pair(a.a, a.b) < std::make_pair(b.a, b.b); });
    out.seams = seams;

    // Links: every teleporter egg between maps that is not a seam crossing, then the usecode ones.
    std::vector<WorldLink> kept;
    for (const TeleportLink& l : eggs) {
        if (l.from_map == l.to_map) continue;
        if (out.seam(l.from_map, l.to_map)) continue;
        WorldLink w;
        w.from_map = l.from_map; w.to_map = l.to_map; w.kind = "egg"; w.id = int(l.id); w.has_from = true;
        w.from[0] = l.fx; w.from[1] = l.fy; w.from[2] = l.fz;
        w.to[0] = l.tx; w.to[1] = l.ty; w.to[2] = l.tz;
        int la = out.atlas.group_of(l.from_map), lb = out.atlas.group_of(l.to_map);
        int L1 = la >= 0 ? out.atlas.groups[size_t(la)].layer : 0, L2 = lb >= 0 ? out.atlas.groups[size_t(lb)].layer : 0;
        w.derived = fmt("teleporter egg (shape 508, id %u) in map %u -> arrival egg in map %u; ", l.id, l.from_map, l.to_map) +
                    (L1 == L2 ? fmt("both on layer %d", L1) : fmt("layer %d -> layer %d", L1, L2)) +
                    "; kept as a teleport (not a seam: fewer than 40 identical border items)";
        kept.push_back(w);
    }
    for (WorldLink& w : out_links)
        if (w.kind != "egg") {
            w.derived = "usecode (critical path map_links.json): " + w.derived + "; kept as a teleport";
            kept.push_back(w);
        }
    out.links = kept;

    for (auto [z, nz] : {std::pair{16u, 26u}, {24u, 25u}}) {
        int gi = out.atlas.group_of(z);
        if (gi < 0) continue;
        WorldVariant v;
        v.group = out.atlas.groups[size_t(gi)].name;
        v.when_zero = z;
        v.when_nonzero = nz;
        v.derived = fmt("map_index.md 1: maps %u and %u are one place in two states; the usecode picks %u while G[52:2] is 0 "
                        "and %u once it is set (CARTHAXH, MAPTELE, RECALL); one slot, same offset",
                        z, nz, z, nz);
        out.variants.push_back(v);
    }
    out.layer_names = {{-2, "deep levels (the Shrine of the Ancient Ones, the Ancient Necromancers, the Lower Catacombs and the Pit of the Dead)"},
                       {-1, "underground: caves, catacombs and interiors under where they are entered (Upper Catacombs, Treasure Cove, "
                            "the ghost armour area, the Obsidian Fortress, the lava tunnel, Carthax's breaking ground, the Hall of the Mountain King)"},
                       {0, "the surface, as on the whole-island fan map (Tenebrae with Herdsman's Valley, the docks, Fisherman's Reef, "
                           "the East Road, the Cemetery, the Plateau, Stone Cove, Carthax Lake, Argentrock Isle, Daemon's Crag) and the "
                           "Unplaced row south of the island"},
                       {1, "the planes (ethereal, fire, water, air, earth): reached by spells, above the island"}};
    for (const AtlasGroup& g : out.atlas.groups)
        if (!out.layer_names.count(g.layer)) out.layer_names[g.layer] = fmt("overflow layer %d (moved off a taken layer)", g.layer);
    return out;
}

std::vector<std::string> check_world_layout(const WorldLayoutFile& l, const std::vector<MapFootprint>& fps) {
    std::vector<std::string> out;
    std::map<unsigned, const MapFootprint*> fp;
    for (const MapFootprint& f : fps) fp[f.map] = &f;
    for (const MapFootprint& f : fps)
        if (l.atlas.group_of(f.map) < 0) out.push_back(fmt("map %u is in no group", f.map));
    // Seam clusters may overlap (the duplicated strip is drawn once, from the map that covers it).
    std::vector<int> cl(l.atlas.groups.size());
    std::iota(cl.begin(), cl.end(), 0);
    std::function<int(int)> root = [&](int i) { return cl[size_t(i)] == i ? i : cl[size_t(i)] = root(cl[size_t(i)]); };
    for (const WorldSeam& s : l.seams) {
        int a = l.atlas.group_of(s.a), b = l.atlas.group_of(s.b);
        if (a >= 0 && b >= 0) cl[size_t(root(a))] = root(b);
    }
    Occupancy occ;
    for (size_t g = 0; g < l.atlas.groups.size(); ++g) {
        const AtlasGroup& G = l.atlas.groups[g];
        MapFootprint u;
        for (unsigned m : G.maps) {
            if (!fp.count(m)) continue;
            const MapFootprint& f = *fp[m];
            u.occ |= f.occ;
            u.cx0 = std::min(u.cx0, f.cx0); u.cy0 = std::min(u.cy0, f.cy0);
            u.cx1 = std::max(u.cx1, f.cx1); u.cy1 = std::max(u.cy1, f.cy1);
            for (size_t c = 0; c < u.count.size(); ++c) u.count[c] = uint16_t(std::min(65535, u.count[c] + f.count[c]));
        }
        std::set<int> allowed;
        for (size_t h = 0; h < l.atlas.groups.size(); ++h)
            if (root(int(h)) == root(int(g))) allowed.insert(int(h));
        for (auto [h, n] : occ.hits(u, G.dx, G.dy, G.layer, allowed))
            out.push_back(fmt("groups %s and %s overlap on layer %d (%d chunks)", l.atlas.groups[size_t(h)].name.c_str(), G.name.c_str(),
                              G.layer, n));
        occ.add(u, G.dx, G.dy, G.layer, int(g));
        if (std::abs(G.dz - G.layer * kLayerZ) > kMaxHeightTrim)
            out.push_back(fmt("group %s: dz %d is not 256 x layer %d (+/- a trim up to %d)", G.name.c_str(), G.dz, G.layer, kMaxHeightTrim));
        // Other layers: a group whose z range [dz, dz + 255] reaches into this one's must not share
        // its chunks either (a height trim makes neighbouring layers' ranges meet).
        for (size_t h = 0; h < g; ++h) {
            const AtlasGroup& H = l.atlas.groups[h];
            if (H.layer == G.layer || std::abs(H.dz - G.dz) >= kLayerZ) continue;
            Occupancy one;
            MapFootprint hu;
            for (unsigned m : H.maps) {
                if (!fp.count(m)) continue;
                const MapFootprint& f = *fp[m];
                hu.cx0 = std::min(hu.cx0, f.cx0); hu.cy0 = std::min(hu.cy0, f.cy0);
                hu.cx1 = std::max(hu.cx1, f.cx1); hu.cy1 = std::max(hu.cy1, f.cy1);
                for (size_t c = 0; c < hu.count.size(); ++c) hu.count[c] = uint16_t(std::min(65535, hu.count[c] + f.count[c]));
            }
            one.add(hu, H.dx, H.dy, 0, int(h));
            for (auto [x, n] : one.hits(u, G.dx, G.dy, 0, {}))
                out.push_back(fmt("groups %s (layer %d, dz %d) and %s (layer %d, dz %d) share z and %d chunks", H.name.c_str(), H.layer,
                                  H.dz, G.name.c_str(), G.layer, G.dz, n));
        }
    }
    for (const WorldSeam& s : l.seams) {
        int a = l.atlas.group_of(s.a), b = l.atlas.group_of(s.b);
        if (a < 0 || b < 0) { out.push_back(fmt("seam %u/%u: a map is not placed", s.a, s.b)); continue; }
        const AtlasGroup &A = l.atlas.groups[size_t(a)], &B = l.atlas.groups[size_t(b)];
        if (A.layer != B.layer) out.push_back(fmt("seam %u/%u: not on one layer", s.a, s.b));
        if (B.dz - A.dz != s.dz) out.push_back(fmt("seam %u/%u: group heights differ by %d, the seam says %d", s.a, s.b, B.dz - A.dz, s.dz));
        if (B.dx - A.dx != s.dx || B.dy - A.dy != s.dy)
            out.push_back(fmt("seam %u/%u: group offsets differ by %d,%d, the seam says %d,%d", s.a, s.b, B.dx - A.dx, B.dy - A.dy, s.dx, s.dy));
        if (s.basis == "votes" && s.votes < 40) out.push_back(fmt("seam %u/%u: only %d votes", s.a, s.b, s.votes));
        if (s.basis != "votes" && s.basis != "teleport edge") out.push_back(fmt("seam %u/%u: unknown basis %s", s.a, s.b, s.basis.c_str()));
    }
    for (const WorldLink& k : l.links)
        if ((k.has_from && l.atlas.group_of(k.from_map) < 0) || l.atlas.group_of(k.to_map) < 0)
            out.push_back(fmt("link %u -> %u: a map is not placed", k.from_map, k.to_map));
    return out;
}

}  // namespace pg
