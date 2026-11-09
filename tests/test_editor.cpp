// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// Edit mode (editor/area_model.*, editor/editor_storage.*, editor/edit_mode.*): brushes, fill,
// undo / redo, moving areas, z layers, the area and layout JSON, the storage backends, and the
// edit mode driven through its input with screenshots.
//   pagan_editor_tests GAME_DIR SHOTS_DIR TMP_DIR
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "core/bugfix.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "editor/area_model.h"
#include "editor/edit_mode.h"
#include "editor/editor_storage.h"
#include "platform/platform.h"
#include "core/variants.h"
#include "world/map_patches.h"
#include "world/world_layout.h"
#include "world/current_map.h"
#include "data/mapfile.h"
#include "world/item.h"
#include <tuple>

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string g_tmp = "editor_tmp", g_shots = "editor_shots", g_kit;

void rmrf(const std::string& p) { std::system(("rm -rf '" + p + "'").c_str()); }

std::string read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void test_brushes() {
    CHECK_EQ(brush_cells(BrushShape::kSquare, 1, 5, 5).size(), 1);
    CHECK(brush_cells(BrushShape::kSquare, 1, 5, 5)[0] == std::make_pair(5, 5));
    CHECK_EQ(brush_cells(BrushShape::kSquare, 2, 0, 0).size(), 4);
    CHECK_EQ(brush_cells(BrushShape::kSquare, 3, 0, 0).size(), 9);
    CHECK_EQ(brush_cells(BrushShape::kRound, 5, 0, 0).size(), 21);  // 5x5 without the corners
    CHECK_EQ(brush_cells(BrushShape::kRound, 2, 0, 0).size(), 4);   // small round = square
    // Even sizes extend to +x / +y.
    auto b2 = brush_cells(BrushShape::kSquare, 2, 0, 0);
    CHECK(std::find(b2.begin(), b2.end(), std::make_pair(1, 1)) != b2.end());
    CHECK(std::find(b2.begin(), b2.end(), std::make_pair(-1, -1)) == b2.end());

    auto l = line_cells(BrushShape::kSquare, 1, 0, 0, 5, 3);
    CHECK_EQ(l.size(), 6);
    CHECK(l.front() == std::make_pair(0, 0));
    CHECK(l.back() == std::make_pair(5, 3));
    CHECK_EQ(line_cells(BrushShape::kSquare, 1, 2, 2, 2, 2).size(), 1);
    CHECK_EQ(line_cells(BrushShape::kSquare, 3, 0, 0, 10, 0).size(), 13 * 3);  // no duplicates
    CHECK_EQ(rect_cells(0, 0, 4, 3, false).size(), 20);
    CHECK_EQ(rect_cells(4, 3, 0, 0, true).size(), 14);

    EditArea a;
    a.id = "t";
    PaintValue floor;
    floor.kind = cell::kFloor;
    floor.height = 2;
    CHECK_EQ(paint_cells(a, rect_cells(0, 0, 3, 3, false), floor), 16);
    CHECK_EQ(paint_cells(a, rect_cells(0, 0, 3, 3, false), floor), 0);  // unchanged
    CHECK_EQ(a.at(1, 1)->height, 2);
    PaintValue h;
    h.kind = 0;
    h.height = 5;
    CHECK_EQ(paint_cells(a, {{0, 0}, {9, 9}}, h), 1);  // height only never creates cells
    CHECK_EQ(a.at(0, 0)->height, 5);
    CHECK(!a.at(9, 9));
    PaintValue th;
    th.kind = 0;
    th.theme = std::string("lava");
    paint_cells(a, {{1, 0}}, th);
    CHECK(a.at(1, 0)->theme == "lava");
    a.pois.push_back({"p1", 2, 2, "camp", {}, "", 2});
    PaintValue erase;
    erase.kind = cell::kVoid;
    CHECK_EQ(paint_cells(a, {{2, 2}, {50, 50}}, erase), 1);
    CHECK(!a.at(2, 2));
    CHECK(a.pois.empty());  // erasing a cell removes its marker
    CHECK_EQ(a.cells.size(), 15);
}

void test_fill() {
    EditArea a;
    PaintValue wall;
    wall.kind = cell::kWall;
    wall.height = 0;
    paint_cells(a, rect_cells(0, 0, 7, 7, true), wall);  // a closed 8x8 wall ring
    PaintValue floor;
    floor.kind = cell::kFloor;
    floor.height = 0;
    CHECK_EQ(flood_fill(a, 3, 3, floor), 36);  // the 6x6 inside
    CHECK_EQ(a.kind(3, 3), cell::kFloor);
    CHECK_EQ(a.kind(0, 3), cell::kWall);
    CHECK_EQ(flood_fill(a, 3, 3, floor), 0);  // same value: nothing changes
    // Filling the walls (connected ring) with water.
    PaintValue water;
    water.kind = cell::kWater;
    water.height = 0;
    CHECK_EQ(flood_fill(a, 0, 0, water), 28);
    // Outside: void bounded by the painted bounds + 1 (a 10x10 frame of 36 cells).
    CHECK_EQ(flood_fill(a, -1, -1, floor, 1), 36);
    // Fill respects heights: a raised cell stops it.
    EditArea b;
    floor.height = 0;
    paint_cells(b, rect_cells(0, 0, 4, 0, false), floor);
    PaintValue up;
    up.kind = 0;
    up.height = 3;
    paint_cells(b, {{2, 0}}, up);
    PaintValue st;
    st.kind = cell::kStairs;
    st.height = -1;
    CHECK_EQ(flood_fill(b, 0, 0, st, 0), 2);
    CHECK_EQ(b.kind(3, 0), cell::kFloor);
}

void test_undo() {
    EditDoc d;
    int org[3] = {1024, 2048, 0};
    EditArea& a0 = d.create_area("Lithos cave", org, 0, "cave");
    CHECK(a0.id == "lithos_cave");
    CHECK_EQ(d.undo_depth(), 1);
    // A stroke: several paints, one undo step.
    d.begin("paint");
    d.touch_area("lithos_cave");
    PaintValue v;
    v.kind = cell::kFloor;
    v.height = 0;
    paint_cells(d.areas[0], brush_cells(BrushShape::kSquare, 3, 1, 1), v);
    paint_cells(d.areas[0], brush_cells(BrushShape::kSquare, 3, 4, 1), v);
    d.commit();
    CHECK_EQ(d.undo_depth(), 2);
    size_t n = d.areas[0].cells.size();
    CHECK_EQ(n, 18);
    // A change that changes nothing is not recorded.
    d.begin("noop");
    d.touch_area("lithos_cave");
    d.commit();
    CHECK_EQ(d.undo_depth(), 2);
    CHECK(d.undo());
    CHECK(d.areas[0].cells.empty());
    CHECK(d.redo());
    CHECK_EQ(d.areas[0].cells.size(), n);
    CHECK(!d.redo());
    // Undo the creation: the area goes; redo brings it back with its cells.
    CHECK(d.undo());
    CHECK(d.undo());
    CHECK(d.areas.empty());
    CHECK(!d.undo());
    CHECK(d.redo());
    CHECK(d.redo());
    CHECK_EQ(d.areas[0].cells.size(), n);
    // A new change clears the redo list.
    d.undo();
    d.rename_area("lithos_cave", "Cave of Lithos");
    CHECK(!d.can_redo());
    CHECK(d.areas[0].name == "Cave of Lithos");
    // Cancel restores.
    d.begin("x");
    d.touch_area("lithos_cave");
    d.areas[0].name = "oops";
    d.cancel();
    CHECK(d.areas[0].name == "Cave of Lithos");
    // Delete and undo.
    CHECK(d.delete_area("lithos_cave"));
    CHECK(d.find("lithos_cave") < 0);
    CHECK(d.undo());
    CHECK(d.find("lithos_cave") == 0);
    // The undo list is capped.
    for (int i = 0; i < int(EditDoc::kMaxUndo) + 20; ++i) d.move_area("lithos_cave", 128, 0, 0);
    CHECK_EQ(d.undo_depth(), EditDoc::kMaxUndo);
    // unique ids
    CHECK(d.unique_id("Cave of Lithos") == "cave_of_lithos");
    CHECK(d.create_area("Lithos cave", org, 0, "cave").id == "lithos_cave_2");  // the first kept its id when renamed
    CHECK(d.unique_id("Lithos cave") == "lithos_cave_3");
    CHECK(d.unique_id("!!!") == "area");
}

