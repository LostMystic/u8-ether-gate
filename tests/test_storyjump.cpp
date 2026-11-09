// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The debug menu's story jump, levels and virtues pages (app/debug_tools.*, game/story_pack.*,
// data/story_jump.json, data/story/checkpoints.pgs; NOTES.md "Story jump"): the checkpoints pack
// (written by ctest `story`) decodes; the chapters' levels follow the pacing; jumping forward and
// back changes the story flags, the inventory, the level and the companions as it should, with no
// stuck scene, no invulnerability and no level-up prompt; saving after a jump works; every chapter
// plays on; levels set up and down keep a consistent state; infamy and marks; Leveling off.
//   pagan_storyjump_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR]
#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "check.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/debug_menu.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/savegame.h"
#include "game/story_pack.h"
#include "game/virtues.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_data, g_shots, g_saves;
bool g_have_pack = false;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool leveling = true, bool companions = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;
        w.party->enabled = companions;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        if (!w.virtues->load_dir(g_data, err)) std::printf("  virtues data: %s\n", err.c_str());
        w.virtues->enabled = true;
        DebugTools& dt = gm.debug();
        dt.force(true);
        dt.no_autosave_run = true;
        dt.load_crit({g_data + "/critpath.json", g_data + "/map_links.json"});
        CHECK(dt.load_story(g_data + "/story_jump.json"));
        gm.start();
    }
    World& w() { return gm.world(); }
    DebugTools& dt() { return gm.debug(); }
    Leveling& l() { return *gm.world().leveling; }
    Party& p() { return *gm.world().party; }
    Actor* av() { return w().avatar(); }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 2);
        std::printf("  shot %s/%s.png\n", g_shots.c_str(), name.c_str());
    }
    int chapter(const std::string& id) {
        for (size_t i = 0; i < dt().chapters.size(); ++i)
            if (dt().chapters[i].id == id) return int(i);
        return -1;
    }
    int step() {
        dt().ctx.observe();
        return dt().crit.current(dt().crit.done(dt().ctx));
    }
    // Carried by the Avatar (anywhere in his inventory).
    int carried(unsigned shape, int frame = -1) {
        int n = 0;
        for (unsigned i = 2; i < w().objects().slots(); ++i) {
            const Item* it = w().item(uint16_t(i));
            if (!it || !it->parent || it->shape != shape || (frame >= 0 && int(it->frame) != frame)) continue;
            const Item* r = w().root_container(*it);
            if (r && r->id == 1) ++n;
        }
        return n;
    }
    unsigned worn(unsigned slot) {
        const Item* it = w().item(w().equipped(1, slot));
        return it ? it->shape : 0;
    }
    // A consistent level: XP within the level, no unspent points or prompt, full HP.
    void check_level(uint16_t id, int level) {
        CHECK_EQ(l().level_of(id), level);
        CHECK_EQ(Leveling::level_for_xp(l().xp_of(id)), level);
        if (id == 1) {
            CHECK_EQ(l().pending_points, 0);
            CHECK(w().gumps.find<LevelPromptGump>() == nullptr);
        }
        if (const Actor* a = w().actor(id)) CHECK_EQ(int(a->hp), p().max_hp(*a));
    }
    // After a jump: plays on without a stuck scene, the replay's invulnerability gone.
    void check_plays_on() {
        frames(90);
        CHECK(!w().stasis);
        CHECK(av() && !av()->is(Actor::kDead));
        CHECK(av() && !av()->is(Actor::kInvincible));
        CHECK(!w().game_over);
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "after a jump");
    return o.data();
}

// ---------------------------------------------------------------------------------------------

