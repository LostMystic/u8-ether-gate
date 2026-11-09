// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Engine-side dialogue topics added to original NPCs (game/npc_dialog.h): the data files, the hook
// into the NPC's own answer menu, and the topic tree (barks + the answer menu).
#include "data/compat.h"
#include "game/npc_dialog.h"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <sstream>

#include "core/bugfix.h"
#include "core/config.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/ask_nav.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/processes.h"
#include "game/stat_growth.h"
#include "game/virtues.h"
#include "game/world.h"
#include "json/json.hpp"
#include "usecode/uc_machine.h"

namespace pg {

using nlohmann::json;

namespace {

std::vector<std::string> str_list(const json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& s : j)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

DialogCond parse_cond(const json& j) {
    DialogCond c;
    if (!j.is_object()) return c;
    c.empty = false;
    if (j.contains("stage")) c.stage_in = str_list(j["stage"]);
    c.from_stage = j.value("from_stage", std::string());
    c.before_stage = j.value("before_stage", std::string());
    if (j.contains("step_done")) c.step_done = str_list(j["step_done"]);
    if (j.contains("step_not_done")) c.step_not_done = str_list(j["step_not_done"]);
    if (j.contains("asked")) c.asked = str_list(j["asked"]);
    if (j.contains("not_asked")) c.not_asked = str_list(j["not_asked"]);
    c.avatar_int = j.value("avatar_int", 0);
    c.virtue_total = j.value("virtue_total", 0);
    if (j.contains("mark")) c.marked = str_list(j["mark"]);
    if (j.contains("not_mark")) c.not_marked = str_list(j["not_mark"]);
    if (j.contains("cond")) {
        const json& k = j["cond"];
        if (k.is_array()) for (const json& e : k) c.cond.push_back(crit_cond_from_text(e.dump()));
        else c.cond.push_back(crit_cond_from_text(k.dump()));
    }
    return c;
}

std::vector<std::pair<DialogCond, std::vector<std::string>>> parse_alts(const json& j) {
    std::vector<std::pair<DialogCond, std::vector<std::string>>> out;
    if (j.is_array())
        for (const json& a : j) out.push_back({parse_cond(a.value("when", json::object())), str_list(a.value("say", json::array()))});
    return out;
}

DialogTopic parse_topic(const json& j) {
    DialogTopic t;
    t.id = j.value("id", std::string());
    t.label = j.value("label", t.id);
    if (j.contains("when")) t.when = parse_cond(j["when"]);
    if (j.contains("say")) t.say = str_list(j["say"]);
    if (j.contains("alt")) t.alt = parse_alts(j["alt"]);
    t.leave = j.value("leave", false);
    t.slot = j.value("slot", std::string());
    t.feature = j.value("feature", std::string());
    t.action = j.value("action", std::string());
    if (j.contains("virtue") && j["virtue"].is_object())
        for (auto it = j["virtue"].begin(); it != j["virtue"].end(); ++it)
            if (int v = Virtues::index_of(it.key()); v >= 0 && it.value().is_number_integer()) t.virtue.emplace_back(v, it.value().get<int>());
    t.cost = j.value("cost", 0);
    if (j.contains("cant_pay")) t.cant_pay = str_list(j["cant_pay"]);
    t.mark = j.value("mark", std::string());
    t.vice = j.value("vice", 0);
    if (j.contains("topics"))
        for (const json& s : j["topics"]) t.topics.push_back(parse_topic(s));
    return t;
}

}  // namespace

NpcDialog::NpcDialog(World& w) : w_(w) {}
NpcDialog::~NpcDialog() {
    if (hooked_) w_.vm().text_hook = nullptr;
}

bool NpcDialog::load_text(const std::string& text, std::string& err) {
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    if (!j.is_object()) { err = "not an object"; return false; }
    NpcDialogDef d;
    d.id = j.value("id", std::string());
    d.name = j.value("name", d.id);
    d.npc = j.value("npc", 0u);
    d.shape = j.value("shape", 0u);
    if (!d.npc && !d.shape) { err = "no npc number or shape"; return false; }
    if (j.contains("menu")) {
        const json& h = j["menu"];
        d.max_entries = std::max(1, h.value("max_entries", 1));
        d.hook_first = h.value("first_menu", true);
        d.hook_exits = h.value("exits_only_menu", true);
        d.back_label = h.value("back", d.back_label);
        d.leave_label = h.value("leave", d.leave_label);
    }
    for (const json& s : j.value("stages", json::array())) {
        DialogStage st;
        st.id = s.value("id", std::string());
        st.title = s.value("title", st.id);
        st.when = parse_cond(s.value("when", json::object()));
        d.stages.push_back(st);
    }
    for (const json& t : j.value("topics", json::array())) d.topics.push_back(parse_topic(t));
    for (const json& h : j.value("hide", json::array()))
        if (h.is_object()) d.hide.emplace_back(h.value("text", std::string()), h.value("feature", std::string()));
    for (const json& sc : j.value("scenes", json::array())) {
        DialogScene x;
        x.id = sc.value("id", std::string());
        x.cls = sc.value("class", std::string());
        if (sc.contains("from_ip")) x.from_ip = int(std::stoul(sc.value("from_ip", std::string("0")), nullptr, 16));
        x.when = parse_cond(sc.value("when", json::object()));
        x.topic = parse_topic(sc);
        x.topic.id = x.id;
        x.line = sc.value("line", std::string());
        x.feature = sc.value("feature", std::string());
        x.replace = sc.value("replace", false);
        if (!x.replace)
            for (DialogTopic& t : x.topic.topics) t.leave = true;
        if (x.cls.empty()) { err = "scene " + x.id + " has no class"; return false; }
        d.scenes.push_back(std::move(x));
    }
    int old = -1;
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].npc == d.npc && defs[i].shape == d.shape) old = int(i);
    if (old >= 0) defs[size_t(old)] = std::move(d);
    else defs.push_back(std::move(d));
    return true;
}

