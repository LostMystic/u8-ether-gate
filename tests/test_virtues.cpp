// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Virtues and the character sheet (game/virtues.h, game/sheet_extras.cpp; PLAN-VIRTUES.md 1, 2):
// the score store, once-per-save keys, the option, the save section and older saves, the level
// line's XP bar and the octagon's gems (hit tests and clicks), and pictures of the Avatar's and a
// companion's sheet.
//   pagan_virtues_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "game/debug_menu.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/game_log.h"
#include "game/banish.h"
#include "game/combat.h"
#include "game/grant_peace.h"
#include "game/gumps.h"
#include "game/npc_dialog.h"
#include "kernel/kernel.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/stat_growth.h"
#include "game/virtues.h"
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
    Ui(const GameData& d, bool leveling = true, bool virtues = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;
        w.party->enabled = true;
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        if (!w.virtues->load_dir(g_data, err)) std::printf("  virtues data: %s\n", err.c_str());
        w.virtues->enabled = virtues;
        gm.start();
    }
    World& w() { return gm.world(); }
    Virtues& v() { return *gm.world().virtues; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    PaperdollGump* doll(uint16_t id) { return dynamic_cast<PaperdollGump*>(w().gumps.container_gump(id)); }
    PaperdollGump* open_avatar() {
        if (!doll(1)) w().call_event(1, kEvUse);
        frames(10);
        return doll(1);
    }
    void shot_crop(const std::string& name, const Gump* g) {
        if (g_shots.empty() || !g) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        const int cw = std::min(320, g->w + 8), ch = std::min(200, g->h + 8);
        const int x0 = std::clamp(g->x - 4, 0, 320 - cw), y0 = std::clamp(g->y - 4, 0, 200 - ch);
        std::vector<uint32_t> crop(size_t(cw) * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) crop[size_t(y) * cw + x] = rgba[size_t(y0 + y) * 320 + x0 + x];
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", crop.data(), cw, ch, 4);
        std::printf("  shot %s/%s.png\n", g_shots.c_str(), name.c_str());
    }
    void shot(const std::string& name, int scale = 2) {
        if (g_shots.empty()) return;
        gm.render(surf);
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, scale);
        std::printf("  shot %s/%s.png\n", g_shots.c_str(), name.c_str());
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

bool has_tag(const std::vector<uint8_t>& b, const char* t) {
    return std::search(b.begin(), b.end(), t, t + 4) != b.end();
}

// The same save as an engine from before Virtues wrote it: no VIRT section.
std::vector<uint8_t> without_virt(std::vector<uint8_t> b) {
    static const char kV[4] = {'V', 'I', 'R', 'T'}, kEnd[4] = {'E', 'N', 'D', '.'};
    auto v = std::find_end(b.begin(), b.end(), kV, kV + 4);
    auto e = std::find_end(b.begin(), b.end(), kEnd, kEnd + 4);
    CHECK(v != b.end() && e != b.end() && v < e);
    if (v != b.end() && e != b.end() && v < e) b.erase(v, e);
    return b;
}

void test_store(const GameData& d) {
    std::printf("the score store\n");
    Ui u(d);
    Virtues& v = u.v();
    CHECK_EQ(v.max_score, 50);  // data/virtues.json
    CHECK_EQ(v.total(), 0);
    CHECK_EQ(v.total_max(), 400);
    CHECK(!v.has_state());
    CHECK(std::string(Virtues::name(kHonesty)) == "Honesty");
    CHECK(std::string(Virtues::name(kHumility)) == "Humility");
    CHECK(std::string(Virtues::name(8)).empty());
    CHECK_EQ(Virtues::index_of("spirituality"), kSpirituality);
    CHECK_EQ(Virtues::index_of("Valor"), kValor);
    CHECK_EQ(Virtues::index_of("courage"), -1);
    // Adding, clamping at the maximum and at 0.
    CHECK_EQ(v.add(kValor, 5), 5);
    CHECK_EQ(v.add(kValor, 100), 45);
    CHECK_EQ(v.get(kValor), 50);
    CHECK_EQ(v.add(kValor, -60), -50);
    CHECK_EQ(v.get(kValor), 0);
    CHECK_EQ(v.add(9, 5), 0);
    // Once-per-save keys: per virtue, so one deed may pay two virtues with one key.
    CHECK_EQ(v.add(kCompassion, 4, "bentic"), 4);
    CHECK_EQ(v.add(kCompassion, 4, "bentic"), 0);
    CHECK_EQ(v.add(kValor, 3, "bentic"), 3);
    CHECK_EQ(v.add(kValor, 3, "bentic"), 0);
    CHECK(v.has_paid(kCompassion, "bentic") && v.has_paid(kValor, "bentic") && !v.has_paid(kHonor, "bentic"));
    CHECK_EQ(v.get(kCompassion), 4);
    CHECK_EQ(v.total(), 7);
    CHECK(v.has_state());
    // Off: no awards, and the key is not used up.
    v.enabled = false;
    CHECK_EQ(v.add(kHonor, 6, "aramina"), 0);
    CHECK(!v.has_paid(kHonor, "aramina"));
    CHECK_EQ(v.get(kHonor), 0);
    v.set(kHonor, 20);  // the debug menu still can
    CHECK_EQ(v.get(kHonor), 20);
    v.enabled = true;
    CHECK_EQ(v.add(kHonor, 6, "aramina"), 6);
    v.set(kJustice, 999);
    CHECK_EQ(v.get(kJustice), 50);
    v.set(kJustice, -3);
    CHECK_EQ(v.get(kJustice), 0);
    // The data file: a different maximum; bad values are refused.
    std::string err;
    Virtues t(u.w());
    CHECK(t.load_text("{\"max\": 80}", err));
    CHECK_EQ(t.max_score, 80);
    CHECK(t.load_text("{}", err));
    CHECK_EQ(t.max_score, 80);
    CHECK(!t.load_text("{\"max\": 0}", err));
    CHECK(!t.load_text("not json", err));
    // A new game starts at 0.
    for (int k = 0; k < kVirtueCount; ++k) v.set(k, 50);
    CHECK(v.all_max());
    u.w().new_game();
    CHECK_EQ(v.total(), 0);
    CHECK(v.paid.empty());
}

void test_save(const GameData& d) {
    std::printf("saving and older saves\n");
    Ui u(d);
    u.frames(30);
    // Nothing earned: no section.
    std::vector<uint8_t> b0 = save_bytes(u.w());
    CHECK(!has_tag(b0, "VIRT"));
    u.v().add(kSpirituality, 17, "lothian");
    u.v().set(kHumility, 33);
    std::vector<uint8_t> b = save_bytes(u.w());
    CHECK(has_tag(b, "VIRT"));
    {
        Ui o(d);
        o.v().set(kValor, 9);  // replaced by the load
        SaveReader r(b.data(), b.size());
        CHECK(o.w().load_game(r));
        CHECK_EQ(o.v().get(kSpirituality), 17);
        CHECK_EQ(o.v().get(kHumility), 33);
        CHECK_EQ(o.v().get(kValor), 0);
        CHECK(o.v().has_paid(kSpirituality, "lothian"));
        CHECK_EQ(o.v().add(kSpirituality, 17, "lothian"), 0);  // the key is in the save: no second pay
    }
    {
        // An older save (no section) loads, every virtue at 0.
        std::vector<uint8_t> old = without_virt(b);
        Ui o(d);
        o.v().set(kValor, 9);
        SaveReader r(old.data(), old.size());
        CHECK(o.w().load_game(r));
        CHECK_EQ(o.v().total(), 0);
        CHECK(o.v().paid.empty());
    }
    {
        // A save with nothing earned loads into a game that had scores: they go.
        Ui o(d);
        o.v().set(kHonor, 12);
        SaveReader r(b0.data(), b0.size());
        CHECK(o.w().load_game(r));
        CHECK_EQ(o.v().total(), 0);
    }
    {
        // A larger maximum in the data later keeps the scores; a smaller one clamps them.
        Ui o(d);
        std::string err;
        CHECK(o.v().load_text("{\"max\": 20}", err));
        SaveReader r(b.data(), b.size());
        CHECK(o.w().load_game(r));
        CHECK_EQ(o.v().get(kSpirituality), 17);
        CHECK_EQ(o.v().get(kHumility), 20);
    }
}

void test_option(const GameData& d) {
    std::printf("the option\n");
    Virtues::register_options();
    Virtues::register_options();  // once only
    Ui u(d);
    int found = 0;
    MenuGump::OptionRow row;
    for (auto& f : World::extra_option_rows()) {
        MenuGump::OptionRow r = f(u.w());
        if (r.label == "Virtues:") { ++found; row = r; }
    }
    CHECK_EQ(found, 1);
    CHECK(row.group == "Game");
    CHECK(row.value && row.value() == "On");
    if (row.change) row.change(1);
    CHECK(!u.v().enabled);
    CHECK(row.value() == "Off");
    // Off hides the octagon (the level line stays: it belongs to Leveling).
    PaperdollGump* p = u.open_avatar();
    CHECK(p != nullptr);
    if (p) {
        CHECK(!p->octagon_shown());
        CHECK(p->level_line_shown());
        int cx, cy;
        PaperdollGump::gem_centre(kValor, cx, cy);
        CHECK_EQ(p->gem_at(cx, cy), -1);
        CHECK(p->click_message(cx, cy).empty());
    }
    row.change(1);
    CHECK(u.v().enabled);
    CHECK(p && p->octagon_shown());
    Config::global().set_bool("virtues", true);
}

void test_clicks(const GameData& d) {
    std::printf("the gems and the XP bar\n");
    Ui u(d);
    u.frames(30);
    PaperdollGump* p = u.open_avatar();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(p->octagon_shown());
    CHECK(p->level_line_shown());
    // Each gem by its middle and its edges; distinct places, all inside the figure's panel.
    for (int g = 0; g <= kVirtueCount; ++g) {
        int cx, cy;
        PaperdollGump::gem_centre(g, cx, cy);
        CHECK(cx >= 17 && cx <= 72 && cy >= 12 && cy <= 100);
        CHECK_EQ(p->gem_at(cx, cy), g);
        CHECK_EQ(p->gem_at(cx - 2, cy - 2), g);
        CHECK_EQ(p->gem_at(cx + 1, cy + 1), g);
        for (int h = 0; h < g; ++h) {
            int hx, hy;
            PaperdollGump::gem_centre(h, hx, hy);
            CHECK(std::abs(hx - cx) + std::abs(hy - cy) >= 6);
        }
    }
    CHECK_EQ(p->gem_at(100, 30), -1);
    CHECK_EQ(p->gem_at(5, 5), -1);
    u.v().set(kSpirituality, 17);
    int sx, sy;
    PaperdollGump::gem_centre(kSpirituality, sx, sy);
    CHECK(p->click_message(sx, sy) == "Spirituality 17");
    PaperdollGump::gem_centre(kHonesty, sx, sy);
    CHECK(p->click_message(sx, sy) == "Honesty 0");
    PaperdollGump::gem_centre(kVirtueCount, sx, sy);
    CHECK(p->click_message(sx, sy).empty());  // the centre says nothing
    // A click: the note on the sheet and the message log.
    PaperdollGump::gem_centre(kSpirituality, sx, sy);
    p->click(1, sx, sy);
    CHECK(p->note == "Spirituality 17");
    CHECK(!u.w().game_log.lines(GameLog::kMessages).empty() &&
          u.w().game_log.lines(GameLog::kMessages).back().text == "Spirituality 17");
    u.frames(2);
    u.shot_crop("virtues_note_gem", p);
    PaperdollGump::gem_centre(kVirtueCount, sx, sy);
    p->note.clear();
    p->click(1, sx, sy);
    CHECK(p->note.empty());
    // Through the real mouse: press and release on the Valor gem, the click comes later.
    u.v().set(kValor, 8);
    PaperdollGump::gem_centre(kValor, sx, sy);
    {
        InputEvent e;
        e.type = InputEvent::kMouseMove; e.x = p->x + sx; e.y = p->y + sy; u.gm.handle(e);
        e.type = InputEvent::kMouseDown; e.button = 1; u.gm.handle(e);
        u.frames(1);
        e.type = InputEvent::kMouseUp; u.gm.handle(e);
        u.frames(40);
        CHECK(p->note == "Valor 8");
    }
    // The XP bar.
    int bx, by, bw, bh;
    CHECK(p->xp_bar_rect(bx, by, bw, bh));
    CHECK(bx + bw <= 151 && by >= 99 && by + bh <= 113);  // under the stats button, inside the sheet
    CHECK(bx > 81 + 30);                                  // right-justified on the line
    Leveling& l = *u.w().leveling;
    const int base = l.xp_of(1);
    l.add_xp(1, 412 - base);
    u.frames(2);
    const int lv = l.level_of(1);
    std::printf("  XP %d, level %d: \"%s\"\n", l.xp_of(1), lv, p->click_message(bx + bw / 2, by + 1).c_str());
    CHECK_EQ(lv, 4);
    CHECK(p->click_message(bx + bw / 2, by + 1) == "XP 412, 388 to level 5");
    CHECK(p->click_message(bx - 1, by) == p->click_message(bx + bw / 2, by + 1));  // a little slack
    CHECK(p->click_message(bx - 6, by).empty());
    CHECK_EQ(p->xp_bar_fill(100), 3);  // (412 - 400) of 400
    // At the top level it is full.
    l.add_xp(1, 7000);
    u.frames(2);
    CHECK_EQ(l.level_of(1), kMaxLevel);
    CHECK_EQ(p->xp_bar_fill(24), 24);
    CHECK(p->click_message(bx + 2, by + 1).find("the top level") != std::string::npos);
    // Leveling off: no level line, as before (the old sheet showed nothing then either).
    l.set_enabled(false);
    CHECK(!p->level_line_shown());
    CHECK(!p->xp_bar_rect(bx, by, bw, bh));
    CHECK(p->click_message(140, 103).empty());
    CHECK(p->octagon_shown());  // the octagon is Virtues', not Leveling's
}

void test_companion(const GameData& d) {
    std::printf("a companion's sheet\n");
    Ui u(d);
    u.frames(30);
    Party& pt = *u.w().party;
    int idx = pt.find_def("varn");
    CHECK(idx >= 0);
    if (idx < 0) return;
    pt.join(idx);
    u.frames(20);
    pt.open_inventory(idx);
    u.frames(10);
    uint16_t id = pt.state[size_t(idx)].id;
    PaperdollGump* p = u.doll(id);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(!p->octagon_shown());
    CHECK(p->level_line_shown());
    int cx, cy;
    PaperdollGump::gem_centre(kValor, cx, cy);
    CHECK_EQ(p->gem_at(cx, cy), -1);
    int bx, by, bw, bh;
    CHECK(p->xp_bar_rect(bx, by, bw, bh));
    const std::string m = p->click_message(bx + 1, by + 1);
    std::printf("  Varn: \"%s\"\n", m.c_str());
    CHECK(m.rfind("XP ", 0) == 0);
    // The same place as the Avatar's.
    PaperdollGump* a = u.open_avatar();
    int ax, ay, aw, ah;
    CHECK(a && a->xp_bar_rect(ax, ay, aw, ah) && ax == bx && ay == by && aw == bw && ah == bh);
    if (a) a->close();
    u.frames(3);
    u.shot_crop("sheet_companion", p);
}

void test_shots(const GameData& d) {
    if (g_shots.empty()) return;
    std::printf("pictures\n");
    Ui u(d);
    u.frames(30);
    Leveling& l = *u.w().leveling;
    l.add_xp(1, 512 - l.xp_of(1));
    u.frames(5);
    l.pending_points = 0;  // no level-up prompt over the pictures
    if (auto* lp = u.w().gumps.find<LevelPromptGump>()) lp->close();
    l.floats.clear();
    PaperdollGump* p = u.open_avatar();
    CHECK(p != nullptr);
    if (!p) return;
    u.shot_crop("sheet_avatar_0", p);
    // Mid: a spread of scores.
    const int mid[kVirtueCount] = {10, 25, 40, 5, 30, 18, 45, 22};
    for (int k = 0; k < kVirtueCount; ++k) u.v().set(k, mid[k]);
    u.frames(2);
    u.shot_crop("sheet_avatar_mid", p);
    for (int k = 0; k < kVirtueCount; ++k) u.v().set(k, 50);
    u.frames(2);
    u.shot_crop("sheet_avatar_max", p);
    u.frames(40);  // another moment of the pulse
    u.shot_crop("sheet_avatar_max_pulse", p);
    u.shot("sheet_avatar_max_screen");
    int bx, by, bw, bh;
    if (p->xp_bar_rect(bx, by, bw, bh)) p->click(1, bx + 3, by + 1);
    u.frames(2);
    u.shot_crop("virtues_note_xp", p);
}

void test_debug(const GameData& d) {
    std::printf("the debug menu\n");
    Ui u(d);
    u.frames(20);
    DebugTools& dt = u.gm.debug();
    dt.force(true);
    dt.open_menu();
    DebugMenuGump* m = dt.menu();
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->select("Virtue scores"));
    m->activate(0);
    CHECK(m->title().rfind("Virtues", 0) == 0);
    CHECK(m->select("Valor"));
    m->activate(1);
    m->activate(1);
    CHECK_EQ(u.v().get(kValor), 10);
    m->activate(-1);
    CHECK_EQ(u.v().get(kValor), 5);
    for (int k : {int('3'), int('7'), int(kKeyReturn)}) m->key(k, 0);
    CHECK_EQ(u.v().get(kValor), 37);
    CHECK(m->select("All to the maximum"));
    m->activate(0);
    CHECK(u.v().all_max());
    CHECK(m->select("All to 0"));
    m->activate(0);
    CHECK_EQ(u.v().total(), 0);
    m->close();
    u.frames(2);
}


