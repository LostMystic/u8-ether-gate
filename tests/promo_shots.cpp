// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Promotional screenshots (not a test): the game as a player has it (every addition loaded and
// on, the HUD side panel, 1280 x 720), with debug jumps to show the new features in good places.
//   pagan_promo_shots GAME_DIR DATA_DIR OUT_DIR [scene]
// Scenes: tenebrae, combat, spellbook, sheets, flame, valley, night. Each writes OUT_DIR/promo_<scene>.png.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/banter.h"
#include "game/creatures.h"
#include "game/gumps.h"
#include "game/leveling.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/spellbook.h"
#include "game/turn_combat.h"
#include "game/virtues.h"
#include "game/world.h"
#include "game/world_stages.h"
#include "gfx/screen_layout.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

using namespace pg;

namespace {

std::string g_data, g_out;

struct Promo {
    GameMode gm;
    Surface8 surf{320, 200};
    Surface8 panel{106, 200};
    int win_w = 1280, win_h = 720;
    ScreenLayout lay;
    explicit Promo(const GameData& d) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.auto_ui = false;
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("companions: %s\n", err.c_str());
        w.party->banter->load_dir(g_data, err);
        w.party->enabled = true;
        w.party->beach_always = true;  // all of them waiting on the start beach (as debug mode gathers them)
        w.leveling->load_dir(g_data, err);
        w.leveling->enabled = true;
        w.creatures->load_dir(g_data, err);
        w.creatures->enabled = true;
        w.virtues->load_dir(g_data, err);
        w.virtues->enabled = true;
        w.stages->load_dir(g_data, err);
        w.dialog->load_dir(g_data + "/dialog", err);
        w.dialog->enabled = true;
        w.turn->enabled = true;
        gm.start();
        lay = compute_layout(win_w, win_h, 320, 200, true, LayoutScale::kInteger, true);
        gm.set_panel_width(lay.panel_px);
        panel.resize(std::max(1, lay.panel_px), 200);
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            gm.tick();
            gm.render(surf);
            if (gm.panel_on()) gm.render_panel(panel);
        }
    }
    void key(int k) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e);
        e.type = InputEvent::kKeyUp; gm.handle(e);
    }
    void mouse(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; gm.handle(e); }
    void join(std::initializer_list<const char*> names) { for (const char* n : names) p().join(p().find_def(n)); }
    void go(unsigned map, int x, int y, int z, int settle = 240) {
        w().teleport_actor(1, x, y, z, map);
        frames(settle);
    }
    void shot(const std::string& name) {
        std::vector<uint32_t> rgba, prgba, win;
        int fw, fh, pw = 0, ph = 0;
        int S = std::clamp(lay.game_w / 320, 1, 4);
        w().barks.clear();  // townsfolk's chatter
        for (int i = 0; i < 6; ++i) gm.render_frame(rgba, fw, fh, S, 0.0, 0.5);  // eased colour, sun rays in
        gm.render_frame(rgba, fw, fh, S, 0.0, 1.0 / 30);
        gm.render_panel_rgba(prgba, pw, ph);
        compose_window(lay, win_w, win_h, rgba.data(), fw, fh, pw ? prgba.data() : nullptr, pw, ph, win);
        mkdir(g_out.c_str(), 0755);
        std::string path = g_out + "/promo_" + name + ".png";
        save_png(path, win.data(), win_w, win_h, 1);
        std::printf("shot %s\n", path.c_str());
    }
    void virtues_high() {
        for (int v = 0; v < 8; ++v) w().virtues->set(v, w().virtues->max_score * (v % 3 == 0 ? 9 : 7) / 10);
    }
};

const char* kParty[] = {"brannoc", "varn", "ilvaran", "kalvax", "wynna", "nerissa"};

void party_up(Promo& u) {
    u.frames(150);  // the companions come to the beach after the opening
    for (const char* n : kParty) u.p().join(u.p().find_def(n));
    u.frames(60);
    int n = 0;
    for (const auto& st : u.p().state) n += st.mode == CompanionState::kFollow;
    std::printf("party: %d following\n", n);
}

// Tenebrae at stage 3: warm colour, sun rays, the party behind the Avatar.
void scene_tenebrae(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.w().stages->forced = 3;
    u.w().stages->forced_period = 2;
    u.virtues_high();
    u.go(40, 10810, 14094, 48, 300);
    u.mouse(400, 400);
    u.shot("tenebrae");
}

// A turn-based fight in Herdsman's Valley: the roster, the turn menu, the range circle.
void scene_combat(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.w().stages->forced = 2;
    u.w().stages->forced_period = 2;
    u.go(7, 15071, 25700, 8, 300);
    Actor* av = u.w().avatar();
    av->set(Actor::kInvincible, true);
    const unsigned foes[] = {142, 142, 807, 142};
    for (int i = 0; i < 4; ++i) {
        uint16_t m = u.w().create_monster(foes[i], 0);
        if (!m) continue;
        for (int r = 260; r < 520; r += 24) {
            double a = 0.6 + i * 0.5;
            int x = av->x + int(r * std::cos(a)), y = av->y - int(r * std::sin(a));
            World::Placement pl = u.w().can_exist_shape(foes[i], x, y, av->z, m);
            if (pl.valid && pl.supported && pl.land) { u.w().move_to(m, x, y, av->z, false); break; }
        }
    }
    u.frames(2);
    u.key('c');
    // Let a few turns pass (casts and blows in the log), then the Avatar's own turn.
    int awaiting = 0;
    for (int f = 0; f < 6000; ++f) {
        u.frames(1);
        if (u.w().turn->awaiting_player()) {
            if (u.w().turn->current() == 1 && ++awaiting > 1) break;
            u.key(' ');  // the others defend: the round goes on to the Avatar
        }
    }
    u.frames(20);
    u.mouse(250, 90);
    u.frames(4);
    u.shot("combat");
}