void test_move_layers() {
    EditDoc d;
    int org[3] = {0, 0, 0};
    d.create_area("A", org, 0, "cave");
    PaintValue v;
    v.kind = cell::kFloor;
    v.height = 1;
    d.begin("p");
    d.touch_area("a");
    paint_cells(d.areas[0], {{2, 3}}, v);
    d.commit();
    int x, y, z;
    d.areas[0].cell_world(2, 3, x, y, z);
    CHECK_EQ(x, 256);
    CHECK_EQ(y, 384);
    CHECK_EQ(z, 8);
    CHECK(d.move_area("a", 1280, -640, 0));
    d.areas[0].cell_world(2, 3, x, y, z);
    CHECK_EQ(x, 1536);
    CHECK_EQ(y, -256);
    int cx, cy;
    d.areas[0].world_cell(1536 + 5, -256 + 127, cx, cy);
    CHECK_EQ(cx, 2);
    CHECK_EQ(cy, 3);
    d.areas[0].world_cell(1279, -641, cx, cy);  // floor division below the origin
    CHECK_EQ(cx, -1);
    CHECK_EQ(cy, -1);
    CHECK(d.undo());
    CHECK_EQ(d.areas[0].origin[0], 0);
    CHECK(d.redo());
    // Layers and z offsets.
    CHECK(d.set_area_layer("a", -1));
    CHECK_EQ(d.areas[0].layer, -1);
    CHECK(d.move_area("a", 0, 0, -256));
    CHECK_EQ(d.areas[0].origin[2], -256);
    d.areas[0].cell_world(2, 3, x, y, z);
    CHECK_EQ(z, -256 + 8);
    d.undo();
    d.undo();
    CHECK_EQ(d.areas[0].layer, 0);
    CHECK_EQ(d.areas[0].origin[2], 0);
    // Map groups: layer and dz through the doc, undoable.
    AtlasGroup g;
    g.name = "map40";
    g.label = "Map 40";
    g.maps = {40};
    d.layout.atlas.groups.push_back(g);
    CHECK(d.move_group(0, 128, 256, -96));
    CHECK(d.set_group_layer(0, -2));
    CHECK_EQ(d.layout.atlas.groups[0].dz, -96 - 2 * 256);  // dz follows the layer (256 x layer + the height trim)
    CHECK_EQ(d.layout.atlas.groups[0].layer, -2);
    CHECK(d.layout.atlas.groups[0].derived.find("moved by hand") != std::string::npos);
    d.undo();
    CHECK_EQ(d.layout.atlas.groups[0].layer, 0);
    d.undo();
    CHECK_EQ(d.layout.atlas.groups[0].dx, 0);
    CHECK_EQ(d.layout.atlas.groups[0].dz, 0);
    d.redo();
    d.redo();
    // The world layout round trip keeps dz, layer, areas and unknown fields.
    d.sync_layout();
    d.layout.extra = R"({"seams": [{"a": 39, "b": 40}]})";
    std::string t = d.layout.to_json();
    WorldLayout w;
    std::string err;
    CHECK(w.from_json(t, err));
    CHECK_EQ(w.atlas.groups.size(), 1);
    CHECK_EQ(w.atlas.groups[0].dz, -608);
    CHECK_EQ(w.atlas.groups[0].layer, -2);
    CHECK_EQ(w.atlas.groups[0].dx, 128);
    CHECK_EQ(w.areas.size(), 1);
    CHECK(w.areas[0].id == "a");
    CHECK(w.extra.find("seams") != std::string::npos);
    CHECK(w.to_json() == t);
    // An atlas-only reader reads the world layout too.
    AtlasLayout al;
    CHECK(al.from_json(t, err));
    CHECK_EQ(al.groups[0].dz, -608);
    // Old atlas files: no dz / layer.
    AtlasLayout old;
    CHECK(old.from_json(R"({"groups": [{"name": "a", "maps": [3], "offset": [10, 20]}]})", err));
    CHECK_EQ(old.groups[0].dz, 0);
    CHECK_EQ(old.groups[0].layer, 0);
    CHECK(old.to_json().find("layer") == std::string::npos);  // unchanged output for flat layouts
    CHECK(!old.from_json(R"({"groups": [{"name": "a", "maps": [3], "offset": [1, 2, 3, 4]}]})", err));
    CHECK(!old.from_json(R"({"groups": [{"name": "a", "maps": [3], "layer": "x"}]})", err));
}

void test_json() {
    EditArea a;
    a.id = "shrine";
    a.name = "Lithos \"shrine\"";
    a.theme = "cave";
    a.seed = 77;
    a.origin[0] = 4096; a.origin[1] = 8192; a.origin[2] = -48;
    a.layer = -1;
    a.revision = 3;
    PaintValue v;
    v.kind = cell::kWall;
    v.height = 0;
    paint_cells(a, rect_cells(-2, -1, 5, 4, true), v);  // negative cells: normalized on save
    v.kind = cell::kFloor;
    v.height = 12;
    flood_fill(a, 0, 0, v, 0);
    a.set(1, 1, {cell::kWater, 12, ""});
    a.set(2, 2, {cell::kStairs, 13, "catacombs"});
    a.set(5, 1, {cell::kDoor, 0, ""});
    a.set(5, 2, {cell::kPassage, 0, "lava"});
    a.set(-2, 2, {cell::kChasm, 0, ""});
    a.pois.push_back({"p1", 0, 1, "shrine to Lithos, ruined", {"shrine", "earth"}, "", 2});
    a.pois.push_back({"p2", 3, 3, "", {}, "camp_1", 3});
    int x0, y0, x1, y1;
    a.bounds(x0, y0, x1, y1);
    CHECK_EQ(x0, -2);
    CHECK_EQ(y0, -1);
    std::string t = a.to_json();
    EditArea b;
    std::string err;
    CHECK(b.from_json(t, err));
    // Same world positions after normalisation.
    EditArea n = a;
    n.normalize();
    CHECK_EQ(n.origin[0], 4096 - 256);
    CHECK_EQ(n.origin[1], 8192 - 128);
    CHECK(b == n);
    CHECK(b.to_json() == t);
    CHECK_EQ(b.kind(0, 0), cell::kWall);
    CHECK_EQ(b.at(4, 3)->height, 13);
    CHECK(b.at(4, 3)->theme == "catacombs");
    CHECK(b.at(7, 3)->theme == "lava");
    CHECK_EQ(b.kind(0, 3), cell::kChasm);
    CHECK(b.pois[0].note == "shrine to Lithos, ruined");
    CHECK_EQ(b.pois[0].x, 2);
    CHECK(b.pois[1].prefab == "camp_1");
    CHECK_EQ(b.pois[1].radius, 3);
    CHECK_EQ(b.revision, 3);
    CHECK_EQ(b.layer, -1);
    CHECK(t.find("\"heights\"") != std::string::npos);
    CHECK(t.find("\"theme_legend\"") != std::string::npos);

    // The sample of docs/area_format.md reads.
    const char* sample = R"({
  "format": "pagan-area", "version": 1, "name": "Lithos cave", "theme": "cave", "seed": 1234, "cell": 128,
  "base_z": 0, "level_z": 8, "origin": [16384, 16384, 0],
  "rows": ["##########", "#........#", "#...~~...#", "#......../....", "####+#####"],
  "heights": ["0000000000", "0000000000", "0000000000", "00000000000000", "0000000000"],
  "pois": [{"id": "p1", "x": 4, "y": 1, "note": "shrine to Lithos, ruined", "tags": ["shrine", "earth"]}]
})";
    EditArea s;
    CHECK(s.from_json(sample, err));
    CHECK_EQ(s.cells.size(), 10 + 10 + 10 + 14 + 10);
    CHECK_EQ(s.kind(4, 4), cell::kDoor);
    CHECK_EQ(s.kind(9, 3), cell::kPassage);
    CHECK_EQ(s.kind(4, 2), cell::kWater);
    CHECK_EQ(s.origin[0], 16384);
    CHECK_EQ(s.seed, 1234);
    CHECK(s.pois[0].tags.size() == 2);
    // Its own output reads back the same.
    EditArea s2;
    CHECK(s2.from_json(s.to_json(), err));
    CHECK(s2 == s);
    // Errors.
    CHECK(!s2.from_json("[]", err));
    CHECK(!s2.from_json(R"({"format": "pagan-dressed", "version": 1, "rows": []})", err));
    CHECK(!s2.from_json(R"({"format": "pagan-area", "version": 1})", err));
    CHECK(!s2.from_json(R"({"format": "pagan-area", "version": 1, "rows": [], "pois": [{"note": 1}]})", err));
    // Height digits.
    CHECK_EQ(height_value(height_digit(35)), 35);
    CHECK_EQ(height_digit(10), 'a');

    // The bundle round trip (Download / Upload).
    EditDoc d;
    int org[3] = {0, 0, 0};
    d.create_area("One", org, 0, "cave");
    d.areas[0] = b;
    d.areas[0].id = "one";
    AtlasGroup g;
    g.name = "map3";
    g.label = "Map 3";
    g.maps = {3};
    g.dz = 64;
    g.layer = 1;
    d.layout.atlas.groups.push_back(g);
    std::string bundle = d.bundle_json();
    EditDoc e;
    CHECK(e.load_bundle(bundle, err));
    CHECK_EQ(e.areas.size(), 1);
    CHECK(e.areas[0] == d.areas[0]);
    CHECK_EQ(e.layout.atlas.groups[0].layer, 1);
    CHECK_EQ(e.layout.atlas.groups[0].dz, 64);
    CHECK(!e.can_undo());
    // A single area file imports as an extra area.
    EditArea extra;
    extra.id = "solo";
    extra.set(0, 0, {cell::kFloor, 0, ""});
    CHECK(e.load_bundle(extra.to_json(), err));
    CHECK_EQ(e.areas.size(), 2);
    CHECK(!e.load_bundle(R"({"format": "something"})", err));
}

