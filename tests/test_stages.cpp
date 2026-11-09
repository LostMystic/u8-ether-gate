// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Pagan transformed as the virtues grow (game/world_stages.h; PLAN-VIRTUES.md 5): the stage from the
// scores and the option, the outdoor colour shift and sun rays in the composed frame (and never on
// the UI, the Underworld or with Virtues off), the heartland and Cemetery hatch rule, the music
// swaps, the townsfolk's greetings by deed, the milder storm, lamps glowing at night, ambience,
// the merchants' rebate (through Darion's real training sale), the frontier XP pool, and the
// creatures and children brought in by stage. Pictures in SHOT_DIR.
//   pagan_stages_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "data/palette.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/sound.h"
#include "game/virtues.h"
#include "game/world.h"
#include "game/world_stages.h"
#include "gfx/surface.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_shots, g_saves, g_data;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    std::vector<uint32_t> px;
    int fw = 0, fh = 0;
    explicit Ui(const GameData& d, bool virtues = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.virtues->load_dir(g_data, err)) std::printf("  virtues data: %s\n", err.c_str());
        w.virtues->enabled = virtues;
        if (!w.stages->load_dir(g_data, err)) std::printf("  stages data: %s\n", err.c_str());
        gm.start();
    }
    World& w() { return gm.world(); }
    Virtues& v() { return *gm.world().virtues; }
    WorldStages& st() { return *gm.world().stages; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void go(unsigned map, int x, int y, int z) {
        w().teleport_actor(1, x, y, z, map);
        frames(40);
        for (int i = 0; i < 5; ++i) compose(0.5);  // the colour shift eases over two seconds
    }
    // A composed frame at scale 2 (dt: the real time since the last one).
    void compose(double dt = 1.0 / 30) { gm.render_frame(px, fw, fh, 2, 0.0, dt); }
    double brightness() {
        double s = 0;
        for (uint32_t c : px) s += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
        return px.empty() ? 0 : s / double(px.size());
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", px.data(), fw, fh, 1);
        std::printf("  shot %s/%s.png\n", g_shots.c_str(), name.c_str());
    }
    void total(int t) {  // spread over the eight
        for (int k = 0; k < kVirtueCount; ++k) v().set(k, t / kVirtueCount + (k < t % kVirtueCount ? 1 : 0));
    }
};

// A moment of the rays' clock when an episode is fully lit at this stage on this map.
bool lit_moment(Ui& u) {
    WorldStages& s = u.st();
    for (int n = 0; n < 400; ++n) {
        s.set_clock(n * s.rays.episode + s.rays.episode * 0.5);
        s.view_frame(0);
        if (s.rays_level() > 0.5f) return true;
    }
    return false;
}

// A moment of the rays' clock with no rays (to look at something else).
bool dark_moment(Ui& u) {
    WorldStages& s = u.st();
    for (int n = 0; n < 400; ++n) {
        s.set_clock(n * s.rays.episode + s.rays.episode * 0.5);
        s.view_frame(0);
        if (s.rays_level() == 0) return true;
    }
    return false;
}

void test_stage(const GameData& d) {
    std::printf("the stage\n");
    Ui u(d);
    WorldStages& s = u.st();
    CHECK_EQ(s.thresholds.size(), 3u);
    CHECK_EQ(s.thresholds[0], 20);
    CHECK_EQ(s.thresholds[2], 60);
    const int want[][2] = {{0, 0}, {19, 0}, {20, 1}, {39, 1}, {40, 2}, {60, 3}, {400, 3}};
    for (const auto& p : want) {
        u.total(p[0]);
        CHECK_EQ(u.v().total(), p[0]);
        CHECK_EQ(s.stage(), p[1]);
    }
    s.forced = 1;
    CHECK_EQ(s.stage(), 1);
    CHECK_EQ(s.natural_stage(), 3);
    u.v().enabled = false;  // the option off: always the original
    CHECK_EQ(s.stage(), 0);
    CHECK_EQ(s.natural_stage(), 0);
    s.forced = -1;
    CHECK_EQ(s.stage(), 0);
    u.v().enabled = true;
    CHECK_EQ(s.stage(), 3);
    std::string err;
    WorldStages bad(u.w());
    CHECK(!bad.load_text("{\"thresholds\": [50, 20, 60]}", "", err));
    CHECK(!err.empty());
}

