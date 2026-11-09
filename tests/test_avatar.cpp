// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Known as the Avatar (game/avatar_title.h, NOTES.md "Known as the Avatar"): Devon's celebration
// topic and its mark, the usecode's words for the Avatar rewritten once the mark is set, and the
// child who would not talk to strangers.
#include <algorithm>
#include <cstdio>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/game_log.h"
#include "game/npc_dialog.h"
#include "game/virtues.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "usecode/uc_machine.h"

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

std::string g_data, g_saves;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.virtues->load_dir(g_data, err)) std::printf("  virtues data: %s\n", err.c_str());
        w.virtues->enabled = true;
        gm.start();
        if (!w.dialog->load_dir(g_data + "/dialog", err)) std::printf("  dialog data: %s\n", err.c_str());
        w.dialog->enabled = true;
    }
    World& w() { return gm.world(); }
    Virtues& v() { return *gm.world().virtues; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
};

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
bool has(const std::vector<std::string>& m, const std::string& s) { return std::find(m.begin(), m.end(), s) != m.end(); }
// Lets the talk run out (barks hurried, a leftover menu left by its last answer).
void finish(Ui& u, int max = 900) {
    for (int f = 0; f < max && (u.w().ask_menu || u.w().in_conversation() || u.w().dialog->active()); ++f) {
        if (u.w().ask_menu && !u.w().dialog->active()) {
            std::vector<std::string> m = menu_of(u.w());
            if (!m.empty()) u.w().choose_answer(unsigned(m.size() - 1));
        }
        for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
        u.frames(1);
    }
}
std::string speech(World& w) {
    std::string all;
    for (const GameLog::Line& l : w.game_log.lines(GameLog::kMessages)) all += l.text + "\n";
    return all;
}

// The string literals (push.str) of a usecode class.
std::vector<std::string> literals(const World& w, const std::string& cls_name, unsigned* cls_out = nullptr) {
    const UsecodeFile& uc = w.data().usecode;
    std::vector<std::string> out;
    for (unsigned c = 0; c < uc.class_count(); ++c) {
        if (uc.class_name(c) != cls_name) continue;
        if (cls_out) *cls_out = c;
        const uint8_t* code = uc.code(c);
        uint32_t size = uc.code_size(c);
        for (uint32_t i = 0x80; code && i + 3 < size; ++i) {
            if (code[i] != 0x0D) continue;
            unsigned n = unsigned(code[i + 1] | (code[i + 2] << 8));
            if (n == 0 || i + 3 + n >= size || code[i + 3 + n] != 0) continue;
            std::string s(reinterpret_cast<const char*>(code + i + 3), n);
            bool text = true;
            for (char ch : s) text = text && ((unsigned char)ch >= 32 || ch == '\t' || ch == '\n');
            if (!text) continue;
            out.push_back(s);
            i += 3 + n;
        }
    }
    return out;
}

// Devon (NPC 2): the celebration shows once he rules and the virtues are high; only "The Avatar."
// sets the mark; other answers leave it open.
void test_devon(const GameData& d) {
    std::printf("devon: the celebration and the name\n");
    Ui u(d);
    u.w().avatar_name = "Hawk";
    u.frames(5);
    NpcDialog& nd = *u.w().dialog;
    int def = nd.find_def(2);
    CHECK(def >= 0);
    if (def < 0) return;
    auto shown = [&](const char* id) {
        for (const DialogTopic* t : nd.entries(def)) if (t->id == id) return true;
        return false;
    };
    CHECK(!shown("celebrate"));
    u.v().set(kJustice, 40);
    u.frames(2);
    CHECK(!shown("celebrate"));  // high virtues, but Devon is still in prison
    u.v().set(kJustice, 0);
    u.w().vm().set_global(86, 1, 1);  // the trial: Devon rules
    u.frames(2);
    CHECK(!shown("celebrate"));  // rules, but the virtues are low
    u.v().set(kJustice, 20);
    u.v().set(kCompassion, 15);
    u.frames(2);
    CHECK(u.v().total() == 35);
    CHECK(shown("celebrate"));
    u.v().enabled = false;
    CHECK(!shown("celebrate"));  // behind the Virtues option
    u.v().enabled = true;

    // A real conversation with him.
    Actor* dv = u.w().actor(2);
    CHECK(dv != nullptr);
    if (!dv) return;
    u.w().teleport_actor(1, dv->x + 96, dv->y, dv->z, dv->mapnum);
    u.frames(30);
    dv = u.w().actor(2);
    u.w().teleport_actor(2, u.w().avatar()->x - 96, u.w().avatar()->y, u.w().avatar()->z, u.w().avatar()->mapnum);
    u.frames(10);
    auto open = [&]() {
        u.w().call_event(2, kEvUse);
        for (int n = 0; n < 25; ++n) {
            if (!wait_menu(u, 1500)) { u.w().call_event(2, kEvUse); continue; }
            std::vector<std::string> m = menu_of(u.w());
            if (has(m, "You sent for me?")) return pick(u, "You sent for me?") && wait_menu(u);
            u.w().choose_answer(0);
            u.frames(2);
        }
        std::printf("  Devon never offered the celebration\n");
        return false;
    };
    // His own name, then not now: no mark, still offered.
    CHECK(open());
    std::vector<std::string> m = menu_of(u.w());
    CHECK(has(m, "The Avatar.") && has(m, "Just Hawk.") && has(m, "I would rather not be known at all."));
    CHECK(pick(u, "Just Hawk.") && wait_menu(u));
    CHECK(has(menu_of(u.w()), "The Avatar."));
    CHECK(pick(u, "Another time, Devon."));
    finish(u);
    CHECK(!u.v().marked("known_avatar"));
    CHECK(shown("celebrate"));
    // Now the title.
    CHECK(open());
    CHECK(pick(u, "The Avatar."));
    finish(u);
    CHECK(u.v().marked("known_avatar"));
    CHECK_EQ(u.v().get(kHonor), 2);
    CHECK(!shown("celebrate"));
    CHECK(shown("feast"));
    CHECK(speech(u.w()).find("Nobody in this city will call you stranger again, Avatar.") != std::string::npos);
    // The epilogue has its card.
    bool card = false;
    for (const std::string& l : u.v().epilogue_page()) card = card || l.find("Devon had it cried from the palace steps") != std::string::npos;
    CHECK(card);
}