void storage_suite(EditorStorage& st, const char* what) {
    std::printf("storage: %s\n", what);
    int writes = 0;
    st.on_write = [&writes]() { ++writes; };
    StorageResult last;
    auto keep = [&last](const StorageResult& r) { last = r; };
    st.list_areas(keep);
    CHECK(last.ok);
    CHECK(last.list.empty());
    st.load_area("nope", keep);
    CHECK(!last.ok);
    EditArea a;
    a.id = "cave_one";
    a.name = "Cave one";
    a.set(0, 0, {cell::kFloor, 0, ""});
    st.save_area("cave_one", a.to_json(), 0, keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 1);
    st.load_area("cave_one", keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 1);
    CHECK_EQ(read_revision(last.text), 1);
    EditArea b;
    std::string err;
    CHECK(b.from_json(last.text, err));
    CHECK_EQ(b.revision, 1);
    CHECK_EQ(b.kind(0, 0), cell::kFloor);
    // Version 2 from 1; then a stale save (based on 1) is a conflict; forcing works.
    a.set(1, 0, {cell::kWall, 0, ""});
    st.save_area("cave_one", a.to_json(), 1, keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 2);
    st.save_area("cave_one", a.to_json(), 1, keep);
    CHECK(!last.ok);
    CHECK(last.conflict);
    CHECK_EQ(last.version, 2);
    st.save_area("cave_one", a.to_json(), -1, keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 3);
    st.save_area("bad/id", a.to_json(), 0, keep);
    CHECK(!last.ok);
    st.save_area("../up", a.to_json(), 0, keep);
    CHECK(!last.ok);
    st.list_areas(keep);
    CHECK_EQ(last.list.size(), 1);
    if (!last.list.empty()) {
        CHECK(last.list[0].id == "cave_one");
        CHECK_EQ(last.list[0].version, 3);
        CHECK(last.list[0].name == "Cave one");
    }
    st.area_history("cave_one", keep);
    CHECK(last.ok);
    CHECK_EQ(last.list.size(), 3);
    if (last.list.size() == 3) CHECK_EQ(last.list[0].version, 3);  // newest first
    st.load_area_version("cave_one", 1, keep);
    CHECK(last.ok);
    CHECK(b.from_json(last.text, err));
    CHECK(!b.at(1, 0));
    // Layout.
    st.load_layout(keep);
    CHECK(!last.ok);
    WorldLayout w;
    AtlasGroup g;
    g.name = "map40";
    g.maps = {40};
    w.atlas.groups.push_back(g);
    st.save_layout(w.to_json(), 0, keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 1);
    st.save_layout(w.to_json(), 0, keep);
    CHECK(last.conflict);
    st.save_layout(w.to_json(), 1, keep);
    CHECK_EQ(last.version, 2);
    st.load_layout(keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 2);
    WorldLayout w2;
    CHECK(w2.from_json(last.text, err));
    CHECK_EQ(w2.revision, 2);
    st.layout_history(keep);
    CHECK_EQ(last.list.size(), 2);
    st.load_layout_version(1, keep);
    CHECK(last.ok);
    // Delete: stale base refused, then deleted; the history stays; a new save goes on counting.
    st.delete_area("cave_one", 1, keep);
    CHECK(last.conflict);
    st.delete_area("cave_one", 3, keep);
    CHECK(last.ok);
    st.load_area("cave_one", keep);
    CHECK(!last.ok);
    st.list_areas(keep);
    CHECK(last.list.empty());
    st.save_area("cave_one", a.to_json(), 0, keep);
    CHECK(last.ok);
    CHECK_EQ(last.version, 4);
    CHECK(writes >= 7);
}

void test_storage() {
    CHECK(stamp_revision(R"({"a": 1, "revision": 4, "b": 2})", 9) == R"({"a": 1, "revision": 9, "b": 2})");
    CHECK_EQ(read_revision(stamp_revision("{\n  \"a\": 1\n}", 5)), 5);
    CHECK_EQ(read_revision(stamp_revision("{}", 2)), 2);
    CHECK(valid_storage_id("cave_1-b"));
    CHECK(!valid_storage_id(""));
    CHECK(!valid_storage_id("_layout"));
    CHECK(!valid_storage_id("a.b"));
    MemoryStorage m;
    storage_suite(m, "memory");
    std::string dir = g_tmp + "/files";
    rmrf(dir);
    FileStorage f(dir + "/areas", dir + "/world_layout.json", dir + "/history");
    storage_suite(f, "files");
    CHECK(read_all(dir + "/areas/cave_one.json").find("\"revision\": 4") != std::string::npos);
    // History is trimmed to the newest kMaxHistory.
    EditArea a;
    a.id = "many";
    for (int i = 0; i < FileStorage::kMaxHistory + 5; ++i) {
        a.seed = uint32_t(i + 1);
        f.save_area("many", a.to_json(), -1, [](const StorageResult&) {});
    }
    StorageResult h;
    f.area_history("many", [&h](const StorageResult& r) { h = r; });
    CHECK_EQ(h.list.size(), FileStorage::kMaxHistory);
    if (!h.list.empty()) CHECK_EQ(h.list[0].version, FileStorage::kMaxHistory + 5);
}

// --- The edit mode through its input --------------------------------------------------------

