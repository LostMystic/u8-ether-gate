// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Original bug fixes in the story scripts and text (usecode/uc_patches.*, data/patches/*.json,
// NOTES.md "Original bug fixes"): each bug reproduced with the "Original bug fixes" option Off
// and shown fixed with it On.
//
//   pagan_storyfix_tests GAME_DIR DATA_DIR [SAVE_DIR] [only]
//   pagan_storyfix_tests GAME_DIR DATA_DIR explore MAP SHAPE...   (list those items of a map)
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/container_sort.h"
#include "game/gumps.h"
#include "game/npc_dialog.h"
#include "game/story_fixes.h"
#include "game/processes.h"
#include "game/world.h"
#include "usecode/uc_machine.h"
#include "usecode/uc_patches.h"
#include "usecode/usecode_file.h"

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

std::string g_data, g_saves;

// A new game started at the camp (no opening scene), the world run headless.
struct Game {
    World w;
    int frames = 0;
    explicit Game(const GameData& d) : w(d) {
        w.skip_opening = true;
        w.ask_name = false;
        w.new_game();
        run(30);
    }
    void step() {
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    void run(int n) { for (int i = 0; i < n; ++i) step(); }
    Actor& av() { return *w.avatar(); }
    void warp(int x, int y, int z, unsigned map) {
        w.teleport_actor(1, x, y, z, map);
        run(10);
    }
    unsigned g(unsigned pos, unsigned n = 1) { return w.vm().get_global(pos, n); }
    void set(unsigned pos, unsigned n, unsigned v) { w.vm().set_global(pos, n, uint16_t(v)); }
    // Items of a shape on the current map (top level or inside containers).
    std::vector<Item*> items(unsigned shape) {
        std::vector<Item*> out;
        for (unsigned i = 1; i < w.objects().slots(); ++i)
            if (Item* it = w.item(uint16_t(i)); it && it->shape == shape) out.push_back(it);
        return out;
    }
};

void explore(const GameData& d, unsigned map, const std::vector<unsigned>& shapes) {
    Game g(d);
    g.w.change_map(map);
    g.run(2);
    for (unsigned s : shapes)
        for (Item* it : g.items(s))
            std::printf("shape %u id %u frame %u quality %u at %d,%d,%d map %u parent %u class %d\n", unsigned(it->shape),
                        unsigned(it->id), unsigned(it->frame), unsigned(it->quality), it->x, it->y, it->z,
                        unsigned(it->mapnum), unsigned(it->parent), g.w.usecode_class(*it));
}


// Every string a script can push (push.str), with the patch layer's view of the code.
std::vector<std::string> all_strings(const UsecodeFile& uc) {
    std::vector<std::string> out;
    for (unsigned c = 0; c < uc.class_count(); ++c) {
        const uint8_t* code = uc.code(c);
        uint32_t size = uc.code_size(c);
        if (!code || size <= 0x80) continue;
        for (uint32_t a = 0x80; a < size;) {
            unsigned len = uc_op_length(code + a, size - a);
            if (!len) break;
            if (code[a] == 0x0D) out.push_back(std::string(reinterpret_cast<const char*>(code + a + 3), code[a + 1] | (code[a + 2] << 8)));
            a += len;
        }
    }
    return out;
}

bool has_word(const std::string& s, const std::string& w) {
    auto wc = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '\''; };
    for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1))
        if ((p == 0 || !wc(s[p - 1])) && (p + w.size() >= s.size() || !wc(s[p + w.size()]))) return true;
    return false;
}

