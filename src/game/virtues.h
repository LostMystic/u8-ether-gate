// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The Avatar's eight virtue scores (PLAN-VIRTUES.md section 1; the engine's own design, not in
// the original game; NOTES.md "Virtues"). Behind the option "Virtues" (default On): with it off the
// octagon on the Avatar's sheet is hidden and add() pays nothing. The scores stay in the save either
// way, so turning it back on shows them again.
//
// Each virtue holds 0..max (data/virtues.json "max", default 50). A deed pays through add() with a
// key: a key pays each virtue once per save, so the same deed can never pay twice (the keys are
// saved). One deed that pays two virtues may use one key for both.
// Saved in the optional section VIRT (after CRTR), written only when something is set; a save
// without it loads with every score at 0.
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "game/critpath.h"

namespace pg {

class World;
class SaveWriter;
class SaveReader;
class Surface8;

enum Virtue : int {
    kHonesty, kCompassion, kValor, kJustice, kSacrifice, kHonor, kSpirituality, kHumility, kVirtueCount
};

constexpr int kVirtueDefaultMax = 50;

class Virtues {
public:
    explicit Virtues(World& w) : w_(&w) {}

    // --- Settings (not in the save)
    bool enabled = false;  // "Virtues" (main turns it on from the setting, default On)
    static void register_options();

    // --- Data (data/virtues.json)
    bool load_dir(const std::string& dir, std::string& err);
    bool load_text(const std::string& text, std::string& err);
    int max_score = kVirtueDefaultMax;
    // Deeds the story flags tell (data/virtues.json "deeds"; PLAN-VIRTUES.md 3): when `when` holds
    // (the critical-path condition language), each listed virtue pays once (key "deed/<id>").
    struct Deed {
        std::string id, say;  // say: the message-log line when it pays ("" = the default)
        std::string mark;     // also needs this mark (set by a dialogue topic), "" = none
        CritCond when;
        std::vector<std::pair<int, int>> pay;  // virtue, points
    };
    std::vector<Deed> deeds;
    // The endings (PLAN-VIRTUES.md 6; "endings", "epilogue"): the first ending whose min_total the
    // total reaches, then a card per deed done (a paid key or "mark/<m>").
    // An ending matches when the total reaches min_total, infamy is within [min_infamy, max_infamy]
    // (max_infamy -1: no limit) and, with mostly_bad, infamy is above the total: the tyrant needs
    // real cruelty, a quiet game with few good deeds is the pragmatist. The first match wins;
    // none matching: the last ending.
    struct Ending {
        std::string id, title;
        int min_total = 0, min_infamy = 0, max_infamy = -1;
        bool mostly_bad = false;
        std::vector<std::string> text;
    };
    std::vector<Ending> endings;
    std::vector<std::pair<std::string, std::string>> epilogue;  // key, text
    const Ending* ending() const;  // nullptr: no endings in the data
    // The epilogue page as credits paragraphs ("+Title", "}centred text"; game/credits.h).
    std::vector<std::string> epilogue_page() const;
    // Valor from fights (data/virtues.json "valor"): a kill well above the Avatar's level, and the
    // dead slain in Tenebrae during the undead invasion. From Leveling::on_kill, for every death
    // there; victim_level / avatar_level are -1 without Leveling.
    struct Valor { int levels = 3, points = 3, times = 3, invasion_kills = 5, invasion_points = 5; } valor;
    void on_kill(uint16_t victim, uint16_t attacker, int victim_level, int avatar_level);
    // Infamy (PLAN-VIRTUES.md 6): the bad acts that make the tyrant ending. Not a virtue and not
    // shown on the octagon. Kept in `paid` as "Vice/<key>/<points>", so it needs no save change.
    // Sources: murdering townsfolk (on_kill: a victim not hostile to the Avatar by its side, slain
    // by the party), theft by a companion (Party::steal_from), cruel dialogue answers ("vice"),
    // and, at the ending, promises made and never kept (a deed with a mark set but unpaid).
    struct Vice { int murder = 4, murder_times = 10, theft = 1, theft_times = 10, broken_promise = 3; } vice_cfg;
    // Taking what belongs to the townsfolk (item: the item taken; caps at vice_cfg.theft_times).
    void on_theft(uint16_t item);
    int sin(int points, const std::string& key, const std::string& say = std::string());
    bool has_sinned(const std::string& key) const;
    int infamy() const;              // the bad acts so far
    int broken_promises() const;     // promises (deed marks) set and not kept
    int infamy_at_end() const { return infamy() + broken_promises() * vice_cfg.broken_promise; }
    // Checks the deeds about once a second (World::frame).
    void frame();
    int deed_check_frames = 30;

