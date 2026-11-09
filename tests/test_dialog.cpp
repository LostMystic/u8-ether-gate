// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Engine-side topics for original NPCs (game/npc_dialog.*, data/dialog/*.json): the data files
// load cleanly, the story stages and the entries shown in the NPC's own menu for many points of the
// story (newer topics replacing older ones), the hook through the real input path (mouse with the
// click guard, number keys, the controller's A / B / D-pad), the message log, the return to the
// NPC's own menu and its goodbye, combat ending the conversation, saving refused inside a topic and
// a menu saved with an entry still working after a load, the option off, and screenshots.
//   pagan_dialog_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [only]
//   pagan_dialog_tests GAME_DIR DATA_DIR explore [SAVE_DIR] [G W ...]   (NPC=n, WATCH=CLASS, WARP=x,y,z,m)
//   pagan_dialog_tests GAME_DIR DATA_DIR strings:CLASS                  (printable strings of a class)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/ask_nav.h"
#include "game/game_log.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "usecode/uc_machine.h"
#include "usecode/usecode_file.h"

using namespace pg;
int g_fail = 0, g_pass = 0;

namespace {
std::string g_saves, g_data, g_shots;

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    explicit Ui(const GameData& d, bool dialog = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (dialog) {
            if (!w.dialog->load_dir(g_data + "/dialog", err)) std::printf("  dialog: %s\n", err.c_str());
            w.dialog->enabled = true;
        }
        gm.start();
    }
    World& w() { return gm.world(); }
    NpcDialog& nd() { return *gm.world().dialog; }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void ev(InputEvent e) { gm.handle(e); }
    void move(int x, int y) { InputEvent e; e.type = InputEvent::kMouseMove; e.x = x; e.y = y; ev(e); }
    void click(int x, int y) {
        move(x, y);
        InputEvent e; e.type = InputEvent::kMouseDown; e.button = 1; e.x = x; e.y = y; ev(e);
        e.type = InputEvent::kMouseUp; ev(e);
    }
    void key(int k) { InputEvent e; e.type = InputEvent::kKeyDown; e.key = k; ev(e); e.type = InputEvent::kKeyUp; ev(e); }
    void pad(int b) {
        InputEvent e; e.type = InputEvent::kPadDown; e.button = b; e.pad = 7; ev(e);
        e.type = InputEvent::kPadUp; ev(e);
    }
    void pad_added() { InputEvent e; e.type = InputEvent::kPadAdded; e.pad = 7; std::strcpy(e.name, "test pad"); ev(e); }
    std::vector<std::string> menu() {
        std::vector<std::string> out;
        if (w().ask_menu) for (uint16_t sid : w().ask_menu->answers) out.push_back(w().vm().string(sid));
        return out;
    }
    int index_of(const std::string& label) {
        std::vector<std::string> m = menu();
        for (size_t i = 0; i < m.size(); ++i) {
            std::string a = m[i];
            while (!a.empty() && a.back() == ' ') a.pop_back();
            if (a == label) return int(i);
        }
        return -1;
    }
    // Runs until a menu has been up `settle` frames (speech hurried along); false on timeout.
    bool wait_menu(int settle = 6, int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (w().ask_menu && w().ask_menu->frames_open >= settle) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
    bool wait_idle(int max = 6000) {
        for (int f = 0; f < max; ++f) {
            if (!w().ask_menu && !w().in_conversation() && w().barks.empty()) return true;
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 2);
            frames(1);
        }
        return false;
    }
    void near_npc(uint16_t npc) {
        const Actor* g = w().actor(npc);
        w().teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
        frames(30);
    }
    void set(unsigned pos, unsigned n, unsigned v) { w().vm().set_global(pos, n, uint16_t(v)); }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        mkdir(g_shots.c_str(), 0755);
        std::vector<uint32_t> rgba;
        int fw, fh;
        gm.render_frame(rgba, fw, fh, 3, 0.0, 1.0 / 30);
        std::string path = g_shots + "/" + name + ".png";
        save_png(path, rgba.data(), fw, fh, 1);
        std::printf("  shot %s\n", path.c_str());
    }
    bool has_log(const std::string& part, unsigned after = 0) {
        for (const auto& l : w().game_log.lines(GameLog::kMessages))
            if (l.serial > after && l.text.find(part) != std::string::npos) return true;
        return false;
    }
};

