// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Data tests against the original v2.10 files: every shape frame decodes, every map loads,
// and a set of facts from specs/ holds. Usage: pagan_tests [game-dir]
#include <cstdio>
#include <cstring>
#include <string>

#include "core/gamefs.h"
#include "data/gamedata.h"
#include "gfx/font.h"
#include "gfx/surface.h"
#include "world/current_map.h"
#include "world/item_sorter.h"
#include "world/world_view.h"
#include "check.h"

void test_kernel();
void test_vm();
void test_usecode_data(const pg::GameData& d);
void test_game(const pg::GameData& d);
void test_milestone4(const pg::GameData& d, const std::string& tmp);
void test_milestone5(const pg::GameData& d, const std::string& tmp, const std::string& soundfont);
void test_milestone3(const pg::GameData& d);
void test_milestone6(const pg::GameData& d);

using namespace pg;

int g_fail = 0, g_pass = 0;

static void test_flex(const GameFS& fs) {
    struct { const char* file; unsigned entries, empty; } t[] = {
        {"STATIC/U8SHAPES.FLX", 2048, 1196}, {"STATIC/U8GUMPS.FLX", 256, 183}, {"STATIC/U8FONTS.FLX", 16, 0},
        {"STATIC/GLOB.FLX", 3072, 943},      {"STATIC/FIXED.DAT", 256, 214},   {"STATIC/XFORMPAL.DAT", 2, 0},
        {"SOUND/SOUND.FLX", 256, 123},       {"SOUND/MUSIC.FLX", 261, 190},    {"USECODE/EUSECODE.FLX", 1433, 916},
    };
    for (auto& e : t) {
        Bytes b;
        CHECK(fs.read(e.file, b));
        Flex f;
        bool ok = f.load(std::move(b));
        if (!ok) std::printf("  %s: %s\n", e.file, f.error().c_str());
        CHECK(ok);
        unsigned empty = 0;
        for (unsigned i = 0; i < f.count(); ++i) empty += f.has(i) ? 0 : 1;
        CHECK_EQ(f.count(), e.entries);
        CHECK_EQ(empty, e.empty);
    }
    // Case-insensitive lookup.
    CHECK(fs.exists("static/u8pal.pal"));
    CHECK(fs.exists("Static/U8Pal.Pal"));
    CHECK(!fs.exists("STATIC/NOPE.DAT"));
}

static void test_palette(const GameData& d) {
    CHECK_EQ(d.palette.rgb6[0][0] + d.palette.rgb6[0][1] + d.palette.rgb6[0][2], 0);
    CHECK_EQ(d.palette.rgb6[8][0], 19); CHECK_EQ(d.palette.rgb6[8][1], 63); CHECK_EQ(d.palette.rgb6[8][2], 18);
    CHECK_EQ(d.palette.rgb6[10][2], 0);
    CHECK_EQ(d.palette.rgb6[255][0], 6); CHECK_EQ(d.palette.rgb6[255][1], 31); CHECK_EQ(d.palette.rgb6[255][2], 31);
    CHECK_EQ(d.palette.rgb8[11][0], 255);
    for (int s = 0; s < 256; ++s) CHECK_EQ(d.xform.tables[1][s], (s >= 8 && s <= 14) ? s - 7 : 0xFF);
    CHECK_EQ(d.xform.blend[5][3], 53);  // index 13: strong blue, alpha 53
    CHECK_EQ(d.xform.blend[1][0], 9);
}

static void decode_all(const char* what, const ShapeArchive& a, unsigned& frames, unsigned& bad, unsigned* enc = nullptr) {
    for (unsigned s = 0; s < a.count(); ++s) {
        const Shape* sh = a.shape(s);
        if (!sh) continue;
        for (unsigned f = 0; f < sh->frame_count(); ++f) {
            ShapeFrame fr;
            std::string err;
            ++frames;
            if (!sh->decode(f, fr, &err)) {
                if (bad < 10) std::printf("  %s shape %u frame %u: %s\n", what, s, f, err.c_str());
                ++bad;
            } else {
                for (uint8_t p : fr.pixels) (void)p;
            }
        }
    }
    (void)enc;
}

