// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Container sort tests (game/container_sort.*): categories from the data, merging, grouping,
// layouts where every item is clickable (checked pixel by pixel through the window's own
// picking and the game mode's input), weight, save / load round trips, the Sort button and
// key, an overfull backpack, and a sweep over every container of the original maps.
//   pagan_sort_tests GAME_DIR [SHOT_DIR] [SAVE_DIR]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "check.h"
#include "app/game_mode.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "data/mapfile.h"
#include "game/container_sort.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, const std::string& saves) : gm(d) {
        gm.world().skip_opening = true;
        gm.world().ask_name = false;
        gm.saves().set_dir(saves);
        gm.start();
        frames(40);
    }
    World& w() { return gm.world(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b; e.x = gm.mouse_x(); e.y = gm.mouse_y();
        ev(e);
    }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void click(int x, int y) { frames(15); move(x, y); button(1, true); frames(1); button(1, false); frames(16); }
    ContainerGump* backpack() {
        return dynamic_cast<ContainerGump*>(w().gumps.container_gump(w().equipped(1, 7)));
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        move(310, 190);  // the cursor out of the way
        frames(1);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 2);
        std::printf("  shot %s\n", path.c_str());
    }
};

uint16_t make(World& w, uint16_t cont, unsigned shape, unsigned frame, unsigned q = 0, uint16_t flags = 0) {
    uint16_t id = w.create_item(shape, frame);
    Item* it = w.item(id);
    it->quality = uint16_t(q);
    it->flags |= flags;
    w.move_to_container(id, cont, false);
    return id;
}

// Drawn bounding box of an item in g's item area.
SortBox box_of(const World& w, const Item& it, int& l, int& t) {
    SortBox b = sort_box(w.data().shapes.frame(it.shape, it.frame), it.flags & kItemFlipped);
    l = (it.y & 0xFF) + b.left_off;
    t = ((it.y >> 8) & 0xFF) + b.top_off;
    return b;
}

std::vector<uint16_t> shown(const World& w, const ContainerGump& g) {
    std::vector<uint16_t> v;
    for (uint16_t k : w.item(g.owner)->contents)
        if (const Item* it = w.item(k); it && !(it->flags & (kItemEquipped | kItemInvisible))) v.push_back(k);
    return v;
}

// Every item: inside the area, pickable through trace_item, the topmost gump at that screen
// point, and (when asked) no two bounding boxes overlap. Returns the smallest fraction of an
// item's opaque pixels that picking reaches.
double check_pickable(Ui& u, ContainerGump& g, bool no_overlap, int* failures = nullptr, bool strict = true) {
    World& w = u.w();
    std::vector<uint16_t> ids = shown(w, g);
    double minf = 1.0;
    int bad = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        const Item* it = w.item(ids[i]);
        int l, t;
        SortBox b = box_of(w, *it, l, t);
        if (strict) CHECK(l >= 0 && t >= 0 && l + b.w <= g.area_w && t + b.h <= g.area_h);
        int opaque = 0, hits = 0, sx = -1, sy = -1;
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) {
                if (!b.opaque(x, y)) continue;
                ++opaque;
                int lx = g.area_l + l + x, ly = g.area_t + t + y;
                if (g.trace_item(lx, ly) == ids[i]) { ++hits; if (sx < 0) { sx = g.x + lx; sy = g.y + ly; } }
            }
        double f = opaque ? double(hits) / opaque : 1.0;
        minf = std::min(minf, f);
        bool ok = hits > 0 && w.gumps.at(sx, sy) == &g;
        if (!ok) { ++bad; if (strict) std::printf("  item %u (shape %u) not pickable\n", ids[i], it->shape); }
        if (strict) CHECK(ok);
        if (no_overlap)
            for (size_t j = i + 1; j < ids.size(); ++j) {
                int l2, t2;
                SortBox b2 = box_of(w, *w.item(ids[j]), l2, t2);
                bool apart = l + b.w <= l2 || l2 + b2.w <= l || t + b.h <= t2 || t2 + b2.h <= t;
                if (!apart) std::printf("  boxes of %u and %u overlap\n", ids[i], ids[j]);
                CHECK(apart);
            }
    }
    if (failures) *failures = bad;
    return minf;
}