void test_look(const GameData& d) {
    std::printf("the look: colour shift and sun rays\n");
    Ui u(d);
    WorldStages& s = u.st();
    // The filter: none at stage 0; warmer and brighter at 3; black stays black.
    u.go(40, 10810, 14094, 48);
    s.forced = 0;
    s.view_frame(0);
    CHECK(s.shift() == 0.0f);
    const uint32_t brown = 0xFF6E5E4Eu;
    CHECK_EQ(s.filter(brown), brown);
    s.forced = 3;
    u.go(50, 2948, 10612, 48);  // a map change shows the shift at once
    u.go(40, 10810, 14094, 48);
    u.compose();
    std::printf("  map %u stage %d shift %.2f\n", u.w().map_number_now(), s.stage(), s.shift());
    CHECK(s.shift() > 0.9f);
    const uint32_t f = s.filter(brown);
    const int dr = int((f >> 16) & 255) - 0x6E, db = int(f & 255) - 0x4E;
    CHECK(dr > 8);
    CHECK(dr > db);
    CHECK_EQ(s.filter(0xFF000000u), 0xFF000000u);
    // Easing: dropping the stage fades the shift out over a couple of seconds, not at once.
    s.forced = 0;
    u.compose(0.5);
    CHECK(s.shift() > 0.5f && s.shift() < 0.9f);
    for (int i = 0; i < 10; ++i) u.compose(0.5);
    CHECK(s.shift() == 0.0f);

    // Pictures: Herdsman's Valley (map 7) at stage 0, the shift alone, the shift with rays.
    u.go(7, 15071, 26000, 8);
    s.forced = 0;
    u.compose();
    s.set_clock(0);
    u.compose();
    CHECK(s.rays_level() == 0.0f);
    const std::vector<uint32_t> plain = u.px;
    const double b0 = u.brightness();
    u.shot("valley_stage0");
    // Virtues off, even at a forced stage: the original picture.
    u.v().enabled = false;
    s.forced = 3;
    u.compose();
    CHECK(u.px == plain);
    u.v().enabled = true;
    u.go(5, 5040, 22784, 48);  // jump the shift in
    u.go(7, 15071, 26000, 8);
    // A dark moment of the rays' clock, then a lit one.
    bool dark = false;
    for (int n = 0; n < 400 && !dark; ++n) {
        s.set_clock(n * s.rays.episode + s.rays.episode * 0.5);
        s.view_frame(0);
        dark = s.rays_level() == 0;
    }
    CHECK(dark);
    u.compose(0);
    const double b1 = u.brightness();
    CHECK(b1 > b0 * 1.04);
    u.shot("valley_stage3_palette");
    CHECK(lit_moment(u));
    u.compose(0);
    CHECK(s.rays_level() > 0.5f);
    {
        std::vector<uint32_t> row(640, 0xFF404040u);
        s.rays_row(row.data(), 640, -480, 1.5, 0, 0.2);
        int lit = 0, peak = 0;
        for (uint32_t c : row) { int r = int((c >> 16) & 255) - 0x40; lit += r > 0; peak = std::max(peak, r); }
        std::printf("  rays level %.2f clock %.1f: a test row has %d lit pixels, peak +%d\n", s.rays_level(), s.clock(), lit, peak);
        CHECK(lit > 20);
    }
    const double b2 = u.brightness();
    CHECK(b2 > b1 + 1.0);
    u.shot("valley_stage3_rays");
    const double t0 = s.clock();
    s.set_clock(t0 + 3);  // a few seconds on: the beams have drifted and swelled
    u.compose(0);
    u.shot("valley_stage3_rays_later");
    // Rays are more frequent at higher stages: count lit episodes.
    auto lit_share = [&](int stage) {
        s.forced = stage;
        int lit = 0;
        for (int n = 0; n < 200; ++n) {
            s.set_clock(n * s.rays.episode + s.rays.episode * 0.5);
            s.view_frame(0);
            lit += s.rays_level() > 0;
        }
        return lit;
    };
    const int l1 = lit_share(1), l2 = lit_share(2), l3 = lit_share(3);
    std::printf("  lit episodes in 200: stage 1 %d, 2 %d, 3 %d\n", l1, l2, l3);
    CHECK(l1 > 20 && l1 < l2 && l2 < l3 && l3 < 200);
    // Tenebrae and the shore.
    s.forced = 3;
    u.go(40, 10810, 14094, 48);
    CHECK(lit_moment(u));
    u.compose(0);
    u.shot("tenebrae_stage3_rays");
    s.forced = 0;
    u.compose(0);
    for (int i = 0; i < 6; ++i) u.compose(0.5);
    u.shot("tenebrae_stage0");
    // The Underworld: no shift, no rays at any stage.
    s.forced = 3;
    u.go(50, 2948, 10612, 48);
    u.compose(0);
    CHECK(s.shift() == 0.0f);
    CHECK(!lit_moment(u));
}

