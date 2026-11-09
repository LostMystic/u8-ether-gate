// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The spellbook (game/spellbook.h, NOTES.md "Spellbook"): spells entered when made or cast the
// normal way (not from the book), the casts shown from talismans and reagents, casting from the
// book (talismans first, then reagents; refused without mana or ingredients), the debug option
// (everything, free, and the real book untouched), saves, the window (G, double-click), and the
// two small debug / bug-fix changes made alongside it (companions sent home, the catacomb lever).
//   pagan_spellbook_tests GAME_DIR DATA_DIR [SHOT_DIR] [SAVE_DIR] [ONLY]
//   ONLY: data, necro, theurgy, sorcery, thaum, debug, saves, ui, home, lever, loot
// (loot: the double-click take, TAKE ALL and the reasons beside the red cross, added with it;
//  scrolls, combat: the book's Scrolls section and the turn-based Cast list; rock: Rock Flesh remade.)
#include <algorithm>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/rock_flesh.h"
#include "game/turn_combat.h"
#include "game/processes.h"
#include "game/spellbook.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "usecode/uc_machine.h"

using namespace pg;
int g_fail = 0, g_pass = 0;

namespace {
std::string g_shots, g_saves, g_data;
const int kX = 14300, kY = 6150;  // open ground on the beach map (as in the servant tests)

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    std::vector<uint16_t> picks;  // answers to the next target cursors (then: the Avatar)
    explicit Ui(const GameData& d, bool party = false) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        gm.saves().set_dir(g_saves);
        if (party) {
            std::string err;
            if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
            w.party->beach_always = true;
            w.party->enabled = true;
        }
        gm.start();
        frames(60);
        w.kernel().kill(1, kProcAny);
        w.move_to(1, kX, kY, 0, false);
        frames(10);
        av()->intel = 40;
        av()->mana = 60;
    }
    World& w() { return gm.world(); }
    Spellbook& sb() { return *gm.world().spellbook; }
    Actor* av() { return w().avatar(); }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            for (Bark& b : w().barks) b.ticks_left = std::min(b.ticks_left, 20);
            for (auto& g : w().gumps.list)
                if (std::string(g->kind()) == "reader" && !g->closed) g->close();
            if (w().target_pid) {
                uint16_t t = 1;
                if (!picks.empty()) { t = picks.front(); picks.erase(picks.begin()); }
                if (Process* p = w().kernel().get(w().target_pid)) { p->result = t; p->terminate(); }
                w().target_pid = 0;
            }
            gm.tick();
        }
    }
    void render() { gm.render(surf); }
    uint16_t give(unsigned shape, unsigned frame, unsigned q = 0) {
        uint16_t id = w().create_item(shape, frame);
        w().item(id)->quality = uint16_t(q);
        if (!w().give_to_avatar(id)) { std::printf("  give %u:%u failed\n", shape, frame); return 0; }
        return id;
    }
    int count(unsigned shape, int frame = -1) {
        std::vector<uint16_t> inv;
        sb().inventory(inv);
        int n = 0;
        for (uint16_t id : inv)
            if (const Item* it = w().item(id); it && it->alive && it->shape == shape && (frame < 0 || it->frame == unsigned(frame)))
                n += (shape == 395 || shape == 398) ? std::max<int>(1, it->quality) : 1;
        return n;
    }
    uint16_t find(unsigned shape, unsigned frame) {
        std::vector<uint16_t> inv;
        sb().inventory(inv);
        for (uint16_t id : inv)
            if (const Item* it = w().item(id); it && it->alive && it->shape == shape && it->frame == frame) return id;
        return 0;
    }
    bool knows(int school, int num) { return sb().known.count(school * 1000 + num) > 0; }
    const Spellbook::Spell& spell(int school, int num) { return *Spellbook::find(school * 1000 + num); }
    void key(int k) {
        InputEvent e;
        e.type = InputEvent::kKeyDown; e.key = k; gm.handle(e);
        e.type = InputEvent::kKeyUp; gm.handle(e);
    }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        render();
        std::vector<uint32_t> rgba;
        surf.to_rgba(w().data().palette, gm.palette(), rgba);
        mkdir(g_shots.c_str(), 0755);
        save_png(g_shots + "/" + name + ".png", rgba.data(), 320, 200, 2);
    }
};

