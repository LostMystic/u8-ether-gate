// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The whole story played headlessly from a new game to the ending movie (specs/world/critical_path.md,
// data/critpath.json): every chapter drives the real engine as a player would, through the usecode
// (conversations answered like a player, items used, picked up and dropped, spells made and cast,
// eggs walked onto, map changes). Debug warps stand in for long walks, as in ctest `world`. Puzzles that
// are only manual dexterity (jumps over platforms) are skipped with a warp, each one listed in the log
// as "SHORTCUT" and in NOTES.md; steps the specs do not yet explain well enough to play (the sorcery pentagram
// tests) have their result set and are listed as "BYPASS" (QUESTIONS.md). After each chapter the critical-path evaluator (CritPath) reports the
// current step, and the world is saved, so one chapter can be replayed from the save before it:
//
//   pagan_story_tests GAME_DIR DATA_DIR SAVE_DIR [from-chapter [to-chapter]]
//   pagan_story_tests GAME_DIR DATA_DIR SAVE_DIR epilogue      (only the epilogue, from story_before_gate.sav)
//   PG_TALK=1 prints every answer menu; PG_BARKS=1 every line spoken; PG_SHOTS=dir for the epilogue picture.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <tuple>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "app/game_mode.h"
#include "game/credits.h"
#include "game/critpath.h"
#include "game/story_pack.h"
#include "game/virtues.h"
#include "json/json.hpp"
#include "gfx/surface.h"
#include "platform/platform.h"
#include "game/game_log.h"
#include "game/gumps.h"
#include "game/movie.h"
#include "game/processes.h"
#include "game/world.h"
#include "platform/input.h"
#include "usecode/uc_machine.h"

int g_fail = 0, g_pass = 0;

