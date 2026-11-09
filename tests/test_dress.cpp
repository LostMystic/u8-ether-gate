// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// The map dresser (src/editor): tile-kit mining, dressing the sample painted areas
// (tests/data/areas), validation, determinism, the JSON formats and the preview renders.
//   pagan_dress_tests GAME_DIR data/tilekit.json tests/data/areas SHOT_DIR
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "data/gamedata.h"
#include "editor/area_render.h"
#include "editor/dresser.h"
#include "editor/tilekit_miner.h"
#include "world/current_map.h"

namespace pg {
bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale);
}
using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has_theme(const TileKit& k, const char* n) { return k.themes.count(n) != 0; }

void test_mining(const GameData& d, TileKit& kit) {
    std::string report;
    CHECK(mine_tilekit(d, kit, report));
    // Doors are found by their usecode class names.
    for (unsigned s : {68u, 69u, 340u, 342u}) {
        const ShapeEntry* e = kit.shape(s);
        CHECK(e && e->role == Role::kDoor);
    }
    // Known floors and water; a 4x4 tile is the dominant floor (the area cell size).
    CHECK(kit.shape(3) && kit.shape(3)->role == Role::kFloor);
    CHECK(kit.shape(163) && kit.shape(163)->role == Role::kFloor);  // Tenebrae wood planks
    CHECK(kit.shape(347) && kit.shape(347)->role == Role::kWater);  // CLRWATER
    CHECK(kit.shape(409) && kit.shape(409)->role == Role::kLava);   // CLRLAVA
    CHECK(kit.shape(149) && kit.shape(149)->role == Role::kWallEW); // timber wall
    CHECK(kit.shape(145) && kit.shape(145)->role == Role::kWallNS);
    CHECK(kit.shape(148) && kit.shape(148)->role == Role::kPost);
    CHECK(kit.shape(412) && kit.shape(412)->role == Role::kRoof);
    CHECK(kit.shape(267) && kit.shape(267)->role == Role::kLight);  // CAMPFIRE
    CHECK(kit.counts["floor_tiles_128"] > 250000);
    CHECK(kit.counts["floor_tiles_128"] > 4 * kit.counts["floor_tiles_256"]);
    CHECK(kit.counts["items"] > 550000);
    CHECK(kit.floors.size() >= 20);
    CHECK(kit.walls.size() >= 8);
    CHECK(kit.prefabs.size() >= 100);
    for (const char* t : {"cave", "catacombs", "tenebrae", "cemetery", "plateau", "lava", "beach"}) CHECK(has_theme(kit, t));
    // Theme choices that should come out of the data.
    CHECK(kit.themes["tenebrae"].wall == "ws15");
    CHECK(kit.themes["catacombs"].wall == "ws296");
    CHECK(kit.themes["cave"].wall == "ws26");
    CHECK(kit.themes["valley"].interior_floor == "fs163");
    const FloorSet* wood = kit.floor("fs163");
    CHECK(wood && wood->roof_fraction > 0.8);  // wood floors are indoors
    const FloorSet* water = kit.floor("fs347");
    CHECK(water && water->kind == "water");
    CHECK(!kit.themes["beach"].shore[14].empty() || !kit.themes["beach"].shore[13].empty());
    // The timber wall style has doors, posts and pieces of several lengths.
    const WallStyle* timber = kit.wall("ws149");
    CHECK(timber && timber->height == 40 && !timber->post.empty() && !timber->ew[4].empty() && !timber->ns[4].empty());
    CHECK(timber && timber->door_ew.shape == 69 && timber->door_ns.shape == 68);
    // Prefab labels for the POIs the dresser is asked for.
    std::set<std::string> labels;
    for (const Prefab& p : kit.prefabs) labels.insert(p.label);
    for (const char* l : {"campfire camp", "altar", "shrine", "grave", "grave row", "crypt", "remains", "bookcase corner",
                          "market stall", "lamp post", "table and chairs", "bedroom"})
        CHECK(labels.count(l));
    // JSON round trip.
    TileKit back;
    std::string err;
    CHECK(parse_tilekit(tilekit_to_json(kit), back, err));
    CHECK_EQ(back.shapes.size(), kit.shapes.size());
    CHECK_EQ(back.floors.size(), kit.floors.size());
    CHECK_EQ(back.walls.size(), kit.walls.size());
    CHECK_EQ(back.prefabs.size(), kit.prefabs.size());
    CHECK(tilekit_to_json(back) == tilekit_to_json(kit));
    // Mining is deterministic.
    TileKit again;
    mine_tilekit(d, again, report);
    CHECK(tilekit_to_json(again) == tilekit_to_json(kit));
}

