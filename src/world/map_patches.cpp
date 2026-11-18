// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/map_patches.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include "json/json.hpp"
#include <sstream>

#include "core/variants.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "data/typeflags.h"
#include <unordered_map>
#include <unordered_set>

namespace pg {
namespace map_patches {

namespace {

ItemRecord rec(uint16_t shape, uint8_t frame, uint16_t x, uint16_t y, uint8_t z) {
    ItemRecord r;
    r.shape = shape;
    r.frame = frame;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

// The built-in copy of data/patches/map_content.json.
std::vector<Group> builtin() {
    Group e;
    e.id = "underworld_entrance";
    e.map = 57;
    e.moves = {
        {359, 0, 13599, 17183, 0, 13791, 17183, 0},
        {218, 5, 13663, 17215, 0, 13855, 17215, 0},
    };
    e.items = {
        rec(526, 0, 13567, 17151, 8),
        rec(526, 6, 13567, 17151, 24),
        rec(526, 1, 13567, 17215, 0),
        rec(526, 2, 13567, 17215, 16),
        rec(526, 7, 13631, 17119, 0),
        rec(526, 0, 13631, 17183, 0),
        rec(526, 1, 13631, 17247, 0),
        rec(526, 8, 13631, 17151, 16),
        rec(526, 2, 13631, 17215, 16),
        rec(524, 1, 13727, 17183, 0),
        rec(526, 0, 13695, 17079, 0),
        rec(524, 0, 13727, 17311, 0),
        rec(553, 24, 13759, 17119, 0),
        rec(553, 26, 13727, 17047, 0),
    };
    return {e};
}

std::vector<Group>& table() {
    static std::vector<Group> t = builtin();
    return t;
}

}  // namespace

const std::vector<Group>& groups() { return table(); }

const Group* find(const std::string& id) {
    for (const Group& g : table())
        if (g.id == id) return &g;
    return nullptr;
}

namespace {
std::set<std::string>& active_roads() {
    static std::set<std::string> s;
    return s;
}
bool parse_groups(const std::string& text, bool roads, std::vector<Group>& t, std::string& err) {
    using json = nlohmann::json;
    try {
        json j = json::parse(text, nullptr, true, true);
        for (const json& e : j.at("groups")) {
            Group g;
            g.id = e.at("id").get<std::string>();
            g.map = e.at("map").get<unsigned>();
            g.road = roads;
            for (const json& i : e.at("items")) {
                ItemRecord r = rec(i.at("shape").get<uint16_t>(), i.at("frame").get<uint8_t>(), i.at("x").get<uint16_t>(),
                                   i.at("y").get<uint16_t>(), i.at("z").get<uint8_t>());
                r.flags = i.value("flags", uint16_t(0));
                r.quality = i.value("quality", uint16_t(0));
                g.items.push_back(r);
            }
            if (e.contains("moves"))
                for (const json& i : e.at("moves")) {
                    Move m;
                    m.shape = i.at("shape").get<uint16_t>();
                    m.frame = i.at("frame").get<uint8_t>();
                    m.x = i.at("x").get<uint16_t>();
                    m.y = i.at("y").get<uint16_t>();
                    m.z = i.at("z").get<uint8_t>();
                    m.to_x = i.at("to_x").get<uint16_t>();
                    m.to_y = i.at("to_y").get<uint16_t>();
                    m.to_z = i.at("to_z").get<uint8_t>();
                    g.moves.push_back(m);
                }
            t.push_back(std::move(g));
        }
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    return true;
}
}  // namespace

bool load_text(const std::string& text, std::string& err) {
    std::vector<Group> t;
    if (!parse_groups(text, false, t, err)) return false;
    for (const Group& g : table())
        if (g.road) t.push_back(g);  // the roads stay
    table() = std::move(t);
    return true;
}

bool load_roads_text(const std::string& text, std::string& err) {
    std::vector<Group> r;
    if (!parse_groups(text, true, r, err)) return false;
    std::vector<Group> t;
    for (const Group& g : table())
        if (!g.road) t.push_back(g);
    for (Group& g : r) t.push_back(std::move(g));
    table() = std::move(t);
    return true;
}

bool load_roads_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_roads_text(ss.str(), err);
}

void set_active_roads(const std::vector<std::string>& ids) { active_roads() = std::set<std::string>(ids.begin(), ids.end()); }
bool road_active(const std::string& id) { return active_roads().count(id) != 0; }
bool road_loaded(const std::string& id) {
    for (const Group& g : table())
        if (g.road && g.id == id) return true;
    return false;
}

bool load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_text(ss.str(), err);
}

bool data_matches(const Group& g) {
    if (compat::fixed_map_known(g.map) && compat::nonfixed_map_known(g.map)) return true;
    compat::off("map patch " + g.id, "map " + std::to_string(g.map) + " differs in this copy");
    return false;
}

bool group_on(const Group& g) {
    if (!data_matches(g)) return false;
    if (g.road) return road_active(g.id);
    if (g.id == "underworld_entrance") return underworld_entrance() && !underworld_breach_open();
    return true;
}

bool moves_on(const Group& g) {
    if (!data_matches(g)) return false;
    if (g.road) return road_active(g.id);
    if (g.id == "underworld_entrance") return underworld_entrance();  // they stay moved when the Breach opens
    return true;
}

void apply(unsigned map, std::vector<ItemRecord>& fixed) {
    for (const Group& g : table())
        if (g.map == map && group_on(g)) fixed.insert(fixed.end(), g.items.begin(), g.items.end());
}

void apply_moves(unsigned map, std::vector<ItemRecord>& nonfixed) {
    for (const Group& g : table()) {
        if (g.map != map || !moves_on(g)) continue;
        for (const Move& m : g.moves)
            for (ItemRecord& r : nonfixed)
                if (r.shape == m.shape && r.frame == m.frame && r.x == m.x && r.y == m.y && r.z == m.z) {
                    r.x = m.to_x;
                    r.y = m.to_y;
                    r.z = m.to_z;
                    break;
                }
    }
}

namespace {
struct HiddenState {
    const GameData* d = nullptr;
    std::unordered_map<unsigned, std::unordered_set<long long>> cells;
    std::unordered_map<unsigned, std::unordered_set<long long>> keep_ground;  // of those, the floor stays
};
HiddenState& hidden_state() {
    static HiddenState h;
    return h;
}
long long hkey(int x, int y) { return (static_cast<long long>(x) << 32) ^ static_cast<long long>(static_cast<uint32_t>(y)); }
}  // namespace

void set_hidden(const GameData* d, const std::vector<std::pair<unsigned, std::vector<HiddenRow>>>& maps) {
    HiddenState& h = hidden_state();
    h.d = d;
    h.cells.clear();
    h.keep_ground.clear();
    for (const auto& [m, rows] : maps) {
        auto& set = h.cells[m];
        for (const HiddenRow& r : rows)
            for (int x = r.x0; x <= r.x1; ++x) {
                if (r.keep_ground) h.keep_ground[m].insert(hkey(x, r.y));
                else set.insert(hkey(x, r.y));
            }
        if (set.empty()) h.cells.erase(m);
    }
}

bool hides(unsigned map) { return hidden_state().d && hidden_state().cells.count(map) != 0; }

bool hidden_cell(unsigned map, int lx, int ly) {
    const HiddenState& h = hidden_state();
    auto it = h.cells.find(map);
    return it != h.cells.end() && it->second.count(hkey(lx, ly)) != 0;
}

size_t hidden_count(unsigned map) {
    auto it = hidden_state().cells.find(map);
    return it == hidden_state().cells.end() ? 0 : it->second.size();
}

std::string hidden_signature(unsigned map) {
    auto it = hidden_state().cells.find(map);
    if (it == hidden_state().cells.end() || !hidden_state().d) return std::string();
    std::vector<long long> v(it->second.begin(), it->second.end());
    std::sort(v.begin(), v.end());
    std::string out;
    out.reserve(v.size() * 8);
    for (long long k : v) out.append(reinterpret_cast<const char*>(&k), sizeof k);
    return out;
}

void apply_hidden(unsigned map, std::vector<ItemRecord>& fixed) {
    if (!hides(map)) return;
    const GameData& d = *hidden_state().d;
    std::vector<ItemRecord> out;
    out.reserve(fixed.size() * 2);
    auto keep = [&](const ItemRecord& r) {
        const unsigned fam = d.typeflags.get(r.shape).family();
        if (fam == kFamMonsterEgg || fam == kFamTeleportEgg || fam == kFamUsecodeEgg) return true;  // scripts' things stay
        // Hidden only when every cell it covers is: a big tile or wall at the edge of the hidden
        // ground stays (a little overlap with the neighbour's version there, never a hole).
        const ShapeInfo& si = d.typeflags.get(r.shape);
        int xd = 0, yd = 0, zd = 0;
        si.footprint(r.flags & kItemFlipped, xd, yd, zd);
        const bool floor = si.is(ShapeInfo::kLand) || si.is(ShapeInfo::kSea);
        const auto kg = hidden_state().keep_ground.find(map);
        const int x0 = std::max(0, int(r.x) - std::max(xd, 1) + 1) / 128, x1 = int(r.x) / 128;
        const int y0 = std::max(0, int(r.y) - std::max(yd, 1) + 1) / 128, y1 = int(r.y) / 128;
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
                if (!hidden_cell(map, cx, cy) ||
                    (floor && kg != hidden_state().keep_ground.end() && kg->second.count(hkey(cx, cy))))
                    return true;
        return false;
    };
    for (const ItemRecord& r : fixed) {
        if (d.typeflags.get(r.shape).family() == kFamGlobEgg) {
            // Its contents, as plain fixed items (CurrentMap::expand_glob's placement), cut cell by cell.
            const int bx = r.x & ~0x1FF, by = r.y & ~0x1FF;
            for (const GlobItem& g : d.globs.get(r.quality)) {
                ItemRecord q;
                q.shape = g.shape;
                q.frame = g.frame;
                q.x = uint16_t(bx + 2 * g.x + 1);
                q.y = uint16_t(by + 2 * g.y + 1);
                q.z = uint8_t(std::min(255, r.z + g.z));
                if (keep(q)) out.push_back(q);
            }
            continue;
        }
        if (keep(r)) out.push_back(r);
    }
    fixed = std::move(out);
}

}  // namespace map_patches
}  // namespace pg