void test_data(const GameData& d) {
    std::printf("story jump data and the checkpoints pack\n");
    Ui u(d);
    DebugTools& dt = u.dt();
    CHECK_EQ(dt.chapters.size(), 13);
    // The pacing (PLAN-LEVELING.md stage 3, quest_xp.json old_save_levels): the chapter's level is
    // the highest reached by its step, the XP between that level's step and the next one's.
    std::ifstream qf(g_data + "/quest_xp.json");
    std::stringstream qs;
    qs << qf.rdbuf();
    nlohmann::json q = nlohmann::json::parse(qs.str());
    std::vector<std::pair<int, int>> pace{{1, 0}};  // level, step number (1-based) it comes at
    for (const auto& e : q["old_save_levels"]) pace.push_back({e["level"].get<int>(), dt.crit.step_index(e["step"].get<std::string>()) + 1});
    for (const auto& c : dt.chapters) {
        int lv = 1, at = 0, next = -1;
        for (size_t k = 0; k < pace.size(); ++k)
            if (c.step >= pace[k].second) { lv = pace[k].first; at = pace[k].second; next = k + 1 < pace.size() ? pace[k + 1].second : -1; }
        int xp = lv >= kMaxLevel || next < 0 ? kLevelXp[kMaxLevel - 1]
                                            : kLevelXp[lv - 1] + (kLevelXp[lv] - kLevelXp[lv - 1]) * (c.step - at) / (next - at);
        std::printf("  %-10s step %3d level %d xp %4d (pacing %d, %d)\n", c.id.c_str(), c.step, c.level, c.xp, lv, xp);
        CHECK_EQ(c.level, lv);
        CHECK_EQ(c.xp, xp);
        CHECK_EQ(Leveling::level_for_xp(c.xp), c.level);
        for (const auto& e : c.equip) CHECK(d.typeflags.get(unsigned(e[0])).equip_type() != 0);
    }
    StoryPack pack;
    std::string err;
    g_have_pack = pack.load_file(dt.story_pack_path, err);
    if (!g_have_pack) {
        std::printf("  no checkpoints pack (%s): run ctest story first; jump tests skipped\n", err.c_str());
        return;
    }
    CHECK_EQ(pack.entries.size(), 12);
    for (const auto& c : dt.chapters) {
        if (c.checkpoint.empty()) continue;
        std::vector<uint8_t> raw;
        CHECK(pack.get(pack.find(c.checkpoint), raw, err));
        SaveHeader h;
        CHECK(SaveHeader::read(raw, h));
    }
    std::ifstream f(dt.story_pack_path, std::ios::binary | std::ios::ate);
    std::printf("  %zu checkpoints, %lld bytes\n", pack.entries.size(), static_cast<long long>(f.tellg()));
    // The delta coder on its own: a changed, shifted copy comes back exactly.
    std::vector<uint8_t> a(5000), b;
    for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t(i * 7 + i / 13);
    b.assign(a.begin() + 100, a.end());
    b.insert(b.begin() + 2000, {1, 2, 3, 4, 5});
    b[3000] ^= 0xFF;
    std::vector<uint8_t> back;
    CHECK(StoryPack::apply(a, StoryPack::delta(a, b), uint32_t(b.size()), back) && back == b);
}

