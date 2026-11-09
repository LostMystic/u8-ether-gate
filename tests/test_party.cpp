// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Companions (game/party.h, PLAN-COMPANIONS.md) through the real game mode: the beach spawn
// after the opening, conversations through the answer menu (join / wait / follow / part ways /
// rejoin), the party cap, following on foot and through a teleporter into Tenebrae, the Avatar
// walking through companions, scenes, combat with a monster (companions fight, healers heal),
// death -> body -> resurrection, trading items (weight limits), lock picking and stealing,
// a save / load round trip with a full party, Companions Off = the original game, and following
// across hazards (stepping stones over water, lava, a deadly drop: they wait, then regroup).
//   pagan_party_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR]
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/assist.h"
#include "game/combat.h"
#include "game/party.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool skip, bool companions = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = skip;
        w.ask_name = false;
        w.auto_ui = !skip;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->enabled = companions;
        w.party->beach_always = true;  // these tests meet everyone on the beach (homes: tests/test_homes.cpp)
        w.banter_one_in = 1;  // these tests count on the barks (and the random numbers) as they were
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    void frames(int n, bool draw = true) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            if (draw) gm.render(surf);
        }
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
    void double_click_actor(uint16_t id) {
        const Item* it = w().item(id);
        int sx, sy;
        w().screen_point(*it, sx, sy);
        // A pixel of this actor (companions stand close together on the beach).
        gm.render(surf);
        int px = sx - 2, py = sy - 22;
        bool found = false;
        for (int dy = -40; dy <= 4 && !found; dy += 2)
            for (int dx = -12; dx <= 12 && !found; dx += 2)
                if (gm.pick(sx + dx, sy + dy) == id) { px = sx + dx; py = sy + dy; found = true; }
        move(px, py);
        frames(2);
        button(1, true); button(1, false);
        frames(1, false);
        button(1, true); button(1, false);
        frames(2);
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        move(316, 196);
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 2);
        std::printf("  shot %s\n", path.c_str());
    }
    // A close-up: the 320 x 200 frame cropped round a point, at `scale` (Nerissa's shots).
    void shot_crop(const std::string& name, int cx, int cy, int cw, int ch, int scale = 3) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        int x0 = std::clamp(cx - cw / 2, 0, 320 - cw), y0 = std::clamp(cy - ch / 2, 0, 200 - ch);
        std::vector<uint32_t> crop(size_t(cw) * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) crop[size_t(y) * cw + x] = rgba[size_t(y0 + y) * 320 + x0 + x];
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, crop.data(), cw, ch, scale);
        std::printf("  shot %s\n", path.c_str());
    }
    void shot_actor(const std::string& name, uint16_t id, int cw = 120, int ch = 90) {
        const Item* it = w().item(id);
        if (!it) return;
        int sx, sy;
        w().screen_point(*it, sx, sy);
        shot_crop(name, sx, sy - 20, cw, ch);
    }
    // Waits for the answer menu (up to n frames); true if it is there.
    bool wait_menu(int n = 900) {
        for (int i = 0; i < n && !w().ask_menu; ++i) {
            frames(1, false);
            // Click through the companion's lines quickly.
            if (!w().barks.empty() && i % 3 == 0) key(kKeyReturn);
        }
        return w().ask_menu != nullptr;
    }
    std::vector<std::string> answers() {
        std::vector<std::string> out;
        if (w().ask_menu)
            for (uint16_t s : w().ask_menu->answers) out.push_back(w().vm().string(s));
        return out;
    }
    bool answer(const std::string& label) {
        if (!wait_menu()) { std::printf("  no answer menu (for '%s')\n", label.c_str()); return false; }
        std::vector<std::string> a = answers();
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] == label) {
                key('1' + int(i));
                frames(2, false);
                return true;
            }
        std::printf("  no answer '%s' in:", label.c_str());
        for (auto& s : a) std::printf(" [%s]", s.c_str());
        std::printf("\n");
        return false;
    }
    // Lets the conversation finish (says its lines, closes).
    void finish_talk(int n = 900) {
        for (int i = 0; i < n && p().talking(); ++i) {
            frames(1, false);
            if (!w().barks.empty() && i % 3 == 0) key(kKeyReturn);
            if (w().ask_menu && i > 20) answer("bye");
        }
    }
    uint16_t cid(const std::string& name) { int i = p().find_def(name); return i >= 0 ? p().state[size_t(i)].id : 0; }
    Actor* ca(const std::string& name) { return w().actor(cid(name)); }
    int idx(const std::string& name) { return p().find_def(name); }
    void avatar_to(int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().move_to(1, x, y, z, false);
        frames(2);
    }
};

// A free, supported spot for a shape in a ring round (x, y).
bool find_free(World& w, unsigned shape, int x, int y, int z, int r0, int r1, uint16_t ignore, int& ox, int& oy, int& oz) {
    for (int r = r0; r <= r1; r += 16)
        for (int k = 0; k < 16; ++k) {
            double ang = 6.2831853 * k / 16;
            int px = x + int(r * std::cos(ang)), py = y + int(r * std::sin(ang));
            for (int dz : {0, 8, -8, 16}) {
                if (z + dz < 0) continue;
                World::Placement pl = w.can_exist_shape(shape, px, py, z + dz, ignore);
                if (pl.valid && pl.supported && pl.land) { ox = px; oy = py; oz = z + dz; return true; }
            }
        }
    return false;
}

int dist(const Item& a, const Item& b) { return int(std::hypot(double(a.x - b.x), double(a.y - b.y))); }

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

// ---------------------------------------------------------------------------------------------

// Not a test: prints a map of free, supported, dry spots in a box (choosing beach spots).
//   pagan_party_tests GAME DATA SHOTS SAVES probe:X0,X1,Y0,Y1
void probe(const GameData& d, int x0, int x1, int y0, int y1) {
    Ui u(d, true, false);
    u.frames(10, false);
    u.avatar_to((x0 + x1) / 2, (y0 + y1) / 2, 8);  // the fast area (and its expanded globs) round the box
    u.frames(10, false);
    const int st = 32;
    std::printf("x from %d step %d; digits = z/8, ~ water, ! damaging\n", x0, st);
    for (int y = y0; y <= y1; y += st) {
        std::printf("%5d ", y);
        for (int x = x0; x <= x1; x += st) {
            char c = '.';
            for (int z : {0, 8, 16, 24, 32, 40}) {
                World::Placement pl = u.w().can_exist_shape(574, x, y, z, 0);
                if (!pl.valid || !pl.supported || !pl.land) continue;
                const Item* l = u.w().item(pl.land);
                if (!l) continue;
                const ShapeInfo& li = d.typeflags.get(l->shape);
                if (li.is(ShapeInfo::kDamaging)) { c = '!'; continue; }
                if (li.is(ShapeInfo::kSea) && !li.is(ShapeInfo::kLand)) { c = '~'; continue; }
                c = char('0' + z / 8);
                break;
            }
            std::putchar(c);
        }
        std::printf("\n");
    }
}

// Not a test: the surface under each point of a box on any map (finding platforming spots).
//   pagan_party_tests GAME DATA SHOTS SAVES surf:MAP,X0,X1,Y0,Y1,STEP[,ZMAX]
// Each cell: the top surface's height (z / 8, 0-9 then a-z), water A-Z (its z / 8), ! damaging, % lava, space none.
void probe_surface(const GameData& d, int map, int x0, int x1, int y0, int y1, int st, int zmax) {
    Ui u(d, true, false);
    u.frames(10, false);
    if (map != int(u.w().map().map_number())) u.w().change_map(unsigned(map));
    u.avatar_to((x0 + x1) / 2, (y0 + y1) / 2, 0);
    u.frames(10, false);
    std::printf("map %d x from %d step %d\n", map, x0, st);
    for (unsigned sh : {347u, 438u, 409u})
        std::printf("shape %u flags %x (sea %d land %d damaging %d)\n", sh, d.typeflags.get(sh).flags(), d.typeflags.get(sh).is(ShapeInfo::kSea),
                    d.typeflags.get(sh).is(ShapeInfo::kLand), d.typeflags.get(sh).is(ShapeInfo::kDamaging));
    for (int y = y0; y <= y1; y += st) {
        std::printf("%5d ", y);
        for (int x = x0; x <= x1; x += st) {
            int top;
            uint16_t f, id;
            char c = ' ';
            if (assist::surface_under(u.w(), x, y, zmax, 1, top, f, id)) {
                if (f & ShapeInfo::kDamaging) c = '!';
                else if (u.w().item(id)->shape == 409 || u.w().item(id)->shape == 483) c = '%';
                else if ((f & ShapeInfo::kSea) && !(f & ShapeInfo::kLand)) c = char('A' + std::clamp(top / 8, 0, 25));
                else { int h = std::clamp(top / 8, 0, 35); c = char(h < 10 ? '0' + h : 'a' + h - 10); }
            }
            std::putchar(c);
        }
        std::printf("\n");
    }
}

