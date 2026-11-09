// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Companions' homes (game/party.h, NOTES.md "Companions: homes"): each home spot is standable, free
// and clear of eggs, scenes and items in both world modes and with the invasion on (shots
// companion_home_<id>.png); a real recruitment there; survival over map changes and saves; Debug
// Mode moving only the never-recruited, live; old beach saves; a copy of the game whose maps differ.
// That the spots can be walked to is in tests/test_route.cpp (test_home_walks).
//   pagan_homes_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
// ONLY = a test's name, or probe:MAP,X,Y,Z,R: the standable cells round a point (choosing spots).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "core/variants.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/debug_menu.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/route_guide.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, bool companions = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = companions;
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    void frames(int n, bool draw = false) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            if (draw) gm.render(surf);
        }
    }
    void warp(unsigned map, int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().teleport_actor(1, x, y, z, map);
        frames(3);
    }
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
    int idx(const std::string& id) { return p().find_def(id); }
    CompanionState& st(const std::string& id) { return p().state[size_t(idx(id))]; }
    const CompanionDef& def(const std::string& id) { return p().defs[size_t(idx(id))]; }
    Actor* a(const std::string& id) { return w().actor(st(id).id); }
    // The Avatar beside someone's home (the fast area, so the ground, is there).
    void beside_home(const std::string& id) {
        const CompanionDef& h = def(id);
        warp(h.home_map, h.home[0] + 140, h.home[1], h.home[2]);
    }
    void double_click_actor(uint16_t aid) {
        const Item* it = w().item(aid);
        int sx, sy;
        w().screen_point(*it, sx, sy);
        gm.render(surf);
        int px = sx - 2, py = sy - 22;
        bool found = false;
        for (int dy = -40; dy <= 4 && !found; dy += 2)
            for (int dx = -12; dx <= 12 && !found; dx += 2)
                if (gm.pick(sx + dx, sy + dy) == aid) { px = sx + dx; py = sy + dy; found = true; }
        move(px, py);
        frames(2);
        button(1, true); button(1, false);
        frames(1);
        button(1, true); button(1, false);
        frames(2);
    }
    bool wait_menu(int n = 900) {
        for (int i = 0; i < n && !w().ask_menu; ++i) {
            frames(1);
            if (!w().barks.empty() && i % 3 == 0) key(kKeyReturn);
        }
        return w().ask_menu != nullptr;
    }
    bool answer(const std::string& label) {
        if (!wait_menu()) return false;
        for (size_t i = 0; i < w().ask_menu->answers.size(); ++i)
            if (w().vm().string(w().ask_menu->answers[i]) == label) {
                key('1' + int(i));
                frames(2);
                return true;
            }
        std::printf("  no answer '%s'\n", label.c_str());
        return false;
    }
    void finish_talk() {
        for (int i = 0; i < 900 && p().talking(); ++i) {
            frames(1);
            if (!w().barks.empty() && i % 3 == 0) key(kKeyReturn);
            if (w().ask_menu && i > 20) answer("bye");
        }
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        move(316, 196);
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 2);
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

const std::vector<std::string> kIds = {"morvain", "kalvax", "brannoc", "varn", "ilvaran", "corvin", "wynna", "nerissa"};

// What stands near a point.
struct Near { int monster = 1 << 30, hatch = 1 << 30, scene = 1 << 30, tele = 1 << 30, item = 1 << 30; unsigned item_shape = 0; };

Near look_round(World& w, int x, int y, int z, uint16_t self) {
    Near n;
    const GameData& d = w.data();
    for (size_t i = 1; i < w.map().item_slots(); ++i) {
        if (i == self || i == 1) continue;
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0) continue;
        int dd = int(std::hypot(double(it->x - x), double(it->y - y)));
        const ShapeInfo& si = d.typeflags.get(it->shape);
        int fam = si.family();
        if (fam == kFamMonsterEgg) n.monster = std::min(n.monster, dd);
        else if (fam == kFamTeleportEgg) n.tele = std::min(n.tele, dd);
        else if (fam == 4) {
            if ((it->quality & 0xFF) == 60) n.hatch = std::min(n.hatch, dd);
            else n.scene = std::min(n.scene, dd);
        } else if (it->z + 8 >= z && it->z <= z + 40 && !si.is(ShapeInfo::kRoof) && dd < n.item) {
            // Flat ground under the feet does not count: only what has height above them.
            int xd, yd, zd;
            si.footprint(false, xd, yd, zd);
            if (it->z + zd > z && it->z < z + 40) { n.item = dd; n.item_shape = it->shape; }
        }
    }
    return n;
}

