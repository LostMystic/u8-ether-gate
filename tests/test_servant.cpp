// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The Aerial Servant (specs/world/magic.md 5; game/servant.h, NOTES.md "The Aerial Servant"):
// the original spell through the real usecode (two target clicks: the item, then where to bring
// it; refused for NPCs and fixed items), and the engine's fighting servant cast on a hostile
// creature (Item powers On): it flies there, strikes a few times for small damage, the creature
// turns on it and a blow or two dispels it.
//   pagan_servant_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
//   ONLY: data, fetch, refuse, fight, dispel, off
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/item_powers.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/servant.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/platform.h"
#include "usecode/uc_machine.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;
const int kX = 14300, kY = 6150;  // open ground on the beach map (as in the skull and turn tests)
constexpr unsigned kFocus = 396, kServantFocus = 10, kServantShape = 481;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    std::vector<uint16_t> picks;  // answers to the next target cursors, in turn (then: cancel)
    int cursors = 0;
    explicit Ui(const GameData& d) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        gm.start();
        frames(60);
        w.kernel().kill(1, kProcAny);
        w.move_to(1, kX, kY, 0, false);
        frames(10);
        Actor* av = w.avatar();
        av->intel = 40;
        av->mana = 60;
    }
    World& w() { return gm.world(); }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 20);
            if (w().target_pid) {
                uint16_t t = 0;
                if (!picks.empty()) { t = picks.front(); picks.erase(picks.begin()); }
                if (Process* p = w().kernel().get(w().target_pid)) { p->result = t; p->terminate(); }
                w().target_pid = 0;
                ++cursors;
            }
            gm.tick();
        }
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 3);
        std::printf("  shot %s\n", path.c_str());
    }
    uint16_t focus() {
        uint16_t f = w().create_item(kFocus, kServantFocus);
        w().move_to_container(f, w().equipped(1, 7));
        return f;
    }
    std::vector<uint16_t> servants() {
        std::vector<uint16_t> out;
        for (unsigned i = 1; i < w().objects().slots(); ++i)
            if (const Item* it = w().item(uint16_t(i)); it && it->shape == kServantShape) out.push_back(uint16_t(i));
        return out;
    }
    bool casting() {
        bool on = false;
        w().kernel().for_each([&](Process* p) {
            if (auto* u = dynamic_cast<UCProcess*>(p); u && u->start_cls == 1430 && u->start_addr == 0x1093 && !p->terminated()) on = true;
        });
        return on;
    }
    bool servant_running() {
        bool on = false;
        w().kernel().for_each([&](Process* p) { if (p->type == 0x26A && !p->terminated()) on = true; });
        return on;
    }
};

bool free_spot(World& w, unsigned shape, int r0, int r1, uint16_t ignore, int& ox, int& oy, int& oz, double ang0) {
    const Actor* av = w.avatar();
    for (int r = r0; r <= r1; r += 16)
        for (int k = 0; k < 16; ++k) {
            double ang = ang0 + 6.2831853 * k / 16;
            int px = av->x + int(r * std::cos(ang)), py = av->y + int(r * std::sin(ang));
            for (int dz : {0, 8, -8, 16}) {
                if (av->z + dz < 0) continue;
                World::Placement pl = w.can_exist_shape(shape, px, py, av->z + dz, ignore);
                if (pl.valid && pl.supported && pl.land) { ox = px; oy = py; oz = av->z + dz; return true; }
            }
        }
    return false;
}

uint16_t spawn(Ui& u, unsigned shape, int r0, int r1, double ang = 0) {
    uint16_t m = u.w().create_monster(shape, 0);
    int x, y, z;
    if (!m || !free_spot(u.w(), shape, r0, r1, m, x, y, z, ang)) return 0;
    u.w().move_to(m, x, y, z, false);
    return m;
}

// ---------------------------------------------------------------------------------------------

void test_data() {
    std::printf("data: servant.json\n");
    std::string err;
    CHECK(servant::load_file(g_data + "/items/servant.json", err));
    CHECK_EQ(servant::params().shape, kServantShape);
    CHECK(servant::params().hp >= 1 && servant::params().hp <= 4);
    CHECK(servant::params().strikes >= 1);
    CHECK(!servant::load_text("not json", err));
    CHECK(!servant::load_text(R"({"hp": 0})", err));
    CHECK(!servant::load_text(R"({"damage": [5, 2]})", err));
    CHECK(servant::load_file(g_data + "/items/servant.json", err));  // (back to the file)
}

