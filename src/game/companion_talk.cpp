// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Companion conversations, in the game's own style: the companion's lines are barks over their
// head and the Avatar's choices are the answer menu (the same UI Devon's conversation uses).
// Topics, conditions, actions and barks all come from engine/data/companions.json.
#include <algorithm>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/banter.h"
#include "game/combat.h"
#include "game/companion_magic.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"

namespace pg {

namespace {
std::string fill(const World& w, const Party& p, std::string s) {
    for (;;) {
        size_t a = s.find('{');
        if (a == std::string::npos) break;
        size_t b = s.find('}', a);
        if (b == std::string::npos) break;
        std::string key = s.substr(a + 1, b - a - 1), val;
        if (key == "avatar") val = w.avatar_name;
        else if (key.rfind("name:", 0) == 0) {
            int d = p.find_def(key.substr(5));
            val = d >= 0 ? p.defs[size_t(d)].name : key.substr(5);
        } else val = key;
        s.replace(a, b - a + 1, val);
    }
    return s;
}
}  // namespace

bool Party::eval(int idx, const PartyCond& c) const {
    if (c.empty) return true;
    const CompanionState& st = state[size_t(idx)];
    auto check = [](int want, bool have) { return want < 0 || (want == 1) == have; };
    if (!check(c.in_party, in_party(idx))) return false;
    if (!check(c.party_full, party_full())) return false;
    if (!check(c.waiting, st.mode == CompanionState::kWait)) return false;
    if (!check(c.combat, avatar_in_combat())) return false;
    if (!check(c.joined_before, st.joined_before)) return false;
    if (c.dead_ally >= 0) {
        bool any = false;
        const Actor* me = w_.actor(st.id);
        for (size_t i = 0; i < state.size(); ++i) {
            const Actor* o = w_.actor(state[i].id);
            if (int(i) != idx && state[i].mode == CompanionState::kDead && o && me && o->mapnum == me->mapnum &&
                std::abs(o->x - me->x) + std::abs(o->y - me->y) < 640)
                any = true;
        }
        if (!check(c.dead_ally, any)) return false;
    }
    if (c.map >= 0 && unsigned(c.map) != w_.map().map_number()) return false;
    for (const std::string& p : c.present) {
        int d = find_def(p);
        if (d < 0 || !in_party(d)) return false;
    }
    for (const std::string& p : c.absent) {
        int d = find_def(p);
        if (d >= 0 && in_party(d)) return false;
    }
    for (const std::string& f : c.flags)
        if (!st.flags.count(f)) return false;
    for (const std::string& f : c.not_flags)
        if (st.flags.count(f)) return false;
    for (const std::string& f : c.asked)
        if (!st.asked.count(f)) return false;
    for (const std::string& f : c.not_asked)
        if (st.asked.count(f)) return false;
    for (const std::string& s : c.step_done)
        if (!step_done(s)) return false;
    for (const std::string& s : c.step_not_done)
        if (step_done(s)) return false;
    if (c.leveling >= 0 || !c.reagents.empty() || !c.not_reagents.empty()) {
        const bool lv = w_.leveling && w_.leveling->active();
        if (!check(c.leveling, lv)) return false;
        const std::string rs = lv && w_.leveling->magic->loaded ? w_.leveling->magic->reagent_state(idx) : std::string();
        if (!c.reagents.empty() && rs != c.reagents) return false;
        if (!c.not_reagents.empty() && rs == c.not_reagents) return false;
    }
    return true;
}

const std::vector<std::string>* Party::bark_lines(int idx, const std::string& trigger) const {
    const CompanionDef& d = defs[size_t(idx)];
    if (trigger.rfind("map:", 0) == 0) {
        int m = std::atoi(trigger.c_str() + 4);
        for (const auto& ab : d.area_barks)
            if (std::find(ab.maps.begin(), ab.maps.end(), m) != ab.maps.end() && !ab.lines.empty()) return &ab.lines;
    }
    auto it = d.barks.find(trigger);
    if (it == d.barks.end() || it->second.empty()) return nullptr;
    return &it->second;
}

void Party::bark_trigger(int idx, const std::string& trigger, bool force) {
    if (idx < 0 || size_t(idx) >= state.size()) return;
    CompanionState& st = state[size_t(idx)];
    const Actor* a = w_.actor(st.id);
    if (!a || st.hidden || a->mapnum != w_.map().map_number() || a->chunk < 0) return;
    if (a->is(Actor::kDead) && trigger != "revived") return;
    uint32_t now = w_.kernel().frame_number();
    if (!force && now < st.next_bark) return;
    // Idle talk is kept to one line in World::banter_one_in. Reactions that were forced (a fallen
    // ally, killing an innocent, being revived) and being seen stealing always speak.
    if (!force && trigger != "theft" && w_.banter_one_in > 1 && w_.rand_below(w_.banter_one_in) != 0) return;
    if (talking() && talk_.idx != idx && trigger != "ally_down") return;
    const std::vector<std::string>* lines = bark_lines(idx, trigger);
    if (!lines) return;
    // Only one of these at a time for the whole party, and not over a conversation.
    if (!force)
        for (const Bark& b : w_.barks)
            if (b.speaker == 1 || is_companion(b.speaker)) return;
    w_.bark(st.id, fill(w_, *this, (*lines)[size_t(w_.rand_below(int(lines->size())))]));
    st.next_bark = now + 450 + unsigned(w_.rand_below(450));
    log.push_back(defs[size_t(idx)].id + " bark " + trigger);
}

void Party::ambient_barks() {
    uint32_t now = w_.kernel().frame_number();
    if (now < next_ambient_ || w_.stasis || talking() || w_.ask_menu || !w_.barks.empty() || banter->playing()) return;
    next_ambient_ = now + 600 + unsigned(w_.rand_below(900));  // 20-50 s
    const Actor* av = w_.avatar();
    if (!av) return;
    std::vector<int> cand;
    for (size_t i = 0; i < state.size(); ++i) {
        const CompanionState& st = state[i];
        const Actor* a = w_.actor(st.id);
        if (!a || st.hidden || a->is(Actor::kDead) || a->chunk < 0 || a->mapnum != w_.map().map_number()) continue;
        if (std::abs(a->x - av->x) + std::abs(a->y - av->y) > 700) continue;
        if (st.mode == CompanionState::kIdle || st.mode == CompanionState::kFollow || st.mode == CompanionState::kWait)
            cand.push_back(int(i));
    }
    if (cand.empty()) return;
    int i = cand[size_t(w_.rand_below(int(cand.size())))];
    const CompanionState& st = state[size_t(i)];
    if (avatar_in_combat()) return;
    // At home they talk of the place; one who parted somewhere is only waiting there.
    bark_trigger(i, st.mode == CompanionState::kIdle ? (st.joined_before ? "waiting" : "home") : st.mode == CompanionState::kWait ? "waiting" : "idle");
}

// ---------------------------------------------------------------------------------------------
// Party commands

bool Party::join(int idx) {
    if (idx < 0 || size_t(idx) >= state.size() || in_party(idx) || party_full()) return false;
    CompanionState& st = state[size_t(idx)];
    if (st.mode == CompanionState::kDead || st.mode == CompanionState::kUnspawned) return false;
    members.push_back(idx);
    st.mode = CompanionState::kFollow;
    if (!st.joined_before && w_.leveling) w_.leveling->on_recruit(st.id);  // late recruits catch up
    st.joined_before = true;
    st.far_frames = 0;
    st.stuck = 0;
    log.push_back(defs[size_t(idx)].id + " joined");
    update_bar();
    return true;
}

void Party::wait(int idx) {
    CompanionState& st = state[size_t(idx)];
    const Actor* a = w_.actor(st.id);
    if (!a || !in_party(idx)) return;
    st.mode = CompanionState::kWait;
    st.wait_pos[0] = a->x; st.wait_pos[1] = a->y; st.wait_pos[2] = a->z;
    st.wait_map = a->mapnum;
    w_.kernel().kill(st.id, kProcPathfind);
    log.push_back(defs[size_t(idx)].id + " waits");
}

void Party::follow(int idx) {
    if (!in_party(idx)) return;
    state[size_t(idx)].mode = CompanionState::kFollow;
    log.push_back(defs[size_t(idx)].id + " follows");
}

void Party::part_ways(int idx) {
    CompanionState& st = state[size_t(idx)];
    members.erase(std::remove(members.begin(), members.end(), idx), members.end());
    const CompanionDef& d = defs[size_t(idx)];
    // They stay where they parted, not going home (NOTES.md "Companions: homes").
    st.mode = CompanionState::kIdle;
    st.far_frames = 0;
    st.target = 0;
    if (Actor* a = w_.actor(st.id)) {
        a->set(Actor::kInCombat, false);
        st.wait_pos[0] = a->x; st.wait_pos[1] = a->y; st.wait_pos[2] = a->z;
        st.wait_map = a->mapnum;
    }
    w_.kernel().kill(st.id, kProcPathfind);
    w_.close_gump(st.id);
    log.push_back(d.id + " parts");
    update_bar();
}

void Party::open_inventory(int idx) {
    CompanionState& st = state[size_t(idx)];
    const Actor* a = w_.actor(st.id);
    if (!a || a->is(Actor::kDead)) return;
    // The sheet and the backpack together, both on top; the backpack beside the sheet (place_pack).
    // The backpack's window is opened directly (gump 2, as its usecode does), not through its use
    // event, and one already open (perhaps under the sheet) is raised and moved beside it again.
    if (Gump* g = w_.gumps.container_gump(st.id)) w_.gumps.raise(g);
    else w_.open_gump(st.id, 1);
    if (uint16_t pack = w_.equipped(st.id, 7)) {
        if (Gump* g = w_.gumps.container_gump(pack)) w_.gumps.raise(g);
        else w_.open_gump(pack, 2);
        if (w_.gumps.container_gump(pack)) {
            place_pack_ = pack;
            place_doll_ = st.id;
            place_tries_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Conversation

void Party::start_talk(int idx) {
    if (w_.ask_menu || w_.stasis || w_.target_pid) return;  // a scripted conversation owns the menu
    const Actor* a = w_.actor(state[size_t(idx)].id);
    if (!a || a->is(Actor::kDead)) return;
    talk_ = Talk{};
    talk_.idx = idx;
    talk_.menu_stack.push_back(&defs[size_t(idx)].topics);
    w_.kernel().kill(a->id, kProcPathfind);
    const CompanionState& st = state[size_t(idx)];
    const char* trig = in_party(idx) ? "greet_party" : st.joined_before ? "greet_again" : "greet";
    // Back home after dying, and spoken to before they could say it: their line comes first.
    if (state[size_t(idx)].flags.erase("came_home")) trig = "found_again";
    const std::vector<std::string>* g = bark_lines(idx, trig);
    if (!g) g = bark_lines(idx, "greet");
    if (g) talk_.queue.push_back((*g)[size_t(w_.rand_below(int(g->size())))]);
    talk_.stage = Talk::kSaying;
    log.push_back(defs[size_t(idx)].id + " talk");
    talk_frame();
}

void Party::end_talk() {
    if (talk_.idx < 0) return;
    if (Process* p = w_.kernel().get(talk_.ask_pid); p && p->live() && w_.ask_menu && w_.ask_menu->pid == talk_.ask_pid) {
        w_.vm().free_slist(w_.ask_menu->list);
        w_.ask_menu.reset();
        p->terminate();
    }
    for (uint16_t sid : talk_.answer_sids)
        if (w_.vm().has_string(sid)) w_.vm().free_string(sid);
    if (Process* p = w_.kernel().get(talk_.target_pid); p && p->live()) p->terminate();
    talk_ = Talk{};
}

std::vector<const PartyTopic*> Party::visible_topics(int idx) const {
    std::vector<const PartyTopic*> out;
    const std::vector<PartyTopic>* menu = (talk_.idx == idx && !talk_.menu_stack.empty()) ? talk_.menu_stack.back()
                                                                                          : &defs[size_t(idx)].topics;
    for (const PartyTopic& t : *menu)
        if (eval(idx, t.when)) out.push_back(&t);
    return out;
}

void Party::talk_say(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) talk_.queue.push_back(l);
    talk_.stage = Talk::kSaying;
}

void Party::talk_menu() {
    int idx = talk_.idx;
    std::vector<const PartyTopic*> vis = visible_topics(idx);
    talk_.answers.clear();
    std::vector<std::string> labels;
    bool has_bye = false;
    for (const PartyTopic* t : vis) {
        if (labels.size() >= 9) break;
        talk_.answers.push_back(t);
        labels.push_back(fill(w_, *this, t->label));
        if (t->action == "bye") has_bye = true;
    }
    if (talk_.menu_stack.size() > 1) {
        talk_.answers.push_back(nullptr);
        labels.push_back("back");
    } else if (!has_bye) {
        talk_.answers.push_back(nullptr);
        labels.push_back("bye");
    }
    UCMachine& vm = w_.vm();
    uint16_t l = vm.new_list(2);
    for (const std::string& s : labels) vm.list(l)->push_word(vm.new_string(s));
    talk_.ask_pid = w_.ask(l);
    vm.free_slist(l);
    talk_.answer_sids = w_.ask_menu ? w_.ask_menu->answers : std::vector<uint16_t>{};
    talk_.stage = Talk::kMenu;
    talk_.frames = 0;
}

void Party::talk_choose(const PartyTopic& t) {
    int idx = talk_.idx;
    CompanionState& st = state[size_t(idx)];
    st.asked.insert(t.id);
    for (const std::string& f : t.set_flags) st.flags.insert(f);
    for (const std::string& f : t.clear_flags) st.flags.erase(f);
    const std::vector<std::string>* say = &t.say;
    for (const auto& alt : t.alt)
        if (eval(idx, alt.first)) { say = &alt.second; break; }
    talk_say(*say);
    if (!t.topics.empty()) talk_.menu_stack.push_back(&t.topics);
    // An order given from a sub-menu returns to the main menu.
    else if (!t.action.empty() && talk_.menu_stack.size() > 1) talk_.menu_stack.resize(1);
    talk_.pending_action = t.action;
    log.push_back(defs[size_t(idx)].id + " topic " + t.id);
}

// Runs a topic's action once its lines have been said. Returns false when the conversation
// ends (the caller then closes it after any lines queued here).
bool Party::do_action(int idx, const std::string& action) {
    auto say = [&](const char* trig) {
        if (const std::vector<std::string>* l = bark_lines(idx, trig))
            talk_.queue.push_back((*l)[size_t(w_.rand_below(int(l->size())))]);
    };
    if (action.empty()) return true;
    if (action == "join") {
        if (party_full()) say("refuse_full");
        else if (join(idx)) say("join");
        return true;
    }
    if (action == "wait") { wait(idx); say("wait"); return true; }
    if (action == "follow") { follow(idx); say("follow"); return true; }
    if (action == "leave") { part_ways(idx); say("leave"); talk_.end_after = true; return false; }
    if (action == "trade") { talk_.end_after = true; talk_.pending_action = "open_inventory"; return false; }
    if (action == "bye") { say("bye"); talk_.end_after = true; return false; }
    if (action == "picklock" || action == "steal") {
        talk_.target_action = action;
        talk_.target_pid = w_.target_pick();
        talk_.stage = Talk::kTarget;
        return true;
    }
    if (action.rfind("cast:", 0) == 0) {
        int si = spell_index(action.substr(5));
        const Actor* me = w_.actor(state[size_t(idx)].id);
        if (si < 0 || !me) return true;
        const SpellDef& s = spells[size_t(si)];
        std::string why;
        if (!spell_ready(idx, si, &why)) {
            // Leveling stage 5: locked, or no uses left.
            say(why == "out of reagents" ? "reagents_out" : "spell_locked");
            w_.game_log.add(GameLog::kMessages, GameLog::kInfo, s.name + ": " + why + ".", me->id);
            return true;
        }
        if (me->mana < s.mana) { say("no_mana"); return true; }
        uint16_t target = 0;
        if (s.kind == "raise") {
            int best = 1 << 30;
            for (size_t i = 0; i < state.size(); ++i) {
                const Actor* o = w_.actor(state[i].id);
                if (int(i) == idx || state[i].mode != CompanionState::kDead || !o || o->mapnum != me->mapnum) continue;
                if (!s.only.empty() && defs[i].id != s.only) continue;
                int dd = std::abs(o->x - me->x) + std::abs(o->y - me->y);
                if (dd < 640 && dd < best) { best = dd; target = o->id; }
            }
        } else {
            // The most hurt of the party (or the caster) for heals and cures.
            int worst = 101;
            auto consider = [&](uint16_t id) {
                const Actor* m = w_.actor(id);
                if (!m || m->is(Actor::kDead)) return;
                int pct = int(m->hp) * 100 / std::max(1, id == 1 ? 2 * int(m->str) : max_hp(*m));
                if (s.kind == "cure" && m->is(Actor::kPoisoned)) pct -= 100;
                if (pct < worst) { worst = pct; target = id; }
            };
            consider(1);
            for (int m : members) consider(state[size_t(m)].id);
            if (worst >= 100 && s.kind != "cure") target = 0;
        }
        if (!target) { say("cast_nobody"); return true; }
        if (cast(idx, si, target)) say("cast_ok");
        return true;
    }
    PG_LOG("party: unknown action '%s'", action.c_str());
    return true;
}

void Party::talk_frame() {
    int idx = talk_.idx;
    if (idx < 0) return;
    CompanionState& st = state[size_t(idx)];
    const Actor* a = w_.actor(st.id);
    const Actor* av = w_.avatar();
    if (!a || !av || a->is(Actor::kDead) || a->mapnum != w_.map().map_number() ||
        std::abs(a->x - av->x) + std::abs(a->y - av->y) > 900 || av->is(Actor::kDead)) {
        end_talk();
        return;
    }
    ++talk_.frames;
    switch (talk_.stage) {
    case Talk::kSaying: {
        if (Process* p = w_.kernel().get(talk_.bark_pid); p && p->live()) return;
        talk_.bark_pid = 0;
        if (!talk_.queue.empty()) {
            std::string line = fill(w_, *this, talk_.queue.front());
            talk_.queue.erase(talk_.queue.begin());
            talk_.bark_pid = w_.bark(st.id, line);
            return;
        }
        if (!talk_.pending_action.empty()) {
            std::string act = talk_.pending_action;
            talk_.pending_action.clear();
            if (act == "open_inventory") {
                int i = idx;
                end_talk();
                open_inventory(i);
                return;
            }
            do_action(idx, act);
            if (talk_.stage == Talk::kTarget) return;
            if (!talk_.queue.empty()) return;  // the action's own line first
        }
        if (talk_.end_after) {
            if (talk_.pending_action == "open_inventory") return;
            end_talk();
            return;
        }
        talk_menu();
        return;
    }
    case Talk::kMenu: {
        Process* p = w_.kernel().get(talk_.ask_pid);
        if (p && p->live()) return;
        uint16_t sid = p ? uint16_t(p->result) : 0;
        int pick = -1;
        for (size_t i = 0; i < talk_.answer_sids.size(); ++i)
            if (talk_.answer_sids[i] == sid && w_.vm().has_string(sid)) pick = int(i);
        if (pick >= 0) w_.vm().free_string(sid);
        talk_.answer_sids.clear();
        talk_.ask_pid = 0;
        if (pick < 0 || size_t(pick) >= talk_.answers.size()) { end_talk(); return; }
        const PartyTopic* t = talk_.answers[size_t(pick)];
        if (!t) {
            if (talk_.menu_stack.size() > 1) { talk_.menu_stack.pop_back(); talk_menu(); return; }
            if (const std::vector<std::string>* l = bark_lines(idx, "bye")) {
                talk_.queue.push_back((*l)[size_t(w_.rand_below(int(l->size())))]);
                talk_.end_after = true;
                talk_.stage = Talk::kSaying;
                return;
            }
            end_talk();
            return;
        }
        talk_choose(*t);
        return;
    }
    case Talk::kTarget: {
        Process* p = w_.kernel().get(talk_.target_pid);
        if (p && p->live()) return;
        uint16_t target = p ? uint16_t(p->result) : 0;
        talk_.target_pid = 0;
        std::string msg;
        std::string act = talk_.target_action;
        talk_.target_action.clear();
        if (!target) msg = "cancel";
        else if (act == "picklock") pick_lock(idx, target, msg);
        else steal_from(idx, target, msg);
        if (const std::vector<std::string>* l = bark_lines(idx, act + "_" + msg))
            talk_.queue.push_back((*l)[size_t(w_.rand_below(int(l->size())))]);
        talk_.stage = Talk::kSaying;
        // Caught stealing: the conversation ends at once (the town has other ideas).
        if (msg == "caught" || msg == "stolen_seen") talk_.end_after = true;
        return;
    }
    default: return;
    }
}

}  // namespace pg