namespace {

using namespace pg;

std::string g_data, g_saves;
std::vector<std::string> g_shortcuts;

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Story {
    World w;
    int frames = 0;
    CritPath cp;
    WorldCritContext ctx{w};
    std::set<std::string> asked;  // answers given so far (a player prefers new ones)
    uint16_t target = 0;          // the answer to the next target cursor (0: cancel)
    std::vector<uint16_t> targets;  // answers to the next cursors in turn (before `target`)
    int cursors = 0;                // target cursors answered so far
    bool trace_talk = std::getenv("PG_TALK") != nullptr, trace_barks = std::getenv("PG_BARKS") != nullptr;
    std::vector<std::string> said;  // every line spoken since the last clear (for checks)

    explicit Story(const GameData& d) : w(d) {
        w.auto_ui = false;
        w.ask_name = false;
        std::string err;
        if (!cp.load_file(g_data + "/critpath.json", err) || !cp.load_file(g_data + "/map_links.json", err))
            std::printf("critpath: %s\n", err.c_str());
        // Virtues on, as the game's default setting (the epilogue after the ending movie).
        if (!std::getenv("PG_NO_VIRTUES")) {
            if (!w.virtues->load_dir(g_data, err)) std::printf("virtues data: %s\n", err.c_str());
            else w.virtues->enabled = true;
        }
    }
    unsigned g(unsigned pos, unsigned n = 1) { return w.vm().get_global(pos, n); }
    Actor& av() { return *w.avatar(); }
    unsigned map() { return w.map().map_number(); }

    // One frame: the UI a player would answer is answered first (speech hurried along, readers
    // closed after a moment, the target cursor given `target`).
    void step() {
        for (Bark& b : w.barks) {
            if (b.ticks_left > 2 && b.page < b.pages.size())
                for (const std::string& l : b.pages[b.page].lines) {
                    if (trace_barks) std::printf("      %u: %s\n", b.speaker, l.c_str());
                    said.push_back(l);
                }
            b.ticks_left = std::min(b.ticks_left, 2);
        }
        if (ReaderGump* r = w.reader()) {
            if (++reader_frames > 20) { r->key(kKeyEscape, 0); reader_frames = 0; }
        } else reader_frames = 0;
        if (w.target_pid) {
            uint16_t t = target;
            if (!targets.empty()) { t = targets.front(); targets.erase(targets.begin()); }
            if (Process* p = w.kernel().get(w.target_pid)) { p->result = t; p->terminate(); }
            w.target_pid = 0;
            ++cursors;
        }
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    int reader_frames = 0;
    std::function<int(const std::vector<std::string>&)> picker;  // a scene's own question (-1: the default)
    unsigned hour() { return w.game_seconds() / 900; }
    // Sleeps 4 x n hours on the bedroll (dropped beside the Avatar, unrolled, used).
    bool sleep(unsigned n) {
        Item* bed = carried(534);
        if (!bed) bed = carried(535);
        if (!bed) { for (Item* b : items(534)) if (!b->parent && std::abs(b->x - av().x) + std::abs(b->y - av().y) < 300) bed = b; }
        if (!bed) return false;
        uint16_t id = bed->id;
        if (bed->parent) drop(id, av().x + 64, av().y, av().z);
        if (w.item(id)->shape == 535) use(id, 60);  // the open one rolls up first
        unsigned h0 = hour();
        picker = [n](const std::vector<std::string>& m) { return m.size() > n && m[n] == std::to_string(n) ? int(n) : -1; };
        use(id, 30);
        settle(3000);
        picker = nullptr;
        take(w.item(id));
        if (w.item(id) && w.item(id)->shape == 535) {}  // (kept as it is)
        std::printf("    slept: hour %u -> %u\n", h0, hour());
        return hour() != h0;
    }
    int talk_patience = 3000;
    void run(int n) { for (int i = 0; i < n; ++i) { default_answer(); step(); } }
    // Answer menus nobody asked for (a scene's question): the first new answer.
    void default_answer() {
        if (!w.ask_menu || w.ask_menu->frames_open < 6 || w.ask_menu->answers.empty()) return;
        unsigned pick = unsigned(w.ask_menu->answers.size() - 1);
        for (unsigned i = 0; i < w.ask_menu->answers.size(); ++i)
            if (!asked.count(w.vm().string(w.ask_menu->answers[i]))) { pick = i; break; }
        if (picker) {
            std::vector<std::string> m;
            for (uint16_t a : w.ask_menu->answers) m.push_back(w.vm().string(a));
            int p = picker(m);
            if (p >= 0) pick = unsigned(p);
        }
        choose(pick);
    }
    void choose(unsigned i) {
        std::string a = w.vm().string(w.ask_menu->answers[i]);
        if (trace_talk) {
            std::printf("    menu:");
            for (uint16_t s : w.ask_menu->answers) std::printf(" [%s]", w.vm().string(s).c_str());
            std::printf(" -> [%s]\n", a.c_str());
        }
        asked.insert(a);
        w.choose_answer(i);
    }
    // Runs until stasis is off and no menu is up (a scene or a talk ends).
    void settle(int max = 6000) {
        for (int i = 0; i < max && (w.stasis || w.ask_menu || w.in_conversation()); ++i) { default_answer(); step(); }
    }
    void warp(int x, int y, int z, unsigned m) {
        w.kernel().kill(1, kProcAny);
        w.avatar()->set(Actor::kAnimLock, false);
        if (w.camera_followed() != 1) w.camera_follow(1);
        w.teleport_actor(1, x, y, z, m);
        w.stasis = false;
        run(10);
    }
    void release() {
        w.input.button[2].down = false;
        w.input.button[2].handled = true;
        w.input.key_jump = false;
    }
    // Walks toward (x, y) like the player (right button held toward the point, a jump or a side
    // step when stuck). Stops within 48, or when the map changes.
    bool walk_to(int x, int y, int max_frames = 1500) {
        unsigned m0 = map();
        int stuck = 0, lx = -1, ly = -1, rel = 0, jumps = 0, side = 0, sides = 0;
        for (int f = 0; f < max_frames; ++f) {
            if (map() != m0) { release(); return true; }
            int dx = x - av().x, dy = y - av().y;
            if (std::abs(dx) < 48 && std::abs(dy) < 48) { release(); return true; }
            PlayerInput& in = w.input;
            in.cursor_dir = direction_to(dx, dy);
            in.cursor_len = std::abs(dx) + std::abs(dy) > 400 ? 2 : 1;
            if (side > 0) {
                --side;
                in.cursor_dir = (in.cursor_dir + ((sides & 1) ? 2 : 6)) & 7;
                in.cursor_len = 1;
            }
            in.key_jump = false;
            if (rel > 0) {
                --rel;
                in.button[2].down = false;
                in.button[2].handled = true;
                if (rel == 10) in.key_jump = true;
            } else if (!in.button[2].down && !w.stasis) {
                in.button[2].press(in.now_ms, 0, 0);
            }
            if (std::getenv("PG_WALK")) std::printf("      walk f%d at %d,%d,%d anim %d jump %d side %d\n", f, av().x, av().y, av().z, int(av().last_anim), int(in.key_jump), side);
            if (av().x == lx && av().y == ly) ++stuck;
            else stuck = 0;
            lx = av().x;
            ly = av().y;
            if (stuck > 40 && !w.stasis) {
                stuck = 0;
                if (++jumps > 14) break;
                if (jumps % 2) rel = 20;
                else { side = 40; ++sides; }
            }
            default_answer();
            step();
        }
        release();
        return false;
    }
    // Walks into a teleporter / map exit at (x, y) and waits for the map change.
    bool walk_into(int x, int y, unsigned to_map, int max_frames = 1500) {
        walk_to(x, y, max_frames);
        for (int i = 0; i < 90 && map() != to_map; ++i) step();
        settle(3000);
        return map() == to_map;
    }

    // --- Conversations
    // Talks to `npc` (a double-click) and answers like a player: the first answer that holds the
    // earliest keyword of `prefer` (new answers first), else a new answer, else one holding a word
    // of `leave` ("bye"); never an answer holding a word of `avoid`. Ends when the talk ends or
    // `done` is true (then it says goodbye). True if a talk happened.
    bool talk(uint16_t npc, const std::vector<std::string>& prefer, const std::function<bool()>& done = {},
              const std::vector<std::string>& avoid = {}, int max_answers = 60, bool use = true) {
        Actor* a = w.actor(npc);
        if (use && a) {
            if (a->mapnum != map()) { std::printf("    (npc %u is on map %u, not %u)\n", npc, a->mapnum, map()); return false; }
            if (std::abs(a->x - av().x) + std::abs(a->y - av().y) > 100 || std::abs(a->z - av().z) > 24) warp(a->x + 64, a->y, a->z, map());
            // The last talk's script may still be finishing (a second use would be excluded).
            for (int i = 0; i < 3000 && w.kernel().count_exact(npc, 0x207); ++i) { default_answer(); step(); }
            w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") != nullptr;
            w.vm().trace = std::getenv("PG_UC_OPS") != nullptr;
            uint16_t pid = w.call_event(npc, kEvUse);
            if (!pid) std::printf("    (npc %u: no use process)\n", npc);
            else if (w.vm().trace) std::printf("    (use pid %u)\n", pid);
        } else if (use) {
            w.use_item(npc);
        }
        bool in_talk = false;
        int answers = 0;
        for (int f = 0; f < 20000; ++f) {
            if (w.ask_menu && w.ask_menu->frames_open >= 6 && !w.ask_menu->answers.empty()) {
                std::vector<std::string> m;
                for (uint16_t s : w.ask_menu->answers) m.push_back(w.vm().string(s));
                bool finished = (done && done()) || answers >= max_answers;
                int pick = -1, best = -1000;
                for (size_t i = 0; i < m.size(); ++i) {
                    const std::string& s = m[i];
                    bool bye = s.find("bye") != std::string::npos || s.find("Bye") != std::string::npos || s.find("arewell") != std::string::npos;
                    int score = asked.count(s) ? 0 : 10;
                    bool preferred = false;
                    for (size_t k = 0; k < prefer.size(); ++k)
                        if (s.find(prefer[k]) != std::string::npos) { score += 1000 - int(k) * 10; preferred = true; break; }
                    if (!preferred) for (const std::string& v : avoid) if (s.find(v) != std::string::npos) score = -100;
                    if (bye) score = finished ? 5000 : -50;
                    if (score > best) { best = score; pick = int(i); }
                }
                if (pick >= 0) { choose(unsigned(pick)); ++answers; }
            }
            step();
            if (!in_talk && f > talk_patience) {
                std::printf("    (no talk with %u)\n", npc);
                if (std::getenv("PG_PROCS")) w.kernel().for_each([&](Process* p) {
                    auto* u = dynamic_cast<UCProcess*>(p);
                    std::printf("      pid %u %s item %u type %u%s%s", p->pid(), p->name(), p->item, p->type, p->suspended() ? " suspended" : "",
                                p->terminated() ? " terminated" : "");
                    if (u) std::printf(" uc %u:%04x ip %04x cls %u", u->start_cls, u->start_addr, u->ip, u->cls);
                    std::printf("\n");
                });
                break;
            }
            if (w.stasis || w.ask_menu || w.in_conversation()) in_talk = true;
            if (in_talk && !w.stasis && !w.ask_menu && !w.in_conversation()) break;
        }
        return in_talk;
    }

    // --- Items
    std::vector<Item*> items(unsigned shape, int frame = -1, int quality = -1) {
        std::vector<Item*> out;
        for (unsigned i = 1; i < w.objects().slots(); ++i)
            if (Item* it = w.item(uint16_t(i)); it && it->shape == shape && (frame < 0 || it->frame == frame) &&
                                                (quality < 0 || it->quality == quality))
                out.push_back(it);
        return out;
    }
    Item* nearest(unsigned shape, int x, int y, int frame = -1, int quality = -1) {
        Item* best = nullptr;
        long bd = 1L << 40;
        for (Item* it : items(shape, frame, quality)) {
            int ix, iy, iz;
            w.location(*it, ix, iy, iz);
            long d = std::labs(ix - x) + std::labs(iy - y);
            if (d < bd) { bd = d; best = it; }
        }
        return best;
    }
    // In the Avatar's inventory (recursively)?
    Item* carried(unsigned shape, int frame = -1, int quality = -1) {
        for (Item* it : items(shape, frame, quality))
            if (it->parent && w.root_container(*it)->id == 1) return it;
        return nullptr;
    }
    int count_carried(unsigned shape, int frame = -1) {
        int n = 0;
        for (Item* it : items(shape, frame))
            if (it->parent && w.root_container(*it)->id == 1) n += std::max<int>(1, it->quality);
        return n;
    }
    uint16_t pack() { return w.equipped(1, 7); }
    // Goes next to an item in the world and picks it up into the backpack (the drag onto the
    // Avatar of GameMode::perform_drop: grab, give, the theft check).
    bool take(Item* it) {
        if (!it) return false;
        uint16_t id = it->id;
        int x, y, z;
        w.location(*it, x, y, z);
        if (std::abs(x - av().x) + std::abs(y - av().y) > 200 || std::abs(z - av().z) > 40) warp(x + 48, y + 48, z, map());
        it = w.item(id);
        if (!it) return false;
        bool from_world = it->chunk >= 0 && !it->parent;
        if (from_world) w.grab(id);
        if (it->parent) {  // in a container: open it first (its gump), as the player does
            const Item* root = w.root_container(*it);
            if (root->id != 1 && !w.gumps.container_gump(it->parent)) use(it->parent, 10);
            it = w.item(id);
            if (!it) return false;
        }
        if (!w.give_to_avatar(id)) { std::printf("    (could not take item %u shape %u)\n", id, it->shape); return false; }
        w.theft_check(id);
        run(3);
        return true;
    }
    // Opens the container nearest (x, y) on this map holding `shape` (a double-click) and takes
    // every item of that shape out of it. The number taken (stack quantities).
    int loot(int x, int y, unsigned shape) {
        Item* best = nullptr;
        long bd = 1L << 40;
        for (Item* it : items(shape)) {
            if (!it->parent || w.root_container(*it)->id == 1) continue;
            int ix, iy, iz;
            w.location(*it, ix, iy, iz);
            long d = std::labs(ix - x) + std::labs(iy - y);
            if (d < bd) { bd = d; best = it; }
        }
        if (!best || bd > 256) { std::printf("    (nothing of shape %u near %d,%d)\n", shape, x, y); return 0; }
        const Item* root = w.root_container(*best);
        uint16_t cont = best->parent;
        int rx, ry, rz;
        w.location(*root, rx, ry, rz);
        warp(rx + 48, ry + 48, rz, map());
        use(root->id, 20);
        if (cont != root->id) use(cont, 20);
        int n = 0;
        std::vector<uint16_t> ids;
        for (Item* it : items(shape)) if (it->parent == cont) ids.push_back(it->id);
        for (uint16_t id : ids) { n += std::max<int>(1, w.item(id)->quality); take(w.item(id)); }
        w.close_gump(cont);
        if (cont != root->id) w.close_gump(root->id);
        return n;
    }
    // Uses an item on a target (the use starts a target cursor, answered with `t`).
    void use_on(uint16_t id, uint16_t t, int n = 90) {
        target = t;
        w.call_event(id, kEvUse);
        run(n);
        target = 0;
    }
    // Necromancy (magic.md 4.2): moves the carried reagents (shape, frame) into the bag, uses the
    // Key of the Caretaker / Scion on it and takes the token out into the backpack. The token.
    Item* make_spell(uint16_t bag, const std::vector<std::pair<unsigned, unsigned>>& reagents) {
        std::vector<uint16_t> left(w.item(bag)->contents.begin(), w.item(bag)->contents.end());
        for (uint16_t k : left) w.move_to_container(k, pack(), true);  // an empty bag first
        for (auto [sh, fr] : reagents) {
            bool have = false;
            for (Item* it : items(sh, int(fr))) have = have || (it->parent && w.root_container(*it)->id == 1);
            if (!have) { std::printf("    (no reagent %u:%u carried)\n", sh, fr); return nullptr; }
        }
        for (auto [sh, fr] : reagents) {
            Item* r = nullptr;
            for (Item* it : items(sh, int(fr))) if (it->parent && it->parent != bag && w.root_container(*it)->id == 1) r = it;
            if (!r) { std::printf("    (no reagent %u:%u carried)\n", sh, fr); return nullptr; }
            if (r->quality > 1) {  // one off the stack (the slider)
                uint16_t one = w.create_item(sh, fr);
                w.item(one)->quality = 1;
                r->quality = uint16_t(r->quality - 1);
                w.move_to_container(one, bag, true);
            } else {
                w.move_to_container(r->id, bag, true);
            }
        }
        // KEYONEC frame 3 (the Key of the Scion) only runs the door-key code (KEYRING) in the
        // original scripts this replay plays (the story patches are not loaded: the usecode patch
        // scion_bag_spells_* that lets it make spells is tested in ctest `storyfixes`), so the Key
        // of the Caretaker (frame 0) is used.
        Item* key = nullptr;
        for (Item* k : items(232)) if (k->parent && w.root_container(*k)->id == 1 && k->frame != 3) key = k;
        if (!key) { std::printf("    (no necromancy key)\n"); return nullptr; }
        said.clear();
        w.vm().trace_intrinsics = std::getenv("PG_KEY") != nullptr;
        use_on(key->id, bag, 120);
        w.vm().trace_intrinsics = false;
        Item* tok = nullptr;
        for (uint16_t k : w.item(bag)->contents) if (Item* it = w.item(k); it && it->shape == 746) tok = it;
        if (!tok) {
            std::printf("    (no spell: key frame %u; bag holds", key->frame);
            for (uint16_t k : w.item(bag)->contents) if (Item* it = w.item(k)) std::printf(" %u:%u x%u", it->shape, it->frame, it->quality);
            std::printf("; said:");
            for (auto& l : said) std::printf(" '%s'", l.c_str());
            std::printf(")\n");
        }
        if (tok) w.move_to_container(tok->id, pack(), true);
        return tok;
    }
    // The Recall item (833): its menu, the answer holding `place`. True if the map changed.
    bool recall(const std::string& place) {
        Item* r = carried(833);
        if (!r) { std::printf("    (no Recall item)\n"); return false; }
        unsigned m0 = map();
        std::vector<std::string> seen;
        picker = [&](const std::vector<std::string>& m) {
            seen = m;
            for (size_t i = 0; i < m.size(); ++i) if (m[i].find(place) != std::string::npos) return int(i);
            return -1;
        };
        use_event(r->id, 30);
        settle(3000);
        run(60);
        picker = nullptr;
        if (map() == m0) {
            std::printf("    (recall to '%s' failed; menu:", place.c_str());
            for (auto& x : seen) std::printf(" [%s]", x.c_str());
            std::printf(")\n");
        }
        return map() != m0;
    }
    // A carried reagent frame of a kind (blood 0-5, bone 6-7, wood 8, dirt 9, hood 10-12,
    // blackmoor 14-15; bugfixes.md 2.2), or -1.
    int reagent(const std::string& kind) {
        static const std::map<std::string, std::pair<int, int>> k = {{"blood", {0, 5}}, {"bone", {6, 7}}, {"wood", {8, 8}}, {"dirt", {9, 9}},
                                                                     {"hood", {10, 12}}, {"blackmoor", {14, 15}}};
        auto [a, b] = k.at(kind);
        for (int f = a; f <= b; ++f) if (carried(395, f)) return f;
        return -1;
    }
    std::pair<unsigned, unsigned> R(const std::string& kind) { int f = reagent(kind); return {395u, unsigned(f < 0 ? 99 : f)}; }
    // Drops an item on the ground at a point (GameMode::perform_drop's place: grab, move, fall).
    void drop(uint16_t id, int x, int y, int z) {
        Item* it = w.item(id);
        if (!it) return;
        if (it->chunk >= 0 && !it->parent) w.grab(id);
        w.collide_move(id, x, y, z, true, true);
        w.fall(id);
        run(10);
    }
    // Uses an item (a double-click) and plays on for n frames.
    void use(uint16_t id, int n = 60) {
        w.use_item(id);
        run(n);
    }
    // Usecode event on an item directly (for an item in the backpack, as the inventory gump does).
    void use_event(uint16_t id, int n = 60) {
        w.call_event(id, kEvUse);
        run(n);
    }
    uint16_t give(unsigned shape, unsigned frame, unsigned quality = 0, uint16_t cont = 0) {
        uint16_t id = w.create_item(shape, frame);
        w.item(id)->quality = uint16_t(quality);
        w.move_to_container(id, cont ? cont : pack());
        return id;
    }

    // --- Progress
    int current_step() {
        ctx.observe();
        return cp.current(ctx);
    }
    std::string step_name(int i) { return i < 0 || i >= int(cp.steps.size()) ? std::string("(the end)") : cp.steps[size_t(i)].id; }
    bool done(const std::string& id) {
        ctx.observe();
        int i = cp.step_index(id);
        if (i < 0) return false;
        std::vector<bool> c = cp.complete(cp.done(ctx));
        return c[size_t(i)];
    }
    void save(const std::string& name) {
        SaveWriter o;
        w.save_game(o, name);
        std::ofstream f(g_saves + "/" + name + ".sav", std::ios::binary);
        f.write(reinterpret_cast<const char*>(o.data().data()), std::streamsize(o.data().size()));
    }
    bool load(const std::string& name) {
        std::ifstream f(g_saves + "/" + name + ".sav", std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        SaveReader in(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        if (!w.load_game(in)) return false;
        run(5);
        return true;
    }
};

std::vector<std::string> g_bypasses;
struct Story;
// A story step the script cannot play yet for want of spec knowledge (not dexterity): its
// result is set directly and listed in the log and the report as BYPASS (see QUESTIONS.md).
[[maybe_unused]] void bypass(Story& s, const std::string& what, const std::function<void()>& set);
void shortcut(const std::string& what) {
    std::printf("    SHORTCUT: %s\n", what.c_str());
    g_shortcuts.push_back(what);
}

[[maybe_unused]] void bypass(Story&, const std::string& what, const std::function<void()>& set) {
    std::printf("    BYPASS: %s\n", what.c_str());
    g_bypasses.push_back(what);
    set();
}

// A milestone: the critical-path step `id` is complete (CHECKed).
bool expect(Story& s, const std::string& id, const std::string& how) {
    bool ok = s.done(id);
    std::printf("  %-26s %s  map %u at %d,%d,%d, frame %d  %s\n", id.c_str(), ok ? "ok " : "NO ", s.map(), s.av().x, s.av().y, s.av().z,
                s.frames, how.c_str());
    CHECK(ok);
    return ok;
}

// =====================================================================================
// Chapter 1: the opening to the Cemetery (as ctest `world`'s replay, Original maps).
bool ch_opening(Story& s) {
    s.w.new_game();
    s.run(8400);  // the opening ends and stasis is released at about frame 8300
    expect(s, "talk_devon", "new game, the opening conversation");

    s.take(s.nearest(535, 14431, 5727));
    expect(s, "bedroll", "picked up the bedroll by the campfire");
    s.warp(11732, 5844, 48, 3);
    bool started = false;
    for (int f = 0; f < 9000; ++f) {
        s.step();
        if (s.w.stasis) started = true;
        if (started && !s.w.stasis && s.g(89)) break;
    }
    s.settle(3000);
    expect(s, "execution", "debug warp to the dock's end, the scene plays");

    for (auto [x, y] : {std::pair{11800, 4600}, {11650, 3200}, {11560, 2300}, {11516, 1660}}) {
        if (s.map() != 3) break;
        s.walk_to(x, y, 2500);
        s.settle(3000);
    }
    if (s.map() == 3) { s.warp(11516, 1700, 48, 3); s.run(60); }
    for (int i = 0; i < 60 && s.map() == 3; ++i) s.step();
    expect(s, "enter_tenebrae", "walked from the dock through the gate");

    s.warp(13087, 8800, 104, 40);
    s.walk_to(13087, 8607, 600);
    s.run(30);
    s.settle(2000);
    expect(s, "central_waypoint", "walked onto the dragon-statue waypoint");

    s.warp(14900, 14070, 48, 40);
    s.walk_into(15800, 14070, 41, 600);
    for (int tries = 0; tries < 4 && !s.g(295); ++tries)
        s.talk(12, {"Mythran", "Plateau", "scholar", "neighbor", "leav", "Devon", "people"}, [&] { return s.g(295) == 1; });
    expect(s, "ask_bentic", "East Tenebrae, talked to Bentic");
    int coins = s.loot(19039, 6399, 143) + s.loot(16287, 7647, 143);
    std::printf("    took %d obsidian coins from two chests in East Tenebrae (%d carried)\n", coins, s.count_carried(143));

    s.warp(13054, 2700, 48, 40);
    s.walk_into(13054, 2322, 7, 900);
    s.warp(9498, 20000, 8, 7);
    s.walk_into(9498, 19702, 8, 900);
    expect(s, "to_plateau", "the north exit, Herdsman's Valley, the cave");
    return true;
}

// Chapter 2: the Plateau: the lever room, Mythran, the Recall item, the scroll, the waypoint.
bool ch_plateau(Story& s) {
    // The lever room (LEVER, quality 20..25 on the west wall at x 25183, 26 by the winch).
    auto levers = [&] {
        std::printf("    levers:");
        for (unsigned q = 20; q <= 27; ++q) if (Item* l = s.items(672, -1, int(q)).empty() ? nullptr : s.items(672, -1, int(q))[0]) std::printf(" q%u f%u", q, l->frame);
        std::printf("  G[557..564]:");
        for (unsigned b = 557; b <= 564; ++b) std::printf(" %u", s.g(b));
        std::printf("\n");
    };
    // The levers: q21, q22 and q24 (set q20..q22 down, q23..q25 up), then the lever q27 by the
    // fence (found by trying every setting; the hint's "three southern down, three northern up").
    s.warp(25250, 25500, 112, 8);
    for (unsigned q : {21u, 22u, 24u, 27u}) {
        Item* l = s.items(672, -1, int(q))[0];
        s.warp(l->x + 64, l->y, l->z, 8);
        s.use(l->id, 90);
    }
    expect(s, "lever_room", "pulled the levers q21, q22, q24 and the fence lever q27");
    if (std::getenv("PG_LEVER_BRUTE")) {
        for (unsigned c = 0; c < 64; ++c) {
            s.load("story_after_opening");
            auto pull = [&](unsigned q) { Item* l = s.items(672, -1, int(q))[0]; s.warp(l->x + 64, l->y, l->z, 8); s.use(l->id, 90); };
            for (unsigned q = 20; q <= 25; ++q) pull(q);
            for (unsigned q = 20; q <= 25; ++q) if ((s.items(672, -1, int(q))[0]->frame == 2) != bool(c >> (q - 20) & 1)) pull(q);
            unsigned before = s.g(563);
            pull(27);
            unsigned a = s.g(563), b = s.g(564);
            pull(26);
            Item* wi = s.items(278)[0];
            s.warp(wi->x + 64, wi->y, wi->z, 8);
            s.use(wi->id, 300);
            std::printf("    config %02x: G563 %u -> %u, G564 %u, after winch G563 %u G564 %u\n", c, before, a, b, s.g(563), s.g(564));
        }
    }
    if (const char* e = std::getenv("PG_LEVERS")) {
        s.warp(25250, 25500, 112, 8);
        levers();
        for (const char* c = e; *c; ++c) {
            if (*c == 'L') { s.load("story_after_opening"); s.warp(25250, 25500, 112, 8); std::printf("    reload\n"); continue; }
            if (*c == 'w') {
                Item* wi = s.items(278)[0];
                s.warp(wi->x + 64, wi->y, wi->z, 8);
                s.use(wi->id, 300);
                std::printf("    winch: ");
                levers();
                continue;
            }
            unsigned q = unsigned(*c - 'a' + 20);
            Item* l = s.items(672, -1, int(q))[0];
            s.warp(l->x + 64, l->y, l->z, 8);
            s.use(l->id, 90);
            std::printf("    used q%u: ", q);
            levers();
        }
    }
    Actor* m = s.w.actor(33);
    if (m) std::printf("    Mythran at map %u %d,%d,%d\n", m->mapnum, m->x, m->y, m->z);
    for (int tries = 0; tries < 3 && !s.g(284); ++tries) s.talk(33, {"name"}, [&] { return s.g(284) == 1; });
    expect(s, "meet_mythran", "talked to Mythran");
    expect(s, "recall_item", "his parting gifts");
    s.talk(33, {"buy scrolls", "scroll", "Aye", "will pay"}, [&] { return s.carried(827, -1, 1) != nullptr; });
    expect(s, "portal_scroll", "bought the scroll");
    s.warp(2783, 1900, 56, 8);
    s.walk_to(2783, 1823, 300);
    s.run(30);
    expect(s, "plateau_waypoint", "walked onto the waypoint upstairs");
    return true;
}

// Chapter 3: the Cemetery, Vividos, Aramina's key, the ceremonial dagger, Lothian's passage.
bool ch_cemetery(Story& s) {
    s.warp(22700, 11007, 48, 41);
    s.walk_into(23500, 11007, 5, 600);
    s.warp(7423, 11300, 48, 5);
    s.walk_into(7423, 10943, 6, 900);
    expect(s, "to_cemetery", "East Road, the cemetery gate");
    Actor* v = s.w.actor(21);
    std::printf("    Vividos at map %u %d,%d,%d\n", v->mapnum, v->x, v->y, v->z);
    for (int t = 0; t < 3 && !s.g(212); ++t)
        s.talk(21, {"problems", "Power mad", "dagger back", "Mordea took", "dagger"}, [&] { return s.g(212) == 1; });
    expect(s, "volunteer_vividos", "talked to Vividos");
    // Aramina (NPC 13) talks only at her house at Bloodwatch: sleep on the bedroll 4 hours at a
    // time and try her after each.
    for (int t = 0; t < 7 && !s.carried(82, -1, 48); ++t) {
        Actor* a = s.w.actor(13);
        std::printf("    hour %u (of day %u): Aramina on map %u at %d,%d,%d\n", s.hour(), s.hour() % 24, a->mapnum, a->x, a->y, a->z);
        if (a->mapnum == 41 || a->mapnum == 40 || a->mapnum == 39) {
            if (s.map() != a->mapnum) s.warp(a->x + 64, a->y, a->z, a->mapnum);
            s.talk(13, {"dagger", "promise", "key", "Yes", "Mordea"}, [&] { return s.carried(82, -1, 48) != nullptr; });
        }
        if (!s.carried(82, -1, 48)) s.sleep(1);
    }
    expect(s, "aramina_key", "Aramina's key");

    // The Royal Hall: the key under the cushion (quality 33) opens Mordea's bedroom (door quality
    // 0x621), Aramina's (48) the room with the big chest (0x630); the dagger is in the jewellery box.
    s.warp(12622 + 64, 9010 + 64, 72, 40);
    Item* k33 = s.nearest(82, 12622, 9010, -1, 33);
    s.take(k33);
    std::printf("    cushion key taken: %d\n", s.carried(82, -1, 33) != nullptr);
    auto unlock_and_open = [&](unsigned key_q, int dx, int dy) {
        Item* door = nullptr;
        for (unsigned sh : {68u, 69u, 340u, 342u})
            for (Item* d : s.items(sh)) if ((d->quality & 0xFF) == key_q) door = d;
        if (!door) { std::printf("    no door for key %u\n", key_q); return; }
        uint16_t did = door->id;
        s.warp(door->x + dx, door->y + dy, door->z, s.map());
        unsigned q0 = door->quality;
        s.use_on(s.carried(82, -1, int(key_q))->id, did);
        std::printf("    door %u quality %04x -> %04x after the key\n", did, q0, s.w.item(did)->quality);
        s.use(did, 60);
        std::printf("    door %u quality %04x frame %u after use\n", did, s.w.item(did)->quality, s.w.item(did)->frame);
    };
    unlock_and_open(33, 0, 64);
    s.walk_to(12799 - 100, 8447 - 200, 600);
    unlock_and_open(48, 64, 0);
    Item* dag = s.nearest(571, 12692, 7796);
    std::printf("    dagger in container %u\n", dag ? dag->parent : 0);
    s.loot(12692, 7796, 571);
    expect(s, "get_dagger", "unlocked the bedroom and the chest room, took the dagger from the jewellery box");

    // Back to the tomb: Vividos takes the dagger, Lothian's passage plays.
    s.warp(7423, 11300, 48, 5);
    s.walk_into(7423, 10943, 6, 900);
    for (int t = 0; t < 3 && !s.g(189); ++t)
        s.talk(21, {"dagger", "I have", "Yes"}, [&] { return s.g(189) == 1; }, {"Farewell"}, 40);
    s.settle(6000);
    expect(s, "lothian_ritual", "gave Vividos the dagger, the ritual played");
    for (int t = 0; t < 3 && !s.g(197); ++t)
        s.talk(21, {"reagents", "honored", "apprentice", "Yes", "Necromancer"}, [&] { return s.g(197) == 1; }, {}, 40);
    expect(s, "apprentice", "accepted the apprenticeship");
    expect(s, "reagent_task", "asked what reagents he needs");
    return true;
}

// Chapter 4: the reagents, the Key of the Caretaker, Open Ground, into the catacombs.
bool ch_necromancy(Story& s) {
    s.av().set(Actor::kInvincible, true);
    shortcut("from the catacombs on, the Avatar is invulnerable (Actor kInvincible): fights are not scripted");
    // An executioner's hood from the changeling pit by the East Road.
    s.warp(11500, 13050, 48, 5);
    s.take(s.nearest(395, 11535, 13071, 10));
    expect(s, "executioners_hood", "picked a hood on the mound (map 5)");
    // The deadman's elbow at the dead tree in West Tenebrae.
    s.warp(15103 + 64, 2655 + 64, 48, 39);
    s.take(s.nearest(395, 15103, 2655, 20));
    expect(s, "deadmans_elbow", "picked the elbow (map 39)");
    s.warp(7423, 11300, 48, 5);
    s.walk_into(7423, 10943, 6, 900);
    for (int t = 0; t < 3 && !s.g(199); ++t)
        s.talk(21, {"brought you the reagents", "reagents"}, [&] { return s.g(199) == 1; }, {}, 30);
    expect(s, "key_caretaker", "gave Vividos the hood and the elbow");
    std::printf("    hp %d, mana %d\n", s.av().hp, s.av().mana);
    // Upstairs in the tomb: the bag (blood and bone in it: Death Speak) and the other reagents.
    Item* bag = s.nearest(637, 7291, 24936);
    uint16_t bag_id = bag ? bag->id : 0;
    s.warp(7291 + 48, 24936 + 48, 80, 6);
    s.take(bag);
    for (auto [f, x, y] : std::vector<std::tuple<int, int, int>>{{3, 7826, 25161}, {14, 7829, 25204}, {4, 7244, 25015}, {15, 7839, 25343},
                                                                {6, 7405, 25422}, {8, 7327, 25407}, {9, 7452, 25446}, {3, 7420, 25468}})
        s.take(s.nearest(395, x, y, f));
    std::printf("    carried: bag %d, blood %d, bone %d, wood %d, dirt %d, blackmoor %d\n", bag_id && s.w.item(bag_id)->parent,
                s.count_carried(395, 2) + s.count_carried(395, 3) + s.count_carried(395, 4), s.count_carried(395, 6) + s.count_carried(395, 7),
                s.count_carried(395, 8), s.count_carried(395, 9), s.count_carried(395, 14) + s.count_carried(395, 15));
    Item* ds = s.make_spell(bag_id, {});  // the bag holds blood and bone already
    std::printf("    Death Speak from the bag as found: %s\n", ds && ds->frame == 0 ? "made" : "not made");
    Item* og = s.make_spell(bag_id, {{395, 3}, {395, 14}});
    expect(s, "make_open_ground", "blood and blackmoor in the bag, the Key of the Caretaker used on it");
    // The stone building north-east of the tomb: the passage ends at a rock wall in front of
    // the teleporter to the Upper Catacombs; Open Ground opens it.
    // Open Ground destroys the rock wall (shape 566, at (7775,19455,8)) within 1280 and sets
    // G[216:1] (leveling_facts.md 1.5). Cast from the passage, then walk in.
    Item* wall = s.nearest(566, 7775, 19455);
    std::printf("    rock wall %u at %d,%d,%d\n", wall ? wall->id : 0, wall ? wall->x : 0, wall ? wall->y : 0, wall ? wall->z : 0);
    // Without the spell the wall (6 x 1 tiles: x 7583-7775, y 19423-19455) blocks the way.
    // (Start the walk clear of it: the Avatar's 64 x 64 box at y 19500 would already overlap the
    // wall (y 19423-19455), and an animation that begins inside something is not stopped by it.)
    uint16_t wall_id = wall ? wall->id : 0;
    s.warp(7676, 19560, 8, 6);
    s.walk_into(7676, 19316, 50, 400);
    std::printf("    walk without Open Ground: map %u at %d,%d,%d\n", s.map(), s.av().x, s.av().y, s.av().z);
    CHECK_EQ(s.map(), 6u);
    CHECK(s.av().y > 19455);
    if (s.map() != 6) s.warp(7676, 19560, 8, 6);
    s.warp(7775 + 200, 19455 + 300, 8, 6);
    if (og) s.use_on(og->id, wall ? wall->id : 0, 300);
    std::printf("    Open Ground: G[216] %u, wall left %d, mana %d\n", s.g(216), s.w.item(wall_id) && s.w.item(wall_id)->shape == 566, s.av().mana);
    CHECK_EQ(s.g(216), 1u);
    CHECK(!(s.w.item(wall_id) && s.w.item(wall_id)->shape == 566));
    s.warp(7676, 19560, 8, 6);
    s.walk_into(7676, 19316, 50, 600);
    expect(s, "enter_catacombs", "the Upper Catacombs");
    // Through Upper Catacombs 1 (long walk: a warp) to the hidden pad where the floor gives way.
    s.warp(17311 + 160, 1919, 0, 50);
    s.walk_to(17311 - 16, 1919 - 16, 400);
    for (int i = 0; i < 300 && s.map() == 50; ++i) s.step();
    s.settle(3000);
    expect(s, "fall_to_ancients", "walked onto the hidden pad (HIDPAD), fell to map 62");
    // Death Speak tokens from the reagents where he lands (blood and bone).
    std::vector<uint16_t> tokens;
    if (ds) tokens.push_back(ds->id);
    s.loot(29631, 30911, 395);
    std::printf("    carried: blood %d, bone %d\n", s.count_carried(395, 0) + s.count_carried(395, 2) + s.count_carried(395, 4),
                s.count_carried(395, 6) + s.count_carried(395, 7));
    for (int i = 0; i < 6; ++i) {
        unsigned blood = s.carried(395, 0) ? 0 : s.carried(395, 4) ? 4 : 2;
        unsigned bone = s.carried(395, 6) ? 6 : 7;
        if (Item* t = s.make_spell(bag_id, {{395, blood}, {395, bone}})) tokens.push_back(t->id);
    }
    std::printf("    %zu Death Speak tokens\n", tokens.size());
    // The six Ancient Necromancers (NEC1 by quality): Death Speak beside each one.
    for (unsigned q = 0; q < 6; ++q) {
        Item* nec = s.nearest(623, 0, 0, -1, int(q + 1));  // quality n teaches lesson n (bugfixes.md 2.2)
        if (!nec) { std::printf("    no necromancer %u\n", q); continue; }
        uint16_t nid = nec->id;
        s.warp(nec->x + 64, nec->y, nec->z, 62);
        Item* tok = nullptr;
        for (uint16_t t : tokens) if (s.w.item(t) && s.w.item(t)->shape == 746 && s.w.item(t)->frame == 0) { tok = s.w.item(t); break; }
        if (!tok) { std::printf("    no Death Speak token left\n"); break; }
        if (s.av().mana < 10) { std::printf("    (mana %d)\n", s.av().mana); }
        s.target = nid;
        s.w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") != nullptr;
        s.w.call_event(tok->id, kEvUse);
        for (int f = 0; f < 4000; ++f) {
            s.default_answer();
            s.step();
            if (f > 60 && !s.w.stasis && !s.w.in_conversation() && !s.w.ask_menu && s.g(201 + q)) break;
        }
        s.target = 0;
        s.w.vm().trace_intrinsics = false;
        s.settle();
        std::printf("    necromancer %u: G[%u] %u, token kept %d, mana %d\n", q + 1, 201 + q, s.g(201 + q), s.w.item(tok->id) != nullptr, s.av().mana);
        expect(s, "necromancer_" + std::to_string(q + 1), "Death Speak beside the remains");
    }
    // Reagents for later spells lie all over the Necromancers' caves: gather them.
    {
        std::vector<uint16_t> ids;
        for (Item* it : s.items(395))
            if (it->frame != 13 && it->frame < 16 && (!it->parent || (s.w.root_container(*it)->id >= 256 && !s.w.root_container(*it)->as_actor())))
                ids.push_back(it->id);  // lying about, or in bags and urns (opened, as a player does)
        int n = 0;
        for (uint16_t id : ids) if (s.w.item(id) && s.take(s.w.item(id))) ++n;
        std::printf("    gathered %d reagents on map 62: blood %d bone %d wood %d dirt %d hood %d blackmoor %d\n", n,
                    s.count_carried(395, 0) + s.count_carried(395, 1) + s.count_carried(395, 2) + s.count_carried(395, 4) + s.count_carried(395, 5),
                    s.count_carried(395, 6) + s.count_carried(395, 7), s.count_carried(395, 8), s.count_carried(395, 9),
                    s.count_carried(395, 10) + s.count_carried(395, 11), s.count_carried(395, 14) + s.count_carried(395, 15));
    }
    s.warp(21470, 11194 + 96, 0, 50);
    s.walk_to(21470, 11194, 300);
    s.run(30);
    expect(s, "catacombs_waypoint", "walked onto the Upper Catacombs waypoint");
    if (const char* e = std::getenv("PG_NECRO")) {  // experiment: a bag of these reagent frames
        uint16_t bag = s.give(637, 0);
        std::string str = e;
        std::stringstream ss(str);
        std::string tok;
        while (std::getline(ss, tok, ',')) s.give(395, unsigned(std::atoi(tok.c_str())), 1, bag);
        Item* key = s.carried(232, 0);
        std::printf("    key %u\n", key ? key->id : 0);
        s.use_on(key->id, bag, 200);
        for (uint16_t k : s.w.item(bag)->contents) std::printf("    bag: shape %u frame %u q %u\n", s.w.item(k)->shape, s.w.item(k)->frame, s.w.item(k)->quality);
        for (Item* t : s.items(746)) std::printf("    token frame %u q %u parent %u root %u\n", t->frame, t->quality, t->parent, s.w.root_container(*t)->id);
    }
    return true;
}

// Chapter 5: Stone Cove, a golem, the Hall of the Mountain King, Lithos.
bool ch_mountain(Story& s) {
    s.warp(22623, 6335 + 96, 48, 50);
    s.walk_into(22623, 6335, 49, 400);
    std::printf("    map %u after the 50 -> 49 egg\n", s.map());
    s.warp(17599, 24927 + 96, 48, 49);
    s.walk_into(17599, 24927, 21, 400);
    expect(s, "to_stone_cove", "Upper Catacombs 2 to Stone Cove");
    uint16_t bag = s.carried(637) ? s.carried(637)->id : 0;
    auto blood = [&] { for (unsigned f = 0; f <= 5; ++f) if (s.carried(395, int(f))) return f; return 0u; };
    auto bone = [&] { return s.carried(395, 6) ? 6u : 7u; };
    auto moor = [&] { return s.carried(395, 14) ? 14u : 15u; };
    Item* golem = s.make_spell(bag, {{395, blood()}, {395, bone()}, {395, 8}, {395, 9}, {395, moor()}});
    expect(s, "make_golem", "blood, bone, wood, dirt and blackmoor in the bag");
    Item* mud = s.nearest(4, 4383, 2271);  // mud: shape 3, 4, 9 or 470 (leveling_facts.md 2)
    s.warp(4383, 2400, 8, 21);
    std::set<uint16_t> before;
    for (unsigned i = 1; i < s.w.objects().slots(); ++i) if (s.w.actor(uint16_t(i))) before.insert(uint16_t(i));
    // The golem asks for an order at once: "Open", then the door (the target cursor).
    uint16_t doors[2] = {s.nearest(342, 4351, 2047)->id, s.nearest(342, 4479, 2047)->id};
    auto pick_open = [](const std::vector<std::string>& m) {
        for (size_t i = 0; i < m.size(); ++i) if (m[i].find("Open") == 0) return int(i);
        return -1;
    };
    s.picker = pick_open;
    s.target = doors[0];
    if (golem && mud) s.use_on(golem->id, mud->id, 300);
    s.target = doors[0];
    s.run(600);
    uint16_t gid = 0;
    for (unsigned i = 1; i < s.w.objects().slots(); ++i)
        if (s.w.actor(uint16_t(i)) && !before.count(uint16_t(i))) gid = uint16_t(i);
    std::printf("    golem actor %u, door 1 quality %04x\n", gid, s.w.item(doors[0])->quality);
    if (gid) {
        s.target = doors[1];
        s.w.call_event(gid, kEvUse);
        s.run(900);
    }
    s.picker = nullptr;
    s.target = 0;
    for (uint16_t d : doors) std::printf("    door %u quality %04x frame %u\n", d, s.w.item(d)->quality, s.w.item(d)->frame);
    for (uint16_t d : doors) if (!(s.w.item(d)->quality & 0x100)) s.use(d, 60);  // open them
    for (uint16_t d : doors) std::printf("    door %u quality %04x frame %u\n", d, s.w.item(d)->quality, s.w.item(d)->frame);
    for (int y0 : {2200, 2120, 2000})  // through the open double doors (y 2047) to the teleporter
        if (s.map() == 21) { s.warp(4415, y0, 0, 21); s.walk_into(4383, 1887, 31, 400); }
    expect(s, "hall_mountain_king", "the golem opened the doors, walked into the Hall");
    // The Hall's crossings (vanishing bridge, floating platforms, the force-field maze, the leap)
    // are jumping puzzles: warped to the end, where the LITHHAT egg (1768,1776,0) starts Lithos.
    shortcut("Hall of the Mountain King: warped past the vanishing bridge, floating platforms, force-field maze and the leap to Lithos' egg");
    s.warp(1768 + 200, 1776 + 200, 0, 31);
    s.walk_to(1768 + 40, 1776 + 40, 600);
    for (int i = 0; i < 600 && !s.w.stasis && !s.w.ask_menu; ++i) s.step();
    s.talk(433, {}, [&] { return s.g(42) == 1; }, {}, 30, false);
    s.settle(6000);
    if (!s.g(42)) {  // he may need a use
        Item* l = s.nearest(433, 1695, 1695);
        if (l) s.talk(l->id, {}, [&] { return s.g(42) == 1; }, {}, 30, true);
    }
    expect(s, "meet_lithos", "Lithos spoke (LITHHAT egg)");
    s.warp(2847, 8991 + 96, 8, 31);
    s.walk_to(2847, 8991, 300);
    s.run(30);
    expect(s, "hall_waypoint", "walked onto the Hall's waypoint");
    return true;
}

// Chapter 6: the Mordea plot: Devon in the dungeon, Bentic's journal, the trial.
bool ch_mordea(Story& s) {
    Actor* dv = s.w.actor(2);
    std::printf("    Devon on map %u at %d,%d,%d; G[88] %u G[93] %u\n", dv->mapnum, dv->x, dv->y, dv->z, s.g(88), s.g(93));
    s.warp(dv->x + 200, dv->y, dv->z, dv->mapnum);
    s.run(30);
    dv = s.w.actor(2);
    std::printf("    Devon now at %d,%d,%d\n", dv->x, dv->y, dv->z);
    for (int t = 0; t < 3 && !s.g(90); ++t) s.talk(2, {"I will help", "I will assist", "help", "Mordea", "Bentic"}, [&] { return s.g(90) == 1; }, {}, 40);
    expect(s, "devon_in_prison", "talked to Devon in the dungeon");
    shortcut("palace dungeon: warped beside Devon's cell (the metal gate's switch was not used)");
    // Bentic's journal behind magic bars (BOOKEGG creates it at (3839,4703,16) once G[93:1]):
    // the Scroll of Dispelling Magic Portals, then read it.
    s.warp(3839 + 160, 4703, 0, 40);
    s.run(60);
    Item* book = s.nearest(607, 3839, 4703);
    std::printf("    journal %u%s\n", book ? book->id : 0, book ? (" at " + std::to_string(book->x) + "," + std::to_string(book->y) + "," + std::to_string(book->z)).c_str() : "");
    // The bars (shape 732 at (3615,4895,0)) and their two posts (733) are fixed-map items between
    // the corridor and the journal's room (map_index.md 4.4): the walk in is blocked first.
    auto bars = [&] { int n = 0; for (Item* b : s.items(732)) n += b->chunk >= 0 && std::abs(b->x - 3615) < 8 && std::abs(b->y - 4895) < 8; return n; };
    std::printf("    magic bars before: %d\n", bars());
    CHECK_EQ(bars(), 1);
    s.warp(3615 - 200, 4831, 0, 40);
    s.walk_to(3615 + 120, 4831, 300);
    CHECK(s.av().x < 3615);  // the bars stop the Avatar
    Item* scroll = s.carried(827, -1, 1);
    s.said.clear();
    s.w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") != nullptr;
    if (scroll) s.use_event(scroll->id, 300);
    s.settle(3000);
    s.w.vm().trace_intrinsics = false;
    std::printf("    scroll used: still carried %d, magic bars left %d\n", s.carried(827, -1, 1) != nullptr, bars());
    CHECK_EQ(bars(), 0);
    CHECK(s.carried(827, -1, 1) != nullptr);  // not used up (as in the original)
    {
        int dark = 0;
        for (Item* p : s.items(733)) dark += std::abs(p->x - 3615) < 8 && (p->frame == 9 || p->frame == 19);
        std::printf("    posts gone dark: %d\n", dark);
        CHECK_EQ(dark, 2);
    }
    if (book) {
        uint16_t bid = book->id;
        // Through the opened portal (y 4767-4895) into the room, then read the journal.
        s.walk_to(3615 + 120, 4831, 400);
        std::printf("    walked through the portal: at %d,%d,%d\n", s.av().x, s.av().y, s.av().z);
        CHECK(s.av().x > 3615);
        s.walk_to(3839 + 64, 4703 + 40, 400);
        s.use(bid, 30);
        if (!s.g(97)) s.use_event(bid, 30);
    }
    s.settle(9000);
    expect(s, "read_journal", "dispelled the bars, read the journal");
    s.settle(9000);
    expect(s, "trial", "the trial at the docks: 'Devon is the rightful heir', 'I have proof'");
    return true;
}

// Chapter 7: the Key of the Scion, Lothian's interment, the Shrine of the Ancient Ones.
bool ch_scion(Story& s) {
    s.warp(7423, 11300, 48, 5);
    s.walk_into(7423, 10943, 6, 900);
    for (int t = 0; t < 3 && !s.g(208); ++t)
        s.talk(21, {"Yes, he did", "Mountain King", "Lithos", "inter", "Yes"}, [&] { return s.g(208) == 1; }, {}, 40);
    expect(s, "report_lithos", "told Vividos of the Mountain King");
    expect(s, "key_scion", "the Key of the Scion");
    // The CEMHAT egg (usecode egg 27 at (7935,28223,8), by the cemetery's north plot) takes
    // Lothian's body from the tomb and lays it on the grave in the skull shrine (INTERN, shape 628
    // frame 0 at (9471,24063,8)) when the Avatar comes by. INTERN's use then needs, within 320
    // of the Avatar: the grave, no lit tall candle and at least three skulls (shape 331) - so the
    // Avatar stands among the four skull pedestals and uses the grave.
    s.warp(7935, 28223 + 400, 8, 6);
    s.walk_to(7935, 28223, 600);
    s.run(200);
    s.settle(6000);
    Item* grave = s.nearest(628, 9471, 24063, 0);
    std::printf("    CEMHAT: Lothian's body %s, the grave %s\n", s.items(754).empty() ? "taken" : "still in the tomb", grave ? "laid out" : "missing");
    if (grave) {
        uint16_t gid = grave->id;
        s.warp(9400, 24000, 8, 6);
        s.use(gid, 60);
        s.settle(9000);
    }
    expect(s, "inter_lothian", "walked past CEMHAT, used Lothian's grave among the skull pedestals");

    // The pilgrimage: the 'Towards Fate Do You Travel' building in Upper Catacombs 1, opened with
    // the Key of the Scion, then its teleporter to the Shrine of the Ancient Ones (map 54).
    s.warp(11168, 11264 + 300, 48, 50);
    for (unsigned sh : {68u, 69u, 340u, 342u})
        for (Item* d : s.items(sh))
            if (std::abs(d->x - 11168) < 600 && std::abs(d->y - 11264) < 600)
            {
                uint16_t did = d->id;
                if (d->quality & 0x400) s.use_on(s.carried(232, 3)->id, did, 90);  // locked: the Key of the Scion
                if (!(s.w.item(did)->quality & 0x100)) s.use(did, 60);
                std::printf("    door %u shape %u q %04x at %d,%d,%d\n", did, d->shape, s.w.item(did)->quality, d->x, d->y, d->z);
            }
    s.walk_to(11327, 11423 + 60, 300);
    s.walk_to(11327, 11423 - 100, 300);
    s.walk_into(11168, 11264, 54, 600);
    expect(s, "to_shrine", "the Shrine of the Ancient Ones");
    shortcut("Shrine of the Ancient Ones: warped to the Zealan shield past the lever cage (Aerial Servant on the chest), the hidden plate gate and the Open Ground pit");
    s.take(s.nearest(828, 12319, 10367));
    expect(s, "zealan_shield", "picked up the Zealan shield");
    // The Zealan altar: the shield laid on it (dropped on its top), the Avatar among the statues.
    shortcut("Shrine of the Ancient Ones: warped past the rolling-sphere and staircase puzzles and the beams to the Zealan Shrine");
    Item* altar = s.nearest(550, 12128, 2880);
    Item* shield = s.carried(828);
    if (altar && shield) {
        int xd, yd, zd;
        s.w.footprint(*altar, xd, yd, zd);
        s.warp(altar->x + 64, altar->y + 64, altar->z, 54);
        s.drop(shield->id, altar->x - xd / 2, altar->y - yd / 2, altar->z + zd);
        Item* sh = s.w.item(shield->id);
        std::printf("    shield at %d,%d,%d (altar %d,%d,%d size %d,%d,%d)\n", sh->x, sh->y, sh->z, altar->x, altar->y, altar->z, xd, yd, zd);
        for (Item* e : s.items(73)) if (e->quality == 12) std::printf("    ANCTONES egg %u hatched %d\n", e->id, e->hatched);
        s.walk_to(11800, 2900, 300);  // among the statues (the ANCTONES eggs at x 11716)
        for (Item* e : s.items(73)) if (e->quality == 12) std::printf("    ANCTONES egg %u hatched %d\n", e->id, e->hatched);
        for (int f = 0; f < 3000 && !s.g(75); ++f) s.step();
        s.settle(9000);
    }
    expect(s, "ancient_ones", "laid the shield on the altar, the Ancient Ones spoke");
    // Khumash-Gor's room: the ghost, then the jewellery box on the west wall.
    s.warp(10000, 2772, 8, 54);
    s.run(60);
    for (unsigned i = 2; i < s.w.objects().slots(); ++i)
        if (Actor* a = s.w.actor(uint16_t(i)); a && std::abs(a->x - 10000) < 800 && std::abs(a->y - 2772) < 800)
            std::printf("    actor %u shape %u at %d,%d,%d '%s'%s\n", a->id, a->shape, a->x, a->y, a->z, GameLog::name_of(s.w, a->id, false).c_str(),
                        a->is(Actor::kDead) ? " dead" : "");
    for (unsigned i = 2; i < s.w.objects().slots(); ++i)
        if (Actor* a = s.w.actor(uint16_t(i)); a && a->shape == 411 && !a->is(Actor::kDead) && std::abs(a->x - 10000) < 800 && std::abs(a->y - 2772) < 800) {
            uint16_t bag = s.carried(637)->id;
            Item* gp = s.make_spell(bag, {s.R("hood"), s.R("blackmoor")});
            std::printf("    Grant Peace token %d, mana %d\n", gp != nullptr, s.av().mana);
            if (gp) s.use_on(gp->id, a->id, 300);
            std::printf("    ghost %u after Grant Peace: %s\n", a->id, !s.w.actor(a->id) ? "gone" : s.w.actor(a->id)->is(Actor::kDead) ? "dead" : "still there");
        }
    s.loot(9804, 2772, 442);
    expect(s, "obelisk_tip", "took the obelisk tip from the jewellery box");
    return true;
}

// Chapter 8: Argentrock Isle, the theurgists' tests, the foci, Torwin, Stratos.
bool ch_theurgy(Story& s) {
    s.warp(19406, 16162 + 200, 48, 49);
    for (unsigned sh : {68u, 69u, 340u, 342u})
        for (Item* d : s.items(sh))
            if (std::abs(d->x - 19406) < 500 && std::abs(d->y - 16162) < 500) {
                uint16_t did = d->id;
                if (d->quality & 0x400) s.use_on(s.carried(232, 3)->id, did, 90);
                if (!(s.w.item(did)->quality & 0x100)) s.use(did, 60);
                std::printf("    door %u shape %u q %04x at %d,%d,%d\n", did, d->shape, s.w.item(did)->quality, d->x, d->y, d->z);
            }
    s.walk_into(19406, 16162, 37, 600);
    expect(s, "to_argentrock", "the door in the spiked wall (Key of the Scion), Argentrock Isle");
    s.warp(25055, 19903 + 96, 80, 37);
    s.walk_to(25055, 19903, 300);
    s.run(30);
    expect(s, "argentrock_waypoint", "walked onto the waypoint");
    // The Test of Wisdom: five of ten questions drawn at random. The answers that score (read from
    // XAVIER's answer checks in the data, as a player learns them in the monastery's library).
    std::vector<std::string> wise = {"a breezy evening on the porch", "tend the injured", "my sight", "the battle is lost", "remain quiet",
                                     "welcome him home", "honesty", "a weapon destroys, wit builds", "give truthful testimony", "comfort a sad child",
                                     "May I try the Test of Wisdom again",
                                     "I wish enlightenment", "Tell me of the tests", "Test of Wisdom", "aye", "I am ready", "Yes, I accept"};
    for (int t = 0; t < 3 && !s.g(118); ++t) s.talk(25, wise, [&] { return s.g(118) == 1; }, {"healing", "not yet", "Another"}, 40);
    expect(s, "test_wisdom", "Xavier's Test of Wisdom");
    // The Test of Centeredness: the circle on the second level of the cliffs (AIRTEST1 egg; it
    // counts time with the Avatar above z 166).
    s.talk(25, {"I will take the Test of Centeredness", "May I take the Test of Centeredness", "Tell me of the Test of Centeredness", "Yes", "aye"},
           [&] { return false; }, {"healing", "not yet", "Another", "not ready", "Wisdom again"}, 12);
    shortcut("Argentrock: warped up the cliffs to the Test of Centeredness circle (the climb)");
    s.warp(14750 + 40, 18898 + 40, 168, 37);
    // Hold the circle against the wind: walk back into it whenever it pushes him off.
    int pushed = 0, minz = 999;
    for (int f = 0; f < 12000 && !s.g(122); ++f) {
        int fx = s.av().x - 32, fy = s.av().y - 32;
        int dx = 14750 - fx, dy = 18898 - fy;
        PlayerInput& in = s.w.input;
        if (std::abs(dx) + std::abs(dy) > 24 && !s.w.stasis) {
            in.cursor_dir = direction_to(dx, dy);
            in.cursor_len = 1;
            if (!in.button[2].down) { in.button[2].press(in.now_ms, 0, 0); ++pushed; }
        } else if (in.button[2].down) {
            s.release();
        }
        minz = std::min(minz, int(s.av().z));
        if (s.av().z < 160 && !s.w.stasis) {  // blown off the cliff: climb back up and try again
            static bool noted = false;
            if (!noted) shortcut("Argentrock: blown off the Centeredness circle by the wind: warped back up (the climb)");
            noted = true;
            s.release();
            s.warp(14750 + 40, 18898 + 40, 168, 37);
        }
        if (std::getenv("PG_WIND") && f % 15 == 0) std::printf("      wind f%d at %d,%d,%d stasis %d anim %u\n", f, s.av().x, s.av().y, s.av().z, int(s.w.stasis), s.av().last_anim);
        s.default_answer();
        s.step();
    }
    s.release();
    std::printf("    in the wind: walked back %d times, lowest z %d, G[122] %u\n", pushed, minz, s.g(122));
    s.settle(6000);
    expect(s, "test_centeredness", "stood in the circle against the wind");
    for (int t = 0; t < 3 && !s.g(453); ++t)
        s.talk(23, {"passed", "both tests", "Test of Centeredness", "foci", "Focus", "silver", "key", "Yes"}, [&] { return s.g(453) == 1; }, {}, 40);
    expect(s, "silver_key", "Stellos gave the silver mine key");
    // The silver mine: its door (lock quality 8) opened with the key.
    for (unsigned sh : {68u, 69u, 340u, 342u})
        for (Item* d : s.items(sh))
            if ((d->quality & 0xFF) == 8 && (d->quality & 0x400)) {
                uint16_t did = d->id;
                s.warp(d->x + 48, d->y + 48, d->z, 37);
                if (Item* key = s.carried(82, -1, 8)) s.use_on(key->id, did, 90);
                s.use(did, 60);
                std::printf("    mine door %u at %d,%d,%d: quality %04x\n", did, d->x, d->y, d->z, s.w.item(did)->quality);
            }
    // Eight of the eleven ores.
    std::vector<uint16_t> ores;
    for (Item* it : s.items(586)) if (!it->parent) ores.push_back(it->id);
    std::printf("    %zu silver ores on the map\n", ores.size());
    for (uint16_t id : ores) if (s.count_carried(586) < 8 && s.w.item(id)) s.take(s.w.item(id));
    expect(s, "silver_ore", "picked up eight silver ores");
    // Korick the smith in West Tenebrae makes the foci.
    Actor* k = s.w.actor(9);
    std::printf("    Korick on map %u at %d,%d,%d\n", k->mapnum, k->x, k->y, k->z);
    s.warp(k->x + 64, k->y, k->z, k->mapnum);
    for (int t = 0; t < 4 && s.count_carried(586) > 0; ++t)
        s.talk(9, {"foci", "Focus", "focus", "silver", "make", "Yes", "yes"}, [&] { return s.count_carried(586) == 0; }, {"buy"}, 60);
    for (Item* f : s.items(396)) if (f->parent && s.w.root_container(*f)->id == 1) std::printf("    focus frame %u\n", f->frame);
    expect(s, "korick_foci", "Korick forged the foci");
    // The Altar of Focus in the monastery: each focus dropped on it (gotHit) is enchanted.
    s.recall("Argentrock");
    Item* altar = s.nearest(206, 18438, 18474);
    if (altar) {
        int xd, yd, zd;
        s.w.footprint(*altar, xd, yd, zd);
        s.warp(altar->x + 64, altar->y + 64, altar->z, 37);
        std::vector<uint16_t> foci;
        for (Item* f : s.items(396)) if (f->parent && s.w.root_container(*f)->id == 1 && (f->frame < 8 || f->frame == 16)) foci.push_back(f->id);
        for (uint16_t f : foci) {
            s.w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") != nullptr;
            s.drop(f, altar->x - xd / 2, altar->y - yd / 2, altar->z + zd + 8);
            s.run(30);
            for (int i = 0; i < 3000 && s.w.kernel().count(altar->id, kProcAny); ++i) s.step();  // the altar's scene
            s.w.vm().trace_intrinsics = false;
            Item* it = s.w.item(f);
            std::printf("    focus %u now frame %u at %d,%d,%d\n", f, it ? it->frame : 99, it ? it->x : 0, it ? it->y : 0, it ? it->z : 0);
            if (it && !it->parent) s.take(it);
        }
    }
    expect(s, "enchant_foci", "dropped each focus on the Altar of Focus");
    auto focus = [&](unsigned frame) { return s.carried(396, int(frame)); };
    for (int t = 0; t < 3 && !s.g(454); ++t)
        s.talk(23, {"constructed my Foci", "Foci", "test", "Yes"}, [&] { return s.g(454) == 1; }, {}, 30);
    expect(s, "report_foci", "told Stellos of the foci");
    s.talk(23, {"third test", "Third Test", "Test of Skills", "test", "What must I do", "Yes"}, {}, {}, 12);
    // The Test of Skills: the injured torax (shape 209, made by Stellos) healed with Healing Touch.
    for (Item* t : s.items(209)) std::printf("    torax %u frame %u at %d,%d,%d\n", t->id, t->frame, t->x, t->y, t->z);
    if (Item* tor = s.nearest(209, s.av().x, s.av().y)) {
        uint16_t tid = tor->id;
        s.warp(tor->x + 96, tor->y, tor->z, 37);
        Item* ht = focus(9);
        if (!ht) ht = focus(11);
        std::printf("    healing focus %u, mana %d\n", ht ? ht->frame : 0, s.av().mana);
        s.w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") != nullptr;
        if (ht) s.use_on(ht->id, tid, 300);
        for (int i = 0; i < 3000 && !s.g(128); ++i) s.step();
        s.w.vm().trace_intrinsics = false;
    }
    expect(s, "test_skills", "healed the torax");
    std::vector<std::string> xav_avoid = {"healing", "not yet", "Another", "not ready", "Wisdom again", "Centeredness"};
    s.talk(25, {"passed the Third Test", "What has happened", "Focus is missing", "stolen", "Do you know who took it", "When did you last see"}, {}, xav_avoid, 14);
    // Torwin, met at the monastery; then Stellos on his research (G[113:1], which opens Cyrrus'
    // "Have you seen Torwin?").
    s.talk(26, {"What do you do here", "You are a healer", "What superior abilities", "Who will recognize"}, {}, {}, 8);
    s.talk(25, {"I have met Torwin", "Best student"}, [&] { return s.g(114) == 1; }, xav_avoid, 12);
    s.talk(23, {"What research is Torwin doing", "I have met Torwin", "One of your finest", "Torwin"}, [&] { return s.g(113) == 1; }, {}, 20);
    std::printf("    G[113] %u G[114] %u\n", s.g(113), s.g(114));
    // Holy Cyrrus answers truthfully under Hear Truth (focus frame 14).
    if (Item* ht = s.carried(396, 14)) {
        Actor* c = s.w.actor(22);
        s.warp(c->x + 64, c->y, c->z, c->mapnum);
        for (int t = 0; t < 3 && !s.g(116); ++t) {
            s.target = 22;
            s.w.call_event(ht->id, kEvUse);
            for (int i = 0; i < 300 && !s.g(37); ++i) s.step();
            s.target = 0;
            std::printf("    Hear Truth: G[37] %u, mana %d\n", s.g(37), s.av().mana);
            s.talk(22, {"Where is he", "Have you seen Torwin"}, [&] { return s.g(116) == 1; }, {}, 6);
        }
    }
    expect(s, "ask_cyrrus", "Hear Truth, asked Holy Cyrrus where Torwin went");
    Actor* tw = s.w.actor(26);
    std::printf("    Torwin on map %u at %d,%d,%d\n", tw->mapnum, tw->x, tw->y, tw->z);
    shortcut("Argentrock: warped up Windy Point to Torwin (the climb)");
    s.warp(tw->x + 96, tw->y, tw->z, tw->mapnum);
    s.run(120);
    s.settle(6000);
    for (int t = 0; t < 3 && !s.g(440); ++t) s.talk(26, {"Xavier", "focus", "Focus", "Torwin", "Rhian", "give"}, [&] { return s.g(440) == 1; }, {}, 30);
    s.settle(9000);
    expect(s, "confront_torwin", "confronted Torwin at Windy Point");
    if (Item* f = s.nearest(396, s.av().x, s.av().y, 9)) if (!f->parent || s.w.root_container(*f)->id != 1) s.take(f);
    std::printf("    the Focus of healing carried: %d\n", s.carried(396, 9) != nullptr);
    for (int t = 0; t < 3 && !s.g(111); ++t)
        s.talk(25, {"I have found your Focus of healing", "Focus"}, [&] { return s.g(111) == 1; }, xav_avoid, 20);
    expect(s, "return_focus", "gave Xavier his Focus of healing");
    for (int t = 0; t < 3 && !s.g(137) && !s.g(131); ++t)
        s.talk(23, {"What has happened", "Adept", "Final test", "Torwin", "Poor Torwin"}, [&] { return s.g(131) == 1; }, {}, 30);
    expect(s, "tell_stellos", "told Stellos of Torwin");
    // The Test of Belief: the floating platforms west of Windy Point to Stratos (STRATHAT egg).
    shortcut("Argentrock: warped over the floating platforms of the Test of Belief to Stratos (jumps)");
    s.warp(10655 + 64, 17247 + 64, 112, 37);
    s.walk_to(10655, 17247, 300);
    for (int i = 0; i < 900 && !s.g(56); ++i) { s.default_answer(); s.step(); }
    s.settle(9000);
    for (int i = 0; i < 900 && !s.g(56); ++i) { s.default_answer(); s.step(); }
    // The Breath of Air (shape 442 frame 3, invisible) on Stratos' platform: Reveal, then the
    // Aerial Servant fetches it. G[56:1] is set by the STRATHAT egg (id 1, at (11068,17172,112))
    // when the Avatar passes it carrying the Breath (read from the egg's code; the theurgists'
    // spells fail from then on), so the Test of Belief completes here.
    Item* breath = s.nearest(442, 10474, 16686, 3);
    if (breath) {
        uint16_t bid = breath->id;
        std::printf("    the Breath of Air %u at %d,%d,%d flags %04x; avatar at %d,%d,%d\n", bid, breath->x, breath->y, breath->z, breath->flags,
                    s.av().x, s.av().y, s.av().z);
        // It lies on a pedestal on a small floating rock no one can stand on (Stratos' platform is
        // beside it): Reveal from where the Avatar stands, then the Aerial Servant fetches it,
        // two target clicks as a player makes them (magic.md 5): the orb, then the Avatar.
        if (Item* rv = s.carried(396, 17)) { s.use_on(rv->id, 0, 400); }
        std::printf("    after Reveal: flags %04x, mana %d; avatar at %d,%d,%d\n", s.w.item(bid)->flags, s.av().mana, s.av().x, s.av().y, s.av().z);
        s.w.vm().trace_intrinsics = std::getenv("PG_AS") != nullptr;
        if (Item* as = s.carried(396, 10)) {
            s.targets = {bid, 1};
            s.cursors = 0;
            s.said.clear();
            s.w.call_event(as->id, kEvUse);
            bool seen = false;
            auto casting = [&] {  // the spell's process (AIRSPEL 1430:1093) still running
                bool on = false;
                s.w.kernel().for_each([&](Process* p) { if (auto* u = dynamic_cast<UCProcess*>(p); u && u->start_cls == 1430 && u->start_addr == 0x1093 && !p->terminated()) on = true; });
                return on;
            };
            for (int i = 0; i < 2000 && (i < 10 || casting() || !s.items(481).empty()); ++i) {
                s.default_answer();
                s.step();
                if (std::getenv("PG_AS") && i % 25 == 0) {
                    std::printf("    f%d avatar frame %u anim %u at %d,%d,%d procs:", i, s.av().frame, s.av().last_anim, s.av().x, s.av().y, s.av().z);
                    s.w.kernel().for_each([&](Process* p) { if (auto* an = dynamic_cast<ActorAnimProcess*>(p)) std::printf(" [%u a%u p%u]", p->pid(), an->action(), an->frames_played()); });
                    std::printf("\n");
                }
                for (Item* sv : s.items(481)) {
                    if (!seen) std::printf("    f%d the servant %u appears at %d,%d,%d\n", i, sv->id, sv->x, sv->y, sv->z);
                    seen = true;
                    if (std::getenv("PG_AS") && i % 10 == 0) std::printf("    f%d servant %u at %d,%d,%d frame %u\n", i, sv->id, sv->x, sv->y, sv->z, sv->frame);
                }
            }
            s.targets.clear();
            if (std::getenv("PG_PROCS")) s.w.kernel().for_each([&](Process* p) {
                auto* u = dynamic_cast<UCProcess*>(p);
                std::printf("      pid %u %s item %u type %u%s%s", p->pid(), p->name(), p->item, p->type, p->suspended() ? " suspended" : "",
                            p->terminated() ? " terminated" : "");
                if (u) std::printf(" uc %u:%04x ip %04x cls %u", u->start_cls, u->start_addr, u->ip, u->cls);
                if (auto* an = dynamic_cast<ActorAnimProcess*>(p)) std::printf(" action %u played %u", an->action(), an->frames_played());
                std::printf("\n");
            });
            std::printf("      avatar frame %u last_anim %u\n", s.av().frame, s.av().last_anim);
            CHECK(seen);
            CHECK_EQ(s.cursors, 2);
            CHECK(s.items(481).empty());  // gone at the end
        }
        s.w.vm().trace_intrinsics = false;
        Item* b2 = s.w.item(bid);
        std::printf("    after Aerial Servant: at %d,%d,%d parent %u (backpack %u), mana %d, said:", b2->x, b2->y, b2->z, b2->parent, s.pack(), s.av().mana);
        for (auto& l : s.said) std::printf(" '%s'", l.c_str());
        std::printf("\n");
        CHECK(s.carried(442, 3) != nullptr);
        CHECK_EQ(unsigned(b2->parent), unsigned(s.pack()));
    }
    expect(s, "breath_of_air", "Reveal and the Aerial Servant on Stratos' orb");
    s.warp(11068 + 32, 17172 + 32, 112, 37);  // onto the platform of the egg (the way back is jumps)
    for (int i = 0; i < 900 && !s.g(56); ++i) { s.default_answer(); s.step(); }
    s.settle(9000);
    std::printf("    G[56] %u G[140] %u; avatar at %d,%d,%d chunk %d parent %u flags %04x stasis %d map %u\n", s.g(56), s.g(140), s.av().x, s.av().y, s.av().z, s.av().chunk, s.av().parent, s.av().flags, s.w.stasis, s.map());
    expect(s, "test_belief", "reached Stratos");
    for (Item* f : s.items(396)) if (f->parent && s.w.root_container(*f)->id == 1 && f->frame == 13) std::printf("    the Air Walk focus is carried\n");
    return true;
}

// Opens every closed door within r of (x, y) on this map (a locked one with the Key of the Scion).
void open_doors(Story& s, int x, int y, int r) {
    for (unsigned sh : {68u, 69u, 340u, 342u})
        for (Item* d : s.items(sh))
            if (std::abs(d->x - x) < r && std::abs(d->y - y) < r && !(d->quality & 0x100)) {
                uint16_t did = d->id;
                s.warp(d->x + 48, d->y + 48, d->z, s.map());
                if ((d->quality & 0x400) && s.carried(232, 3)) s.use_on(s.carried(232, 3)->id, did, 90);
                s.use(did, 60);
                std::printf("    door %u shape %u at %d,%d,%d: quality %04x\n", did, d->shape, d->x, d->y, d->z, s.w.item(did)->quality);
            }
}

// Chapter 9: Carthax Lake, the water freed, Hydros.
bool ch_hydros(Story& s) {
    s.recall("Upper Catacombs");
    s.warp(22623, 6335 + 96, 48, 50);
    s.walk_into(22623, 6335, 49, 400);
    open_doors(s, 22744, 22944, 500);
    s.warp(22744, 22944 + 200, 48, 49);
    s.walk_into(22744, 22944, 16, 600);
    expect(s, "to_carthax", "the door south of the hidden switch, the CARTHAXH egg");
    s.warp(19808 + 64, 9472, 119, s.map());
    s.walk_to(19808, 9472, 300);
    s.run(30);
    expect(s, "carthax_waypoint", "walked onto the waypoint");
    // The dry lake bed's cave (map 28) and the gravestone at the source: Open Ground.
    s.warp(17764 + 64, 18124 + 64, 104, s.map());
    s.walk_into(17764, 18124, 28, 400);
    std::printf("    map %u\n", s.map());
    uint16_t bag = s.carried(637)->id;
    Item* og = s.make_spell(bag, {s.R("blood"), s.R("blackmoor")});
    Item* grave = s.nearest(350, 10943, 4639);
    shortcut("Carthax: warped through the cave past the trolls, the two switches and the automatic gate to the gravestone");
    if (grave && og) {
        s.warp(grave->x + 160, grave->y, grave->z, 28);
        s.use_on(og->id, grave->id, 300);
        s.settle(9000);
    }
    expect(s, "free_water", "Open Ground at the gravestone");
    // Back to the lake, now full (map 26), round to the central pedestal: Hydros (HYDHAT egg).
    s.settle(9000);
    std::printf("    G[52] %u, map %u\n", s.g(52, 2), s.map());
    if (s.map() == 28) {
        s.warp(15182 - 64, 8146, 104, 28);
        s.walk_into(15182, 8146, 26, 400);
    }
    std::printf("    map %u\n", s.map());
    shortcut("Carthax Lake: warped round the west edge and over the stone bridge to the white steps");
    s.warp(20031 + 64, 14399 + 64, 136, s.map());
    s.walk_to(20031, 14399, 300);
    for (int i = 0; i < 900 && !s.w.stasis && !s.w.ask_menu; ++i) s.step();
    for (int t = 0; t < 3 && s.g(52, 2) != 2; ++t) {
        s.talk(0, {"free", "Free", "release", "Yes", "Hydros"}, [&] { return s.g(52, 2) == 2; }, {}, 30, false);
        s.settle(6000);
        if (s.g(52, 2) != 2) for (Item* h : s.items(80)) { s.talk(h->id, {"free", "Free", "release", "Yes"}, [&] { return s.g(52, 2) == 2; }, {}, 30); break; }
    }
    expect(s, "free_hydros", "spoke with Hydros");
    return true;
}

// --- Sorcery at a pentagram (magic.md 3.1-3.3)
// The five points from the pentagram's reference point (px, py): Aphelion, Mesostel Pa,
// Perivolcan Pa, Perivolcan Ze, Mesostel Ze.
enum { kAph, kMPa, kPPa, kPZe, kMZe };
const int kPointDX[5] = {70, 41, 167, 266, 201}, kPointDY[5] = {70, 201, 266, 167, 41};
// Reagent kinds (shape 398) by their first frame: ash 0, pumice 2, obsidian 6, iron 10,
// brimstone 14, daemon bone 18.
enum { kAsh = 0, kPumice = 2, kObsidian = 6, kIron = 10, kBrimstone = 14, kBone = 18 };
int reagent_kind(unsigned frame) { return frame <= 1 ? kAsh : frame <= 5 ? kPumice : frame <= 9 ? kObsidian : frame <= 13 ? kIron : frame <= 17 ? kBrimstone : kBone; }
struct Recipe {
    unsigned spell;
    const char* name;
    std::set<int> red;                     // points with a red candle (the others black)
    std::vector<std::pair<int, int>> reagents;  // (point, kind)
};
const Recipe kFlash{3, "Flash", {kAph, kPPa, kPZe}, {{kMPa, kAsh}, {kMZe, kAsh}, {kAph, kPumice}}};
const Recipe kFlameBolt{4, "Flame Bolt", {kAph, kMPa, kPZe}, {{kMPa, kAsh}, {kPZe, kPumice}, {kMZe, kIron}}};
const Recipe kEndureHeat{5, "Endure Heat", {kPPa, kPZe}, {{kPPa, kObsidian}, {kPZe, kObsidian}, {kAph, kIron}}};
const Recipe kExplosion{9, "Explosion", {kPPa, kMZe, kAph}, {{kMZe, kAsh}, {kPPa, kPumice}, {kMPa, kIron}, {kAph, kBrimstone}}};
const Recipe kSummonDaemon{10, "Summon Daemon", {kPPa, kPZe, kMPa, kAph}, {{kMZe, kAsh}, {kMPa, kPumice}, {kPPa, kObsidian}, {kPZe, kObsidian}, {kAph, kBone}}};
const Recipe kBanishDaemon{11, "Banish Daemon", {kPPa, kPZe, kMZe, kAph}, {{kMPa, kAsh}, {kMZe, kPumice}, {kPPa, kIron}, {kPZe, kIron}, {kAph, kBone}}};

bool g_supplies_given = false;
void supplies_given() {
    if (!g_supplies_given) shortcut("sorcery: reagents and candles the Enclave and the fortress do not have lying about were given (as if bought)");
    g_supplies_given = true;
}
int carried_reagents(Story& s, int kind) {
    int n = 0;
    for (Item* it : s.items(398)) if (it->parent && s.w.root_container(*it)->id == 1 && reagent_kind(it->frame) == kind) n += std::max<int>(1, it->quality);
    return n;
}
// Gathers reagents before a ritual, as a player picks them up: until `n` of the kind are carried,
// the nearest loose one on this map (or in a container that is not an NPC's) is taken; failing
// that, one is given (one SHORTCUT note). Not near `avoid` (a pentagram whose layout is in use).
void stock_reagents(Story& s, int kind, int n) {
    const int x0 = s.av().x, y0 = s.av().y, z0 = s.av().z;
    const unsigned m = s.map();
    std::set<uint16_t> tried;
    for (int guard = 0; guard < 40 && carried_reagents(s, kind) < n; ++guard) {
        Item* best = nullptr;
        long bd = 1L << 40;
        for (Item* it : s.items(398)) {
            if (reagent_kind(it->frame) != kind || tried.count(it->id)) continue;
            const Item* root = it->parent ? s.w.root_container(*it) : it;
            if (root->id == 1 || root->as_actor() || root->chunk < 0) continue;
            int x, y, z;
            s.w.location(*it, x, y, z);
            long d = std::labs(x - x0) + std::labs(y - y0);
            if (d < bd && d < 6000) { bd = d; best = it; }
        }
        if (best) {
            uint16_t id = best->id;
            tried.insert(id);
            s.take(best);
            if (s.w.item(id) && !(s.w.item(id)->parent && s.w.root_container(*s.w.item(id))->id == 1)) {
                s.w.give_to_avatar(id);  // (a container gump that did not open)
            }
            continue;
        }
        supplies_given();
        s.give(398, unsigned(kind));
    }
    if (s.map() == m) s.warp(x0, y0, z0, m);
}
Item* get_reagent(Story& s, int kind) {
    for (Item* it : s.items(398)) if (it->parent && s.w.root_container(*it)->id == 1 && reagent_kind(it->frame) == kind) return it;
    supplies_given();
    return s.w.item(s.give(398, unsigned(kind)));
}
int carried_candles(Story& s, bool red) {
    int n = 0;
    for (Item* c : s.items(454)) if (c->parent && s.w.root_container(*c)->id == 1 && (red ? (c->frame >= 12 && c->frame <= 15) : (c->frame >= 8 && c->frame <= 11))) ++n;
    return n;
}
// A candle (454) of a colour (red 12-15, black 8-11) the Avatar carries, else the nearest loose
// one on this map not standing on `avoid`'s points, else given.
Item* get_candle(Story& s, bool red, uint16_t avoid_pent, bool another = false) {  // another: not one already carried
    auto colour_ok = [&](const Item* c) { return red ? (c->frame >= 12 && c->frame <= 15) : (c->frame >= 8 && c->frame <= 11); };
    if (!another)
        for (Item* c : s.items(454)) if (c->parent && s.w.root_container(*c)->id == 1 && colour_ok(c)) return c;
    const Item* pent = s.w.item(avoid_pent);
    Item* best = nullptr;
    long bd = 1L << 40;
    for (Item* c : s.items(454)) {
        if (!colour_ok(c) || c->parent || c->chunk < 0 || (c->flags & 0x0001)) continue;  // (not fixed-map ones)
        if (pent && c->x <= pent->x + 32 && c->x > pent->x - 330 && c->y <= pent->y + 32 && c->y > pent->y - 330) continue;  // on its star
        long d = std::labs(c->x - s.av().x) + std::labs(c->y - s.av().y);
        if (d < bd && d < 6000) { bd = d; best = c; }
    }
    if (best) {
        int x = s.av().x, y = s.av().y, z = s.av().z;
        unsigned m = s.map();
        uint16_t id = best->id;
        s.take(best);
        if (s.w.item(id) && !s.w.item(id)->parent) s.w.give_to_avatar(id);
        s.warp(x, y, z, m);
        for (Item* c : s.items(454)) if (c->parent && s.w.root_container(*c)->id == 1 && colour_ok(c)) return c;
    }
    supplies_given();
    return s.w.item(s.give(454, red ? 12 : 8));
}
// One enchanting at pentagram `pent` (frame 4): the recipe's candles lit on the five points, its
// reagents beside them, the focus on the star, the Avatar outside it, the pentagram used. True if
// the focus took the spell (quality high byte).
bool enchant(Story& s, uint16_t pent, const Recipe& r, uint16_t focus) {
    Item* p = s.w.item(pent);
    if (!p || !s.w.item(focus)) return false;
    const int px = p->x, py = p->y, pz = p->z;
    const unsigned m = s.map();
    s.warp(px + 48, py - 149, pz, m);  // beside it (outside the star)
    // Clear the star: loose reagents and other foci on or near it are moved aside, east of the
    // pentagram (a spare daemon bone or brimstone within x <= px, y <= py would bring a daemon
    // or an explosion, step 11). The backpack has no room for all of it.
    int aside = 0;
    auto put_aside = [&](uint16_t id) {
        int k = aside++;
        s.drop(id, px + 40 + 20 * (k % 4), py - 40 - 20 * (k / 4), pz);
    };
    for (unsigned sh : {398u, 397u}) {
        std::vector<uint16_t> clear;
        for (Item* it : s.items(sh))
            if (!it->parent && it->chunk >= 0 && it->id != focus && it->x <= px + 32 && it->y <= py + 32 &&
                (sh == 398 && reagent_kind(it->frame) >= kBrimstone ? it->x > px - 560 && it->y > py - 560  // step 11's whole search
                                                                     : it->x > px - 330 && it->y > py - 330))
                clear.push_back(it->id);
        for (uint16_t id : clear) put_aside(id);
    }
    s.warp(px + 48, py - 149, pz, m);
    // Candles.
    for (int pt = 0; pt < 5; ++pt) {
        const int cx = px - kPointDX[pt], cy = py - kPointDY[pt];
        const bool red = r.red.count(pt) > 0;
        Item* keep = nullptr;
        for (Item* c : s.items(454)) {
            if (c->parent || c->chunk < 0 || std::abs(c->x - cx) > 24 || std::abs(c->y - cy) > 24 || c->z != pz) continue;
            bool is_red = c->frame >= 12 && c->frame <= 15, is_black = c->frame >= 8 && c->frame <= 11;
            if (!keep && ((red && is_red) || (!red && is_black))) { keep = c; continue; }
            put_aside(c->id);  // wrong colour (or a second one): moved aside
        }
        if (!keep) {
            keep = get_candle(s, red, pent);
            uint16_t id = keep->id;
            s.drop(id, cx, cy, pz);
            keep = s.w.item(id);
        }
        if (keep->frame == 8 || keep->frame == 12) s.use_event(keep->id, 5);  // light it
    }
    // Reagents beside their candles (towards the star's centre, within 48).
    for (auto [pt, kind] : r.reagents) {
        const int cx = px - kPointDX[pt], cy = py - kPointDY[pt];
        Item* rg = get_reagent(s, kind);
        uint16_t id = rg->id;
        if (rg->quality > 1) {  // one off the stack
            id = s.w.create_item(398, rg->frame);
            s.w.item(id)->quality = 1;
            rg->quality = uint16_t(rg->quality - 1);
        }
        int dx = cx > px - 149 ? -24 : 24, dy = cy > py - 149 ? -24 : 24;
        s.drop(id, cx + dx, cy + dy, pz);
    }
    // The focus at the centre.
    s.drop(focus, px - 149, py - 149, pz);
    std::printf("    %s at the pentagram %u (%d,%d,%d): candles", r.name, pent, px, py, pz);
    for (int pt = 0; pt < 5; ++pt)
        for (Item* c : s.items(454))
            if (!c->parent && std::abs(c->x - (px - kPointDX[pt])) <= 24 && std::abs(c->y - (py - kPointDY[pt])) <= 24)
                std::printf(" %u%s", c->frame, c->z == pz ? "" : ("@z" + std::to_string(c->z)).c_str());
    std::printf("; mana %d\n", s.av().mana);
    s.warp(px + 48, py - 149, pz, m);
    s.said.clear();
    s.use_event(pent, 30);
    s.settle(3000);
    s.run(60);
    Item* f = s.w.item(focus);
    bool ok = f && (f->quality >> 8) == r.spell;
    std::printf("    -> focus %u frame %u quality %u (%s), mana %d, said:", focus, f ? f->frame : 0, f ? f->quality : 0, ok ? "charged" : "not charged",
                s.av().mana);
    for (auto& l : s.said) std::printf(" '%s'", l.c_str());
    std::printf("\n");
    if (f && !f->parent) s.take(f);
    return ok;
}
// Rests on the bedroll until the Avatar's mana is at least `need` (a few tries).
void rest_for_mana(Story& s, int need) {
    for (int t = 0; t < 4 && s.av().mana < need; ++t) {
        int before = s.av().mana;
        if (!s.sleep(2)) { std::printf("    (no bedroll to rest on)\n"); break; }
        std::printf("    rested: mana %d -> %d\n", before, s.av().mana);
    }
}

// Chapter 10: the Lava Tunnel, Daemon's Crag, the sorcerers, the Obsidian Fortress, Malchir.
bool ch_sorcery(Story& s) {
    // The backpack is at its weight limit (40 x STR) with the necromancy reagents picked up so
    // far; the sorcery needs room, so the spare ones (beyond two of each frame) are left behind.
    {
        std::map<unsigned, int> kept;
        std::vector<uint16_t> spare;
        for (Item* it : s.items(395))
            if (it->parent && s.w.root_container(*it)->id == 1 && (kept[it->frame] += std::max<int>(1, it->quality)) > 2)
                spare.push_back(it->id);
        for (uint16_t id : spare) s.w.destroy(id);
        std::printf("    left %zu spare necromancy reagents behind (weight now %u of %d)\n", spare.size(), unsigned(s.w.total_weight(s.av())), 40 * s.av().str);
    }
    s.recall("Upper Catacombs");
    s.warp(22623, 6335 + 96, 48, 50);
    s.walk_into(22623, 6335, 49, 400);
    shortcut("Upper Catacombs 2: warped across the water, past the electric gate, the stone bridge and the two metal gates (Aerial Servant on the lever)");
    open_doors(s, 27359, 19935, 600);
    s.warp(27359 - 96, 19935, 48, 49);
    s.walk_to(27359, 19935, 300);
    for (int i = 0; i < 120 && s.map() == 49; ++i) s.step();
    s.settle(3000);
    expect(s, "to_lava_tunnel", "the MAPTELE egg into the Lava Tunnel");
    shortcut("Lava Tunnel: warped over the dark lava (the long jump / Air Walk) to the south exit");
    s.warp(19263, 31487 - 96, 64, s.map());
    s.walk_to(19263, 31487, 300);
    for (int i = 0; i < 120 && s.map() != 12; ++i) s.step();
    s.settle(3000);
    expect(s, "to_daemons_crag", "the MAPTELE egg to Daemon's Crag");
    s.warp(21279, 15647 + 96, 72, 12);
    s.walk_to(21279, 15647, 300);
    s.run(30);
    expect(s, "crag_waypoint", "walked onto the waypoint");
    for (unsigned n : {29u, 28u, 27u}) {
        Actor* a = s.w.actor(uint16_t(n));
        if (a) std::printf("    NPC %u '%s' on map %u at %d,%d,%d\n", n, GameLog::name_of(s.w, a->id, false).c_str(), a->mapnum, a->x, a->y, a->z);
    }
    // Bane: the current sorcerers, the acolytes, Vardion, her terms (the answer chain read from
    // BANE's answer checks: "I agree to your terms." sets G[385:1]).
    std::vector<std::string> bane = {"I agree to your terms", "No, please go on", "Malicious comments", "What do you mean by First Acolyte",
                                     "Tell me more of Vardion", "Who are the Acolytes", "Current.", "Tell me about Sorcerers", "What is a Truename",
                                     "I am Avatar", "Yes."};
    for (int t = 0; t < 3 && !s.g(385); ++t)
        s.talk(29, bane, [&] { return s.g(385) == 1; }, {"Morgaelin", "crag", "Elements", "Titan", "Elder Tongue", "not my friend", "change the subject"}, 40);
    expect(s, "choose_mentor", "Bane: sorcerers, acolytes, Vardion, her offer");
    // Vardion's mirror deal (G[371:1]): he gives his Truename, Morduin, which Bane then wants.
    std::vector<std::string> vard = {"Yes, I am prepared", "And what might that be", "Shrewd bargaining on both sides",
                                     "You have dealings with Tenebrae", "First Acolyte. What is that", "Who are you", "I am Avatar", "Ah, I see",
                                     "What do you want from me", "I am a stranger here"};
    for (int t = 0; t < 3 && !s.g(371); ++t)
        s.talk(28, vard, [&] { return s.g(371) == 1; }, {"Earth", "Water", "Air", "Lurker", "Pyros", "Hydros", "Lithos", "Stratos", "grandmother",
                                                         "island", "world", "Far away", "dirty work", "decline", "unknown task", "think on it", "extort", "lackey", "Whatever you want",
                                                         "great hero", "business", "permission", "Master research", "Spellcraft", "element", "Fire"}, 40);
    std::printf("    Vardion's deal G[371] %u\n", s.g(371));
    for (int t = 0; t < 3 && !s.g(142, 2); ++t)
        s.talk(29, {" Morduin.", "I don't know it yet", "Truename", "truename"}, [&] { return s.g(142, 2) != 0; },
               {"Morgaelin", "crag", "Elements", "Titan", " Sabriane.", " Daemion.", " Vardion."}, 30);
    s.settle(9000);
    expect(s, "truename", "gave Bane Vardion's Truename");
    // The first test (DAEMSCEN): ask to be tested (G[147:1]), then enchant Flash, Flame Bolt and
    // Endure Heat at the mentor's house pentagram (magic.md 3.3): the candles lit on the points,
    // the reagents beside them, a blank focus (Bane's rod, from her bag) on the star.
    // Everything is gathered first: walking out of the house (672 from its pentagram) ends the
    // test. The supplies: Bane's reagents (her bag, her pentagram's floor), two more black
    // candles (three spares: the Endure Heat layout has three black and the house only one), Bane's blank rod; then a rest for mana (the three cost up to 16).
    std::printf("    Avatar: INT %d, mana %d\n", s.av().intel, s.av().mana);
    Actor* bane_npc = s.w.actor(29);
    Item* house = nullptr;
    for (Item* p : s.items(124))
        if (p->frame == 4 && !p->parent && std::abs(p->x - bane_npc->x) < 640 && std::abs(p->y - bane_npc->y) < 640) house = p;
    CHECK(house != nullptr);
    uint16_t house_id = house ? house->id : 0;
    s.warp(18280 + 48, 15904 + 48, 56, 12);
    Item* rod = s.nearest(397, 18280, 15904, 16);  // in Bane's bag on her table
    if (rod) s.take(rod);
    uint16_t focus = rod ? rod->id : 0;
    for (auto [kind, n] : std::vector<std::pair<int, int>>{{kAsh, 3}, {kPumice, 2}, {kIron, 2}, {kObsidian, 2}}) stock_reagents(s, kind, n);
    for (int t = 0; t < 6 && carried_candles(s, false) < 3; ++t) get_candle(s, false, house_id, true);
    std::printf("    gathered: black candles %d, ash %d, pumice %d, iron %d, obsidian %d\n", carried_candles(s, false), carried_reagents(s, kAsh),
                carried_reagents(s, kPumice), carried_reagents(s, kIron), carried_reagents(s, kObsidian));
    rest_for_mana(s, 20);
    if (std::getenv("PG_GW")) s.w.vm().global_watch = [&](uint16_t cls, unsigned pos, unsigned n, bool wr, uint16_t v) {
        if (wr && pos >= 147 && pos <= 153) std::printf("      G[%u:%u] = %u by class %u (avatar %d,%d frame %d)\n", pos, n, v, cls, s.av().x, s.av().y, s.frames);
    };
    s.talk(29, {"I am ready to be tested", "How will I be tested", "What is the Ritual of Fire"}, [&] { return s.g(147) == 1; }, {}, 20);
    std::printf("    G[147] %u (the test begun)\n", s.g(147));
    s.settle(3000);
    {
        for (const Recipe* r : {&kFlash, &kFlameBolt, &kEndureHeat}) {
            if (!house_id || !focus || s.g(153)) break;
            bool ok = enchant(s, house_id, *r, focus);
            CHECK(ok);
            std::printf("    G[148] %u, G[150..152] %u %u %u\n", s.g(148, 2), s.g(150), s.g(151), s.g(152));
        }
        for (int i = 0; i < 3000 && !s.g(153); ++i) { s.default_answer(); s.step(); }
        s.settle(6000);
        std::printf("    the first test: G[148] %u, G[153] %u\n", s.g(148, 2), s.g(153));
        CHECK_EQ(s.g(153), 1u);
    }
    s.talk(29, {"I have", "test", "Farewell"}, {}, {}, 6);
    expect(s, "first_test", "the first test passed");
    // Candles for the Pyros ceremony (a red one is needed; which frame is red is not in the specs,
    // so one of each kind found lying in the Enclave).
    for (int f : {0, 12}) if (Item* c = s.nearest(454, 18719, 15903, f)) s.take(c);
    // For Malchir's test (his pentacle, his staff, his reagents, magic.md 3.3): two talismans
    // (Summon and Banish Daemon need one; two lie loose in the Enclave's south house), the
    // reagents of the four spells and spare candles, gathered here (Recall does not reach the
    // fortress and nothing is to be had inside).
    for (auto [x, y] : std::vector<std::pair<int, int>>{{14143, 18367}, {14239, 19103}})
        if (Item* t = s.nearest(397, x, y, 18); t && !t->parent) s.take(t);
    for (auto [kind, n] : std::vector<std::pair<int, int>>{{kAsh, 4}, {kPumice, 4}, {kIron, 5}, {kObsidian, 2}, {kBrimstone, 1}, {kBone, 2}})
        stock_reagents(s, kind, n);
    for (int t = 0; t < 6 && carried_candles(s, false) < 2; ++t) get_candle(s, false, 0, true);
    for (int t = 0; t < 6 && carried_candles(s, true) < 2; ++t) get_candle(s, true, 0, true);
    {
        std::map<unsigned, int> by;
        std::function<void(uint16_t)> walk = [&](uint16_t c) { for (uint16_t k : s.w.item(c)->contents) { by[s.w.item(k)->shape]++; walk(k); } };
        walk(1);
        std::printf("    carried (STR %d, weight %u):", s.av().str, unsigned(s.w.total_weight(s.av())));
        for (auto [sh, n] : by) std::printf(" %u x%d", sh, n);
        std::printf("\n");
    }
    std::printf("    for Malchir: talismans %d, black candles %d, red %d, ash %d pumice %d iron %d obsidian %d brimstone %d bone %d\n",
                s.count_carried(397, 18), carried_candles(s, false), carried_candles(s, true), carried_reagents(s, kAsh), carried_reagents(s, kPumice),
                carried_reagents(s, kIron), carried_reagents(s, kObsidian), carried_reagents(s, kBrimstone), carried_reagents(s, kBone));
    // The Obsidian Fortress: west, north, west over the bridge; the doors open for a disciple.
    s.warp(5503 + 64, 18751, 96, 12);
    s.run(120);
    s.settle(3000);
    open_doors(s, 3903, 18687, 600);
    for (int dx : {96, 64, 40})
        if (s.map() == 12) { s.warp(3903 + dx, 18687, 56, 12); s.walk_into(3903 - 32, 18687, 14, 300); }
    std::printf("    at %d,%d,%d map %u\n", s.av().x, s.av().y, s.av().z, s.map());
    expect(s, "to_fortress", "the FORTOPEN egg, the fortress doors");
    Actor* arc = s.w.actor(57);
    if (arc) std::printf("    Arcadion (NPC 57) on map %u at %d,%d,%d\n", arc->mapnum, arc->x, arc->y, arc->z);
    shortcut("Obsidian Fortress: warped past Malchir's two daemons to Arcadion");
    for (int t = 0; t < 3 && !s.g(232); ++t) s.talk(57, {}, [&] { return s.g(232) == 1; }, {}, 30);
    expect(s, "meet_arcadion", "spoke to Arcadion");
    // The four symbols (shape 397 frame 20), one beyond each trial.
    shortcut("Obsidian Fortress: warped past the four trials (rollers, mushrooms, burning pentagram, the Endure Heat maze) to each symbol");
    std::vector<uint16_t> sym;
    for (Item* it : s.items(397, 20)) sym.push_back(it->id);
    std::printf("    %zu symbols on the map\n", sym.size());
    for (uint16_t id : sym) if (s.w.item(id)) s.take(s.w.item(id));
    std::printf("    symbols carried: %d\n", s.count_carried(397, 20));
    expect(s, "four_symbols", "took the four symbols");
    s.warp(15615 + 32, 16639 + 32, 88, 14);
    s.walk_to(15615, 16639, 200);
    for (int i = 0; i < 300 && !s.w.stasis; ++i) s.step();
    s.settle(6000);
    for (int t = 0; t < 3 && !s.g(144); ++t) s.talk(57, {}, [&] { return s.g(144) == 1; }, {}, 30);
    expect(s, "arcadion_done", "the centre platform with the symbols, Arcadion");
    // Malchir's test: Flame Bolt, Explosion, Summon Daemon at him, then Banish Daemon on his
    // daemon and his staff dropped (G[353:3] counts 1..6, magic.md 3.3). Each spell is enchanted
    // first at his pentacle (his staff for the two bolts, a talisman for each daemon spell), then
    // cast at him; his watcher then ends the test (G[155:1]) and takes the Avatar to the Great Pentacle.
    Actor* mal = s.w.actor(27);
    std::printf("    Malchir on map %u at %d,%d,%d\n", mal->mapnum, mal->x, mal->y, mal->z);
    if (s.map() != mal->mapnum) s.warp(mal->x + 96, mal->y, mal->z, mal->mapnum);
    s.talk(27, {"test", "Test", "ready", "Yes", "I am ready"}, [&] { return s.g(353, 3) >= 1; }, {}, 30);
    std::printf("    G[353] %u (the test begun)\n", s.g(353, 3));
    {
        Item* pent = nullptr;
        for (Item* p : s.items(124))
            if (p->frame == 4 && !p->parent && std::abs(p->x - mal->x) < 640 && std::abs(p->y - mal->y) < 640) pent = p;
        Item* staff = pent ? s.nearest(397, pent->x - 149, pent->y - 149, 17) : nullptr;  // on his pentacle
        CHECK(pent && staff);
        uint16_t pent_id = pent ? pent->id : 0, staff_id = staff ? staff->id : 0;
        std::vector<uint16_t> talismans;
        for (Item* t : s.items(397)) if (t->frame == 18 && t->parent && s.w.root_container(*t)->id == 1) talismans.push_back(t->id);
        CHECK(talismans.size() >= 2);
        // Enchants, comes back to him and casts the focus at `target` until G[353:3] reaches `next`.
        auto step = [&](const Recipe& r, uint16_t focus, unsigned next, const std::function<uint16_t()>& target) {
            if (s.g(353, 3) >= next || !focus) return;
            rest_for_mana(s, 13);
            bool ok = enchant(s, pent_id, r, focus);
            CHECK(ok);
            Actor* m = s.w.actor(27);
            s.warp(m->x + 160, m->y, m->z, s.map());
            for (int t = 0; t < 3 && s.g(353, 3) < next; ++t) {
                uint16_t tg = target();
                // "The focus must be held": in the Avatar's hand (his own inventory, not the backpack).
                s.w.move_to_container(focus, 1);
                s.w.vm().trace_intrinsics = std::getenv("PG_MAL") != nullptr;
                s.said.clear();
                s.use_on(focus, tg, 60);
                s.w.vm().trace_intrinsics = false;
                std::printf("      cast %u at %u (focus parent %u q %u):", focus, tg, s.w.item(focus) ? s.w.item(focus)->parent : 0, s.w.item(focus) ? s.w.item(focus)->quality : 0);
                for (auto& l : s.said) std::printf(" '%s'", l.c_str());
                std::printf("\n");
                for (int i = 0; i < 900 && s.g(353, 3) < next; ++i) { s.default_answer(); s.step(); }
                s.settle(3000);
            }
            std::printf("    %s cast: G[353] %u\n", r.name, s.g(353, 3));
            CHECK(s.g(353, 3) >= next);
        };
        auto malchir = [] { return uint16_t(27); };
        step(kFlameBolt, staff_id, 2, malchir);
        step(kExplosion, staff_id, 3, malchir);
        std::set<uint16_t> before;
        for (unsigned i = 2; i < 256; ++i) if (Actor* a = s.w.actor(uint16_t(i)); a && a->shape == 96) before.insert(uint16_t(i));
        step(kSummonDaemon, talismans.size() > 0 ? talismans[0] : 0, 4, malchir);
        // His daemon: a daemon (NPC shape 96) that is not ours, going for the Avatar.
        auto his_daemon = [&]() -> uint16_t {
            uint16_t any = 0;
            for (unsigned i = 2; i < 256; ++i)
                if (Actor* a = s.w.actor(uint16_t(i)); a && a->shape == 96 && !a->is(Actor::kDead) && a->mapnum == s.map()) {
                    if (a->target == 1) return uint16_t(i);
                    if (!before.count(uint16_t(i))) any = uint16_t(i);
                }
            return any;
        };
        s.run(120);
        step(kBanishDaemon, talismans.size() > 1 ? talismans[1] : 0, 5, his_daemon);
        // Compliant replies in the ceremony, which starts as soon as the staff is down (the Avatar
        // has no Ignite focus).
        s.picker = [](const std::vector<std::string>& m) {
            for (const char* k : {"Yes, Master", "I don't have the spell prepared", "I do not have one", "Yes"})
                for (size_t i = 0; i < m.size(); ++i) if (m[i].find(k) != std::string::npos) return int(i);
            return -1;
        };
        // "Return to me my staff ... Just put it anywhere": dropped beside the Avatar.
        if (Item* st = s.w.item(staff_id)) {
            if (!st->parent) s.take(st);
            s.drop(staff_id, s.av().x + 64, s.av().y + 32, s.av().z);
        }
        for (int i = 0; i < 1800 && s.g(353, 3) < 6; ++i) { s.default_answer(); s.step(); }
        std::printf("    staff dropped: G[353] %u\n", s.g(353, 3));
        CHECK(s.g(353, 3) >= 6);
    }
    for (int t = 0; t < 3 && !s.g(155) && !s.g(55); ++t) s.talk(27, {"ceremony", "Yes", "ready"}, [&] { return s.g(155) == 1; }, {}, 20);
    s.settle(9000);
    expect(s, "third_test", "Malchir: 'Come, Acolyte. We must begin the ceremony.'");
    std::printf("    map %u at %d,%d,%d\n", s.map(), s.av().x, s.av().y, s.av().z);
    for (int i = 0; i < 20000 && !s.g(55); ++i) { s.default_answer(); s.step(); }
    s.settle(9000);
    s.picker = nullptr;
    expect(s, "pyros_ceremony", "the ceremony at the Great Pentacle");
    // Malchir, back in his fortress: the fight is not scripted, so the Avatar's blows are dealt
    // directly (receive_hit from the Avatar, the engine's combat damage path) until he falls.
    s.warp(3903 + 96, 18687, 56, 12);
    s.walk_into(3903 - 32, 18687, 14, 300);
    mal = s.w.actor(27);
    std::printf("    Malchir on map %u at %d,%d,%d hp %u\n", mal->mapnum, mal->x, mal->y, mal->z, mal->hp);
    shortcut("Obsidian Fortress: warped to Malchir; the fight's blows dealt directly with receive_hit (combat is not scripted)");
    if (mal->mapnum == s.map()) {
        s.warp(mal->x + 96, mal->y, mal->z, s.map());
        for (int i = 0; i < 400 && s.w.actor(27) && !s.w.actor(27)->is(Actor::kDead); ++i) {
            Actor* m = s.w.actor(27);
            s.w.receive_hit(27, 1, direction_to(m->x - s.av().x, m->y - s.av().y), 20, 0);
            s.run(15);
        }
    }
    s.settle(6000);
    expect(s, "kill_malchir", "Malchir defeated");
    if (Actor* m = s.w.actor(27)) {
        s.warp(m->x + 64, m->y, m->z, s.map());
        s.use(27, 10);  // the body's container
        for (Item* it : s.items(442)) if (it->parent && s.w.root_container(*it)->id == 27) s.take(it);
    }
    expect(s, "tongue_of_flame", "took the Tongue of Flame from his body");
    return true;
}

// Chapter 11: the Blackrock: Mythran, the book, Devon's key, the Tear, the Heart of Earth.
bool ch_blackrock(Story& s) {
    if (s.map() == 14) {  // Recall does not work in the fortress: walk out first
        shortcut("Obsidian Fortress: warped out to its doors on Daemon's Crag (the walk back)");
        s.warp(5503 - 200, 18751, 96, 12);
    }
    // Recall does not work on Daemon's Crag or in the Lava Tunnel either (RECALL's map list):
    // back through the tunnel to Upper Catacombs 2.
    if (s.map() == 12) {
        s.warp(20223, 13663 + 96, 56, 12);
        s.walk_to(20223, 13663, 300);
        for (int i = 0; i < 120 && s.map() == 12; ++i) s.step();
    }
    if (s.map() == 24 || s.map() == 25) {
        s.warp(20287, 23807 + 96, 64, s.map());
        s.walk_to(20287, 23807, 300);
        for (int i = 0; i < 120 && s.map() != 49; ++i) s.step();
    }
    std::printf("    map %u\n", s.map());
    s.recall("Plateau");
    std::printf("    map %u\n", s.map());
    std::vector<std::string> myth_avoid = {"buy scrolls", "Pagan history", "What do you do", "Tell me about Pagan"};
    for (int t = 0; t < 3 && !s.g(65); ++t) s.talk(33, {"I found this with the Sorcerers"}, [&] { return s.g(65) == 1; }, myth_avoid, 20);
    expect(s, "show_tongue", "showed Mythran the Tongue of Flame");
    s.run(60);
    Item* book = s.nearest(288, 1109, 1621, -1, 25);
    std::printf("    the book %s\n", book ? "is there" : "is missing");
    if (book) { s.warp(1109 + 64, 1621 + 32, 42, 8); s.use(book->id, 120); }
    s.settle(3000);
    expect(s, "read_destruction", "read 'The Destruction of the Temple'");
    for (int t = 0; t < 3 && !s.g(304); ++t)
        s.talk(33, {"I want to recreate the obelisk", "obelisk"}, [&] { return s.g(304) == 1; }, myth_avoid, 20);
    expect(s, "ask_obelisk", "asked Mythran how to recreate the obelisk");
    // Devon in the Royal Hall: the key to the Tear's chest.
    s.recall("Central Tenebrae");
    Actor* dv = s.w.actor(2);
    std::printf("    Devon on map %u at %d,%d,%d\n", dv->mapnum, dv->x, dv->y, dv->z);
    for (int t = 0; t < 3 && !s.carried(82, -1, 90); ++t)
        s.talk(2, {"I need the Blackrock fragment", "Blackrock", "Tear of the Seas", "fragment"}, [&] { return s.carried(82, -1, 90) != nullptr; }, {}, 30);
    expect(s, "devon_key", "Devon gave his key");
    if (Item* chest = s.nearest(673, 11871, 11007)) {
        uint16_t cid = chest->id;
        s.warp(chest->x + 64, chest->y + 64, chest->z, 40);
        if (Item* k = s.carried(82, -1, 90)) s.use_on(k->id, cid, 90);
        std::printf("    chest quality %04x\n", s.w.item(cid)->quality);
        s.loot(11871, 11007, 442);
    }
    expect(s, "tear_of_seas", "unlocked the chest, took the Tear of the Seas");
    // The Pit of Death: Stone Cove's east door, the Key of the Conventicler on a body, the
    // Conventicle's door, Open Ground on the mud for the Heart of Earth.
    s.recall("Hall of the Mountain King");
    shortcut("Hall of the Mountain King: warped from its waypoint back to Stone Cove (the Earth platform ride)");
    s.warp(5374 - 96, 1890, 0, 21);
    s.walk_into(5374, 1890, 57, 400);
    expect(s, "to_pit_of_death", "Stone Cove's east door into the Pit of Death");
    s.loot(14303, 16223, 82);
    std::printf("    the Key of the Conventicler: %d\n", s.carried(82, -1, 59) != nullptr);
    Item* heart = s.nearest(442, 25280, 29096, 1);
    if (heart) std::printf("    the Heart of Earth at %d,%d,%d flags %04x\n", heart->x, heart->y, heart->z, heart->flags);
    for (unsigned sh : {68u, 69u, 340u, 342u})
        for (Item* d : s.items(sh))
            if ((d->quality & 0xFF) == 59) {
                uint16_t did = d->id;
                s.warp(d->x + 48, d->y + 48, d->z, 57);
                if (Item* k = s.carried(82, -1, 59)) s.use_on(k->id, did, 90);
                s.use(did, 60);
                std::printf("    Conventicle door %u at %d,%d,%d: quality %04x\n", did, d->x, d->y, d->z, s.w.item(did)->quality);
            }
    if (heart) {
        uint16_t hid = heart->id;
        shortcut("Pit of Death: warped through the Conventicle onto the mud by the Heart of Earth");
        s.warp(heart->x + 128, heart->y, heart->z + 8, 57);
        uint16_t bag = s.carried(637)->id;
        Item* og = s.make_spell(bag, {s.R("blood"), s.R("blackmoor")});
        if (og) s.use_on(og->id, hid, 300);
        Item* h = s.w.item(hid);
        std::printf("    after Open Ground: the Heart at %d,%d,%d flags %04x\n", h->x, h->y, h->z, h->flags);
        if (!s.carried(442, 1)) s.take(s.w.item(hid));
    }
    expect(s, "heart_of_earth", "dug up the Heart of Earth");
    return true;
}

// Chapter 12: Ethereal Travel, the four Titans, the pentagram, the Black Gate.
bool ch_ether(Story& s) {
    // Ethereal Travel without the book: the obelisk tip used with the other four fragments on the
    // ground within 160 (leveling_facts.md 1.5).
    for (int f = 0; f <= 4; ++f) std::printf("    fragment %d carried: %d\n", f, s.carried(442, f) != nullptr);
    int ax = s.av().x, ay = s.av().y, az = s.av().z;
    int k = 0;
    for (int f = 1; f <= 4; ++f)
        if (Item* fr = s.carried(442, f)) { s.drop(fr->id, ax + 64 + (k % 2) * 32, ay - 32 + (k / 2) * 64, az); ++k; }
    if (Item* tip = s.carried(442, 0)) s.use_event(tip->id, 600);
    s.settle(6000);
    for (int i = 0; i < 300 && s.map() != 43; ++i) s.step();
    for (int f = 0; f <= 4; ++f) {
        Item* fr = s.nearest(442, s.av().x, s.av().y, f);
        std::printf("    fragment %d: %s\n", f, s.carried(442, f) ? "carried" : fr ? "on the map" : "gone");
    }
    expect(s, "ethereal_travel", "the obelisk tip with the four fragments beside it");
    // The four planes: the path's egg out of the pentagram, then (past each plane's jumps and
    // platforms) the fragment used on its Titan; it glows (shape 793) and he returns to 43.
    struct Plane { const char* step; unsigned map; int ex, ey, ez; unsigned titan; int frame; int tx, ty, tz; const char* what; };
    const Plane planes[] = {
        {"defeat_stratos", 46, 14770, 19166, 16, 385, 3, 14719, 3295, 216, "Plane of Air: the path south and west"},
        {"defeat_hydros", 45, 14764, 14036, 16, 80, 2, 7071, 12319, 0, "Plane of Water: the vanishing stones and the three bridges"},
        {"defeat_pyros", 44, 13567, 15295, 16, 109, 4, 23583, 14879, 24, "Plane of Fire: the spheres on the nine-point star and the rocks"},
        {"defeat_lithos", 47, 18678, 15266, 16, 433, 1, 2719, 14495, 56, "Plane of Earth: the lava and the floating rocks"},
    };
    for (const Plane& p : planes) {
        if (s.map() != 43) s.warp(16767, 17279, 16, 43);
        s.warp(p.ex + 64, p.ey, p.ez, 43);
        s.walk_into(p.ex, p.ey, p.map, 300);
        std::printf("    %s: map %u\n", p.step, s.map());
        shortcut(std::string(p.what) + ": warped to the Titan");
        Item* t = s.nearest(p.titan, p.tx, p.ty);
        if (!t && p.titan == 109) { s.warp(p.tx + 128, p.ty, p.tz, p.map); s.run(600); t = s.nearest(p.titan, p.tx, p.ty); }
        if (!t) { std::printf("    no Titan (shape %u) on map %u\n", p.titan, s.map()); continue; }
        uint16_t tid = t->id;
        if (p.titan == 80) {  // the top of the white stairs (usecode egg 39): Hydros rises
            s.warp(7154 + 32, 12790 + 32, 32, 45);
            for (int i = 0; i < 1500; ++i) { s.default_answer(); s.step(); if (!s.w.stasis && i > 300) break; }
            s.settle(6000);
            std::printf("    Hydros frame %u\n", s.w.item(tid) ? s.w.item(tid)->frame : 99);
        } else {
            s.warp(t->x + 96, t->y + 64, t->z, p.map);
        }
        for (int i = 0; i < 1500; ++i) { s.default_answer(); s.step(); if (!s.w.stasis && i > 200) break; }
        s.settle(6000);
        Item* fr = s.carried(442, p.frame);
        if (fr) {
            s.target = tid;
            s.w.vm().trace_intrinsics = std::getenv("PG_UC_INTR") && p.titan == unsigned(std::atoi(std::getenv("PG_UC_INTR")));
            s.w.call_event(fr->id, kEvUse);
            for (int i = 0; i < 200; ++i) s.step();
            s.w.vm().trace_intrinsics = false;
            for (int i = 0; i < 6000 && s.map() == p.map; ++i) { s.default_answer(); s.step(); }
            s.target = 0;
            s.settle(6000);
        }
        expect(s, p.step, std::string("used the fragment on the Titan, back on map ") + std::to_string(s.map()));
    }
    // Each glowing fragment dropped on its pentagram point (ENDPLATE quality q takes 793 frame q-1).
    // The obelisk tip glows when used with the four glowing fragments within 320 (read from
    // BROCK's use event), so the four go on their points first, then the tip is charged at the
    // centre and placed last.
    const char* names[] = {"place_obelisk_tip", "place_heart_of_earth", "place_tear_of_the_seas", "place_breath_of_air", "place_tongue_of_flame"};
    auto place = [&](int f) {
        Item* plate = nullptr;
        for (Item* pl : s.items(686)) if (pl->quality == f + 1) plate = pl;
        Item* fr = s.carried(793, f);
        if (!plate || !fr) { std::printf("    plate %d %d, fragment %d\n", f + 1, plate != nullptr, fr != nullptr); return; }
        int xd, yd, zd;
        s.w.footprint(*plate, xd, yd, zd);
        s.warp(plate->x + 48, plate->y + 48, plate->z, 43);
        s.drop(fr->id, plate->x - xd / 2, plate->y - yd / 2, plate->z + zd + 8);
        s.run(120);
        s.settle(6000);
        expect(s, names[f], "dropped it on its point");
    };
    for (int f = 1; f <= 4; ++f) place(f);
    s.warp(16155 + 16, 16637 + 16, 24, 43);
    s.w.vm().trace_intrinsics = std::getenv("PG_TIP") != nullptr;
    if (Item* tip = s.carried(442, 0)) { s.target = 1; s.use_event(tip->id, 600); s.target = 0; s.settle(6000); }  // on himself
    s.w.vm().trace_intrinsics = false;
    expect(s, "charge_tip", "used the obelisk tip at the pentagram's centre");
    for (unsigned sh : {442u, 793u})
        for (Item* t : s.items(sh, 0)) {
            int x, y, z;
            s.w.location(*t, x, y, z);
            std::printf("    tip: shape %u at %d,%d,%d parent %u root %u\n", sh, x, y, z, t->parent, s.w.root_container(*t)->id);
        }
    std::printf("    G[565..570]: %u %u %u %u %u %u\n", s.g(565), s.g(566), s.g(567), s.g(568), s.g(569), s.g(570));
    place(0);
    s.save("story_before_gate");
    // The Black Gate (shape 685) rises; the Avatar walks into it.
    s.run(300);
    s.settle(9000);
    Item* gate = s.nearest(685, 16155, 16637);
    std::printf("    the Black Gate %s\n", gate ? ("at " + std::to_string(gate->x) + "," + std::to_string(gate->y) + "," + std::to_string(gate->z)).c_str() : "missing");
    if (gate) {
        s.warp(gate->x + 96, gate->y + 96, gate->z, 43);
        s.walk_to(gate->x - 16, gate->y - 16, 600);
        for (int i = 0; i < 600 && !s.g(570); ++i) s.step();
    }
    expect(s, "enter_black_gate", "walked into the Black Gate");
    MovieGump* mv = s.w.gumps.find<MovieGump>();
    std::printf("    the ending movie %s\n", mv ? "plays" : "is not playing");
    CHECK(mv != nullptr);
    std::printf("    critical path: current step %s\n", s.step_name(s.current_step()).c_str());
    CHECK_EQ(s.current_step(), -1);  // no step left: the game is finished
    return true;
}

// After the ending movie (Virtues on): world_ui's play_movie opens the epilogue (CreditsGump with
// Virtues::epilogue_page()). Played in the game mode from the save before the gate, the movie
// run to its end, the epilogue page shot to PG_SHOTS (or SAVE_DIR).
// The Ceremony of Eternity played the way a player meets it (specs/world/scenes.md 1): a new game
// with the story flags of the return trip (G[196] "Mordea took the dagger", G[212] volunteered)
// and the ceremonial dagger in the backpack; the Avatar walks into map 6 through the map 5
// teleporter (CEMHAT2; in the stitched world over the 5/6 seam), north along the track past CEMHAT
// to the tomb's front door, opens it, talks to Vividos from outside and gives him the dagger. Checked: Vividos at his spot by the altar before
// and through the scene, Lothian on the altar, the Avatar walked to the marker and kneeling there,
// the camera inside the hall, the ghouls, dead Lothian, G[189], the second talk, and stasis and the
// camera given back after its Farewell.
//   pagan_story_tests GAME_DIR DATA_DIR SAVE_DIR ritual [stitched]
void ritual_scene(Story& s, bool stitched) {
    std::printf("ritual (%s)\n", stitched ? "stitched world" : "original maps");
    if (std::getenv("PG_GW")) s.w.vm().global_watch = [&](uint16_t cls, unsigned pos, unsigned n, bool wr, uint16_t val) {
        if (wr && (pos == 194 || pos == 195 || pos == 214 || pos == 189 || pos == 13))
            std::printf("      G[%u:%u] = %u (class %u) frame %d, Vividos %d,%d,%d Avatar %d,%d,%d\n", pos, n, val, cls, s.frames, s.w.actor(21)->x,
                        s.w.actor(21)->y, s.w.actor(21)->z, s.av().x, s.av().y, s.av().z);
    };
    s.w.skip_opening = true;
    if (stitched) CHECK(s.w.set_world_mode(true, g_data + "/world_layout.json"));
    s.w.new_game();
    s.run(30);
    s.w.vm().set_global(196, 1, 1);
    s.w.vm().set_global(212, 1, 1);
    s.give(571, 0);
    s.warp(7423, 11300, 48, 5);
    s.walk_into(7423, 10943, 6, 900);
    CHECK_EQ(s.map(), 6);
    // North along the track past CEMHAT (7935, 28223) to the front door (7999, 25599).
    for (auto [x, y] : {std::pair{5900, 29500}, {6600, 29000}, {7300, 28500}, {7935, 28223}, {7999, 27200}, {7999, 26300}, {7999, 25780}})
        s.walk_to(x, y, 1500);
    std::printf("    Avatar at %d,%d,%d\n", s.av().x, s.av().y, s.av().z);
    s.run(30);
    CHECK_EQ(s.g(195), 1);  // CEMHAT2: the dagger is in the backpack
    // He is inside, under the roof: the player opens the front door to find him (the east leaf),
    // then steps back out (PG_RITUAL_CLOSED=1: talks through the closed door instead).
    if (!std::getenv("PG_RITUAL_CLOSED")) {
        Item* door = nullptr;
        for (Item* d : s.items(69)) if (d->y == 25599 && d->x == 8063) door = d;
        CHECK(door != nullptr);
        if (door) {
            uint16_t id = door->id;
            s.walk_to(door->x - 32, 25680, 300);
            s.use(id, 120);
            CHECK(s.w.item(id) && (s.w.item(id)->quality & 0x100));  // open
        }
        s.walk_to(7999, 25780, 300);
    }
    Actor* v = s.w.actor(21);
    const int kSpotX = 7839, kSpotY = 24831, kMarkX = 8063, kMarkY = 24863, kAltarX = 7925, kAltarY = 24863;
    auto at = [](const Item* it, int x, int y, int r) { return it && std::abs(it->x - x) <= r && std::abs(it->y - y) <= r; };
    std::printf("    Vividos at %d,%d,%d; G[194] %u G[214] %u\n", v->x, v->y, v->z, s.g(194), s.g(214));
    CHECK_EQ(s.g(214), 1);  // CEMHAT: Lothian moved onto the altar
    CHECK_EQ(s.g(194), 1);  // CEMHAT: Vividos' schedule off
    CHECK(at(v, kSpotX, kSpotY, 0));
    Item* alive = s.nearest(572, kAltarX, kAltarY, 0);
    CHECK(at(alive, kAltarX, kAltarY, 0));
    // A few game hours on: nothing moves him (G[194] stops his schedule).
    for (int i = 0; i < 4; ++i) {
        s.w.set_game_hours(s.w.game_seconds() / 900 + 1);
        s.run(300);
    }
    CHECK(at(v, kSpotX, kSpotY, 0));
    std::printf("    after 4 hours Vividos at %d,%d,%d\n", v->x, v->y, v->z);

    // The talk from outside the tomb (a double-click: actors at any range).
    CHECK(s.av().y > 25599);
    s.w.use_item(21);
    bool knelt = false, cam_inside = false, cam_outside_late = false, ghouls = false, vividos_moved = false, roof_drawn = false,
         vivalter = false;
    int max_ghouls = 0;
    std::vector<uint16_t> before;
    for (Item* g : s.items(142)) before.push_back(g->id);
    auto scene_watch = [&] {
        if (s.g(189)) return;  // the ritual's own part
        if (!s.w.stasis) return;
        int cam[3];
        s.w.camera_point(cam);
        bool inside = cam[0] > 7170 && cam[0] < 8700 && cam[1] > 24070 && cam[1] < 25599;
        if (s.av().last_anim == 35 && at(&s.av(), kMarkX, kMarkY, 100)) knelt = true;
        if (knelt) {
            if (inside) cam_inside = true;
            else cam_outside_late = true;
            if (s.w.map().roof_limit(cam) > 104) roof_drawn = true;  // the hall's roof hidden from the camera point
        }
        for (Item* va : s.items(429)) if (!va->parent && at(va, kAltarX, kAltarY, 0)) vivalter = true;
        if (!at(v, kSpotX, kSpotY, 0) && v->z != 164) vividos_moved = true;
        int n = 0;
        for (Item* g : s.items(142))
            if (std::find(before.begin(), before.end(), g->id) == before.end() && at(g, 7900, 24800, 400)) ++n;
        max_ghouls = std::max(max_ghouls, n);
        if (n >= 3) ghouls = true;
    };
    // Answer like the player, watching every frame.
    bool second = false;
    for (int f = 0; f < 30000; ++f) {
        if (s.w.ask_menu && s.w.ask_menu->frames_open >= 6 && !s.w.ask_menu->answers.empty()) {
            std::vector<std::string> m;
            for (uint16_t a : s.w.ask_menu->answers) m.push_back(s.w.vm().string(a));
            int pick = -1;
            for (size_t i = 0; i < m.size() && pick < 0; ++i) if (!s.g(189) && m[i].find("dagger") != std::string::npos) pick = int(i);
            for (size_t i = 0; i < m.size() && pick < 0; ++i) if (!s.g(189) && (m[i] == "Yes" || m[i].find("Yes") == 0)) pick = int(i);
            if (s.g(189)) {
                for (size_t i = 0; i < m.size(); ++i) if (m[i].find("Necromancer") != std::string::npos || m[i].find("murdered") != std::string::npos) second = true;
                for (size_t i = 0; i < m.size() && pick < 0; ++i) if (m[i].find("arewell") != std::string::npos) pick = int(i);
            }
            if (pick < 0) pick = 0;
            for (size_t i = 0; i < m.size() && s.trace_talk; ++i) std::printf("      [%s]%s\n", m[i].c_str(), int(i) == pick ? " <-" : "");
            s.choose(unsigned(pick));
        }
        s.step();
        scene_watch();
        if (s.g(189) && !s.w.stasis && !s.w.ask_menu && !s.w.in_conversation()) break;
    }
    std::printf("    knelt at the marker %d, camera inside %d (outside after kneeling %d, roof drawn %d), ghouls %d, VIVALTER at the altar %d, "
                "Vividos left his spot %d\n", knelt, cam_inside, cam_outside_late, roof_drawn, max_ghouls, vivalter, vividos_moved);
    std::printf("    Avatar at %d,%d,%d, Vividos at %d,%d,%d\n", s.av().x, s.av().y, s.av().z, v->x, v->y, v->z);
    CHECK(knelt);
    CHECK(cam_inside);
    CHECK(!cam_outside_late);
    CHECK(!roof_drawn);
    CHECK(ghouls);
    CHECK(vivalter);
    CHECK(!vividos_moved);
    CHECK_EQ(s.g(189), 1);
    CHECK(second);
    Item* dead = s.nearest(572, kAltarX, kAltarY, 1);
    CHECK(at(dead, kAltarX, kAltarY, 0));
    CHECK(s.items(572, 0).empty());
    CHECK(!s.carried(571));
    CHECK(at(&s.av(), kMarkX, kMarkY, 100));
    CHECK(at(v, kSpotX, kSpotY, 0));
    CHECK(!s.w.stasis);
    CHECK_EQ(s.w.camera_followed(), 1);
    CHECK_EQ(s.g(13), 0);
}

void epilogue(const GameData& d) {
    std::printf("epilogue\n");
    GameMode gm(d);
    World& w = gm.world();
    w.skip_opening = true;
    w.ask_name = false;
    w.play_intro = false;
    w.auto_ui = false;
    std::string err;
    CHECK(w.virtues->load_dir(g_data, err));
    w.virtues->enabled = true;
    gm.start();
    std::ifstream f(g_saves + "/story_before_gate.sav", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string bytes = ss.str();
    SaveReader in(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    CHECK(w.load_game(in));
    w.virtues->enabled = true;
    Surface8 surf{320, 200};
    for (int i = 0; i < 30; ++i) { gm.tick(); gm.render(surf); }
    Item* gate = nullptr;
    for (unsigned i = 1; i < w.objects().slots(); ++i) if (Item* it = w.item(uint16_t(i)); it && it->shape == 685) gate = it;
    CHECK(gate != nullptr);
    if (!gate) return;
    // A step into the gate (a physical move of the Avatar: BGATE's gotHit, G[570:1], the movie).
    w.teleport_actor(1, gate->x + 64, gate->y + 64, gate->z, 43);
    for (int i = 0; i < 10; ++i) gm.tick();
    w.collide_move(1, gate->x - 8, gate->y - 8, gate->z, false, false);
    int ticks = 0;
    for (; ticks < 200000 && !w.gumps.find<MovieGump>(); ++ticks) gm.tick();
    std::printf("  G[570] %u, the ending movie %s after %d ticks\n", w.vm().get_global(570, 1), w.gumps.find<MovieGump>() ? "plays" : "did not start", ticks);
    CHECK(w.gumps.find<MovieGump>() != nullptr);
    int movie = 0;
    for (; movie < 400000 && w.gumps.find<MovieGump>(); ++movie) gm.tick();
    std::printf("  the movie ran %d ticks to its end\n", movie);
    CreditsGump* c = w.gumps.find<CreditsGump>();
    std::printf("  the epilogue %s; ending '%s', virtue total %d\n", c ? "is open" : "is NOT open",
                w.virtues->ending() ? w.virtues->ending()->id.c_str() : "?", w.virtues->total());
    CHECK(c != nullptr);
    std::vector<std::string> page = w.virtues->epilogue_page();
    for (const std::string& l : page) std::printf("    | %s\n", l.c_str());
    for (int i = 0; i < 90; ++i) { gm.tick(); gm.render(surf); }
    std::string dir = std::getenv("PG_SHOTS") ? std::getenv("PG_SHOTS") : g_saves;
    mkdir(dir.c_str(), 0755);
    gm.render(surf);
    std::vector<uint32_t> rgba;
    surf.to_rgba(w.data().palette, gm.palette(), rgba);
    std::string path = dir + "/story_epilogue.png";
    save_png(path, rgba.data(), 320, 200, 3);
    std::printf("  shot %s\n", path.c_str());
}

struct Chapter { const char* name; bool (*fn)(Story&); };
const Chapter kChapters[] = {
    {"opening", ch_opening},
    {"plateau", ch_plateau},
    {"cemetery", ch_cemetery},
    {"necromancy", ch_necromancy},
    {"mountain", ch_mountain},
    {"mordea", ch_mordea},
    {"scion", ch_scion},
    {"theurgy", ch_theurgy},
    {"hydros", ch_hydros},
    {"sorcery", ch_sorcery},
    {"blackrock", ch_blackrock},
    {"ether", ch_ether},
};

}  // namespace

// See main: packs the chapter saves for the story jump and checks story_jump.json against them.
void export_checkpoints(Story& s) {
    std::printf("story checkpoints\n");
    std::ifstream jf(g_data + "/story_jump.json");
    std::stringstream js;
    js << jf.rdbuf();
    nlohmann::json j = nlohmann::json::parse(js.str(), nullptr, false);
    CHECK(!j.is_discarded() && j.contains("chapters"));
    if (j.is_discarded() || !j.contains("chapters")) return;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> saves;
    for (const nlohmann::json& c : j["chapters"]) {
        const std::string cp = c.value("checkpoint", std::string());
        if (cp.empty()) continue;
        std::ifstream f(g_saves + "/" + cp + ".sav", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string b = ss.str();
        CHECK(!b.empty());
        if (b.empty()) { std::printf("  no %s.sav\n", cp.c_str()); return; }
        saves.emplace_back(cp, std::vector<uint8_t>(b.begin(), b.end()));
        // The chapter's step: the one current in its checkpoint.
        CHECK(s.load(cp));
        const int cur = s.current_step();
        const int want = c.value("step", -2);
        std::printf("  %-10s %-22s step %d (story_jump.json %d)\n", c.value("id", std::string()).c_str(), cp.c_str(), cur, want);
        CHECK_EQ(cur, want);
    }
    std::vector<uint8_t> pack = StoryPack::build(saves, [](const std::vector<uint8_t>& in) { return zlib_compress(in); });
    StoryPack back;
    std::string err;
    CHECK(back.parse(pack, err));
    for (size_t i = 0; i < saves.size(); ++i) {
        std::vector<uint8_t> raw;
        CHECK(back.get(int(i), raw, err) && raw == saves[i].second);
    }
    const std::string dir = g_data + "/story", path = dir + "/checkpoints.pgs";
    mkdir(dir.c_str(), 0755);
    std::ifstream old(path, std::ios::binary);
    std::stringstream os;
    os << old.rdbuf();
    const std::string was = os.str();
    const bool same = was.size() == pack.size() && std::memcmp(was.data(), pack.data(), pack.size()) == 0;
    if (!same) {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(pack.data()), std::streamsize(pack.size()));
        CHECK(bool(f));
    }
    size_t raw_total = 0;
    for (auto& e : saves) raw_total += e.second.size();
    std::printf("  %zu checkpoints, %zu bytes (%zu as saves): %s %s\n", saves.size(), pack.size(), raw_total,
                same ? "unchanged" : "written to", path.c_str());
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_saves = argc > 3 ? argv[3] : "/tmp/pagan_story_saves";
    std::string from = argc > 4 ? argv[4] : "", to = argc > 5 ? argv[5] : "";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    double t0 = now_ms();
    Story s(d);
    if (from == "code") {  // code CLASS FROM TO: hex of a class's code space (the game data)
        unsigned c = unsigned(std::strtol(argv[5], nullptr, 0)), a = unsigned(std::strtol(argv[6], nullptr, 0)), b = unsigned(std::strtol(argv[7], nullptr, 0));
        const uint8_t* p = d.usecode.code(c);
        std::printf("class %u (%s), size %u, events:", c, d.usecode.class_name(c).c_str(), d.usecode.code_size(c));
        for (unsigned e = 0; e < UsecodeFile::kEvents; ++e) if (d.usecode.event_entry(c, e)) std::printf(" %u@%04X", e, d.usecode.event_entry(c, e));
        std::printf("\n");
        for (unsigned i = a; i < b && p && i < d.usecode.code_size(c); ++i) std::printf("%s%02X", (i - a) % 16 ? " " : (i == a ? "" : "\n"), p[i]);
        std::printf("\n");
        return 0;
    }
    if (from == "grep") {  // grep HEXBYTES...: classes whose code holds that byte string
        std::vector<uint8_t> pat;
        for (int a = 5; a < argc; ++a) pat.push_back(uint8_t(std::strtol(argv[a], nullptr, 16)));
        for (unsigned c = 0; c < d.usecode.class_count(); ++c) {
            const uint8_t* p = d.usecode.code(c);
            uint32_t n = d.usecode.code_size(c);
            if (!p) continue;
            for (uint32_t i = 0; i + pat.size() <= n; ++i)
                if (std::equal(pat.begin(), pat.end(), p + i)) std::printf("  class %u (%s) at %04X\n", c, d.usecode.class_name(c).c_str(), i);
        }
        return 0;
    }
    if (from == "near") {  // near MAP X Y R: every item within R of (X, Y)
        s.w.skip_opening = true;
        s.w.new_game();
        s.run(30);
        s.w.change_map(unsigned(std::atoi(argv[5])));
        s.run(2);
        int x0 = std::atoi(argv[6]), y0 = std::atoi(argv[7]), r = std::atoi(argv[8]);
        for (unsigned i = 1; i < s.w.objects().slots(); ++i) {
            Item* it = s.w.item(uint16_t(i));
            if (!it || it->parent || std::abs(it->x - x0) > r || std::abs(it->y - y0) > r) continue;
            std::printf("  id %u shape %u frame %u q %u npc %u at %d,%d,%d flags %04x cls %d '%s'\n", it->id, it->shape, it->frame, it->quality,
                        it->npcnum, it->x, it->y, it->z, it->flags, s.w.usecode_class(*it), GameLog::name_of(s.w, it->id, false).c_str());
        }
        return 0;
    }
    if (from == "explore") {  // explore MAP SHAPE...: list a map's items of those shapes
        s.w.skip_opening = true;
        s.w.new_game();
        s.run(30);
        s.w.change_map(unsigned(std::atoi(argv[5])));
        s.run(2);
        for (int a = 6; a < argc; ++a)
            for (Item* it : s.items(unsigned(std::atoi(argv[a])))) {
                int x, y, z;
                s.w.location(*it, x, y, z);
                std::printf("  id %u shape %u frame %u q %u npc %u at %d,%d,%d (own %d,%d,%d) parent %u flags %04x\n", it->id, it->shape,
                            it->frame, it->quality, it->npcnum, x, y, z, it->x, it->y, it->z, it->parent, it->flags);
            }
        return 0;
    }
    if (from == "ritual") {
        ritual_scene(s, argc > 5 && std::string(argv[5]) == "stitched");
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    bool started = from.empty();
    std::string prev;
    for (const Chapter& c : kChapters) {
        if (!started && from == c.name) {
            started = true;
            if (!prev.empty() && !s.load("story_after_" + prev)) { std::printf("no save story_after_%s\n", prev.c_str()); return 1; }
        }
        prev = c.name;
        if (!started) continue;
        double t = now_ms();
        std::printf("chapter %s\n", c.name);
        int fails = g_fail;
        if (const char* e = std::getenv("PG_LIST")) {  // items of these shapes now (comma list)
            std::stringstream ss{std::string(e)};
            std::string tok;
            while (std::getline(ss, tok, ','))
                for (Item* it : s.items(unsigned(std::atoi(tok.c_str())))) {
                    int x, y, z;
                    s.w.location(*it, x, y, z);
                    std::printf("  list: id %u shape %u frame %u q %u at %d,%d,%d parent %u flags %04x map %u\n", it->id, it->shape, it->frame,
                                it->quality, x, y, z, it->parent, it->flags, s.map());
                }
        }
        c.fn(s);
        int cur = s.current_step();
        std::printf("  (chapter %s: %.1f s, %d frames; current critical-path step: %s)\n", c.name, (now_ms() - t) / 1000, s.frames,
                    s.step_name(cur).c_str());
        s.save("story_after_" + std::string(c.name));
        if (g_fail != fails && !std::getenv("PG_STORY_GO_ON")) break;
        if (to == c.name) break;
    }
    if ((from.empty() || from == "epilogue" || started) && !std::getenv("PG_NO_EPILOGUE") && (to.empty() || to == "ether" || from == "epilogue")) {
        std::ifstream probe(g_saves + "/story_before_gate.sav");
        if (probe) epilogue(d);
    }
    // The story checkpoints for the debug menu's "Jump to story chapter..." (game/story_pack.h,
    // data/story_jump.json): after a whole run that passed, the saves are packed into
    // DATA/story/checkpoints.pgs (written only when it changed), and every chapter's step in
    // story_jump.json must be the step current in its checkpoint.
    if (from.empty() && to.empty() && g_fail == 0 && !std::getenv("PG_NO_EXPORT")) export_checkpoints(s);
    std::printf("bypasses (%zu):\n", g_bypasses.size());
    for (const std::string& x : g_bypasses) std::printf("  %s\n", x.c_str());
    std::printf("shortcuts (%zu):\n", g_shortcuts.size());
    for (const std::string& x : g_shortcuts) std::printf("  %s\n", x.c_str());
    std::printf("story: %.1f s\n", (now_ms() - t0) / 1000);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