void test_data() {
    std::printf("data\n");
    int per[Spellbook::kSchools] = {};
    for (const auto& s : Spellbook::spells()) ++per[s.school];
    CHECK_EQ(per[Spellbook::kNecromancy], 9);
    CHECK_EQ(per[Spellbook::kTheurgy], 9);
    CHECK_EQ(per[Spellbook::kSorcery], 12);
    CHECK_EQ(per[Spellbook::kThaumaturgy], 6);
    CHECK_EQ(per[Spellbook::kTempest], 0);
    CHECK_EQ(per[Spellbook::kScrolls], 3);
    CHECK(Spellbook::find(Spellbook::kSorcery * 1000 + 4) && std::string(Spellbook::find(4004 - 2000)->name) == "Flame Bolt");
    CHECK_EQ(Spellbook::reagent_kind(Spellbook::kNecromancy, 395, 14), 5);
    CHECK_EQ(Spellbook::reagent_kind(Spellbook::kSorcery, 398, 19), 5);
}

void test_necro(const GameData& d) {
    std::printf("necro\n");
    Ui u(d);
    CHECK(u.sb().listed().empty());
    // A token found and never used: not in the book.
    u.give(746, 1);
    u.frames(5);
    CHECK(!u.knows(0, 1));
    // Making Death Speak from a bag with the Key of the Caretaker enters it.
    uint16_t key = u.give(232, 0);
    uint16_t bag = u.w().create_item(637, 0);
    u.w().move_to_container(bag, u.w().equipped(1, 7));
    for (unsigned f : {0u, 6u}) {
        uint16_t r = u.w().create_item(395, f);
        u.w().item(r)->quality = 1;
        u.w().move_to_container(r, bag);
    }
    u.picks = {bag};
    u.w().call_event(key, kEvUse);
    u.frames(200);
    CHECK_EQ(u.count(746, 0), 1);
    CHECK(u.knows(0, 0));
    // Mask of Death cast the normal way (the found token) enters it once the mana is spent.
    u.w().call_event(u.find(746, 1), kEvUse);
    u.frames(300);
    CHECK(u.knows(0, 1));
    // Counts: one Death Speak token plus reagents for two more (3 blood, 2 bone).
    u.give(395, 0, 3);
    u.give(395, 6, 2);
    Spellbook::Status st = u.sb().status(u.spell(0, 0));
    CHECK_EQ(st.talismans, 1);
    CHECK_EQ(st.reagent_casts, 2);
    CHECK_EQ(st.casts, 3);
    CHECK(st.castable);
    // The book casts the token first, the reagents untouched.
    std::string why;
    int m0 = u.av()->mana;
    CHECK(u.sb().cast(u.spell(0, 0), why));
    u.frames(300);
    CHECK_EQ(u.count(746, 0), 0);
    CHECK_EQ(u.count(395, 0), 3);
    CHECK(u.av()->mana < m0);
    // Then the reagents: one set used up.
    CHECK(u.sb().cast(u.spell(0, 0), why));
    u.frames(300);
    CHECK_EQ(u.count(395, 0), 2);
    CHECK_EQ(u.count(395, 6), 1);
    CHECK_EQ(u.sb().status(u.spell(0, 0)).casts, 1);
    // No mana: refused, nothing used.
    u.av()->mana = 0;
    st = u.sb().status(u.spell(0, 0));
    CHECK(!st.castable);
    CHECK_EQ(st.mana_casts, 0);
    CHECK(!u.sb().cast(u.spell(0, 0), why));
    CHECK(why.find("mana") != std::string::npos);
    CHECK_EQ(u.count(395, 0), 2);
    // No reagents and no token: refused.
    u.av()->mana = 60;
    CHECK(!u.sb().cast(u.spell(0, 1), why));
    CHECK(why == "No reagents");
    // Casting from the book never enters a spell by itself (Rock Flesh unknown, reagents carried).
    u.give(395, 8, 1);
    u.give(395, 9, 1);
    CHECK(!u.knows(0, 2));
}