std::vector<std::string> labels(const std::vector<const DialogTopic*>& ts) {
    std::vector<std::string> out;
    for (const DialogTopic* t : ts) out.push_back(t->label);
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

// --- The data files
void test_load(const GameData& d) {
    std::printf("data: the dialogue files load, every step / stage / condition is known\n");
    Ui u(d);
    NpcDialog& nd = u.nd();
    CHECK(nd.find_def(14) >= 0);  // Gwillim
    CHECK(nd.find_def(32) >= 0);  // Beren
    std::vector<std::string> p = nd.problems();
    for (const std::string& s : p) std::printf("  problem: %s\n", s.c_str());
    CHECK(p.empty());
    for (const NpcDialogDef& def : nd.defs) {
        if (!def.npc) {  // a file for a kind of actor (children.json: shape 708)
            CHECK(def.shape != 0);
            std::printf("  %s: shape %u, %zu topics\n", def.id.c_str(), def.shape, def.topics.size());
            continue;
        }
        const Actor* a = u.w().actor(uint16_t(def.npc));
        CHECK(a != nullptr);
        std::printf("  %s: npc %u (%s), %zu stages, %zu topics\n", def.id.c_str(), def.npc, a ? a->name.c_str() : "?",
                    def.stages.size(), def.topics.size());
    }
    // Bad JSON is refused with a message.
    std::string err;
    NpcDialog bad(u.w());
    CHECK(!bad.load_text("{ \"npc\": 14, ", err) && !err.empty());
    CHECK(!bad.load_text("{ \"id\": \"x\" }", err));
}

// --- Gwillim's stages and the entries in his menu, through the story
struct Point { const char* what; std::vector<std::pair<int, int>> set; int visit; const char* stage; std::vector<const char*> entries; };

void test_gwillim_stages(const GameData& d) {
    std::printf("gwillim: stages and menu entries through the story (newest replaces older)\n");
    Ui u(d);
    u.frames(10);
    NpcDialog& nd = u.nd();
    int g = nd.find_def(14);
    auto two = [](unsigned pos) { return pos == 52 ? 2 : 1; };
    (void)two;
    const Point pts[] = {
        {"new game", {}, -1, "arrived", {}},
        {"execution", {{89, 1}}, -1, "execution", {"The execution"}},
        {"told of Mythran", {{295, 1}}, -1, "seek_mythran", {"The plateau path"}},
        {"met Mythran", {{284, 1}}, -1, "mythran", {"Mythran"}},
        {"the cemetery", {}, 6, "necromancers", {"The Necromancers"}},
        {"met Lithos", {{42, 1}}, -1, "lithos", {"The Mountain King"}},
        {"Mordea's trial", {{86, 1}}, -1, "mordea", {"Mordea"}},
        {"Lothian at rest", {{209, 1}}, -1, "lothian", {"The cemetery"}},
        {"the Zealan altar", {{75, 1}}, -1, "pilgrimage", {"The Zealans"}},
        {"Argentrock", {}, 37, "titan_tasks", {"The Zealans", "Cyrrus"}},
        {"met Cyrrus", {{116, 1}}, -1, "titan_tasks", {"The Zealans", "Your son"}},
        {"Hydros freed", {{52, 2}}, -1, "titan_tasks", {"Your son", "Hydros"}},
        {"Stratos", {{56, 1}}, -1, "titan_tasks", {"Your son", "The wind"}},
        {"the Pyros ceremony", {{55, 1}}, -1, "titan_tasks", {"Your son", "The fire isle"}},
        {"the Tongue shown", {{65, 1}}, -1, "blackrock", {"Your son", "Blackrock"}},
        {"the ether", {}, 43, "ether", {"Your son", "The Titans"}},
    };
    for (const Point& p : pts) {
        for (auto [pos, v] : p.set) u.set(unsigned(pos), pos == 52 ? 2 : 1, unsigned(v));
        if (p.visit >= 0) u.w().visited_maps.insert(unsigned(p.visit));
        u.frames(1);
        std::vector<std::string> e = labels(nd.entries(g));
        std::printf("  %-20s stage %-13s entries: %s\n", p.what, nd.stage_id(g).c_str(), join(e).c_str());
        CHECK(nd.stage_id(g) == p.stage);
        CHECK_EQ(e.size(), p.entries.size());
        for (const char* l : p.entries) CHECK(has(e, l));
    }
    // A variant: the Necromancer apprentice changes that topic's lines (alt), Malchir dead the fire's.
    Ui v(d);
    v.frames(10);
    v.w().visited_maps.insert(6);
    v.set(213, 1, 1);
    v.frames(1);
    std::vector<const DialogTopic*> e = v.nd().entries(g);
    CHECK(e.size() == 1 && e[0]->id == "necromancers");
}

// Talks to Gwillim with the stage set; returns true when his first menu is up (entries in it).
bool start_gwillim(Ui& u) {
    u.near_npc(14);
    u.w().call_event(14, kEvUse);
    return u.wait_menu();
}

void test_gwillim_talk(const GameData& d) {
    std::printf("gwillim: the entry in his own menu, mouse / keys / pad, back to his menu, goodbye\n");
    Ui u(d);
    u.frames(10);
    u.set(89, 1, 1);   // the execution has been seen
    u.set(86, 1, 1);   // Mordea's trial (everything before it counts as done)
    CHECK(start_gwillim(u));
    std::vector<std::string> m0 = u.menu();
    std::printf("  first menu: %s\n", join(m0).c_str());
    int e = u.index_of("Mordea");
    CHECK(e >= 0);
    int fare = u.index_of("Farewell.");
    CHECK(fare == int(m0.size()) - 1 && e < fare);  // before the exit, which stays last
    CHECK_EQ(ask_exit_index(m0), fare);             // B still says goodbye
    uint16_t own_pid = u.w().ask_menu ? u.w().ask_menu->pid : 0;
    // The mouse: a click on the entry while the menu is fresh is ignored (the click guard).
    u.w().ask_menu->frames_open = 0;
    int x, y;
    CHECK(u.gm.ask_button_point(unsigned(e), x, y));
    u.click(x, y);
    u.frames(1);
    CHECK(!u.nd().active());
    u.frames(8);
    unsigned serial = u.w().game_log.serial(GameLog::kMessages);
    CHECK(u.gm.ask_button_point(unsigned(e), x, y));
    u.click(x, y);
    u.frames(2);
    CHECK(u.nd().active());
    CHECK(!u.w().can_save());
    CHECK(u.has_log("> Mordea", serial));
    CHECK(u.wait_menu());
    std::vector<std::string> m1 = u.menu();
    std::printf("  topic menu: %s\n", join(m1).c_str());
    CHECK(u.has_log("Gwillim: Is it true?", serial));
    CHECK(u.index_of("It is true.") >= 0 && u.index_of("What of Devon?") >= 0 && u.index_of("Something else.") >= 0);
    // A number key picks a sub-answer (keys have no guard).
    int t = u.index_of("It is true.");
    u.key('1' + t);
    u.frames(2);
    CHECK(u.has_log("> It is true.", serial));
    CHECK(u.wait_menu());
    CHECK(u.index_of("It is true.") >= 0);  // the sub-menu again after his lines
    CHECK(u.has_log("Corinth danced", serial));
    // "Something else." puts his own menu back, as it was, with the click guard again.
    int se = u.index_of("Something else.");
    u.key('1' + se);
    for (int f = 0; f < 60 && u.nd().active(); ++f) u.frames(1);
    CHECK(!u.nd().active());
    CHECK(u.w().ask_menu && u.w().ask_menu->pid == own_pid);
    CHECK(u.menu() == m0);
    CHECK(u.w().ask_menu && u.w().ask_menu->frames_open < 4);
    CHECK(u.w().can_save());
    // The controller: A picks the highlighted (first) answer after the guard; pick the entry by D-pad.
    u.pad_added();
    u.frames(2);
    u.pad(kPadY);  // the wake-up press (a phantom pad cannot act)
    u.frames(6);
    for (int i = 0; i < 6 && u.gm.ask_highlight() != e; ++i) { u.pad(kPadRight); u.frames(1); }
    CHECK_EQ(u.gm.ask_highlight(), e);
    u.pad(kPadA);
    u.frames(2);
    CHECK(u.nd().active());
    CHECK(u.wait_menu(16));
    // B in the topic = "Something else." (a back answer) -> his own menu; B again = "Farewell.".
    CHECK_EQ(ask_exit_index(u.menu()), u.index_of("Something else."));
    u.pad(kPadB);
    for (int f = 0; f < 60 && u.nd().active(); ++f) u.frames(1);
    CHECK(!u.nd().active());
    CHECK(u.menu() == m0);
    u.frames(16);
    u.pad(kPadB);
    u.frames(2);
    CHECK(u.has_log("> Farewell.", serial));
    CHECK(u.wait_idle());
    CHECK(!u.w().in_conversation());
    // The next conversation offers the entry again on his first menu.
    u.w().call_event(14, kEvUse);
    CHECK(u.wait_menu());
    CHECK(u.index_of("Mordea") >= 0);
    // Farewell straight away: the usecode ends as always.
    u.key('1' + u.index_of("Farewell."));
    CHECK(u.wait_idle());
}

void test_gwillim_exits_menu(const GameData& d) {
    std::printf("gwillim: the entry also on a menu holding only Farewell.\n");
    Ui u(d);
    u.frames(10);
    u.set(89, 1, 1);
    CHECK(start_gwillim(u));
    std::printf("  first: %s\n", join(u.menu()).c_str());
    // Greet him and go through his own talk until only exits are left.
    for (int k = 0; k < 40; ++k) {
        std::vector<std::string> m = u.menu();
        if (m.size() == 2 && u.index_of("The execution") == 0) break;
        int pick = -1;
        for (size_t i = 0; i < m.size(); ++i)
            if (ask_exit_kind(m[i]) == 0 && m[i] != "The execution") { pick = int(i); break; }
        if (pick < 0) break;
        u.w().choose_answer(unsigned(pick));
        if (!u.wait_menu()) break;
    }
    std::vector<std::string> m = u.menu();
    std::printf("  last: %s\n", join(m).c_str());
    CHECK(m.size() == 2 && u.index_of("The execution") == 0 && u.index_of("Farewell.") == 1);
    u.w().choose_answer(0);
    CHECK(u.wait_menu());
    CHECK(u.nd().active());
    u.shot("gwillim_topic");
    u.w().choose_answer(unsigned(u.index_of("Something else.")));
    for (int f = 0; f < 60 && u.nd().active(); ++f) u.frames(1);
    CHECK(u.index_of("Farewell.") == 1);
    u.w().choose_answer(1);
    CHECK(u.wait_idle());
}

void test_gwillim_combat(const GameData& d) {
    std::printf("gwillim: combat ends the conversation inside a topic\n");
    Ui u(d);
    u.frames(10);
    u.set(89, 1, 1);
    u.set(52, 2, 2);
    CHECK(start_gwillim(u));
    int e = u.index_of("Hydros");
    CHECK(e >= 0);
    u.w().choose_answer(unsigned(e));
    CHECK(u.wait_menu());
    CHECK(u.nd().active());
    unsigned ended = u.w().conversations_ended;
    CHECK(u.w().combat_interrupts_conversation(0, "test fight"));
    CHECK_EQ(u.w().conversations_ended, ended + 1);
    CHECK(!u.nd().active());
    CHECK(!u.w().ask_menu);
    u.frames(30);
    CHECK(!u.w().in_conversation());
    CHECK(!u.w().stasis);
    // And talking again works.
    u.w().call_event(14, kEvUse);
    CHECK(u.wait_menu());
    CHECK(u.index_of("Hydros") >= 0);
}

void test_gwillim_save(const GameData& d) {
    std::printf("gwillim: saves (refused inside a topic; a saved menu with the entry works after a load)\n");
    Ui u(d);
    u.frames(10);
    u.set(89, 1, 1);
    u.set(295, 1, 1);
    CHECK(start_gwillim(u));
    CHECK(u.index_of("The plateau path") >= 0);
    CHECK(u.w().can_save());
    CHECK(u.gm.save_slot(5, "dialog test"));
    Ui v(d);
    CHECK(v.gm.load_slot(5));
    v.frames(2);
    CHECK(v.w().ask_menu != nullptr);
    CHECK(v.index_of("The plateau path") >= 0);
    v.frames(8);
    v.w().choose_answer(unsigned(v.index_of("The plateau path")));
    CHECK(v.nd().active());
    CHECK(v.wait_menu());
    CHECK(v.index_of("Is the way safe?") >= 0);
    CHECK(!v.w().can_save());  // the diary and the autosave ask this
    v.w().choose_answer(unsigned(v.index_of("Something else.")));
    for (int f = 0; f < 60 && v.nd().active(); ++f) v.frames(1);
    CHECK(v.index_of("Farewell.") >= 0);
    v.w().choose_answer(unsigned(v.index_of("Farewell.")));
    CHECK(v.wait_idle());
    // Loading while a topic is up ends it cleanly.
    CHECK(start_gwillim(u));
    u.w().choose_answer(unsigned(u.index_of("The plateau path")));
    CHECK(u.nd().active());
    CHECK(u.gm.load_slot(5));
    u.frames(2);
    CHECK(!u.nd().active());
}

// The controller through a whole conversation, as a player taps it (bug: a press skipped the
// last line of an answer and the menu came back unread). Devon is picked as the auto-target and
// talked to with two taps of A; each menu is answered with the D-pad + A, and his lines are
// advanced with A taps every `every` frames (held `hold` frames). Every page must be on screen
// for at least one press of its own or a full second (PAD_LINES_LOG=1 lists every page).
struct PadLines { int pages = 0, skipped = 0, menus = 0; };
PadLines pad_lines_run(const GameData& d, uint16_t npc, int every, int hold) {
    Ui u(d);
    World& w = u.w();
    u.frames(10);
    u.pad_added();
    u.frames(2);
    u.pad(kPadY);  // the wake-up press
    u.near_npc(npc);
    for (int i = 0; i < 12 && u.gm.auto_target() != npc; ++i) { u.pad(kPadRight); u.frames(1); }
    CHECK_EQ(u.gm.auto_target(), npc);
    PadLines r;
    int presses = 0;
    auto down = [&](bool on) {
        InputEvent e; e.type = on ? InputEvent::kPadDown : InputEvent::kPadUp; e.button = kPadA; e.pad = 7; u.ev(e);
    };
    // Two taps: talk.
    down(true); down(false); u.frames(2); down(true); down(false);
    struct Seen { int press_at = 0; int ticks = 0, first_ticks = 0, at = 0; std::string text; };
    std::map<std::pair<uint16_t, unsigned>, Seen> seen;  // (bark pid, page) -> state
    std::set<std::string> asked;
    int held = 0, since = 0, answers = 0;
    for (int f = 0; f < 9000 && answers < 5; ++f) {
        if (w.ask_menu && !w.ask_menu->answers.empty()) {
            if (held) { down(false); held = 0; }
            if (w.ask_menu->frames_open == 20) {
                // The first answer not asked yet that is not an exit; else stop.
                std::vector<std::string> m = u.menu();
                int pick = -1, ex = ask_exit_index(m);
                for (int i = 0; i < int(m.size()) && pick < 0; ++i)
                    if (i != ex && !asked.count(m[size_t(i)])) pick = i;
                if (pick < 0) break;
                asked.insert(m[size_t(pick)]);
                ++r.menus;
                for (int i = 0; i < 12 && u.gm.ask_highlight() != pick; ++i) u.pad(kPadRight);
                u.pad(kPadA);
                ++answers;
                since = 0;
            }
        } else if (!w.barks.empty()) {
            // Speech: a tap every `every` frames.
            if (held && --held == 0) down(false);
            if (++since >= every) { since = 0; down(true); ++presses; held = std::max(1, hold); if (hold == 0) { down(false); held = 0; } }
        }
        u.frames(1);
        // Pages on screen now; ones gone since last frame either had a press of their own or ran out.
        std::set<std::pair<uint16_t, unsigned>> now;
        for (const Bark& b : w.barks) {
            if (b.speaker == 1) continue;  // the Avatar's own line (his answer) is not the point
            auto k = std::make_pair(b.pid, b.page);
            now.insert(k);
            auto it = seen.find(k);
            if (it == seen.end()) {
                Seen s;
                s.press_at = presses;
                s.first_ticks = b.ticks_left;
                s.at = f;
                for (const auto& l : b.pages[b.page].lines) s.text += l + " ";
                it = seen.emplace(k, s).first;
                ++r.pages;
            }
            it->second.ticks = b.ticks_left;
        }
        for (auto& kv : seen) {
            Seen& s = kv.second;
            if (now.count(kv.first) || s.ticks < 0) continue;
            bool unread = presses == s.press_at && f - s.at < 30;  // gone in under 1 s, untouched
            if (unread) ++r.skipped;
            if (unread || std::getenv("PAD_LINES_LOG"))
                std::printf("    %s after %d frames (%d of %d ticks left, %d presses, stasis %d): %s\n",
                            unread ? "SKIPPED unread" : "ended", f - s.at, s.ticks, s.first_ticks, presses - s.press_at,
                            int(w.stasis), s.text.c_str());
            s.ticks = -1;  // done; kept so a page is counted once
        }
        if (!w.in_conversation() && w.barks.empty() && answers > 0) break;
    }
    if (held) down(false);
    std::printf("  pad through Devon (tap every %d frames, held %d): %d pages, %d skipped unread, %d answers\n",
                every, hold, r.pages, r.skipped, r.menus);
    return r;
}

void test_pad_lines(const GameData& d) {
    std::printf("pad: every line of a conversation is shown for at least one press of A\n");
    // The cause: a page split off at the end of a speech got no time of its own (its characters
    // were counted on the page before), so "forgive me." vanished after 4 frames.
    {
        Ui u(d);
        const std::string text = "I am sorry. I did not mean to pry, but when I found you, I knew not who you were. "
                                 "I am afraid I read through your logbook and discovered your name. Please forgive me.";
        std::vector<TextPage> pages = layout_text(u.w().font(7), text, 194, 55);  // Devon's bark font and box
        unsigned sum = 0;
        bool each = true;
        for (const TextPage& p : pages) { sum += p.chars; each &= p.chars >= 5; }
        std::printf("  layout: %zu pages, %u of %zu characters timed, last page %u\n", pages.size(), sum, text.size(),
                    pages.empty() ? 0 : pages.back().chars);
        CHECK(pages.size() >= 2);
        CHECK_EQ(sum, unsigned(text.size()));
        CHECK(each);
    }
    for (auto [every, hold] : {std::pair{20, 0}, std::pair{20, 4}, std::pair{16, 2}, std::pair{30, 8}, std::pair{45, 3}}) {
        PadLines r = pad_lines_run(d, 2, every, hold);
        CHECK(r.menus >= 2);
        CHECK(r.pages >= 4);
        CHECK_EQ(r.skipped, 0);
    }
}

void test_off(const GameData& d) {
    std::printf("option off: the original menus\n");
    Ui u(d);
    u.nd().enabled = false;
    u.frames(10);
    u.set(89, 1, 1);
    u.set(86, 1, 1);
    CHECK(start_gwillim(u));
    std::printf("  menu: %s\n", join(u.menu()).c_str());
    CHECK(u.index_of("Mordea") < 0);
    CHECK_EQ(u.menu().size(), 2u);
    u.w().choose_answer(1);
    CHECK(u.wait_idle());
    // The option row.
    NpcDialog::register_options();
    bool found = false;
    for (auto& f : World::extra_option_rows()) {
        MenuGump::OptionRow r = f(u.w());
        if (r.label == "New Dialogue:") {
            found = true;
            CHECK(r.value() == "Off");
            r.change(1);
            CHECK(u.nd().enabled && r.value() == "On");
        }
    }
    CHECK(found);
}

// --- Beren
void test_beren_stages(const GameData& d) {
    std::printf("beren: stages and menu entries (hidden while he is angry or grieving)\n");
    Ui u(d);
    u.frames(10);
    NpcDialog& nd = u.nd();
    int b = nd.find_def(32);
    CHECK(b >= 0);
    if (b < 0) return;
    struct P { const char* what; std::vector<std::pair<int, int>> set; int visit; int kill; const char* stage; std::vector<const char*> entries; };
    const P pts[] = {
        {"new game", {}, -1, -1, "", {}},
        {"the Enclave", {{52, 2}}, 12, -1, "enclave", {"Your duties", "The flooded Mouth"}},
        {"a disciple", {{385, 1}}, -1, -1, "disciple", {"The townsfolk", "The flooded Mouth"}},
        {"first test", {{153, 1}}, -1, -1, "trials", {"The townsfolk", "The trials"}},
        {"the ceremony", {{55, 1}}, -1, -1, "ceremony", {"The townsfolk", "The ceremony"}},
        {"Mordea's trial", {{86, 1}}, -1, -1, "ceremony", {"Tenebrae", "The ceremony"}},
        {"Malchir dead", {}, -1, 27, "master", {"The Cabal now", "Malchir"}},
        {"the Tongue shown", {{65, 1}}, -1, -1, "blackrock", {"The Cabal now", "The black stone"}},
        {"the ether", {}, 43, -1, "ether", {"The Cabal now", "The Titans"}},
    };
    for (const P& p : pts) {
        for (auto [pos, v] : p.set) u.set(unsigned(pos), pos == 52 ? 2 : 1, unsigned(v));
        if (p.visit >= 0) u.w().visited_maps.insert(unsigned(p.visit));
        if (p.kill >= 0) if (Actor* a = u.w().actor(uint16_t(p.kill))) { a->set(Actor::kDead, true); a->hp = 0; }
        u.frames(1);
        std::vector<std::string> e = labels(nd.entries(b));
        std::printf("  %-18s stage %-10s entries: %s\n", p.what, nd.stage_id(b).c_str(), join(e).c_str());
        CHECK(nd.stage_id(b) == p.stage);
        CHECK_EQ(e.size(), p.entries.size());
        for (const char* l : p.entries) CHECK(has(e, l));
    }
    u.set(396, 2, 1);  // angry with the Avatar
    CHECK(nd.entries(b).empty());
    u.set(396, 2, 0);
    u.set(142, 2, 1);  // grieving for Bane
    CHECK(nd.entries(b).empty());
    u.set(142, 2, 2);
    CHECK(!nd.entries(b).empty());
}

bool start_beren(Ui& u) {
    u.near_npc(32);
    u.w().call_event(32, kEvUse);
    return u.wait_menu();
}

void test_beren_talk(const GameData& d) {
    std::printf("beren: his entries in his own menu (not on the name question), opinions, back, farewell\n");
    Ui u(d);
    u.frames(10);
    u.set(52, 2, 2);
    u.w().visited_maps.insert(12);
    u.set(385, 1, 1);  // Bane's disciple
    CHECK(start_beren(u));
    std::printf("  first menu: %s\n", join(u.menu()).c_str());
    CHECK(u.index_of("The townsfolk") < 0);  // "My name is ..." has no goodbye: nothing added
    for (int k = 0; k < 6 && u.index_of("Farewell.") < 0; ++k) {
        u.w().choose_answer(0);
        CHECK(u.wait_menu());
    }
    std::printf("  menu: %s\n", join(u.menu()).c_str());
    int t = u.index_of("The townsfolk");
    CHECK(t >= 0 && u.index_of("The flooded Mouth") >= 0);
    CHECK(t < u.index_of("Farewell."));
    std::vector<std::string> own = u.menu();
    unsigned serial = u.w().game_log.serial(GameLog::kMessages);
    u.w().choose_answer(unsigned(t));
    CHECK(u.wait_menu());
    std::printf("  townsfolk: %s\n", join(u.menu()).c_str());
    CHECK(u.index_of("Korick the smith") >= 0 && u.index_of("Devon the fisherman") >= 0);
    CHECK(u.index_of("Bentic the librarian") >= 0);
    u.w().choose_answer(unsigned(u.index_of("Korick the smith")));
    CHECK(u.wait_menu());
    CHECK(u.has_log("Beren: A strong arm", serial));
    u.w().choose_answer(unsigned(u.index_of("Something else.")));
    for (int f = 0; f < 60 && u.nd().active(); ++f) u.frames(1);
    CHECK(u.menu() == own);
    u.w().choose_answer(unsigned(u.index_of("Farewell.")));
    CHECK(u.wait_idle());
    // Talk again (the next day's greeting).
    u.frames(60);
    u.w().call_event(32, kEvUse);
    bool again = u.wait_menu();
    if (!again) { std::printf("  again: no menu; log:\n"); const auto& ls = u.w().game_log.lines(GameLog::kMessages); for (size_t i = ls.size() > 4 ? ls.size() - 4 : 0; i < ls.size(); ++i) std::printf("    | %s\n", ls[i].text.c_str()); }
    CHECK(again);
    std::printf("  again: %s\n", join(u.menu()).c_str());
    int t2 = u.index_of("The townsfolk");
    CHECK(t2 >= 0);
    if (t2 >= 0) {
        u.w().choose_answer(unsigned(t2));
        CHECK(u.wait_menu());
        u.shot("beren_townsfolk");
        u.w().choose_answer(unsigned(u.index_of("Something else.")));
        for (int f = 0; f < 60 && u.nd().active(); ++f) u.frames(1);
    }
    u.w().choose_answer(unsigned(u.index_of("Farewell.")));
    CHECK(u.wait_idle());
}

// The Mouth of the Daemon (map 25): the scene egg (class BERENHCH) plays as the Avatar arrives.
struct Mouth {
    std::vector<std::string> lines;
    std::vector<std::string> scenes;
    bool stasis_after = true, beren_home = false;
};
Mouth run_mouth(const GameData& d, bool dialog, unsigned met, unsigned angry, bool fight = false, const char* shot = nullptr,
               bool topics = true) {
    Ui u(d, dialog);
    u.nd().enabled = topics;
    u.frames(10);
    u.set(52, 2, 2);
    u.set(392, 2, met);
    u.set(396, 2, angry);
    u.w().avatar()->set(Actor::kInvincible, true);  // he arrives over the lava
    unsigned serial = u.w().game_log.serial(GameLog::kMessages);
    u.w().teleport_actor(1, 19740, 26524, 8, 25);
    bool shot_done = false;
    for (int f = 0; f < 2400; ++f) {
        u.frames(1);
        for (Bark& b : u.w().barks) b.ticks_left = std::min(b.ticks_left, 2);
        if (u.w().ask_menu && u.w().ask_menu->frames_open > 6) {
            if (shot && !shot_done) { u.shot(shot); shot_done = true; }
            if (fight) { u.w().combat_interrupts_conversation(0, "test fight"); fight = false; continue; }
            u.w().choose_answer(0);
        }
    }
    Mouth m;
    for (const auto& l : u.w().game_log.lines(GameLog::kMessages))
        if (l.serial > serial && l.text.rfind("Beren: ", 0) == 0) m.lines.push_back(l.text.substr(7));
    m.scenes = u.nd().scenes_played;
    m.stasis_after = u.w().stasis;
    const Actor* b = u.w().actor(32);
    m.beren_home = b && b->mapnum == 12;
    CHECK(!u.nd().active());
    return m;
}

void test_beren_mouth(const GameData& d) {
    std::printf("beren: the Mouth of the Daemon scene in its three states (the bug and the fix)\n");
    auto show = [](const char* what, const Mouth& m) {
        std::printf("  %s: %zu lines%s\n", what, m.lines.size(), m.scenes.empty() ? "" : " (patched)");
        for (const std::string& l : m.lines) std::printf("    | %.110s\n", l.c_str());
    };
    // Never met him: the original's own welcome, no patch.
    Mouth a = run_mouth(d, true, 0, 0);
    show("never met", a);
    CHECK(a.scenes.empty() && a.lines.size() >= 4);
    CHECK(!a.stasis_after && a.beren_home);
    // Met and angry: the original's own hostile version, no patch.
    Mouth b = run_mouth(d, true, 1, 1);
    show("angry", b);
    CHECK(b.scenes.empty() && b.lines.size() >= 3);
    // Met and not angry (never angered, or forgiven): the original says only its parting warning.
    Mouth c0 = run_mouth(d, false, 1, 0);
    show("met, original (the bug)", c0);
    CHECK_EQ(c0.lines.size(), 1u);
    // With the fix: our lines and his three replies first, then the same warning; the scene ends.
    Mouth c = run_mouth(d, true, 1, 0, false, "beren_mouth_fixed");
    show("met, fixed", c);
    CHECK(c.scenes.size() == 1 && c.scenes[0] == "mouth_known");
    CHECK(c.lines.size() >= 5);
    CHECK(!c.lines.empty() && c0.lines.size() == 1 && c.lines.back() == c0.lines[0]);
    CHECK(!c.stasis_after && c.beren_home);
    // The patch is a bug fix: it follows "Original bug fixes", not "New Dialogue".
    Mouth c2 = run_mouth(d, true, 1, 0, false, nullptr, false);
    CHECK(c2.scenes.size() == 1 && c2.lines.size() == c.lines.size());
    original_bug_fixes() = false;
    Mouth c3 = run_mouth(d, true, 1, 0);
    original_bug_fixes() = true;
    show("met, bug fixes off", c3);
    CHECK(c3.scenes.empty() && c3.lines.size() == 1);
    // A fight in the middle of the patch: the script goes on with its own line.
    Mouth f = run_mouth(d, true, 1, 0, true);
    show("met, fight", f);
    CHECK(!f.stasis_after);
    CHECK(!f.lines.empty() && f.lines.back() == c0.lines[0]);
}

void test_shots(const GameData& d) {
    std::printf("shots\n");
    Ui u(d);
    u.frames(10);
    u.set(89, 1, 1);
    u.set(86, 1, 1);
    u.w().visited_maps.insert(37);
    u.set(116, 1, 1);
    CHECK(start_gwillim(u));
    u.frames(10);
    u.shot("gwillim_menu");
    u.w().choose_answer(unsigned(u.index_of("Your son")));
    for (int f = 0; f < 40; ++f) u.frames(1);
    u.shot("gwillim_line");
}
void explore(const GameData& d, const std::vector<std::pair<int,int>>& globals) {
    Ui u(d);
    u.frames(30);
    World& w = u.w();
    // "pos val" sets a width that holds val (2 bits for 2..3, 3 bits for 4..7).
    for (auto [pos, val] : globals) w.vm().set_global(unsigned(pos), val > 3 ? 3 : val > 1 ? 2 : 1, uint16_t(val));
    if (getenv("GOD")) w.avatar()->set(Actor::kInvincible, true);
    if (getenv("KILL")) { Actor* k = w.actor(uint16_t(atoi(getenv("KILL")))); if (k) { k->set(Actor::kDead, true); k->hp = 0; } }
    unsigned NPC = getenv("NPC") ? unsigned(atoi(getenv("NPC"))) : 14u;
    const Actor* g = w.actor(uint16_t(NPC));
    std::printf("gwillim map %u at %d,%d,%d\n", g->mapnum, g->x, g->y, g->z);
    if (getenv("HOURS") && getenv("WARP")) {
        int x, y, z, m; std::sscanf(getenv("WARP"), "%d,%d,%d,%d", &x, &y, &z, &m);
        w.teleport_actor(1, x, y, z, unsigned(m));
        u.frames(30);
    }
    if (getenv("HOURS")) {
        for (unsigned h = 0; h < 24; h += 2) {
            w.set_game_hours(h);
            u.frames(400);
            const Actor* b = w.actor(uint16_t(NPC));
            std::printf("hour %2u: npc %u map %u at %d,%d,%d G402=%u\n", h, NPC, b->mapnum, b->x, b->y, b->z, w.vm().get_global(402, 3));
        }
        return;
    }
    if (getenv("WARP")) {
        int x, y, z, m; std::sscanf(getenv("WARP"), "%d,%d,%d,%d", &x, &y, &z, &m);
        if (getenv("WATCH")) {
            std::string want = getenv("WATCH");
            w.vm().global_watch = [&w, want](uint16_t cls, unsigned pos, unsigned n, bool wr, uint16_t v) {
                if (want != "*" && w.data().usecode.class_name(cls) != want) return;
                std::printf("    G%s[%u:%u]=%u (%s)\n", wr ? "W" : "R", pos, n, v, w.data().usecode.class_name(cls).c_str());
            };
        }
        w.trace_events = getenv("TRACE") != nullptr;
        w.teleport_actor(1, x, y, z, unsigned(m));
        size_t before = 0;
        if (getenv("FINDCLASS")) {
            u.frames(5);
            for (unsigned id = 1; id < w.objects().slots(); ++id) {
                const Item* it = w.item(uint16_t(id));
                if (!it) continue;
                int c = w.usecode_class(*it);
                if ((c >= 0 && w.data().usecode.class_name(unsigned(c)) == getenv("FINDCLASS")) || it->shape == 73)
                    std::printf("item %u shape %u frame %u q %u at %d,%d,%d class %s\n", id, it->shape, it->frame, it->quality, it->x, it->y, it->z, c >= 0 ? w.data().usecode.class_name(unsigned(c)).c_str() : "-");
            }
        }
        if (getenv("HIT")) {
            u.frames(30);
            w.receive_hit(uint16_t(atoi(getenv("HIT"))), 1, 0, 1, 0x4);
        }
        for (int f = 0; f < (getenv("FRAMES") ? atoi(getenv("FRAMES")) : 900); ++f) {
            u.frames(1);
            for (Bark& b : w.barks) b.ticks_left = std::min(b.ticks_left, 2);
            if (w.ask_menu && w.ask_menu->frames_open > 5) {
                std::string all;
                for (size_t i = 0; i < w.ask_menu->answers.size(); ++i) all += w.vm().string(w.ask_menu->answers[i]) + " | ";
                std::printf("  MENU: %s\n", all.c_str());
                w.choose_answer(getenv("PICK") ? unsigned(atoi(getenv("PICK"))) : 0u);
            }
        }
        const auto& ls = w.game_log.lines(GameLog::kMessages);
        for (size_t i = before; i < ls.size(); ++i) std::printf("    | %s\n", ls[i].text.c_str());
        const Actor* b = w.actor(uint16_t(NPC));
        std::printf("npc %u map %u at %d,%d,%d stasis %d\n", NPC, b->mapnum, b->x, b->y, b->z, int(w.stasis));
        return;
    }
    w.teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
    u.frames(30);
    g = w.actor(uint16_t(NPC));
    std::printf("now gwillim at %d,%d,%d avatar %d,%d,%d map %u\n", g->x, g->y, g->z, w.avatar()->x, w.avatar()->y, w.avatar()->z, w.map().map_number());
    std::set<std::string> chosen;
    if (getenv("WATCH")) {
        std::string want = getenv("WATCH");
        w.vm().global_watch = [&w, want](uint16_t cls, unsigned pos, unsigned n, bool wr, uint16_t v) {
            if (want != "*" && w.data().usecode.class_name(cls) != want) return;
            std::printf("    G%s[%u:%u]=%u (%s)\n", wr ? "W" : "R", pos, n, v, w.data().usecode.class_name(cls).c_str());
        };
    }
    for (int talk = 0; talk < (getenv("TALKS") ? atoi(getenv("TALKS")) : 8); ++talk) {
        std::printf("=== talk %d\n", talk);
        size_t before = w.game_log.lines(GameLog::kMessages).size();
        w.call_event(uint16_t(NPC), kEvUse);
        for (int f = 0; f < 20000; ++f) {
            u.frames(1);
            if (!w.barks.empty()) for (Bark& b : w.barks) b.ticks_left = std::min(b.ticks_left, 2);
            if (w.ask_menu && w.ask_menu->frames_open > 5) {
                std::string all; int pick = -1;
                for (size_t i = 0; i < w.ask_menu->answers.size(); ++i) {
                    std::string s = w.vm().string(w.ask_menu->answers[i]);
                    all += s + " | ";
                    if (pick < 0 && !chosen.count(s) && (!getenv("AVOID") || s.find(getenv("AVOID")) == std::string::npos)) pick = int(i);
                }
                if (getenv("PREFER"))
                    for (size_t i = 0; i < w.ask_menu->answers.size(); ++i)
                        if (w.vm().string(w.ask_menu->answers[i]).find(getenv("PREFER")) != std::string::npos && !chosen.count(w.vm().string(w.ask_menu->answers[i]))) { pick = int(i); break; }
                if (pick < 0) pick = int(w.ask_menu->answers.size()) - 1;
                chosen.insert(w.vm().string(w.ask_menu->answers[size_t(pick)]));
                std::printf("  MENU: %s\n", all.c_str());
                w.choose_answer(unsigned(pick));
            }
            if (!w.ask_menu && !w.in_conversation() && w.barks.empty() && f > 30) break;
        }
        const auto& ls = w.game_log.lines(GameLog::kMessages);
        for (size_t i = before; i < ls.size(); ++i) std::printf("    | %s\n", ls[i].text.c_str());
    }
}
}  // namespace

