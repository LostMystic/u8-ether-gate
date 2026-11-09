// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The spellbook (the engine's own design, NOTES.md "Spellbook"; key G). A book in the style of
// Ultima VII's: the spells in sections by school, in the order the Avatar meets them (Necromancy,
// Theurgy, Sorcery, Thaumaturgy, and an empty Tempest section for the water magic to come), a few
// to a page. Double-clicking a spell casts it through the game's own items and scripts.
//
// - A spell is entered in the book the first time the Avatar makes it (a necromancy token from a
//   bag of reagents with the key, a sorcery focus charged at a pentagram, a thaumaturgy book
//   charged) or casts it the normal way himself (a token, a theurgy focus, a charged sorcery focus,
//   a charged book). Reading a scroll does not enter a school spell. Saved (section SPBK), never forgotten.
// - Scrolls (shape 827, by quality) have their own section, listed while the Avatar carries one: each
//   scroll is one cast; casting from the book uses the scroll up (except the Scroll of Dispelling Magic
//   Portals, which the story needs again and the game itself never uses up). Not saved: it follows
//   the inventory. Turn-based combat's Cast list for the Avatar is the book (turn_combat.cpp).
// - Each listed spell shows how many casts the Avatar has: his talismans (tokens, focus charges,
//   charged books) plus the full sets of reagents he carries, and whether his mana is enough.
// - Casting from the book uses the talismans first, then the reagents (made into the talisman on
//   the spot), and needs the mana (theurgy: the focus and the mana; sorcery from reagents: the
//   enchanting mana). With neither, or too little mana, nothing is used up and the book says why.
// - Debug (debug menu "Infinite spells"): every spell is listed and casts without talismans,
//   reagents or mana, through temporary items; the real book is untouched (debug casts never enter
//   a spell) and comes back as it was when the option is turned off. Not saved.
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/gumps.h"

namespace pg {

class World;
class SaveWriter;
class SaveReader;

class Spellbook {
public:
    enum School : int { kNecromancy, kTheurgy, kSorcery, kThaumaturgy, kTempest, kScrolls, kSchools };
    static const char* school_name(int s);

    struct Reagent { int kind; int count; };
    struct Spell {
        School school;
        int num;               // token frame / focus frame / sorcery spell number / book quality
        const char* name;
        const char* runes;
        int cost;              // mana per cast (sorcery: the enchanting cost's top; thaumaturgy 0)
        int cost_min;          // sorcery: the enchanting cost's bottom
        std::vector<Reagent> reagents;  // by reagent kind (kinds of this school's reagent shape)
        unsigned icon_shape, icon_frame;
        int key() const { return int(school) * 1000 + num; }
    };
    static const std::vector<Spell>& spells();
    static const Spell* find(int key);

    explicit Spellbook(World& w);
    ~Spellbook();

    // --- State (saved: section SPBK, only when something is known)
    std::set<int> known;   // Spell::key()
    bool debug_infinite = false;  // the debug menu's "Infinite spells" (not saved)
    bool has_state() const { return !known.empty(); }
    void reset();
    void save(SaveWriter& o) const;
    bool load(SaveReader& in);

    // --- Queries
    std::vector<const Spell*> listed() const;  // what the book shows now (all of them in debug)
    struct Status {
        int talismans = 0;      // tokens / focus charges / charged books
        bool focus = false;     // theurgy: the focus is carried (casts limited by mana only)
        int reagent_casts = 0;  // full reagent sets carried
        int casts = 0;          // what the ingredients allow (-1: unlimited, theurgy with a focus)
        int mana_casts = -1;    // how many the mana allows (-1: no mana needed)
        bool castable = false;
        std::string note;       // why not ("No focus", "Need 5 mana", ...)
    };
    Status status(const Spell& s) const;
    // Casts from the book (the caller closes the book first). False with `why` if refused; nothing
    // is used up then.
    bool cast(const Spell& s, std::string& why);
    uint16_t last_cast_pid = 0;  // the use event the last cast started (turn-based combat waits on it)

    // --- Hooks (World)
    void on_use(uint16_t item, uint16_t pid);  // a use event started (not by the book itself)
    void on_create(uint16_t item);             // an item was created
    void frame();
    bool internal_use = false;                 // set while the book itself fires a use event
    void learn(int key);
    std::vector<std::string> log;              // what happened (tests)

    // Reagent kind of a reagent item (-1: not one of this school's), and the inventory scan.
    static int reagent_kind(School school, unsigned shape, unsigned frame);
    void inventory(std::vector<uint16_t>& out) const;

private:
    struct Watch;
    World& w_;
    std::vector<std::unique_ptr<Watch>> watches_;
    int count_reagent_sets(const Spell& s, const std::vector<uint16_t>& inv) const;
    void use_up_reagents(const Spell& s);
    uint16_t find_talisman(const Spell& s, const std::vector<uint16_t>& inv) const;
    uint16_t fire(uint16_t item);  // the book's own use event
    bool debug_cast(const Spell& s, std::string& why);
};

// The book on screen (key G): modal and pausing, on the game's book picture (gump 6). Each page
// carries its school's name, up to four spells (icon, name, casts, mana) and a footer (the Avatar's
// mana, page turns, the last message). Click selects, double-click (or Enter) casts and closes the
// book; Left / Right, PgUp / PgDn or the corner arrows turn the pages; Esc, right click or G close.
class SpellbookGump : public Gump {
public:
    explicit SpellbookGump(World& w);
    const char* kind() const override { return "spellbook"; }
    void paint(Surface8& s) override;
    bool hit(int, int) const override { return true; }
    bool press(int button, int lx, int ly) override;
    void double_click(int button, int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void tick() override;

    struct Page { int school = 0; std::vector<const Spellbook::Spell*> spells; };
    struct Entry { const Spellbook::Spell* spell; int page, x, y, w, h; };
    std::vector<Page> pages;          // rebuilt each frame (what is listed can change)
    std::vector<Entry> entries;       // the current spread's (gump-local boxes)
    int spread = 0;
    int selected = -1;                // Spell::key()
    std::string message;
    int message_frames = 0;
    int per_page = 4;
    void build();
    bool turn(int dir);
    bool cast_selected();             // closes on success
    void select_step(int d);
    int spreads() const { return int((pages.size() + 1) / 2); }
    // The gump-local box of a page's text area (side 0 left, 1 right) and the corner arrows.
    void page_rect(int side, int& px, int& py, int& pw, int& ph) const;
    bool arrow_hit(int which, int lx, int ly) const;  // 0 previous, 1 next
    std::string info_line(const Spellbook::Spell& s, const Spellbook::Status& st, bool& warn, std::string& line2) const;

private:
    uint8_t red_[256];
    uint8_t ink_ = 0;
    int frames_ = 0;
};

}  // namespace pg