// Deeds from story flags (data/virtues.json "deeds"), dialogue topics with "virtue", earn().
void test_deeds(const GameData& d) {
    std::printf("deeds: story flags and dialogue pay once\n");
    Ui u(d);
    u.frames(5);
    CHECK(u.v().deeds.size() >= 2);
    CHECK_EQ(u.v().get(kHonesty), 0);
    u.w().vm().set_global(505, 1, 1);  // told Rhian
    u.frames(40);
    CHECK_EQ(u.v().get(kHonesty), 5);
    CHECK(u.v().has_paid(kHonesty, "deed/rhian_truth"));
    u.frames(40);
    CHECK_EQ(u.v().get(kHonesty), 5);  // once
    // Off: nothing pays, and it pays once turned on again.
    u.v().enabled = false;
    u.w().vm().set_global(138, 1, 1);  // Torwin's ring
    u.frames(40);
    CHECK_EQ(u.v().get(kCompassion), 0);
    u.v().enabled = true;
    u.frames(40);
    CHECK_EQ(u.v().get(kCompassion), 3);
    // earn(): pays like add and says so in the message log.
    const size_t n0 = u.w().game_log.lines(GameLog::kMessages).size();
    CHECK_EQ(u.v().earn(kValor, 2, "t"), 2);
    CHECK_EQ(u.v().earn(kValor, 2, "t"), 0);
    CHECK_EQ(u.w().game_log.lines(GameLog::kMessages).size(), n0 + 1);
    // Bad data is refused.
    Virtues t(u.w());
    std::string err;
    CHECK(!t.load_text("{\"deeds\": [{\"id\": \"x\", \"when\": {\"global\": [1, 1]}, \"pay\": {\"Courage\": 1}}]}", err));
    CHECK(!t.load_text("{\"deeds\": [{\"id\": \"x\", \"pay\": {\"Valor\": 1}}]}", err));
    // Devon's virtue topics parse with their rewards.
    NpcDialog nd(u.w());
    CHECK(nd.load_file(g_data + "/dialog/devon.json", err));
    int rewards = 0;
    for (const NpcDialogDef& def : nd.defs)
        for (const DialogTopic& top : def.topics)
            for (const DialogTopic& sub : top.topics) rewards += int(sub.virtue.size());
    CHECK_EQ(rewards, 4);  // the lake, the Tear, Toran, the Avatar's title
}