void test_parse() {
    PaintedArea a;
    std::string err;
    CHECK(parse_area(R"({"format":"pagan-area","version":1,"theme":"cave","rows":["#.#","+~/"],"heights":["010"],
                        "pois":[{"x":1,"y":0,"note":"n","tags":["t"]}],"styles":{"wall":"ws149"}})", a, err));
    CHECK_EQ(a.w, 3);
    CHECK_EQ(a.h, 2);
    CHECK(a.kind(0, 1) == '+' && a.kind(1, 1) == '~' && a.kind(2, 1) == '/');
    CHECK_EQ(a.z(1, 0), 8);
    CHECK_EQ(a.z(1, 1), 0);
    CHECK(a.kind(5, 5) == ' ');
    CHECK(a.pois.size() == 1 && a.pois[0].tags.size() == 1);
    CHECK(a.styles["wall"] == "ws149");
    CHECK(!parse_area(R"({"format":"other","rows":[]})", a, err));
    CHECK(!parse_area(R"({"format":"pagan-area","cell":64,"rows":["."]})", a, err));
    CHECK(!parse_area("not json", a, err));
}

void check_area(const GameData& d, const TileKit& kit, const std::string& dir, const std::string& name,
                const std::string& shots) {
    PaintedArea a;
    std::string err;
    bool ok = parse_area(read_file(dir + "/" + name + ".area.json"), a, err);
    CHECK(ok);
    if (!ok) {
        std::printf("  %s: %s\n", name.c_str(), err.c_str());
        return;
    }
    DressResult r = dress(a, kit);
    const Validation& v = r.validation;
    std::printf("  %-16s %4zu items, floors %d/%d, wall gaps %d/%d, overlaps %d, unsupported %d, reach %d/%d, POIs",
                name.c_str(), r.items.size(), v.floor_cells - v.uncovered_cells, v.floor_cells, v.wall_gaps, v.wall_links,
                v.overlaps, v.unsupported, v.reachable, v.positions);
    for (const PoiResult& p : r.pois) std::printf(" %s=%s", p.id.c_str(), p.placed ? p.prefab.c_str() : "-");
    std::printf("\n");
    for (const std::string& p : v.problems) std::printf("    %s\n", p.c_str());
    CHECK(v.floor_covered);
    CHECK(v.walls_closed);
    bool walled = false;
    for (const PaintedCell& c : a.cells) walled |= c.kind == '#';
    CHECK(!walled || v.wall_links > 0);
    CHECK_EQ(v.overlaps, 0);
    CHECK_EQ(v.unsupported, 0);
    CHECK(v.walkable);
    CHECK(v.reachable * 10 > v.positions * 8);  // nearly every standing place is reachable
    CHECK(v.ok);
    for (const PoiResult& p : r.pois) {
        CHECK(p.placed);
        CHECK(!p.prefab.empty());  // a prefab matched (not just a marker)
    }
    // Doors are the original door classes, closed and unlocked.
    int doors = 0;
    for (const DressedItem& it : r.items)
        if (it.role == "door") {
            ++doors;
            CHECK(it.shape == 68 || it.shape == 69 || it.shape == 340 || it.shape == 342);
            CHECK_EQ(it.quality & 0x100, 0);
        }
    int want_doors = 0;
    for (const PaintedCell& c : a.cells) want_doors += c.kind == '+';
    CHECK_EQ(doors, want_doors);
    // Every item is inside the area and on the 32-unit grid like the original maps.
    for (const DressedItem& it : r.items) {
        CHECK(it.x >= 0 && it.y >= 0 && it.z >= 0 && it.z <= 255);
        if (it.role != "poi") CHECK((it.x & 31) == 31 && (it.y & 31) == 31);
    }
    // Deterministic per seed; a different seed gives a different dressing.
    std::string j1 = dressed_to_json(a, r);
    CHECK(j1 == dressed_to_json(a, dress(a, kit)));
    DressResult r2 = dress(a, kit, a.seed + 1);
    CHECK(dressed_to_json(a, r2) != j1);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(r.items.size(), r2.items.size()); ++i)
        differ += r.items[i].shape != r2.items[i].shape || r.items[i].frame != r2.items[i].frame;
    CHECK(differ > r.items.size() / 4);
    CHECK(r2.validation.ok);
    // No identical neighbouring floor tiles in a row (variety): at most a few per area.
    std::map<std::pair<int, int>, const DressedItem*> floor;
    for (const DressedItem& it : r.items)
        if (it.role == "floor") floor[{it.cx, it.cy}] = &it;
    int same = 0, pairs = 0;
    for (auto& [c, it] : floor) {
        auto e = floor.find({c.first + 1, c.second});
        if (e == floor.end() || e->second->z != it->z) continue;
        ++pairs;
        same += e->second->shape == it->shape && e->second->frame == it->frame;
    }
    CHECK(pairs == 0 || same * 5 <= pairs);
    // The JSON reads back for the viewer, and renders through the real sorter.
    std::vector<ItemRecord> recs;
    int origin[3];
    CHECK(parse_dressed(j1, recs, origin, err));
    CHECK_EQ(recs.size(), r.items.size());
    AreaImage img = render_items(d, to_records(r, false));
    CHECK(img.w > 200 && img.h > 150);
    size_t lit = 0;
    for (uint32_t p : img.rgba) lit += (p & 0xFFFFFF) != 0;
    CHECK(lit > img.rgba.size() / 5);
    if (!shots.empty()) {
        save_png(shots + "/dress_" + name + ".png", img.rgba.data(), img.w, img.h, 1);
        AreaImage img2 = render_items(d, to_records(r2, false));
        save_png(shots + "/dress_" + name + "_seed" + std::to_string(r2.seed) + ".png", img2.rgba.data(), img2.w, img2.h, 1);
        bool roofs = false;
        for (const DressedItem& it : r.items) roofs |= it.role == "roof";
        if (roofs) {
            AreaImage img3 = render_items(d, to_records(r, true));
            save_png(shots + "/dress_" + name + "_roofs.png", img3.rgba.data(), img3.w, img3.h, 1);
        }
    }
    // The viewer path: a CurrentMap holding the area at an offset.
    CurrentMap map(d);
    map.reset(254);
    std::vector<uint16_t> ids = place_items(map, recs, 8192, 8192, 0);
    CHECK_EQ(ids.size(), recs.size());
}

