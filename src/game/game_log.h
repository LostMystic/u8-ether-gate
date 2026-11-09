// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The HUD's two text logs (PLAN-HUD.md; the engine's own design): the message log (what was
// looked at, conversation lines and answers, other game messages) and the combat log (hits,
// misses, blocks, spells, heals, deaths, turn changes). Filled by small hooks in the world
// (barks, answers, look events, receive_hit, kill_actor, spells, the turn controller) and
// shown by the side panel (app/hud_panel.h). Not saved: it is a view of what happened this
// session (a load keeps the lines and adds a marker).
#include <cstdint>
#include <deque>
#include <string>

namespace pg {

class World;

class GameLog {
public:
    enum Channel { kMessages = 0, kCombat = 1, kChannels = 2 };
    enum Kind {
        kLook, kSpeech, kAnswer, kInfo,                        // messages
        kHit, kMiss, kBlock, kSpell, kHeal, kDeath, kTurn,     // combat
    };
    struct Line {
        std::string text;
        Kind kind = kInfo;
        uint16_t who = 0;   // the speaker / the actor the line is about (0 = none)
        uint32_t serial = 0;
    };
    static constexpr size_t kMaxLines = 400;

    void add(Channel ch, Kind kind, const std::string& text, uint16_t who = 0);
    const std::deque<Line>& lines(Channel ch) const { return lines_[ch]; }
    // Lines ever added to a channel (the panel scrolls to new ones by this).
    uint32_t serial(Channel ch) const { return serial_[ch]; }
    void clear();

    // --- Hooks (each called from one place in the world code)
    void on_look(const World& w, uint16_t item);               // a look event was started on it
    void on_bark(const World& w, uint16_t speaker, const std::string& text);
    void on_answer(const World& w, const std::string& text);    // an answer picked in the menu
    // A blow or spell landed (after the damage calculation): raw = the damage asked for, dealt =
    // what it does; blocking = the victim was blocking; fatal = it kills.
    void on_hit(const World& w, uint16_t victim, uint16_t attacker, int raw, int dealt, bool blocking, bool fatal,
                bool dodged = false);  // dodged: Leveling's dodge (game/leveling.h)
    void on_death(const World& w, uint16_t victim);
    void on_heal(const World& w, uint16_t healer, uint16_t target, int amount, const std::string& spell);
    // The spell being resolved right now (hits it causes read "X's Fire Bolt hits ..."); "" = none.
    std::string spell_context;
    // The latest foe in a blow with the party (the panel's monster slot in real time).
    uint16_t last_foe = 0;

    // A display name: "Brannoc", "Devon", "the ghoul" (lower case monster names with "the"),
    // "the Avatar" for the Avatar (his own name if he has one).
    static std::string name_of(const World& w, uint16_t id, bool article = true);

private:
    std::deque<Line> lines_[kChannels];
    uint32_t serial_[kChannels] = {0, 0};
    uint16_t look_item_ = 0;
    uint32_t look_frame_ = 0;
    std::string last_answer_;
    uint32_t last_answer_frame_ = 0;
    uint16_t killed_by_hit_ = 0;  // the death that on_hit already reported
};

}  // namespace pg
