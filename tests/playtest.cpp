// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The early-game play-test (milestone 4): a headless player drives the real game mode only
// through mouse and keyboard events, like a person would. New game with name entry, the
// opening, the walk to the dock (the execution opens the gate), into Tenebrae, talking to
// people, picking things up and equipping them, combat with the weapon overlay, saving in the
// diary, then a fresh engine that loads the save and plays on.
//   pagan_playtest GAME_DIR SHOTS_DIR SAVES_DIR
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "app/game_mode.h"
#include "data/gamedata.h"
#include "game/processes.h"
#include "gfx/surface.h"
#include "platform/platform.h"

using namespace pg;

namespace {

int g_problems = 0;
void problem(const char* what) {
    ++g_problems;
    std::printf("PROBLEM: %s\n", what);
}

struct Player {
    const GameData& d;
    std::unique_ptr<GameMode> gm;
    Surface8 surf{320, 200};
    std::string shots;
    int frame = 0;
    bool keep_auto = false;  // answer menus automatically

    Player(const GameData& data, const std::string& shot_dir, const std::string& saves) : d(data), shots(shot_dir) {
        gm = std::make_unique<GameMode>(d);
        gm->saves().set_dir(saves);
    }
    World& w() { return gm->world(); }
    Actor& av() { return *w().avatar(); }
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            gm->tick();
            gm->render(surf);
            ++frame;
        }
    }
    void shot(const std::string& name) {
        std::vector<uint32_t> rgba;
        surf.to_rgba(d.palette, gm->palette(), rgba);
        std::string path = shots + "/" + name + ".png";
        save_png(path, rgba.data(), 320, 200, 2);
        std::printf("shot %s (frame %d, map %u, avatar %d,%d,%d)\n", path.c_str(), frame, w().map().map_number(), av().x,
                    av().y, av().z);
    }
    void ev(InputEvent e) { gm->handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void button(int b, bool down) {
        InputEvent e;
        e.type = down ? InputEvent::kMouseDown : InputEvent::kMouseUp;
        e.button = b; e.x = gm->mouse_x(); e.y = gm->mouse_y();
        ev(e);
    }
    void key(int k, unsigned mods = 0) {
        InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; e.mods = mods; ev(e);
        e.type = InputEvent::kKeyUp; ev(e);
    }
    void type(const std::string& t) {
        for (size_t i = 0; i < t.size(); i += 7) {
            InputEvent e; e.type = InputEvent::kText;
            std::strncpy(e.text, t.substr(i, 7).c_str(), sizeof(e.text) - 1);
            ev(e);
        }
    }
    void click(int x, int y) { tick(14); move(x, y); button(1, true); tick(1); button(1, false); tick(16); }
    void double_click(int x, int y) {
        tick(14); move(x, y); button(1, true); button(1, false); tick(2); button(1, true); button(1, false); tick(4);
    }
    void drag(int x0, int y0, int x1, int y1) {
        tick(14);
        move(x0, y0); button(1, true); tick(1);
        for (int k = 1; k <= 6; ++k) { move(x0 + (x1 - x0) * k / 6, y0 + (y1 - y0) * k / 6); tick(1); }
        button(1, false); tick(2);
    }
    // A screen pixel showing object id near its anchor (from the last render).
    bool find_pixel(uint16_t id, int& sx, int& sy) {
        const Item* it = w().item(id);
        if (!it) return false;
        int ax, ay;
        w().screen_point(*it, ax, ay);
        for (int r = 0; r < 60; r += 2)
            for (int dy = -r; dy <= 4; dy += 2)
                for (int dx = -r / 2; dx <= r / 2; dx += 2) {
                    int x = ax + dx, y = ay + dy - r / 2;
                    if (x < 0 || y < 0 || x >= 320 || y >= 200) continue;
                    if (gm->pick(x, y) == id && !w().gumps.at(x, y)) { sx = x; sy = y; return true; }
                }
        return false;
    }
    // Answers menus like a curious player (each answer once, then the last one), skips
    // readers. Used through conversations.
    std::vector<std::string> given;
    void answer() {
        if (!w().ask_menu || w().ask_menu->answers.empty()) return;
        const AskMenu& m = *w().ask_menu;
        unsigned pick = unsigned(m.answers.size() - 1);
        for (unsigned i = 0; i < m.answers.size(); ++i) {
            std::string a = w().vm().string(m.answers[i]);
            if (std::find(given.begin(), given.end(), a) == given.end()) { pick = i; break; }
        }
        given.push_back(w().vm().string(m.answers[pick]));
        key('1' + int(pick));
    }
    // Runs until stasis ends (conversations, scenes), answering and closing readers.
    std::function<void(int)> on_wait;  // called each frame spent waiting
    int wait_free(int max_frames) {
        int f = 0;
        for (; f < max_frames; ++f) {
            if (on_wait) on_wait(f);
            if (w().ask_menu && w().ask_menu->frames_open > 40) answer();
            if (ReaderGump* r = w().reader(); r && r->frames_open++ > 60) key(kKeyEscape);
            tick();
            if (!w().stasis && !w().ask_menu && f > 5) break;
        }
        return f;
    }
    // Holds the right button with the cursor toward (x, y), like a player, until within 48.
    // When stuck: lets go and jumps (climbs), or sidesteps. Talks through anyone who halts him.
    bool walk_to(int x, int y, int max_frames = 3000) {
        int stuck = 0, lx = -1, ly = -1, rel = 0, jumps = 0, side = 0, sides = 0;
        bool held = false;
        unsigned map0 = w().map().map_number();
        for (int f = 0; f < max_frames; ++f) {
            if (w().map().map_number() != map0) break;
            int dx = x - av().x, dy = y - av().y;
            if (std::abs(dx) < 48 && std::abs(dy) < 48) break;
            if (w().stasis || w().ask_menu) {
                if (held) { button(3, false); held = false; }
                wait_free(6000);
                continue;
            }
            int dir = direction_to(dx, dy);
            if (side > 0) { --side; dir = (dir + ((sides & 1) ? 2 : 6)) & 7; }
            static const int sx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, sy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
            int sd = (dir + 1) & 7;
            bool run = std::abs(dx) + std::abs(dy) > 400 && side == 0;
            int mx = 160 + sx[sd] * (run ? 150 : 50), my = 114 + sy[sd] * (run ? 85 : 35);
            move(std::clamp(mx, 0, 319), std::clamp(my, 0, 199));
            if (rel > 0) {
                --rel;
                if (held) { button(3, false); held = false; }
                if (rel == 10) { InputEvent e; e.type = InputEvent::kKeyDown; e.key = kKeySpace; ev(e); }
                if (rel == 8) { InputEvent e; e.type = InputEvent::kKeyUp; e.key = kKeySpace; ev(e); }
            } else if (!held) {
                button(3, true);
                held = true;
            }
            if (av().x == lx && av().y == ly) ++stuck;
            else stuck = 0;
            lx = av().x;
            ly = av().y;
            if (stuck > 40) {
                stuck = 0;
                if (++jumps > 14) break;
                if (jumps % 2) rel = 20;
                else { side = 40; ++sides; }
            }
            tick();
        }
        if (held) button(3, false);
        tick(10);
        int dx = x - av().x, dy = y - av().y;
        return std::abs(dx) < 64 && std::abs(dy) < 64;
    }
};

