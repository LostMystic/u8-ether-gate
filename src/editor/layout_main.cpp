// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// pagan_layout: tools for the stitched world's layout (engine/data/world_layout.json).
//   pagan_layout GAME check LAYOUT.json      the validator: fails (exit 1) on any overlap of two groups
//                                            on one layer (seam partners: only their shared strip),
//                                            a map in no group, a bad dz or a seam off its offset
//   pagan_layout GAME footprints OUT.json    every map's occupied 512-unit chunks (record counts)
//   pagan_layout GAME generate OUT.json [MAP_LINKS.json]   writes the derived layout
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/gamefs.h"
#include "data/gamedata.h"
#include "world/atlas_layout.h"
#include "world/world_layout.h"
#include "world/item.h"
#include "world/world_space.h"
#include <algorithm>
#include <map>
#include <unordered_map>

using namespace pg;

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: pagan_layout GAME check LAYOUT | footprints OUT | generate OUT [MAP_LINKS]\n");
        return 2;
    }
    GameFS fs;
    if (!fs.open(argv[1])) { std::fprintf(stderr, "game folder not found: %s\n", argv[1]); return 2; }
    GameData d;
    if (!d.load(fs)) { std::fprintf(stderr, "load: %s\n", d.error().c_str()); return 2; }
    std::string cmd = argv[2];
    if (cmd == "check") {
        WorldLayoutFile f;
        std::string err;
        if (!f.load(argv[3], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::vector<std::string> probs = check_world_layout(f, compute_footprints(d));
        for (const std::string& p : probs) std::printf("problem: %s\n", p.c_str());
        std::printf("%s: %zu groups, %zu seams, %zu links: %s\n", argv[3], f.atlas.groups.size(), f.seams.size(), f.links.size(),
                    probs.empty() ? "no overlap on any layer" : "FAILED");
        return probs.empty() ? 0 : 1;
    }
    if (cmd == "footprints") {
        std::ofstream o(argv[3]);
        o << "{\"maps\": [\n";
        bool first = true;
        for (const MapFootprint& f : compute_footprints(d)) {
            o << (first ? "" : ",\n") << "{\"map\": " << f.map << ", \"bounds\": [" << f.cx0 << ", " << f.cy0 << ", " << f.cx1 << ", "
              << f.cy1 << "], \"median_z\": " << f.median_z << ", \"chunks\": [";
            bool fc = true;
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx)
                    if (f.count[size_t(cy) * 64 + cx]) {
                        o << (fc ? "" : ",") << "[" << cx << "," << cy << "," << f.count[size_t(cy) * 64 + cx] << "]";
                        fc = false;
                    }
            o << "]}";
            first = false;
        }
        o << "\n]}\n";
        return 0;
    }
    if (cmd == "generate") {
        WorldLayoutFile f = generate_world_layout(d, argc > 4 ? read_file(argv[4]) : std::string());
        std::ofstream o(argv[3]);
        o << f.to_json();
        std::vector<std::string> probs = check_world_layout(f, compute_footprints(d));
        for (const std::string& p : probs) std::printf("problem: %s\n", p.c_str());
        return probs.empty() ? 0 : 1;
    }
    if (cmd == "coverage") {
        // Per map, the 64-unit cells holding fixed scenery: one byte per cell (bit 0 drawn, bit 1
        // land / roof), 512 x 512 cells, written raw to OUT.<map>.bin.
        for (const MapFootprint& f : compute_footprints(d)) {
            MapCoverage c = compute_coverage(d, f.map);
            std::ofstream o(std::string(argv[3]) + "." + std::to_string(f.map) + ".bin", std::ios::binary);
            o.write(reinterpret_cast<const char*>(c.bits.data()), std::streamsize(c.bits.size()));
        }
        return 0;
    }
    if (cmd == "votes" && argc > 4) {
        // Identical top-level records (shape, frame, z) of map B that line up with map A: the
        // best offsets (A point - B point) over the whole maps, for finding where two maps join.
        unsigned a = unsigned(std::atoi(argv[3])), b = unsigned(std::atoi(argv[4]));
        std::vector<ItemRecord> ra = atlas_top_level_records(d, a), rb = atlas_top_level_records(d, b);
        std::unordered_multimap<uint32_t, const ItemRecord*> idx;
        for (const ItemRecord& r : ra) idx.emplace((uint32_t(r.shape) << 8) | r.frame, &r);
        std::map<std::pair<int, int>, int> votes;
        for (const ItemRecord& r : rb) {
            auto rg = idx.equal_range((uint32_t(r.shape) << 8) | r.frame);
            int n = 0;
            for (auto it = rg.first; it != rg.second; ++it) ++n;
            if (n > 400) continue;  // common ground tiles: noise
            for (auto it = rg.first; it != rg.second; ++it)
                if (it->second->z == r.z) ++votes[{it->second->x - r.x, it->second->y - r.y}];
        }
        std::vector<std::pair<int, std::pair<int, int>>> v;
        for (auto& [k, n] : votes) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 12; ++i) std::printf("%d votes at %d,%d\n", v[i].first, v[i].second.first, v[i].second.second);
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