// Grant Peace (magic.md 7.2; game/grant_peace.h): lays the undead nearby to rest, pays XP and
// Spirituality.
void test_peace(const GameData& d) {
    std::printf("grant peace: the undead are laid to rest\n");
    Ui u(d);
    u.frames(5);
    unsigned undead = 0;
    for (unsigned sh = 1; sh < 2048 && !undead; ++sh)
        if (const MonsterInfo* m = monster_info(sh); m && (m->def_type & kDmgUndead) && m->hp0 > 0) undead = sh;
    CHECK(undead != 0);
    Actor* av = u.w().avatar();
    std::vector<uint16_t> ids;
    for (int k = 0; k < 2; ++k) {
        uint16_t id = u.w().create_monster(undead, 0);
        if (Actor* a = u.w().actor(id)) {
            u.w().move_to(id, av->x + 96 + 64 * k, av->y + 64, av->z, false);
            a->set(Actor::kInvincible, false);
            a->hp = 12;  // the damage is the clicked one's HP: the same for both
            ids.push_back(id);
        }
    }
    CHECK_EQ(ids.size(), size_t(2));
    const int xp0 = u.w().leveling->xp_of(1);
    start_grant_peace(u.w());
    for (int f = 0; f < 600 && !u.w().target_pid; ++f) u.frames(1);
    CHECK(u.w().target_pid != 0);
    if (Process* p = u.w().kernel().get(u.w().target_pid)) { p->result = ids[0]; p->terminate(); }
    u.frames(120);
    for (uint16_t id : ids) CHECK(u.w().actor(id) && u.w().actor(id)->is(Actor::kDead));
    CHECK_EQ(u.v().get(kSpirituality), 2);
    CHECK(u.w().leveling->xp_of(1) >= xp0);
}

