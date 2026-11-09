// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Travel banter between two companions at banter points (game/banter.h).
#include "game/banter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "core/serial.h"
#include "game/critpath.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using nlohmann::json;

namespace {
std::vector<std::string> strs(const json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& s : j)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

// One exchange: {"id": ..., "when": {"step_done": [...]}, "lines": [{"kalvax": "..."}, ...]}.
bool parse_exchange(const json& j, BanterExchange& e, std::string& err) {
    if (!j.is_object()) { err = "an exchange is not an object"; return false; }
    e.id = j.value("id", std::string());
    if (e.id.empty()) { err = "an exchange has no id"; return false; }
    if (j.contains("when") && j["when"].is_object()) {
        const json& w = j["when"];
        if (w.contains("step_done")) e.step_done = strs(w["step_done"]);
        if (w.contains("step_not_done")) e.step_not_done = strs(w["step_not_done"]);
    }
    for (const json& l : j.value("lines", json::array())) {
        if (!l.is_object() || l.size() != 1 || !l.begin().value().is_string()) {
            err = e.id + ": each line is {\"<companion id>\": \"<text>\"}";
            return false;
        }
        e.lines.push_back({l.begin().key(), l.begin().value().get<std::string>()});
    }
    if (e.lines.empty()) { err = e.id + ": no lines"; return false; }
    return true;
}

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    ok = bool(f);
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    return ss.str();
}
}  // namespace

Banter::Banter(World& w, Party& p) : w_(w), p_(p) {}

bool Banter::load_text(const std::string& exchanges, const std::string& pts, std::string& err) {
    json je, jp;
    try {
        je = json::parse(exchanges, nullptr, true, true);
        jp = json::parse(pts, nullptr, true, true);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    std::vector<BanterPair> np;
    for (const json& pj : je.value("pairs", json::array())) {
        BanterPair p;
        std::vector<std::string> who = strs(pj.value("pair", json::array()));
        if (who.size() != 2) { err = "a pair needs two companion ids"; return false; }
        p.a = who[0];
        p.b = who[1];
        p.mood = pj.value("mood", std::string());
        for (const json& ej : pj.value("exchanges", json::array())) {
            BanterExchange e;
            if (!parse_exchange(ej, e, err)) return false;
            p.exchanges.push_back(std::move(e));
        }
        if (pj.contains("repeat")) {
            if (!parse_exchange(pj["repeat"], p.repeat, err)) return false;
            p.repeat.repeat = true;
        }
        np.push_back(std::move(p));
    }
    std::vector<BanterPoint> npt;
    for (const json& q : jp.value("points", json::array())) {
        BanterPoint b;
        b.id = q.value("id", std::string());
        b.place = q.value("place", std::string());
        b.map = q.value("map", 0u);
        b.x = q.value("x", 0);
        b.y = q.value("y", 0);
        b.z = q.value("z", 0);
        b.r = q.value("r", 192);
        if (b.id.empty() || !b.map) { err = "a point needs an id and a map"; return false; }
        npt.push_back(std::move(b));
    }
    pairs = std::move(np);
    points = std::move(npt);
    return true;
}

bool Banter::load_dir(const std::string& dir, std::string& err) {
    bool ok1 = false, ok2 = false;
    std::string a = read_file(dir + "/banter.json", ok1), b = read_file(dir + "/banter_points.json", ok2);
    if (!ok1 || !ok2) { err = "cannot open " + dir + (ok1 ? "/banter_points.json" : "/banter.json"); return false; }
    if (!load_text(a, b, err)) return false;
    PG_LOG("banter: %zu pairs, %zu points", pairs.size(), points.size());
    return true;
}

std::vector<std::string> Banter::problems() const {
    std::vector<std::string> out;
    std::set<std::string> seen_pairs, ids;
    auto check = [&](const BanterPair& p, const BanterExchange& e) {
        if (!ids.insert(e.id).second) out.push_back("exchange id twice: " + e.id);
        if (e.lines.size() < 2) out.push_back(e.id + ": fewer than two lines");
        std::set<std::string> speakers;
        for (const BanterLine& l : e.lines) {
            if (l.who != p.a && l.who != p.b) out.push_back(e.id + ": a line by " + l.who + ", not of the pair");
            if (l.text.empty()) out.push_back(e.id + ": an empty line");
            speakers.insert(l.who);
        }
        if (speakers.size() < 2) out.push_back(e.id + ": only one speaker");
        for (const auto* v : {&e.step_done, &e.step_not_done})
            for (const std::string& s : *v)
                if (!p_.crit_ || p_.crit_->step_index(s) < 0) out.push_back(e.id + ": unknown step " + s);
    };
    p_.step_done("");  // loads the critical path
    for (const BanterPair& p : pairs) {
        if (p_.find_def(p.a) < 0) out.push_back("unknown companion " + p.a);
        if (p_.find_def(p.b) < 0) out.push_back("unknown companion " + p.b);
        if (p.a == p.b) out.push_back("a pair of one: " + p.a);
        std::string key = std::min(p.a, p.b) + "+" + std::max(p.a, p.b);
        if (!seen_pairs.insert(key).second) out.push_back("pair listed twice: " + key);
        if (p.exchanges.empty()) out.push_back(key + ": no exchanges");
        if (p.repeat.lines.empty()) out.push_back(key + ": no repeat");
        for (const BanterExchange& e : p.exchanges) check(p, e);
        if (!p.repeat.lines.empty()) check(p, p.repeat);
    }
    std::set<std::string> pids;
    for (const BanterPoint& b : points) {
        if (!pids.insert(b.id).second) out.push_back("point id twice: " + b.id);
        if (b.r < 32 || b.r > 1024) out.push_back(b.id + ": radius out of range");
    }
    return out;
}

void Banter::reset() {
    played.clear();
    ready_at.clear();
    cur_ = nullptr;
    cur_pair_ = -1;
    line_ = 0;
    last_speaker_ = 0;
    next_line_ = next_check_ = 0;
    log.clear();
}

int Banter::point_index(const std::string& id) const {
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].id == id) return int(i);
    return -1;
}

