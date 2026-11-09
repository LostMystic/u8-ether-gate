// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// Checks for the world atlas layout (world/atlas_layout.*) and the zoom tiles
// (world/tile_cache.*). Usage: pagan_atlas_tests GAME_DIR [LAYOUT_JSON]
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "check.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "gfx/surface.h"
#include "world/atlas_layout.h"
#include "world/current_map.h"
#include "world/tile_cache.h"
#include "world/world_view.h"

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

void test_json() {
    AtlasLayout l;
    std::string err;
    CHECK(l.from_json(R"({"comment": "x", "groups": [
        {"name": "a", "label": "A", "maps": [3, 40], "offset": [100, -200], "source": "data", "derived": "why"},
        {"name": "b", "maps": [5]}]})", err));
    CHECK_EQ(l.groups.size(), 2);
    if (l.groups.size() == 2) {
        CHECK_EQ(l.groups[0].maps.size(), 2);
        CHECK_EQ(l.groups[0].dx, 100);
        CHECK_EQ(l.groups[0].dy, -200);
        CHECK(l.groups[0].from_data);
        CHECK(l.groups[1].label == "b");  // label defaults to the name
        CHECK_EQ(l.groups[1].dx, 0);
        CHECK(!l.groups[1].from_data);
    }
    CHECK_EQ(l.group_of(40), 0);
    CHECK_EQ(l.group_of(5), 1);
    CHECK_EQ(l.group_of(6), -1);
    // Round trip.
    AtlasLayout r;
    CHECK(r.from_json(l.to_json(), err));
    CHECK_EQ(r.groups.size(), l.groups.size());
    CHECK(r.to_json() == l.to_json());
    // Errors.
    AtlasLayout bad;
    CHECK(!bad.from_json("not json", err));
    CHECK(!bad.from_json(R"({"groups": [{"name": "a", "maps": [3]}, {"name": "b", "maps": [3]}]})", err));
    CHECK(err.find("two groups") != std::string::npos);
    CHECK(!bad.from_json(R"({"groups": [{"name": "a", "maps": []}]})", err));
    CHECK(!bad.from_json(R"({"groups": [{"name": "a", "maps": [3], "offset": [1]}]})", err));
    CHECK(!bad.from_json(R"({"groups": [{"name": "a", "maps": [300]}]})", err));
    CHECK(bad.from_json(R"({"groups": []})", err));
}

void check_covers(const AtlasLayout& l, const GameData& d, const char* what) {
    std::set<unsigned> want;
    for (const MapFootprint& f : compute_footprints(d)) want.insert(f.map);
    std::set<unsigned> have;
    for (const AtlasGroup& g : l.groups)
        for (unsigned m : g.maps) have.insert(m);
    if (have != want) std::printf("  %s: layout maps differ from the populated maps\n", what);
    CHECK(have == want);
}

void test_layout(const GameData& d, const char* file) {
    std::vector<MapFootprint> fps = compute_footprints(d);
    CHECK_EQ(fps.size(), 42);  // items.md: 42 non-empty maps
    std::vector<TeleportLink> links = teleport_links(d);
    bool found = false;
    for (const TeleportLink& t : links)
        if (t.from_map == 3 && t.to_map == 40 && t.id == 0) {
            found = true;
            CHECK_EQ(t.fx, 11516); CHECK_EQ(t.fy, 1660);  // maps.md 4.3 example
            CHECK_EQ(t.tx, 13038); CHECK_EQ(t.ty, 15042);
        }
    CHECK(found);
    for (const TeleportLink& t : links) CHECK(t.from_map != t.to_map);

    AtlasLayout l = generate_atlas_layout(d);
    check_covers(l, d, "generated");
    int g40 = l.group_of(40), g39 = l.group_of(39), g41 = l.group_of(41), g49 = l.group_of(49);
    CHECK(g40 >= 0 && g39 >= 0 && g41 >= 0);
    if (g40 >= 0 && g39 >= 0 && g41 >= 0) {
        CHECK_EQ(l.groups[size_t(g40)].dx, 0);
        CHECK_EQ(l.groups[size_t(g40)].dy, 0);
        // Tenebrae: map 39 is west of 40 and 41 east, on seams of repeated border items.
        CHECK(l.groups[size_t(g39)].from_data);
        CHECK(l.groups[size_t(g41)].from_data);
        CHECK(l.groups[size_t(g39)].dx < -6000);
        CHECK(l.groups[size_t(g41)].dx > 1000);
        CHECK(std::abs(l.groups[size_t(g39)].dy) < 1024);
    }
    // Maps 49 and 50 share one coordinate space (their teleport eggs are at the same spots).
    CHECK(g49 >= 0 && g49 == l.group_of(50));
    int data_n = 0;
    for (const AtlasGroup& g : l.groups) data_n += g.from_data;
    std::printf("  generated layout: %zu groups, %d from data\n", l.groups.size(), data_n);
    CHECK(data_n >= 10);

    // No two groups placed by rule or guess cover the same chunks.
    auto ex = atlas_exclusions(l, fps);
    for (auto& [m, b] : ex) {
        int g = l.group_of(m);
        CHECK(g >= 0);
    }

    if (file) {
        AtlasLayout f;
        std::string err;
        bool ok = f.load(file, err);
        if (!ok) std::printf("  %s: %s\n", file, err.c_str());
        CHECK(ok);
        if (ok) check_covers(f, d, file);
    }
}