// Casts sorcery spell `spell` from a held daemon talisman at `target` (Banish Daemon is usecode,
// 1417 DAEMSPEL; the talisman's quality is spell << 8 | charges, magic.md 3.1).
void cast_talisman(Ui& u, unsigned spell, uint16_t target) {
    uint16_t f = u.w().create_item(397, 9);
    u.w().item(f)->quality = uint16_t((spell << 8) | 1);
    u.w().move_to_container(f, 1);
    u.w().item(f)->flags |= kItemEquipped;  // held
    u.w().call_event(f, kEvUse);
    for (int fr = 0; fr < 600 && !u.w().target_pid; ++fr) u.frames(1);
    CHECK(u.w().target_pid != 0);
    if (Process* p = u.w().kernel().get(u.w().target_pid)) { p->result = target; p->terminate(); }
    u.frames(300);
    if (u.w().item(f)) u.w().destroy_now(f);
}

void test_banish(const GameData& d) {
    std::printf("banish: a daemon sent home pays kill XP and Spirituality\n");
    Ui u(d);
    u.frames(5);
    u.w().vm().set_global(371, 1, 1);  // a mentor deal: foci work (leveling_facts.md 1.3)
    u.frames(60);
    Actor* av = u.w().avatar();
    auto daemon = [&] {
        uint16_t id = u.w().create_monster(96, 0);
        u.w().move_to(id, av->x + 128, av->y + 64, av->z, false);
        if (Actor* a = u.w().actor(id)) a->set(Actor::kInvincible, false);
        return id;
    };
    for (int round = 1; round <= 2; ++round) {
        const uint16_t dm = daemon();
        CHECK(u.w().actor(dm) && is_enemy(*u.w().actor(dm), *av));
        const int xp0 = u.w().leveling->xp_of(1);
        for (int k = 0; k < 5 && u.w().actor(dm) && u.w().actor(dm)->shape == 96; ++k) cast_talisman(u, 11, dm);
        CHECK(!u.w().actor(dm) || u.w().actor(dm)->shape != 96);  // banished
        std::printf("  banish %d: XP %d -> %d, Spirituality %d\n", round, xp0, u.w().leveling->xp_of(1), u.v().get(kSpirituality));
        CHECK(u.w().leveling->xp_of(1) > xp0);
        const auto& lg = u.w().leveling->log;
        CHECK_EQ(int(std::count_if(lg.begin(), lg.end(), [](const std::string& e) { return e.rfind("kill ", 0) == 0; })), round);
        CHECK_EQ(u.v().get(kSpirituality), kBanishVirtue * round);
    }
    bool said = false;
    for (const auto& l : u.w().game_log.lines(GameLog::kMessages)) said |= l.text.find("sent home") != std::string::npos;
    CHECK(said);
    // The Avatar's own daemon (Summon Daemon, spell 10) pays nothing when banished.
    const uint16_t foe = daemon();
    u.w().move_to(foe, av->x + 400, av->y + 64, av->z, false);
    std::vector<uint16_t> before, after;
    u.w().area_search(av->x, av->y, 2048, false, before);
    cast_talisman(u, 10, foe);
    u.w().area_search(av->x, av->y, 2048, false, after);
    uint16_t own = 0;
    for (uint16_t id : after)
        if (const Item* it = u.w().item(id); it && it->shape == 96 && id != foe &&
            std::find(before.begin(), before.end(), id) == before.end()) own = id;
    CHECK(own != 0);
    if (own) {
        CHECK(u.w().actor(own)->ext_flags & kExtXpPaid);
        const int xp0 = u.w().leveling->xp_of(1);
        for (int k = 0; k < 5 && u.w().actor(own) && u.w().actor(own)->shape == 96; ++k) cast_talisman(u, 11, own);
        CHECK(!u.w().actor(own) || u.w().actor(own)->shape != 96);
        CHECK_EQ(u.w().leveling->xp_of(1), xp0);
        CHECK_EQ(u.v().get(kSpirituality), 2 * kBanishVirtue);
    }
    // The cap: after kBanishVirtueCap daemons, no more Spirituality.
    for (int n = 1; n <= kBanishVirtueCap; ++n) u.v().add(kSpirituality, kBanishVirtue, "banish/" + std::to_string(n));
    const int sp = u.v().get(kSpirituality);
    const uint16_t last = daemon();
    for (int k = 0; k < 5 && u.w().actor(last) && u.w().actor(last)->shape == 96; ++k) cast_talisman(u, 11, last);
    CHECK_EQ(u.v().get(kSpirituality), sp);
}