bool NpcDialog::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (data_dir.empty()) {
        // data/dialog/x.json -> data
        size_t s1 = path.find_last_of('/');
        std::string dir = s1 == std::string::npos ? "." : path.substr(0, s1);
        size_t s2 = dir.find_last_of('/');
        data_dir = s2 == std::string::npos ? "." : dir.substr(0, s2);
    }
    if (!load_text(ss.str(), err)) { err = path + ": " + err; return false; }
    return true;
}

bool NpcDialog::load_dir(const std::string& dir, std::string& err) {
    std::vector<std::string> files;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && n != "index.json") files.push_back(n);
        }
        closedir(d);
    } else {
        err = "cannot open " + dir;
        return false;
    }
    std::sort(files.begin(), files.end());
    size_t s = dir.find_last_of('/');
    data_dir = s == std::string::npos ? "." : dir.substr(0, s);
    bool ok = true;
    for (const std::string& n : files) {
        std::string e;
        if (!load_file(dir + "/" + n, e)) { PG_LOG("dialog: %s", e.c_str()); err = e; ok = false; }
    }
    // Only for the NPCs and scenes the writing was made for: another copy's are left as they are.
    for (size_t i = defs.size(); i-- > 0;) {
        NpcDialogDef& d = defs[i];
        if (!compat::class_known(1024 + d.npc)) {
            compat::off("new dialogue for " + d.id, "that NPC's usecode class differs in this copy");
            defs.erase(defs.begin() + long(i));
            continue;
        }
        const UsecodeFile& uc = w_.data().usecode;
        for (size_t k = d.scenes.size(); k-- > 0;) {
            bool fits = false;
            for (unsigned c = 0; c < uc.class_count(); ++c)
                if (uc.class_name(c) == d.scenes[k].cls) fits = compat::class_known(c);
            if (!fits) {
                compat::off("scene patch " + d.scenes[k].id, "class " + d.scenes[k].cls + " differs in this copy");
                d.scenes.erase(d.scenes.begin() + long(k));
            }
        }
    }
    PG_LOG("dialog: %zu NPC file(s) from %s", defs.size(), dir.c_str());
    // "Everyone knows" (game/avatar_title.h): the rewrites of the usecode's words for the Avatar.
    std::string terr;
    std::string tp = data_dir + "/avatar_title.json";
    if (std::ifstream(tp).good() && !title.load_file(tp, terr)) { PG_LOG("dialog: %s", terr.c_str()); err = terr; ok = false; }
    if (w_.dialog.get() == this && !hooked_) {
        hooked_ = true;
        w_.vm().text_hook = [this](uint16_t cls, std::string& text) {
            if (title_on()) title.rewrite(w_.data().usecode.class_name(cls), text);
        };
    }
    return ok;
}