int dump_strings(const GameData& d, const char* cname) {
    const UsecodeFile& uc = d.usecode;
    for (unsigned c = 0; c < uc.class_count(); ++c) {
        if (std::string(cname) != "*" && uc.class_name(c) != cname) continue;
        const uint8_t* p = uc.code(c); uint32_t n = uc.code_size(c);
        std::printf("class %u %s size %u\n", c, cname, n);
        std::string cur;
        for (uint32_t i = 0; i < n; ++i) {
            if (p[i] >= 32 && p[i] < 127) cur += char(p[i]);
            else { if (cur.size() >= 6) std::printf("%s %05x %s\n", uc.class_name(c).c_str(), i, cur.c_str()); cur.clear(); }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    std::string mode = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_dialog_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    if (mode.rfind("strings:", 0) == 0) return dump_strings(d, mode.c_str() + 8);
    if (mode == "explore") {
        std::vector<std::pair<int, int>> gl;
        for (int i = 5; i + 1 < argc; i += 2) gl.push_back({std::atoi(argv[i]), std::atoi(argv[i + 1])});
        explore(d, gl);
        return 0;
    }
    g_shots = mode;
    std::string only = argc > 5 ? argv[5] : "";
    auto run = [&](const char* n, auto f) { if (only.empty() || only == n) f(); };
    run("load", [&] { test_load(d); });
    run("gw_stages", [&] { test_gwillim_stages(d); });
    run("gw_talk", [&] { test_gwillim_talk(d); });
    run("gw_exits", [&] { test_gwillim_exits_menu(d); });
    run("gw_combat", [&] { test_gwillim_combat(d); });
    run("gw_save", [&] { test_gwillim_save(d); });
    run("beren_stages", [&] { test_beren_stages(d); });
    run("beren_talk", [&] { test_beren_talk(d); });
    run("beren_mouth", [&] { test_beren_mouth(d); });
    run("pad_lines", [&] { test_pad_lines(d); });
    run("off", [&] { test_off(d); });
    run("shots", [&] { test_shots(d); });
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