uint16_t nearest_npc(World& w, int range) {
    std::vector<uint16_t> near;
    const Actor* av = w.avatar();
    w.area_search(av->x, av->y, range, false, near);
    uint16_t best = 0;
    int bd = 1 << 30;
    for (uint16_t k : near) {
        const Actor* a = w.actor(k);
        if (!a || k == 1 || a->is(Actor::kDead) || !(a->ext_flags & kExtPermanentNpc)) continue;
        int dd = std::abs(a->x - av->x) + std::abs(a->y - av->y);
        if (dd < bd) { bd = dd; best = k; }
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    std::string game = argc > 1 ? argv[1] : "../game";
    std::string shots = argc > 2 ? argv[2] : "shots";
    std::string saves = argc > 3 ? argv[3] : "/tmp/pagan_playtest_saves";
    GameFS fs;
    GameData d;
    if (!fs.open(game) || !d.load(fs)) { std::printf("cannot load the game from %s\n", game.c_str()); return 1; }
    std::system(("mkdir -p '" + shots + "' '" + saves + "'").c_str());
    std::remove((saves + "/pagan_002.sav").c_str());

    Player p(d, shots, saves);
    p.gm->start();
    World& w = p.w();
    // --- Name entry.
    p.tick(3);
    p.type("Mark");
    p.tick(4);
    p.shot("m4_name_entry");
    p.key(kKeyReturn);
    p.tick(2);
    if (w.avatar_name != "Mark" || w.gumps.find<MenuGump>()) problem("name entry");
    // --- The opening: Devon's conversation, answered by clicking (keys 1..9).
    bool shot_ask = false;
    for (int f = 0; f < 12000; ++f) {
        if (w.ask_menu && w.ask_menu->frames_open > 40) {
            if (!shot_ask) { p.shot("m4_opening_conversation"); shot_ask = true; }
            p.answer();
        }
        p.tick();
        if (!w.stasis && w.startup_state && f > 600 && !w.ask_menu) break;
    }
    std::printf("opening over at frame %d (stasis %d)\n", p.frame, w.stasis);
    if (w.stasis) problem("the opening did not end");
    // --- The main menu over the paused world.
    p.key(kKeyEscape);
    p.tick(2);
    p.move(200, 60);
    p.tick(2);
    p.shot("m4_main_menu");
    p.key(kKeyEscape);
    p.tick(2);
    if (w.gumps.find<MenuGump>()) problem("Esc did not close the menu");
    // --- Inventory at the camp: the paperdoll and the backpack.
    p.key('z');
    p.tick(3);
    p.key('i');
    p.tick(3);
    p.move(250, 180);
    p.tick(2);
    p.shot("m4_paperdoll_backpack");
    p.key(kKeyBackspace);
    p.tick(2);
    // --- Walk to the dock (via the road past the gate): the execution scene.
    static const int route[][2] = {{14600, 4700}, {14559, 3000}, {14100, 3100}, {13483, 2371}, {12031, 2311},
                                   {12031, 3000}, {11900, 4000}, {11800, 5000}, {11732, 5844}};
    bool halted = false;
    bool exec_shot = false;
    p.on_wait = [&](int f) {
        // The execution scene runs while the walk waits for the stasis to end.
        if (!exec_shot && f == 2500 && !w.vm().get_global(89, 1) && w.map().map_number() == 3 && p.av().y > 5000) {
            p.shot("m4_execution");
            exec_shot = true;
        }
    };
    for (const auto& r : route) {
        p.walk_to(r[0], r[1]);
        if (w.vm().get_global(89, 1)) break;
        if (w.stasis) halted = true;
    }
    (void)halted;
    // The scene (the Avatar in stasis until Tarna's talk ends).
    bool scene = false;
    for (int f = 0; f < 12000 && !w.vm().get_global(89, 1); ++f) {
        if (w.stasis) scene = true;
        if (f == 2500) p.shot("m4_execution");
        if (w.ask_menu && w.ask_menu->frames_open > 40) p.answer();
        p.tick();
    }
    p.wait_free(6000);
    p.on_wait = nullptr;
    std::printf("execution: %s, G[89] = %u, frame %d\n", scene ? "played" : "not seen", w.vm().get_global(89, 1), p.frame);
    if (!w.vm().get_global(89, 1)) problem("the execution did not happen");
    // --- To the gate (now open) and through the teleporter into Central Tenebrae.
    static const int back[][2] = {{11800, 5000}, {11900, 4000}, {12031, 3000}, {11700, 2350}};
    for (const auto& r : back) p.walk_to(r[0], r[1]);
    p.shot("m4_gate_open");
    for (int tries = 0; tries < 6 && w.map().map_number() == 3; ++tries) p.walk_to(11516, 1660, 1500);
    p.tick(30);
    std::printf("entered map %u at %d,%d,%d frame %d\n", w.map().map_number(), p.av().x, p.av().y, p.av().z, p.frame);
    if (w.map().map_number() != 40) { problem("did not reach Tenebrae"); }
    p.shot("m4_tenebrae");
    // --- Talk to people: walk up to the nearest NPC and double-click on them.
    int talks = 0;
    std::vector<uint16_t> talked;
    for (int attempt = 0; attempt < 6 && talks < 2; ++attempt) {
        uint16_t npc = 0;
        {
            std::vector<uint16_t> near;
            w.area_search(p.av().x, p.av().y, 1600, false, near);
            int bd = 1 << 30;
            for (uint16_t k : near) {
                const Actor* a = w.actor(k);
                if (!a || k == 1 || a->is(Actor::kDead) || !(a->ext_flags & kExtPermanentNpc)) continue;
                if (std::find(talked.begin(), talked.end(), k) != talked.end()) continue;
                int dd = std::abs(a->x - p.av().x) + std::abs(a->y - p.av().y);
                if (dd < bd) { bd = dd; npc = k; }
            }
        }
        if (!npc) break;
        talked.push_back(npc);
        const Actor* a = w.actor(npc);
        std::printf("talk: %s (npc %u) at %d,%d\n", a->name.c_str(), npc, a->x, a->y);
        p.walk_to(a->x + 64, a->y + 64, 1500);
        p.tick(5);
        int sx, sy;
        if (!p.find_pixel(npc, sx, sy)) { std::printf("  not on screen\n"); continue; }
        p.double_click(sx, sy);
        bool spoke = false;
        for (int f = 0; f < 4000; ++f) {
            if (!w.barks.empty()) spoke = true;
            if (spoke && f == 60 && talks == 0) p.shot("m4_talk_npc");
            if (w.ask_menu && w.ask_menu->frames_open > 40) {
                if (talks == 0 && !shot_ask) { p.shot("m4_talk_menu"); shot_ask = true; }
                p.answer();
            }
            p.tick();
            if (spoke && !w.stasis && !w.ask_menu && w.barks.empty() && f > 100) break;
        }
        std::printf("  %s\n", spoke ? "they talked" : "no answer");
        if (spoke) ++talks;
    }
    if (!talks) problem("nobody talked");
    // --- Pick something up: walk to a dagger lying in the city, drag it onto the Avatar
    // (into the backpack), then from the backpack onto the paperdoll (equipped).
    // The nearest loose item on the Avatar's level (equipment preferred).
    uint16_t dagger = 0;
    for (int attempt = 0; attempt < 4 && !dagger; ++attempt) {
        std::vector<uint16_t> near;
        w.area_search(p.av().x, p.av().y, 1200, false, near);
        int bd = 1 << 30;
        uint16_t pick = 0;
        for (uint16_t k : near) {
            const Item* it = w.item(k);
            if (!it || k < 256 || it->parent) continue;
            const ShapeInfo& si = d.typeflags.get(it->shape);
            unsigned fam = si.family();
            if (si.is(ShapeInfo::kFixed) || si.is(ShapeInfo::kEditor) || si.weight() == 0 || si.weight() > 20) continue;
            if (fam == kFamGlobEgg || fam == kFamUsecodeEgg || fam == kFamMonsterEgg || fam == kFamTeleportEgg) continue;
            if ((it->flags & kItemInvisible) || std::abs(it->z - p.av().z) > 8 || !it->contents.empty()) continue;
            int dd = std::abs(it->x - p.av().x) + std::abs(it->y - p.av().y) - (si.equip_type() ? 600 : 0);
            if (dd < bd) { bd = dd; pick = k; }
        }
        if (!pick) break;
        const Item* it = w.item(pick);
        std::printf("pick up %u (shape %u frame %u) at %d,%d,%d\n", pick, it->shape, it->frame, it->x, it->y, it->z);
        p.walk_to(it->x + 40, it->y + 40, 2500);
        p.wait_free(3000);
        p.tick(5);
        int sx, sy, ax, ay;
        if (p.find_pixel(pick, sx, sy) && p.find_pixel(1, ax, ay)) {
            p.shot("m4_before_pickup");
            p.drag(sx, sy, ax, ay);
            p.tick(5);
        }
        it = w.item(pick);
        std::printf("  after the drag: inside %u\n", it ? it->parent : 0);
        if (it && it->parent) dagger = pick;
    }
    if (!dagger) problem("pick up by dragging onto the Avatar");
    // Something to wield: the picked item if it is a weapon, else a dagger handed over by the
    // test (debug), still equipped below by dragging it onto the paperdoll.
    if (!dagger || d.typeflags.get(w.item(dagger)->shape).equip_type() != 6) {
        uint16_t extra = w.create_item(418, 0);
        if (!w.give_to_avatar(extra)) problem("cannot give a dagger");
        else std::printf("debug: a dagger (%u) put into the backpack for the equip test\n", extra);
        dagger = extra;
    }
    p.wait_free(3000);
    p.key('z');
    p.tick(3);
    p.key('i');
    p.tick(3);
    auto* pd = dynamic_cast<PaperdollGump*>(w.gumps.container_gump(1));
    auto* bp = dynamic_cast<ContainerGump*>(w.gumps.container_gump(w.equipped(1, 7)));
    if (bp && pd) {
        // Put the backpack beside the paperdoll.
        int gx = -1, gy = -1;
        for (int y = 0; y < bp->h && gx < 0; ++y)
            for (int x = 0; x < bp->w; ++x)
                if (bp->hit(x, y) && !bp->trace_item(x, y) && w.gumps.at(bp->x + x, bp->y + y) == bp) { gx = x; gy = y; break; }
        if (gx >= 0) p.drag(bp->x + gx, bp->y + gy, bp->x + gx + 140, bp->y + gy + 70);
        p.tick(2);
        p.move(300, 190);
        p.tick(1);
        p.shot("m4_backpack_with_weapon");
        if (dagger && w.item(dagger) && w.item(dagger)->parent == bp->owner) {
            int sx = 0, sy = 0;
            bool found = false;
            for (int y = 0; y < bp->h && !found; ++y)
                for (int x = 0; x < bp->w && !found; ++x)
                    if (bp->trace_item(x, y) == dagger) { sx = bp->x + x; sy = bp->y + y; found = true; }
            if (found) p.drag(sx, sy, pd->x + 7 + 40, pd->y + 9 + 30);
        }
        p.tick(3);
        p.move(300, 190);
        p.tick(1);
        p.shot("m4_paperdoll_equipped");
        if (!w.equipped(1, 6)) problem("weapon not equipped");
        else std::printf("equipped weapon %u\n", w.equipped(1, 6));
    } else {
        problem("paperdoll / backpack windows");
    }
    p.key(kKeyBackspace);
    p.tick(3);
    // --- Combat stance with the weapon drawn (overlay).
    p.key('c');
    p.tick(45);
    p.move(260, 120);
    p.tick(2);
    p.button(1, true); p.button(1, false); p.tick(2); p.button(1, true); p.button(1, false);  // attack
    p.tick(8);
    p.shot("m4_combat_overlay");
    p.tick(30);
    p.key('c');
    p.tick(30);
    // --- Save through the diary: Esc, 3 (Write Diary), entry 2, type, Enter.
    int save_x = p.av().x, save_y = p.av().y;
    unsigned save_map = w.map().map_number();
    uint16_t save_weapon = w.equipped(1, 6);
    p.key(kKeyEscape);
    p.tick(2);
    p.key('3');
    p.tick(2);
    MenuGump* menu = w.gumps.find<MenuGump>();
    if (menu && menu->mode() == MenuGump::kSave) {
        menu->spread = 0;
        p.click(menu->x + 43 + 20, menu->y + 6 + 43 + 15);
        for (int k = 0; k < 30; ++k) p.key(kKeyBackspace);
        p.type("In Tenebrae");
        p.tick(4);
        p.shot("m4_diary_save");
        p.key(kKeyReturn);
        p.tick(3);
    } else {
        problem("Write Diary did not open");
    }
    if (!p.gm->saves().exists(2)) problem("the save was not written");
    std::printf("saved at %d,%d map %u frame %d\n", save_x, save_y, save_map, p.frame);
    // Play on a little after saving (the world moves on).
    p.walk_to(save_x + 300, save_y, 400);
    // --- A fresh engine (like reloading the page): load entry 2 from the diary.
    Player q(d, shots, saves);
    q.gm->world().ask_name = false;
    q.gm->start();
    q.tick(30);
    q.key(kKeyEscape);
    q.tick(2);
    q.key('2');
    q.tick(2);
    MenuGump* m2 = q.w().gumps.find<MenuGump>();
    if (m2 && m2->mode() == MenuGump::kLoad) {
        q.move(m2->x + 43 + 20, m2->y + 6 + 43 + 15);
        q.tick(2);
        q.shot("m4_diary_load");
        q.click(m2->x + 43 + 20, m2->y + 6 + 43 + 15);
        q.tick(3);
    } else {
        problem("Read Diary did not open");
    }
    World& w2 = q.w();
    std::printf("loaded: map %u at %d,%d (saved %u at %d,%d), weapon %u (saved %u), name %s\n", w2.map().map_number(),
                q.av().x, q.av().y, save_map, save_x, save_y, w2.equipped(1, 6), save_weapon, w2.avatar_name.c_str());
    if (w2.map().map_number() != save_map || q.av().x != save_x || q.av().y != save_y) problem("load: wrong position");
    if (w2.equipped(1, 6) != save_weapon) problem("load: wrong equipment");
    if (w2.vm().get_global(89, 1) != 1) problem("load: story flag lost");
    q.tick(5);
    q.shot("m4_after_load");
    // Continue: walk about and talk again.
    q.walk_to(save_x - 200, save_y + 200, 600);
    if (uint16_t npc = nearest_npc(w2, 1200)) {
        const Actor* a = w2.actor(npc);
        q.walk_to(a->x + 64, a->y + 64, 1500);
        q.tick(5);
        int sx, sy;
        if (q.find_pixel(npc, sx, sy)) {
            q.double_click(sx, sy);
            bool spoke = false;
            for (int f = 0; f < 3000; ++f) {
                if (!w2.barks.empty()) spoke = true;
                if (f == 80) q.shot("m4_continue_after_load");
                if (w2.ask_menu && w2.ask_menu->frames_open > 40) q.answer();
                q.tick();
                if (spoke && !w2.stasis && !w2.ask_menu && w2.barks.empty() && f > 100) break;
            }
            std::printf("after load: %s %s\n", a->name.c_str(), spoke ? "talked" : "did not talk");
        }
    }
    std::printf("play-test done: %d problem(s), %d + %d frames\n", g_problems, p.frame, q.frame);
    return g_problems ? 1 : 0;
}