static void test_shapes(const GameData& d) {
    unsigned frames = 0, bad = 0;
    decode_all("U8SHAPES", d.shapes, frames, bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(frames, 21447);
    unsigned gf = 0, gb = 0;
    decode_all("U8GUMPS", d.gumps, gf, gb);
    CHECK_EQ(gb, 0);
    CHECK(gf > 73);
    unsigned ff = 0, fb = 0;
    decode_all("U8FONTS", d.fonts, ff, fb);
    CHECK_EQ(fb, 0);
    unsigned mf = 0, mb = 0;
    decode_all("U8MOUSE", d.mouse, mf, mb);
    CHECK_EQ(mb, 0);
    CHECK_EQ(mf, 42);
    std::printf("  decoded %u world, %u gump, %u font, %u cursor frames\n", frames, gf, ff, mf);

    // shapes.md examples.
    const Shape* av = d.shapes.shape(1);
    CHECK(av != nullptr);
    if (av) {
        CHECK_EQ(av->frame_count(), 1550);
        const ShapeFrame* f0 = av->frame(0);
        CHECK(f0 != nullptr);
        if (f0) {
            CHECK_EQ(f0->w, 24); CHECK_EQ(f0->h, 50); CHECK_EQ(f0->xoff, 14); CHECK_EQ(f0->yoff, 53);
        }
    }
    unsigned max_frames = 0, max_shape = 0, present = 0, last = 0;
    for (unsigned s = 0; s < d.shapes.count(); ++s)
        if (const Shape* sh = d.shapes.shape(s)) {
            ++present;
            last = s;
            if (sh->frame_count() > max_frames) { max_frames = sh->frame_count(); max_shape = s; }
        }
    CHECK_EQ(max_shape, 1);
    CHECK_EQ(last, 851);
    CHECK_EQ(present, 852);
    const ShapeFrame* g1 = d.gumps.frame(1, 0);
    CHECK(g1 && g1->w == 168 && g1->h == 113);
    CHECK(d.gumps.shape(37) && d.gumps.shape(37)->frame_count() == 48);
    const ShapeFrame* m0 = d.mouse.frame(0, 0);
    CHECK(m0 && m0->w == 12 && m0->h == 9 && m0->xoff == 6 && m0->yoff == 0);
    const ShapeFrame* m2 = d.mouse.frame(0, 2);
    CHECK(m2 && m2->w == 14 && m2->h == 9 && m2->xoff == 13 && m2->yoff == 4);
    const ShapeFrame* m36 = d.mouse.frame(0, 36);
    CHECK(m36 && m36->w == 17 && m36->h == 19 && m36->xoff == 2 && m36->yoff == 0);

    // fonts.md examples.
    const ShapeFrame* a0 = d.fonts.frame(0, 'A');
    CHECK(a0 && a0->w == 6 && a0->h == 11 && a0->yoff == 8);
    CHECK(d.fonts.frame(0, 'i') && d.fonts.frame(0, 'i')->w == 3);
    CHECK(d.fonts.frame(0, 'm') && d.fonts.frame(0, 'm')->w == 9);
    const ShapeFrame* a9 = d.fonts.frame(9, 'A');
    CHECK(a9 && a9->w == 5 && a9->h == 9 && a9->yoff == 7);
    const ShapeFrame* a10 = d.fonts.frame(10, 'A');
    CHECK(a10 && a10->w == 14 && a10->h == 14 && a10->yoff == 13);
    ShapeFont f0(d.fonts.shape(0), 0);
    CHECK_EQ(f0.height(), 11);
    CHECK_EQ(f0.baseline(), 8);
    CHECK_EQ(f0.line_advance(), 10);
    ShapeFont f10(d.fonts.shape(10), 10);
    CHECK_EQ(f10.line_advance(), 18);
}

static void test_typeflags(const GameData& d) {
    CHECK_EQ(d.typeflags.count(), 2048);
    const uint8_t av[8] = {0x22, 0x60, 0x20, 0x52, 0x10, 0x01, 0x01, 0xFA};
    CHECK(std::memcmp(d.typeflags.get(1).raw.data(), av, 8) == 0);
    const ShapeInfo& s1 = d.typeflags.get(1);
    CHECK(s1.is(ShapeInfo::kSolid));
    CHECK_EQ(s1.family(), kFamContainer);
    CHECK_EQ(s1.x(), 2); CHECK_EQ(s1.y(), 2); CHECK_EQ(s1.z(), 5);
    CHECK_EQ(d.typeflags.get(2).family(), kFamGlobEgg);
    CHECK(d.typeflags.get(2).is(ShapeInfo::kEditor));
    CHECK(d.typeflags.get(73).is(ShapeInfo::kTransl));
    CHECK_EQ(d.typeflags.get(73).family(), kFamUsecodeEgg);
    CHECK(d.typeflags.get(3).is(ShapeInfo::kOccl));
    CHECK_EQ(d.typeflags.get(5).anim_type(), 3);
    CHECK_EQ(d.typeflags.get(5).anim_data(), 8);
    CHECK_EQ(d.typeflags.get(5).anim_speed(), 4);
    CHECK_EQ(d.typeflags.get(420).equip_type(), 6);
    CHECK_EQ(d.typeflags.get(540).weight(), 7);
    CHECK(d.typeflags.get(579).is(ShapeInfo::kExplode));
    unsigned transl = 0;
    for (unsigned s = 0; s < 2048; ++s) transl += d.typeflags.get(s).is(ShapeInfo::kTransl);
    CHECK_EQ(transl, 66);
}

static void test_anim(const GameData& d) {
    CHECK_EQ(d.anim.shape_count(), 57);
    CHECK_EQ(d.anim.sequence_count(), 1070);
    unsigned av = 0;
    for (unsigned a = 0; a < 64; ++a) av += d.anim.get(1, a) != nullptr;
    CHECK_EQ(av, 53);
    const AnimSequence* walk = d.anim.get(1, 0);
    CHECK(walk != nullptr);
    if (walk) {
        CHECK_EQ(walk->length, 9);
        CHECK_EQ(walk->flags_a, 0x05);
        CHECK_EQ(walk->repeat, 1);
        const int frames[9] = {10, 11, 12, 13, 14, 15, 16, 17, 10};
        const int dist[9] = {0, 6, 4, 8, 8, 4, 4, 8, 8};
        for (int i = 0; i < 9; ++i) {
            CHECK_EQ(walk->at(0, unsigned(i)).frame, frames[i]);
            CHECK_EQ(walk->at(0, unsigned(i)).distance, dist[i]);
            CHECK_EQ(walk->at(0, unsigned(i)).flags_c, 2);
        }
        CHECK(walk->at(0, 3).special && walk->at(0, 7).special && !walk->at(0, 4).special);
        CHECK_EQ(walk->at(1, 0).frame, 20);
    }
    const AnimSequence* stand = d.anim.get(1, 2);
    CHECK(stand && stand->length == 1 && stand->at(3, 0).frame == 3);
    const AnimSequence* jump = d.anim.get(1, 17);
    CHECK(jump && jump->length == 18 && jump->flags_a == 0x18);
}

static void test_maps(const GameData& d) {
    // Record counts (maps.md section 3).
    size_t fixed_total = 0, nonfixed_total = 0;
    for (unsigned m = 0; m < d.fixed.count(); ++m) fixed_total += d.fixed.entry(m).size / 16;
    for (unsigned m = 0; m < d.nonfixed.count(); ++m) nonfixed_total += d.nonfixed.entry(m).size / 16;
    CHECK_EQ(fixed_total, 87592);
    CHECK_EQ(nonfixed_total, 12432);
    CHECK_EQ(d.fixed.entry(3).size / 16, 1835);
    CHECK_EQ(d.nonfixed.entry(3).size / 16, 153);
    CHECK_EQ(d.fixed.entry(40).size / 16, 1963);
    CHECK_EQ(d.nonfixed.entry(50).size / 16, 1667);

    // Glob example: glob 750 has 56 items, the first shape 10 frame 0 at (255, 63, 0).
    const auto& g = d.globs.get(750);
    CHECK_EQ(g.size(), 56);
    if (!g.empty()) { CHECK_EQ(g[0].shape, 10); CHECK_EQ(g[0].x, 255); CHECK_EQ(g[0].y, 63); CHECK_EQ(g[0].z, 0); }

    // NPCs.
    const NpcRecord* av = nullptr;
    for (auto& n : d.npcs) if (n.number == 1) av = &n;
    CHECK(av != nullptr);
    if (av) {
        CHECK_EQ(av->shape, 1); CHECK_EQ(av->mapnum, 3);
        CHECK_EQ(av->x, 14527); CHECK_EQ(av->y, 5887); CHECK_EQ(av->z, 8);
        CHECK_EQ(av->frame, 697);
        CHECK_EQ(av->str, 15); CHECK_EQ(av->hp, 30); CHECK_EQ(av->mana, 30);
    }
    std::printf("  %zu NPC slots in use\n", d.npcs.size());

    // The FIRST egg and the music egg on map 3 (startup.md).
    CurrentMap map(d);
    map.load(3);
    const WorldItem* first = map.find_item(73, 36, 16188, 7500, 256);
    CHECK(first != nullptr);
    if (first) { CHECK_EQ(first->x, 16188); CHECK_EQ(first->y, 7500); CHECK_EQ(first->z, 0); }
    const WorldItem* music = map.find_item(562, -1, 11551, 2079, 256);
    CHECK(music && music->quality == 52);
    CHECK(map.npc(1) != nullptr);
    CHECK(map.npc(2) && map.npc(2)->shape == 137);

    // Every map loads, and glob eggs expand when the camera passes over them.
    Surface8 surf(320, 200);
    WorldView view(d);
    size_t total_globbed = 0;
    unsigned maps_loaded = 0;
    for (unsigned m = 0; m < 256; ++m) {
        map.load(m);
        size_t before = map.live_items();
        ++maps_loaded;
        // Sweep a few camera points over the map.
        for (int cy = 2048; cy < 32768; cy += 8192)
            for (int cx = 2048; cx < 32768; cx += 8192) {
                int cam[3] = {cx, cy, 64};
                map.update_fast_area(cam, cam, 320, 200);
                view.render(map, surf, cam, cam, WorldView::Options{});
            }
        if (map.live_items() > before) total_globbed += map.live_items() - before;
    }
    CHECK_EQ(maps_loaded, 256);
    CHECK(total_globbed > 0);

    // Map 3 around the glob egg at (1023, 511, 48): glob 750's first item appears.
    map.load(3);
    int cam[3] = {1023, 511, 68};
    map.update_fast_area(cam, cam, 320, 200);
    bool found = false;
    for (size_t i = 0; i < map.item_slots(); ++i) {
        const WorldItem& it = map.item(uint32_t(i));
        if (it.alive && it.origin == WorldItem::kGlob && it.shape == 10 && it.x == 1023 && it.y == 127 && it.z == 48)
            found = true;
    }
    CHECK(found);

    // Teleport example: map 3 teleporter (11516, 1660, 48) -> map 40 id 0 at (13038, 15042, 48).
    map.load(40);
    bool dest = false;
    for (size_t i = 0; i < map.item_slots(); ++i) {
        const WorldItem& it = map.item(uint32_t(i));
        if (it.alive && it.shape == 508 && it.frame == 1 && (it.quality & 0xFF) == 0 && it.x == 13038 && it.y == 15042)
            dest = true;
    }
    CHECK(dest);
}

static SortItem box(int x, int y, int z, int xd, int yd, int zd) {
    SortItem s;
    s.x = x; s.y = y; s.z = z; s.xLeft = x - xd; s.yFar = y - yd; s.zTop = z + zd;
    s.sxLeft = s.xLeft / 4 - s.y / 4; s.sxRight = s.x / 4 - s.yFar / 4;
    s.sxTop = s.xLeft / 4 - s.yFar / 4; s.syTop = s.xLeft / 8 + s.yFar / 8 - s.zTop;
    s.sxBot = s.x / 4 - s.y / 4; s.syBot = s.x / 8 + s.y / 8 - s.z;
    s.frLeft = s.sxLeft; s.frRight = s.sxRight; s.frTop = s.syTop; s.frBottom = s.syBot + 1;
    s.flat = zd == 0; s.xflat = xd == 0; s.yflat = yd == 0;
    return s;
}

static void test_sorter() {
    // A floor tile below a crate standing on it.
    SortItem floor = box(1000, 1000, 0, 128, 128, 0), crate = box(960, 960, 0, 32, 32, 16);
    CHECK(ItemSorter::overlap(floor, crate));
    CHECK(ItemSorter::below(floor, crate));
    CHECK(!ItemSorter::below(crate, floor));
    // Something stacked higher is drawn later.
    SortItem low = box(1000, 1000, 0, 64, 64, 16), high = box(1000, 1000, 16, 64, 64, 16);
    CHECK(ItemSorter::below(low, high));
    // Separated in y: the far one first.
    SortItem far = box(1000, 900, 0, 32, 32, 32), nearer = box(1000, 1000, 0, 32, 32, 32);
    CHECK(ItemSorter::below(far, nearer));
    // Paint order from the sorter.
    ItemSorter s;
    s.begin();
    SortItem a = crate; a.item = 1;
    SortItem b = floor; b.item = 2;
    s.add(a);
    s.add(b);
    std::vector<uint32_t> order;
    s.paint([&](const SortItem& it) { order.push_back(it.item); });
    CHECK(order.size() == 2 && order[0] == 2 && order[1] == 1);
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    GameFS fs;
    if (root.empty() || !fs.open(root)) {
        std::printf("game folder not found\n");
        return 1;
    }
    std::printf("game folder: %s\n", root.c_str());
    test_flex(fs);
    GameData d;
    bool ok = d.load(fs);
    if (!ok) std::printf("load: %s\n", d.error().c_str());
    CHECK(ok);
    if (ok) {
        test_palette(d);
        test_shapes(d);
        test_typeflags(d);
        test_anim(d);
        test_maps(d);
    }
    test_sorter();
    int before = g_pass;
    test_kernel();
    test_vm();
    if (ok) {
        test_usecode_data(d);
        test_game(d);
    }
    std::printf("  milestone 2 checks: %d\n", g_pass - before);
    before = g_pass;
    if (ok) test_milestone3(d);
    std::printf("  milestone 3 checks: %d\n", g_pass - before);
    before = g_pass;
    if (ok) test_milestone4(d, argc > 2 ? argv[2] : "/tmp/pagan_test_saves");
    if (ok) test_milestone5(d, argc > 2 ? argv[2] : "/tmp/pagan_test_saves", argc > 3 ? argv[3] : "data/soundfont/u8_gm.sf2");
    {
        int b6 = g_pass;
        if (ok) test_milestone6(d);
        std::printf("  milestone 6 checks: %d\n", g_pass - b6);
    }
    std::printf("  milestone 4 checks: %d\n", g_pass - before);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