// --- The patch layer: the data files load, every patch fits the v2.10 code, text corrections
void test_patches(GameData& d) {
    std::printf("patches: data/patches/*.json load and every patch applies to the v2.10 usecode\n");
    std::string err;
    CHECK(load_story_patches(d.usecode, g_data + "/patches", err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    const StoryPatches& p = story_patches();
    CHECK(p.bytes.size() >= 2);
    CHECK(p.texts.size() >= 50);
    // Bad files are refused.
    StoryPatches bad;
    CHECK(!bad.load_usecode_json("{\"patches\": [{\"id\": \"x\", \"class\": 1, \"at\": \"80\", \"expect\": \"12\", \"replace\": \"1234\"}]}", err));
    CHECK(!bad.load_text_json("{\"replace\": [{\"replace\": \"y\"}]}", err));
    std::vector<std::string> problems;
    StoryPatches wrong;
    CHECK(wrong.load_usecode_json("{\"patches\": [{\"id\": \"w\", \"class\": 288, \"at\": \"16B2\", \"expect\": \"00 00\", \"replace\": \"11 11\"}]}", err));
    UsecodeFile scratch = d.usecode;
    CHECK_EQ(wrong.install(scratch, problems), 0);
    CHECK_EQ(problems.size(), 1u);
    // Whole words only, and nothing while the option is Off.
    original_bug_fixes() = true;
    CHECK(p.fix_text("the Restortation spell") == "the Restoration spell");
    CHECK(p.fix_text("Do you always wear that silly costume? ") == "Do you always wear that silly costume? ");
    CHECK(p.fix_text("a repulsive costum and") == "a repulsive costume and");
    original_bug_fixes() = false;
    CHECK(p.fix_text("the Restortation spell") == "the Restortation spell");
    // Each correction is for text that is really in the game, and none is left with the option On.
    std::vector<std::string> before = all_strings(d.usecode);
    for (const StoryPatches::Text& t : p.texts) {
        bool found = false;
        for (const std::string& s : before) if (t.word ? has_word(s, t.find) : s.find(t.find) != std::string::npos) { found = true; break; }
        if (!found) std::printf("  not in the game's text: '%s'\n", t.find.c_str());
        CHECK(found);
    }
    original_bug_fixes() = true;
    int left = 0;
    for (const std::string& s0 : all_strings(d.usecode)) {
        std::string s = p.fix_text(s0);
        for (const StoryPatches::Text& t : p.texts)
            if (t.word ? has_word(s, t.find) : s.find(t.find) != std::string::npos) ++left;
    }
    CHECK_EQ(left, 0);
}

// The text of the reader (book / scroll) that is open, lines joined.
std::string reader_text(World& w) {
    std::string out;
    if (ReaderGump* r = w.reader())
        for (const TextPage& pg : r->pages)
            for (const std::string& l : pg.lines) out += l + " ";
    return out;
}

// Opens a book: shape `shape` with `quality` in the Avatar's backpack, used; the reader's text.
std::string read_book(const GameData& d, unsigned shape, unsigned quality, bool fixes) {
    original_bug_fixes() = fixes;
    Game g(d);
    uint16_t b = g.w.create_item(shape, 0);
    g.w.item(b)->quality = uint16_t(quality);
    g.w.give_to_avatar(b);
    g.w.call_event(b, kEvUse);
    for (int f = 0; f < 60 && !g.w.reader(); ++f) g.step();
    return reader_text(g.w);
}

// --- 6. The Spell of Resurrection book (GRIMOIRE / BASEBOOK quality 102) shows Intervention
void test_resurrection_book(const GameData& d) {
    std::printf("resurrection: the quality-102 spell book shows the Resurrection text\n");
    for (unsigned shape : {288u, 613u}) {
        std::string off = read_book(d, shape, 102, false), on = read_book(d, shape, 102, true);
        std::printf("  shape %u off: %.60s...\n  shape %u on:  %.60s...\n", shape, off.c_str(), shape, on.c_str());
        CHECK(off.find("Intervention") != std::string::npos);  // the original bug, reproduced
        CHECK(off.find("Resurrection") == std::string::npos);
        CHECK(on.find("Resurrection") != std::string::npos);    // fixed
        CHECK(on.find("Intervention") == std::string::npos);
        // The real Intervention book (quality 101) is unchanged.
        CHECK(read_book(d, shape, 101, true).find("Intervention") != std::string::npos);
    }
}

// --- 7. Typos: a book and a conversation answer through the real VM
void test_typos(const GameData& d) {
    std::printf("typos: corrected in books and menus with the option On, as shipped with it Off\n");
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Game g(d);
        // The Aerial Servant book (SGBOOK 1399:0F95): "called into existance".
        uint16_t b = g.w.create_item(288, 0);
        g.w.give_to_avatar(b);
        g.w.vm().start(1399, 0x0F95, true, b, {}, false);
        for (int f = 0; f < 60 && !g.w.reader(); ++f) g.step();
        std::string t = reader_text(g.w);
        CHECK(!t.empty());
        CHECK(has_word(t, fixes ? "existence" : "existance"));
        CHECK(!has_word(t, fixes ? "existance" : "existence"));
        // A string pushed by a script: the Theurgy spell list (AVATAR) names "Restortation".
        uint16_t sid = g.w.vm().new_string(story_patches().fix_text("Restortation"));
        CHECK(g.w.vm().string(sid) == (fixes ? "Restoration" : "Restortation"));
    }
}

// --- 5. The Ancient Ones' speech starts once (two ANCTONES eggs watch the Zealan altar)
void test_ancient_ones(const GameData& d) {
    std::printf("ancient ones: the shield on the altar starts the scene exactly once\n");
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Game g(d);
        g.warp(11900, 2900, 8, 54);
        std::vector<Item*> shield = g.items(828), altar = g.items(550);
        CHECK(!shield.empty() && !altar.empty());
        if (shield.empty() || altar.empty()) continue;
        int xd, yd, zd;
        g.w.footprint(*altar[0], xd, yd, zd);
        g.w.move_to(shield[0]->id, altar[0]->x - xd / 2, altar[0]->y - yd / 2, altar[0]->z + zd);
        std::set<uint16_t> scenes;
        unsigned most = 0;
        for (int f = 0; f < 900; ++f) {
            g.step();
            unsigned live = 0;
            g.w.kernel().for_each([&](Process* p) {
                auto* u = dynamic_cast<UCProcess*>(p);
                if (u && u->live() && u->start_cls == 1163 && u->start_addr == 0x0116) { scenes.insert(u->pid()); ++live; }
            });
            most = std::max(most, live);
        }
        std::printf("  fixes %s: %zu scene process(es), at most %u at once, G[75] %u\n", fixes ? "on" : "off", scenes.size(), most,
                    g.g(75));
        CHECK_EQ(g.g(75), 1u);   // the scene ran
        CHECK_EQ(most, 1u);      // never two at once
        CHECK_EQ(scenes.size(), 1u);
    }
}