// Clicking each item through the game mode's input selects it: a press on that pixel makes it
// the pressed item, and dragging it a little lifts exactly that item.
void check_clicks(Ui& u, ContainerGump& g) {
    World& w = u.w();
    int ok = 0, n = 0;
    for (uint16_t id : shown(w, g)) {
        const Item* it = w.item(id);
        int l, t, sx = -1, sy = -1;
        SortBox b = box_of(w, *it, l, t);
        for (int y = 0; y < b.h && sx < 0; ++y)
            for (int x = 0; x < b.w; ++x)
                if (g.trace_item(g.area_l + l + x, g.area_t + t + y) == id) { sx = g.x + g.area_l + l + x; sy = g.y + g.area_t + t + y; break; }
        ++n;
        if (sx < 0) continue;
        u.frames(15);
        u.move(sx, sy);
        u.button(1, true);
        u.frames(1);
        u.move(sx + 3, sy + 3);  // starts a drag of the pressed item
        u.frames(1);
        bool lifted = g.hidden_item == id;
        u.key(kKeyEscape);  // cancel the drag: it stays where it was
        u.button(1, false);
        u.frames(2);
        if (lifted) ++ok;
        else std::printf("  click on item %u lifted %u\n", id, g.hidden_item);
    }
    CHECK_EQ(ok, n);
    std::printf("  %d / %d items selected by a press on their pixels\n", ok, n);
}

void test_categories(const GameData& d) {
    struct { unsigned shape; SortCategory c; } t[] = {
        {420, SortCategory::kWeapon}, {418, SortCategory::kWeapon}, {816, SortCategory::kWeapon},
        {579, SortCategory::kWeapon}, {531, SortCategory::kArmour}, {539, SortCategory::kArmour},
        {64, SortCategory::kArmour}, {143, SortCategory::kMoney}, {395, SortCategory::kReagent},
        {398, SortCategory::kReagent}, {399, SortCategory::kReagent}, {766, SortCategory::kPotion},
        {82, SortCategory::kKey}, {79, SortCategory::kKey}, {232, SortCategory::kKey},
        {537, SortCategory::kFood}, {538, SortCategory::kFood}, {284, SortCategory::kFood},
        {159, SortCategory::kBook}, {613, SortCategory::kBook}, {288, SortCategory::kBook},
        {827, SortCategory::kBook}, {637, SortCategory::kContainer}, {185, SortCategory::kContainer},
        {396, SortCategory::kMagic}, {397, SortCategory::kMagic}, {746, SortCategory::kMagic},
        {568, SortCategory::kMisc}, {534, SortCategory::kMisc},
    };
    for (auto& e : t) {
        SortCategory c = sort_category(d, e.shape);
        if (c != e.c) std::printf("  shape %u: %s, expected %s\n", e.shape, sort_category_name(c), sort_category_name(e.c));
        CHECK(c == e.c);
    }
    // Tally over every movable shape of the data.
    std::map<int, int> n;
    for (unsigned s = 0; s < d.typeflags.count(); ++s) {
        const ShapeInfo& si = d.typeflags.get(s);
        if (si.is(ShapeInfo::kFixed) || si.weight() == 0 || si.is(ShapeInfo::kEditor) || s >= 1024) continue;
        if (si.family() == kFamContainer && si.weight() >= 100) continue;  // creatures, big chests
        ++n[int(sort_category(d, s))];
    }
    std::printf("  movable shapes per category:");
    for (auto& [c, k] : n) std::printf(" %s %d", sort_category_name(SortCategory(c)), k);
    std::printf("\n");
}

unsigned qty(const World& w, uint16_t id) { const Item* it = w.item(id); return it ? it->quality : 0; }