// The usecode's own words for the Avatar (data/avatar_title.json) on real lines of the game.
void test_words(const GameData& d) {
    std::printf("words: 'stranger' becomes 'Avatar' after the mark\n");
    Ui u(d);
    u.frames(5);
    World& w = u.w();
    NpcDialog& nd = *w.dialog;
    CHECK(!nd.title.empty());
    CHECK(bool(w.vm().text_hook));
    auto through = [&](unsigned cls, std::string s) { w.vm().text_hook(uint16_t(cls), s); return s; };
    unsigned salkind = 0, rhian = 0, beren = 0, tarna = 0;
    std::vector<std::string> sl = literals(w, "SALKIND", &salkind), rl = literals(w, "RHIAN", &rhian);
    std::vector<std::string> bl = literals(w, "BEREN", &beren), tl = literals(w, "TARNA", &tarna);
    CHECK(has(sl, "Greetings stranger. "));
    CHECK(has(rl, "Stranger, are you lost? "));
    CHECK(has(bl, "I am a stranger here, please forgive me. "));
    // No mark: the original text.
    CHECK(through(salkind, "Greetings stranger. ") == "Greetings stranger. ");
    w.virtues->set_mark("known_avatar");
    CHECK(through(salkind, "Greetings stranger. ") == "Greetings, Avatar. ");
    CHECK(through(salkind, "Good day to you, stranger. ") == "Good day to you, Avatar. ");
    CHECK(through(rhian, "Stranger, are you lost? ") == "Avatar, are you lost? ");
    CHECK(through(rhian, "What do you want here, stranger? ") == "What do you want here, Avatar? ");
    CHECK(through(tarna, "'Tis good to hear, stranger. I am certain that will not change. Do not prove me wrong, stranger. ") ==
          "'Tis good to hear, Avatar. I am certain that will not change. Do not prove me wrong, Avatar. ");
    // The Avatar's own answer and a line about strangers in general stay.
    CHECK(through(beren, "I am a stranger here, please forgive me. ") == "I am a stranger here, please forgive me. ");
    CHECK(through(beren, "I don't talk to nosy strangers. ") == "I don't talk to nosy strangers. ");
    CHECK(through(beren, "stranger ") == "Avatar ");  // the word Beren joins in place of a name
    // Another class's line is not touched by these rules.
    CHECK(through(beren, "Greetings, stranger. ") == "Greetings, stranger. ");
    // Every rule finds its phrase in its class's real lines; count the lines rewritten.
    int changed = 0, rules = 0;
    for (const auto& kv : nd.title.classes) {
        unsigned c = 0;
        std::vector<std::string> lits = literals(w, kv.first, &c);
        CHECK(!lits.empty());
        for (const AvatarTitle::Rule& r : kv.second) {
            ++rules;
            bool found = false;
            for (const std::string& s : lits) {
                std::string a = s;
                while (!a.empty() && a.back() == ' ') a.pop_back();
                bool cap = false;
                found = found || (r.exact ? a == r.find : AvatarTitle::find_phrase(s, r.find, r.word, 0, cap) != std::string::npos);
            }
            if (!found) std::printf("  rule not found in %s: \"%s\"\n", kv.first.c_str(), r.find.c_str());
            CHECK(found);
        }
        for (const std::string& s : lits) if (through(c, s) != s) ++changed;
    }
    std::printf("  %d rules, %d usecode lines rewritten\n", rules, changed);
    CHECK(changed >= 80);
    // NPC lines say the title for the Avatar's name; the Avatar's own lines do not.
    w.avatar_name = "Hawk";
    CHECK(nd.speech(2, "Greetings once again, Hawk. ") == "Greetings once again, Avatar. ");
    CHECK(nd.speech(1, "I am Hawk. ") == "I am Hawk. ");
    CHECK(nd.speech(2, "Hawksbill. ") == "Hawksbill. ");
    // Behind the Virtues option.
    w.virtues->enabled = false;
    CHECK(through(salkind, "Greetings stranger. ") == "Greetings stranger. ");
    CHECK(nd.speech(2, "Greetings once again, Hawk. ") == "Greetings once again, Hawk. ");
}