void test_walls(const TileKit& kit) {
    // A 4 x 3 room: posts at the four corners and next to the door, nothing in the open middle.
    PaintedArea a = make_area("tenebrae", {"####", "#..+", "####"});
    DressResult r = dress(a, kit);
    int posts = 0, walls = 0, doors = 0, jambs = 0;
    for (const DressedItem& it : r.items) {
        posts += it.role == "post";
        walls += it.role == "wall";
        doors += it.role == "door";
        jambs += it.role == "jamb";
    }
    CHECK(posts + jambs >= 5);
    CHECK(walls >= 4);
    CHECK_EQ(doors, 1);
    CHECK(jambs >= 1);
    CHECK(r.validation.walls_closed);
    // An entrance with no wall line is reported, not placed.
    PaintedArea b = make_area("cave", {"...", ".+.", "..."});
    DressResult rb = dress(b, kit);
    CHECK(!rb.warnings.empty());
    // A gap in a wall is found by the validator.
    DressResult rc = dress(a, kit);
    for (auto it = rc.items.begin(); it != rc.items.end(); ++it)
        if (it->role == "wall") {
            rc.items.erase(it);
            break;
        }
    Validation vc = validate(rc, a, kit);
    CHECK(!vc.walls_closed);
    // A room without any way in: its POI is unreachable.
    PaintedArea d = make_area("cave", {"#####", "#...#", "#####", ".....", "....."});
    d.pois.push_back({"p", 2, 1, "campfire camp", {"camp"}, "", 0});
    DressResult rd = dress(d, kit);
    CHECK(!rd.validation.walkable);
    // Heights: a drop of 48 gets a riser whose top is flush with the upper floor.
    PaintedArea e = make_area("cave", {"....", "...."}, {"6600", "6600"});
    DressResult re = dress(e, kit);
    int ledges = 0;
    for (const DressedItem& it : re.items)
        if (it.role == "ledge") {
            ++ledges;
            const ShapeEntry* s = kit.shape(it.shape);
            CHECK(s && it.z + s->fp[2] <= 48);
        }
    CHECK(ledges >= 2);
    // Styles override the theme's choices.
    PaintedArea f = make_area("tenebrae", {"####", "#..#", "####"});
    f.styles["wall"] = "ws149";
    DressResult rf = dress(f, kit);
    bool timber = false;
    for (const DressedItem& it : rf.items) timber |= it.role == "wall" && (it.shape == 149 || it.shape == 145);
    CHECK(timber);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    std::string kit_path = argc > 2 ? argv[2] : "data/tilekit.json";
    std::string areas = argc > 3 ? argv[3] : "tests/data/areas";
    std::string shots = argc > 4 ? argv[4] : "";
    if (!shots.empty()) mkdir(shots.c_str(), 0755);
    GameFS fs;
    GameData d;
    if (root.empty() || !fs.open(root) || !d.load(fs)) {
        std::printf("game folder not found\n");
        return 1;
    }
    test_parse();
    TileKit mined;
    test_mining(d, mined);
    // The dresser runs on the checked-in kit (what the engine ships); it must match a fresh mine.
    TileKit kit;
    std::string err;
    bool ok = load_tilekit(kit_path, kit, err);
    CHECK(ok);
    if (!ok) std::printf("  %s\n", err.c_str());
    else if (tilekit_to_json(kit) != tilekit_to_json(mined))
        std::printf("  note: %s differs from a fresh mine (edited by hand, or re-run pagan_tilekit)\n", kit_path.c_str());
    if (ok) {
        test_walls(kit);
        for (const char* n : {"cave_shrine", "tenebrae_house", "catacomb_graves", "beach_ledge"}) check_area(d, kit, areas, n, shots);
    }
    std::printf("dress: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