void test_backpack(const GameData& d, const std::string& saves) {
    Ui u(d, saves);
    World& w = u.w();
    uint16_t pack = w.equipped(1, 7);
    CHECK(pack != 0);
    if (!pack) return;
    // Empty the starting pack (into the void) so the contents are known.
    for (uint16_t k : std::vector<uint16_t>(w.item(pack)->contents)) w.destroy_now(k);
    // A mixed load: weapons, armour, reagents in split stacks, coins in several piles (one
    // owned), keys, potions, food, a scroll and a bag with coins inside.
    std::vector<uint16_t> all;
    auto add = [&](unsigned s, unsigned f, unsigned q = 0, uint16_t fl = 0) { uint16_t id = make(w, pack, s, f, q, fl); all.push_back(id); return id; };
    add(418, 0);                // dagger
    add(420, 0);                // sword
    add(531, 0);                // helmet
    uint16_t c1 = add(143, 0, 10), c2 = add(143, 0, 25), c3 = add(143, 0, 7);
    // One pile not owned by the player (as if put in by usecode): never merged with his own.
    uint16_t cown = add(143, 0, 5);
    w.item(cown)->flags &= uint16_t(~kItemOwned);
    uint16_t b1 = add(395, 0, 3), b2 = add(395, 2, 4);  // blood (frame group 0-5)
    uint16_t o1 = add(395, 6, 2), o2 = add(395, 7, 5);  // bone (6-7)
    uint16_t g1 = add(395, 10, 6);                      // executioner's hood
    add(82, 0, 12);             // two keys
    add(82, 1, 30);
    add(766, 0);                // two potions
    add(766, 3);
    add(538, 0);                // food
    add(159, 0);                // a scroll
    uint16_t bag = add(637, 0);
    uint16_t inner = make(w, bag, 143, 0, 9);
    uint16_t inner2 = make(w, bag, 143, 0, 4);
    uint32_t weight_before = w.total_weight(*w.item(pack));
    u.key('i');
    u.frames(5);
    ContainerGump* g = u.backpack();
    CHECK(g != nullptr);
    if (!g) return;
    u.frames(3);
    u.shot("sort_before");
    int clicks_before = 0;
    check_pickable(u, *g, false, &clicks_before, false);
    std::printf("  before: %zu items, %d not pickable at their random places\n", shown(w, *g).size(), clicks_before);
    // --- S with the mouse over the backpack.
    u.move(g->x + g->w / 2, g->y + 10);
    u.key('s');
    std::printf("  right after:");
    for (uint16_t k : shown(w, *g)) std::printf(" %u", w.item(k)->shape);
    std::printf("\n");
    u.frames(10);
    CHECK(g->sort_wait == 0);
    // Merged with the right totals; the owned pile and the bag's piles stay apart.
    std::vector<uint16_t> now = shown(w, *g);
    unsigned coins = 0, piles = 0;
    for (uint16_t k : now)
        if (w.item(k)->shape == 143 && k != cown) { coins += qty(w, k); ++piles; }
    CHECK_EQ(coins, 42u);
    CHECK_EQ(piles, 1u);
    CHECK_EQ(qty(w, cown), 5u);
    CHECK(w.item(cown) && w.item(cown)->parent == pack);
    CHECK_EQ(qty(w, c1) + qty(w, c2) + qty(w, c3), 42u);
    CHECK_EQ(qty(w, b1) + qty(w, b2), 7u);
    CHECK((w.item(b1) == nullptr) != (w.item(b2) == nullptr));
    CHECK_EQ(qty(w, o1) + qty(w, o2), 7u);
    CHECK_EQ(qty(w, g1), 6u);
    CHECK(w.item(inner) && w.item(inner2) && w.item(inner)->parent == bag && qty(w, inner) == 9 && qty(w, inner2) == 4);
    // Coins fired combine: the merged pile has the frame its usecode picks for 42.
    for (uint16_t k : now)
        if (w.item(k)->shape == 143 && k != cown)
            std::printf("  coins: one pile of %u (frame %u), owned pile of %u kept apart\n", qty(w, k), w.item(k)->frame, qty(w, cown));
    // Weight: the same, except the rounding of the quantity family (coins weigh ceil(q / 10)
    // per stack, world/items.md), which a merge changes exactly as a drag-and-drop merge.
    uint32_t weight_after = w.total_weight(*w.item(pack));
    uint32_t coin_before = (10 + 9) / 10 + (25 + 9) / 10 + (7 + 9) / 10, coin_after = (42 + 9) / 10;
    CHECK_EQ(weight_after, weight_before - coin_before + coin_after);
    std::printf("  weight %u -> %u (coin rounding %u -> %u)\n", weight_before, weight_after, coin_before, coin_after);
    // Nothing left the container; every item is still there and owned flags unchanged.
    CHECK_EQ(now.size(), all.size() - 4);  // 2 coin piles, 1 blood and 1 bone stack merged away
    // Grouped: contents in reading order follow the category order.
    SortCategory last = SortCategory::kWeapon;
    bool grouped = true;
    for (uint16_t k : now) {
        SortCategory c = sort_category(d, w.item(k)->shape);
        if (c < last) grouped = false;
        last = c;
    }
    CHECK(grouped);
    std::printf("  order:");
    for (uint16_t k : now) std::printf(" %u:%s", w.item(k)->shape, sort_category_name(sort_category(d, w.item(k)->shape)));
    std::printf("\n");
    double minf = check_pickable(u, *g, true);
    std::printf("  after: %zu items, no overlapping boxes, every item pickable (min %.0f%% of pixels)\n", now.size(), minf * 100);
    check_clicks(u, *g);
    u.shot("sort_after");
    // --- Save / load round trip keeps positions, order and counts.
    SaveWriter out;
    w.save_game(out, "sort");
    World w2(d);
    w2.auto_ui = true;
    w2.new_game();
    SaveReader in(out.data().data(), out.data().size());
    CHECK(w2.load_game(in));
    const Item* p2 = w2.item(pack);
    CHECK(p2 != nullptr);
    if (p2) {
        CHECK(p2->contents == w.item(pack)->contents);
        bool same = true;
        for (uint16_t k : p2->contents)
            if (!w2.item(k) || w2.item(k)->y != w.item(k)->y || w2.item(k)->quality != w.item(k)->quality) same = false;
        CHECK(same);
        CHECK_EQ(w2.total_weight(*p2), weight_after);
        SaveWriter out2;
        w2.save_game(out2, "sort");
        CHECK(out2.data() == out.data());
        std::printf("  save / load: %zu bytes, contents, positions and counts identical\n", out.data().size());
    }
    // --- Sorting again changes nothing (stable).
    std::vector<uint16_t> order = w.item(pack)->contents;
    std::vector<int32_t> pos;
    for (uint16_t k : order) pos.push_back(w.item(k)->y);
    sort_container(w, pack);
    u.frames(5);
    bool stable = w.item(pack)->contents == order;
    for (size_t i = 0; i < order.size() && stable; ++i) stable = w.item(order[i])->y == pos[i];
    CHECK(stable);
    // --- The Sort button through the input path: scramble, click it, sorted again.
    for (uint16_t k : w.item(pack)->contents) w.item(k)->y = 0xFFFF;
    u.frames(2);
    int bx, by, bw, bh;
    CHECK(g->sort_button(bx, by, bw, bh));
    CHECK(w.gumps.at(g->x + bx + 3, g->y + by + 3) == g);
    u.click(g->x + bx + bw / 2, g->y + by + bh / 2);
    u.frames(5);
    bool resorted = w.item(pack)->contents == order;
    for (size_t i = 0; i < order.size() && resorted; ++i) resorted = w.item(order[i])->y == pos[i];
    CHECK(resorted);
    CHECK(u.backpack() == g);  // the click did not "use" (close) the backpack
    // A double-click on the button does not close the window either.
    u.frames(15);
    u.move(g->x + bx + 2, g->y + by + 2);
    u.button(1, true); u.button(1, false); u.frames(2); u.button(1, true); u.button(1, false); u.frames(16);
    CHECK(u.backpack() == g);
    std::printf("  Sort button: re-sorts the scrambled pack; S key, stable on a second sort\n");
    // --- Controller: X with the virtual pointer over the window.
    {
        for (uint16_t k : w.item(pack)->contents) w.item(k)->y = 0xFFFF;
        u.frames(2);
        InputEvent e;
        e.type = InputEvent::kPadAdded;
        u.ev(e);
        // (Milestone 6: a newly connected controller acts only after a first button press.)
        e = InputEvent();
        e.type = InputEvent::kPadDown; e.button = kPadGuide;
        u.ev(e);
        e.type = InputEvent::kPadUp;
        u.ev(e);
        u.move(g->x + g->w / 2, g->y + 12);
        e = InputEvent();
        e.type = InputEvent::kPadAxis; e.axis = kAxisRX; e.value = 9000;  // nudge the pointer
        u.ev(e);
        u.frames(1);
        e.value = 0;
        u.ev(e);
        u.frames(1);
        CHECK(w.gumps.at(u.gm.mouse_x(), u.gm.mouse_y()) == g);
        e = InputEvent();
        e.type = InputEvent::kPadDown; e.button = kPadX;
        u.ev(e);
        e.type = InputEvent::kPadUp;
        u.ev(e);
        u.frames(5);
        bool padsorted = w.item(pack)->contents == order;
        for (size_t i = 0; i < order.size() && padsorted; ++i) padsorted = w.item(order[i])->y == pos[i];
        CHECK(padsorted);
        std::printf("  controller X over the window: %s\n", padsorted ? "sorted" : "not sorted");
    }
    // --- The bag's own window sorts only the bag (its two piles merge there).
    sort_container(w, bag);  // no window: refused
    CHECK(w.item(inner2) != nullptr);
    w.open_gump(bag, 3);
    u.frames(3);
    SortResult r = sort_container(w, bag);
    CHECK(r.ok);
    u.frames(5);
    CHECK_EQ(qty(w, inner) + qty(w, inner2), 13u);
    CHECK((w.item(inner) == nullptr) != (w.item(inner2) == nullptr));
}