void test_jump(const GameData& d) {
    std::printf("jump forward and back (Leveling on, a companion along)\n");
    Ui u(d);
    u.frames(60);
    DebugTools& dt = u.dt();
    World& w = u.w();
    // Before: a companion walking with the Avatar, some Valor.
    u.p().spawn_here();
    const int bi = u.p().find_def("brannoc");
    CHECK(bi >= 0 && u.p().join(bi));
    u.w().virtues->set(kValor, 20);
    u.frames(10);
    const uint16_t brannoc = u.p().state[size_t(bi)].id;

    // Forward to Theurgy (after the Shrine of the Ancient Ones).
    CHECK(dt.jump_to_chapter(u.chapter("theurgy")));
    u.check_plays_on();
    CHECK_EQ(u.step(), 47);
    CHECK_EQ(w.vm().get_global(75, 1), 1);  // the Zealan shield is on the altar (ancient_ones)
    u.check_level(1, 5);
    CHECK_EQ(u.l().xp_of(1), 1186);
    int s, x, n;
    CHECK(u.l().base_stats(1, 5, s, x, n));
    CHECK(u.av()->str == s && u.av()->dex == x && u.av()->intel == n);
    CHECK_EQ(u.worn(6), 420);  // sword
    CHECK_EQ(u.worn(4), 523);  // armour
    CHECK(u.carried(821) == 1);  // Khumash-Gor
    CHECK(u.carried(232, 3) == 1);  // the Key of the Scion (from the world as it was)
    CHECK(u.carried(143) >= 1);
    CHECK_EQ(w.virtues->get(kValor), 20);  // kept
    CHECK(u.p().in_party(bi));
    const Actor* b = w.actor(brannoc);
    CHECK(b && b->mapnum == u.av()->mapnum && std::abs(b->x - u.av()->x) + std::abs(b->y - u.av()->y) < 700);
    u.check_level(brannoc, 5);
    CHECK(u.l().paid_steps.count("inter_lothian") == 1);  // no quest XP for what is done
    u.shot("story_jump_theurgy");
    const int xp_before = u.l().xp_of(1);
    u.frames(60);
    CHECK_EQ(u.l().xp_of(1), xp_before);

    // Back to the Plateau: later things are gone, the level is back down.
    CHECK(dt.jump_to_chapter(u.chapter("plateau")));
    u.check_plays_on();
    CHECK_EQ(u.step(), 7);
    CHECK_EQ(w.vm().get_global(75, 1), 0);
    u.check_level(1, 1);
    CHECK_EQ(u.l().xp_of(1), 87);
    CHECK_EQ(u.av()->str, kAvatarStartStr);
    CHECK_EQ(u.carried(232), 0);  // no necromancy keys
    CHECK_EQ(u.carried(821), 0);
    CHECK_EQ(u.carried(396), 0);  // no theurgy foci
    CHECK_EQ(u.worn(6), 418);  // a dagger
    CHECK_EQ(u.worn(4), 0);
    u.check_level(brannoc, 1);
    CHECK(u.p().in_party(bi));

    // Forward to the Blackrock chapter, typical virtues; save and load it.
    dt.jump_virtues = DebugTools::kTypicalVirtues;
    const int br = u.chapter("blackrock");
    CHECK(dt.jump_to_chapter(br));
    u.check_plays_on();
    CHECK_EQ(u.step(), 80);
    u.check_level(1, 7);
    CHECK_EQ(u.worn(4), 841);  // the magic armour
    CHECK_EQ(u.worn(6), 819);  // Striking
    for (int k = 0; k < kVirtueCount; ++k) CHECK_EQ(w.virtues->get(k), dt.chapters[size_t(br)].virtues[size_t(k)]);
    std::vector<uint8_t> bytes = save_bytes(w);
    CHECK(bytes.size() > 100000);
    CHECK(u.gm.save_slot(5, "after a story jump"));
    CHECK(u.gm.load_slot(5));
    u.frames(30);
    u.check_level(1, 7);
    CHECK_EQ(u.worn(4), 841);
    CHECK_EQ(u.step(), 80);
    CHECK(!w.stasis);
    CHECK(u.p().in_party(bi));
    // Through the menu: the page, a chapter row, Enter; the jump runs once the menu has closed.
    dt.jump_virtues = DebugTools::kKeepVirtues;
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m && m->select("Jump to story chapter"));
    if (!m) return;
    m->activate(0);
    CHECK(m->title().rfind("Jump to story chapter", 0) == 0);
    u.shot("debug_story_jump_top");
    CHECK(m->select("11 The Blackrock pieces"));  // the chapter the story is in, marked
    CHECK(m->rows()[size_t(m->selected())].value().rfind("now", 0) == 0);
    u.shot("debug_story_jump");
    CHECK(m->select(" 9 Hydros"));
    m->activate(0);
    u.frames(2);
    CHECK(dt.menu() == nullptr);
    CHECK_EQ(dt.chapter_now(), u.chapter("hydros"));
    u.check_plays_on();
    CHECK_EQ(u.worn(6), 818);  // Protector
    u.check_level(1, 6);
}

void test_every_chapter(const GameData& d) {
    std::printf("every chapter plays on and saves\n");
    Ui u(d);
    u.frames(30);
    for (size_t i = 0; i < u.dt().chapters.size(); ++i) {
        const auto& c = u.dt().chapters[i];
        CHECK(u.dt().jump_to_chapter(int(i)));
        u.check_plays_on();
        const int cur = u.step();
        std::printf("  %-10s map %3u step %3d level %d STR %d DEX %d INT %d HP %d weapon %u\n", c.id.c_str(), u.w().map().map_number(), cur,
                    u.l().level_of(1), u.av()->str, u.av()->dex, u.av()->intel, u.av()->hp, u.worn(6));
        CHECK_EQ(cur, c.step);
        CHECK_EQ(u.l().level_of(1), c.level);
        std::vector<uint8_t> b = save_bytes(u.w());
        SaveReader in(b.data(), b.size());
        CHECK(u.w().load_game(in));
        u.dt().world_reset();
        u.frames(5);
    }
}