void probe(const GameData& d, unsigned map, int cx, int cy, int cz, int r) {
    Ui u(d, false);
    u.frames(10);
    u.warp(map, cx, cy, cz);
    u.frames(10);
    const int st = 32;
    World& w = u.w();
    std::printf("map %u; x from %d step %d; digits = z/8 (base32), ~ water, ! damaging\n", map, cx - r, st);
    for (int y = cy - r; y <= cy + r; y += st) {
        std::printf("%5d ", y);
        for (int x = cx - r; x <= cx + r; x += st) {
            char c = '.';
            for (int z = 0; z <= 248; z += 8) {
                World::Placement pl = w.can_exist_shape(574, x, y, z, 1);
                if (!pl.valid || !pl.supported || !pl.land) continue;
                const Item* l = w.item(pl.land);
                if (!l) continue;
                const ShapeInfo& li = d.typeflags.get(l->shape);
                if (li.is(ShapeInfo::kDamaging)) { c = '!'; continue; }
                if (li.is(ShapeInfo::kSea) && !li.is(ShapeInfo::kLand)) { c = '~'; continue; }
                c = "0123456789abcdefghijklmnopqrstuv"[z / 8];
                if (std::abs(z - cz) <= 32) break;
            }
            std::putchar(c);
        }
        std::printf("\n");
    }
    Near n = look_round(w, cx, cy, cz, 1);
    std::printf("at centre: monster egg %d, hatcher %d, other usecode egg %d, teleporter %d, item %d (shape %u)\n", n.monster,
                n.hatch, n.scene, n.tele, n.item, n.item_shape);
}

// Each home: on its map, standing on real dry ground, nothing in the way.
void check_spot(Ui& u, const std::string& id, unsigned map) {
    World& w = u.w();
    const CompanionDef& h = u.def(id);
    const uint16_t aid = u.st(id).id;
    World::Placement pl = w.can_exist_shape(h.shape, h.home[0], h.home[1], h.home[2], aid);
    CHECK(pl.valid);
    CHECK(pl.supported);
    const Item* land = w.item(pl.land);
    CHECK(land != nullptr);
    if (land) {
        const ShapeInfo& li = w.data().typeflags.get(land->shape);
        CHECK(!li.is(ShapeInfo::kDamaging));
        CHECK(!(li.is(ShapeInfo::kSea) && !li.is(ShapeInfo::kLand)));
    }
    Near n = look_round(w, h.home[0], h.home[1], h.home[2], aid);
    std::printf("  %-8s map %u at %d,%d,%d on shape %u: monster egg %d, hatcher %d, other usecode egg %d, teleporter %d, item %d (shape %u)\n",
                id.c_str(), map, h.home[0], h.home[1], h.home[2], land ? land->shape : 0, n.monster, n.hatch, n.scene, n.tele, n.item, n.item_shape);
    CHECK(n.monster >= 768);  // the distance a monster looks for a target
    CHECK(n.hatch >= 512);
    CHECK(n.scene >= 256);
    CHECK(n.tele >= 256);
    CHECK(n.item >= 48);
}

// Critical-path targets on the home's map: none within the scene's reach of the spot.
void test_critpath() {
    std::printf("critical path against the homes\n");
    std::ifstream fc(g_data + "/critpath.json"), fp(g_data + "/companions.json");
    nlohmann::json crit = nlohmann::json::parse(fc, nullptr, true, true), comp = nlohmann::json::parse(fp, nullptr, true, true);
    int closest = 1 << 30;
    for (const nlohmann::json& c : comp["companions"]) {
        CHECK(c.contains("home"));
        const nlohmann::json& h = c["home"];
        int map = h["map"], x = h["x"], y = h["y"];
        for (const nlohmann::json& s : crit["steps"]) {
            std::vector<nlohmann::json> pts;
            if (s.contains("target") && s["target"].is_object()) {
                pts.push_back(s["target"]);
                if (s["target"].contains("via"))
                    for (const nlohmann::json& v : s["target"]["via"]) pts.push_back(v);
            }
            for (const nlohmann::json& t : pts) {
                if (!t.contains("map") || !t.contains("x")) continue;
                int tm = t["map"], d = int(std::hypot(double(int(t["x"]) - x), double(int(t["y"]) - y)));
                const bool here = tm == map || (map == 16 && tm == 26);
                if (!here) continue;
                closest = std::min(closest, d);
                if (d < 640) std::printf("  %s is %d from step %s\n", c["id"].get<std::string>().c_str(), d, s["id"].get<std::string>().c_str());
                CHECK(d >= 640);
            }
        }
    }
    std::printf("  closest critical-path point to any home: %d units\n", closest);
    CHECK(closest >= 640);
}

// Both world modes, the invasion flag on as well: they stand and live through a while.
void test_spots(const GameData& d, bool stitched, bool invasion) {
    std::printf("home spots (%s, invasion %s)\n", stitched ? "stitched" : "original maps", invasion ? "on" : "off");
    restored_invasion() = invasion;
    for (const std::string& id : kIds) {
        Ui u(d);
        if (stitched) CHECK(u.w().set_world_mode(true, g_data + "/world_layout.json"));
        u.frames(40);
        CHECK(u.p().spawned_once);
        const CompanionDef& h = u.def(id);
        const Actor* a = u.a(id);
        CHECK(a != nullptr);
        if (!a) continue;
        CHECK_EQ(unsigned(a->mapnum), h.home_map);
        if (invasion) u.w().vm().set_global(58, 1, 1);
        u.beside_home(id);
        u.frames(5);
        CHECK(a->chunk >= 0);
        CHECK_EQ(a->x, h.home[0]);
        CHECK_EQ(a->y, h.home[1]);
        CHECK_EQ(a->z, h.home[2]);
        check_spot(u, id, h.home_map);
        if (!stitched && !invasion) {
            u.frames(30, true);
            u.shot("companion_home_" + id);
        }
        u.frames(300);
        a = u.a(id);
        CHECK(a && !a->is(Actor::kDead));
        CHECK(a && a->x == h.home[0] && a->y == h.home[1] && a->z == h.home[2]);
        CHECK(u.st(id).mode == CompanionState::kIdle);
        // Carthax Lake with the water back (the other state of the place).
        if (h.home_map == 16) {
            u.w().kernel().kill(1, kProcAny);
            u.w().teleport_actor(1, h.home[0] + 140, h.home[1], h.home[2], 26);
            u.frames(10);
            a = u.a(id);
            CHECK_EQ(unsigned(a->mapnum), 26u);
            CHECK(a->chunk >= 0);
            check_spot(u, id, 26);
            u.frames(120);
            CHECK(a->x == h.home[0] && a->y == h.home[1] && a->z == h.home[2]);
        }
    }
    restored_invasion() = true;
}