void test_monsters(const GameData& d) {
    std::printf("the heartland and the peaceful dead\n");
    Ui u(d);
    WorldStages& s = u.st();
    u.go(3, 11551, 2079, 48);
    Item egg;
    egg.id = 999;
    egg.shape = 500;
    egg.quality = 120;  // torax
    egg.x = 18730;
    egg.y = 4174;
    egg.z = 48;
    auto pattern = [&](const Item& e, int n) {
        std::string p;
        for (int i = 0; i < n; ++i) p += s.skip_hatch(e) ? 'x' : 'o';
        return p;
    };
    s.forced = 0;
    CHECK(pattern(egg, 6) == "oooooo");
    s.reset();
    s.forced = 1;
    CHECK(pattern(egg, 8) == "oooxooox");
    s.reset();
    s.forced = 3;
    CHECK(pattern(egg, 6) == "oxoxox");
    // People and one-shot eggs always hatch.
    Item kid = egg;
    kid.quality = 708;
    CHECK(pattern(kid, 4) == "oooo");
    Item once = egg;
    once.frame = 1;
    CHECK(pattern(once, 4) == "oooo");
    // Virtues off: as the original.
    u.v().enabled = false;
    CHECK(pattern(egg, 4) == "oooo");
    u.v().enabled = true;
    // Herdsman's Valley: the lower track yes, the far north no.
    u.go(7, 15071, 26000, 8);
    s.reset();
    Item low = egg, far = egg;
    low.x = 15839; low.y = 24255;
    far.x = 10047; far.y = 17919;
    CHECK(pattern(low, 4) == "oxox");
    CHECK(pattern(far, 4) == "oooo");
    // The Cemetery: only once Grant Peace has been done, whatever the stage.
    s.forced = 0;
    u.go(6, 5375, 29823, 8);
    s.reset();
    Item ghoul = egg;
    ghoul.quality = 142;
    ghoul.x = 11807; ghoul.y = 27743; ghoul.z = 8;
    CHECK_EQ(s.peace_count(), 0);
    CHECK(pattern(ghoul, 4) == "oooo");
    for (int n = 1; n <= 10; ++n) u.v().add(kSpirituality, 1, "peace/" + std::to_string(n));
    CHECK_EQ(s.peace_count(), 10);
    CHECK_EQ(s.skip_one_in(ghoul), 3);
    CHECK(pattern(ghoul, 6) == "ooxoox");
    // ... and not in the catacombs.
    u.go(50, 2948, 10612, 48);
    CHECK_EQ(s.skip_one_in(ghoul), 0);
    // The real intrinsic: a usecode hatch of a real egg on the shore makes nothing on a skipped turn.
    s.forced = 3;
    u.go(3, 18700, 4300, 48);
    std::vector<uint16_t> near;
    u.w().area_search(18730, 4174, 256, false, near);
    Item* real = nullptr;
    for (uint16_t id : near)
        if (Item* it = u.w().item(id); it && it->shape == 500 && (it->quality & 0x7FF) == 120) real = it;
    CHECK(real != nullptr);
    // The real usecode hatches (MONFAST / MONEGG eggs on the shore as the Avatar comes and goes): at
    // stage 3 some stay empty; at stage 0 none do.
    s.reset();
    s.hatches_skipped = 0;
    for (int i = 0; i < 2; ++i) {
        u.go(3, 11551, 2079, 48);
        u.go(3, 18700, 4300, 48);
    }
    std::printf("  real eggs at stage 3: %u hatches skipped\n", s.hatches_skipped);
    CHECK(s.hatches_skipped > 0);
    s.forced = 0;
    s.reset();
    s.hatches_skipped = 0;
    for (int i = 0; i < 2; ++i) {
        u.go(3, 11551, 2079, 48);
        u.go(3, 18700, 4300, 48);
    }
    CHECK_EQ(s.hatches_skipped, 0u);
}

void test_music(const GameData& d) {
    std::printf("music\n");
    Ui u(d);
    WorldStages& s = u.st();
    u.go(40, 10810, 14094, 48);
    s.forced = 1;
    CHECK_EQ(s.music(56), 56);
    s.forced = 2;
    CHECK_EQ(s.music(56), 57);
    CHECK_EQ(s.music(98), 98);
    u.w().sound.music_play(56);
    CHECK_EQ(u.w().sound.music_last_request(), 57);
    u.v().enabled = false;
    u.w().sound.music_play(56);
    CHECK_EQ(u.w().sound.music_last_request(), 56);
    u.v().enabled = true;
    u.go(7, 15071, 26000, 8);
    CHECK_EQ(s.music(57), 57);
    s.forced = 3;
    CHECK_EQ(s.music(57), 51);
    u.go(3, 11551, 2079, 48);
    CHECK_EQ(s.music(52), 52);  // the docks keep theirs
}