void test_theurgy(const GameData& d) {
    std::printf("theurgy\n");
    Ui u(d);
    uint16_t focus = u.give(396, 8);  // Divination
    // With no mana the cast fizzles: not entered.
    u.av()->mana = 0;
    u.w().call_event(focus, kEvUse);
    u.frames(200);
    CHECK(!u.knows(1, 8));
    u.av()->mana = 60;
    u.w().call_event(focus, kEvUse);
    u.frames(200);
    CHECK(u.knows(1, 8));
    Spellbook::Status st = u.sb().status(u.spell(1, 8));
    CHECK(st.focus);
    CHECK_EQ(st.casts, -1);
    CHECK_EQ(st.mana_casts, u.av()->mana / 3);
    std::string why;
    int m0 = u.av()->mana;
    CHECK(u.sb().cast(u.spell(1, 8), why));
    u.frames(200);
    CHECK(u.av()->mana < m0);
    CHECK(u.w().item(focus) && u.w().item(focus)->alive);  // foci are not used up
    // Without the focus: listed, not castable.
    u.w().destroy(focus, false);
    u.frames(2);
    st = u.sb().status(u.spell(1, 8));
    CHECK(!st.castable);
    CHECK(st.note == "No focus");
}

void test_sorcery(const GameData& d) {
    std::printf("sorcery\n");
    Ui u(d);
    u.w().vm().set_global(371, 1, 1);  // Vardion's deal
    uint16_t pack = u.w().equipped(1, 7);
    uint16_t f = u.give(397, 0, (1 << 8) | 3);  // an Ignite wand, 3 charges, in the backpack
    // Cast the normal way (held): entered.
    u.w().move_to_container(f, 1);
    u.w().call_event(f, kEvUse);
    u.frames(300);
    CHECK(u.knows(2, 1));
    CHECK_EQ(u.w().item(f)->quality & 0xFF, 2);
    u.w().move_to_container(f, pack);
    // From the book: the focus is held for the cast and goes back in the backpack.
    Spellbook::Status st = u.sb().status(u.spell(2, 1));
    CHECK_EQ(st.talismans, 2);
    std::string why;
    CHECK(u.sb().cast(u.spell(2, 1), why));
    u.frames(300);
    CHECK_EQ(u.w().item(f)->quality & 0xFF, 1);
    CHECK_EQ(u.w().item(f)->parent, pack);
    // Reagents: ash 1, pumice 2 per cast; the charge first, then the reagents (with the mana).
    u.give(398, 0, 2);
    u.give(398, 2, 4);
    st = u.sb().status(u.spell(2, 1));
    CHECK_EQ(st.casts, 3);
    CHECK(u.sb().cast(u.spell(2, 1), why));
    u.frames(300);
    CHECK_EQ(u.w().item(f)->quality & 0xFF, 0);
    CHECK_EQ(u.count(398, 0), 2);
    int m0 = u.av()->mana;
    CHECK(u.sb().cast(u.spell(2, 1), why));
    u.frames(300);
    CHECK_EQ(u.count(398, 0), 1);
    CHECK_EQ(u.count(398, 2), 2);
    CHECK(u.av()->mana <= m0 - 2);
    CHECK_EQ(u.count(397, 12), 0);  // the temporary focus is gone
    // Too little mana for the enchanting: refused, reagents kept.
    u.av()->mana = 1;
    CHECK(!u.sb().cast(u.spell(2, 1), why));
    CHECK_EQ(u.count(398, 0), 1);
}