// The spellbook over Tenebrae: the spells the Avatar knows, with his reagents and foci.
void scene_spellbook(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.w().stages->forced = 3;
    u.w().stages->forced_period = 2;
    u.go(40, 10810, 14094, 48, 200);
    for (const Spellbook::Spell& sp : Spellbook::spells())
        if (sp.school == Spellbook::kNecromancy || sp.school == Spellbook::kTheurgy) u.w().spellbook->learn(sp.key());
    auto give = [&](unsigned shape, unsigned frame, unsigned q) {
        uint16_t id = u.w().create_item(shape, frame);
        if (Item* it = u.w().item(id)) it->quality = uint16_t(q);
        u.w().give_to_avatar(id);
    };
    for (unsigned f = 0; f < 32; ++f)
        if (Spellbook::reagent_kind(Spellbook::kNecromancy, 395, f) >= 0) give(395, f, 9);  // necromancy reagents
    for (unsigned f : {8u, 9u, 10u, 12u}) give(396, f, 0);  // theurgy foci
    if (Actor* av = u.w().avatar()) { av->mana = 40; av->intel = 40; }
    u.key('g');
    u.frames(6);
    u.mouse(250, 150);
    u.frames(2);
    u.shot("spellbook");
}

// The Avatar's sheet with the virtue octagon, and a companion's (her portrait head).
void scene_sheets(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.virtues_high();
    u.w().stages->forced = 3;
    u.w().stages->forced_period = 2;
    u.go(40, 10810, 14094, 48, 200);
    u.w().open_gump(1, 1);
    u.frames(3);
    int vi = u.p().find_def("varn");
    u.p().open_inventory(vi);
    u.frames(30);
    // Lay the sheets side by side, the packs out of the way.
    const uint16_t varn = u.p().state[size_t(vi)].id;
    u.frames(3);
    for (auto& g : u.w().gumps.list) {
        if (g->closed) continue;
        if (auto* pd = dynamic_cast<PaperdollGump*>(g.get())) {
            if (pd->owner == 1) { pd->x = 4; pd->y = 4; }
            else if (pd->owner == varn) { pd->x = 150; pd->y = 92; }
        } else if (!dynamic_cast<SpellbookGump*>(g.get())) {
            g->closed = true;  // the packs
        }
    }
    u.mouse(300, 190);
    u.shot("sheets");
}

// A Storm Eye on the Plane of Air, the party fighting it turn by turn.
void scene_flame(const GameData& d) {
    Promo u(d);
    party_up(u);
    Creatures& c = *u.w().creatures;
    const int fi = c.def_index("storm_eye");
    if (fi < 0) return;
    const CreaturePlace& fp = c.defs[size_t(fi)].places[0];
    u.go(fp.map, fp.x, fp.y + 260, fp.z, 300);
    Actor* av = u.w().avatar();
    av->set(Actor::kInvincible, true);
    av->set(Actor::kImmortal, true);
    for (const auto& st : u.p().state) if (Actor* a = u.w().actor(st.id)) a->set(Actor::kImmortal, true);
    uint16_t fl = c.spawn_at(fi, av->x + 60, av->y - 260, av->z);
    if (!fl) fl = c.spawn(fi, 0);
    u.frames(30);
    u.key('c');
    int turns = 0;
    for (int f = 0; f < 6000; ++f) {
        u.frames(1);
        if (u.w().turn->awaiting_player()) {
            if (u.w().turn->current() == 1 && ++turns > 1) break;
            u.key(' ');
        }
    }
    u.frames(20);
    u.mouse(250, 60);
    u.frames(4);
    u.shot("flame");
}

// Herdsman's Valley by day at the last stage: warm light, sun rays, the party on the road.
void scene_valley(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.virtues_high();
    u.w().stages->forced = 3;
    u.w().stages->forced_period = 2;
    u.go(7, 15071, 26000, 8, 400);
    u.mouse(300, 190);
    u.shot("valley");
}

// West Tenebrae's lamp posts glowing at night, the party in the street.
void scene_night(const GameData& d) {
    Promo u(d);
    party_up(u);
    u.w().stages->forced = 3;
    u.w().stages->forced_period = 5;
    u.go(39, 10850, 10600, 48, 300);
    u.mouse(300, 190);
    u.shot("night");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { std::printf("usage: pagan_promo_shots GAME_DIR DATA_DIR OUT_DIR [scene]\n"); return 2; }
    g_data = argv[2];
    g_out = argv[3];
    mkdir(g_out.c_str(), 0755);
    Config::global().load(g_out + "/promo.cfg");
    GameFS fs;
    if (!fs.open(argv[1])) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 4 ? argv[4] : "";
    auto run = [&](const char* n, void (*f)(const GameData&)) { if (only.empty() || only == n) f(d); };
    run("tenebrae", scene_tenebrae);
    run("combat", scene_combat);
    run("spellbook", scene_spellbook);
    run("sheets", scene_sheets);
    run("flame", scene_flame);
    run("valley", scene_valley);
    run("night", scene_night);
    return 0;
}