void test_greetings(const GameData& d) {
    std::printf("greetings\n");
    Ui u(d);
    WorldStages& s = u.st();
    CHECK(!s.greetings.empty());
    // Somebody to stand beside: a townsperson on the shore (the opening's cast), a guard in the city.
    auto find = [&](unsigned map, bool want_guard) -> uint16_t {
        for (uint16_t id = 2; id < 256; ++id) {
            const Actor* a = u.w().actor(id);
            if (!a || a->mapnum != map || a->parent || !(a->ext_flags & kExtInChunk) || a->is(Actor::kDead)) continue;
            if (s.greet_not_shapes.count(a->shape) || s.greet_not_npcs.count(id) || (s.guard_shapes.count(a->shape) != 0) != want_guard) continue;
            return id;
        }
        return 0;
    };
    unsigned map_now = 3;
    auto stand_by = [&](uint16_t id) {
        const Actor* a = u.w().actor(id);
        u.w().teleport_actor(1, a->x + 64, a->y + 32, a->z, map_now);
        u.frames(3);
        u.w().barks.clear();
        u.w().stasis = false;  // (the skipped opening leaves him held)
    };
    u.go(3, 11551, 2079, 48);
    const uint16_t who = find(3, false);
    CHECK(who != 0);
    if (!who) return;
    std::printf("  townsperson %u (shape %u)\n", who, u.w().actor(who)->shape);
    stand_by(who);
    // Stage 0: nobody greets.
    s.forced = 0;
    CHECK_EQ(s.try_greet(true), 0);
    // Stage 1 with a deed: the deed's line goes first.
    s.forced = 1;
    u.v().set_mark("rhian_helped");
    CHECK(!s.lines_for(who).empty());
    CHECK(s.greeter_ok(who));
    // Mordea (NPC 7) never thanks him for anything.
    CHECK(!s.greeter_ok(7));
    uint16_t sp = s.try_greet(true);
    CHECK(sp != 0);
    std::printf("  %u says \"%s\"\n", sp, s.last_greeting.c_str());
    CHECK(s.last_greeting.find("Toran") != std::string::npos || s.last_greeting.find("gem cutter") != std::string::npos);
    CHECK(!u.w().barks.empty());
    u.frames(2);
    u.compose();
    u.shot("greeting_deed");
    // Not while a bark is showing; then the waits apply to the normal tries.
    CHECK_EQ(s.try_greet(true), 0);
    u.w().barks.clear();
    CHECK_EQ(s.try_greet(false), 0);  // within `wait` of the last
    // {you}: stranger until he is known as the Avatar.
    CHECK(s.fill("Good day, {you}.") == "Good day, stranger.");
    u.v().set_mark("known_avatar");
    CHECK(s.fill("Good day, {you}.") == "Good day, Avatar.");
    // Guards only say guards' lines.
    u.go(40, 10810, 14094, 48);
    map_now = 40;
    const uint16_t guard = find(40, true);
    CHECK(guard != 0);
    if (guard) {
        for (const auto* g : s.lines_for(guard)) CHECK(g->who != 0);
        stand_by(guard);
        CHECK(s.greeter_ok(guard));
        sp = s.try_greet(true);
        std::printf("  guard %u says \"%s\"\n", sp, s.last_greeting.c_str());
        if (sp == guard) { u.frames(2); u.compose(); u.shot("greeting_guard"); }
        u.w().barks.clear();
    }
    // The real loop: at stage 3 in a while somebody greets on his own.
    s.forced = 3;
    s.reset();
    u.go(3, 11551, 2079, 48);
    map_now = 3;
    stand_by(who);
    const unsigned before = s.greetings_said;
    for (int i = 0; i < 1500 && s.greetings_said == before; ++i) { u.gm.tick(); if (i % 50 == 0) u.w().barks.clear(); }
    CHECK(s.greetings_said > before);
    // Virtues off: never.
    u.v().enabled = false;
    u.w().barks.clear();
    CHECK_EQ(s.try_greet(true), 0);
}