void test_thaum(const GameData& d) {
    std::printf("thaum\n");
    Ui u(d);
    uint16_t book = u.give(288, 0, 81);
    u.frames(2);
    CHECK(u.sb().listed().empty());
    u.w().destroy(book, false);
    book = u.give(288, 11, 81);  // a charged Confusion Blast
    CHECK_EQ(u.sb().status(u.spell(3, 81)).casts, 1);
    u.w().call_event(book, kEvUse);
    u.frames(300);
    CHECK(u.knows(3, 81));
}

void test_debug(const GameData& d) {
    std::printf("debug\n");
    Ui u(d);
    u.sb().learn(1000 + 8);
    const size_t real = u.sb().listed().size();
    CHECK_EQ(real, 1u);
    u.sb().debug_infinite = true;
    CHECK_EQ(u.sb().listed().size(), Spellbook::spells().size());
    const int m0 = u.av()->mana, int0 = u.av()->intel;
    std::string why;
    const size_t items0 = [&] { std::vector<uint16_t> v; u.sb().inventory(v); return v.size(); }();
    for (int key : {0 * 1000 + 2, 1000 + 9, 2000 + 4, 3000 + 81}) {
        u.picks = {1};
        CHECK(u.sb().cast(*Spellbook::find(key), why));
        u.frames(400);
    }
    u.frames(200);
    CHECK_EQ(u.av()->mana, m0);
    CHECK_EQ(u.av()->intel, int0);
    CHECK_EQ([&] { std::vector<uint16_t> v; u.sb().inventory(v); return v.size(); }(), items0);  // no temporary items left
    CHECK_EQ(u.sb().known.size(), 1u);  // debug casts enter nothing
    CHECK_EQ(u.w().vm().get_global(371, 1), 0);  // the story flag lent for the sorcery cast is back
    u.sb().debug_infinite = false;
    CHECK_EQ(u.sb().listed().size(), real);
}

void test_saves(const GameData& d) {
    std::printf("saves\n");
    {
        Ui u(d);
        u.sb().learn(4);
        u.sb().learn(2012);
        CHECK(u.gm.save_slot(3, "spellbook"));
    }
    Ui u(d);
    CHECK(u.sb().known.empty());
    CHECK(u.gm.load_slot(3));
    CHECK_EQ(u.sb().known.size(), 2u);
    CHECK(u.knows(0, 4) && u.knows(2, 12));
}

void test_ui(const GameData& d) {
    std::printf("ui\n");
    Ui u(d);
    for (int k : {0, 1, 3, 4, 7}) u.sb().learn(k);
    u.sb().learn(1008);
    u.sb().learn(1013);
    u.sb().learn(2004);
    u.give(746, 0);
    u.give(746, 0);
    u.give(395, 0, 4);
    u.give(395, 6, 1);
    u.give(396, 8);
    u.key('g');
    u.frames(2);
    auto* g = u.w().gumps.find<SpellbookGump>();
    CHECK(g != nullptr);
    if (!g) return;
    CHECK(u.w().gumps.top_modal() == g);
    CHECK(g->per_page >= 3 && g->per_page <= 4);
    CHECK(!g->entries.empty());
    CHECK_EQ(g->pages.front().school, int(Spellbook::kNecromancy));
    u.shot("spellbook_necromancy");
    g->turn(1);
    u.frames(1);
    u.shot("spellbook_spread2");
    g->turn(-1);
    u.frames(1);
    // Double-click Death Speak: the book closes and the spell is cast from a token.
    const SpellbookGump::Entry e = g->entries.front();
    CHECK_EQ(e.spell->key(), 0);
    g->double_click(1, e.x + 30, e.y + 5);
    u.frames(300);
    CHECK(u.w().gumps.find<SpellbookGump>() == nullptr);
    CHECK_EQ(u.count(746, 0), 1);
    // Refused (no mana): the book stays open and says why.
    u.av()->mana = 0;
    u.key('g');
    u.frames(2);
    g = u.w().gumps.find<SpellbookGump>();
    CHECK(g != nullptr);
    if (!g) return;
    g->selected = 0;
    CHECK(!g->cast_selected());
    CHECK(g->message.find("mana") != std::string::npos);
    u.frames(1);
    u.shot("spellbook_no_mana");
    u.key(kKeyEscape);
    u.frames(2);
    CHECK(u.w().gumps.find<SpellbookGump>() == nullptr);
    u.sb().debug_infinite = true;
    u.key('g');
    u.frames(2);
    u.shot("spellbook_debug");
    g = u.w().gumps.find<SpellbookGump>();
    CHECK(g && g->spreads() >= 6);
}