// Waits until an answer menu is up (speech hurried along); false on timeout.
bool wait_menu(Game& g, int max = 6000) {
    for (int f = 0; f < max; ++f) {
        if (g.w.ask_menu && g.w.ask_menu->frames_open >= 6) return true;
        for (Bark& b : g.w.barks) b.ticks_left = std::min(b.ticks_left, 2);
        g.step();
    }
    return false;
}
std::vector<std::string> menu(Game& g) {
    std::vector<std::string> out;
    if (g.w.ask_menu) for (uint16_t sid : g.w.ask_menu->answers) out.push_back(g.w.vm().string(sid));
    return out;
}

uint16_t add_reagent(Game& g, uint16_t pack, unsigned frame, unsigned qty) {
    uint16_t r = g.w.create_item(395, frame);
    g.w.item(r)->quality = uint16_t(qty);
    g.w.move_to_container(r, pack);
    return r;
}

// --- 4. Vividos' reagents: his hood (any of frames 10-12 look the same) and the dead man's
// elbow (frame 20, merged with the other wooden sticks 16-19 in the original)
struct VividosRun { unsigned stacks = 0; bool offered = false, delivered = false, sticks_kept = false; };

VividosRun vividos_run(const GameData& d, bool fixes, bool merge) {
    original_bug_fixes() = fixes;
    Game g(d);
    g.set(189, 1, 1);  // past the first meeting
    g.set(213, 1, 1);  // the apprentice
    g.set(197, 1, 1);  // he has asked for the reagents
    uint16_t pack = g.w.equipped(1, 7);
    VividosRun r;
    // The player's own hood (frame 11) and plateau sticks (frame 17) come first in the backpack;
    // the hood from the East Road (frame 10) and the elbow from West Tenebrae (frame 20) follow.
    add_reagent(g, pack, 11, 2);
    uint16_t sticks = add_reagent(g, pack, 17, 3);
    if (merge) {
        add_reagent(g, pack, 10, 1);
        add_reagent(g, pack, 20, 2);
        g.w.open_gump(pack, 3);
        sort_container(g.w, pack);  // merges stacks (the same rule as a drop on a stack)
        g.run(5);
        g.w.close_gump(pack);
    } else {
        add_reagent(g, pack, 20, 1);  // only a frame-11 hood: no frame-10 one at all
    }
    for (uint16_t k : g.w.item(pack)->contents) r.stacks += g.w.item(k) && g.w.item(k)->shape == 395;
    const Actor* v = g.w.actor(21);
    if (!v) return r;
    g.warp(v->x + 96, v->y, v->z, v->mapnum);
    g.w.call_event(21, kEvUse);
    if (!wait_menu(g)) return r;
    std::vector<std::string> m = menu(g);
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i].rfind("I have brought you the reagents", 0) == 0) {
            r.offered = true;
            g.w.choose_answer(unsigned(i));
            for (int f = 0; f < 3000 && !g.g(199); ++f) { for (Bark& b : g.w.barks) b.ticks_left = std::min(b.ticks_left, 2); g.step(); }
            r.delivered = g.g(199) == 1;
            r.sticks_kept = g.w.item(sticks) && g.w.item(sticks)->parent;
            for (uint16_t k : g.w.item(pack)->contents)  // his hood and elbow are gone
                if (const Item* it = g.w.item(k); it && it->shape == 395 && ((it->frame >= 10 && it->frame <= 12) || it->frame == 20))
                    r.sticks_kept = false;
            break;
        }
    return r;
}