// A real conversation at home; he then follows through map changes, a save and a load.
void test_recruit(const GameData& d) {
    std::printf("recruit at home\n");
    Ui u(d);
    u.frames(40);
    u.beside_home("brannoc");
    Actor* b = u.a("brannoc");
    u.frames(10, true);
    u.double_click_actor(b->id);
    CHECK(u.p().talking());
    CHECK(u.answer("join me"));
    CHECK(u.wait_menu());
    int bi = u.idx("brannoc");
    CHECK(u.p().in_party(bi));
    CHECK(u.answer("bye"));
    u.finish_talk();
    // Over to another map and back: he travels along; the others stay on theirs.
    u.warp(40, 13038, 15042, 48);
    u.frames(20);
    b = u.a("brannoc");
    CHECK_EQ(unsigned(b->mapnum), 40u);
    CHECK(b->chunk >= 0);
    CHECK_EQ(unsigned(u.a("morvain")->mapnum), 6u);
    CHECK(u.a("morvain")->chunk < 0);
    std::vector<uint8_t> a = save_bytes(u.w());
    Ui u2(d);
    u2.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u2.w().load_game(r));
    u2.frames(5);
    CHECK(u2.p().in_party(bi));
    CHECK_EQ(unsigned(u2.a("brannoc")->mapnum), 40u);
    CHECK_EQ(unsigned(u2.a("morvain")->mapnum), 6u);
    CHECK_EQ(u2.a("morvain")->x, u2.def("morvain").home[0]);
    // Saved from a map where nobody lives, loaded, the Avatar walks in: Morvain is at home.
    u2.beside_home("morvain");
    CHECK(u2.a("morvain")->chunk >= 0);
    CHECK_EQ(u2.a("morvain")->x, u2.def("morvain").home[0]);
    // And a load does not move anyone.
    CHECK(u2.p().state[size_t(u2.idx("morvain"))].mode == CompanionState::kIdle);
}

// Stitched: a seam crossing keeps everyone on their own map.
void test_seam(const GameData& d) {
    std::printf("seam crossing\n");
    Ui u(d);
    CHECK(u.w().set_world_mode(true, g_data + "/world_layout.json"));
    u.frames(40);
    u.warp(40, 13038, 15042, 48);
    u.frames(20);
    for (const std::string& id : kIds) {
        const Actor* a = u.a(id);
        CHECK(a && unsigned(a->mapnum) == u.def(id).home_map);
        CHECK(a && a->x == u.def(id).home[0]);
    }
    u.beside_home("corvin");
    CHECK(u.a("corvin")->chunk >= 0);
}