void test_home(const GameData& d) {
    std::printf("home\n");
    Ui u(d, true);
    Party& p = *u.w().party;
    p.spawn_here();
    for (size_t i = 0; i < p.defs.size() && i < 3; ++i) p.join(int(i));
    u.frames(30);
    CHECK(!p.members.empty());
    const int n = p.send_all_home();
    u.frames(30);
    CHECK(n > 0);
    CHECK(p.members.empty());
    int home = 0;
    for (size_t i = 0; i < p.state.size(); ++i) {
        unsigned map; int pos[3], dir;
        const Actor* a = u.w().actor(p.state[i].id);
        if (!a || !p.resting_place(int(i), map, pos, dir, true)) continue;
        if (a->mapnum == map && std::abs(a->x - pos[0]) < 200 && std::abs(a->y - pos[1]) < 200) ++home;
    }
    CHECK_EQ(home, n);
}

void test_lever(const GameData& d) {
    std::printf("lever\n");
    // The catacomb lever's screen flip (intrinsic 0x0C8) is sparkles now, with bug fixes on.
    Ui u(d);
    original_bug_fixes() = true;
    screen_invert_effect(u.w(), true);
    u.frames(10);
    CHECK_EQ(u.w().invert_state, 0);
    int sparks = 0;
    for (uint16_t id = 512; id < 32767; ++id)
        if (const Item* it = u.w().item(id); it && it->alive && it->shape == 587) ++sparks;
    CHECK(sparks > 0);
    u.shot("lever_sparkles");
    original_bug_fixes() = false;
    screen_invert_effect(u.w(), true);
    u.frames(40);
    CHECK_EQ(u.w().invert_state, 0x8000);
    original_bug_fixes() = true;
}