void test_spawn_after_opening(const GameData& d) {
    std::printf("spawn after the opening\n");
    Ui u(d, false);
    bool spawned_in_stasis = false;
    int released = -1;
    for (int f = 0; f < 30000; ++f) {
        u.frames(1, false);
        if (u.w().stasis && u.p().spawned_once) spawned_in_stasis = true;
        if (!u.w().stasis && released < 0 && u.w().startup_state) released = f;
        if (u.p().spawned_once && released >= 0) break;
    }
    std::printf("  stasis released at frame %d, companions spawned: %s\n", released, u.p().spawned_once ? "yes" : "no");
    CHECK(released > 100);  // the opening scene played
    CHECK(u.p().spawned_once);
    CHECK(!spawned_in_stasis);
    int n = 0;
    for (const CompanionState& s : u.p().state) n += s.mode == CompanionState::kIdle;
    CHECK_EQ(n, 8);
}

void test_beach(const GameData& d) {
    std::printf("beach lineup\n");
    Ui u(d, true);
    u.frames(60);
    CHECK(u.p().spawned_once);
    const Actor* av = u.w().avatar();
    int reach = 0;
    for (size_t i = 0; i < u.p().defs.size(); ++i) {
        const Actor* a = u.w().actor(u.p().state[i].id);
        CHECK(a != nullptr);
        if (!a) continue;
        CHECK(a->id >= 62 && a->id <= 255);   // not one of the 61 permanent NPCs
        CHECK(a->shape == u.p().defs[i].shape);
        CHECK(a->chunk >= 0 && a->mapnum == 3);
        CHECK(!(a->ext_flags & kExtPermanentNpc));
        int dd = dist(*a, *av);
        // Reachable: the path planner finds a way from near the Avatar to each one.
        Pathfinder pf(u.w());
        pf.set_actor(1);
        pf.set_target_item(a->id, false);
        std::vector<PathStep> path;
        bool ok = pf.plan(path);
        if (!ok) {
            // The planner's 200-node limit: plan from half way along instead.
            ActorState st;
            st.x = (av->x + a->x) / 2; st.y = (av->y + a->y) / 2; st.z = a->z;
            Pathfinder pf2(u.w());
            pf2.set_actor(1, &st);
            pf2.set_target_item(a->id, false);
            ok = pf2.plan(path);
        }
        reach += ok;
        std::printf("  %-16s actor %3u shape %3u at %d,%d,%d, %d units from the Avatar, path %s\n",
                    u.p().defs[i].name.c_str(), a->id, a->shape, a->x, a->y, a->z, dd, ok ? "found" : "NOT found");
        CHECK(dd > 900 && dd < 2200);  // down the beach, about a screen from the start
        // On screen together: the group spans less than a screen.
        const Actor* first = u.w().actor(u.p().state[0].id);
        int sx0 = (first->x - first->y) / 4, sy0 = (first->x + first->y) / 8, sx1 = (a->x - a->y) / 4, sy1 = (a->x + a->y) / 8;
        CHECK(std::abs(sx1 - sx0) < 300 && std::abs(sy1 - sy0) < 160);
    }
    CHECK(reach >= 7);
    {
        // The start and the group in one zoomed-out frame (50%), the camera between them.
        const Actor* m = u.ca("varn");
        u.w().camera_stay((av->x + m->x) / 2, (av->y + m->y) / 2, 8);
        u.w().controls.zoom_step = 2;
        u.frames(2);
        u.move(316, 196);
        std::vector<uint32_t> px;
        int fw = 0, fh = 0;
        u.gm.render_frame(px, fw, fh, 4, 0, 0);
        if (!g_shots.empty() && fw > 0) {
            mkdir(g_shots.c_str(), 0755);
            save_png(g_shots + "/party_beach_lineup.png", px.data(), fw, fh, 1);
            std::printf("  shot %s/party_beach_lineup.png (%dx%d, 50%%)\n", g_shots.c_str(), fw, fh);
        }
        u.w().controls.zoom_step = 0;
        u.w().camera_follow(1);
    }
    // Reachable on foot: the Avatar walks from the start to the group in pathfinder hops.
    {
        const Actor* g = u.ca("varn");
        int hops = 0;
        for (; hops < 12 && dist(*av, *g) > 200; ++hops) {
            double dx = g->x - av->x, dy = g->y - av->y, l = std::hypot(dx, dy), k = std::min(1.0, 360.0 / l);
            int hx = av->x + int(dx * k), hy = av->y + int(dy * k);
            u.w().pathfind(1, hx, hy, av->z, 0, 0);
            int f = 0;
            while (f < 600 && u.w().kernel().count_exact(1, kProcPathfind)) { u.frames(1, false); ++f; }
            u.frames(5, false);
        }
        std::printf("  the Avatar walked from the start to Varn in %d pathfinder hops, ending %d units away\n", hops, dist(*av, *g));
        CHECK(dist(*av, *g) <= 200);
        // The path from the camp north to Tenebrae (the graveyard bank) is clear of them.
        for (const CompanionState& st : u.p().state) {
            const Actor* a = u.w().actor(st.id);
            CHECK(a->y > 5800 || a->x > 15500);
        }
    }
}

void test_talk_and_commands(const GameData& d) {
    std::printf("conversation and party commands\n");
    Ui u(d, true);
    u.frames(60);
    Actor* m = u.ca("morvain");
    u.avatar_to(m->x + 80, m->y - 40, m->z);
    u.frames(10);
    u.double_click_actor(m->id);
    CHECK(u.p().talking());
    u.frames(8);
    CHECK(!u.w().barks.empty());  // his greeting, over his head
    u.shot("party_conversation_bark");
    CHECK(u.wait_menu());
    std::vector<std::string> a = u.answers();
    CHECK(std::find(a.begin(), a.end(), "join me") != a.end());
    CHECK(std::find(a.begin(), a.end(), "orders") == a.end());  // not in the party yet
    u.shot("party_conversation");
    CHECK(u.answer("Lithos"));
    CHECK(u.wait_menu());
    a = u.answers();
    CHECK(std::find(a.begin(), a.end(), "necromancy") != a.end());
    CHECK(std::find(a.begin(), a.end(), "back") != a.end());
    CHECK(u.answer("back"));
    CHECK(u.answer("join me"));
    CHECK(u.wait_menu());
    int mi = u.idx("morvain");
    CHECK(u.p().in_party(mi));
    a = u.answers();
    CHECK(std::find(a.begin(), a.end(), "orders") != a.end());
    CHECK(u.answer("orders"));
    CHECK(u.answer("wait here"));
    CHECK(u.wait_menu());
    CHECK(u.p().state[size_t(mi)].mode == CompanionState::kWait);
    CHECK(u.answer("orders"));
    CHECK(u.answer("follow me"));
    CHECK(u.wait_menu());
    CHECK(u.p().state[size_t(mi)].mode == CompanionState::kFollow);
    CHECK(u.answer("bye"));
    u.finish_talk();
    CHECK(!u.p().talking());
    // Part ways: he stays where he is (homes: tests/test_homes.cpp); then rejoins.
    {
        int x, y, z;
        if (find_free(u.w(), 1, m->x - 500, m->y + 100, m->z, 0, 200, 1, x, y, z)) u.avatar_to(x, y, z);
    }
    u.frames(120);
    m = u.ca("morvain");
    {
        int x, y, z;
        if (find_free(u.w(), 1, m->x, m->y, m->z, 64, 160, 1, x, y, z)) u.avatar_to(x, y, z);
    }
    u.frames(20);
    u.double_click_actor(m->id);
    CHECK(u.answer("orders"));
    CHECK(u.answer("part ways"));
    u.finish_talk();
    CHECK(!u.p().in_party(mi));
    CHECK(u.p().state[size_t(mi)].mode == CompanionState::kIdle);
    {
        const int px = m->x, py = m->y;
        u.frames(300, false);
        CHECK(m->x == px && m->y == py);
    }
    m = u.ca("morvain");
    u.avatar_to(m->x + 70, m->y - 30, m->z);
    u.double_click_actor(m->id);
    CHECK(u.answer("join me"));
    u.finish_talk();
    CHECK(u.p().in_party(mi));
    // Kalvax refuses while Ilvaran travels with us, until persuaded.
    int ii = u.idx("ilvaran"), ki = u.idx("kalvax");
    CHECK(u.p().join(ii));
    Actor* k = u.ca("kalvax");
    u.avatar_to(k->x + 70, k->y - 30, k->z);
    u.double_click_actor(k->id);
    CHECK(u.answer("join me"));
    CHECK(u.wait_menu());
    CHECK(!u.p().in_party(ki));
    a = u.answers();
    CHECK(std::find(a.begin(), a.end(), "he means no harm") != a.end());
    CHECK(u.answer("he means no harm"));
    CHECK(u.answer("join me"));
    u.finish_talk();
    CHECK(u.p().in_party(ki));
    // The cap: six.
    for (size_t i = 0; i < u.p().defs.size(); ++i) u.p().join(int(i));
    CHECK_EQ(int(u.p().members.size()), 6);
    int out = -1;
    for (size_t i = 0; i < u.p().defs.size(); ++i)
        if (!u.p().in_party(int(i))) out = int(i);
    CHECK(out >= 0);
    CHECK(!u.p().join(out));
    Actor* o = u.w().actor(u.p().state[size_t(out)].id);
    u.avatar_to(o->x + 70, o->y - 30, o->z);
    u.double_click_actor(o->id);
    CHECK(u.answer("join me"));
    u.finish_talk();
    CHECK(!u.p().in_party(out));
    bool refused = false;
    for (const std::string& l : u.p().log) refused |= l.find("talk") != std::string::npos;
    CHECK(refused);
    CHECK_EQ(int(u.p().members.size()), 6);
}