// Debug Mode through the real options row: live, both ways, only the never-recruited.
void test_debug(const GameData& d) {
    std::printf("debug mode moves the never-recruited\n");
    Ui u(d);
    u.frames(40);
    Party& p = u.p();
    // Set up: Morvain in the party; Kalvax told to wait on map 40; Wynna parted on map 40; Corvin dead.
    p.join(u.idx("morvain"));
    p.join(u.idx("kalvax"));
    p.join(u.idx("wynna"));
    u.warp(40, 13038, 15042, 48);
    u.frames(30);
    p.wait(u.idx("kalvax"));
    p.part_ways(u.idx("wynna"));
    CHECK(u.st("wynna").mode == CompanionState::kIdle);
    CHECK(!p.in_party(u.idx("wynna")));
    const int kx = u.a("kalvax")->x, wx = u.a("wynna")->x, wy = u.a("wynna")->y;
    Actor* cv = u.a("corvin");
    cv->set(Actor::kDead, true);
    u.st("corvin").mode = CompanionState::kDead;
    MenuGump* book = u.w().open_menu(MenuGump::kOptions);
    CHECK(book != nullptr);
    int row = -1;
    std::vector<MenuGump::OptionRow> rows = book->option_rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].label == "Debug Mode:") row = int(i);
    CHECK(row >= 0);
    CHECK(rows[size_t(row)].value() == "Off");
    book->change_option(row, 1);
    u.frames(2);  // the book still open: toggled at once
    for (const char* id : {"brannoc", "varn", "ilvaran", "nerissa"}) {
        const Actor* a = u.a(id);
        CHECK_EQ(unsigned(a->mapnum), 3u);
        CHECK_EQ(a->x, u.def(id).spawn[0]);
        CHECK_EQ(a->y, u.def(id).spawn[1]);
        CHECK_EQ(unsigned(u.st(id).wait_map), 3u);
    }
    // The recruited and the dead stayed.
    CHECK_EQ(unsigned(u.a("morvain")->mapnum), 40u);
    CHECK(u.st("morvain").mode == CompanionState::kFollow);
    CHECK_EQ(u.a("kalvax")->x, kx);
    CHECK(u.st("kalvax").mode == CompanionState::kWait);
    CHECK(u.a("wynna")->x == wx && u.a("wynna")->y == wy);
    CHECK_EQ(unsigned(u.a("wynna")->mapnum), 40u);
    CHECK_EQ(unsigned(cv->mapnum), u.def("corvin").home_map);
    CHECK(cv->x == u.def("corvin").home[0]);
    // The Avatar goes to the beach: there they are, standing on it, with the option on and a save.
    if (MenuGump* g = u.w().gumps.find<MenuGump>()) g->close();
    u.frames(3);
    u.warp(3, 15500, 5800, 8);
    u.frames(10);
    for (const char* id : {"brannoc", "varn", "ilvaran", "nerissa"}) CHECK(u.a(id)->chunk >= 0);
    CHECK(rows[size_t(row)].value() == "On");
    std::vector<uint8_t> on_save = save_bytes(u.w());
    // Off, on the beach itself: they leave at once, off the map, to their homes.
    {
        MenuGump* b2 = u.w().open_menu(MenuGump::kOptions);
        b2->change_option(row, -1);
    }
    u.frames(2);
    for (const char* id : {"brannoc", "varn", "ilvaran", "nerissa"}) {
        const CompanionDef& h = u.def(id);
        const Actor* a = u.a(id);
        CHECK_EQ(unsigned(a->mapnum), h.home_map);
        CHECK(a->x == h.home[0] && a->y == h.home[1] && a->z == h.home[2]);
        CHECK(a->chunk < 0);
    }
    CHECK_EQ(unsigned(u.a("wynna")->mapnum), 40u);
    if (MenuGump* g = u.w().gumps.find<MenuGump>()) g->close();
    u.frames(3);
    // Sent home by the toggle, he is there when the Avatar arrives, and can be talked to.
    {
        Ui t(d);
        t.frames(40);
        Config::global().set_bool("debug_mode", true);
        t.frames(3);
        t.warp(3, 15500, 5800, 8);
        t.frames(10);
        Config::global().set_bool("debug_mode", false);
        t.frames(3);
        t.beside_home("ilvaran");
        t.frames(10, true);
        const Actor* il = t.a("ilvaran");
        CHECK(il->chunk >= 0);
        CHECK(il->x == t.def("ilvaran").home[0] && il->z == t.def("ilvaran").home[2]);
        t.double_click_actor(il->id);
        CHECK(t.p().talking());
        CHECK(t.answer("bye"));
        t.finish_talk();
    }
    // A save made with it on, loaded with it off: at home; made off, loaded on: on the beach.
    {
        Ui u2(d);
        u2.frames(5);
        SaveReader r(on_save.data(), on_save.size());
        CHECK(u2.w().load_game(r));
        u2.frames(3);
        CHECK_EQ(unsigned(u2.a("varn")->mapnum), u2.def("varn").home_map);
        CHECK(u2.p().in_party(u2.idx("morvain")));
    }
    // A companion on the beach, recruited in debug mode: stays when it goes off.
    Config::global().set_bool("debug_mode", true);
    u.frames(3);
    u.warp(3, 15935, 5880, 8);
    u.frames(5);
    CHECK_EQ(unsigned(u.a("varn")->mapnum), 3u);
    p.join(u.idx("varn"));
    u.frames(5);
    p.part_ways(u.idx("varn"));
    int vx = u.a("varn")->x, vy = u.a("varn")->y;
    Config::global().set_bool("debug_mode", false);
    u.frames(3);
    CHECK_EQ(unsigned(u.a("varn")->mapnum), 3u);
    CHECK(u.a("varn")->x == vx && u.a("varn")->y == vy);
    CHECK_EQ(unsigned(u.a("brannoc")->mapnum), u.def("brannoc").home_map);
    // The debug menu: Where they are; All join from anywhere.
    Config::global().set_bool("debug_mode", true);
    u.frames(3);
    u.warp(40, 13038, 15042, 48);
    DebugTools& dt = u.gm.debug();
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->select("Where they are"));
        u.key(kKeyReturn);
        u.frames(2);
        u.shot("companion_homes_debug_list");
        u.key(kKeyEscape);
        CHECK(m->select("All join"));
        u.key(kKeyReturn);
        u.frames(2);
    }
    if (dt.menu()) u.key(kKeyF10);
    u.frames(20);
    Config::global().set_bool("debug_mode", false);
    u.frames(5);
    int here = 0;
    for (int mi : p.members) here += u.w().actor(p.state[size_t(mi)].id)->mapnum == 40;
    std::printf("  All join: %zu members, %d of them on this map\n", p.members.size(), here);
    CHECK_EQ(p.members.size(), 6u);
}