void test_loot(const GameData& d) {
    std::printf("loot\n");
    Ui u(d);
    World& w = u.w();
    const uint16_t pack = w.equipped(1, 7);
    // A bag on the ground beside the Avatar, with three things in it.
    uint16_t bag = w.create_item(637, 0);
    w.move_to(bag, kX + 40, kY, 0, false);
    std::vector<uint16_t> things;
    for (unsigned sh : {395u, 398u, 746u}) {
        uint16_t t = w.create_item(sh, 0);
        w.item(t)->quality = 1;
        w.move_to_container(t, bag);
        things.push_back(t);
    }
    w.use_item(bag);
    u.frames(5);
    auto* g = dynamic_cast<ContainerGump*>(w.gumps.container_gump(bag));
    CHECK(g != nullptr);
    if (!g) return;
    CHECK(g->foreign());
    int bx, by, bw, bh;
    CHECK(g->take_button(bx, by, bw, bh));
    u.shot("loot_bag");
    // Double-click an item: into the backpack.
    int px = -1, py = -1;
    for (int yy = 0; yy < g->h && px < 0; ++yy)
        for (int xx = 0; xx < g->w; ++xx)
            if (g->trace_item(xx, yy) == things[0]) { px = xx; py = yy; break; }
    CHECK(px >= 0);
    g->double_click(1, px, py);
    u.frames(2);
    CHECK_EQ(w.item(things[0])->parent, pack);
    // Too heavy: refused with the reason, nothing moved.
    const int str = u.av()->str;
    u.av()->str = 0;
    CHECK(!w.take_item_ui(things[1]));
    CHECK(w.refuse_text.find("Too heavy") != std::string::npos);
    CHECK_EQ(w.item(things[1])->parent, bag);
    CHECK(w.put_in_refusal(things[1], pack).find("Too heavy for you") == 0);
    u.frames(1);
    u.shot("loot_too_heavy");
    u.av()->str = str;
    // TAKE ALL: the rest.
    CHECK_EQ(g->take_all(), 2);
    CHECK(w.item(bag)->contents.empty());
    // Too far: walk away, the button refuses with the reason.
    uint16_t t = w.create_item(395, 1);
    w.item(t)->quality = 1;
    w.move_to_container(t, bag);
    w.move_to(1, kX - 400, kY, 0, false);
    u.frames(2);
    CHECK_EQ(g->take_all(), 0);
    CHECK(w.refuse_text.find("Too far") == 0);
    CHECK_EQ(w.item(t)->parent, bag);
    // The Avatar's own backpack is not foreign: no button, a double-click still uses.
    w.move_to(1, kX, kY, 0, false);
    w.call_event(pack, kEvUse);
    u.frames(5);
    if (auto* pg = dynamic_cast<ContainerGump*>(w.gumps.container_gump(pack))) {
        CHECK(!pg->foreign());
        CHECK(!pg->take_button(bx, by, bw, bh));
    }
    // Room: a full bag says so.
    uint16_t small = w.create_item(637, 0);
    w.move_to(small, kX + 40, kY + 40, 0, false);
    for (int k = 0; k < 200 && w.put_in_refusal(t, small).empty(); ++k) {
        uint16_t f = w.create_item(395, 0);  // reagents, one at a time
        w.item(f)->quality = 1;
        if (!w.move_to_container(f, small, true)) { w.destroy_now(f); break; }
    }
    std::printf("  full bag: \"%s\" (%zu inside)\n", w.put_in_refusal(t, small).c_str(), w.item(small)->contents.size());
    CHECK(w.put_in_refusal(t, small) == "Not enough room in there");
}

void test_scrolls(const GameData& d) {
    std::printf("scrolls\n");
    Ui u(d);
    const int key = Spellbook::kScrolls * 1000 + 50;
    auto listed = [&] {
        for (const auto* sp : u.sb().listed()) if (sp->key() == key) return true;
        return false;
    };
    CHECK(!listed());
    uint16_t a = u.give(827, 0, 50), b = u.give(827, 0, 50);
    CHECK(listed());
    CHECK_EQ(u.sb().status(*Spellbook::find(key)).casts, 2);
    CHECK(u.sb().known.empty());  // scrolls follow the inventory, nothing is learned
    std::string why;
    CHECK(u.sb().cast(*Spellbook::find(key), why));
    u.frames(300);
    CHECK_EQ(int(u.w().item(a) != nullptr) + int(u.w().item(b) != nullptr), 1);  // one used up
    CHECK_EQ(u.sb().status(*Spellbook::find(key)).casts, 1);
    CHECK(u.sb().cast(*Spellbook::find(key), why));
    u.frames(300);
    CHECK(!listed());  // none left: gone from the book
    u.give(827, 0, 50);
    CHECK(listed());   // and back with the next one
    // The portal scroll is never used up.
    uint16_t p = u.give(827, 0, 1);
    CHECK(u.sb().cast(*Spellbook::find(Spellbook::kScrolls * 1000 + 1), why));
    u.frames(300);
    CHECK(u.w().item(p) && u.w().item(p)->alive);
}