struct Driver {
    EditMode& m;
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    explicit Driver(EditMode& em) : m(em) { frame(); }
    void frame() { m.tick(); m.render(px, w, h, 2); }
    InputEvent ev(InputEvent::Type t, double u, double v, int button = 1, unsigned mods = 0) {
        InputEvent e;
        e.type = t;
        e.fx = float(u / m.scale());
        e.fy = float(v / m.scale());
        e.x = int(e.fx);
        e.y = int(e.fy);
        e.button = button;
        e.mods = mods;
        return e;
    }
    void move(double u, double v) { m.handle(ev(InputEvent::kMouseMove, u, v)); }
    void down(double u, double v, int b = 1, unsigned mods = 0) { move(u, v); m.handle(ev(InputEvent::kMouseDown, u, v, b, mods)); }
    void up(double u, double v, int b = 1) { m.handle(ev(InputEvent::kMouseUp, u, v, b)); }
    void click(double u, double v, unsigned mods = 0) { down(u, v, 1, mods); up(u, v); frame(); }
    void drag(double u0, double v0, double u1, double v1, int steps = 8, unsigned mods = 0) {
        down(u0, v0, 1, mods);
        for (int i = 1; i <= steps; ++i) move(u0 + (u1 - u0) * i / steps, v0 + (v1 - v0) * i / steps);
        up(u1, v1);
        frame();
    }
    void key(int k, unsigned mods = 0) {
        InputEvent e;
        e.type = InputEvent::kKeyDown;
        e.key = k;
        e.mods = mods;
        m.handle(e);
        e.type = InputEvent::kKeyUp;
        m.handle(e);
    }
    void text(const std::string& s) {
        for (char c : s) {
            InputEvent e;
            e.type = InputEvent::kText;
            e.text[0] = c;
            m.handle(e);
        }
    }
    // Canvas point of a cell's centre of area a (at its height).
    void cell_px(const EditArea& a, int cx, int cy, double& u, double& v) {
        m.world_to_canvas(a.origin[0] + (cx + 0.5) * a.cell, a.origin[1] + (cy + 0.5) * a.cell, a.base_z + m.height * a.level_z, u, v);
    }
    void shot(const std::string& name) {
        m.set_budget(-1);
        frame();
        std::string p = g_shots + "/" + name;
        CHECK(save_png(p, px.data(), w, h, 1));
        std::printf("shot %s (%dx%d)\n", p.c_str(), w, h);
    }
};