// A companion told to wait at the lake is there in the other state of the lake too.
void test_twin(const GameData& d) {
    std::printf("the two states of Carthax Lake\n");
    Ui u(d);
    u.frames(40);
    u.beside_home("varn");
    CHECK_EQ(unsigned(u.a("varn")->mapnum), 16u);
    u.p().join(u.idx("wynna"));
    u.frames(5);
    u.p().wait(u.idx("wynna"));
    const int wx = u.a("wynna")->x;
    u.warp(40, 13038, 15042, 48);
    u.warp(26, 20200, 9800, 104);
    u.frames(5);
    CHECK_EQ(unsigned(u.a("varn")->mapnum), 26u);
    CHECK(u.a("varn")->chunk >= 0);
    CHECK_EQ(unsigned(u.a("wynna")->mapnum), 26u);
    CHECK_EQ(u.a("wynna")->x, wx);
    // A save there, loaded: he is not sent "home" to the other state.
    std::vector<uint8_t> a = save_bytes(u.w());
    Ui u2(d);
    u2.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u2.w().load_game(r));
    u2.frames(3);
    CHECK_EQ(unsigned(u2.a("varn")->mapnum), 26u);
    CHECK(u2.a("varn")->chunk >= 0);
}

// A save made when every companion waited on the beach (the build before homes): `beach_always`
// reproduces that state; the old walk back to the beach (kReturn) too.
void test_old_save(const GameData& d) {
    std::printf("an old save with the companions on the beach\n");
    Ui old(d);
    old.p().beach_always = true;
    old.frames(40);
    for (const std::string& id : kIds) CHECK_EQ(unsigned(old.a(id)->mapnum), 3u);
    Party& p = old.p();
    // Morvain follows; Kalvax waits; Brannoc parted and walking home; Corvin has been dead on the beach.
    p.join(old.idx("morvain"));
    p.join(old.idx("kalvax"));
    p.join(old.idx("brannoc"));
    old.frames(20);
    p.wait(old.idx("kalvax"));
    p.part_ways(old.idx("brannoc"));
    old.st("brannoc").mode = CompanionState::kReturn;
    old.a("corvin")->set(Actor::kDead, true);
    old.st("corvin").mode = CompanionState::kDead;
    old.frames(5);
    const int kx = old.a("kalvax")->x, bx = old.a("brannoc")->x, cx = old.a("corvin")->x;
    std::vector<uint8_t> a = save_bytes(old.w());
    Ui u(d);
    u.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u.w().load_game(r));
    u.frames(3);
    for (const char* id : {"varn", "ilvaran", "wynna", "nerissa"}) {
        const CompanionDef& h = u.def(id);
        const Actor* x = u.a(id);
        CHECK_EQ(unsigned(x->mapnum), h.home_map);
        CHECK(x->x == h.home[0] && x->y == h.home[1] && x->z == h.home[2]);
        CHECK(x->chunk < 0);
        CHECK(u.st(id).mode == CompanionState::kIdle);
    }
    CHECK(u.p().in_party(u.idx("morvain")));
    CHECK_EQ(unsigned(u.a("morvain")->mapnum), 3u);
    CHECK_EQ(u.a("kalvax")->x, kx);
    CHECK(u.st("kalvax").mode == CompanionState::kWait);
    CHECK_EQ(u.a("brannoc")->x, bx);
    CHECK(u.st("brannoc").mode == CompanionState::kIdle);
    CHECK_EQ(unsigned(u.a("brannoc")->mapnum), 3u);
    CHECK_EQ(u.a("corvin")->x, cx);  // dead stays dead, where he fell
    CHECK(u.a("corvin")->is(Actor::kDead));
    // Saves still round-trip.
    std::vector<uint8_t> b = save_bytes(u.w());
    Ui u2(d);
    u2.frames(5);
    SaveReader r2(b.data(), b.size());
    CHECK(u2.w().load_game(r2));
    u2.frames(3);
    CHECK_EQ(unsigned(u2.a("varn")->mapnum), u2.def("varn").home_map);
}

// Companion safety (PLAN-LEVELING.md 1): not behind the Leveling option, off in this Ui.
void test_safety_unrecruited(const GameData& d) {
    std::printf("the never-recruited cannot be hurt, monsters ignore them\n");
    Ui u(d);
    u.frames(40);
    u.beside_home("corvin");
    u.frames(5);
    Actor* c = u.a("corvin");
    const uint16_t cid = c->id;
    // The ghoul's enemies are the companions' side, not this Avatar: only Corvin could be its target.
    u.w().avatar()->alignment = 0;
    u.w().avatar()->set(Actor::kInvincible, true);
    const uint16_t hp = c->hp;
    // A monster's blow, a blast with no attacker, a killing blow, the Avatar's own: nothing.
    uint16_t g = u.w().create_monster(142, 0);
    u.w().move_to(g, c->x + 48, c->y, c->z, false);
    u.w().receive_hit(cid, g, 0, 5, kDmgPierce);
    CHECK_EQ(c->hp, hp);
    u.w().receive_hit(cid, 0, 0, 9999, kDmgPierce | kDmgFire);
    CHECK_EQ(c->hp, hp);
    CHECK(!c->is(Actor::kDead));
    u.w().receive_hit(cid, 1, 0, 9999, kDmgPierce);
    CHECK_EQ(c->hp, hp);
    // In combat right beside him for 20 s (it would find him as its nearest enemy): it does not take him.
    u.w().set_in_combat(g);
    Actor* ga = u.w().actor(g);
    CHECK(ga != nullptr);
    CHECK(is_enemy(*ga, *c));
    CHECK(ga->flags & kItemFastArea);
    bool targeted = false;
    for (int i = 0; i < 600 && ga; ++i) {
        u.frames(1);
        ga = u.w().actor(g);
        if (ga && ga->target == cid) targeted = true;
    }
    CHECK(!targeted);
    CHECK(c->hp >= hp);  // (they regain a little by themselves)
    CHECK(!c->is(Actor::kDead));
    CHECK(u.st("corvin").mode == CompanionState::kIdle);
    // Even told to: a fixed target of his is not a valid one.
    if (ga) {
        ga->target = ga->fixed_target = cid;
        u.frames(900);
        CHECK(ga->target != cid);
        CHECK(c->hp >= hp);
    }
    // Recruited, it is an ordinary risk again.
    u.p().join(u.idx("corvin"));
    CHECK(!u.p().untouchable(cid));
    const int h1 = c->hp;
    u.w().receive_hit(cid, g, 0, 3, kDmgPierce);
    CHECK_EQ(c->hp, h1 - 3);
    // Parting does not make them safe again (they have been recruited).
    u.p().part_ways(u.idx("corvin"));
    u.frames(2);
    u.w().receive_hit(cid, g, 0, 3, kDmgPierce);
    CHECK(c->hp < h1);
    // Companions off: no such rule (nor any companion).
    u.p().enabled = false;
    CHECK(!u.p().untouchable(cid));
    u.p().enabled = true;
}