// A level-1 tile is the 2x2 box filter of a normal render of its 512 x 512 block.
void test_tiles(const GameData& d) {
    const unsigned map = 40;
    const int bx = 1, by = 4;  // a block in Tenebrae (native screen 512..1023, 2048..2559)
    TileCache cache(d);
    CHECK(cache.build(map, 1, bx, by, 1e300));
    const TileCache::Tile* t = cache.get(map, 1, bx, by);
    CHECK(t && !t->empty);
    CurrentMap cm(d);
    cm.load(map);
    WorldView v(d);
    WorldView::Options o;
    o.roof_hiding = false;
    Surface8 s(512, 512);
    int Sx = bx * 512 + 256, Sy = by * 512 + 256;
    int cam[3] = {2 * Sx + 4 * Sy, 4 * Sy - 2 * Sx, 0};
    cm.update_fast_area(cam, cam, 512 + 256, 512 + 512);
    s.clear(0);
    v.render(cm, s, cam, cam, o);
    if (t && !t->empty) {
        int bad = 0;
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                unsigned sum[3] = {0, 0, 0}, a = 0;
                for (int k = 0; k < 4; ++k) {
                    uint8_t i = s.data()[size_t(2 * y + k / 2) * 512 + 2 * x + k % 2];
                    if (i == 0) continue;
                    a += 255;
                    for (int c = 0; c < 3; ++c) sum[c] += d.palette.rgb8[i][size_t(c)];
                }
                uint32_t want = ((a / 4) << 24) | ((sum[0] / 4) << 16) | ((sum[1] / 4) << 8) | (sum[2] / 4);
                if (t->px[size_t(y) * 256 + x] != want) ++bad;
            }
        CHECK_EQ(bad, 0);
    }
    // Level 2 from four level-1 tiles; far outside the map is empty without rendering.
    size_t before = cache.renders();
    CHECK(cache.build(map, 2, 0, 2, 1e300));
    CHECK(cache.renders() - before <= 4);
    before = cache.renders();
    CHECK(cache.build(map, 3, 40, 40, 1e300));
    CHECK(cache.get(map, 3, 40, 40) && cache.get(map, 3, 40, 40)->empty);
    CHECK_EQ(cache.renders() - before, 0);
    // A deadline in the past builds nothing that needs a render.
    TileCache c2(d);
    CHECK(!c2.build(map, 1, bx, by, 0));
    CHECK_EQ(c2.renders(), 0);

    // Composing: the native-scale view of one map equals the tiles.
    std::vector<uint32_t> out(64 * 64);
    int still = draw_tiles(cache, {{map, 0, 0}}, 1, bx * 512 + 256, by * 512 + 256, out.data(), 64, 64, 1e300);
    CHECK_EQ(still, 0);
    if (t && !t->empty) CHECK_EQ(out[size_t(32) * 64 + 32] | 0xFF000000u, t->px[size_t(128) * 256 + 128] | 0xFF000000u);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    GameFS fs;
    if (root.empty() || !fs.open(root)) {
        std::printf("game folder not found\n");
        return 1;
    }
    test_json();
    GameData d;
    bool ok = d.load(fs);
    CHECK(ok);
    if (ok) {
        test_layout(d, argc > 2 ? argv[2] : nullptr);
        test_tiles(d);
    }
    std::printf("atlas: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