// The other five virtues (2026-11-08): Honor (a promise kept), Valor (Khumash-Gor), and a real
// conversation with Rhian that costs obsidian (Sacrifice, Compassion); the endings and epilogue.
std::vector<std::string> menu_of(World& w) {
    std::vector<std::string> out;
    if (w.ask_menu) for (uint16_t sid : w.ask_menu->answers) {
        std::string a = w.vm().string(sid);
        while (!a.empty() && a.back() == ' ') a.pop_back();
        out.push_back(a);
    }
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
bool pick(Ui& u, const std::string& label) {
    std::vector<std::string> m = menu_of(u.w());
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i] == label) { u.w().choose_answer(unsigned(i)); u.frames(2); return true; }
    std::printf("  no answer \"%s\" in:", label.c_str());
    for (const std::string& a : m) std::printf(" [%s]", a.c_str());
    std::printf("\n");
    return false;
}

void test_more_deeds(const GameData& d) {
    std::printf("more deeds: Honor, Valor, Sacrifice; the endings\n");
    {
        Ui u(d);
        u.frames(5);
        u.w().vm().set_global(86, 1, 1);  // the trial: Devon is free
        u.frames(40);
        CHECK_EQ(u.v().get(kHonor), 0);  // no promise was made
        u.v().set_mark("devon_promise");
        u.frames(40);
        CHECK_EQ(u.v().get(kHonor), 5);
        uint16_t sc = u.w().create_item(821, 0);
        u.w().move_to_container(sc, u.w().equipped(1, 7), true);
        u.frames(40);
        CHECK_EQ(u.v().get(kValor), 5);
        // Endings: a quiet game (10 points, no infamy) is the pragmatist, never the tyrant.
        CHECK(u.v().ending() && u.v().ending()->id == "pragmatist");
        CHECK_EQ(u.v().infamy_at_end(), 0);
        // Cruelty: infamy 12+ and above the total -> the tyrant; enough good outweighs it.
        CHECK_EQ(u.v().sin(4, "murder/200"), 4);
        CHECK_EQ(u.v().sin(4, "murder/200"), 0);  // once per key
        CHECK(u.v().has_sinned("murder/200"));
        u.v().sin(4, "murder/201");
        CHECK(u.v().ending()->id == "pragmatist");  // 8 infamy: not yet
        u.v().sin(3, "dialog/rhian:family_cruel");
        u.v().set_mark("kilandra_promise");  // a promise never kept: +3 at the end
        CHECK_EQ(u.v().broken_promises(), 1);
        CHECK_EQ(u.v().infamy(), 11);
        CHECK_EQ(u.v().infamy_at_end(), 14);
        CHECK(u.v().ending()->id == "tyrant");
        u.v().set(kJustice, 15);  // total 25 > 14: mostly good, but too much blood for the liberator
        CHECK(u.v().ending()->id == "pragmatist");
        u.v().set(kHumility, 40);
        CHECK(u.v().ending()->id == "pragmatist");
        u.v().paid.erase(u.v().paid.lower_bound("Vice/"), u.v().paid.lower_bound("Vice0"));
        u.v().paid.erase("mark/kilandra_promise");
        CHECK(u.v().ending()->id == "liberator");
        u.v().set_mark("toran_cleared");
        std::vector<std::string> page = u.v().epilogue_page();
        CHECK(!page.empty() && page[0] == "+The Liberator");
        int cards = 0;
        for (const std::string& l : page) if (l.find("Devon ruled") != std::string::npos || l.find("Toran the gem") != std::string::npos) ++cards;
        CHECK_EQ(cards, 2);
        u.v().enabled = false;
        u.frames(2);
    }
    {
        Ui u(d);
        std::string err;
        CHECK(u.w().dialog->load_dir(g_data + "/dialog", err));
        u.w().dialog->enabled = true;
        u.frames(5);
        u.w().vm().set_global(89, 1, 1);  // the execution
        u.frames(5);
        const Actor* r = u.w().actor(18);
        CHECK(r != nullptr);
        if (!r) return;
        // At her shop in Tenebrae (leveling_facts.md 6: map 41 (15935,8127,48), G[495:2] = 1 open;
        // G[510:1]: "What do you do?" asked), where her menu has a goodbye.
        u.w().teleport_actor(1, 15935 + 96, 8127, 48, 41);
        u.frames(30);
        u.w().teleport_actor(18, 15935, 8127, 48, 41);
        u.w().vm().set_global(495, 2, 1);
        u.w().vm().set_global(510, 1, 1);
        u.frames(30);
        auto talk = [&](const char* answer) {
            u.w().call_event(18, kEvUse);
            // Her introductions come first; the topic joins her first menu with a goodbye.
            int tries = 0;
            for (int n = 0; n < 25; ++n) {
                if (!wait_menu(u, 1500)) {  // that talk ended: talk to her again
                    if (++tries > 3) { std::printf("  no menu from Rhian\n"); return false; }
                    u.w().call_event(18, kEvUse);
                    continue;
                }
                std::vector<std::string> m = menu_of(u.w());
                if (std::find(m.begin(), m.end(), "Toran's family") != m.end()) {
                    if (!pick(u, "Toran's family") || !wait_menu(u)) return false;
                    return pick(u, answer);
                }
                u.w().choose_answer(0);
                u.frames(2);
            }
            return false;
        };
        // Too poor: nothing given, nothing paid, the topic stays.
        StatGrowth& g = *u.w().leveling->growth;
        if (int have = g.obsidian(1)) g.pay_own(1, have);
        CHECK(talk("Take these 20 obsidian. For the family."));
        for (int f = 0; f < 600 && u.w().dialog->active(); ++f) { for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2); u.frames(1); }
        CHECK_EQ(u.v().get(kSacrifice), 0);
        CHECK(!u.v().marked("rhian_helped"));
        // With 25 obsidian: 20 given, Sacrifice 4 and Compassion 2, and the topic is gone.
        uint16_t money = u.w().create_item(143, 0);
        if (Item* m = u.w().item(money)) m->quality = 25;
        u.w().move_to_container(money, u.w().equipped(1, 7), true);
        CHECK_EQ(g.obsidian(1), 25);
        for (int f = 0; f < 300 && (u.w().ask_menu || u.w().in_conversation()); ++f) {
            if (u.w().ask_menu) { std::vector<std::string> m = menu_of(u.w()); if (!m.empty()) u.w().choose_answer(unsigned(m.size() - 1)); }
            for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            u.frames(1);
        }
        CHECK(talk("Take these 20 obsidian. For the family."));
        for (int f = 0; f < 600 && u.w().dialog->active(); ++f) { for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2); u.frames(1); }
        CHECK_EQ(g.obsidian(1), 5);
        CHECK_EQ(u.v().get(kSacrifice), 4);
        CHECK_EQ(u.v().get(kCompassion), 2);
        CHECK(u.v().marked("rhian_helped"));
        // The popups: not while she talks; "Sacrifice +4" and "Compassion +2", one after the other, once it is over.
        bool shown_talking = false;
        int shown = 0;
        std::string first_text;
        for (int f = 0; f < 900 && shown < 2; ++f) {
            if (u.w().ask_menu) { std::vector<std::string> m = menu_of(u.w()); if (!m.empty()) u.w().choose_answer(unsigned(m.size() - 1)); }
            for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            const size_t before = u.v().popups.size();
            u.frames(1);
            if (u.v().popups.size() > before) {
                if (u.w().in_conversation()) shown_talking = true;
                const Virtues::Popup& p = u.v().popups.back();
                const std::string text = Virtues::popup_text(p);
                std::printf("  popup %s\n", text.c_str());
                CHECK(text == "Sacrifice +4" || text == "Compassion +2");
                if (shown == 1) CHECK(text != first_text);
                first_text = text;
                ++shown;
            }
        }
        CHECK_EQ(shown, 2);
        CHECK(!shown_talking);
    }
}