void test_vividos(const GameData& d) {
    std::printf("vividos: any hood and his elbow, merged or not, are his reagents\n");
    for (bool merge : {true, false})
        for (bool fixes : {false, true}) {
            VividosRun r = vividos_run(d, fixes, merge);
            std::printf("  %-30s fixes %-3s: %u reagent stacks, offered %d, Key of the Caretaker %d, plateau sticks kept %d\n",
                        merge ? "hood and elbow merged" : "a frame-11 hood, not merged", fixes ? "on" : "off", r.stacks,
                        int(r.offered), int(r.delivered), int(r.sticks_kept));
            CHECK_EQ(r.stacks, merge ? (fixes ? 3u : 2u) : 3u);  // On: the elbow keeps its own stack
            CHECK_EQ(r.offered, fixes);                            // Off: the original bug, reproduced
            CHECK_EQ(r.delivered, fixes);
            if (fixes) CHECK(r.sticks_kept);  // he takes the hoods and the elbow, not the other sticks
        }
    // The merge rule itself.
    original_bug_fixes() = true;
    CHECK(necro_elbow_apart(395, 20, 17));
    CHECK(!necro_elbow_apart(395, 20, 20));
    CHECK(!necro_elbow_apart(395, 16, 19));
    CHECK(!necro_elbow_apart(398, 20, 17));
    original_bug_fixes() = false;
    CHECK(!necro_elbow_apart(395, 20, 17));
}

// Hurries speech along for n frames; answers the target cursor with `target` if it comes up.
void play(Game& g, int n, uint16_t target = 0) {
    for (int f = 0; f < n; ++f) {
        for (Bark& b : g.w.barks) b.ticks_left = std::min(b.ticks_left, 2);
        if (g.w.ask_menu && g.w.ask_menu->frames_open >= 6 && !g.w.ask_menu->answers.empty()) g.w.choose_answer(0);
        if (g.w.target_pid && target) {
            if (Process* p = g.w.kernel().get(g.w.target_pid)) { p->result = target; p->terminate(); }
            g.w.target_pid = 0;
        }
        g.step();
    }
}
bool uc_alive(Game& g, unsigned cls, unsigned addr) {
    bool any = false;
    g.w.kernel().for_each([&](Process* p) {
        auto* u = dynamic_cast<UCProcess*>(p);
        if (u && u->live() && u->start_cls == cls && u->start_addr == addr) any = true;
    });
    return any;
}
// Casts Death Speak (ERTHSPEL 1414:0080, as the necromancy talisman starts it) on `target`.
void death_speak(Game& g, uint16_t target, int frames = 3000) {
    g.av().mana = 50;
    g.w.vm().start(1414, 0x0080, true, 1, {0, 0}, true);
    for (int f = 0; f < frames; ++f) {
        play(g, 1, target);
        if (!uc_alive(g, 1414, 0x0080) && !g.w.in_conversation() && !g.w.stasis && f > 30) break;
    }
}