void test_overfull(const GameData& d, const std::string& saves, size_t count, const char* shot) {
    Ui u(d, saves);
    World& w = u.w();
    uint16_t pack = w.equipped(1, 7);
    for (uint16_t k : std::vector<uint16_t>(w.item(pack)->contents)) w.destroy_now(k);
    // Up to 42 items: more than the 77 x 39 pixel area holds side by side.
    unsigned shapes[][2] = {{420, 0}, {419, 0}, {541, 0}, {542, 0}, {540, 0}, {417, 0}, {418, 0}, {531, 0}, {532, 0},
                            {539, 0}, {530, 0}, {523, 0}, {766, 0}, {766, 1}, {766, 2}, {766, 5}, {82, 0}, {82, 2},
                            {537, 0}, {538, 2}, {538, 6}, {284, 0}, {159, 0}, {613, 0}, {288, 0}, {637, 0}, {185, 0},
                            {568, 0}, {534, 0}, {395, 6}, {395, 10}, {395, 14}, {395, 16}, {398, 0}, {398, 3},
                            {399, 4}, {396, 0}, {397, 0}, {746, 0}, {143, 3}, {79, 1}, {116, 0}};
    for (size_t i = 0; i < count && i < sizeof(shapes) / sizeof(shapes[0]); ++i)
        make(w, pack, shapes[i][0], shapes[i][1], d.typeflags.get(shapes[i][0]).family() == kFamReagent ? 5 : 1);
    u.key('i');
    u.frames(5);
    ContainerGump* g = u.backpack();
    CHECK(g != nullptr);
    if (!g) return;
    long opaque = 0;
    for (uint16_t k : shown(w, *g)) {
        int l, t;
        SortBox b = box_of(w, *w.item(k), l, t);
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) opaque += b.opaque(x, y);
    }
    std::printf("  overfull pack (%zu items): %ld opaque pixels of items in an area of %d (%.1fx)\n", count, opaque, g->area_w * g->area_h,
                double(opaque) / (g->area_w * g->area_h));
    auto t0 = std::chrono::steady_clock::now();
    SortResult r = sort_container(w, *g);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  sort took %.1f ms\n", ms);
    u.frames(10);
    CHECK(r.ok);
    CHECK(r.mode == SortLayout::kOverlap);
    int fails = 0;
    double minf = check_pickable(u, *g, false, &fails);
    CHECK_EQ(fails, 0);
    std::printf("  overfull pack: %d items, layout %s, %d partly covered, %s; picking reaches >= %.0f%% of every item\n",
                r.items, sort_mode_name(r.mode), r.covered, r.all_clickable ? "all keep >= 50% and a clear patch" : "some crowded",
                minf * 100);
    if (count <= 30) CHECK(r.all_clickable);
    check_clicks(u, *g);
    u.shot(shot);
}