// The children (data/dialog/children.json, by shape 708): their "why" talk ends in a menu with
// "Bye!", where the Humility games join it.
void test_kids(const GameData& d) {
    std::printf("kids: play with the children (Humility), promises (Honor), Valor\n");
    {
        Ui u(d);
        std::string err;
        CHECK(u.w().dialog->load_dir(g_data + "/dialog", err));
        u.w().dialog->enabled = true;
        u.frames(5);
        u.w().vm().set_global(89, 1, 1);
        u.w().teleport_actor(1, 16607 + 128, 18687 + 128, 48, 41);  // a child's egg in Tenebrae
        uint16_t kid = 0;
        for (int f = 0; f < 600 && !kid; ++f) {
            u.frames(1);
            for (unsigned id = 1; id < 256 && !kid; ++id)
                if (const Actor* a = u.w().actor(uint16_t(id)); a && a->shape == 708 && a->mapnum == 41) kid = uint16_t(id);
        }
        CHECK(kid != 0);
        if (!kid) return;
        // Talk until the menu with the game in it, then play: three times, three games.
        auto play = [&](const char* entry, const char* answer) {
            u.w().call_event(kid, kEvUse);
            for (int n = 0; n < 30; ++n) {
                if (!wait_menu(u, 1500)) { u.w().call_event(kid, kEvUse); continue; }
                std::vector<std::string> m = menu_of(u.w());
                if (std::find(m.begin(), m.end(), entry) != m.end()) {
                    if (!pick(u, entry) || !wait_menu(u)) return false;
                    bool ok = pick(u, answer);
                    for (int f = 0; f < 900 && (u.w().ask_menu || u.w().in_conversation()); ++f) {
                        if (u.w().ask_menu && !u.w().dialog->active()) {  // back in the child's menu: Bye!
                            std::vector<std::string> mm = menu_of(u.w());
                            auto b = std::find(mm.begin(), mm.end(), "Bye!");
                            u.w().choose_answer(unsigned(b != mm.end() ? b - mm.begin() : 0));
                        }
                        for (Bark& bk : u.w().barks) bk.ticks_left = std::min(bk.ticks_left, 2);
                        u.frames(1);
                    }
                    return ok;
                }
                u.w().choose_answer(0);
                u.frames(2);
            }
            return false;
        };
        CHECK(play("Want to play a game?", "Chase the child, and stumble just before you catch up."));
        CHECK_EQ(u.v().get(kHumility), 2);
        CHECK(play("What do you like to play?", "Roar like a troll, and fall down defeated."));
        CHECK_EQ(u.v().get(kHumility), 5);
        CHECK(play("I have a question for you, too.", "Why ARE toraxes big and brown? I do not know. Will you tell me what you think?"));
        CHECK_EQ(u.v().get(kHumility), 7);
        CHECK(u.v().marked("kids_hero"));
    }
    {
        Ui u(d);
        u.frames(5);
        // Promises: a mark alone pays nothing; the deed it waits for does.
        u.v().set_mark("vividos_promise");
        u.v().set_mark("xavier_promise");
        u.w().vm().set_global(220, 1, 1);  // Elaina's grave read, but no promise to Kilandra
        u.frames(40);
        CHECK_EQ(u.v().get(kHonor), 0);
        u.w().vm().set_global(189, 1, 1);
        u.w().vm().set_global(111, 1, 1);
        u.frames(40);
        CHECK_EQ(u.v().get(kHonor), 8);
        // Valor: a foe 3 levels above, by the Avatar's own hand, up to 3 times.
        u.v().on_kill(62, 1, 5, 2);
        CHECK_EQ(u.v().get(kValor), 0);  // no actor 62 here
        uint16_t t = u.w().create_monster(83, 0);
        u.v().on_kill(t, 1, 5, 2);
        CHECK_EQ(u.v().get(kValor), 3);
        u.v().on_kill(t, 1, 4, 2);  // only 2 above
        CHECK_EQ(u.v().get(kValor), 3);
        for (int k = 0; k < 4; ++k) u.v().on_kill(t, 1, 9, 2);
        CHECK_EQ(u.v().get(kValor), 9);  // three times at most
        // The invasion: 5 ghouls in Tenebrae while G[58:1] is set.
        u.w().teleport_actor(1, 15935, 8127, 48, 41);
        u.frames(20);
        uint16_t g = u.w().create_monster(142, 0);
        u.w().move_to(g, 15935 + 200, 8127, 48, false);
        for (int k = 0; k < 5; ++k) u.v().on_kill(g, 1, -1, -1);
        CHECK_EQ(u.v().get(kValor), 9);  // no invasion
        u.w().vm().set_global(58, 1, 1);
        for (int k = 0; k < 4; ++k) u.v().on_kill(g, 1, -1, -1);
        CHECK_EQ(u.v().get(kValor), 9);
        u.v().on_kill(g, 1, -1, -1);
        CHECK_EQ(u.v().get(kValor), 14);
        u.w().vm().set_global(58, 1, 0);
    }
}