std::vector<std::string> menu_of(World& w) {
    std::vector<std::string> out;
    if (w.ask_menu) for (uint16_t sid : w.ask_menu->answers) out.push_back(w.vm().string(sid));
    return out;
}
bool wait_menu(Ui& u, int max = 6000) {
    for (int f = 0; f < max; ++f) {
        if (u.w().ask_menu && u.w().ask_menu->frames_open >= 6) return true;
        for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
        u.frames(1);
    }
    return false;
}
void test_market(const GameData& d) {
    std::printf("market: a merchant gives a little back\n");
    auto run = [&](int stage) {
        Ui u(d);
        u.st().forced = stage;
        u.frames(40);
        u.w().vm().set_global(89, 1, 1);  // the execution: Darion's schedule runs
        u.w().teleport_actor(1, 15583, 13700, 48, 41);
        u.frames(30);
        const Actor* g = u.w().actor(5);
        u.w().teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
        u.frames(30);
        uint16_t id = u.w().create_item(143, 0);
        u.w().item(id)->quality = 200;
        uint16_t pack = u.w().equipped(1, 7);
        u.w().move_to_container(id, pack ? pack : 1);
        u.w().vm().set_global(473, 4, 1);  // his training hours
        u.w().call_event(5, kEvUse);
        int left = -1;
        if (wait_menu(u) && menu_of(u.w()).size() > 1 && menu_of(u.w())[1] == "I need training. ") {
            u.w().choose_answer(1);
            if (wait_menu(u) && menu_of(u.w())[0] == "Yes. ") {
                u.w().choose_answer(0);
                u.frames(60);
                left = u.st().obsidian_carried();
            }
        }
        std::printf("  stage %d: 200 obsidian, a session (50): %d left, %u given back\n", stage, left, u.st().rebated);
        return left;
    };
    CHECK_EQ(run(0), 150);
    CHECK_EQ(run(1), 152);  // 5% of 50
    CHECK_EQ(run(3), 157);  // 15%: 7 now, the half carried to the next sale
}

void test_weather(const GameData& d) {
    std::printf("weather: the storm milder near the towns\n");
    Ui u(d);
    WorldStages& s = u.st();
    const PaletteTransform storm = PaletteTransform::preset(3), clear;
    u.go(40, 10810, 14094, 48);
    s.forced_period = 3;  // midday
    s.forced = 0;
    CHECK(s.storm_transform().m == storm.m);
    s.forced = 3;
    const PaletteTransform t3 = s.storm_transform();
    std::printf("  clearing at stage 3 on map 40: %.2f (m0 %d -> %d)\n", s.storm_clearing(), storm.m[0], t3.m[0]);
    CHECK(s.storm_clearing() > 0.8f && s.storm_clearing() < 0.9f);
    CHECK(t3.m[0] > storm.m[0] && t3.m[0] < clear.m[0]);
    s.forced = 1;
    CHECK(s.storm_transform().m[0] < t3.m[0]);
    s.forced = 2;
    s.forced_period = 1;  // a clear morning from stage 2
    CHECK(s.storm_transform().m == clear.m);
    s.forced = 1;
    CHECK(s.storm_transform().m != clear.m);
    s.forced_period = 3;
    // Away from the towns the storm is as it was.
    s.forced = 3;
    u.go(16, 10000, 10000, 48);
    CHECK(s.storm_transform().m == storm.m);
    u.v().enabled = false;
    u.go(40, 10810, 14094, 48);
    CHECK(s.storm_transform().m == storm.m);
    u.v().enabled = true;
    // Flashes: only the storm's (class 1393), more of them left out by stage.
    auto skipped = [&](int stage, uint16_t cls) {
        s.forced = stage;
        int n = 0;
        for (int i = 0; i < 400; ++i) n += s.skip_storm_flash(cls);
        return n;
    };
    const int k0 = skipped(0, 1393), k1 = skipped(1, 1393), k3 = skipped(3, 1393), kspell = skipped(3, 1414);
    std::printf("  flashes left out of 400: stage 0 %d, 1 %d, 3 %d; Grant Peace's at 3: %d\n", k0, k1, k3, kspell);
    CHECK(k0 == 0 && k1 > 60 && k1 < k3 && k3 < 340 && kspell == 0);
    // The thunder after a skipped flash goes too; the next real flash's does not.
    s.forced = 3;
    s.forced_period = 1;
    CHECK(s.skip_storm_flash(1393));
    CHECK(s.hush_sfx(91));
    s.forced_period = 3;
    s.forced = 0;
    CHECK(!s.skip_storm_flash(1393));
    CHECK(!s.hush_sfx(91));
    // The real storm (STORM 1393's loop, as Open Ground on Carthax Lake starts it) on Central Tenebrae.
    s.forced = 0;
    u.w().vm().start(1393, 0x80, false, 0, {}, false);
    u.go(41, 15935, 8127, 48);
    u.go(40, 10810, 14094, 48);
    u.frames(120);
    CHECK_EQ(u.w().palette_xform, 3u);
    CHECK(u.w().palette_target.m == storm.m);
    auto colour = [&]() {  // how far from grey the picture is
        double c = 0;
        for (uint32_t p : u.px) {
            const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            c += std::max({r, g, b}) - std::min({r, g, b});
        }
        return u.px.empty() ? 0 : c / double(u.px.size());
    };
    CHECK(dark_moment(u));
    for (int i = 0; i < 4; ++i) u.compose(0.5);
    const double b0 = colour();
    u.shot("storm_stage0");
    s.forced = 3;
    u.frames(150);  // the change is picked up within a second and eased over three
    CHECK(u.w().palette_target.m == s.storm_transform().m);
    CHECK(u.w().palette.m == s.storm_transform().m);
    CHECK(dark_moment(u));
    for (int i = 0; i < 6; ++i) u.compose(0.5);
    const double b3 = colour();
    std::printf("  storm picture colour: stage 0 %.1f, stage 3 %.1f\n", b0, b3);
    CHECK(b3 > b0 * 2);
    u.shot("storm_stage3");
    // A map change with the storm still on: the Avatar's schedule sets the transform again, softened.
    u.go(41, 15935, 8127, 48);
    u.frames(120);
    CHECK(u.w().palette_target.m == s.storm_transform().m);
    // And the loop's own flashes: over a long while some are left out at stage 3.
    s.flashes_skipped = 0;
    for (int i = 0; i < 20000 && s.flashes_skipped == 0; ++i) u.gm.tick();
    std::printf("  storm flashes left out in the real loop: %u\n", s.flashes_skipped);
    CHECK(s.flashes_skipped > 0);
}