// No fight, conversation or scene (the same test as the level-up prompt's), nothing else said by
// the party, the game running.
bool Banter::calm() const {
    if (w_.kernel().paused() || !w_.leveling || !w_.leveling->calm()) return false;
    if (p_.talking() || w_.ask_menu) return false;
    return true;
}

std::vector<int> Banter::present() const {
    std::vector<int> out;
    const Actor* av = w_.avatar();
    if (!av) return out;
    for (int m : p_.members) {
        const CompanionState& st = p_.state[size_t(m)];
        const Actor* a = w_.actor(st.id);
        if (!a || st.hidden || st.mode != CompanionState::kFollow || a->is(Actor::kDead) || a->chunk < 0) continue;
        if (a->mapnum != av->mapnum || a->is(Actor::kInCombat)) continue;
        if (std::abs(a->x - av->x) + std::abs(a->y - av->y) > kNear) continue;
        out.push_back(m);
    }
    return out;
}

bool Banter::exchange_ok(const BanterExchange& e) const {
    for (const std::string& s : e.step_done)
        if (!p_.step_done(s)) return false;
    for (const std::string& s : e.step_not_done)
        if (p_.step_done(s)) return false;
    return true;
}

int Banter::unplayed(const BanterPair& p) const {
    int n = 0;
    for (const BanterExchange& e : p.exchanges) n += !played.count(e.id) && exchange_ok(e);
    return n;
}

int Banter::heard(const BanterPair& p) const {
    int n = 0;
    for (const BanterExchange& e : p.exchanges) n += played.count(e.id) ? 1 : 0;
    return n;
}

bool Banter::speaker_here(const std::string& id) const {
    int d = p_.find_def(id);
    if (d < 0) return false;
    std::vector<int> pr = present();
    return std::find(pr.begin(), pr.end(), d) != pr.end();
}

void Banter::frame() {
    if (!p_.enabled || !p_.spawned_once || !loaded()) return;
    if (cur_) { advance(); return; }
    const uint32_t now = w_.kernel().frame_number();
    if (now < next_check_) return;
    next_check_ = now + kCheckEvery;
    const Actor* av = w_.avatar();
    if (!av) return;
    const unsigned map = w_.map().map_number();
    for (size_t i = 0; i < points.size(); ++i) {
        const BanterPoint& b = points[i];
        // Carthax Lake (16 / 26) and the lava tunnel (24 / 25) are one place in two states.
        const unsigned twin = b.map == 16 ? 26 : b.map == 26 ? 16 : b.map == 24 ? 25 : b.map == 25 ? 24 : b.map;
        if (b.map != map && twin != map) continue;
        if (std::hypot(double(av->x - b.x), double(av->y - b.y)) > b.r || std::abs(av->z - b.z) > 96) continue;
        auto it = ready_at.find(b.id);
        if (it != ready_at.end() && now < it->second) continue;
        try_point(int(i));
        return;
    }
}