// The Avatar walks, the party follows; through a teleporter into Tenebrae.
void test_follow(const GameData& d) {
    std::printf("following\n");
    Ui u(d, true);
    u.frames(60);
    for (const char* n : {"brannoc", "varn", "ilvaran", "nerissa"}) u.p().join(u.idx(n));
    const Actor* av = u.w().avatar();
    // The Avatar walks through a companion (no collision with party members).
    {
        Actor* b = u.ca("brannoc");
        u.avatar_to(14400, 6100, 0);
        u.frames(5);
        u.w().kernel().kill(b->id, kProcAny);
        u.w().move_to(b->id, av->x + 48, av->y, av->z, false);
        World::Placement pl = u.w().can_exist(av->x + 48, av->y, av->z, 32, 32, 40, u.w().shape_flags(*av), 1);
        CHECK(pl.blocker != b->id);
        AnimTracker tr(u.w());
        tr.init(1, 0, 2);
        AnimTracker::Result r = tr.run_all();
        std::printf("  Avatar walks east through Brannoc: %s, to %d (from %d)\n", r == AnimTracker::kOk ? "ok" : "blocked", tr.x(), av->x);
        CHECK(r == AnimTracker::kOk && tr.x() > b->x - 16);
    }
    // Walk from the beach to the camp and up toward the graveyard (the milestone 3 route).
    u.avatar_to(14200, 6300, 0);
    u.frames(60);
    u.w().pathfind(1, 14527, 5887, 8, 0, 0);
    u.frames(240);
    std::printf("  Avatar at %d,%d\n", av->x, av->y);
    int near = 0;
    for (int m : u.p().members) {
        const Actor* a = u.w().actor(u.p().state[size_t(m)].id);
        int dd = dist(*a, *av);
        std::printf("  %-16s %d units behind\n", u.p().defs[size_t(m)].name.c_str(), dd);
        near += dd < 420;
    }
    CHECK(near >= 3);
    u.shot("party_following");
    // A teleporter into Central Tenebrae (the gate's teleport egg, as check_eggs runs it).
    unsigned dest_egg = 0;
    bool found = false;
    for (size_t i = 512; i < u.w().objects().slots() && !found; ++i) {
        const Item* e = u.w().item(uint16_t(i));
        if (e && e->chunk >= 0 && d.typeflags.get(e->shape).family() == kFamTeleportEgg && e->frame != 1 && e->mapnum == 40) {
            dest_egg = e->quality & 0xFF;
            found = true;
        }
    }
    CHECK(found);
    u.w().kernel().add(std::make_unique<TeleportProcess>(u.w(), 40, dest_egg));
    u.frames(20);
    CHECK_EQ(u.w().map().map_number(), 40u);
    near = 0;
    for (int m : u.p().members) {
        const Actor* a = u.w().actor(u.p().state[size_t(m)].id);
        bool here = a->mapnum == 40 && a->chunk >= 0;
        std::printf("  %-16s map %u, %d units from the Avatar\n", u.p().defs[size_t(m)].name.c_str(), a->mapnum, here ? dist(*a, *av) : -1);
        near += here && dist(*a, *av) < 400;
    }
    CHECK_EQ(near, 4);
    // The others stayed on map 3, and are back when we return there.
    const Actor* morv = u.ca("morvain");
    CHECK(morv->mapnum == 3 && morv->chunk < 0);
    u.frames(60);
    u.shot("party_tenebrae");
    // A far usecode-style teleport on the same map: they catch up.
    // (Onto ground, as a script's teleport puts him: they only ever join him on safe ground.)
    {
        int tx = av->x + 2500, ty = av->y, tz = av->z;
        u.w().teleport_actor(1, tx, ty, tz, 40);  // the fast area round the destination
        u.frames(2);
        CHECK(find_free(u.w(), av->shape, tx, ty, 48, 0, 400, 1, tx, ty, tz));
        u.w().teleport_actor(1, tx, ty, tz, 40);
    }
    u.frames(200);
    near = 0;
    for (int m : u.p().members) near += dist(*u.w().actor(u.p().state[size_t(m)].id), *av) < 500;
    std::printf("  after a 2500-unit jump: %d of 4 caught up\n", near);
    CHECK(near >= 3);
    // Scenes: in stasis they hold still.
    u.w().stasis = true;
    std::vector<int> xs;
    for (int m : u.p().members) xs.push_back(u.w().actor(u.p().state[size_t(m)].id)->x);
    u.w().kernel().kill(1, kProcAny);
    u.w().move_to(1, av->x + 300, av->y, av->z, false);
    u.frames(90, false);
    int moved = 0;
    for (size_t k = 0; k < u.p().members.size(); ++k) moved += u.w().actor(u.p().state[size_t(u.p().members[k])].id)->x != xs[k];
    CHECK_EQ(moved, 0);
    u.w().stasis = false;
}