void test_safety_home(const GameData& d) {
    std::printf("the dead go home alive when the Avatar leaves\n");
    Ui u(d);
    u.frames(40);
    Party& p = u.p();
    const std::string found = "found_again";
    for (const std::string& id : kIds) CHECK(u.def(id).barks.count(found) && !u.def(id).barks.at(found).empty());
    // Brannoc joins, walks with the Avatar to the Cemetery, dies there.
    const CompanionDef& bh = u.def("brannoc");
    u.beside_home("brannoc");
    u.frames(5);
    p.join(u.idx("brannoc"));
    u.frames(5);
    u.warp(6, 7360 + 300, 27120, 8);
    u.frames(5);
    Actor* b = u.a("brannoc");
    CHECK_EQ(unsigned(b->mapnum), 6u);
    const size_t items = b->contents.size();
    CHECK(items > 0);
    b->hp = 3;
    b->mana = 0;
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(60);
    CHECK(u.st("brannoc").mode == CompanionState::kDead);
    CHECK(!p.in_party(u.idx("brannoc")));
    // While the Avatar stays he is a body where he fell, and can be raised on the spot.
    u.frames(60);
    CHECK(b->is(Actor::kDead));
    CHECK_EQ(unsigned(b->mapnum), 6u);
    CHECK_EQ(b->contents.size(), items);
    CHECK(p.revive(u.idx("brannoc")));
    CHECK(!b->is(Actor::kDead));
    CHECK(p.in_party(u.idx("brannoc")));
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(60);
    // A body opened keeps its window until he leaves.
    u.w().use_item(b->id);
    u.frames(2);
    CHECK(u.w().gumps.container_gump(b->id) != nullptr);
    // The Avatar leaves the map: he is home, alive, with his things, not in the party.
    u.warp(3, 14300, 6150, 0);
    u.frames(3);
    CHECK(!b->is(Actor::kDead));
    CHECK(u.st("brannoc").mode == CompanionState::kIdle);
    CHECK_EQ(unsigned(b->mapnum), bh.home_map);
    CHECK(b->x == bh.home[0] && b->y == bh.home[1] && b->z == bh.home[2]);
    CHECK_EQ(b->contents.size(), items);
    CHECK_EQ(int(b->hp), p.max_hp(*b));
    CHECK(!p.in_party(u.idx("brannoc")));
    CHECK(u.st("brannoc").joined_before);
    CHECK(u.st("brannoc").flags.count("came_home"));
    CHECK(!u.w().gumps.container_gump(b->id));
    // Saved and loaded, the line is still owed.
    {
        std::vector<uint8_t> s = save_bytes(u.w());
        Ui v(d);
        v.frames(5);
        SaveReader r(s.data(), s.size());
        CHECK(v.w().load_game(r));
        v.frames(3);
        CHECK(v.st("brannoc").flags.count("came_home"));
        CHECK(!v.a("brannoc")->is(Actor::kDead));
    }
    // Found: he says his line, once. The player can recruit him again.
    u.beside_home("brannoc");
    u.frames(30);
    CHECK(!u.st("brannoc").flags.count("came_home"));
    CHECK(std::count(p.log.begin(), p.log.end(), "brannoc bark found_again") == 1);
    bool said = false;
    for (const Bark& k : u.w().barks)
        if (k.speaker == b->id) said = true;
    CHECK(said);
    u.shot("companion_found_again");
    u.frames(60);
    CHECK(std::count(p.log.begin(), p.log.end(), "brannoc bark found_again") == 1);
    for (int i = 0; i < 600 && !u.w().barks.empty(); ++i) u.frames(1);
    b = u.a("brannoc");
    u.double_click_actor(b->id);
    CHECK(u.answer("join me"));
    u.finish_talk();
    CHECK(p.in_party(u.idx("brannoc")));

    // Spoken to before the line came: it opens the conversation.
    p.part_ways(u.idx("brannoc"));
    u.frames(3);
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(30);
    u.warp(6, 7360 + 300, 27120, 8);
    u.frames(3);
    CHECK(!b->is(Actor::kDead));
    CHECK(u.st("brannoc").flags.count("came_home"));
    CHECK_EQ(unsigned(b->mapnum), bh.home_map);
}