// Virtue popups (game/virtue_popups.cpp): "Humility +4" over the Avatar's head at a calm moment.
void test_popups(const GameData& d) {
    std::printf("popups: queued while busy, shown one after another, rising, gone\n");
    Ui u(d);
    u.frames(20);
    Virtues& v = u.v();
    CHECK(v.popups.empty() && v.popups_waiting.empty());
    // Busy (stasis: a scene): the gains wait; two of one virtue merge.
    u.w().stasis = true;
    CHECK_EQ(v.earn(kSacrifice, 4, "test/popup1"), 4);
    CHECK_EQ(v.earn(kCompassion, 2, "test/popup1"), 2);
    CHECK_EQ(v.add(kSacrifice, 1, "test/popup2"), 1);
    v.set(kHonor, 10);                 // the debug menu: no popup
    CHECK_EQ(v.sin(2, "test/popup"), 2);  // infamy: no popup
    u.frames(60);
    CHECK(v.popups.empty());
    CHECK_EQ(int(v.popups_waiting.size()), 2);
    if (v.popups_waiting.size() == 2) {
        CHECK(Virtues::popup_text(v.popups_waiting[0]) == "Sacrifice +5");
        CHECK(Virtues::popup_text(v.popups_waiting[1]) == "Compassion +2");
    }
    // Calm again: the first shows after a short beat, the second kPopupGap frames later.
    u.w().stasis = false;
    int first = -1, second = -1;
    for (int f = 0; f < 200 && second < 0; ++f) {
        u.frames(1);
        if (first < 0 && v.popups.size() == 1) first = f;
        if (v.popups.size() == 2) second = f;
    }
    CHECK(first >= Virtues::kPopupCalm - 2 && first <= Virtues::kPopupCalm + 4);
    CHECK(second - first >= Virtues::kPopupGap - 2 && second - first <= Virtues::kPopupGap + 2);
    CHECK(v.popups_waiting.empty());
    if (v.popups.size() == 2) {
        CHECK_EQ(v.popups[0].virtue, int(kSacrifice));
        CHECK_EQ(v.popups[1].virtue, int(kCompassion));
    }
    // Mid-rise: drawn over the Avatar in the virtue's colours (the picture, at 3x like a window).
    auto shot = [&](const char* name) {
        if (g_shots.empty()) return;
        mkdir(g_shots.c_str(), 0755);
        std::vector<uint32_t> rgba;
        int fw, fh;
        u.gm.render_frame(rgba, fw, fh, 3, 0.0, 1.0 / 30);
        save_png(g_shots + "/" + name + ".png", rgba.data(), fw, fh, 1);
        std::printf("  shot %s/%s.png\n", g_shots.c_str(), name);
    };
    u.frames(6);
    shot("virtue_popup_rise");
    {
        // Something is drawn above the Avatar's head on the plain 320x200 picture.
        Surface8 a(320, 200), b(320, 200);
        u.gm.render(a);
        auto keep = v.popups;
        v.popups.clear();
        u.gm.render(b);
        v.popups = keep;
        int ax, ay, diff = 0, below = 0;
        u.w().screen_point(*u.w().avatar(), ax, ay);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 320; ++x)
                if (a.row(y)[x] != b.row(y)[x]) { ++diff; if (y > ay) ++below; }
        CHECK(diff > 40);
        CHECK_EQ(below, 0);
    }
    // The first dithering away near its end, then both gone after their time.
    if (!v.popups.empty()) u.frames(Virtues::kPopupFrames - 16 - v.popups[0].age);
    shot("virtue_popup_fade");
    u.frames(Virtues::kPopupFrames);
    CHECK(v.popups.empty());
    // (A real conversation: test_more_deeds, Rhian.)
    u.w().stasis = false;
    CHECK_EQ(v.earn(kHumility, 4, "test/popup3"), 4);
    CHECK_EQ(int(v.popups_waiting.size()), 1);
    // The option off: nothing queued, and what waited is dropped.
    v.enabled = false;
    u.frames(2);
    CHECK(v.popups_waiting.empty() && v.popups.empty());
    CHECK_EQ(v.earn(kHumility, 4, "test/popup4"), 0);
    u.frames(30);
    CHECK(v.popups.empty());
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_virtues_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 5 ? argv[5] : "";
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("store")) test_store(d);
    if (want("save")) test_save(d);
    if (want("option")) test_option(d);
    if (want("clicks")) test_clicks(d);
    if (want("companion")) test_companion(d);
    if (want("debug")) test_debug(d);
    if (want("shots")) test_shots(d);
    if (want("deeds")) test_deeds(d);
    if (want("peace")) test_peace(d);
    if (want("banish")) test_banish(d);
    if (want("more")) test_more_deeds(d);
    if (want("kids")) test_kids(d);
    if (want("popups")) test_popups(d);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
