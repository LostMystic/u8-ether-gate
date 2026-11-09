// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The Avatar's virtue scores (PLAN-VIRTUES.md 1; NOTES.md "Virtues"). See virtues.h.
#include "game/virtues.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/config.h"
#include "core/serial.h"
#include "game/combat.h"
#include "game/game_log.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using json = nlohmann::json;

namespace {
const char* const kNames[kVirtueCount] = {"Honesty", "Compassion", "Valor", "Justice",
                                          "Sacrifice", "Honor", "Spirituality", "Humility"};
}  // namespace

void Virtues::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World& w) {
        Virtues* v = w.virtues.get();
        MenuGump::OptionRow r{"Virtues:", [v] { return std::string(v && v->enabled ? "On" : "Off"); },
                              [v](int) {
                                  if (!v) return;
                                  v->enabled = !v->enabled;
                                  Config& cf = Config::global();
                                  cf.set_bool("virtues", v->enabled);
                                  if (!cf.path().empty()) cf.save();
                              }};
        r.group = "Game";
        return r;
    });
}

bool Virtues::load_dir(const std::string& dir, std::string& err) {
    std::ifstream f(dir + "/virtues.json", std::ios::binary);
    if (!f) { err = dir + "/virtues.json not found"; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_text(ss.str(), err);
}

bool Virtues::load_text(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "virtues.json: not valid JSON"; return false; }
    const json& m = j.contains("max") ? j["max"] : json();
    if (m.is_number_integer() && m.get<int>() >= 1 && m.get<int>() <= 1000) max_score = m.get<int>();
    else if (!m.is_null()) { err = "virtues.json: \"max\" must be a whole number 1..1000"; return false; }
    for (int& s : score) s = std::min(s, max_score);
    deeds.clear();
    for (const json& d : j.value("deeds", json::array())) {
        Deed x;
        x.id = d.value("id", std::string());
        x.say = d.value("say", std::string());
        x.mark = d.value("mark", std::string());
        if (x.id.empty() || !d.contains("when") || !d.contains("pay") || !d["pay"].is_object()) {
            err = "virtues.json: a deed needs \"id\", \"when\" and \"pay\"";
            return false;
        }
        x.when = crit_cond_from_text(d["when"].dump());
        if (x.when.kind == CritCond::kUnknown) { err = "virtues.json: deed " + x.id + ": bad \"when\""; return false; }
        for (auto it = d["pay"].begin(); it != d["pay"].end(); ++it) {
            const int v = index_of(it.key());
            if (v < 0 || !it.value().is_number_integer()) { err = "virtues.json: deed " + x.id + ": bad pay " + it.key(); return false; }
            x.pay.emplace_back(v, it.value().get<int>());
        }
        deeds.push_back(std::move(x));
    }
    if (j.contains("valor") && j["valor"].is_object()) {
        const json& v = j["valor"];
        const json b = v.value("beyond_level", json::object()), iv = v.value("invasion", json::object());
        valor.levels = b.value("levels", valor.levels);
        valor.points = b.value("points", valor.points);
        valor.times = b.value("times", valor.times);
        valor.invasion_kills = iv.value("kills", valor.invasion_kills);
        valor.invasion_points = iv.value("points", valor.invasion_points);
    }
    endings.clear();
    for (const json& e : j.value("endings", json::array())) {
        Ending x;
        x.id = e.value("id", std::string());
        x.title = e.value("title", x.id);
        x.min_total = e.value("min_total", 0);
        x.min_infamy = e.value("min_infamy", 0);
        x.max_infamy = e.value("max_infamy", -1);
        x.mostly_bad = e.value("mostly_bad", false);
        for (const json& t : e.value("text", json::array())) if (t.is_string()) x.text.push_back(t.get<std::string>());
        endings.push_back(std::move(x));
    }
    if (j.contains("vice") && j["vice"].is_object()) {
        const json& v = j["vice"];
        vice_cfg.murder = v.value("murder", vice_cfg.murder);
        vice_cfg.murder_times = v.value("murder_times", vice_cfg.murder_times);
        vice_cfg.theft = v.value("theft", vice_cfg.theft);
        vice_cfg.theft_times = v.value("theft_times", vice_cfg.theft_times);
        vice_cfg.broken_promise = v.value("broken_promise", vice_cfg.broken_promise);
    }
    epilogue.clear();
    for (const json& e : j.value("epilogue", json::array()))
        epilogue.emplace_back(e.value("key", std::string()), e.value("text", std::string()));
    return true;
}

const Virtues::Ending* Virtues::ending() const {
    const int t = total(), bad = infamy_at_end();
    for (const Ending& e : endings) {
        if (t < e.min_total || bad < e.min_infamy) continue;
        if (e.max_infamy >= 0 && bad > e.max_infamy) continue;
        if (e.mostly_bad && bad <= t) continue;
        return &e;
    }
    return endings.empty() ? nullptr : &endings.back();
}

namespace {
const char kVicePrefix[] = "Vice/";
}