void Banter::try_point(int pt) {
    if (!calm()) { why_not = "not calm"; return; }
    for (const Bark& b : w_.barks)
        if (b.speaker == 1 || p_.is_companion(b.speaker)) { why_not = "the party is talking"; return; }
    std::vector<int> pr = present();
    if (pr.size() < 2) { why_not = "fewer than two companions"; return; }
    why_not.clear();
    auto here = [&](const std::string& id) {
        int d = p_.find_def(id);
        return d >= 0 && std::find(pr.begin(), pr.end(), d) != pr.end();
    };
    std::vector<int> fresh, spent;  // pairs present with real exchanges left / with only the repeat
    for (size_t i = 0; i < pairs.size(); ++i) {
        const BanterPair& p = pairs[i];
        if (!here(p.a) || !here(p.b)) continue;
        if (unplayed(p) > 0) fresh.push_back(int(i));
        else if (!p.repeat.lines.empty()) spent.push_back(int(i));
    }
    if (fresh.empty() && spent.empty()) { why_not = "no pair"; return; }  // the point stays ready
    const BanterPoint& b = points[size_t(pt)];
    const uint32_t now = w_.kernel().frame_number();
    ready_at[b.id] = now + kRecharge;
    if (!fresh.empty()) {
        // Fairly: the pair that has heard the fewest of its exchanges, ties at random.
        int least = 1 << 30;
        for (int i : fresh) least = std::min(least, heard(pairs[size_t(i)]));
        std::vector<int> tied;
        for (int i : fresh)
            if (heard(pairs[size_t(i)]) == least) tied.push_back(i);
        int pick = tied[size_t(w_.rand_below(int(tied.size())))];
        for (const BanterExchange& e : pairs[size_t(pick)].exchanges)
            if (!played.count(e.id) && exchange_ok(e)) {
                log.push_back("point " + b.id + " fires");
                start(pick, e);
                return;
            }
        return;
    }
    // Everyone present has said everything: the repeat, one time in three.
    if (w_.rand_below(3) != 0) {
        log.push_back("point " + b.id + " fires: quiet");
        return;
    }
    int pick = spent[size_t(w_.rand_below(int(spent.size())))];
    log.push_back("point " + b.id + " fires: repeat");
    start(pick, pairs[size_t(pick)].repeat);
}

void Banter::start(int pair, const BanterExchange& e) {
    cur_ = &e;
    cur_pair_ = pair;
    line_ = 0;
    last_speaker_ = 0;
    next_line_ = w_.kernel().frame_number();
    log.push_back("start " + e.id);
    PG_LOG("banter: %s", e.id.c_str());
}

void Banter::stop(const char* why) {
    if (cur_) log.push_back("stop " + cur_->id + ": " + why);
    cur_ = nullptr;
    cur_pair_ = -1;
    line_ = 0;
    last_speaker_ = 0;
}

void Banter::advance() {
    const uint32_t now = w_.kernel().frame_number();
    // A fight, a conversation, a scene, or a speaker gone: the rest goes unsaid and the exchange
    // is not counted as heard.
    if (!calm()) { stop("not calm"); return; }
    const BanterPair& pr = pairs[size_t(cur_pair_)];
    if (!speaker_here(pr.a) || !speaker_here(pr.b)) { stop("a speaker left"); return; }
    // The previous line is still on screen.
    if (last_speaker_)
        for (const Bark& b : w_.barks)
            if (b.speaker == last_speaker_) {
                next_line_ = now + 6;  // a short breath after it ends
                return;
            }
    if (now < next_line_) return;
    if (line_ >= cur_->lines.size()) {
        log.push_back("done " + cur_->id);
        stop("done");
        return;
    }
    const BanterLine& l = cur_->lines[line_];
    int d = p_.find_def(l.who);
    if (d < 0) { stop("unknown speaker"); return; }
    const uint16_t id = p_.state[size_t(d)].id;
    std::string text = l.text;
    for (size_t a; (a = text.find("{avatar}")) != std::string::npos;) text.replace(a, 8, w_.avatar_name);
    w_.bark(id, text);
    last_speaker_ = id;
    // Keep the party's other talk (idle remarks) out of the gaps.
    p_.next_ambient_ = std::max(p_.next_ambient_, now + 600);
    ++line_;
    // Heard once its last line has been said (a save from here on keeps it heard).
    if (line_ == cur_->lines.size() && !cur_->repeat) played.insert(cur_->id);
}

void Banter::save(SaveWriter& o) const {
    o.tag("BNTR");
    o.u8(1);
    o.u32(uint32_t(played.size()));
    for (const std::string& s : played) o.str(s);
    o.u32(uint32_t(ready_at.size()));
    for (const auto& r : ready_at) { o.str(r.first); o.u32(r.second); }
}

bool Banter::load(SaveReader& in) {
    reset();
    if (!in.tag("BNTR")) return false;
    unsigned version = in.u8();
    if (version < 1) return false;
    uint32_t n = std::min<uint32_t>(in.u32(), 100000);
    for (uint32_t i = 0; i < n && !in.fail(); ++i) played.insert(in.str());
    n = std::min<uint32_t>(in.u32(), 100000);
    for (uint32_t i = 0; i < n && !in.fail(); ++i) {
        std::string id = in.str();
        ready_at[id] = in.u32();
    }
    return !in.fail();
}

}  // namespace pg
