// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The HUD's message and combat logs: see game_log.h.
#include "game/creatures.h"
#include "game/game_log.h"

#include <cctype>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/world.h"

namespace pg {

namespace {

// One line: whitespace runs (line breaks, page marks) become single spaces.
std::string tidy(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ' || c == '*') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string capital(std::string s) {
    if (!s.empty()) s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

}  // namespace

void GameLog::add(Channel ch, Kind kind, const std::string& text, uint16_t who) {
    if (text.empty()) return;
    Line l;
    l.text = text;
    l.kind = kind;
    l.who = who;
    l.serial = ++serial_[ch];
    lines_[ch].push_back(std::move(l));
    while (lines_[ch].size() > kMaxLines) lines_[ch].pop_front();
}

void GameLog::clear() {
    for (auto& l : lines_) l.clear();
}

std::string GameLog::name_of(const World& w, uint16_t id, bool article) {
    if (id == 666) return "The Guardian";
    const Item* it = w.item(id);
    if (!it) return "someone";
    if (w.party && w.party->is_companion(id)) return w.party->display_name(id);
    if (id == 1) return w.avatar_name.empty() ? "Avatar" : w.avatar_name;
    const Actor* a = it->as_actor();
    if (a) {  // a new creature's name, or a weaker variant's ("young troll"): game/creatures.h
        std::string cn = creature_display_name(w, id);
        if (!cn.empty()) return article ? "the " + cn : cn;
    }
    if (a && !a->name.empty()) {
        // NPC names are stored in capitals ("DEVON"): shown as "Devon".
        std::string n = a->name;
        bool caps = true;
        for (char c : n) caps = caps && !std::islower(static_cast<unsigned char>(c));
        if (caps)
            for (size_t i = 1; i < n.size(); ++i)
                if (std::isalpha(static_cast<unsigned char>(n[i - 1]))) n[i] = char(std::tolower(static_cast<unsigned char>(n[i])));
        return n;
    }
    std::string n = w.data().usecode.class_name(it->shape);
    if (n.empty()) return a ? (article ? "the monster" : "monster") : "something";
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) out += n[i] == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(n[i])));
    return article ? "the " + out : out;
}

void GameLog::on_look(const World& w, uint16_t item) {
    look_item_ = item;
    look_frame_ = w.game_frames();
}

void GameLog::on_bark(const World& w, uint16_t speaker, const std::string& text) {
    std::string t = tidy(text);
    if (t.empty()) return;
    uint32_t now = w.game_frames();
    // What a look shows: the bark of the item looked at, soon after the look.
    if (look_item_ && speaker == look_item_ && now - look_frame_ < 60) {
        look_item_ = 0;
        while (!t.empty() && (t.back() == '.' || t.back() == '!')) t.pop_back();
        add(kMessages, kLook, "You see " + t + ".", speaker);
        return;
    }
    // The Avatar saying the answer just picked: already in the log.
    if (speaker == 1 && !last_answer_.empty() && now - last_answer_frame_ < 90 && tidy(last_answer_) == t) {
        last_answer_.clear();
        return;
    }
    add(kMessages, kSpeech, name_of(w, speaker, false) + ": " + t, speaker);
}

void GameLog::on_answer(const World& w, const std::string& text) {
    std::string t = tidy(text);
    if (t.empty()) return;
    last_answer_ = t;
    last_answer_frame_ = w.game_frames();
    add(kMessages, kAnswer, "> " + t, 1);
}

void GameLog::on_hit(const World& w, uint16_t victim, uint16_t attacker, int raw, int dealt, bool blocking, bool fatal,
                     bool dodged) {
    const Item* v = w.item(victim);
    if (!v || !v->as_actor()) return;
    auto ours = [&](uint16_t id) { return id == 1 || w.same_party(1, id); };
    if (attacker && ours(victim) && !ours(attacker)) last_foe = attacker;
    else if (attacker && ours(attacker) && !ours(victim)) last_foe = victim;
    std::string vn = name_of(w, victim), an = attacker ? name_of(w, attacker) : "";
    std::string by = an.empty() ? "" : spell_context.empty() ? an : an + "'s " + spell_context;
    std::string line;
    Kind kind = kHit;
    if (dealt > 0) {
        if (fatal) {
            line = by.empty() ? capital(vn) + " is killed." : capital(by) + " kills " + vn + ".";
            kind = kDeath;
            killed_by_hit_ = victim;
        } else if (by.empty()) {
            line = capital(vn) + " takes " + std::to_string(dealt) + " damage.";
        } else {
            line = capital(by) + " hits " + vn + " for " + std::to_string(dealt) + ".";
        }
        if (!spell_context.empty()) kind = fatal ? kDeath : kSpell;
    } else if (!spell_context.empty()) {
        line = capital(vn) + " shrugs off " + (by.empty() ? spell_context : by) + ".";
        kind = kSpell;
    } else if (dodged) {
        line = capital(vn) + " dodges" + (an.empty() ? "" : " " + an) + ".";
        kind = kMiss;
    } else if (blocking && raw > 0) {
        line = capital(vn) + " blocks" + (an.empty() ? "" : " " + an) + ".";
        kind = kBlock;
    } else if (!an.empty()) {
        line = capital(an) + " misses " + vn + ".";
        kind = kMiss;
    } else {
        return;  // nothing happened worth a line (a harmless bump)
    }
    add(kCombat, kind, line, attacker ? attacker : victim);
}

void GameLog::on_death(const World& w, uint16_t victim) {
    if (victim == killed_by_hit_) { killed_by_hit_ = 0; return; }
    add(kCombat, kDeath, capital(name_of(w, victim)) + " dies.", victim);
}

void GameLog::on_heal(const World& w, uint16_t healer, uint16_t target, int amount, const std::string& spell) {
    if (amount <= 0) return;
    std::string who = capital(name_of(w, healer)) + (spell.empty() ? "" : "'s " + spell);
    std::string line = who + " restores " + std::to_string(amount) + " HP" + (target == healer ? "" : " to " + name_of(w, target)) + ".";
    add(kCombat, kHeal, line, healer);
}

}  // namespace pg