void test_combat(const GameData& d) {
    std::printf("combat\n");
    Ui u(d, true);
    u.frames(60);
    for (const char* n : {"brannoc", "varn", "ilvaran", "kalvax"}) u.p().join(u.idx(n));
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    Actor* av = u.w().avatar();
    // A wounded Avatar and a ghoul.
    av->hp = uint16_t(std::max(4, int(av->str) / 2));
    int hp_before = av->hp;
    uint16_t g = u.w().create_monster(142, 0);
    int gx = 0, gy = 0, gz = 0;
    bool spot = find_free(u.w(), 142, av->x, av->y, av->z, 220, 480, g, gx, gy, gz);
    CHECK(spot);
    if (!spot) return;
    u.w().move_to(g, gx, gy, gz, false);
    u.frames(2);
    u.w().set_in_combat(g);
    if (Actor* ga = u.w().actor(g)) { ga->target = 1; ga->fixed_target = 1; }
    u.w().set_in_combat(1);
    int ghoul_hp = u.w().actor(g)->hp;
    bool companions_fought = false, healed = false, dead = false, combat_shot = false;
    int f = 0;
    for (; f < 1800 && !dead; ++f) {
        u.frames(1, f % 2 == 0);
        if (int(av->hp) > hp_before) healed = true;
        hp_before = std::min<int>(hp_before, av->hp);
        if (av->is(Actor::kDead)) break;
        const Actor* ga = u.w().actor(g);
        dead = !ga || ga->is(Actor::kDead);
        for (int m : u.p().members)
            if (u.w().actor(u.p().state[size_t(m)].id)->is(Actor::kInCombat)) companions_fought = true;
        if (!combat_shot && ga && int(ga->hp) < ghoul_hp - 4) { u.shot("party_combat"); combat_shot = true; }
    }
    int casts = 0;
    for (const std::string& l : u.p().log) casts += l.find(" casts ") != std::string::npos;
    std::printf("  ghoul (hp %d) %s after %d frames; companions in combat: %s; spells cast: %d; Avatar healed: %s\n",
                ghoul_hp, dead ? "killed" : "alive", f, companions_fought ? "yes" : "no", casts, healed ? "yes" : "no");
    CHECK(dead);
    CHECK(companions_fought);
    CHECK(casts >= 1);
    CHECK(healed);
    CHECK(!av->is(Actor::kDead));
    // Monsters attack companions too: a second ghoul next to Varn draws on him.
    uint16_t g2 = u.w().create_monster(142, 0);
    Actor* varn = u.ca("varn");
    u.w().clear_in_combat(1);
    u.frames(30);
    int px = 0, py = 0, pz = 0;
    // Beyond Varn as seen from the Avatar, so Varn is much the nearer.
    double vdx = varn->x - av->x, vdy = varn->y - av->y, vl = std::max(1.0, std::hypot(vdx, vdy));
    bool placed = find_free(u.w(), 142, varn->x + int(160 * vdx / vl), varn->y + int(160 * vdy / vl), varn->z, 0, 96, g2, px, py, pz);
    std::printf("  second ghoul %d units from Varn, %d from the Avatar\n", int(std::hypot(px - varn->x, py - varn->y)),
                int(std::hypot(px - av->x, py - av->y)));
    CHECK(placed);
    if (placed) u.w().move_to(g2, px, py, pz, false);
    u.w().set_in_combat(g2);
    if (Actor* ga = u.w().actor(g2)) { ga->target = 1; ga->fixed_target = 1; }
    bool on_companion = false;
    for (int i = 0; i < 900 && !on_companion; ++i) {
        u.frames(1, false);
        const Actor* ga = u.w().actor(g2);
        if (!ga || ga->is(Actor::kDead)) break;
        on_companion = u.p().is_companion(ga->target);
    }
    CHECK(on_companion);
    u.w().set_in_combat(1);
    u.frames(900, false);
}

// Every companion, alone with the Avatar (who stands back and does not swing), damages a ghoul
// in real time: melee for the fighters (Brannoc, Corvin: Corvin's shape has no hit frame, the AI
// resolves his blow), spells or melee for the casters.
void test_each_fights(const GameData& d) {
    std::printf("each companion fights\n");
    Ui u(d, true);
    u.frames(60);
    Actor* av = u.w().avatar();
    for (size_t i = 0; i < u.p().defs.size(); ++i) {
        for (int m : std::vector<int>(u.p().members)) u.p().part_ways(m);
        CHECK(u.p().join(int(i)));
        Actor* c = u.w().actor(u.p().state[i].id);
        int x, y, z;
        // A fresh spot on the shore for each bout.
        CHECK(find_free(u.w(), 1, 15000 + int(i) * 40, 5800, 8, 0, 160, 1, x, y, z));
        u.avatar_to(x, y, z);
        av->set(Actor::kInvincible, true);
        u.frames(120, false);
        c->hp = uint16_t(u.p().max_hp(*c));
        c->mana = int16_t(u.p().max_mana(*c));
        c->set(Actor::kInvincible, true);
        uint16_t g = u.w().create_monster(142, 0);
        int gx = 0, gy = 0, gz = 0;
        bool ok = find_free(u.w(), 142, c->x + 120, c->y, c->z, 0, 120, g, gx, gy, gz);
        CHECK(ok);
        if (!ok) continue;
        u.w().move_to(g, gx, gy, gz, false);
        Actor* ga = u.w().actor(g);
        ga->hp = 400;
        u.frames(2, false);
        u.w().set_in_combat(g);
        if ((ga = u.w().actor(g))) { ga->target = c->id; ga->fixed_target = c->id; }
        u.w().set_in_combat(1);
        size_t log0 = u.p().log.size();
        int f = 0;
        for (; f < 900 && (ga = u.w().actor(g)) && ga->hp >= 400; ++f) u.frames(1, false);
        int casts = 0;
        for (size_t k = log0; k < u.p().log.size(); ++k) casts += u.p().log[k].find(" casts ") != std::string::npos;
        ga = u.w().actor(g);
        int lost = ga ? 400 - int(ga->hp) : 0;
        std::printf("  %-16s (shape %3u, attack %s): the ghoul lost %d HP after %d frames (%d spells)\n",
                    u.p().defs[i].name.c_str(), c->shape, u.p().defs[i].spells.empty() ? "melee" : "melee or spell", lost, f, casts);
        bool calmed = false;  // Nerissa's Calm can end the bout before anyone is hurt
        for (size_t k = log0; k < u.p().log.size(); ++k) calmed |= u.p().log[k].rfind("calmed ", 0) == 0;
        CHECK(lost > 0 || calmed);
        if (u.p().defs[i].spells.empty()) CHECK(casts == 0);  // the fighters did it with their blades
        u.w().clear_in_combat(1);
        if (ga) u.w().destroy_now(g);
        c->set(Actor::kInvincible, false);
        u.frames(30, false);
    }
}

void test_death_and_raise(const GameData& d) {
    std::printf("death and resurrection\n");
    Ui u(d, true);
    u.frames(60);
    for (const char* n : {"brannoc", "ilvaran", "nerissa"}) u.p().join(u.idx(n));
    u.avatar_to(14300, 6150, 0);
    u.frames(150);
    Actor* b = u.ca("brannoc");
    int bi = u.idx("brannoc");
    size_t items = b->contents.size();
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(60);
    CHECK(u.p().state[size_t(bi)].mode == CompanionState::kDead);
    CHECK(!u.p().in_party(bi));
    CHECK(b->is(Actor::kDead));
    CHECK_EQ(b->contents.size(), items);  // the body keeps his inventory
    // The body opens like any corpse.
    u.w().use_item(b->id);
    u.frames(2);
    CHECK(u.w().gumps.container_gump(b->id) != nullptr);
    u.w().close_gump(b->id);
    // Ilvaran raises him (out of combat) through the conversation.
    Actor* il = u.ca("ilvaran");
    u.frames(30);
    il->mana = int16_t(u.p().max_mana(*il));
    u.double_click_actor(il->id);
    CHECK(u.answer("orders"));
    CHECK(u.answer("raise the fallen"));
    u.finish_talk();
    u.frames(30);
    CHECK(!b->is(Actor::kDead));
    CHECK(u.p().in_party(bi));
    std::printf("  Brannoc raised: hp %u, mode %d\n", b->hp, int(u.p().state[size_t(bi)].mode));
    // Ilvaran dies; Nerissa's rite calls him back.
    u.w().kill_actor(il->id, kDmgNormal);
    u.frames(60);
    Actor* ne = u.ca("nerissa");
    ne->mana = int16_t(u.p().max_mana(*ne));
    u.p().start_talk(u.idx("nerissa"));  // (Ilvaran's body may lie over her on screen)
    {
        int sx, sy;
        u.w().screen_point(*ne, sx, sy);
        std::printf("  Nerissa at screen %d,%d talking %d menu %d stasis %d target %u picked %u\n", sx, sy, u.p().talking(),
                    u.w().ask_menu != nullptr, u.w().stasis, u.w().target_pid, u.gm.pick(sx - 2, sy - 22));
    }
    CHECK(u.answer("orders"));
    CHECK(u.answer("call Ilvaran back"));
    u.finish_talk();
    u.frames(30);
    CHECK(!il->is(Actor::kDead));
    // Debug revive.
    u.w().kill_actor(b->id, kDmgNormal);
    u.frames(30);
    CHECK(u.p().revive(bi));
}