Item* necromancer(Game& g, unsigned q) {
    for (Item* it : g.items(623)) if (it->quality == q) return it;
    return nullptr;
}
// Double-clicks a Necromancer without the spell: does he speak?
bool plain_use_talks(Game& g, Item* nec) {
    g.warp(nec->x + 64, nec->y, nec->z, 62);
    g.w.call_event(nec->id, kEvUse);
    bool spoke = false;
    for (int f = 0; f < 1500; ++f) {
        for (const Bark& b : g.w.barks) spoke = spoke || b.speaker == nec->id;
        play(g, 1);
    }
    return spoke;
}


// Makes a spell with the Key of the Scion: wood and dirt (Rock Flesh, the 2nd Necromancer's) in a
// bag. True if the spell item (shape 746 frame 2) is in the bag afterwards.
bool make_rock_flesh(const GameData& d, bool fixes, bool taught, unsigned key_frame = 1) {
    original_bug_fixes() = fixes;
    Game g(d);
    g.set(202, 1, taught ? 1 : 0);
    uint16_t pack = g.w.equipped(1, 7);
    uint16_t bag = g.w.create_item(637, 0);
    g.w.move_to_container(bag, pack);
    add_reagent(g, bag, 8, 1);
    add_reagent(g, bag, 9, 1);
    uint16_t key = g.w.create_item(232, key_frame);
    g.w.move_to_container(key, pack);
    g.w.call_event(key, kEvUse);
    play(g, 200, bag);
    bool made = false;
    for (uint16_t k : g.w.item(bag)->contents) made = made || (g.w.item(k) && g.w.item(k)->shape == 746 && g.w.item(k)->frame == 2);
    return made;
}

// --- 9. The sorcerers' first test (DAEMSCEN 0x414C) after a partial pass (bugfixes.md 16): the
// marks of the spells already done (G[150..152]) are cleared when the test starts again.
void test_first_test(const GameData& d) {
    std::printf("first test: a retry after a partial pass\n");
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Game g(d);
        // A forfeited attempt that had done Flash and Flame Bolt: marks set, the count reset.
        g.set(150, 1, 1);
        g.set(151, 1, 1);
        g.set(152, 1, 0);
        g.set(148, 2, 0);
        // The watcher on the mentor (Bane, NPC 29) and any frame-4 pentagram, as DAEMSCEN starts it.
        uint16_t pent = 0;
        for (Item* it : g.items(124)) if (it->frame == 4) { pent = it->id; break; }
        if (!pent) pent = 1;
        g.w.vm().start(1418, 0x414C, true, 29, {uint8_t(pent), uint8_t(pent >> 8)}, true);
        g.run(5);
        std::printf("  fixes %s: G[147] %u, G[150..152] %u %u %u\n", fixes ? "on" : "off", g.g(147), g.g(150), g.g(151), g.g(152));
        if (fixes) {
            CHECK_EQ(g.g(150), 0u);
            CHECK_EQ(g.g(151), 0u);
            CHECK_EQ(g.g(152), 0u);
        } else {
            CHECK_EQ(g.g(150), 1u);  // the original: stuck, only Endure Heat can still count
            CHECK_EQ(g.g(151), 1u);
        }
    }
    original_bug_fixes() = true;
}