// The original spell: the item, then the Avatar: into his backpack.
void test_fetch(const GameData& d) {
    std::printf("fetch: an item flown into the backpack (two target clicks)\n");
    Ui u(d);
    World& w = u.w();
    uint16_t f = u.focus();
    // A bottle on the ground a few steps away.
    uint16_t item = w.create_item(592, 0);
    int x, y, z;
    CHECK(free_spot(w, 592, 160, 260, item, x, y, z, 0.8));
    w.move_to(item, x, y, z, false);
    u.frames(5);
    int mana = w.avatar()->mana;
    u.picks = {item, 1};
    w.call_event(f, kEvUse);
    bool seen = false, flew = false, shot = false;
    int sx0 = 0, sy0 = 0;
    for (int i = 0; i < 1500 && (i < 10 || u.casting()); ++i) {
        u.frames(1);
        std::vector<uint16_t> sv = u.servants();
        if (!sv.empty()) {
            const Item* s = w.item(sv[0]);
            if (!seen) { sx0 = s->x; sy0 = s->y; }
            seen = true;
            flew |= std::abs(s->x - sx0) + std::abs(s->y - sy0) > 64;
            if (!shot && flew && std::abs(s->x - x) + std::abs(s->y - y) < 80) { u.shot("servant_fetch"); shot = true; }
        }
    }
    u.frames(30);
    const Item* it = w.item(item);
    std::printf("  cursors %d, servant seen %d flew %d, item parent %u (backpack %u), mana %d -> %d\n", u.cursors, int(seen), int(flew),
                it ? it->parent : 0, w.equipped(1, 7), mana, w.avatar()->mana);
    CHECK_EQ(u.cursors, 2);
    CHECK(seen && flew);
    CHECK(it && it->parent == w.equipped(1, 7));
    CHECK_EQ(w.avatar()->mana, mana - 5);
    CHECK(u.servants().empty());
    CHECK_EQ(w.vm().get_global(117, 1), 0u);
}

// The original refusals: a friendly NPC, a fixed item. No servant, no fight.
void test_refuse(const GameData& d) {
    std::printf("refuse: a friendly NPC and a fixed item\n");
    Ui u(d);
    World& w = u.w();
    uint16_t f = u.focus();
    uint16_t guard = spawn(u, 574, 150, 260, 1.0);  // an idle guard
    CHECK(guard);
    CHECK(!servant::is_foe(w, guard));
    CHECK(!servant::is_foe(w, 1));
    servant::Stats before = servant::stats();
    u.picks = {guard};
    w.call_event(f, kEvUse);
    for (int i = 0; i < 900 && (i < 10 || u.casting()); ++i) u.frames(1);
    u.frames(20);
    CHECK_EQ(servant::stats().summoned, before.summoned);
    CHECK(u.servants().empty());
    CHECK_EQ(u.cursors, 1);  // no second cursor
    // A fixed item (the ground under the Avatar's feet).
    std::vector<uint16_t> near;
    w.area_search(w.avatar()->x, w.avatar()->y, 256, false, near);
    uint16_t fixed = 0;
    for (uint16_t k : near)
        if (const Item* it = w.item(k); it && !it->as_actor() && it->origin == Item::kFixed) { fixed = k; break; }
    CHECK(fixed);
    u.cursors = 0;
    u.picks = {fixed};
    w.call_event(f, kEvUse);
    for (int i = 0; i < 900 && (i < 10 || u.casting()); ++i) u.frames(1);
    CHECK(w.item(fixed) && !w.item(fixed)->parent);
    CHECK(u.servants().empty());
    CHECK_EQ(u.cursors, 1);
}

// Cast on a hostile creature: the fighting servant.
void test_fight(const GameData& d) {
    std::printf("fight: cast on a ghoul\n");
    Ui u(d);
    World& w = u.w();
    item_powers::enabled() = true;
    uint16_t f = u.focus();
    uint16_t ghoul = spawn(u, 142, 220, 300, 0.4);
    CHECK(ghoul);
    if (!ghoul) return;
    Actor* g = w.actor(ghoul);
    g->hp = 300;  // survives the blows, so they can be counted
    CHECK(servant::is_foe(w, ghoul));
    servant::Stats before = servant::stats();
    int mana = w.avatar()->mana;
    u.picks = {ghoul};
    w.call_event(f, kEvUse);
    bool turned = false, shot_fly = false, shot_fight = false;
    int hp0 = g->hp;
    uint16_t sid = 0;
    for (int i = 0; i < 2000 && (i < 20 || u.casting() || u.servant_running()); ++i) {
        u.frames(1);
        std::vector<uint16_t> sv = u.servants();
        if (!sv.empty()) {
            sid = sv[0];
            const Actor* s = w.actor(sid);
            CHECK(s != nullptr);  // an actor this time
            if (!s) break;
            Actor* gg = w.actor(ghoul);
            int dist = std::abs(s->x - gg->x) + std::abs(s->y - gg->y);
            if (!shot_fly && dist > 150 && dist < 230) { u.shot("servant_fight_flying"); shot_fly = true; }
            turned |= gg->target == sid;
            if (!shot_fight && servant::stats().strikes > before.strikes) { u.frames(4); u.shot("servant_fight"); shot_fight = true; }
        }
    }
    const servant::Stats& st = servant::stats();
    Actor* gg = w.actor(ghoul);
    std::printf("  summoned %u, strikes %u, damage %u (ghoul hp %d -> %d), the ghoul turned on it %d, mana %d -> %d, cursors %d\n",
                st.summoned - before.summoned, st.strikes - before.strikes, st.damage - before.damage, hp0, gg ? int(gg->hp) : -1,
                int(turned), mana, w.avatar()->mana, u.cursors);
    CHECK_EQ(st.summoned, before.summoned + 1);
    CHECK(st.strikes > before.strikes);
    CHECK(st.strikes - before.strikes <= unsigned(servant::params().strikes));
    CHECK(gg && int(gg->hp) < hp0);
    CHECK(hp0 - int(gg->hp) <= servant::params().strikes * servant::params().damage_max);  // limited
    CHECK(turned);
    CHECK_EQ(w.avatar()->mana, mana - 5);
    CHECK_EQ(u.cursors, 1);
    CHECK(u.servants().empty());  // gone in a puff at the end
    CHECK(!sid || !w.item(sid));
    CHECK(!u.servant_running());
}