void test_trade(const GameData& d) {
    std::printf("trade and paperdoll\n");
    Ui u(d, true);
    u.frames(60);
    int vi = u.idx("varn");
    u.p().join(vi);
    u.p().join(u.idx("corvin"));
    u.p().join(u.idx("wynna"));
    u.avatar_to(14300, 6150, 0);
    u.frames(120);
    Actor* v = u.ca("varn");
    u.double_click_actor(v->id);
    CHECK(u.answer("orders"));
    CHECK(u.answer("trade"));
    u.finish_talk();
    u.frames(5);
    auto* doll = dynamic_cast<PaperdollGump*>(u.w().gumps.container_gump(v->id));
    CHECK(doll != nullptr);
    uint16_t vpack = u.w().equipped(v->id, 7);
    CHECK(vpack != 0);
    CHECK(u.w().gumps.container_gump(vpack) != nullptr);
    CHECK(u.w().equipped(v->id, 6) != 0);  // his sword
    for (uint16_t k : v->contents) {
        const Item* it = u.w().item(k);
        std::printf("  Varn carries %u shape %u flags %04X z %d%s\n", k, it ? it->shape : 0, it ? it->flags : 0, it ? it->z : 0,
                    it && it->shape == 529 ? " (backpack)" : "");
    }
    std::printf("  Varn weight %u (limit %d)\n", u.w().total_weight(*v), 40 * v->str);
    for (uint16_t k : u.w().item(u.w().equipped(v->id, 7))->contents) {
        const Item* it = u.w().item(k);
        std::printf("    pack: %u shape %u frame %u q %u flags %04X weight %u\n", k, it->shape, it->frame, it->quality, it->flags, u.w().total_weight(*it));
    }
    // An item from the Avatar's pack into Varn's.
    uint16_t pack = u.w().equipped(1, 7);
    uint16_t coin = u.w().create_item(143, 0);
    u.w().item(coin)->quality = 10;
    u.w().move_to_container(coin, pack);
    CHECK(u.w().can_put_in(coin, vpack));
    CHECK(doll->drop(coin, 49 + 7 + 20, 25 + 9 + 20, 0, 0));  // onto his paperdoll's backpack spot
    CHECK(u.w().item(coin)->parent == vpack);
    // Weight limits: an anvil-weight load is refused.
    uint16_t heavy = u.w().create_item(540, 0);  // a hammer
    int n = 0;
    while (n < 200 && u.w().can_put_in(heavy, vpack)) {
        uint16_t h2 = u.w().create_item(540, 0);
        if (!u.w().move_to_container(h2, vpack, true)) break;
        ++n;
    }
    std::printf("  Varn carried %d hammers before his limit (STR %d)\n", n, v->str);
    CHECK(n > 0 && n < 200);
    CHECK(!u.w().can_put_in(heavy, vpack));
    u.shot("party_paperdoll");
    u.w().gumps.close_item_dependent();
    u.frames(5);
    // The party bar: portraits; a click opens the paperdoll.
    u.frames(5);
    Gump* bar = nullptr;
    for (auto& g : u.w().gumps.list)
        if (std::string(g->kind()) == "partybar") bar = g.get();
    CHECK(bar != nullptr);
    if (bar) {
        u.shot("party_bar");
        u.move(bar->x + 28 + 10, bar->y + 10);  // the second portrait: Varn
        u.frames(1);
        u.button(1, true); u.button(1, false);
        u.frames(20);
        CHECK(u.w().gumps.container_gump(v->id) != nullptr);
        u.key('p');
        u.frames(2);
        bool still = false;
        for (auto& g : u.w().gumps.list) still |= std::string(g->kind()) == "partybar" && !g->closed;
        CHECK(!still);
        u.key('p');
        u.frames(2);
    }
}

void test_rogue(const GameData& d) {
    std::printf("lock picking and stealing\n");
    Ui u(d, true);
    u.frames(60);
    int ci = u.idx("corvin");
    u.p().join(ci);
    // Into Central Tenebrae, beside a locked door.
    u.w().teleport_actor(1, 2600, 3700, 0, 40);
    u.frames(30);
    uint16_t door = 0;
    for (size_t i = 512; i < u.w().objects().slots(); ++i) {
        const Item* it = u.w().item(uint16_t(i));
        if (it && it->chunk >= 0 && u.w().is_door_shape(it->shape) && (it->quality & 0x400) && it->x == 2527 && it->y == 3583) door = uint16_t(i);
    }
    CHECK(door != 0);
    if (!door) return;
    Item* di = u.w().item(door);
    int x, y, z;
    u.w().location(*di, x, y, z);
    u.avatar_to(x + 64, y + 96, z);
    u.frames(150);
    Actor* c = u.ca("corvin");
    std::printf("  door %u at %d,%d q %04X; Corvin %d units away\n", door, x, y, di->quality, dist(*c, *u.w().avatar()));
    // Through the conversation: "pick a lock", then click the door with the target cursor.
    u.double_click_actor(c->id);
    CHECK(u.answer("orders"));
    CHECK(u.answer("pick a lock"));
    for (int i = 0; i < 300 && !u.w().target_pid; ++i) { u.frames(1, false); if (i % 3 == 0) u.key(kKeyReturn); }
    CHECK(u.w().target_pid != 0);
    if (Process* p = u.w().kernel().get(u.w().target_pid)) { p->result = door; p->terminate(); }
    u.finish_talk();
    int tries = 1;
    std::string msg;
    while ((di->quality & 0x400) && tries < 20) { u.p().pick_lock(ci, door, msg); ++tries; u.frames(10, false); }
    std::printf("  unlocked after %d tries\n", tries);
    CHECK(!(di->quality & 0x400));
    CHECK(!u.p().pick_lock(ci, door, msg) && msg == "not_locked");
    // Stealing from a townsman nearby.
    uint16_t victim = 0;
    for (unsigned n = 2; n < 62 && !victim; ++n) {
        const Actor* a = u.w().actor(uint16_t(n));
        if (a && a->mapnum == 40 && a->chunk >= 0 && !a->is(Actor::kDead)) victim = uint16_t(n);
    }
    CHECK(victim != 0);
    if (!victim) return;
    Actor* va = u.w().actor(victim);
    u.w().kernel().kill(c->id, kProcAny);
    u.w().kernel().kill(victim, kProcAny);
    int px = 0, py = 0, pz = 0;
    bool moved = find_free(u.w(), c->shape, va->x, va->y, va->z, 48, 200, c->id, px, py, pz);
    if (moved) u.w().move_to(c->id, px, py, pz, false);
    CHECK(moved);
    uint64_t ev0 = u.w().events_started;
    uint16_t pack = u.w().equipped(c->id, 7);
    size_t before = u.w().item(pack)->contents.size();
    int stolen = 0, seen = 0;
    for (int k = 0; k < 6; ++k) {
        bool ok = u.p().steal_from(ci, victim, msg);
        stolen += ok;
        seen += msg != "stolen";
        std::printf("  steal from %s: %s\n", va->name.c_str(), msg.c_str());
        u.frames(5, false);
    }
    CHECK(stolen >= 1);
    CHECK(u.w().item(pack)->contents.size() >= before);
    std::printf("  stolen %d of 6, %d seen or caught, usecode events started %llu\n", stolen, seen,
                (unsigned long long)(u.w().events_started - ev0));
    if (seen) CHECK(u.w().events_started > ev0);  // the stole event went to the witnesses
}

void test_save_load(const GameData& d) {
    std::printf("save / load with a full party\n");
    Ui u(d, true);
    u.frames(60);
    for (size_t i = 0; i < 6; ++i) u.p().join(int(i));
    u.p().wait(u.idx("varn"));
    u.avatar_to(14300, 6150, 0);
    u.frames(200);
    std::vector<uint8_t> a = save_bytes(u.w());
    SaveHeader h;
    CHECK(SaveHeader::read(a, h));
    Ui u2(d, true);
    u2.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u2.w().load_game(r));
    CHECK_EQ(u2.p().members.size(), 6u);
    CHECK(u2.p().state[size_t(u2.idx("varn"))].mode == CompanionState::kWait);
    for (size_t i = 0; i < u.p().state.size(); ++i) {
        const Actor* x1 = u.w().actor(u.p().state[i].id);
        const Actor* x2 = u2.w().actor(u2.p().state[i].id);
        CHECK(x1 && x2 && x1->x == x2->x && x1->y == x2->y && x1->hp == x2->hp && x1->contents.size() == x2->contents.size());
    }
    std::vector<uint8_t> b = save_bytes(u2.w());
    CHECK(a == b);
    // Both run on identically.
    u.frames(300, false);
    u2.frames(300, false);
    CHECK(save_bytes(u.w()) == save_bytes(u2.w()));
    // An older save (version 3 layout) still loads: the shipped test saves of other suites cover
    // that; here, a save made with Companions Off loads with them on and they appear on the beach.
}