bool Virtues::has_sinned(const std::string& key) const {
    const std::string k = kVicePrefix + key + "/";
    auto it = paid.lower_bound(k);
    return it != paid.end() && it->compare(0, k.size(), k) == 0;
}

int Virtues::sin(int points, const std::string& key, const std::string& say) {
    if (!enabled || points <= 0 || key.empty() || has_sinned(key)) return 0;
    paid.insert(kVicePrefix + key + "/" + std::to_string(points));
    if (w_ && !say.empty()) w_->game_log.add(GameLog::kMessages, GameLog::kInfo, say, 1);
    return points;
}

void Virtues::on_theft(uint16_t item) {
    if (!enabled) return;
    int count = 0;
    for (auto it = paid.lower_bound("Vice/theft/"); it != paid.end() && it->compare(0, 11, "Vice/theft/") == 0; ++it) ++count;
    if (count >= vice_cfg.theft_times) return;
    sin(vice_cfg.theft, "theft/" + std::to_string(item), count == 0 ? "You took what was not yours. Pagan will remember." : "");
}

int Virtues::infamy() const {
    int n = 0;
    for (auto it = paid.lower_bound(kVicePrefix); it != paid.end() && it->compare(0, 5, kVicePrefix) == 0; ++it) {
        const size_t slash = it->rfind('/');
        n += std::atoi(it->c_str() + slash + 1);
    }
    return n;
}

int Virtues::broken_promises() const {
    int n = 0;
    for (const Deed& d : deeds) {
        if (d.mark.empty() || !marked(d.mark)) continue;
        bool kept = false;
        for (const auto& p : d.pay)
            if (has_paid(p.first, "deed/" + d.id)) kept = true;
        if (!kept) ++n;
    }
    return n;
}

std::vector<std::string> Virtues::epilogue_page() const {
    std::vector<std::string> out;
    const Ending* e = ending();
    if (!e) return out;
    out.push_back("+" + e->title);
    for (const std::string& t : e->text) out.push_back("}" + t);
    bool any = false;
    for (const auto& c : epilogue) {
        if (!paid.count(c.first) || c.second.empty()) continue;
        if (!any) { out.push_back("}&----------"); any = true; }  // a red rule before the deeds
        out.push_back("}" + c.second);
    }
    return out;
}

int Virtues::earn(int v, int points, const std::string& key, const std::string& say) {
    const int got = add(v, points, key);
    if (got > 0 && w_)
        w_->game_log.add(GameLog::kMessages, GameLog::kInfo, say.empty() ? std::string("Your ") + name(v) + " grows." : say, 1);
    return got;
}

void Virtues::on_kill(uint16_t victim, uint16_t attacker, int victim_level, int avatar_level) {
    if (!enabled || !w_) return;
    const Actor* v = w_->actor(victim);
    if (!v || victim == 1 || !attacker) return;
    const bool ours = attacker == 1 || w_->same_party(1, attacker);
    if (!ours || w_->same_party(1, victim)) return;
    // Murder: people (named NPCs, or the children of Tenebrae) who were no enemy of the Avatar by
    // their side (combat.h is_enemy), killed by the party. Monsters and the undead are fair game.
    if (const Actor* av = w_->avatar(); av && (victim < 256 || v->shape == 708) && !is_enemy(*v, *av) && !is_enemy(*av, *v)) {
        int count = 0;
        for (auto it = paid.lower_bound("Vice/murder/"); it != paid.end() && it->compare(0, 12, "Vice/murder/") == 0; ++it) ++count;
        if (count < vice_cfg.murder_times)
            sin(vice_cfg.murder, "murder/" + std::to_string(v->shape == 708 ? 10000 + count : victim),
                "Innocent blood is on your hands. Pagan will remember.");
    }
    // A foe far above the Avatar, felled by his own hand.
    if (attacker == 1 && victim_level >= 0 && avatar_level >= 0 && victim_level >= avatar_level + valor.levels) {
        for (int n = 1; n <= valor.times; ++n) {
            const std::string key = "beyond/" + std::to_string(n);
            if (has_paid(kValor, key)) continue;
            earn(kValor, valor.points, key, "You struck down a foe far beyond you. Your Valor grows.");
            break;
        }
    }
    // The undead invasion of Tenebrae (G[58:1]; specs/world/cut_undead_invasion.md): ghouls in its streets.
    if (v->shape == 142 && (v->mapnum == 39 || v->mapnum == 40 || v->mapnum == 41) && w_->vm().get_global(58, 1) == 1 &&
        !has_paid(kValor, "invasion")) {
        int n = 1;
        while (n <= valor.invasion_kills && marked("invasion_kill/" + std::to_string(n))) ++n;
        if (n <= valor.invasion_kills) set_mark("invasion_kill/" + std::to_string(n));
        if (n >= valor.invasion_kills)
            earn(kValor, valor.invasion_points, "invasion", "You stood with Tenebrae against the dead. Your Valor grows.");
    }
}