    // --- State (saved: section VIRT)
    std::array<int, kVirtueCount> score{};
    std::set<std::string> paid;  // "Virtue/key" of the deeds that have paid

    // --- Queries
    static const char* name(int v);         // "Honesty" .. "Humility"; "" out of range
    static int index_of(const std::string& name);  // case-insensitive; -1 if none
    int get(int v) const { return v >= 0 && v < kVirtueCount ? score[size_t(v)] : 0; }
    int total() const;
    int total_max() const { return max_score * kVirtueCount; }
    bool all_max() const { return total() >= total_max(); }
    bool has_paid(int v, const std::string& key) const { return paid.count(std::string(name(v)) + "/" + key) != 0; }
    // Marks: saved notes of deeds (a promise made, a deed for the epilogue), kept with the keys as "mark/<m>".
    bool marked(const std::string& m) const { return paid.count("mark/" + m) != 0; }
    void set_mark(const std::string& m) { paid.insert("mark/" + m); }

    // --- Changes
    // A deed's points: clamped to 0..max. With a key, a key that has paid already pays nothing.
    // Off (the option): pays nothing and does not use up the key. Returns the change made.
    int add(int v, int points, const std::string& key = std::string());
    // add(), and a line in the message log when it paid ("Your Compassion grows.").
    int earn(int v, int points, const std::string& key, const std::string& say = std::string());
    // Sets a score (the debug menu, tests), clamped; works with the option off too.
    void set(int v, int value);
    void reset();  // a new game, a load without the section
    // The debug menu: infamy set directly (the difference kept as "Vice/debug/<n>"; below the bad
    // acts recorded, they are dropped), every bad act dropped, the marks the data knows (deeds' and
    // the epilogue's "mark/..." keys) and clearing one.
    void set_infamy(int value);
    void clear_infamy();
    std::vector<std::string> known_marks() const;
    void clear_mark(const std::string& m) { paid.erase("mark/" + m); }
    // The deeds whose story flags hold now are marked paid without paying (a story jump: the world
    // was put there, nobody did them just now). Returns how many.
    int settle_deeds();

    // --- Popups (NOTES.md "Virtue popups"; not saved): every gain add() makes queues "Humility +4",
    // shown rising and fading over the Avatar's head at the first calm moment (no conversation or
    // answer menu, stasis, modal window - a movie, a book - or fade). Gains of one virtue still
    // waiting merge; several show one after another, kPopupGap frames apart, so they stack.
    struct Popup { int virtue = 0, points = 0, age = 0; };
    std::vector<Popup> popups_waiting;  // gains not shown yet (oldest first)
    std::vector<Popup> popups;          // on screen now (age in world frames, oldest first)
    static constexpr int kPopupFrames = 84;  // 2.8 s at 30 frames a second
    static constexpr int kPopupGap = 40;     // frames between two popups' starts
    static constexpr int kPopupCalm = 12;    // calm frames before the first one shows
    bool popup_moment();  // a calm moment to show them
    void popup_frame();   // from frame()
    static std::string popup_text(const Popup& p);  // "Humility +4"

    bool has_state() const;
    void save(SaveWriter& o) const;
    bool load(SaveReader& in);

private:
    World* w_;
    int deed_wait_ = 0;
    int popup_calm_ = 0;
};

// Draws the virtue popups over the Avatar's head on the UI layer (game/virtue_popups.cpp; from
// GameMode::draw_barks): font 0 in the virtue's colour with a dark outline, rising, dithered out.
void draw_virtue_popups(World& w, Surface8& s);

}  // namespace pg