void test_lamps(const GameData& d) {
    std::printf("lamps: a warm glow at night\n");
    Ui u(d);
    WorldStages& s = u.st();
    s.forced = 3;
    s.forced_period = 3;
    u.go(39, 10850, 10600, 48);  // a street of lamp posts in West Tenebrae
    u.compose(0);
    CHECK(s.glow_level() == 0.0f);  // by day: none
    s.forced_period = 5;
    CHECK(s.night());
    s.forced = 0;
    u.compose(0);
    for (int i = 0; i < 6; ++i) u.compose(0.5);
    CHECK(s.glow_level() == 0.0f);
    const double b0 = u.brightness();
    u.shot("lamps_night_stage0");
    s.forced = 3;
    for (int i = 0; i < 6; ++i) u.compose(0.5);
    CHECK(dark_moment(u));
    u.compose(0);
    std::printf("  glows in view %zu, level %.2f\n", s.glows_in_view(), s.glow_level());
    CHECK(s.glows_in_view() >= 4);
    CHECK(s.glow_level() > 0.8f);
    // Same picture without the glows (the colour shift alone) to compare with.
    s.glow_maps.erase(39);
    u.compose(0);
    const std::vector<uint32_t> no_glow = u.px;
    const double bs = u.brightness();
    s.glow_maps.insert(39);
    u.compose(0);
    const double b3 = u.brightness();
    int warmer = 0;
    for (size_t i = 0; i < u.px.size(); ++i) warmer += ((u.px[i] >> 16) & 255) > ((no_glow[i] >> 16) & 255);
    std::printf("  brightness: stage 0 %.1f, stage 3 shift only %.1f, with glows %.1f; %d pixels warmer\n", b0, bs, b3, warmer);
    CHECK(b3 > bs);
    CHECK(warmer > 2000);
    u.shot("lamps_night_stage3");
    // Central Tenebrae's main street, and the docks.
    u.go(40, 12963, 2900, 48);
    for (int i = 0; i < 3; ++i) u.compose(0.5);
    CHECK(s.glows_in_view() > 0);
    u.shot("lamps_night_stage3_central");
    // Not off the listed maps (the valley), and never with Virtues off.
    u.go(7, 15071, 26000, 8);
    u.compose(0);
    CHECK(s.glow_level() == 0.0f);
    u.v().enabled = false;
    u.go(39, 11107, 10851, 48);
    u.compose(0);
    CHECK(s.glow_level() == 0.0f);
}