void test_edit_mode(const GameData& data) {
    AtlasLayout layout = generate_atlas_layout(data);
    auto make = [&](EditorStorage* st) {
        auto m = std::make_unique<EditMode>(data);
        m->set_fallback_layout(layout);
        if (st) {
            // The storage is shared between edit mode instances: wrap it.
            struct Ref : EditorStorage {
                EditorStorage& s;
                explicit Ref(EditorStorage& x) : s(x) {}
                std::string name() const override { return s.name(); }
                void list_areas(Done d) override { s.list_areas(d); }
                void load_area(const std::string& i, Done d) override { s.load_area(i, d); }
                void save_area(const std::string& i, const std::string& j, int b, Done d) override { s.save_area(i, j, b, d); }
                void delete_area(const std::string& i, int b, Done d) override { s.delete_area(i, b, d); }
                void area_history(const std::string& i, Done d) override { s.area_history(i, d); }
                void load_area_version(const std::string& i, int v, Done d) override { s.load_area_version(i, v, d); }
                void load_layout(Done d) override { s.load_layout(d); }
                void save_layout(const std::string& j, int b, Done d) override { s.save_layout(j, b, d); }
                void layout_history(Done d) override { s.layout_history(d); }
                void load_layout_version(int v, Done d) override { s.load_layout_version(v, d); }
            };
            m->set_storage(std::make_unique<Ref>(*st));
        }
        m->set_budget(-1);
        CHECK(m->load_all());
        return m;
    };
    MemoryStorage store;
    auto mp = make(&store);
    EditMode& m = *mp;
    CHECK(m.loaded());
    CHECK(m.doc.areas.empty());
    CHECK_EQ(m.doc.layout.atlas.groups.size(), layout.groups.size());
    CHECK(!m.dirty());
    // Next to Tenebrae: the empty ground south of its coast (seen at 1:32 from the docks gate).
    m.show_map_point(40, 13038, 15042, 48, 5);
    Driver d(m);
    double wx, wy;
    m.canvas_to_world(380, 262, 0, wx, wy);
    int ox = int(wx) / 128 * 128, oy = int(wy) / 128 * 128;
    m.show_world_point(ox + 6 * 128, oy + 5 * 128, 2);
    d.frame();

    // A walled room drawn with the rectangle tool (walls: outline), then the floor filled.
    m.tool = EditMode::Tool::kRect;
    m.kind = cell::kWall;
    double u0, v0, u1, v1;
    m.world_to_canvas(ox + 64, oy + 64, 0, u0, v0);
    m.world_to_canvas(ox + 11 * 128 + 64, oy + 9 * 128 + 64, 0, u1, v1);
    d.drag(u0, v0, u1, v1);
    CHECK_EQ(m.doc.areas.size(), 1);
    CHECK(!m.selected.empty());
    EditArea* a = m.doc.find(m.selected) >= 0 ? &m.doc.areas[size_t(m.doc.find(m.selected))] : nullptr;
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->origin[0], ox);
    CHECK_EQ(a->origin[1], oy);
    CHECK_EQ(a->cells.size(), 2 * 12 + 2 * 8);  // a 12 x 10 outline
    CHECK_EQ(a->kind(0, 0), cell::kWall);
    CHECK_EQ(a->kind(5, 5), cell::kVoid);
    m.tool = EditMode::Tool::kFill;
    m.kind = cell::kFloor;
    d.cell_px(*a, 5, 5, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->kind(5, 5), cell::kFloor);
    CHECK_EQ(a->cells.size(), 12 * 10);
    // A door and a passage in the walls, a water pool with a round brush stroke.
    m.tool = EditMode::Tool::kBrush;
    m.kind = cell::kDoor;
    d.cell_px(*a, 5, 9, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->kind(5, 9), cell::kDoor);
    m.kind = cell::kPassage;
    d.cell_px(*a, 11, 4, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->kind(11, 4), cell::kPassage);
    m.kind = cell::kWater;
    m.brush = BrushShape::kRound;
    m.brush_size = 3;
    d.cell_px(*a, 3, 3, u0, v0);
    d.cell_px(*a, 4, 4, u1, v1);
    size_t depth = m.doc.undo_depth();
    d.drag(u0, v0, u1, v1, 6);
    CHECK_EQ(m.doc.undo_depth(), depth + 1);  // a stroke is one undo step
    CHECK_EQ(a->kind(3, 3), cell::kWater);
    CHECK_EQ(a->kind(4, 4), cell::kWater);
    CHECK_EQ(a->kind(8, 6), cell::kFloor);
    // A raised dais (height 2) with stairs, using the height paint.
    m.brush = BrushShape::kSquare;
    m.brush_size = 2;
    m.kind = EditMode::kHeightOnly;
    m.height = 2;
    d.cell_px(*a, 8, 6, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->at(8, 6)->height, 2);
    CHECK_EQ(a->at(9, 7)->height, 2);
    CHECK_EQ(a->kind(8, 6), cell::kFloor);
    m.height = 1;
    m.kind = cell::kStairs;
    m.brush_size = 1;
    d.cell_px(*a, 7, 6, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->kind(7, 6), cell::kStairs);
    CHECK_EQ(a->at(7, 6)->height, 1);
    m.height = 0;
    // A line of chasm outside the room, then erased again with the eraser.
    m.tool = EditMode::Tool::kLine;
    m.kind = cell::kChasm;
    d.cell_px(*a, 13, 0, u0, v0);
    d.cell_px(*a, 13, 9, u1, v1);
    d.drag(u0, v0, u1, v1);
    CHECK_EQ(a->kind(13, 5), cell::kChasm);
    m.tool = EditMode::Tool::kErase;
    d.cell_px(*a, 13, 5, u0, v0);
    d.click(u0, v0);
    CHECK_EQ(a->kind(13, 5), cell::kVoid);
    CHECK_EQ(a->kind(13, 4), cell::kChasm);
    // Undo / redo through the keys.
    d.key('z', kModCtrl);
    CHECK_EQ(a->kind(13, 5), cell::kChasm);
    d.key('y', kModCtrl);
    CHECK_EQ(a->kind(13, 5), cell::kVoid);
    d.key('z', kModCtrl);
    d.key('z', kModCtrl);
    CHECK_EQ(a->kind(13, 4), cell::kVoid);
    d.key('z', kModShift | kModCtrl);
    CHECK_EQ(a->kind(13, 4), cell::kChasm);

    // Points of interest: the marker tool opens a dialog; type the note and tags.
    m.tool = EditMode::Tool::kMarker;
    d.cell_px(*a, 9, 3, u0, v0);
    d.click(u0, v0);
    d.text("shrine to Lithos, ruined");
    d.key(kKeyTab);
    d.text("shrine, earth");
    d.key(kKeyReturn);
    CHECK_EQ(a->pois.size(), 1);
    if (!a->pois.empty()) {
        CHECK(a->pois[0].note == "shrine to Lithos, ruined");
        CHECK(a->pois[0].tags.size() == 2 && a->pois[0].tags[1] == "earth");
        CHECK_EQ(a->pois[0].x, 9);
        CHECK_EQ(a->pois[0].y, 3);
    }
    d.cell_px(*a, 2, 7, u0, v0);
    d.click(u0, v0);
    d.text("campfire camp of the fishermen");
    d.key(kKeyTab);
    d.text("camp, light");
    d.frame();
    d.shot("edit_dialog.png");
    d.key(kKeyReturn);
    CHECK_EQ(a->pois.size(), 2);
    // Editing a marker: click it again, change the note; Esc cancels a dialog.
    d.cell_px(*a, 2, 7, u0, v0);
    d.click(u0, v0);
    d.key(kKeyBackspace);
    d.key(kKeyEscape);
    CHECK(a->pois[1].note == "campfire camp of the fishermen");
    m.tool = EditMode::Tool::kBrush;
    m.kind = cell::kFloor;
    m.set_budget(-1);
    m.show_world_point(ox + 6 * 128, oy + 5 * 128, 2);
    d.move(-1000, -1000);
    d.shot("edit_tenebrae.png");
    // Close up with the marker labels.
    m.show_world_point(ox + 6 * 128, oy + 5 * 128, 0);
    d.move(50, 300);
    d.shot("edit_poi.png");

    // Dress: the dresser's items in place; re-roll (a new seed, undoable); back to the paint.
    m.set_tilekit_path(g_kit);
    if (m.dresser_available()) {
        m.show_world_point(ox + 6 * 128, oy + 5 * 128, 1);
        d.move(-1000, -1000);
        m.dress();
        CHECK(m.dressed());
        auto rep = m.dress_report();
        CHECK(rep.size() >= 3);
        if (!rep.empty()) CHECK(rep[0].find("Dresser checks") == 0);
        bool poi_lines = false;
        for (const std::string& l : rep) poi_lines |= l.find("marker p1") == 0;
        CHECK(poi_lines);
        CHECK(m.dressed_json().find("\"pagan-dressed\"") != std::string::npos);
        d.shot("edit_dressed.png");
        uint32_t seed0 = a->seed;
        m.reroll();
        a = &m.doc.areas[size_t(m.doc.find(m.selected))];
        CHECK(a->seed != seed0);
        CHECK(m.dressed());
        d.shot("edit_dressed_reroll.png");
        d.key('z', kModCtrl);
        a = &m.doc.areas[size_t(m.doc.find(m.selected))];
        CHECK_EQ(a->seed, seed0);
        d.frame();
        CHECK(m.dressed());  // re-dressed with the old seed
        m.dress();           // dressing twice from the code path re-dresses; the button toggles
        CHECK(m.dressed());
    } else {
        std::printf("dresser: no tile kit at %s, Dress not tested\n", g_kit.c_str());
    }

    // Rename through the panel; the theme.
    m.panel = true;
    d.frame();
    m.doc.rename_area(a->id, "Lithos shrine");
    a = &m.doc.areas[size_t(m.doc.find(m.selected))];
    m.doc.set_area_theme(a->id, "catacombs");
    CHECK(a->name == "Lithos shrine");
    m.panel = false;

    // Save to the storage; a second edit mode loads the same thing.
    CHECK(m.dirty());
    m.save();
    CHECK(!m.dirty());
    StorageResult r;
    store.list_areas([&r](const StorageResult& x) { r = x; });
    CHECK_EQ(r.list.size(), 1);
    store.load_layout([&r](const StorageResult& x) { r = x; });
    CHECK(r.ok);
    CHECK(r.text.find("\"areas\"") != std::string::npos);
    {
        auto m2 = make(&store);
        CHECK_EQ(m2->doc.areas.size(), 1);
        if (m2->doc.areas.size() == 1) {
            EditArea b = m2->doc.areas[0];
            EditArea c = *a;
            c.normalize();
            b.revision = c.revision = 0;
            CHECK(b == c);
            CHECK(!m2->dirty());
        }
        // A second editor saves first: the first one's next save is a conflict, then forced.
        m2->doc.rename_area(m2->doc.areas[0].id, "Renamed elsewhere");
        m2->save();
        m.doc.set_area_seed(a->id, 99);
        m.save();
        CHECK(m.message.find("again") != std::string::npos);
        CHECK(m.dirty());
        m.save();  // the second Save overwrites
        CHECK(!m.dirty());
        store.load_area(a->id, [&r](const StorageResult& x) { r = x; });
        EditArea s;
        std::string err;
        CHECK(s.from_json(r.text, err));
        CHECK_EQ(s.seed, 99);
    }

    // Move the area with the Move tool (snapped to 128).
    m.show_world_point(ox + 6 * 128, oy + 5 * 128, 2);
    d.frame();
    a = &m.doc.areas[size_t(m.doc.find(m.selected))];
    int before[2] = {a->origin[0], a->origin[1]};
    m.tool = EditMode::Tool::kMove;
    d.cell_px(*a, 5, 5, u0, v0);
    double du, dv;
    m.world_to_canvas(a->origin[0] + 5.5 * 128 + 512, a->origin[1] + 5.5 * 128, 0, du, dv);
    d.drag(u0, v0, du, dv, 10);
    a = &m.doc.areas[size_t(m.doc.find(m.selected))];
    CHECK_EQ(a->origin[0], before[0] + 512);
    CHECK_EQ(a->origin[1], before[1]);
    d.key('z', kModCtrl);
    a = &m.doc.areas[size_t(m.doc.find(m.selected))];
    CHECK_EQ(a->origin[0], before[0]);

    // Layers: a second area one layer below, under the first (the levels ghost each other).
    m.tool = EditMode::Tool::kRect;
    m.kind = cell::kFloor;
    m.layer = -1;
    m.selected.clear();
    m.world_to_canvas(ox - 3 * 128 + 64, oy + 2 * 128 + 64, 0, u0, v0);
    m.world_to_canvas(ox + 8 * 128 + 64, oy + 13 * 128 + 64, 0, u1, v1);
    d.drag(u0, v0, u1, v1);
    CHECK_EQ(m.doc.areas.size(), 2);
    EditArea* low = &m.doc.areas.back();
    CHECK_EQ(low->layer, -1);
    m.kind = cell::kWall;
    d.drag(u0, v0, u1, v1);
    m.doc.rename_area(low->id, "Catacombs below");
    low = &m.doc.areas.back();
    CHECK(m.doc.move_area(low->id, 0, 0, -256));
    // The map group of Tenebrae can move a layer up and back.
    int g40 = m.doc.layout.atlas.group_of(40);
    CHECK(g40 >= 0);
    CHECK(m.doc.set_group_layer(g40, 1));
    CHECK(m.doc.undo());
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g40)].layer, 0);
    m.layer = 0;
    m.select_area(m.doc.areas[0].id);
    m.show_world_point(ox + 3 * 128, oy + 7 * 128, 3);
    d.move(-1000, -1000);
    d.shot("edit_layers.png");
    m.layer = -1;
    m.select_area(low->id);
    d.shot("edit_layer_below.png");
    m.layer = 0;

    // The controller: A paints at the virtual pointer (left stick).
    m.tool = EditMode::Tool::kBrush;
    m.kind = cell::kFloor;
    m.select_area(m.doc.areas[0].id);
    a = &m.doc.areas[0];
    d.cell_px(*a, 15, 2, u0, v0);
    d.move(u0, v0);
    InputEvent pe;
    pe.type = InputEvent::kPadDown;
    pe.key = kPadA;
    m.handle(pe);
    pe.type = InputEvent::kPadUp;
    m.handle(pe);
    CHECK_EQ(a->kind(15, 2), cell::kFloor);
    pe.type = InputEvent::kPadDown;
    pe.key = kPadX;  // undo
    m.handle(pe);
    CHECK_EQ(a->kind(15, 2), cell::kVoid);

    // Toolbar buttons are clickable: "Wall" in the palette, then "Undo".
    m.panel = false;
    d.frame();
    m.kind = cell::kFloor;
    // Find the Wall button by scanning the top bar for a click that changes the kind.
    bool found = false;
    for (int u = 2; u < 300 && !found; u += 4) {
        d.click(u, 12);
        if (m.kind == cell::kWall) found = true;
    }
    CHECK(found);
    // Help and Esc.
    d.key(kKeyF1);
    CHECK(m.help);
    d.key(kKeyEscape);
    CHECK(!m.help);
    // Esc with unsaved changes asks; Discard leaves.
    m.set_from_game(true);
    CHECK(m.dirty());
    d.key(kKeyEscape);
    CHECK(!m.take_exit());
    d.key(kKeyEscape);  // closes the question
    m.save();
    CHECK(m.message.find("again") != std::string::npos);  // the other editor saved the layout meanwhile
    m.save();
    CHECK(!m.dirty());
    d.key(kKeyEscape);
    CHECK(m.take_exit());

    // The bundle (Download / Upload) round trip through import_text.
    std::string bundle = m.doc.bundle_json();
    auto m3 = make(nullptr);
    CHECK(m3->import_text(bundle));
    CHECK_EQ(m3->doc.areas.size(), m.doc.areas.size());
    CHECK(m3->dirty());
}