void test_levels(const GameData& d) {
    std::printf("levels set up and down\n");
    Ui u(d);
    u.frames(130);  // (past the "F10: debug menu" note, for the picture)
    Leveling& l = u.l();
    // Up two levels: the gains and his points (STR then DEX) as level-ups would give them.
    l.set_level(1, 3, false);
    u.check_level(1, 3);
    CHECK_EQ(l.xp_of(1), 200);
    CHECK_EQ(u.av()->str, kAvatarStartStr + 2 + 1);
    CHECK_EQ(u.av()->dex, kAvatarStartDex + 2 + 1);
    CHECK_EQ(u.av()->intel, kAvatarStartInt + 2);
    // Trained stats stay through a relative change; a rebuild drops them.
    u.av()->str += 3;
    l.set_level(1, 2, false);
    CHECK_EQ(u.av()->str, kAvatarStartStr + 1 + 1 + 3);
    CHECK_EQ(u.av()->dex, kAvatarStartDex + 1);
    l.set_level(1, 2, true);
    CHECK_EQ(u.av()->str, kAvatarStartStr + 2);
    // Unspent points are spread, never left for a prompt.
    l.pending_points = 2;
    l.set_level(1, 4, false);
    u.check_level(1, 4);
    CHECK_EQ(u.av()->str + u.av()->dex + u.av()->intel, kAvatarStartStr + kAvatarStartDex + kAvatarStartInt + 3 * 3 + 3 + 2);
    u.frames(30);
    CHECK(u.w().gumps.find<LevelPromptGump>() == nullptr);
    // A companion, through the page.
    u.p().spawn_here();
    const int vi = u.p().find_def("varn");
    CHECK(u.p().join(vi));
    const uint16_t varn = u.p().state[size_t(vi)].id;
    DebugTools& dt = u.dt();
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m && m->select("Levels"));
    if (!m) return;
    m->activate(0);
    CHECK(m->title().rfind("Levels", 0) == 0);
    CHECK(m->select("Varn"));
    const int lv0 = l.level_of(varn);
    m->activate(1);
    m->activate(1);
    CHECK_EQ(l.level_of(varn), lv0 + 2);
    u.check_level(varn, lv0 + 2);
    m->activate(-1);
    CHECK_EQ(l.level_of(varn), lv0 + 1);
    for (int k : {int('7'), int(kKeyReturn)}) m->key(k, 0);
    u.check_level(varn, 7);
    CHECK(m->select("Avatar"));
    m->activate(1);
    u.check_level(1, 5);
    u.shot("debug_levels");
    CHECK(m->select("Party to the Avatar's level"));
    m->activate(0);
    u.check_level(varn, 5);
    CHECK(m->select("Rebuild the party's stats"));
    m->activate(0);
    int s, x, n;
    CHECK(l.base_stats(varn, 5, s, x, n));
    CHECK(u.w().actor(varn)->str == s && u.w().actor(varn)->dex == x && u.w().actor(varn)->intel == n);
    m->close();
    u.frames(2);
}

void test_virtues_page(const GameData& d) {
    std::printf("the virtues page: scores, infamy, marks\n");
    Ui u(d);
    u.frames(130);
    Virtues& v = *u.w().virtues;
    DebugTools& dt = u.dt();
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m && m->select("Virtue scores"));
    if (!m) return;
    m->activate(0);
    CHECK(m->select("Honor"));
    m->activate(1);
    CHECK_EQ(v.get(kHonor), 5);
    v.sin(4, "murder/77");
    CHECK(m->select("Infamy"));
    m->activate(1);
    CHECK_EQ(v.infamy(), 5);
    m->activate(-1);
    m->activate(-1);
    CHECK_EQ(v.infamy(), 3);  // below the acts recorded: they are dropped, the rest kept as a debug entry
    for (int k : {int('1'), int('2'), int(kKeyReturn)}) m->key(k, 0);
    CHECK_EQ(v.infamy(), 12);
    CHECK(m->select("Clear infamy"));
    m->activate(0);
    CHECK_EQ(v.infamy(), 0);
    CHECK(m->select("  known_avatar"));
    m->activate(0);
    CHECK(v.marked("known_avatar"));
    m->activate(0);
    CHECK(!v.marked("known_avatar"));
    m->activate(0);
    CHECK(m->select("Honesty"));
    u.shot("debug_virtues");
    CHECK(m->select("Reset all"));
    m->activate(0);
    CHECK(v.total() == 0 && v.paid.empty());
    m->close();
    u.frames(2);
}

void test_leveling_off(const GameData& d) {
    std::printf("Leveling off: the page says so, a jump gives the usual stats\n");
    Ui u(d, false, false);
    u.frames(30);
    DebugTools& dt = u.dt();
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m && m->select("Levels"));
    if (!m) return;
    m->activate(0);
    CHECK(!m->rows().empty() && m->rows()[0].label.find("Leveling is Off") != std::string::npos);
    m->close();
    u.frames(2);
    if (!g_have_pack) return;
    const int c = u.chapter("sorcery");
    CHECK(dt.jump_to_chapter(c));
    u.check_plays_on();
    CHECK_EQ(u.av()->str, dt.chapters[size_t(c)].stats_original[0]);
    CHECK_EQ(u.av()->intel, dt.chapters[size_t(c)].stats_original[2]);
    CHECK_EQ(int(u.av()->hp), 2 * u.av()->str);
    CHECK(!u.l().started);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_storyjump_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    test_data(d);
    if (g_have_pack) {
        test_jump(d);
        test_every_chapter(d);
    }
    test_levels(d);
    test_virtues_page(d);
    test_leveling_off(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