void test_off(const GameData& d) {
    std::printf("companions off\n");
    Ui on(d, true, false), off(d, true, false);
    on.w().party->defs.clear();  // the data not loaded at all
    on.frames(400, false);
    off.frames(400, false);
    CHECK(!off.p().spawned_once);
    for (unsigned id = 200; id < 256; ++id) CHECK(!(off.w().item(uint16_t(id)) && (off.w().item(uint16_t(id))->ext_flags & kExtCompanion)));
    CHECK(save_bytes(on.w()) == save_bytes(off.w()));
    // Turned on in the options: they spawn; off again: they are taken away; on: back.
    off.p().set_enabled(true);
    off.frames(60, false);
    CHECK(off.p().spawned_once);
    uint16_t m = off.cid("morvain");
    CHECK(off.w().actor(m) && off.w().actor(m)->chunk >= 0);
    off.p().set_enabled(false);
    off.frames(5, false);
    CHECK(off.w().actor(m)->chunk < 0);
    off.p().set_enabled(true);
    off.frames(5, false);
    CHECK(off.w().actor(m)->chunk >= 0);
}


static bool w_xform(const World& w, uint8_t c) { return w.data().xform.loaded && w.data().xform.is_xform(c); }

// Nerissa's own figure (data/shapes/nerissa.shp at shape 2048, NOTES.md "Custom shapes"): the
// shape and its animations match the Avatar's frame for frame; she stands on the beach, walks,
// runs, casts and fights in it; her party-bar portrait and paperdoll show her; saves keep it;
// without data/shapes/ she falls back to her old recoloured robe.
void test_nerissa(const GameData& d) {
    std::printf("nerissa's own figure\n");
    const unsigned kN = GameData::kFirstCustomShape;
    CHECK(std::find(d.custom_shapes.begin(), d.custom_shapes.end(), kN) != d.custom_shapes.end());
    const Shape* ns = d.shapes.shape(kN);
    const Shape* as = d.shapes.shape(1);
    CHECK(ns && as);
    if (!ns || !as) return;
    CHECK_EQ(ns->frame_count(), as->frame_count());
    CHECK(d.typeflags.get(kN).raw == d.typeflags.get(1).raw);
    int checked = 0, bad = 0, actions = 0;
    for (unsigned a = 0; a < AnimDat::kActions; ++a) {
        const AnimSequence* s1 = d.anim.get(1, a);
        const AnimSequence* s2 = d.anim.get(kN, a);
        CHECK((s1 == nullptr) == (s2 == nullptr));
        if (!s1 || !s2) continue;
        ++actions;
        if (s1->length != s2->length || s1->flags_a != s2->flags_a || s1->repeat != s2->repeat || s1->frames.size() != s2->frames.size()) {
            ++bad;
            continue;
        }
        for (size_t k = 0; k < s1->frames.size(); ++k) {
            const AnimFrame& f1 = s1->frames[k];
            const AnimFrame& f2 = s2->frames[k];
            const ShapeFrame* a1 = as->frame(f1.frame);
            const ShapeFrame* a2 = ns->frame(f2.frame);
            ++checked;
            if (f1.frame != f2.frame || f1.flags_c != f2.flags_c || f1.distance != f2.distance || f1.dz != f2.dz || !a1 || !a2 ||
                a1->w != a2->w || a1->h != a2->h || a1->xoff != a2->xoff || a1->yoff != a2->yoff)
                ++bad;
        }
    }
    std::printf("  %d actions, %d frame records checked against the Avatar's, %d differ\n", actions, checked, bad);
    CHECK_EQ(actions, 53);
    CHECK_EQ(bad, 0);
    // The art is hers: the standing frame facing the viewer has the dress's sea-foam and long hair,
    // and none of the Avatar's red tunic.
    {
        const ShapeFrame* f = ns->frame(3);
        int teal = 0, red = 0, hair = 0;
        for (uint8_t p : f->pixels) {
            teal += p >= 81 && p <= 93;
            red += p >= 32 && p <= 47;
            hair += p >= 211 && p <= 221;
        }
        std::printf("  stand (facing the viewer): %d dress, %d hair, %d red pixels\n", teal, hair, red);
        CHECK(teal > 150 && hair > 30 && red == 0);
    }

    Ui u(d, true);
    u.frames(60);
    int ni = u.idx("nerissa");
    Actor* n = u.ca("nerissa");
    CHECK(n && n->shape == kN);
    if (!n) return;
    CHECK(u.p().remap(n->id) != nullptr);
    // On the beach with the others (the Avatar walks over to her).
    u.avatar_to(n->x - 260, n->y + 40, n->z);
    u.frames(90);
    u.shot_actor("nerissa_beach", n->id, 200, 110);
    CHECK(u.p().join(ni));
    for (const char* o : {"wynna", "corvin"}) u.p().join(u.idx(o));
    // She follows: walks after him along the shore, and runs to catch up when he is far ahead.
    std::set<unsigned> seen;
    bool walk_shot = false, run_shot = false;
    Actor* av = u.w().avatar();
    int x, y, z;
    CHECK(find_free(u.w(), 1, 15900, 5800, 8, 0, 200, 1, x, y, z));
    u.w().pathfind(1, x, y, z, 0, 0);
    for (int f = 0; f < 400; ++f) {
        u.frames(1, false);
        seen.insert(n->last_anim);
        if (!walk_shot && n->last_anim == 0 && f > 30) { u.shot_actor("nerissa_walk", n->id); walk_shot = true; }
    }
    std::printf("  Avatar at %d,%d; Nerissa at %d,%d\n", av->x, av->y, n->x, n->y);
    CHECK(find_free(u.w(), 1, 15350, 5830, 8, 0, 200, 1, x, y, z));
    u.avatar_to(x, y, z);
    for (int f = 0; f < 300; ++f) {
        u.frames(1, false);
        seen.insert(n->last_anim);
        if (!run_shot && n->last_anim == 1 && f > 10) { u.shot_actor("nerissa_run", n->id); run_shot = true; }
    }
    std::printf("  Avatar at %d,%d; Nerissa at %d,%d\n", av->x, av->y, n->x, n->y);
    CHECK(seen.count(0) && seen.count(1));
    CHECK(walk_shot && run_shot);
    std::printf("  followed by walking and running (%d units from the Avatar now)\n", dist(*n, *av));
    CHECK(dist(*n, *av) < 400);
    // The party bar and her paperdoll (her own figure, not the Avatar's body).
    u.p().show_bar = true;
    u.frames(3);
    u.shot_crop("nerissa_party_bar", 60, 20, 120, 40);
    u.shot_crop("nerissa_portrait_partybar", 60, 20, 120, 40);
    // Her painted portrait (data/portraits/nerissa_bar.png) is what her box shows; every other
    // companion has one too (data/portraits/<id>_bar.png); the Avatar keeps his cropped figure.
    {
        const ShapeFrame* pf = u.p().bar_portrait(n->id);
        CHECK(pf && pf->w == 24 && pf->h == 25);
        CHECK(u.p().bar_portrait(1) == nullptr);
        for (const char* c : {"morvain", "kalvax", "brannoc", "varn", "ilvaran", "corvin", "wynna"}) {
            const ShapeFrame* cp = u.p().bar_portrait(u.cid(c));
            CHECK(cp && cp->w == 24 && cp->h == 25);
        }
        if (pf) {
            int opaque = 0, plain = 0;
            for (uint8_t c : pf->pixels) {
                opaque += c != ShapeFrame::kTransparent;
                plain += c != ShapeFrame::kTransparent && !w_xform(u.w(), c);
            }
            CHECK(opaque == 24 * 25 && plain == opaque);  // fully painted, no translucency indices
            std::vector<uint16_t> bar = u.p().bar_ids();
            int slot = int(std::find(bar.begin(), bar.end(), n->id) - bar.begin());
            CHECK(slot > 0 && slot < int(bar.size()));
            u.gm.render(u.surf);
            int same = 0;
            for (int yy = 0; yy < 25; ++yy)
                for (int xx = 0; xx < 24; ++xx)
                    same += u.surf.row(4 + yy)[4 + slot * 28 + 1 + xx] == pf->pixels[size_t(yy) * 24 + xx];
            std::printf("  her party-bar box: %d of 600 pixels are the portrait's\n", same);
            CHECK(same > 590);
        }
    }
    u.p().open_inventory(ni);
    u.frames(5);
    CHECK(u.p().doll_picture(n->id) != nullptr);
    CHECK(u.p().doll_picture(1) == nullptr);
    // The others: the Avatar's paperdoll body in their colours, their painted head (14 x 18) over
    // his, every pixel of it, inside the body panel and no wider than his head and shoulders.
    for (const char* c : {"morvain", "kalvax", "brannoc", "varn", "ilvaran", "corvin", "wynna"}) {
        uint16_t id = u.cid(c);
        const ShapeFrame* dp = u.p().doll_picture(id);
        const ShapeFrame* hd = u.p().picture(std::string("portraits/") + c + "_doll_head.png");
        CHECK(dp != nullptr && hd != nullptr && hd->w == 14 && hd->h == 18);
        if (!dp || !hd) continue;
        int total = 0, best = 0, bx = 0, by = 0;
        for (uint8_t px : hd->pixels) total += px != ShapeFrame::kTransparent;
        for (int oy = 0; oy + hd->h <= std::min(60, dp->h); ++oy)
            for (int ox = 0; ox + hd->w <= std::min(80, dp->w); ++ox) {
                int m = 0;
                for (int yy = 0; yy < hd->h; ++yy)
                    for (int xx = 0; xx < hd->w; ++xx) {
                        uint8_t px = hd->pixels[size_t(yy) * hd->w + xx];
                        m += px != ShapeFrame::kTransparent && dp->pixels[size_t(oy + yy) * dp->w + ox + xx] == px;
                    }
                if (m > best) { best = m; bx = ox; by = oy; }
            }
        std::printf("  %s's paperdoll head: %d of %d pixels at %d,%d\n", c, best, total, bx, by);
        CHECK(total > 100 && best == total && bx >= 15 && bx + hd->w <= 72 && by >= 13 && by < 40);
        int ci = u.p().find_def(c);
        if (ci >= 0) {
            u.p().open_inventory(ci);
            u.frames(5);
            if (const Gump* pg = u.w().gumps.container_gump(id))
                u.shot_crop(std::string("portrait_paperdoll_") + c, pg->x + pg->w / 2, pg->y + pg->h / 2, std::min(320, pg->w + 8), std::min(200, pg->h + 8));
            u.w().close_gump(id);
            if (uint16_t pack = u.w().equipped(id, 7)) u.w().close_gump(pack);
            u.frames(3);
        }
    }
    u.p().open_inventory(ni);
    u.frames(5);
    // The painted head (data/portraits/nerissa_doll_head.png) is in her paperdoll picture.
    if (const ShapeFrame* dp = u.p().doll_picture(n->id)) {
        const ShapeFrame* hd = u.p().picture("portraits/nerissa_doll_head.png");
        CHECK(hd != nullptr);
        if (hd) {
            int total = 0, best = 0, bx = 0, by = 0;
            for (uint8_t c : hd->pixels) total += c != ShapeFrame::kTransparent;
            for (int oy = 0; oy + hd->h <= dp->h; ++oy)
                for (int ox = 0; ox + hd->w <= dp->w; ++ox) {
                    int m = 0;
                    for (int yy = 0; yy < hd->h; ++yy)
                        for (int xx = 0; xx < hd->w; ++xx) {
                            uint8_t c = hd->pixels[size_t(yy) * hd->w + xx];
                            m += c != ShapeFrame::kTransparent && dp->pixels[size_t(oy + yy) * dp->w + ox + xx] == c;
                        }
                    if (m > best) { best = m; bx = ox; by = oy; }
                }
            std::printf("  her paperdoll head: %d of %d pixels at %d,%d\n", best, total, bx, by);
            CHECK(total > 120 && best == total && bx >= 15 && bx + hd->w <= 72 && by >= 13 && by < 40);
        }
    }
    u.shot("nerissa_paperdoll");
    if (const Gump* pg = u.w().gumps.container_gump(n->id))
        u.shot_crop("nerissa_portrait_paperdoll", pg->x + pg->w / 2, pg->y + pg->h / 2, std::min(320, pg->w + 8), std::min(200, pg->h + 8));
    u.w().close_gump(n->id);
    u.frames(5);
    // A fight: she casts at a ghoul (cast animation) and takes a combat stance.
    n->set(Actor::kInvincible, true);
    av->set(Actor::kInvincible, true);
    n->mana = int16_t(u.p().max_mana(*n));
    uint16_t g = u.w().create_monster(142, 0);
    int gx = 0, gy = 0, gz = 0;
    CHECK(find_free(u.w(), 142, n->x + 180, n->y - 60, n->z, 0, 200, g, gx, gy, gz));
    u.w().move_to(g, gx, gy, gz, false);
    u.frames(2, false);
    u.w().set_in_combat(g);
    if (Actor* ga = u.w().actor(g)) { ga->target = n->id; ga->fixed_target = n->id; ga->hp = 300; }
    u.w().set_in_combat(1);
    bool cast_shot = false, combat_shot = false;
    for (int f = 0; f < 900 && !(cast_shot && combat_shot); ++f) {
        u.frames(1, false);
        seen.insert(n->last_anim);
        if (!cast_shot && n->last_anim >= 27 && n->last_anim <= 31) {
            u.frames(6, false);
            u.shot_actor("nerissa_cast", n->id, 160, 100);
            cast_shot = true;
        }
        if (!combat_shot && n->is(Actor::kInCombat) && (n->last_anim == 15 || n->last_anim == 8 || n->last_anim == 9 || n->last_anim == 7)) {
            u.shot_actor("nerissa_combat", n->id, 160, 100);
            combat_shot = true;
        }
    }
    std::printf("  actions played:");
    for (unsigned a : seen) std::printf(" %u", a);
    std::printf("\n");
    CHECK(cast_shot);
    // Every action she played has her frames (nothing fell back).
    for (unsigned a : seen) CHECK(d.anim.get(kN, a) != nullptr);
    u.w().clear_in_combat(1);
    if (u.w().actor(g)) u.w().destroy_now(g);
    u.frames(30, false);
    // Save and load mid-walk: her shape and frame come back, and the game runs on identically.
    u.w().pathfind(1, av->x + 300, av->y - 100, av->z, 0, 0);
    u.frames(40, false);
    std::vector<uint8_t> a = save_bytes(u.w());
    Ui u2(d, true);
    u2.frames(5);
    SaveReader r(a.data(), a.size());
    CHECK(u2.w().load_game(r));
    const Actor* n2 = u2.ca("nerissa");
    CHECK(n2 && n2->shape == kN && n2->frame == n->frame && n2->x == n->x && n2->y == n->y);
    CHECK(save_bytes(u2.w()) == a);
    u.frames(200, false);
    u2.frames(200, false);
    CHECK(save_bytes(u.w()) == save_bytes(u2.w()));
}