void test_ambience(const GameData& d) {
    std::printf("ambience: birdsong and the sea near the towns, quieter dead\n");
    Ui u(d);
    WorldStages& s = u.st();
    u.go(40, 10810, 14094, 48);
    s.forced_period = 3;
    s.forced = 0;
    s.ambience_played = 0;
    for (int i = 0; i < 3000; ++i) u.gm.tick();
    CHECK_EQ(s.ambience_played, 0u);
    s.forced = 3;
    s.ambience_tick(true);
    CHECK_EQ(s.last_ambience, 218);  // birdsong by day
    s.forced_period = 5;
    s.ambience_tick(true);
    CHECK_EQ(s.last_ambience, 30);   // crickets at night
    s.forced_period = 3;
    s.ambience_played = 0;
    s.sfx_heard.clear();
    for (int i = 0; i < 3000; ++i) u.gm.tick();
    std::printf("  Central Tenebrae, stage 3, 100 s: %u calls (birdsong asked %u times)\n", s.ambience_played, s.sfx_heard[218]);
    CHECK(s.ambience_played >= 3 && s.ambience_played < 20);
    CHECK(s.sfx_heard[218] > 0);
    u.go(3, 11551, 2079, 48);
    s.ambience_played = 0;
    s.sfx_heard.clear();
    for (int i = 0; i < 3000; ++i) u.gm.tick();
    std::printf("  the docks: %u calls, the sea %u\n", s.ambience_played, s.sfx_heard[105]);
    CHECK(s.sfx_heard[105] > 0);
    // The Cemetery: the undead's cries thin out with the peace given; crickets come back at 10.
    u.go(6, 5375, 29823, 8);
    auto quiet = [&](int n) { int q = 0; for (int i = 0; i < n; ++i) q += s.hush_sfx(117); return q; };
    CHECK_EQ(quiet(6), 0);
    s.ambience_played = 0;
    for (int i = 0; i < 2000; ++i) u.gm.tick();
    CHECK_EQ(s.ambience_played, 0u);
    for (int n = 1; n <= 10; ++n) u.v().add(kSpirituality, 1, "peace/" + std::to_string(n));
    CHECK_EQ(quiet(6), 2);  // one in 3 at 10 laid to rest
    CHECK(!s.hush_sfx(27));  // other sounds untouched
    for (int i = 0; i < 3000; ++i) u.gm.tick();
    std::printf("  the Cemetery with 10 at peace: %u calls\n", s.ambience_played);
    CHECK(s.ambience_played > 0);
    u.go(50, 2948, 10612, 48);  // the catacombs keep their voices
    CHECK_EQ(quiet(6), 0);
}

void test_frontier(const GameData& d) {
    std::printf("frontier: the heartland's skipped XP paid on frontier kills\n");
    Ui u(d);
    WorldStages& s = u.st();
    Leveling& l = *u.w().leveling;
    std::string err;
    if (!l.load_dir(g_data, err)) std::printf("  leveling: %s\n", err.c_str());
    l.set_enabled(true);
    l.started = true;
    CHECK(l.active());
    s.forced = 3;
    u.go(3, 11551, 2079, 48);
    CHECK_EQ(s.frontier_pool(), 0);
    Item egg;
    egg.id = 999; egg.shape = 500; egg.quality = 120; egg.x = 18730; egg.y = 4174; egg.z = 48;
    for (int i = 0; i < 8; ++i) s.skip_hatch(egg);  // four skipped at stage 3
    const auto& tor = l.monsters.at(120);
    const int one = tor.variant(tor.level_on_map(3))->xp;
    std::printf("  4 skipped torax hatches: pool %d (%d each)\n", s.frontier_pool(), one);
    CHECK_EQ(s.frontier_pool(), 4 * one);
    // The pool is in the save (a Virtues key).
    CHECK(u.v().has_state());
    // Not paid in the heartland...
    CHECK_EQ(s.frontier_bonus(100), 0);
    // ... but on the Plateau: half a kill's XP again, until the pool runs dry.
    u.go(8, 10000, 10000, 48);
    const int pool = s.frontier_pool();
    const int b = s.frontier_bonus(20);
    CHECK_EQ(b, std::min(pool, 10));
    CHECK_EQ(s.frontier_pool(), pool - b);
    int paid = b;
    while (int x = s.frontier_bonus(1000)) paid += x;
    CHECK_EQ(paid, pool);
    CHECK_EQ(s.frontier_pool(), 0);
    // The real path: a monster slain by the Avatar on a frontier map pays its XP and the bonus.
    s.add_frontier_pool(500);
    const uint16_t m = u.w().create_monster(83, 0);  // a troll
    Actor* a = u.w().actor(m);
    CHECK(a != nullptr);
    if (!a) return;
    const Actor* av = u.w().avatar();
    u.w().move_to(m, av->x + 64, av->y, av->z);
    const int xp0 = l.xp_of(1);
    l.on_kill(m, 1);
    const int got = l.xp_of(1) - xp0;
    std::printf("  a troll on the Plateau paid %d XP, pool now %d\n", got, s.frontier_pool());
    CHECK(got > 0);
    CHECK(s.frontier_pool() < 500);
    const int bonus = 500 - s.frontier_pool(), kill = got - bonus;
    CHECK(kill > 0);
    CHECK_EQ(bonus, int(std::lround(kill * 0.5)));  // the kill, and half of it again from the pool
}