// Every container with contents in the original maps, laid out in the window its kind opens.
void test_sweep(const GameData& d) {
    auto gump_for = [&](unsigned shape) -> int {
        std::string n = d.usecode.class_name(shape);
        if (n == "BACKPACK") return 2;
        if (n == "BAG") return 3;
        if (n == "BARREL") return 4;
        if (n == "BASKET") return 5;
        if (n.find("CHEST") != std::string::npos || n == "LCHST_EW" || n == "LCHST_NS") return 7;
        if (n == "JBOX") return 10;
        if (n.find("DEAD") == 0 || n.find("BONES") == 0) return 12;
        return 0;
    };
    std::map<int, int> modes;
    int total = 0, clickable = 0, unknown = 0, worst_n = 0;
    double worst = 1.0;
    for (const Flex* fx : {&d.nonfixed, &d.fixed})
        for (unsigned m = 0; m < fx->count(); ++m) {
            std::vector<MapEntry> tree = build_item_tree(parse_item_records(fx->entry(m)), d.typeflags);
            std::map<int, std::vector<int>> kids;
            for (size_t i = 0; i < tree.size(); ++i)
                if (tree[i].parent >= 0) kids[tree[i].parent].push_back(int(i));
            for (auto& [p, ks] : kids) {
                int gs = gump_for(tree[size_t(p)].rec.shape);
                if (!gs) { ++unknown; continue; }
                GameData::GumpArea a = d.gump_area(unsigned(gs));
                std::vector<SortBox> boxes;
                for (int k : ks) {
                    const ItemRecord& r = tree[size_t(k)].rec;
                    if (r.flags & (kItemInvisible | kItemEquipped)) continue;
                    if (const ShapeFrame* f = d.shapes.frame(r.shape, r.frame)) boxes.push_back(sort_box(f, r.flags & kItemFlipped));
                }
                if (boxes.empty()) continue;
                SortLayout L = sort_layout(boxes, a.right - a.left, a.bottom - a.top);
                ++total;
                ++modes[int(L.mode)];
                if (L.all_clickable) ++clickable;
                if (L.mode == SortLayout::kOverlap && L.min_visible < worst) { worst = L.min_visible; worst_n = int(boxes.size()); }
            }
        }
    std::printf("  real containers: %d sorted (%d of unmapped kinds skipped):", total, unknown);
    for (auto& [m, k] : modes) std::printf(" %s %d", sort_mode_name(SortLayout::Mode(m)), k);
    std::printf("; all clickable in %d; worst overlap case %d items, min visible %.0f%%\n", clickable, worst_n, worst * 100);
    CHECK(total > 50);
    CHECK_EQ(clickable, total);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_shots = argc > 2 ? argv[2] : "";
    std::string saves = argc > 3 ? argv[3] : "/tmp/pagan_sort_saves";
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    test_categories(d);
    test_backpack(d, saves);
    test_overfull(d, saves, 28, "sort_crowded");
    test_overfull(d, saves, 42, "sort_overfull");
    test_sweep(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