void test_safety_old_saves(const GameData& d) {
    std::printf("old saves with dead companions\n");
    // Never recruited, killed in an old build where they could be, on the Avatar's map: a body...
    Ui u(d);
    u.frames(40);
    u.beside_home("morvain");
    u.frames(5);
    Actor* m = u.a("morvain");
    const size_t items = m->contents.size();
    u.w().kill_actor(m->id, kDmgNormal);
    u.frames(60);
    CHECK(u.st("morvain").mode == CompanionState::kDead);
    // ... and another, in the same save, who died on a map the Avatar has already left. As the old
    // build left them: dead, on that map, with the mode saved so. Recruited: Nerissa.
    Actor* n = u.a("nerissa");
    u.st("nerissa").joined_before = true;
    n->set(Actor::kDead, true);
    n->mapnum = 45;
    n->x = 5000; n->y = 5000; n->z = 0;
    u.st("nerissa").mode = CompanionState::kDead;
    u.st("nerissa").before_death = CompanionState::kFollow;
    std::vector<uint8_t> s = save_bytes(u.w());
    Ui v(d);
    v.frames(5);
    SaveReader r(s.data(), s.size());
    CHECK(v.w().load_game(r));
    v.frames(3);
    // On arrival: Nerissa is home at once; Morvain, here, is still a body.
    const CompanionDef& nh = v.def("nerissa");
    Actor* nv = v.a("nerissa");
    CHECK(!nv->is(Actor::kDead));
    CHECK(v.st("nerissa").mode == CompanionState::kIdle);
    CHECK_EQ(unsigned(nv->mapnum), nh.home_map);
    CHECK(nv->x == nh.home[0] && nv->y == nh.home[1]);
    Actor* mv = v.a("morvain");
    CHECK(mv->is(Actor::kDead));
    CHECK_EQ(unsigned(mv->mapnum), v.def("morvain").home_map);
    // The Avatar leaves: Morvain, never recruited, goes home too (he lies there already) and may join.
    v.warp(3, 14300, 6150, 0);
    v.frames(3);
    CHECK(!mv->is(Actor::kDead));
    CHECK(v.st("morvain").mode == CompanionState::kIdle);
    CHECK_EQ(unsigned(mv->mapnum), v.def("morvain").home_map);
    CHECK_EQ(mv->contents.size(), items);
    CHECK(v.p().join(v.idx("morvain")));
}

void test_safety_debug(const GameData& d) {
    std::printf("Debug Mode's beach with the dead coming home\n");
    Ui u(d);
    u.frames(40);
    Party& p = u.p();
    // Brannoc joined and fell; Kalvax (an old save's case) fell without ever joining.
    u.beside_home("brannoc");
    u.frames(5);
    p.join(u.idx("brannoc"));
    u.frames(3);
    Actor* b = u.a("brannoc");
    Actor* k = u.a("kalvax");
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(30);
    Config::global().set_bool("debug_mode", true);
    u.frames(3);
    CHECK(b->is(Actor::kDead));  // the toggle alone moves no body on this map
    k->set(Actor::kDead, true);
    u.st("kalvax").mode = CompanionState::kDead;
    u.frames(1);
    CHECK_EQ(unsigned(k->mapnum), 3u);
    CHECK(!k->is(Actor::kDead));  // he was elsewhere: to the beach at once
    // The Avatar leaves. The one recruited goes to his own home; the never-recruited to the beach with
    // the others, as the rule of the debug gathering says.
    u.warp(3, 14300, 6150, 0);
    u.frames(3);
    CHECK(!b->is(Actor::kDead));
    CHECK_EQ(unsigned(b->mapnum), u.def("brannoc").home_map);
    CHECK_EQ(b->x, u.def("brannoc").home[0]);
    CHECK_EQ(unsigned(k->mapnum), 3u);
    CHECK(u.st("kalvax").mode == CompanionState::kIdle);
    // Debug off: the never-recruited go home; the one recruited is left alone.
    Config::global().set_bool("debug_mode", false);
    u.frames(3);
    CHECK_EQ(unsigned(k->mapnum), u.def("kalvax").home_map);
    CHECK_EQ(k->x, u.def("kalvax").home[0]);
    CHECK_EQ(unsigned(b->mapnum), u.def("brannoc").home_map);
}

// Dying gives nothing extra: a companion in a monster's shape keeps only what they carry.
void test_safety_body(const GameData& d) {
    std::printf("a body holds only their own things\n");
    Ui u(d);
    u.frames(40);
    u.beside_home("morvain");  // his shape has a monster row with treasure
    u.frames(5);
    Actor* m = u.a("morvain");
    CHECK(monster_info(m->shape) != nullptr);
    const size_t items = m->contents.size();
    for (int i = 0; i < 4; ++i) {
        u.w().kill_actor(m->id, kDmgNormal);
        u.frames(5);
        CHECK_EQ(m->contents.size(), items);
        CHECK(u.p().revive(u.idx("morvain")));
        u.frames(5);
    }
}