// The creature's blows dispel it: one or two hits.
void test_dispel(const GameData& d) {
    std::printf("dispel: the ghoul's blows\n");
    servant::Params keep = servant::params();
    servant::params().strikes = 1000;     // only a blow can end it now
    servant::params().duration = 100000;
    servant::params().damage_min = servant::params().damage_max = 1;  // and the ghoul does not die
    Ui u(d);
    World& w = u.w();
    item_powers::enabled() = true;
    uint16_t f = u.focus();
    uint16_t ghoul = spawn(u, 142, 220, 300, 0.4);
    CHECK(ghoul);
    if (!ghoul) { servant::params() = keep; return; }
    w.actor(ghoul)->hp = 300;
    w.actor(ghoul)->dex = 30;
    servant::Stats before = servant::stats();
    u.picks = {ghoul};
    w.call_event(f, kEvUse);
    int i = 0;
    for (; i < 6000 && (i < 20 || u.casting() || u.servant_running()); ++i) {
        u.frames(1);
        if (std::getenv("PG_DEBUG") && i % 100 == 0) {
            const Actor* g = w.actor(ghoul);
            std::vector<uint16_t> sv = u.servants();
            const Item* s = sv.empty() ? nullptr : w.item(sv[0]);
            std::printf("    f%d ghoul at %d,%d,%d target %u combat %d anim %u; servant %d,%d,%d procs:", i, g->x, g->y, g->z, g->target,
                        int(g->is(Actor::kInCombat)), g->last_anim, s ? s->x : 0, s ? s->y : 0, s ? s->z : 0);
            w.kernel().for_each([&](Process* p) { if (p->item == ghoul) std::printf(" %s", p->name()); });
            std::printf("\n");
        }
    }
    const servant::Stats& st = servant::stats();
    std::printf("  after %d frames: hits taken %u, dispelled %u, strikes %u\n", i, st.hits_taken - before.hits_taken, st.dispelled - before.dispelled,
                st.strikes - before.strikes);
    CHECK(st.hits_taken - before.hits_taken >= 1 && st.hits_taken - before.hits_taken <= 2);
    CHECK_EQ(st.dispelled, before.dispelled + 1);
    CHECK(u.servants().empty());
    CHECK(i < 6000);
    servant::params() = keep;
}

// Item powers Off: the original refusal on a hostile creature too.
void test_off(const GameData& d) {
    std::printf("off: Item powers Off keeps the original refusal\n");
    Ui u(d);
    World& w = u.w();
    item_powers::enabled() = false;
    uint16_t f = u.focus();
    uint16_t ghoul = spawn(u, 142, 220, 300, 0.4);
    CHECK(ghoul);
    servant::Stats before = servant::stats();
    int mana = w.avatar()->mana;
    u.picks = {ghoul};
    w.call_event(f, kEvUse);
    for (int i = 0; i < 900 && (i < 10 || u.casting()); ++i) u.frames(1);
    u.frames(30);
    CHECK_EQ(servant::stats().summoned, before.summoned);
    CHECK(u.servants().empty());
    CHECK_EQ(w.avatar()->mana, mana - 5);  // spent all the same (as the original)
    item_powers::enabled() = true;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_servant_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, void (*fn)(const GameData&)) { if (only.empty() || only == n) fn(d); };
    if (only.empty() || only == "data") test_data();
    run("fetch", test_fetch);
    run("refuse", test_refuse);
    run("fight", test_fight);
    run("dispel", test_dispel);
    run("off", test_off);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