// Segments: every map drawn whole (nothing excluded, the loaded items = the map files' items),
// grab-and-drag one segment (only it), a seam broken and re-joined, the snap grid, the resets.
void test_segments(const GameData& data, EditMode& m, Driver& d) {
    std::printf("segments\n");
    // The two restore points: Pagan (the original placement) and Baseline (frozen 2026-11-06).
    m.set_reference_layout(EditMode::kPagan, read_text(g_kit.substr(0, g_kit.find_last_of('/')) + "/atlas_layout.json"));
    m.set_reference_layout(EditMode::kBaseline, read_text(g_kit.substr(0, g_kit.find_last_of('/')) + "/world_layout.baseline.json"));
    CHECK(m.has_reference(EditMode::kPagan) && m.has_reference(EditMode::kBaseline));
    CHECK(!m.has_reference(0));
    // 1. Complete: no map of any segment has a chunk left out, and what a map loads (what the
    // editor's tiles render) is exactly its FIXED + NONFIXED records (plus the documented data
    // fixes: specs/world/items.md 9 / world/bugfixes.md 12, and with "Original bug fixes" On the
    // engine's further ones, NOTES.md "Original bug fixes"; they only add tiles or move some).
    // The documented fixes: records added per map, and 14 records moved (maps 5, 21 and 49).
    // Map 57: the "Underworld entrance" group (NOTES.md "The buried cave mouth": new content, not a
    // fix), the records of data/patches/map_content.json and its moves of two nonfixed items.
    const map_patches::Group* cave_mouth = map_patches::find("underworld_entrance");
    CHECK(cave_mouth != nullptr);
    std::map<unsigned, size_t> added_by_fixes = {{7, 3}, {50, 1}, {62, 28 + (original_bug_fixes() ? 6 : 0)}};
    if (cave_mouth && underworld_entrance() && !underworld_breach_open()) added_by_fixes[cave_mouth->map] = cave_mouth->items.size();
    size_t excluded = 0, maps = 0, items = 0, fix_changed = 0;
    bool same = true, added_ok = true;
    for (const AtlasGroup& g : m.doc.layout.atlas.groups)
        for (unsigned mp : g.maps) {
            excluded += m.tiles().excluded_chunks(mp);
            ++maps;
            std::vector<ItemRecord> raw = parse_item_records(data.fixed.entry(mp));
            std::vector<ItemRecord> fixed = raw;
            apply_map_fixes(mp, fixed);
            {
                auto a = added_by_fixes.find(mp);
                size_t want_added = a == added_by_fixes.end() ? 0 : a->second;
                if (fixed.size() - raw.size() != want_added) {
                    added_ok = false;
                    std::printf("  map %u: the data fixes add %zu records, documented %zu\n", mp, fixed.size() - raw.size(), want_added);
                }
            }
            std::multiset<std::tuple<int, int, int, int, int>> want, have, rawset;
            auto key = [](const ItemRecord& r) { return std::make_tuple(int(r.shape), int(r.frame), int(r.x), int(r.y), int(r.z)); };
            for (const ItemRecord& r : fixed) want.insert(key(r));
            for (const ItemRecord& r : raw) rawset.insert(key(r));
            std::vector<ItemRecord> nonfixed = parse_item_records(data.nonfixed.entry(mp));
            for (const MapEntry& e : build_item_tree(nonfixed, data.typeflags))
                if (e.parent < 0) rawset.insert(key(e.rec));
            map_patches::apply_moves(mp, nonfixed);
            for (const MapEntry& e : build_item_tree(nonfixed, data.typeflags))
                if (e.parent < 0) want.insert(key(e.rec));
            // (fixed containers' contents are not top level either)
            std::multiset<std::tuple<int, int, int, int, int>> want_top;
            for (const MapEntry& e : build_item_tree(fixed, data.typeflags))
                if (e.parent < 0) want_top.insert(key(e.rec));
            for (const MapEntry& e : build_item_tree(nonfixed, data.typeflags))
                if (e.parent < 0) want_top.insert(key(e.rec));
            CurrentMap cm(data);
            cm.load(mp);
            for (int cy = 0; cy < 64; ++cy)
                for (int cx = 0; cx < 64; ++cx)
                    for (uint16_t id : cm.chunk(cx, cy)) {
                        const Item& it = cm.item(id);
                        if (it.origin == Item::kFixed || it.origin == Item::kNonFixed)
                            have.insert(std::make_tuple(int(it.shape), int(it.frame), int(it.x), int(it.y), int(it.z)));
                    }
            if (have != want_top) {
                same = false;
                std::printf("  map %u: loaded %zu top-level items, the files give %zu\n", mp, have.size(), want_top.size());
            }
            items += have.size();
            for (const auto& k : rawset) fix_changed += want.count(k) == 0;
        }
    std::printf("  %zu maps in %zu segments: %zu items loaded = the map files' items (+ data fixes); %zu file records changed by "
                "the data fixes (z lifts); %zu chunks excluded\n",
                maps, m.doc.layout.atlas.groups.size(), items, fix_changed, excluded);
    CHECK(same);
    CHECK_EQ(excluded, 0);
    CHECK(added_ok);
    // Maps 5, 21 and 49: tiles the spec moves into place; map 57: the moved items of the cave mouth.
    CHECK_EQ(fix_changed, 14 + (cave_mouth && underworld_entrance() ? cave_mouth->moves.size() : 0));
    // 2. Grab and move: the Segments tool is the default; hovering names the segment.
    EditMode fresh(data);
    CHECK(fresh.tool == EditMode::Tool::kSegments);
    CHECK(!fresh.move_joined);
    CHECK_EQ(fresh.snap_grid, 512);
    int g7 = m.doc.layout.atlas.group_of(7), g40 = m.doc.layout.atlas.group_of(40), g8 = m.doc.layout.atlas.group_of(8);
    m.layer = 0;
    m.tool = EditMode::Tool::kSegments;
    m.select_group(g8);
    d.move(m.canvas_w() / 2.0, m.canvas_h() / 2.0);
    d.frame();
    CHECK_EQ(m.hover_group(), g8);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_hover.png", d.px.data(), d.w, d.h, 1);
    // Valley pulled away from the city: only it moves, the 7/40 seam breaks (a teleport again).
    auto walk = [&](unsigned a, unsigned b) {
        for (const auto& s2 : m.seams())
            if ((s2.a == a && s2.b == b) || (s2.a == b && s2.b == a)) return s2.walk ? 1 : 0;
        return -1;
    };
    CHECK_EQ(walk(7, 40), 1);
    int x7 = m.doc.layout.atlas.groups[size_t(g7)].dx, x40 = m.doc.layout.atlas.groups[size_t(g40)].dx;
    std::string msg = m.drag_group_by(g7, 6000, -6000);
    std::printf("  %s\n", msg.c_str());
    CHECK(m.doc.layout.atlas.groups[size_t(g7)].dx != x7);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g40)].dx, x40);
    CHECK_EQ(walk(7, 40), 0);
    CHECK(msg.find("broken") != std::string::npos);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx % 512, 0);  // the 512 grid
    {
        WorldLayoutFile f;
        std::string err;
        CHECK(f.from_json(m.doc.layout.to_json(), err));
        CHECK(!f.seam(7, 40));  // the game: map 7's gate is a teleporter again
        CHECK(f.seam(5, 6));
    }
    m.select_group(g7);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_moved.png", d.px.data(), d.w, d.h, 1);
    // Dragged back near its old place: it snaps onto the exact seam offset and re-joins.
    msg = m.drag_group_by(g7, double(x7 - m.doc.layout.atlas.groups[size_t(g7)].dx) + 150,
                          double(-24057 - m.doc.layout.atlas.groups[size_t(g7)].dy) - 120);
    std::printf("  %s\n", msg.c_str());
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx, x7);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dy, -24057);
    CHECK_EQ(walk(7, 40), 1);
    CHECK(msg.find("re-joined") != std::string::npos);
    // Undo twice: broken again, then back to the start (the seam mode is undone with the move).
    CHECK(m.doc.undo());
    CHECK_EQ(walk(7, 40), 0);
    CHECK(m.doc.undo());
    CHECK_EQ(walk(7, 40), 1);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx, x7);
    // Joined: the whole Tenebrae cluster comes along and no seam breaks.
    m.move_joined = true;
    msg = m.drag_group_by(g7, 2048, 0);
    CHECK(msg.find("broken") == std::string::npos);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g40)].dx - x40, m.doc.layout.atlas.groups[size_t(g7)].dx - x7);  // together (snapped)
    CHECK(m.doc.layout.atlas.groups[size_t(g40)].dx != x40);
    CHECK(m.doc.undo());
    m.move_joined = false;
    // Snap sizes, and a drag with the mouse in the Segments tool (the plateau).
    m.snap_grid = 256;
    int px8 = m.doc.layout.atlas.groups[size_t(g8)].dx;
    m.select_group(g8);
    d.drag(m.canvas_w() / 2.0, m.canvas_h() / 2.0, m.canvas_w() / 2.0 + 60, m.canvas_h() / 2.0 + 10, 6);
    CHECK(m.doc.layout.atlas.groups[size_t(g8)].dx != px8);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx % 256, 0);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dy % 256, 0);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_grid.png", d.px.data(), d.w, d.h, 1);
    m.snap_grid = 512;
    // Resets: the plateau to the Baseline, then everything to the Pagan spots and back (one undo
    // step each).
    CHECK(m.reset_group(g8, EditMode::kBaseline));
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx, px8);
    CHECK(m.reset_all(EditMode::kPagan));
    AtlasLayout atlas;
    std::string err;
    CHECK(atlas.from_json(read_text(g_kit.substr(0, g_kit.find_last_of('/')) + "/atlas_layout.json"), err));
    int a8 = atlas.group_of(8);
    CHECK(a8 >= 0 && m.doc.layout.atlas.groups[size_t(g8)].dx == atlas.groups[size_t(a8)].dx);
    CHECK_EQ(walk(7, 40), 0);  // the valley's atlas spot is not the seam's offset
    CHECK(!m.overlapping_groups().empty());  // and they overlap: marked, allowed
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_reset_pagan.png", d.px.data(), d.w, d.h, 1);
    CHECK(m.doc.undo());
    CHECK_EQ(walk(7, 40), 1);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx, px8);
    // A selection of several segments: map 7 moved, map 37 a layer down, the plateau moved;
    // Shift+click picks 7 and 37, Reset to Baseline puts just those two back (one undo step).
    int g37 = m.doc.layout.atlas.group_of(37);
    int x37 = m.doc.layout.atlas.groups[size_t(g37)].dx;
    m.drag_group_by(g7, 4096, 0);
    m.drag_group_by(g37, 4096, 0);
    m.drag_group_by(g8, -4096, 0);
    int moved8 = m.doc.layout.atlas.groups[size_t(g8)].dx;
    m.select_group(g7);
    {
        double u, v;
        const AtlasGroup& G = m.doc.layout.atlas.groups[size_t(g37)];
        const MapFootprint* f37 = nullptr;
        std::vector<MapFootprint> fps = compute_footprints(data);
        for (const MapFootprint& f : fps) if (f.map == 37) f37 = &f;
        CHECK(f37 != nullptr);
        if (f37) {
            m.show_world_point(G.dx + (f37->cx0 + f37->cx1) * 256, G.dy + (f37->cy0 + f37->cy1) * 256, 6);
            m.world_to_canvas(G.dx + (f37->cx0 + f37->cx1) * 256, G.dy + (f37->cy0 + f37->cy1) * 256, f37->median_z, u, v);
            m.tool = EditMode::Tool::kSegments;
            d.click(u, v, kModShift);
        }
    }
    CHECK_EQ(m.selection().size(), 2);
    CHECK(m.reset_selection(EditMode::kBaseline));
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx, x7);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].dx, x37);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx, moved8);  // not selected: left alone
    CHECK_EQ(walk(7, 40), 1);
    CHECK(m.doc.undo());
    CHECK(m.doc.layout.atlas.groups[size_t(g7)].dx != x7);
    CHECK(m.doc.redo());
    // A box: everything it touches is selected (the whole view: every segment drawn there).
    m.show_world_point(0, 0, 9);
    int inbox = m.select_box(0, 0, m.canvas_w(), m.canvas_h(), false);
    std::printf("  box over the whole view: %d segments\n", inbox);
    CHECK(inbox >= 5);
    CHECK_EQ(int(m.selection().size()), inbox);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_selection.png", d.px.data(), d.w, d.h, 1);
    // Reset all to Baseline: places, layers, z, seams and links exactly as frozen.
    CHECK(m.reset_all(EditMode::kBaseline));
    {
        WorldLayoutFile a, b;
        std::string e1;
        CHECK(a.from_json(m.doc.layout.to_json(), e1));
        CHECK(b.from_json(read_text(g_kit.substr(0, g_kit.find_last_of('/')) + "/world_layout.baseline.json"), e1));
        bool eq = a.atlas.groups.size() == b.atlas.groups.size() && a.seams.size() == b.seams.size() && a.links.size() == b.links.size();
        for (size_t i = 0; eq && i < a.atlas.groups.size(); ++i)
            eq = a.atlas.groups[i].dx == b.atlas.groups[i].dx && a.atlas.groups[i].dy == b.atlas.groups[i].dy &&
                 a.atlas.groups[i].dz == b.atlas.groups[i].dz && a.atlas.groups[i].layer == b.atlas.groups[i].layer;
        CHECK(eq);
    }
    CHECK(m.doc.undo());
    CHECK(m.doc.redo());
    // The spots views (read only): Pagan, Baseline.
    m.multi.clear();
    m.show_spots = EditMode::kPagan;
    m.select_group(g40);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_pagan_spots.png", d.px.data(), d.w, d.h, 1);
    int before = m.doc.layout.atlas.groups[size_t(g8)].dx;
    d.drag(m.canvas_w() / 2.0, m.canvas_h() / 2.0, m.canvas_w() / 2.0 + 60, m.canvas_h() / 2.0, 4);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx, before);
    m.show_spots = EditMode::kBaseline;
    d.frame();
    m.show_spots = 0;
    // The Layers panel with the reset buttons.
    m.layers_panel = true;
    m.show_world_point(0, 0, 5);
    m.select_group(g7);
    m.multi.insert(m.doc.layout.atlas.group_of(6));  // two selected: "Reset 2 to ..."
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) save_png(g_shots + "/editor_segments_panel.png", d.px.data(), d.w, d.h, 1);
}