// Parting at the far side of the world stays there; talking to them there still works.
void test_parted_stays(const GameData& d) {
    std::printf("parted companions stay where they parted\n");
    Ui u(d);
    u.frames(40);
    Party& p = u.p();
    u.beside_home("nerissa");
    p.join(u.idx("nerissa"));
    u.frames(5);
    u.warp(7, 15071, 26239, 8);
    u.frames(30);
    p.part_ways(u.idx("nerissa"));
    const Actor* n = u.a("nerissa");
    const int nx = n->x, ny = n->y;
    u.frames(600);
    CHECK(n->x == nx && n->y == ny);
    CHECK_EQ(unsigned(n->mapnum), 7u);
    u.warp(3, 14527, 5887, 8);
    u.frames(5);
    CHECK_EQ(unsigned(n->mapnum), 7u);
    CHECK(n->chunk < 0);
    std::vector<uint8_t> a = save_bytes(u.w());
    Ui u2(d);
    u2.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u2.w().load_game(r));
    u2.frames(3);
    CHECK_EQ(unsigned(u2.a("nerissa")->mapnum), 7u);
    CHECK_EQ(u2.a("nerissa")->x, nx);
    u2.warp(7, nx + 90, ny - 60, n->z);
    u2.frames(10, true);
    u2.double_click_actor(u2.a("nerissa")->id);
    CHECK(u2.p().talking());
    CHECK(u2.answer("join me"));
    CHECK(u2.wait_menu());
    CHECK(u2.p().in_party(u2.idx("nerissa")));
}

// A copy of the game with a changed map: only the companions living there are affected.
void test_compat_fallback(const std::string& game) {
    namespace fs = std::filesystem;
    std::printf("a copy of the game with other maps\n");
    fs::path dst = fs::path(g_saves) / "copy";
    fs::remove_all(dst);
    fs::create_directories(dst);
    for (const char* sub : {"STATIC", "GAMEDAT", "USECODE", "SOUND"}) {
        GameFS g;
        g.open(game);
        std::string src = g.find(sub);
        if (!src.empty()) fs::copy(src, dst / sub, fs::copy_options::recursive);
    }
    for (auto& e : fs::recursive_directory_iterator(dst))
        if (e.is_regular_file()) fs::permissions(e.path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add);
    auto edit = [&](unsigned map) {
        fs::path f = dst / "STATIC/FIXED.DAT";
        std::ifstream in(f, std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        size_t off = size_t(uint8_t(b[0x80 + 8 * map])) | size_t(uint8_t(b[0x81 + 8 * map])) << 8 | size_t(uint8_t(b[0x82 + 8 * map])) << 16 |
                     size_t(uint8_t(b[0x83 + 8 * map])) << 24;
        b[off + 3] ^= 1;
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o.write(b.data(), std::streamsize(b.size()));
    };
    edit(6);
    {
        GameFS g;
        CHECK(g.open(dst.string()));
        GameData cd;
        CHECK(cd.load(g));
        CHECK(!compat::fixed_map_known(6));
        Ui u(cd);
        u.frames(40);
        CHECK_EQ(unsigned(u.a("morvain")->mapnum), 3u);  // the beach
        CHECK_EQ(u.a("morvain")->y, u.def("morvain").spawn[1]);
        CHECK_EQ(unsigned(u.a("kalvax")->mapnum), u.def("kalvax").home_map);  // the others at home
        bool said = false;
        for (const std::string& s : compat::switched_off()) said = said || s.find("Morvain") != std::string::npos;
        CHECK(said);
    }
    edit(3);  // no beach either: Morvain does not appear, Kalvax (map 12) still does, Corvin (home on map 3) not
    {
        GameFS g;
        CHECK(g.open(dst.string()));
        GameData cd;
        CHECK(cd.load(g));
        Ui u(cd);
        u.frames(40);
        CHECK(u.st("morvain").mode == CompanionState::kUnspawned);
        CHECK(u.st("corvin").mode == CompanionState::kUnspawned);
        CHECK_EQ(unsigned(u.a("kalvax")->mapnum), u.def("kalvax").home_map);
    }
    fs::remove_all(dst);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_homes_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    if (only.rfind("probe:", 0) == 0) {
        int b[5] = {0, 0, 0, 0, 256};
        std::sscanf(only.c_str() + 6, "%d,%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3], &b[4]);
        probe(d, unsigned(b[0]), b[1], b[2], b[3], b[4]);
        return 0;
    }
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("critpath")) test_critpath();
    if (want("spots")) test_spots(d, false, false);
    if (want("stitched")) test_spots(d, true, false);
    if (want("invasion")) test_spots(d, true, true);
    if (want("recruit")) test_recruit(d);
    if (want("seam")) test_seam(d);
    if (want("debug")) test_debug(d);
    if (want("twin")) test_twin(d);
    if (want("oldsave")) test_old_save(d);
    if (want("parted")) test_parted_stays(d);
    if (want("safety")) test_safety_unrecruited(d);
    if (want("safetyhome")) test_safety_home(d);
    if (want("safetyold")) test_safety_old_saves(d);
    if (want("safetydebug")) test_safety_debug(d);
    if (want("safetybody")) test_safety_body(d);
    if (want("compat")) test_compat_fallback(root);  // last: it changes what compat knows
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