// The child who would not talk to strangers (class CHILD, shape 708): the use script refuses at
// random ("I don't know you." ... "My mommie told me never to talk to strangers."). With the mark
// the scene avatar_friend takes those lines' place: the lost comb, Humility 4.
void test_child(const GameData& d) {
    for (int with_mark = 0; with_mark < 2; ++with_mark) {
        std::printf("child: %s\n", with_mark ? "known as the Avatar: she talks" : "a stranger: she refuses");
        Ui u(d);
        u.frames(5);
        u.w().vm().set_global(89, 1, 1);
        if (with_mark) u.v().set_mark("known_avatar");
        u.w().teleport_actor(1, 16607 + 128, 18687 + 128, 48, 41);  // a child's egg in Tenebrae
        uint16_t kid = 0;
        for (int f = 0; f < 600 && !kid; ++f) {
            u.frames(1);
            for (unsigned id = 1; id < 256 && !kid; ++id)
                if (const Actor* a = u.w().actor(uint16_t(id)); a && a->shape == 708 && a->mapnum == 41) kid = uint16_t(id);
        }
        CHECK(kid != 0);
        if (!kid) continue;
        NpcDialog& nd = *u.w().dialog;
        bool refused = false, scene = false;
        // The refusal is a coin toss each time: talk until it comes up.
        for (int tries = 0; tries < 40 && !refused && !scene; ++tries) {
            nd.scenes_played.clear();
            u.w().call_event(kid, kEvUse);
            for (int f = 0; f < 1500; ++f) {
                for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
                u.frames(1);
                if (has(nd.scenes_played, "avatar_friend")) { scene = true; break; }
                if (speech(u.w()).find("My mommie told me never to talk to strangers.") != std::string::npos) { refused = true; break; }
                if (u.w().ask_menu && !nd.active()) { finish(u); break; }  // she talked anyway: bye
                if (!u.w().in_conversation() && f > 200 && !u.w().ask_menu) break;
            }
            finish(u);
        }
        if (!with_mark) {
            CHECK(refused);
            CHECK(!scene);
            continue;
        }
        CHECK(scene && !refused);
        if (!scene) continue;
        CHECK(wait_menu(u));
        CHECK(speech(u.w()).find("You're the Avatar!") != std::string::npos);
        std::vector<std::string> m = menu_of(u.w());
        CHECK(has(m, "Of course. What is it?") && has(m, "Bye!"));
        CHECK(pick(u, "Of course. What is it?") && wait_menu(u));
        CHECK(pick(u, "Lead the way. You know this city better than I do.") && wait_menu(u));
        CHECK(pick(u, "Lift the stone, and let the child reach in."));
        finish(u);
        CHECK_EQ(u.v().get(kHumility), 4);
        CHECK(u.v().marked("kids_comb"));
        // The rest of her refusal stayed unsaid, and her script ended.
        CHECK(speech(u.w()).find("My mommie told me never") == std::string::npos);
        CHECK(speech(u.w()).find("I can't talk to you.") == std::string::npos);
        CHECK(!nd.active());
        bool card = false;
        for (const std::string& l : u.v().epilogue_page()) card = card || l.find("showed the Avatar the way") != std::string::npos;
        CHECK(card);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_saves = argc > 3 ? argv[3] : "/tmp/pagan_avatar_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    std::string only = argc > 4 ? argv[4] : "";
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("words")) test_words(d);
    if (want("devon")) test_devon(d);
    if (want("child")) test_child(d);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