void test_combat(const GameData& d) {
    std::printf("combat\n");
    Ui u(d);
    TurnCombat& t = *u.w().turn;
    CHECK(t.cast_options(1).empty());
    u.give(746, 0);  // a token he has not learned: not in the book, not in combat
    CHECK(t.cast_options(1).empty());
    u.sb().learn(0);
    u.sb().learn(1008);
    u.give(396, 8);
    auto opts = t.cast_options(1);
    CHECK_EQ(opts.size(), u.sb().listed().size());
    CHECK(opts.size() == 2 && opts[0].name == "Death Speak" && opts[0].uses == 1 && opts[0].book == 0);
    CHECK(opts.size() == 2 && opts[1].uses == -1 && opts[1].mana == 3 && !opts[1].locked);
    u.av()->mana = 0;
    opts = t.cast_options(1);
    CHECK(opts.size() == 2 && opts[0].locked && opts[0].why.find("mana") != std::string::npos);
    u.sb().debug_infinite = true;
    opts = t.cast_options(1);
    CHECK_EQ(opts.size(), Spellbook::spells().size());
    CHECK(!opts.empty() && !opts[0].locked && opts[0].uses == -1 && opts[0].mana == 0);
}

void test_rock(const GameData& d) {
    std::printf("rock\n");
    original_bug_fixes() = true;
    Ui u(d);
    World& w = u.w();
    uint16_t tok = u.give(746, 2);
    const int m0 = u.av()->mana;
    u.picks = {1};
    w.call_event(tok, kEvUse);
    u.frames(240);
    CHECK(rock_flesh::active(w, 1));
    CHECK(u.av()->is(Actor::kImmortal));
    CHECK(rock_flesh::remap(w, 1) != nullptr);
    CHECK(!w.item(tok) || !w.item(tok)->alive);
    CHECK(u.av()->mana <= m0 - 2);
    CHECK(u.knows(0, 2));  // cast the normal way: in the book
    u.shot("rock_flesh_stone");
    // Hits take no HP.
    const int hp = u.av()->hp;
    w.receive_hit(1, 0, 0, 10, 0);
    CHECK_EQ(u.av()->hp, hp);
    // It wears off (2 minutes; here the last frame), and the Avatar is mortal again.
    w.stone_skin[1].frames = 1;
    u.frames(2);
    CHECK(!rock_flesh::active(w, 1));
    CHECK(!u.av()->is(Actor::kImmortal));
    // A foe: refused, nothing spent, the token kept.
    uint16_t foe = w.create_monster(142, 0);
    w.move_to(foe, kX + 80, kY, 0, false);
    tok = u.give(746, 2);
    const int m1 = u.av()->mana;
    u.picks = {foe};
    w.call_event(tok, kEvUse);
    u.frames(240);
    CHECK(!rock_flesh::active(w, foe));
    CHECK(w.item(tok) && w.item(tok)->alive);
    CHECK_EQ(u.av()->mana, m1);
    // Rounds of turn-based combat count instead of time: 12.
    CHECK(rock_flesh::apply(w, 1));
    CHECK_EQ(w.stone_skin[1].turns, rock_flesh::kTurns);
    // Saved and loaded.
    CHECK(u.gm.save_slot(5, "rock"));
    w.stone_skin.clear();
    CHECK(u.gm.load_slot(5));
    CHECK(rock_flesh::active(u.w(), 1));
}

void test_rock_ally(const GameData& d) {
    std::printf("rock ally\n");
    Ui u(d, true);
    Party& p = *u.w().party;
    p.spawn_here();
    p.join(0);
    u.frames(30);
    uint16_t ally = p.state[0].id;
    CHECK(rock_flesh::apply(u.w(), ally));
    CHECK(u.w().actor(ally)->is(Actor::kImmortal));
    u.frames(2);
    u.shot("rock_flesh_ally");
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_shots = argc > 3 ? argv[3] : "";
    g_saves = argc > 4 ? argv[4] : "/tmp/pagan_spellbook_saves";
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
    run("necro", test_necro);
    run("theurgy", test_theurgy);
    run("sorcery", test_sorcery);
    run("thaum", test_thaum);
    run("debug", test_debug);
    run("saves", test_saves);
    run("ui", test_ui);
    run("home", test_home);
    run("lever", test_lever);
    run("loot", test_loot);
    run("scrolls", test_scrolls);
    run("combat", test_combat);
    run("rock", test_rock);
    run("rock_ally", test_rock_ally);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