// --- 3. The Ancient Necromancers answer only to Death Speak (G[40:1] while the spell runs)
void test_necromancers(const GameData& d) {
    std::printf("necromancers: each answers Death Speak once, none without it\n");
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Game g(d);
        g.warp(24127, 29503, 64, 62);
        for (unsigned q = 1; q <= 6; ++q) {
            Item* nec = necromancer(g, q);
            CHECK(nec != nullptr);
            if (!nec) continue;
            CHECK(!plain_use_talks(g, nec));
            g.warp(nec->x + 64, nec->y, nec->z, 62);
            death_speak(g, nec->id);
            std::printf("  fixes %s, necromancer %u: lesson G[%u] %u, G[40] %u after, avatar at %d,%d\n", fixes ? "on" : "off", q,
                        200 + q, g.g(200 + q), g.g(40), g.av().x, g.av().y);
            CHECK_EQ(g.g(200 + q), 1u);
            CHECK_EQ(g.g(40), 0u);
        }
    }
    // Their spells cannot be made before they have taught them (the reason the test could be skipped).
    bool off = make_rock_flesh(d, false, false), untaught = make_rock_flesh(d, true, false), taught = make_rock_flesh(d, true, true);
    std::printf("  Rock Flesh from the bag: untaught, fixes off %d; untaught, fixes on %d; taught, fixes on %d\n", int(off),
                int(untaught), int(taught));
    CHECK(off);        // the original: any recipe works untaught
    CHECK(!untaught);  // fixed: "Something's not right."
    CHECK(taught);
    // The Key of the Scion (frame 3) on a bag: the original only runs the door-and-chest code
    // (KEYRING) and makes nothing; with the patch (scion_bag_spells_*) it makes the spell like the
    // other keys, under the same teaching gates.
    bool scion_off = make_rock_flesh(d, false, true, 3), scion_on = make_rock_flesh(d, true, true, 3),
         scion_untaught = make_rock_flesh(d, true, false, 3);
    std::printf("  Key of the Scion on the bag: fixes off %d, on %d, on but untaught %d\n", int(scion_off), int(scion_on), int(scion_untaught));
    CHECK(!scion_off);
    CHECK(scion_on);
    CHECK(!scion_untaught);
    // Any other target is not a bag, so KEYRING goes on as before: it still opens locked doors and
    // chests (the story replay opens its doors with it, ctest `story`).
    // The way the flag stuck in the original: the spell's process dies while the Necromancer
    // speaks (a map change kills every process but the persistent ones), so it never clears it.
    for (bool fixes : {false, true}) {
        original_bug_fixes() = fixes;
        Game g(d);
        Item* n4 = (g.warp(24127, 29503, 64, 62), necromancer(g, 4));
        CHECK(n4 != nullptr);
        if (!n4) continue;
        g.warp(n4->x + 64, n4->y, n4->z, 62);
        g.av().mana = 50;
        g.w.vm().start(1414, 0x0080, true, 1, {0, 0}, true);
        for (int f = 0; f < 200 && !g.g(40); ++f) play(g, 1, n4->id);
        CHECK_EQ(g.g(40), 1u);          // he is speaking
        g.warp(11168, 11264, 48, 50);   // ... and the Avatar leaves the map (the spell dies)
        g.warp(24127, 29503, 64, 62);
        play(g, 300);
        unsigned stuck = g.g(40);
        Item* n5 = necromancer(g, 5);
        bool talks = n5 && plain_use_talks(g, n5);
        std::printf("  fixes %s: after the map change G[40] %u; the 5th Necromancer talks without the spell: %s\n",
                    fixes ? "on" : "off", stuck, talks ? "yes" : "no");
        CHECK_EQ(stuck, fixes ? 0u : 1u);  // Off: the original's stuck flag, reproduced
        CHECK_EQ(talks, !fixes);
    }
}

struct Absorb { unsigned map = 0; bool stasis = false, hmm = false, kept = false; };