// Without data/shapes/ (an install that lacks it) Nerissa wears her fallback art.
void test_nerissa_fallback(const GameData& plain) {
    std::printf("nerissa without data/shapes/\n");
    Ui u(plain, true);
    int ni = u.idx("nerissa");
    CHECK(ni >= 0);
    if (ni < 0) return;
    const CompanionDef& def = u.p().defs[size_t(ni)];
    CHECK_EQ(def.shape, 387u);
    CHECK(!def.recolour.empty());
    CHECK(!def.own_doll);
    u.frames(60, false);
    const Actor* n = u.ca("nerissa");
    CHECK(n && n->shape == 387);
}
}  // namespace

// Platforming (user report): followers must never walk or fall into water, lava or a deadly drop
// while the Avatar crosses something they cannot walk (stepping stones). They wait at the safe
// edge, and once he stands on safe ground with room for them they appear beside him.
struct Hop { int x, y; const char* what; };

// The first spot within r of (x, y) where the Avatar can stand safely (whole body on land).
// With `safe_only` false: also a spot on walkable lava (he crosses it, they must not).
bool safe_spot_near(World& w, int x, int y, int zmax, int r, int& ox, int& oy, int& oz, bool safe_only = true) {
    const Actor* av = w.avatar();
    for (int rr = 0; rr <= r; rr += 8)
        for (int k = 0; k < (rr ? 16 : 1); ++k) {
            double ang = 6.2831853 * k / 16;
            int px = x + int(rr * std::cos(ang)), py = y + int(rr * std::sin(ang));
            int top;
            uint16_t f, id;
            if (!assist::surface_under(w, px - 32, py - 32, zmax, 1, top, f, id)) continue;
            if (assist::safe_landing(w, *av, px, py, top) && (!safe_only || !assist::hazard(w, *av, px, py, top))) {
                ox = px; oy = py; oz = top;
                return true;
            }
        }
    return false;
}

