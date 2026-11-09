// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// World-side state of the usecode-driven UI: barks, the ask menu, readers, the targeting
// cursor and palette fades (specs/intrinsics/ui.md, specs/presentation/gumps.md 2.8-2.11),
// plus the actor action helpers used by intrinsics.
#include <algorithm>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/virtues.h"
#include "game/credits.h"
#include "game/creatures.h"
#include "game/leveling.h"
#include "game/combat.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/world.h"
#include "game/movie.h"
#include "usecode/uc_machine.h"

namespace pg {

namespace {
int bark_font(uint16_t speaker) {
    if (speaker == 1 || speaker == 666) return 6;
    if (speaker > 256) return 8;
    switch (speaker % 3) {
    case 1: return 5;
    case 2: return 7;
    default: return 0;
    }
}
}  // namespace

uint16_t World::bark(uint16_t speaker, const std::string& said) {
    if (!item(speaker) && speaker != 666) return 0;
    // A scene patch of the engine-side dialogue (game/npc_dialog.h) may speak first.
    if (dialog) if (uint16_t pid = dialog->on_bark(speaker, said)) return pid;
    // Known as the Avatar (game/avatar_title.h): an NPC says the title for the Avatar's name.
    const std::string text = dialog ? dialog->speech(speaker, said) : said;
    game_log.on_bark(*this, speaker, text);  // the HUD's message log (game/game_log.h)
    close_bark(speaker);
    Bark b;
    b.speaker = speaker;
    b.font = bark_font(speaker);
    b.pages = layout_text(font(b.font), text, 194, 55);
    if (b.pages.empty()) b.pages.emplace_back();
    b.page = 0;
    int speed = (!stasis && talk_speed() < 10) ? 10 : talk_speed();
    b.ticks_left = std::max(1u, b.pages[0].chars) * 480 / unsigned(speed);
    b.pid = kernel_.add(std::make_unique<NotifyProcess>(speaker));
    barks.push_back(std::move(b));
    return barks.back().pid;
}

uint16_t World::skip_banter(const UCProcess& proc, uint16_t speaker, const std::string& text) {
    // The original's NPC ambient scripts run as process type 0x205 (started from an NPC's
    // enter-fast-area event: the pottering about and muttering; specs/usecode/events.md): every
    // other kind of bark (looks, conversations, hatching eggs, scenes) is left alone, and so is
    // a line while a conversation or a scene runs, or an Avatar's line.
    static constexpr uint16_t kAmbientProcess = 0x205;
    if (banter_one_in <= 1 || proc.type != kAmbientProcess || stasis || in_conversation()) return 0;
    if (speaker == 1 || speaker >= 256 || proc.item != speaker) return 0;
    const UsecodeFile& uc = data_.usecode;
    // Two ambient-typed scripts are scripted moves, not talk, and one asks a question (Korick's
    // forging: any function with an answer menu is a conversation).
    const std::string cname = uc.class_name(proc.cls);
    if (cname == "ARCADION" || cname == "MOVE") return 0;
    const uint8_t* code = uc.code(proc.start_cls);
    const uint32_t size = uc.code_size(proc.start_cls);
    if (code)
        for (uint32_t i = proc.start_addr; i + 3 < size && !(code[i] == 0x79 && code[i + 1] == 0x5A); ++i)  // to the function's end
            if (code[i] == 0x0F && code[i + 2] == 0x4A && code[i + 3] == 0x00) return 0;
    if (rand_below(banter_one_in) == 0) return 0;
    int speed = talk_speed() < 10 ? 10 : talk_speed();
    const int frames = std::max(1, int(std::max<size_t>(1, text.size())) * 480 / speed / 2);
    return kernel_.add(std::make_unique<DelayProcess>(frames));
}

void World::close_bark(uint16_t speaker) {
    for (size_t i = 0; i < barks.size(); ++i)
        if (barks[i].speaker == speaker) {
            uint16_t pid = barks[i].pid;
            barks.erase(barks.begin() + long(i));
            if (Process* p = kernel_.get(pid)) p->terminate();
            return;
        }
}

void World::click_bark(uint16_t speaker) {
    for (Bark& b : barks)
        if (b.speaker == speaker) { b.ticks_left = 0; return; }
}

uint16_t World::ask(uint16_t list, uint16_t owner, uint16_t asker) {
    UCList* l = vm_.list(list);
    if (!l) return 0;
    close_bark(1);
    if (ask_menu) {
        if (Process* p = kernel_.get(ask_menu->pid)) p->terminate();
        vm_.free_slist(ask_menu->list);
    }
    auto m = std::make_unique<AskMenu>();
    m->list = vm_.dup_slist(list);
    UCList* copy = vm_.list(m->list);
    for (unsigned i = 0; i < copy->size(); ++i) m->answers.push_back(copy->word(i));
    // Engine-side topics (game/npc_dialog.h) may add their answer to an NPC's own menu.
    if (dialog && owner) dialog->on_ask(owner, asker, *m);
    const ShapeFont& f = font(6);
    int px = 0, py = 0, rowh = 0;
    for (size_t i = 0; i < m->answers.size(); ++i) {
        uint16_t sid = m->answers[i];
        std::string label = "@ " + vm_.string(sid);
        int bw = f.text_width(label), bh = f.line_advance();
        if (i + 1 == m->answers.size()) bh = f.height();
        if (px + bw > 160 && px != 0) { px = 0; py = m->h; rowh = 0; }
        m->buttons.push_back({px, py, bw, bh});
        rowh = std::max(rowh, bh);
        m->w = std::max(m->w, px + bw);
        m->h = std::max(m->h, py + bh);
        px += bw + 4;
    }
    m->pid = kernel_.add(std::make_unique<NotifyProcess>(1));
    std::string all;
    for (uint16_t sid : m->answers) all += (all.empty() ? "" : " | ") + vm_.string(sid);
    PG_LOG("ask: menu %s", all.c_str());
    ask_menu = std::move(m);
    return ask_menu->pid;
}

void World::choose_answer(unsigned index) {
    if (!ask_menu || index >= ask_menu->answers.size()) return;
    // The engine-side topics' own answer in an NPC's menu starts their tree instead.
    if (dialog && dialog->intercept(index)) return;
    uint16_t sid = ask_menu->answers[index];
    // The chosen string now belongs to usecode: take it out of the copy, free the rest.
    if (UCList* l = vm_.list(ask_menu->list)) {
        UCList keep;
        for (unsigned i = 0; i < l->size(); ++i)
            if (l->word(i) != sid) keep.push_word(l->word(i));
        l->data = keep.data;
    }
    vm_.free_slist(ask_menu->list);
    if (Process* p = kernel_.get(ask_menu->pid)) {
        p->result = sid;
        p->terminate();
    }
    PG_LOG("ask: answered \"%s\"", vm_.string(sid).c_str());
    game_log.on_answer(*this, vm_.string(sid));  // the HUD's message log
    ask_menu.reset();
}

// ---------------------------------------------------------------------------------------------
// A fight ends a conversation (NOTES.md "Combat ends a conversation")

bool World::in_conversation() const {
    if (ask_menu) return true;
    if (dialog && dialog->active()) return true;
    if (party && party->talking()) return true;
    if (!talk_pid) return false;
    const Process* p = kernel_.get(talk_pid);
    return p && p->live() && dynamic_cast<const UCProcess*>(p);
}

bool World::combat_interrupts_conversation(uint16_t cause, const char* why) {
    if (!in_conversation()) return false;
    const Process* run = kernel_.running();
    // The conversation's own script starts the fight ("then die!"): the scene goes on.
    if (run && talk_pid && run->pid() == talk_pid) {
        PG_LOG("talk: the conversation starts a fight itself (%s): it goes on", why);
        talk_pid = talk_item = 0;
        return false;
    }
    // While the answer menu is up the conversation only waits for the player: it ends. Between
    // menus (someone is speaking) a fight started by a script, or by the one talked to, belongs
    // to the scene.
    if (!ask_menu && !(party && party->talking())) {
        if (cause && cause == talk_item) return false;
        if (run && dynamic_cast<const UCProcess*>(run)) return false;
    }
    end_conversation(why);
    return true;
}

void World::end_conversation(const char* why) {
    uint16_t pid = talk_pid, who = talk_item;
    talk_pid = talk_item = 0;
    ++conversations_ended;
    PG_LOG("talk: %s: the conversation ends (process %u, item %u)", why, pid, who);
    if (dialog) dialog->abort(why);  // the engine-side topics: the NPC's own menu is put back first
    if (party && party->talking()) {
        int i = party->talk_companion();
        uint16_t cid = i >= 0 && size_t(i) < party->state.size() ? party->state[size_t(i)].id : 0;
        party->end_talk();
        if (cid) close_bark(cid);
    }
    Process* p = pid ? kernel_.get(pid) : nullptr;
    bool script = p && p->live() && dynamic_cast<UCProcess*>(p);
    // The speech it is waiting for, and the lines of the Avatar and the one talked to.
    for (size_t i = 0; i < barks.size();) {
        const Bark& b = barks[i];
        const Process* bp = kernel_.get(b.pid);
        bool waited = script && bp && std::find(bp->waiters().begin(), bp->waiters().end(), pid) != bp->waiters().end();
        if (waited || b.speaker == 1 || (who && b.speaker == who)) {
            uint16_t bpid = b.pid;
            barks.erase(barks.begin() + long(i));
            if (Process* q = kernel_.get(bpid)) q->terminate();
            continue;
        }
        ++i;
    }
    // The script ends where it waits (like walking away mid-conversation); whatever waited for
    // it (the use event that started the talk) runs on.
    if (script) p->terminate();
    if (ask_menu) {
        vm_.free_slist(ask_menu->list);
        if (Process* q = kernel_.get(ask_menu->pid)) q->terminate();
        ask_menu.reset();
    }
    // Stasis (the conversation's, usually set by a helper it started) would never be lifted now,
    // unless the script that set it still runs (a scene around the conversation: it lifts it).
    if (stasis) {
        const auto* owner = dynamic_cast<const UCProcess*>(kernel_.get(stasis_pid));
        if (!(owner && owner->live() && owner->cls == stasis_cls && stasis_pid != pid)) {
            stasis = false;
            PG_LOG("talk: the conversation's stasis lifted");
        }
    }
}

uint16_t World::open_reader(ReaderGump::Kind kind, uint16_t it, unsigned gump_shape, const std::string& text) {
    if (ReaderGump* old = reader()) old->close();
    gumps.reap();
    auto g = std::make_unique<ReaderGump>(*this, kind, it, gump_shape, text);
    auto np = std::make_unique<NotifyProcess>(it);
    np->run_while_paused = true;
    g->notify_pid = kernel_.add(std::move(np));
    uint16_t pid = g->notify_pid;
    gumps.add(std::move(g));
    return pid;
}

uint16_t World::target_pick() {
    if (Process* p = kernel_.get(target_pid)) p->terminate();
    target_pid = kernel_.add(std::make_unique<NotifyProcess>(0));
    return target_pid;
}

void World::update_ui() {
    // Barks count down in 60 Hz ticks while the world runs.
    if (!kernel_.paused()) {
        for (size_t i = 0; i < barks.size();) {
            Bark& b = barks[i];
            Process* p = kernel_.get(b.pid);
            if (!p || !p->live()) { barks.erase(barks.begin() + long(i)); continue; }
            b.ticks_left -= 2;
            if (b.ticks_left <= 0) {
                if (b.page + 1 < b.pages.size()) {
                    ++b.page;
                    b.ticks_left = int(std::max(1u, b.pages[b.page].chars) * 480 / unsigned(std::max(10, talk_speed())));
                } else {
                    uint16_t pid = b.pid;
                    barks.erase(barks.begin() + long(i));
                    if (Process* q = kernel_.get(pid)) q->terminate();
                    continue;
                }
            }
            ++i;
        }
    }
    if (ask_menu) {
        Process* p = kernel_.get(ask_menu->pid);
        if (!p || !p->live()) { vm_.free_slist(ask_menu->list); ask_menu.reset(); }
        else if (++ask_menu->frames_open >= 45 && auto_ui && !ask_menu->answers.empty()) {
            // Headless: take the first answer not given before, else the last one (often "bye").
            std::vector<std::string>& given = auto_ui_given();
            unsigned pick = unsigned(ask_menu->answers.size() - 1);
            for (unsigned i = 0; i < ask_menu->answers.size(); ++i)
                if (std::find(given.begin(), given.end(), vm_.string(ask_menu->answers[i])) == given.end()) { pick = i; break; }
            given.push_back(vm_.string(ask_menu->answers[pick]));
            choose_answer(pick);
        }
    }
    if (target_pid) {
        Process* p = kernel_.get(target_pid);
        if (!p || !p->live()) target_pid = 0;
        else if (auto_ui) { p->result = 0; p->terminate(); target_pid = 0; }
    }
}

std::vector<std::string>& World::auto_ui_given() {
    static std::vector<std::string> given;
    return given;
}

uint16_t World::start_fade(const PaletteTransform& to, int frames, int priority, const PaletteTransform* from) {
    if (Process* p = kernel_.get(fade_pid); p && p->live()) {
        if (fade_priority > priority) return 0;
        p->terminate();
    }
    fade_pid = kernel_.add(std::make_unique<FadeProcess>(*this, from ? *from : palette, to, frames));
    fade_priority = priority;
    return fade_pid;
}

// ---------------------------------------------------------------------------------------------
// Gumps (gumps.md 2.5, 2.6; intrinsics 0x04C / 0x04D)

void World::open_gump(uint16_t id, unsigned gump_shape) {
    Item* it = item(id);
    if (!it || gumps.container_gump(id)) return;
    std::unique_ptr<ContainerGump> g;
    if (id == 1 || (it->ext_flags & kExtCompanion)) g = std::make_unique<PaperdollGump>(*this, id);  // companions too
    else g = std::make_unique<ContainerGump>(*this, id, gump_shape);
    g->follow_item();
    g->place_contents();
    gumps.add(std::move(g));
    it->flags |= kItemGumpOpen;
    // The direct contents count as being in the fast area while the window is open, the
    // paperdoll's too (gumps.md 2.6 'Fast-area events').
    {
        std::vector<uint16_t> inner = it->contents;
        for (uint16_t k : inner) {
            Item* c = item(k);
            if (!c || (c->flags & kItemFastArea)) continue;
            item_enter_fast(*c);
            if ((c = item(k))) c->flags |= kItemFastArea;
        }
    }
}

void World::close_gump(uint16_t id) {
    if (Gump* g = gumps.container_gump(id)) g->close();
    gumps.reap();
    if (Item* it = item(id)) it->flags &= uint16_t(~kItemGumpOpen);
}

void World::use_item(uint16_t id) {
    Item* it = item(id);
    if (!it) return;
    Actor* a = it->as_actor();
    if (!a && !can_reach(1, id, 128)) {
        refuse("Too far away");  // the red cross for 250 ms, the reason a little longer
        return;
    }
    if (a && a->is(Actor::kDead)) {  // the body's container window (items.md 5)
        if (gumps.container_gump(id)) close_gump(id);
        else open_gump(id, 12);
        return;
    }
    call_event(id, kEvUse);
}

std::string World::put_in_refusal(uint16_t id, uint16_t cont) const {
    const Item* it = item(id);
    const Item* c = item(cont);
    if (!it || !c || !is_container(*c) || id < 256) return "It won't go in there";
    if (it->parent == cont) return "";
    for (const Item* p = c; p; p = p->parent ? item(p->parent) : nullptr)
        if (p == it) return "It can't go inside itself";
    if (!c->as_actor()) {
        unsigned used = 0;
        for (uint16_t k : c->contents)
            if (const Item* ci = item(k); ci && k != id) used += item_volume(*ci);
        unsigned cap = data_.typeflags.get(c->shape).volume();
        if (cap == 0) cap = 32;
        if (used + item_volume(*it) > cap) return "Not enough room in there";
    }
    // Carrying limits: the Avatar's (avatar.md 8.5), and the same rule for companions.
    const Item* croot = root_container(*c);
    const Actor* av = croot->id < 256 ? croot->as_actor() : nullptr;
    uint32_t own = (av && croot->id != 1) ? item_weight(*av) : 0;  // a companion's body is not luggage
    if (av && (croot->id == 1 || (croot->ext_flags & kExtCompanion)) && root_container(*it) != croot &&
        total_weight(*av) - own + total_weight(*it) > uint32_t(40 * std::max<int>(0, av->str))) {
        const uint32_t now = total_weight(*av) - own + total_weight(*it), cap = uint32_t(40 * std::max<int>(0, av->str));
        const std::string who = croot->id == 1 ? "you" : (av->name.empty() ? "them" : av->name);
        return "Too heavy for " + who + ": load " + std::to_string(now / 10) + ", limit " + std::to_string(cap / 10);
    }
    return "";
}

bool World::can_equip(uint16_t actor_id, uint16_t id) const {
    const Item* it = item(id);
    const Actor* a = actor(actor_id);
    if (!it || !a || id < 256) return false;
    unsigned slot = data_.typeflags.get(it->shape).equip_type();
    if (!slot || slot > 6) return false;
    if (it->parent == actor_id && (it->flags & kItemEquipped)) return true;
    for (const Item* p = a; p; p = p->parent ? item(p->parent) : nullptr)
        if (p == it) return false;
    if ((actor_id == 1 || (a->ext_flags & kExtCompanion)) && root_container(*it)->id != actor_id &&
        total_weight(*a) - (actor_id == 1 ? 0 : item_weight(*a)) + total_weight(*it) > uint32_t(40 * std::max<int>(0, a->str)))
        return false;
    // An occupied slot refuses the item (gumps.md 2.5 'Occupied slot': no swap).
    uint16_t old = equipped(actor_id, slot);
    if (old && old != id) return false;
    return true;
}

bool World::equip(uint16_t actor_id, uint16_t id) {
    if (!can_equip(actor_id, id)) return false;
    Item* it = item(id);
    unsigned slot = data_.typeflags.get(it->shape).equip_type();
    if (it->parent == actor_id && (it->flags & kItemEquipped)) return true;
    uint16_t old = equipped(actor_id, slot);
    if (old && old != id) {
        uint16_t pack = equipped(actor_id, 7);
        if (!move_to_container(old, pack, true)) return false;
    }
    if (!move_to_container(id, actor_id)) return false;
    if ((it = item(id))) {
        it->flags = uint16_t((it->flags & ~kItemContained) | kItemEquipped);
        it->z = int(slot);
        it->y = 0;
    }
    if (leveling) leveling->on_equip(actor_id, id);  // Leveling: a heavy weapon is told about
    return true;
}

void World::screen_point(const Item& it, int& sx, int& sy) const {
    int cam[3];
    camera_point(cam);
    int x, y, z;
    location(it, x, y, z);
    if (screen_mapper) { screen_mapper(x, y, z, sx, sy); return; }
    int camSx = (cam[0] - cam[1]) / 4, camSy = (cam[0] + cam[1]) / 8 - cam[2];
    sx = 160 + (x - y) / 4 - camSx;
    sy = 100 + (x + y) / 8 - z - camSy;
}

std::vector<World::OptionRowFactory>& World::extra_option_rows() {
    static std::vector<OptionRowFactory> rows;
    return rows;
}

uint16_t World::play_movie(MovieKind kind, bool from_menu) {
    const Flex& f = kind == kMovieIntro ? data_.intro : data_.endgame;
    if (f.count() < 2) {
        PG_LOG("movie: the %s movie is not in this copy", kind == kMovieIntro ? "intro" : "ending");
        return 0;
    }
    bool keep_music = kind == kMovieIntro && !from_menu;
    auto g = std::make_unique<MovieGump>(*this, f, keep_music, from_menu ? 30 : 0);
    if (kind == kMovieEndgame && !from_menu)
        g->on_end = [this](bool) {
            // Virtues (PLAN-VIRTUES.md 6): the epilogue the Avatar's deeds earned, then the menu.
            std::vector<std::string> page = virtues && virtues->enabled ? virtues->epilogue_page() : std::vector<std::string>{};
            if (page.empty()) { open_menu(MenuGump::kMenu); return; }
            PG_LOG("epilogue: %s (virtue total %d)", virtues->ending()->id.c_str(), virtues->total());
            auto* c = static_cast<CreditsGump*>(gumps.add(std::make_unique<CreditsGump>(*this, page, 51)));
            c->on_end = [this](bool) { open_menu(MenuGump::kMenu); };
        };
    auto np = std::make_unique<NotifyProcess>(0);
    np->run_while_paused = true;
    g->notify_pid = kernel_.add(std::move(np));
    uint16_t pid = g->notify_pid;
    PG_LOG("movie: %s%s", kind == kMovieIntro ? "intro" : "endgame", from_menu ? " (from the menu)" : "");
    gumps.add(std::move(g));
    return pid;
}

MenuGump* World::open_menu(MenuGump::Mode mode) {
    if (MenuGump* m = gumps.find<MenuGump>()) { m->set_mode(mode); return m; }
    return static_cast<MenuGump*>(gumps.add(std::make_unique<MenuGump>(*this, mode)));
}

bool World::can_save(const Gump* except) const {
    if (dialog && dialog->active()) return false;  // engine-side topics are not saved
    for (auto& g : gumps.list)
        if (!g->closed && g->modal && g.get() != except) return false;
    if (startup_state == 0 && !skip_opening) return false;
    if (Actor* av = avatar(); av && av->is(Actor::kDead)) return false;
    bool blocked = false;
    kernel_.for_each([&](Process* p) { if (p->live() && p->prevents_saving) blocked = true; });
    return !blocked;
}

// ---------------------------------------------------------------------------------------------
// Actor actions

uint16_t World::do_anim(uint16_t id, unsigned action, int dir, int steps) {
    Actor* a = actor(id);
    if (!a) return 0;
    if (fast_init_ && a->chunk >= 0 && !(a->flags & kItemFastArea)) return 0;  // actors.md 6.3
    return kernel_.add(std::make_unique<ActorAnimProcess>(*this, id, action, dir, steps));
}

uint16_t World::pathfind(uint16_t id, int x, int y, int z, uint16_t target, int range) {
    if (!actor(id)) return 0;
    kernel_.kill(id, kProcPathfind);
    (void)range;
    return kernel_.add(std::make_unique<PathfindProcess>(*this, id, x, y, z, target));
}

uint16_t World::set_activity(uint16_t id, unsigned activity) {
    Actor* a = actor(id);
    if (!a) return 0;
    a->activity = uint16_t(activity);
    switch (activity) {
    case 0:
        kernel_.kill(id, kProcLoiter);
        kernel_.kill(id, kProcPathfind);
        kernel_.add(std::make_unique<LoiterProcess>(*this, id, -1));
        return kernel_.add(std::make_unique<DelayProcess>(1));
    case 1: set_in_combat(id); return 0;
    case 2: return do_anim(id, 2, a->dir);
    default: return 0;
    }
}

void World::set_in_combat(uint16_t id) {
    Actor* a = actor(id);
    if (!a || a->is(Actor::kInCombat)) return;
    if (id == 1) {
        a->set(Actor::kInCombat, true);
        sound.combat_music(98);
        combat_interrupts_conversation(0, "combat mode");
        return;
    }
    if (creatures && creatures->is_creature(id)) {  // the new creatures fight by their own brain
        a->set(Actor::kInCombat, true);
        return;
    }
    kernel_.kill(id, kProcAny);
    uint16_t cast = call_event_w(id, kEvCast, 0);
    auto cp = std::make_unique<CombatProcess>(*this, id);
    uint16_t cpid = kernel_.add(std::move(cp));
    if (cast)
        if (Process* p = kernel_.get(cpid)) p->wait_for(cast);
    if ((a = actor(id))) a->set(Actor::kInCombat, true);
}

void World::clear_in_combat(uint16_t id) {
    Actor* a = actor(id);
    if (!a || !a->is(Actor::kInCombat)) return;
    kernel_.kill(id, kProcCombat);
    a->set(Actor::kInCombat, false);
    if (id == 1) sound.music_restore();
}

}  // namespace pg
