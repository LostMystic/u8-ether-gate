// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/map_patches.h"

#include <fstream>
#include <map>
#include "json/json.hpp"
#include <sstream>

#include "core/variants.h"
#include "data/compat.h"

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

bool load_text(const std::string& text, std::string& err) {
    using json = nlohmann::json;
    std::vector<Group> t;
    try {
        json j = json::parse(text, nullptr, true, true);
        for (const json& e : j.at("groups")) {
            Group g;
            g.id = e.at("id").get<std::string>();
            g.map = e.at("map").get<unsigned>();
            for (const json& i : e.at("items"))
                g.items.push_back(rec(i.at("shape").get<uint16_t>(), i.at("frame").get<uint8_t>(), i.at("x").get<uint16_t>(),
                                      i.at("y").get<uint16_t>(), i.at("z").get<uint8_t>()));
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
    table() = std::move(t);
    return true;
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
    if (g.id == "underworld_entrance") return underworld_entrance() && !underworld_breach_open();
    return true;
}

bool moves_on(const Group& g) {
    if (!data_matches(g)) return false;
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

}  // namespace map_patches
}  // namespace pg