// Absorbs a Titan (shape `titan`) on map `map` with blackrock frame `piece`, the Avatar
// beside it. `prepare` runs first (e.g. the Titan's plane scene).
Absorb absorb(const GameData& d, bool fixes, unsigned map, unsigned titan, unsigned piece, int far_x, int far_y, int far_z,
              bool bypass_lithos_egg, bool near) {
    original_bug_fixes() = fixes;
    Game g(d);
    g.av().set(Actor::kInvincible, true);  // the planes' monsters do not matter here
    g.warp(far_x, far_y, far_z, map);
    if (bypass_lithos_egg)
        for (Item* e : g.items(73)) if (e->quality == 7) e->mapnum |= 0x80;  // as if pushed past it (the wiki's trick)
    if (map == 37)  // Stratos' meeting scene (STRATHAT) is not what is tested here
        for (Item* e : g.items(73)) if (e->quality == 5) e->hatched = true;
    std::vector<Item*> t = g.items(titan);
    Absorb r;
    if (t.empty() && titan == 109) {  // Pyros away from his plane (as the summoning at the pentagram brings him)
        uint16_t p = g.w.create_monster(109, 1);
        g.w.move_to(p, far_x + 256, far_y, far_z);
        t.push_back(g.w.item(p));
    }
    if (t.empty()) return r;
    // The pieces have no range limit; Lithos' plane scene (ENDLITH, he rises) needs the Avatar near.
    if (near) g.w.teleport_actor(1, t[0]->x + 96, t[0]->y + 64, t[0]->z, map);
    play(g, 1500);  // any scene the Titan has when the Avatar comes near
    if (g.w.stasis) std::printf("  (stasis by class %u before the piece is used)\n", unsigned(g.w.stasis_cls));
    uint16_t b = g.w.create_item(442, piece);
    g.w.give_to_avatar(b);
    g.w.call_event(b, kEvUse);
    bool hmm = false;
    for (int f = 0; f < 4000 && g.av().mapnum == map; ++f) {
        play(g, 1, t[0]->id);
        for (const Bark& bk : g.w.barks)
            if (bk.speaker == 1) for (const TextPage& pg : bk.pages) for (const std::string& l : pg.lines) hmm = hmm || l.find("Hmm") != std::string::npos;
        if (hmm && f > 120) break;
    }
    play(g, 120);
    r.map = g.av().mapnum;
    r.stasis = g.w.stasis;
    r.hmm = hmm;
    r.kept = g.w.item(b) && g.w.item(b)->shape == 442;
    return r;
}

// --- 1. Lithos absorbed in the Hall of the Mountain King before the meeting (frozen game)
// --- 2. The Breath of Air used on the present-day Stratos (dead end in the ethereal plane)
void test_titans(const GameData& d) {
    std::printf("titans: Lithos, Stratos and Pyros can be absorbed only on their own planes\n");
    struct Case { const char* what; unsigned map, titan, piece; int x, y, z; bool bypass, near, allowed_on; };
    const Case cases[] = {
        {"Lithos, Hall of the Mountain King, before meeting him", 31, 433, 1, 5215, 4607, 32, true, true, false},
        {"Lithos, Hall of the Mountain King, after meeting him", 31, 433, 1, 5215, 4607, 32, false, true, false},
        {"Lithos, Plane of Earth", 47, 433, 1, 12543, 1503, 56, false, true, true},
        {"Stratos, Argentrock (present day)", 37, 385, 3, 25055, 19903, 80, false, false, false},
        {"Stratos, Plane of Air", 46, 385, 3, 14719, 3295 + 96, 216, false, true, true},
        {"Pyros, away from his plane (Daemon's Crag)", 12, 109, 4, 16063, 9567, 8, false, false, false},
        {"Pyros, Plane of Fire", 44, 109, 4, 23583 + 128, 14879, 24, false, true, true},
    };
    for (const Case& c : cases) {
        for (bool fixes : {false, true}) {
            Absorb r = absorb(d, fixes, c.map, c.titan, c.piece, c.x, c.y, c.z, c.bypass, c.near);
            std::printf("  %-55s fixes %-3s -> map %u, stasis %d, 'Hmm...' %d, piece kept %d\n", c.what, fixes ? "on" : "off", r.map,
                        int(r.stasis), int(r.hmm), int(r.kept));
            bool absorbed = r.map == 43;
            CHECK_EQ(absorbed, fixes ? c.allowed_on : true);  // Off: every one is absorbed (the original)
            CHECK(!r.stasis);
            if (!absorbed) { CHECK(r.hmm); CHECK(r.kept); }
        }
    }
}