void Virtues::frame() {
    popup_frame();
    if (!enabled || deeds.empty() || !w_) return;
    if (--deed_wait_ > 0) return;
    deed_wait_ = deed_check_frames;
    WorldCritContext ctx(*w_);
    for (const Deed& d : deeds) {
        bool open = false;
        for (const auto& p : d.pay)
            if (!has_paid(p.first, "deed/" + d.id)) open = true;
        if (!open || (!d.mark.empty() && !marked(d.mark)) || !d.when.eval(ctx)) continue;
        bool said = false;
        for (const auto& p : d.pay)
            if (add(p.first, p.second, "deed/" + d.id) > 0 && !said) {
                w_->game_log.add(GameLog::kMessages, GameLog::kInfo,
                                 d.say.empty() ? std::string("Your ") + name(p.first) + " grows." : d.say, 1);
                said = true;
            }
    }
}

int Virtues::settle_deeds() {
    if (!w_) return 0;
    WorldCritContext ctx(*w_);
    int n = 0;
    for (const Deed& d : deeds) {
        if ((!d.mark.empty() && !marked(d.mark)) || !d.when.eval(ctx)) continue;
        for (const auto& p : d.pay)
            if (paid.insert(std::string(name(p.first)) + "/deed/" + d.id).second) ++n;
    }
    return n;
}

const char* Virtues::name(int v) { return v >= 0 && v < kVirtueCount ? kNames[v] : ""; }

int Virtues::index_of(const std::string& n) {
    for (int v = 0; v < kVirtueCount; ++v) {
        const std::string k = kNames[v];
        if (k.size() == n.size() &&
            std::equal(k.begin(), k.end(), n.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
            return v;
    }
    return -1;
}

int Virtues::total() const {
    int t = 0;
    for (int s : score) t += s;
    return t;
}

int Virtues::add(int v, int points, const std::string& key) {
    if (!enabled || v < 0 || v >= kVirtueCount || points == 0) return 0;
    if (!key.empty()) {
        const std::string k = std::string(kNames[v]) + "/" + key;
        if (paid.count(k)) return 0;
        paid.insert(k);
    }
    const int before = score[size_t(v)];
    score[size_t(v)] = std::clamp(before + points, 0, max_score);
    const int got = score[size_t(v)] - before;
    if (got > 0) {
        auto it = std::find_if(popups_waiting.begin(), popups_waiting.end(), [&](const Popup& p) { return p.virtue == v; });
        if (it != popups_waiting.end()) it->points += got;
        else popups_waiting.push_back({v, got, 0});
    }
    return got;
}

void Virtues::set(int v, int value) {
    if (v >= 0 && v < kVirtueCount) score[size_t(v)] = std::clamp(value, 0, max_score);
}

void Virtues::reset() {
    score.fill(0);
    paid.clear();
    popups_waiting.clear();
    popups.clear();
}

void Virtues::clear_infamy() {
    for (auto it = paid.lower_bound(kVicePrefix); it != paid.end() && it->compare(0, 5, kVicePrefix) == 0;) it = paid.erase(it);
}

void Virtues::set_infamy(int value) {
    value = std::max(0, value);
    const std::string dbg = std::string(kVicePrefix) + "debug/";
    for (auto it = paid.lower_bound(dbg); it != paid.end() && it->compare(0, dbg.size(), dbg) == 0;) it = paid.erase(it);
    const int rest = infamy();
    if (value < rest) clear_infamy();
    const int add = value < rest ? value : value - rest;
    if (add > 0) paid.insert(dbg + std::to_string(add));
}

std::vector<std::string> Virtues::known_marks() const {
    std::set<std::string> m;
    for (const Deed& d : deeds)
        if (!d.mark.empty()) m.insert(d.mark);
    for (const auto& e : epilogue)
        if (e.first.rfind("mark/", 0) == 0) m.insert(e.first.substr(5));
    for (const std::string& k : paid)
        if (k.rfind("mark/", 0) == 0) m.insert(k.substr(5));
    return std::vector<std::string>(m.begin(), m.end());
}

bool Virtues::has_state() const { return total() > 0 || !paid.empty(); }

// Saving: optional section VIRT after CRTR (written only when a score or a key is set).
void Virtues::save(SaveWriter& o) const {
    o.tag("VIRT");
    o.u8(1);
    o.u8(kVirtueCount);
    for (int s : score) o.i32(s);
    o.u32(uint32_t(paid.size()));
    for (const std::string& k : paid) o.str(k);
}

bool Virtues::load(SaveReader& in) {
    reset();
    if (!in.tag("VIRT")) return false;
    if (in.u8() != 1) return false;
    const unsigned n = in.u8();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        int s = in.i32();
        if (i < unsigned(kVirtueCount)) score[i] = std::clamp(s, 0, max_score);
    }
    uint32_t k = std::min<uint32_t>(in.u32(), 100000);
    for (uint32_t i = 0; i < k && !in.fail(); ++i) paid.insert(in.str());
    return !in.fail();
}

}  // namespace pg