// The layered world in the editor: a stale stored layout replaced by the shipped one, the
// Layers panel, selecting / nudging / re-layering map groups (with their seam partners), undo.
void test_layers(const GameData& data) {
    std::printf("layers\n");
    std::string dir = g_kit.substr(0, g_kit.find_last_of('/'));
    std::ifstream lf(dir + "/world_layout.json");
    std::stringstream ss;
    ss << lf.rdbuf();
    std::string shipped = ss.str();
    CHECK(!shipped.empty());
    // The live server had an atlas-only layout from before the stitched world (no layers, no
    // seams): the editor must not keep showing it.
    MemoryStorage store, seed;
    AtlasLayout old = generate_atlas_layout(data);
    WorldLayout ow;
    ow.atlas = old;
    int ver = 0;
    store.save_layout(ow.to_json(), 0, [&](const StorageResult& r) { ver = r.version; });
    {
        EditArea kept;
        kept.id = "area_x";
        kept.name = "Kept";
        store.save_area("area_x", kept.to_json(), 0, [](const StorageResult&) {});
    }
    seed.save_layout(shipped, 0, [](const StorageResult&) {});
    EditMode m(data);
    struct Ref : EditorStorage {
        EditorStorage& s;
        explicit Ref(EditorStorage& x) : s(x) {}
        std::string name() const override { return s.name(); }
        void list_areas(Done d) override { s.list_areas(d); }
        void load_area(const std::string& i, Done d) override { s.load_area(i, d); }
        void save_area(const std::string& i, const std::string& j, int b, Done d) override { s.save_area(i, j, b, d); }
        void delete_area(const std::string& i, int b, Done d) override { s.delete_area(i, b, d); }
        void area_history(const std::string& i, Done d) override { s.area_history(i, d); }
        void load_area_version(const std::string& i, int v, Done d) override { s.load_area_version(i, v, d); }
        void load_layout(Done d) override { s.load_layout(d); }
        void save_layout(const std::string& j, int b, Done d) override { s.save_layout(j, b, d); }
        void layout_history(Done d) override { s.layout_history(d); }
        void load_layout_version(int v, Done d) override { s.load_layout_version(v, d); }
    };
    m.set_storage(std::make_unique<Ref>(store));
    m.set_seed_storage(std::make_unique<Ref>(seed));
    m.set_budget(-1);
    CHECK(m.load_all());
    CHECK(m.replaced_stale_layout());
    CHECK(m.dirty());
    CHECK(m.doc.layout.extra.find("\"seams\"") != std::string::npos);
    CHECK_EQ(m.doc.areas.size(), 1);  // the painted areas are kept
    std::vector<int> ls = m.layers_in_use();
    CHECK(ls.size() >= 4);
    CHECK(!m.layer_name(0).empty());
    CHECK(m.layout_problems().empty());
    // Herdsman's Valley: one seam cluster with the city, the docks, the reef, the road and the cemetery.
    int g7 = m.doc.layout.atlas.group_of(7), g8 = m.doc.layout.atlas.group_of(8), g37 = m.doc.layout.atlas.group_of(37);
    CHECK(g7 >= 0 && g8 >= 0 && g37 >= 0);
    CHECK_EQ(m.seam_cluster(g7).size(), 8);
    CHECK_EQ(m.seam_cluster(g8).size(), 1);
    Driver d(m);
    d.key('y');
    CHECK(m.layers_panel);
    m.select_group(g7);
    CHECK_EQ(m.layer, 0);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) CHECK(save_png(g_shots + "/layout_editor_layers.png", d.px.data(), d.w, d.h, 1));
    // Nudging the cluster keeps every seam exact (no problem but overlaps can appear).
    std::vector<int> before;
    for (const AtlasGroup& g : m.doc.layout.atlas.groups) before.push_back(g.dx);
    m.move_joined = true;        // (off by default: only the grabbed segment moves)
    d.key(kKeyRight, kModCtrl);  // 512 right on screen: +x -y
    int moved = 0;
    for (size_t i = 0; i < before.size(); ++i) moved += m.doc.layout.atlas.groups[i].dx != before[i];
    CHECK_EQ(moved, 8);
    {
        WorldLayoutFile f;
        std::string err;
        CHECK(f.from_json(m.doc.layout.to_json(), err));
        bool seam_ok = true;
        for (const std::string& p : check_world_layout(f, compute_footprints(data))) seam_ok &= p.find("seam") == std::string::npos;
        CHECK(seam_ok);
    }
    d.key('z', kModCtrl);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx, before[size_t(g7)]);
    d.key('z', kModCtrl | kModShift);  // redo
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g7)].dx, before[size_t(g7)] + 512);
    d.key('z', kModCtrl);
    m.move_joined = false;
    // Argentrock a layer down and back: dz follows (one undo step each).
    m.select_group(g37);
    int dz0 = m.doc.layout.atlas.groups[size_t(g37)].dz;
    d.key(kKeyPageDown, kModCtrl);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].layer, -1);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].dz, dz0 - 256);
    CHECK_EQ(m.layer, -1);
    m.render(d.px, d.w, d.h, 4);
    if (!g_shots.empty()) CHECK(save_png(g_shots + "/layout_editor_moved_down.png", d.px.data(), d.w, d.h, 1));
    d.key('y', kModCtrl);  // redo: nothing
    d.key('z', kModCtrl);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].layer, 0);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].dz, dz0);
    d.key('y', kModCtrl);
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g37)].layer, -1);
    d.key('z', kModCtrl);
    // The plateau dragged with the Move tool (one undo step), the toolbar's Undo button.
    m.select_group(g8);
    m.tool = EditMode::Tool::kMove;
    int dx8 = m.doc.layout.atlas.groups[size_t(g8)].dx;
    d.drag(m.canvas_w() / 2.0, m.canvas_h() / 2.0, m.canvas_w() / 2.0 + 40, m.canvas_h() / 2.0, 6);
    CHECK(m.doc.layout.atlas.groups[size_t(g8)].dx != dx8);
    CHECK(m.doc.can_undo());
    CHECK(m.doc.undo());
    CHECK_EQ(m.doc.layout.atlas.groups[size_t(g8)].dx, dx8);
    // Hide / show a layer.
    m.hidden_layers.insert(-1);
    d.frame();
    m.hidden_layers.clear();
    // Saving stores the new layout (with its layers) in the storage.
    m.save();
    std::string saved;
    store.load_layout([&](const StorageResult& r) { saved = r.text; });
    CHECK(saved.find("\"layout_revision\": 2") != std::string::npos || saved.find("\"layout_revision\":2") != std::string::npos);
    test_segments(data, m, d);
    // A second load from the same storage: not stale any more.
    EditMode m2(data);
    m2.set_storage(std::make_unique<Ref>(store));
    m2.set_seed_storage(std::make_unique<Ref>(seed));
    m2.set_budget(-1);
    CHECK(m2.load_all());
    CHECK(!m2.replaced_stale_layout());
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    if (argc > 2) g_shots = argv[2];
    if (argc > 3) g_tmp = argv[3];
    if (argc > 4) g_kit = argv[4];
    mkdir(g_shots.c_str(), 0755);
    mkdir(g_tmp.c_str(), 0755);
    test_brushes();
    test_fill();
    test_undo();
    test_move_layers();
    test_json();
    test_storage();
    GameFS fs;
    if (root.empty() || !fs.open(root)) {
        std::printf("game folder not found\n");
        return 1;
    }
    GameData data;
    bool ok = data.load(fs);
    CHECK(ok);
    if (ok) test_edit_mode(data);
    if (ok) test_layers(data);
    std::printf("editor: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