void test_life(const GameData& d) {
    std::printf("life: grazing torax and rats in the valley, children in Tenebrae\n");
    Ui u(d);
    WorldStages& s = u.st();
    auto count = [&](unsigned shape) {
        int n = 0;
        for (const auto& kv : s.life_out)
            for (uint16_t id : kv.second)
                if (const Actor* a = u.w().actor(id); a && a->shape == shape && !a->is(Actor::kDead)) ++n;
        return n;
    };
    s.forced_period = 3;
    s.forced = 0;
    u.go(7, 12500, 19000, 0);
    u.frames(90);
    CHECK_EQ(count(120), 0);
    s.forced = 3;
    u.frames(90);
    std::printf("  stage 3 in the valley: %d torax, %d rats out\n", count(120), count(806));
    CHECK(count(120) >= 3);
    // They graze: none is in combat, none comes for the Avatar.
    u.frames(600);
    bool fighting = false;
    for (const auto& kv : s.life_out)
        for (uint16_t id : kv.second)
            if (const Actor* a = u.w().actor(id); a && a->is(Actor::kInCombat)) fighting = true;
    CHECK(!fighting);
    CHECK(!u.w().avatar()->is(Actor::kInCombat));
    for (int i = 0; i < 4; ++i) u.compose(0.5);
    u.shot("valley_torax_stage3");
    // Slain: no XP, and the spot stays empty for the visit.
    uint16_t one = 0;
    for (uint16_t id : s.life_out["torax herd"]) if (id) { one = id; break; }
    CHECK(one != 0);
    if (one) {
        CHECK((u.w().actor(one)->ext_flags & kExtXpPaid) != 0);
        const int before = count(120);
        u.w().actor(one)->set(Actor::kDead, true);
        u.frames(60);
        CHECK_EQ(count(120), before - 1);
    }
    // Rats down the valley.
    u.go(7, 14300, 21300, 0);
    u.frames(90);
    std::printf("  rats near the lower track: %d\n", count(806));
    CHECK(count(806) >= 1);
    for (int i = 0; i < 4; ++i) u.compose(0.5);
    u.shot("valley_rats_stage3");
    // Children by day in Tenebrae from stage 2.
    s.forced = 1;
    u.go(39, 13300, 10600, 48);
    u.frames(90);
    CHECK_EQ(count(708), 0);
    s.forced = 2;
    u.frames(90);
    std::printf("  West Tenebrae at stage 2: %d children\n", count(708));
    CHECK(count(708) >= 1);
    u.frames(300);  // they wander
    for (uint16_t id : s.life_out["children"])
        if (const Actor* c = u.w().actor(id); c && c->shape == 708) {
            u.w().move_to(1, c->x + 192, c->y - 192, c->z, false);  // stand aside and watch
            break;
        }
    u.frames(40);
    u.w().barks.clear();
    CHECK(dark_moment(u));
    for (int i = 0; i < 4; ++i) u.compose(0.5);
    u.shot("tenebrae_children_stage2");
    // Not at night.
    s.forced_period = 5;
    u.go(41, 15800, 12100, 48);
    u.frames(90);
    CHECK_EQ(count(708), 0);
    s.forced_period = 3;
    u.frames(60);
    CHECK(count(708) >= 1);
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_stages_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("stage")) test_stage(d);
    if (want("look")) test_look(d);
    if (want("monsters")) test_monsters(d);
    if (want("music")) test_music(d);
    if (want("greetings")) test_greetings(d);
    if (want("weather")) test_weather(d);
    if (want("lamps")) test_lamps(d);
    if (want("ambience")) test_ambience(d);
    if (want("market")) test_market(d);
    if (want("frontier")) test_frontier(d);
    if (want("life")) test_life(d);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