// --- 8. Gwillim on later visits: "Hello, my friend." gave nothing but "Farewell"
std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& x : v) out += (out.empty() ? "" : " | ") + x;
    return out;
}
int find_answer(const std::vector<std::string>& m, const std::string& prefix) {
    for (size_t i = 0; i < m.size(); ++i) if (m[i].rfind(prefix, 0) == 0) return int(i);
    return -1;
}
void test_gwillim(const GameData& d) {
    std::printf("gwillim: his topics on a later visit, with the new dialogue entries beside them\n");
    for (bool dialog : {false, true})
        for (bool fixes : {false, true}) {
            original_bug_fixes() = fixes;
            Game g(d);
            if (dialog) {
                std::string err;
                CHECK(g.w.dialog->load_dir(g_data + "/dialog", err));
                g.w.dialog->enabled = true;
            }
            g.set(211, 1, 1);  // met him before
            g.set(89, 1, 1);   // the execution seen
            g.set(86, 1, 1);   // Mordea's trial (a new-dialogue stage with an entry)
            const Actor* gw = g.w.actor(14);
            CHECK(gw != nullptr);
            if (!gw) continue;
            g.warp(gw->x + 96, gw->y, gw->z, gw->mapnum);
            g.w.call_event(14, kEvUse);
            CHECK(wait_menu(g));
            std::vector<std::string> m0 = menu(g);
            int hello = find_answer(m0, "Hello, my friend.");
            CHECK(hello >= 0);
            if (hello < 0) continue;
            g.w.choose_answer(unsigned(hello));
            g.run(10);
            CHECK(wait_menu(g));
            std::vector<std::string> m1 = menu(g);
            int fine = find_answer(m1, "I'm fine, thanks.");
            // New-dialogue entries: in his first menu (and in a menu of exits only), at most two,
            // always before the goodbye.
            auto extras = [](const std::vector<std::string>& m) {
                unsigned n = 0;
                for (const std::string& a : m)
                    n += a.rfind("Farewell", 0) != 0 && a.rfind("I'm fine", 0) != 0 && a.rfind("Hello, my friend", 0) != 0;
                return n;
            };
            std::printf("  dialog %-3s fixes %-3s: first menu: %s\n    after 'Hello': %s\n", dialog ? "on" : "off",
                        fixes ? "on" : "off", joined(m0).c_str(), joined(m1).c_str());
            CHECK_EQ(fine >= 0, fixes);  // Off: the original bug (only "Farewell" and new entries)
            CHECK(m0.back().rfind("Farewell", 0) == 0);
            CHECK(!m1.empty() && m1.back().rfind("Farewell", 0) == 0);  // the goodbye stays last
            CHECK(extras(m0) <= (dialog ? 2u : 0u));
            CHECK(extras(m1) <= (dialog ? 2u : 0u));
            if (dialog) CHECK(extras(m0) + extras(m1) >= 1);
            if (fine < 0) continue;
            g.w.choose_answer(unsigned(fine));
            g.run(10);
            CHECK(wait_menu(g));
            std::vector<std::string> m2 = menu(g);
            std::printf("    his topics: %s\n", joined(m2).c_str());
            CHECK(m2.size() >= 4);  // Corinth, the valley, visitors, ... and Farewell
            CHECK(!m2.empty() && m2.back().rfind("Farewell", 0) == 0);
        }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    std::string mode = argc > 3 ? argv[3] : "";
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    if (mode == "explore") {
        std::vector<unsigned> shapes;
        for (int i = 5; i < argc; ++i) shapes.push_back(unsigned(std::atoi(argv[i])));
        explore(d, unsigned(std::atoi(argc > 4 ? argv[4] : "0")), shapes);
        return 0;
    }
    g_saves = mode.empty() ? "/tmp/pagan_storyfix_saves" : mode;
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    std::string only = argc > 4 ? argv[4] : "";
    auto run = [&](const char* n, auto f) { if (only.empty() || only == n) f(); };
    test_patches(d);  // always: installs the patches the other tests use
    run("book", [&] { test_resurrection_book(d); });
    run("typos", [&] { test_typos(d); });
    run("ancient", [&] { test_ancient_ones(d); });
    run("vividos", [&] { test_vividos(d); });
    run("necro", [&] { test_necromancers(d); });
    run("titans", [&] { test_titans(d); });
    run("gwillim", [&] { test_gwillim(d); });
    run("firsttest", [&] { test_first_test(d); });
    original_bug_fixes() = true;
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