// Runs a crossing: the Avatar hops from spot to spot (as his jumps would put him there), the
// companions follow on their own. Checks nobody dies, nobody stands in water / lava / over a
// deadly drop, they wait on the start side while he is on the hazard, and are beside him at the end.
void crossing(Ui& u, const char* name, const std::vector<Hop>& hops, int zmax, bool start_side(const Actor&)) {
    World& w = u.w();
    const Actor* av = w.avatar();
    std::vector<uint16_t> ids, all;
    for (int m : u.p().members) ids.push_back(u.p().state[size_t(m)].id);
    all = ids;
    all.push_back(1);
    int unsafe = 0, deaths = 0, left_early = 0, overlaps = 0;
    std::vector<std::array<int, 2>> was;
    for (uint16_t id : ids) was.push_back({w.actor(id)->x, w.actor(id)->y});
    // Every frame: nobody dead, lost or in harm's way; one that appears somewhere (a catch-up)
    // does not land on another companion or on him.
    auto watch = [&](size_t h) {
        u.frames(1, false);
        for (size_t k = 0; k < ids.size(); ++k) {
            const Actor* a = w.actor(ids[k]);
            if (!a || a->is(Actor::kDead) || a->chunk < 0 || a->mapnum != w.map().map_number()) { ++deaths; continue; }
            if (assist::hazard(w, *a, a->x, a->y, a->z)) {
                if (unsafe < 5) std::printf("  %s: companion %u in danger at %d,%d,%d (hop %zu)\n", name, ids[k], a->x, a->y, a->z, h);
                ++unsafe;
            }
            if (std::abs(a->x - was[k][0]) + std::abs(a->y - was[k][1]) > 160)
                for (uint16_t o : all)
                    if (const Actor* b = w.actor(o); b && b != a && std::abs(a->x - b->x) < 48 && std::abs(a->y - b->y) < 48 && std::abs(a->z - b->z) < 32) {
                        std::printf("  %s: companion %u appeared on %u\n", name, ids[k], o);
                        ++overlaps;
                    }
            was[k] = {a->x, a->y};
        }
    };
    for (size_t h = 0; h < hops.size(); ++h) {
        int x, y, z;
        // The Avatar's own spots are safe ground, except where he walks the lava on purpose.
        if (!safe_spot_near(w, hops[h].x, hops[h].y, zmax, 48, x, y, z, std::strcmp(hops[h].what, "lava") != 0)) {
            std::printf("  %s: no spot for him at hop %zu (%s, %d,%d)\n", name, h, hops[h].what, hops[h].x, hops[h].y);
            CHECK(false);
            return;
        }
        w.kernel().kill(1, kProcAny);
        w.move_to(1, x, y, z, false);
        bool last = h + 1 == hops.size();
        for (int f = 0; f < (last ? 1 : 150); ++f) watch(h);
        if (h > 0 && !last)
            for (uint16_t id : ids)
                if (const Actor* a = w.actor(id); a && !start_side(*a)) {
                    if (left_early < 5) std::printf("  %s: companion %u left the start side at hop %zu: %d,%d,%d\n", name, id, h, a->x, a->y, a->z);
                    ++left_early;
                }
        if (h == 1) u.shot(std::string("party_") + name + "_waiting");
    }
    // He is on the far side: they appear beside him within a few seconds.
    int near = 0;
    for (int f = 0; f < 300 && near < int(ids.size()); ++f) {
        watch(hops.size() - 1);
        near = 0;
        for (uint16_t id : ids)
            if (const Actor* a = w.actor(id); a && !a->is(Actor::kDead) && a->chunk >= 0 && dist(*a, *av) < 400 &&
                                              std::abs(a->z - av->z) < 32 && !assist::hazard(w, *a, a->x, a->y, a->z))
                ++near;
    }
    u.frames(30, false);
    u.shot(std::string("party_") + name + "_arrived");
    for (uint16_t id : ids) {
        const Actor* a = w.actor(id);
        std::printf("  %s: companion %u at %d,%d,%d, %d from the Avatar%s\n", name, id, a->x, a->y, a->z, dist(*a, *av), a->is(Actor::kDead) ? " DEAD" : "");
    }
    for (const std::string& l : u.p().log)
        if (l.find("placed") != std::string::npos || l.find("regrouped") != std::string::npos) std::printf("  %s: log: %s\n", name, l.c_str());
    std::printf("  %s: %d deaths / lost frames, %d unsafe frames, %d left early, %d of %zu beside him, %d overlaps\n", name, deaths, unsafe,
                left_early, near, ids.size(), overlaps);
    CHECK_EQ(deaths, 0);
    CHECK_EQ(unsafe, 0);
    CHECK_EQ(left_early, 0);
    CHECK_EQ(near, int(ids.size()));
    CHECK_EQ(overlaps, 0);
}

// A fresh game with three followers, moved to `map` beside (sx, sy); then the crossing.
void platform_case(const GameData& d, const char* name, unsigned map, int sx, int sy, int zmax, const std::vector<Hop>& hops,
                   bool start_side(const Actor&)) {
    Ui u(d, true);
    u.frames(60, false);
    for (const char* n : {"brannoc", "varn", "ilvaran"}) u.p().join(u.idx(n));
    World& w = u.w();
    w.change_map(map);
    u.frames(2, false);
    u.avatar_to(sx, sy, zmax - 8);  // the fast area (its globs) round the place first
    u.frames(10, false);
    int x, y, z;
    CHECK(safe_spot_near(w, sx, sy, zmax, 64, x, y, z));
    u.avatar_to(x, y, z);
    u.frames(200, false);  // they catch up and settle
    u.p().log.clear();
    crossing(u, name, hops, zmax, start_side);
}

void test_platforming(const GameData& d) {
    std::printf("platforming\n");
    // 1. The Plateau's stepping stones (map 8, the lake east of the cave; water at z 0): from
    // the south shore (z 8) north over five 96-unit stones to the north shore. They wait on the
    // south shore and come over once he is on the north shore (2000 away: the far rule).
    platform_case(d, "stones", 8, 30560, 22960, 40,
                  {{30560, 22860, "south shore edge"}, {30528, 22368, "stone 1"}, {30816, 22144, "stone 2"},
                   {30816, 21760, "stone 3"}, {30592, 21536, "stone 4"}, {30592, 21152, "stone 5"},
                   {30624, 20720, "north shore"}},
                  [](const Actor& a) { return a.y > 22700; });
    // 2. Lava (map 24, the Lava River tunnel, hot): from the island's north tip (z 8) he walks
    // the lava east to the north ground 700 away (the near rule: they wait, then regroup).
    platform_case(d, "lava", 24, 19200, 26700, 24,
                  {{19280, 26700, "island edge"}, {19500, 26620, "lava"}, {19700, 26560, "lava"},
                   {19950, 26380, "north ground"}},
                  [](const Actor& a) { return a.x < 19600 && a.y > 26600; });
    // 3. A deadly drop (map 8, by the lake west of the cave): the party on rock at z 112, he
    // goes down to the shore at z 8 (a 104 fall kills). They do not step off the edge; he is on
    // the shore with room, so they come down.
    platform_case(d, "drop", 8, 25750, 25700, 120,
                  {{25780, 25760, "the edge"}, {25960, 26150, "below"}},
                  [](const Actor& a) { return a.z >= 100; });
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_party_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");  // Nerissa's own figure (data/shapes/)
    std::string only = argc > 5 ? argv[5] : "";
    if (only.rfind("probe:", 0) == 0) {
        int b[4] = {0, 0, 0, 0};
        std::sscanf(only.c_str() + 6, "%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3]);
        probe(d, b[0], b[1], b[2], b[3]);
        return 0;
    }
    if (only.rfind("surf:", 0) == 0) {
        int b[7] = {0, 0, 0, 0, 0, 32, 400};
        std::sscanf(only.c_str() + 5, "%d,%d,%d,%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6]);
        probe_surface(d, b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
        return 0;
    }
    auto run = [&](const char* n, void (*f)(const GameData&)) { if (only.empty() || only == n) f(d); };
    run("beach", test_beach);
    run("talk", test_talk_and_commands);
    run("follow", test_follow);
    run("combat", test_combat);
    run("fight", test_each_fights);
    run("death", test_death_and_raise);
    run("trade", test_trade);
    run("rogue", test_rogue);
    run("save", test_save_load);
    run("off", test_off);
    run("opening", test_spawn_after_opening);
    run("nerissa", test_nerissa);
    run("platforming", test_platforming);
    if (only.empty() || only == "nerissa_fallback") {
        GameData plain;  // the same game data without the engine's custom shapes
        if (plain.load(fs)) test_nerissa_fallback(plain);
    }
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