int NpcDialog::find_def(unsigned npc) const {
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].npc && defs[i].npc == npc) return int(i);
    if (const Actor* a = w_.actor(uint16_t(npc)))  // a file for a kind of actor (by shape)
        for (size_t i = 0; i < defs.size(); ++i)
            if (!defs[i].npc && defs[i].shape == a->shape) return int(i);
    return -1;
}

void NpcDialog::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World& w) {
        NpcDialog* d = w.dialog.get();
        MenuGump::OptionRow r{"New Dialogue:", [d] { return std::string(d && d->enabled ? "On" : "Off"); },
                              [d](int) {
                                  if (!d) return;
                                  if (d->active()) d->abort("the option was changed");
                                  d->enabled = !d->enabled;
                                  Config& c = Config::global();
                                  c.set_bool("npc_dialog", d->enabled);
                                  if (!c.path().empty()) c.save();
                              }};
        r.group = "Game";
        return r;
    });
}

bool NpcDialog::feature_on(const std::string& feature) const {
    if (feature.empty()) return enabled;
    if (feature == "leveling") return w_.leveling && w_.leveling->active();
    if (feature == "virtues") return w_.virtues && w_.virtues->enabled;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Conditions and stages

bool NpcDialog::step_done(const std::string& id) const {
    if (!crit_tried_) {
        crit_tried_ = true;
        crit_ = std::make_unique<CritPath>();
        std::string err;
        std::string p = (data_dir.empty() ? std::string("data") : data_dir) + "/critpath.json";
        if (!crit_->load_file(p, err)) PG_LOG("dialog: %s: %s", p.c_str(), err.c_str());
    }
    if (!crit_ || crit_->empty() || id.empty()) return false;
    int k = crit_->step_index(id);
    if (k < 0) {
        PG_LOG("dialog: unknown critical-path step '%s'", id.c_str());
        return false;
    }
    uint32_t f = w_.kernel().frame_number();
    if (f != crit_frame_ || crit_done_.empty()) {
        WorldCritContext ctx(w_);
        crit_done_ = crit_->complete(crit_->done(ctx));
        crit_frame_ = f;
    }
    return size_t(k) < crit_done_.size() && crit_done_[size_t(k)];
}

namespace {
int stage_index(const NpcDialogDef& d, const std::string& id) {
    for (size_t i = 0; i < d.stages.size(); ++i)
        if (d.stages[i].id == id) return int(i);
    return -1;
}
}  // namespace

int NpcDialog::stage(int def) const {
    if (def < 0 || size_t(def) >= defs.size()) return -1;
    const NpcDialogDef& d = defs[size_t(def)];
    int cur = -1;
    for (size_t i = 0; i < d.stages.size(); ++i) {
        const DialogCond& c = d.stages[i].when;
        // Stage conditions cannot refer to stages.
        bool ok = true;
        for (const std::string& s : c.step_done) ok = ok && step_done(s);
        for (const std::string& s : c.step_not_done) ok = ok && !step_done(s);
        if (ok && !c.cond.empty()) {
            WorldCritContext ctx(w_);
            for (const CritCond& k : c.cond) ok = ok && k.eval(ctx);
        }
        if (ok) cur = int(i);
    }
    return cur;
}

std::string NpcDialog::stage_id(int def) const {
    int s = stage(def);
    return s >= 0 ? defs[size_t(def)].stages[size_t(s)].id : std::string();
}

bool NpcDialog::eval(int def, const DialogCond& c) const {
    if (c.empty) return true;
    const NpcDialogDef& d = defs[size_t(def)];
    if (!c.stage_in.empty() || !c.from_stage.empty() || !c.before_stage.empty()) {
        int s = stage(def);
        std::string sid = s >= 0 ? d.stages[size_t(s)].id : std::string();
        if (!c.stage_in.empty() && std::find(c.stage_in.begin(), c.stage_in.end(), sid) == c.stage_in.end()) return false;
        if (!c.from_stage.empty()) {
            int k = stage_index(d, c.from_stage);
            if (k < 0 || s < k) return false;
        }
        if (!c.before_stage.empty()) {
            int k = stage_index(d, c.before_stage);
            if (k >= 0 && s >= k) return false;
        }
    }
    for (const std::string& s : c.step_done)
        if (!step_done(s)) return false;
    for (const std::string& s : c.step_not_done)
        if (step_done(s)) return false;
    for (const std::string& a : c.asked)
        if (!asked_.count(d.id + ":" + a)) return false;
    for (const std::string& a : c.not_asked)
        if (asked_.count(d.id + ":" + a)) return false;
    for (const std::string& m : c.marked)
        if (!w_.virtues || !w_.virtues->marked(m)) return false;
    for (const std::string& m : c.not_marked)
        if (w_.virtues && w_.virtues->marked(m)) return false;
    if (c.virtue_total > 0 && (!w_.virtues || w_.virtues->total() < c.virtue_total)) return false;
    if (c.avatar_int > 0) {  // INT-gated topics (PLAN-LEVELING.md 5): the Avatar's INT, Leveling on or off
        const Actor* av = w_.avatar();
        if (!av || av->intel < c.avatar_int) return false;
    }
    if (!c.cond.empty()) {
        WorldCritContext ctx(w_);
        for (const CritCond& k : c.cond)
            if (!k.eval(ctx)) return false;
    }
    return true;
}

std::vector<const DialogTopic*> NpcDialog::visible_topics(int def, const std::vector<DialogTopic>& menu) const {
    std::vector<const DialogTopic*> out;
    for (const DialogTopic& t : menu)
        if (eval(def, t.when)) out.push_back(&t);
    return out;
}

std::vector<const DialogTopic*> NpcDialog::entries(int def) const {
    if (def < 0 || size_t(def) >= defs.size()) return {};
    const NpcDialogDef& d = defs[size_t(def)];
    // Per slot the last visible topic in file order; slots ordered by that topic's place.
    std::vector<std::pair<std::string, size_t>> slots;  // slot, topic index
    for (size_t i = 0; i < d.topics.size(); ++i) {
        const DialogTopic& t = d.topics[i];
        if (!feature_on(t.feature) || !eval(def, t.when)) continue;
        std::string sl = t.slot.empty() ? t.id : t.slot;
        auto it = std::find_if(slots.begin(), slots.end(), [&](const auto& p) { return p.first == sl; });
        if (it != slots.end()) slots.erase(it);
        slots.push_back({sl, i});
    }
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (slots.size() > size_t(d.max_entries)) slots.erase(slots.begin(), slots.end() - d.max_entries);
    std::vector<const DialogTopic*> out;
    for (const auto& p : slots) out.push_back(&d.topics[p.second]);
    return out;
}

std::vector<const DialogTopic*> NpcDialog::visible_topics() const {
    if (!active() || talk_.menu_stack.empty()) return {};
    return visible_topics(talk_.def, *talk_.menu_stack.back());
}

const std::vector<std::string>* NpcDialog::pick_lines(int def, const std::vector<std::pair<DialogCond, std::vector<std::string>>>& alts,
                                                      const std::vector<std::string>* fallback) const {
    for (const auto& a : alts)
        if (eval(def, a.first)) return &a.second;
    return fallback;
}

std::string NpcDialog::fill(const std::string& in) const {
    std::string s = in;
    for (size_t a; (a = s.find("{avatar}")) != std::string::npos;) s.replace(a, 8, w_.avatar_name);
    return s;
}

std::vector<std::string> NpcDialog::problems() const {
    std::vector<std::string> out;
    step_done("");  // loads the critical path
    for (const NpcDialogDef& d : defs) {
        auto cond = [&](const DialogCond& c, const std::string& where) {
            for (const auto* l : {&c.step_done, &c.step_not_done})
                for (const std::string& s : *l)
                    if (!crit_ || crit_->step_index(s) < 0) out.push_back(d.id + " " + where + ": unknown step " + s);
            std::vector<std::string> st = c.stage_in;
            if (!c.from_stage.empty()) st.push_back(c.from_stage);
            if (!c.before_stage.empty()) st.push_back(c.before_stage);
            for (const std::string& s : st)
                if (stage_index(d, s) < 0) out.push_back(d.id + " " + where + ": unknown stage " + s);
            for (const CritCond& k : c.cond)
                if (k.describe().find('?') != std::string::npos) out.push_back(d.id + " " + where + ": bad condition " + k.describe());
        };
        for (const DialogStage& s : d.stages) cond(s.when, "stage " + s.id);
        std::function<void(const std::vector<DialogTopic>&, const std::string&)> walk = [&](const std::vector<DialogTopic>& ts, const std::string& path) {
            for (const DialogTopic& t : ts) {
                std::string w = path + "/" + t.id;
                if (t.label.empty() || t.id.empty()) out.push_back(d.id + " " + w + ": no id or label");
                if (t.say.empty() && t.alt.empty() && t.topics.empty() && t.action.empty()) out.push_back(d.id + " " + w + ": says nothing");
                cond(t.when, w);
                for (const auto& a : t.alt) cond(a.first, w + " alt");
                walk(t.topics, w);
            }
        };
        walk(d.topics, "");
        for (const DialogScene& sc : d.scenes) {
            cond(sc.when, "scene " + sc.id);
            bool found = false;
            for (unsigned c = 0; c < w_.data().usecode.class_count(); ++c)
                if (w_.data().usecode.class_name(c) == sc.cls) found = true;
            if (!found) out.push_back(d.id + " scene " + sc.id + ": unknown class " + sc.cls);
            walk(sc.topic.topics, "scene " + sc.id);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The hook

uint16_t NpcDialog::menu_owner(const AskMenu& m) const {
    const Process* n = w_.kernel().get(m.pid);
    if (!n) return 0;
    for (uint16_t pid : n->waiters()) {
        const auto* p = dynamic_cast<const UCProcess*>(w_.kernel().get(pid));
        if (p && p->live()) return p->item;
    }
    return 0;
}

void NpcDialog::on_ask(uint16_t owner, uint16_t pid, AskMenu& m) {
    if (active() || !owner) return;
    int def = find_def(owner);
    if (def < 0) return;
    const NpcDialogDef& d = defs[size_t(def)];
    UCMachine& vm = w_.vm();
    // Answers of the NPC's own that a feature replaces (only out of the menu shown: the string stays
    // in the menu's list and is freed with it, so the usecode never sees a difference).
    auto trimmed = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        return s;
    };
    for (const auto& h : d.hide) {
        if (!feature_on(h.second)) continue;
        for (size_t k = m.answers.size(); k-- > 0;)
            if (trimmed(vm.string(m.answers[k])) == trimmed(h.first)) {
                m.answers.erase(m.answers.begin() + long(k));
                log.push_back(d.id + " hid " + trimmed(h.first));
            }
    }
    const Process* p = w_.kernel().get(pid);
    uint64_t serial = p ? p->serial() : 0;
    if (serial != conv_serial_) { conv_serial_ = serial; conv_offered_ = false; }
    bool exits_only = !m.answers.empty(), any_exit = false;
    std::set<std::string> have;
    for (uint16_t sid : m.answers) {
        std::string s = vm.string(sid);
        have.insert(s);
        int k = ask_exit_kind(s);
        if (k == 0) exits_only = false;
        if (k == 1) any_exit = true;
    }
    // The first menu of the conversation that has a goodbye (not a question like "your name?"),
    // and any menu holding only exits.
    bool first = !conv_offered_ && any_exit;
    if (first) conv_offered_ = true;
    if (!((first && d.hook_first) || (exits_only && d.hook_exits))) return;
    // Before the trailing exits ("Farewell."), so B and the last answer still leave.
    size_t at = m.answers.size();
    while (at > 0 && ask_exit_kind(vm.string(m.answers[at - 1])) == 1) --at;
    for (const DialogTopic* t : entries(def)) {
        std::string label = fill(t->label);
        if (have.count(label)) continue;
        uint16_t sid = vm.new_string(label);
        if (UCList* l = vm.list(m.list)) l->push_word(sid);
        m.answers.insert(m.answers.begin() + long(at++), sid);
        log.push_back(d.id + " entry " + t->id);
    }
}

bool NpcDialog::intercept(unsigned index) {
    AskMenu* m = w_.ask_menu.get();
    if (!m || index >= m->answers.size()) return false;
    if (active()) {
        // One of the tree's own menus: remember the pick; World::choose_answer goes on as usual.
        if (m->pid == talk_.ask_pid) talk_.picked = int(index);
        return false;
    }
    uint16_t owner = menu_owner(*m);
    int def = owner ? find_def(owner) : -1;
    if (def < 0) return false;
    const NpcDialogDef& d = defs[size_t(def)];
    std::string label = w_.vm().string(m->answers[index]);
    // The entry by its text: one shown now first, else any top-level topic (a menu from a save).
    const DialogTopic* entry = nullptr;
    for (const DialogTopic* t : entries(def))
        if (fill(t->label) == label) entry = t;
    if (!entry)
        for (const DialogTopic& t : d.topics)
            if (feature_on(t.feature) && fill(t.label) == label) { entry = &t; break; }
    if (!entry) return false;
    const Actor* npc = w_.actor(owner);
    if (!npc || npc->is(Actor::kDead)) return false;
    PG_LOG("ask: answered \"%s\" (engine dialogue, stage %s)", label.c_str(), stage_id(def).c_str());
    w_.game_log.on_answer(w_, label);
    talk_ = Talk{};
    talk_.def = def;
    talk_.npc = owner;
    talk_.held = std::move(w_.ask_menu);
    log.push_back(d.id + " start " + stage_id(def));
    choose(*entry);
    frame();
    return true;
}

// ---------------------------------------------------------------------------------------------
// The tree

void NpcDialog::say_lines(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) talk_.queue.push_back(l);
    talk_.stage = Talk::kSaying;
}

void NpcDialog::show_menu() {
    const NpcDialogDef& d = defs[size_t(talk_.def)];
    std::vector<const DialogTopic*> vis = visible_topics(talk_.def, *talk_.menu_stack.back());
    talk_.answers.clear();
    std::vector<std::string> labels;
    for (const DialogTopic* t : vis) {
        if (labels.size() >= 8) break;
        talk_.answers.push_back(t);
        labels.push_back(fill(t->label));
    }
    if (!talk_.scene_notify || talk_.menu_stack.size() > 1) {  // a scene's replies are the only answers
        talk_.answers.push_back(nullptr);
        labels.push_back(talk_.menu_stack.size() > 1 ? d.back_label : d.leave_label);
    }
    UCMachine& vm = w_.vm();
    uint16_t l = vm.new_list(2);
    for (const std::string& s : labels) vm.list(l)->push_word(vm.new_string(s));
    talk_.ask_pid = w_.ask(l);
    vm.free_slist(l);
    talk_.answer_sids = w_.ask_menu ? w_.ask_menu->answers : std::vector<uint16_t>{};
    talk_.picked = -1;
    talk_.stage = Talk::kMenu;
}

void NpcDialog::choose(const DialogTopic& t) {
    const NpcDialogDef& d = defs[size_t(talk_.def)];
    asked_.insert(d.id + ":" + t.id);
    if (t.cost > 0) {
        StatGrowth* g = w_.leveling ? w_.leveling->growth.get() : nullptr;
        if (!g || !g->pay(1, t.cost)) {
            say_lines(t.cant_pay.empty() ? std::vector<std::string>{"You do not have " + std::to_string(t.cost) + " obsidian."} : t.cant_pay);
            talk_.leave_after = true;
            log.push_back(d.id + " cannot pay " + t.id);
            return;
        }
        w_.game_log.add(GameLog::kMessages, GameLog::kInfo, "Gave " + std::to_string(t.cost) + " obsidian.", 1);
    }
    if (!t.mark.empty() && w_.virtues && w_.virtues->enabled) w_.virtues->set_mark(t.mark);
    if (w_.virtues)
        for (const auto& v : t.virtue) w_.virtues->earn(v.first, v.second, "dialog/" + d.id + ":" + t.id);
    if (w_.virtues && t.vice > 0) w_.virtues->sin(t.vice, "dialog/" + d.id + ":" + t.id);
    say_lines(*pick_lines(talk_.def, t.alt, &t.say));
    if (!t.action.empty()) {
        // The engine's lines and next menu (a trainer: who, what, the price, the session).
        DialogAction a = w_.leveling && w_.leveling->growth ? w_.leveling->growth->dialog_action(talk_.npc, t.action) : DialogAction{};
        say_lines(a.say);
        log.push_back(d.id + " action " + t.action);
        if (a.topics.empty()) {
            talk_.leave_after = true;
            return;
        }
        talk_.dyn.push_back(std::make_unique<std::vector<DialogTopic>>(std::move(a.topics)));
        talk_.menu_stack.push_back(talk_.dyn.back().get());
        return;
    }
    if (!t.topics.empty()) talk_.menu_stack.push_back(&t.topics);
    else if (talk_.menu_stack.empty()) talk_.leave_after = true;  // an entry with lines only
    if (t.leave) talk_.leave_after = true;
    log.push_back(d.id + " topic " + t.id);
}

void NpcDialog::free_menu() {
    if (w_.ask_menu && talk_.ask_pid && w_.ask_menu->pid == talk_.ask_pid) {
        w_.vm().free_slist(w_.ask_menu->list);
        if (Process* p = w_.kernel().get(w_.ask_menu->pid)) p->terminate();
        w_.ask_menu.reset();
    }
    talk_.ask_pid = 0;
    talk_.answer_sids.clear();
}

void NpcDialog::restore() {
    if (!talk_.held) { talk_ = Talk{}; return; }
    free_menu();
    if (w_.ask_menu) {  // somebody else's menu came up meanwhile: the held one cannot come back
        w_.vm().free_slist(talk_.held->list);
        if (Process* p = w_.kernel().get(talk_.held->pid)) p->terminate();
    } else {
        w_.ask_menu = std::move(talk_.held);
        w_.ask_menu->frames_open = 0;  // the click guard starts again
    }
    if (talk_.def >= 0) log.push_back(defs[size_t(talk_.def)].id + " back to the menu");
    talk_ = Talk{};
}

void NpcDialog::abort(const char* why) {
    if (!active()) return;
    PG_LOG("dialog: %s: the engine dialogue ends", why);
    log.push_back(defs[size_t(talk_.def)].id + " abort");
    free_menu();
    if (Process* p = w_.kernel().get(talk_.bark_pid); p && p->live()) w_.close_bark(talk_.npc);
    talk_.queue.clear();
    if (talk_.scene_notify) {  // the script goes on; its own line is still said
        uint16_t npc = talk_.npc, notify = talk_.scene_notify;
        std::string text = talk_.scene_text;
        bool said = talk_.final_said || talk_.scene_replace;
        if (talk_.scene_replace) silenced_.insert(talk_.scene_serial);
        talk_ = Talk{};
        if (Process* n = w_.kernel().get(notify)) n->terminate();
        if (!said && w_.actor(npc)) {
            own_bark_ = true;
            w_.bark(npc, text);
            own_bark_ = false;
        }
        return;
    }
    if (talk_.held) {
        Process* hp = w_.kernel().get(talk_.held->pid);
        if (hp && hp->live()) { restore(); return; }  // the caller deals with the NPC's own menu
        w_.vm().free_slist(talk_.held->list);
    }
    talk_ = Talk{};
}

void NpcDialog::reset() {
    talk_ = Talk{};  // the VM and kernel were reset with the world: nothing to free
    conv_serial_ = 0;
    conv_offered_ = false;
    asked_.clear();
    scenes_done_.clear();
    silenced_.clear();
}

bool NpcDialog::title_on() const {
    return !title.empty() && w_.virtues && w_.virtues->enabled && w_.virtues->marked(title.mark);
}

std::string NpcDialog::speech(uint16_t speaker, const std::string& text) const {
    // An NPC's line says the title where the script put the Avatar's name (not the Avatar's own lines).
    if (speaker == 1 || !title.name_in_speech || !title_on()) return text;
    return title.with_title(text, w_.avatar_name);
}

uint16_t NpcDialog::on_bark(uint16_t speaker, const std::string& text) {
    // Scene patches are fixes of the original: they follow the "Original bug fixes" option
    // (core/bugfix.h), not "New Dialogue".
    // Scenes of a feature ("feature": "virtues") follow that feature instead.
    if (own_bark_) return 0;
    const auto* p = dynamic_cast<const UCProcess*>(w_.kernel().running());
    // The rest of a script a "replace" scene took over is silent (a short pause per line).
    if (p && silenced_.count(p->serial())) {
        log.push_back("silenced " + text);
        return w_.kernel().add(std::make_unique<DelayProcess>(1));
    }
    if (active()) return 0;
    int def = find_def(speaker);
    if (def < 0 || defs[size_t(def)].scenes.empty()) return 0;
    if (!p || scenes_done_.count(p->serial())) return 0;
    std::string cls = w_.data().usecode.class_name(p->cls);
    const NpcDialogDef& d = defs[size_t(def)];
    auto trimmed = [](std::string t) {
        while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
        return t;
    };
    auto gate = [&](const DialogScene& sc) { return sc.feature.empty() ? original_bug_fixes() : feature_on(sc.feature); };
    bool ours = false;
    for (const DialogScene& sc : d.scenes) ours = ours || (sc.cls == cls && gate(sc));
    if (!ours) return 0;
    scenes_done_.insert(p->serial());  // only the script's first line of this NPC can be patched
    for (const DialogScene& sc : d.scenes) {
        if (sc.cls != cls || !gate(sc) || !eval(def, sc.when)) continue;
        if (!sc.line.empty() && trimmed(sc.line) != trimmed(text)) continue;
        // The first line must come from the part of the script the patch is for (a script whose
        // own fix sends it elsewhere first is left alone).
        if (sc.from_ip >= 0 && int(p->ip) < sc.from_ip) continue;
        PG_LOG("dialog: scene patch %s (class %s at %04X) before \"%s\"", sc.id.c_str(), cls.c_str(), unsigned(p->ip), text.c_str());
        talk_ = Talk{};
        talk_.def = def;
        talk_.npc = speaker;
        talk_.scene_proc = p->pid();
        talk_.scene_text = text;
        talk_.scene_notify = w_.kernel().add(std::make_unique<NotifyProcess>(speaker));
        talk_.scene_replace = sc.replace;
        talk_.scene_serial = p->serial();
        scenes_played.push_back(sc.id);
        log.push_back(d.id + " scene " + sc.id);
        say_lines(sc.topic.say);
        if (!sc.topic.topics.empty()) talk_.menu_stack.push_back(&sc.topic.topics);
        else talk_.leave_after = true;
        return talk_.scene_notify;
    }
    return 0;
}

void NpcDialog::finish_scene() {
    if (talk_.scene_replace) {  // the script goes on without a word (game/npc_dialog.h "replace")
        free_menu();
        silenced_.insert(talk_.scene_serial);
        if (Process* n = w_.kernel().get(talk_.scene_notify)) n->terminate();
        if (talk_.def >= 0) log.push_back(defs[size_t(talk_.def)].id + " scene done");
        talk_ = Talk{};
        return;
    }
    // The script's own line, then the script goes on (its wait ends with that speech).
    if (!talk_.final_said) {
        free_menu();
        talk_.final_said = true;
        own_bark_ = true;
        talk_.final_bark = w_.bark(talk_.npc, talk_.scene_text);
        own_bark_ = false;
        talk_.stage = Talk::kSaying;
        return;
    }
    if (Process* p = w_.kernel().get(talk_.final_bark); p && p->live()) return;
    if (Process* n = w_.kernel().get(talk_.scene_notify)) n->terminate();
    if (talk_.def >= 0) log.push_back(defs[size_t(talk_.def)].id + " scene done");
    talk_ = Talk{};
}

void NpcDialog::frame() {
    if (!active()) return;
    // The NPC's own conversation (or the patched scene's script) must still be waiting, the NPC
    // there and alive.
    Process* hp = talk_.scene_notify ? w_.kernel().get(talk_.scene_proc)
                                     : talk_.held ? w_.kernel().get(talk_.held->pid) : nullptr;
    const Actor* npc = w_.actor(talk_.npc);
    const Actor* av = w_.avatar();
    if (!hp || !hp->live()) { abort("the conversation ended"); return; }
    if (talk_.final_said) { finish_scene(); return; }
    if (!npc || npc->is(Actor::kDead) || !av || av->is(Actor::kDead) || npc->mapnum != av->mapnum) {
        abort("the speaker is gone");
        return;
    }
    switch (talk_.stage) {
    case Talk::kSaying: {
        if (Process* p = w_.kernel().get(talk_.bark_pid); p && p->live()) return;
        talk_.bark_pid = 0;
        if (!talk_.queue.empty()) {
            std::string line = fill(talk_.queue.front());
            talk_.queue.erase(talk_.queue.begin());
            own_bark_ = true;
            talk_.bark_pid = w_.bark(talk_.npc, line);
            own_bark_ = false;
            return;
        }
        if (talk_.leave_after) {
            if (talk_.scene_notify) finish_scene();
            else restore();
            return;
        }
        show_menu();
        return;
    }
    case Talk::kMenu: {
        Process* p = w_.kernel().get(talk_.ask_pid);
        if (p && p->live() && w_.ask_menu && w_.ask_menu->pid == talk_.ask_pid) return;
        int pick = talk_.picked;
        if (pick >= 0 && size_t(pick) < talk_.answer_sids.size()) {
            uint16_t sid = talk_.answer_sids[size_t(pick)];
            if (w_.vm().has_string(sid)) w_.vm().free_string(sid);  // handed over by choose_answer
        }
        talk_.answer_sids.clear();
        talk_.ask_pid = 0;
        talk_.picked = -1;
        if (pick < 0 || size_t(pick) >= talk_.answers.size()) { abort("the menu went away"); return; }
        const DialogTopic* t = talk_.answers[size_t(pick)];
        if (!t) {
            if (talk_.menu_stack.size() > 1) {
                talk_.menu_stack.pop_back();
                show_menu();
                return;
            }
            if (talk_.scene_notify) finish_scene();
            else restore();
            return;
        }
        choose(*t);
        frame();
        return;
    }
    default: return;
    }
}

}  // namespace pg
