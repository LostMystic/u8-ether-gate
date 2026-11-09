// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "game/critpath.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/world.h"
#include "json/json.hpp"

namespace pg {

using nlohmann::json;

std::string map_name(unsigned m) {
    // specs/world/map_index.md section 1 (short forms).
    static const std::map<unsigned, const char*> names = {
        {3, "the Tenebrae docks and shore"}, {4, "Fisherman's Reef"}, {5, "the East Road"}, {6, "the Cemetery"},
        {7, "Herdsman's Valley"}, {8, "the Plateau"}, {12, "Daemon's Crag"}, {14, "the Obsidian Fortress"},
        {15, "Treasure Cove"}, {16, "Carthax Lake"}, {21, "Stone Cove"}, {24, "the Lava River tunnel"},
        {25, "the Lava River tunnel"}, {26, "Carthax Lake"}, {28, "Carthax Lake (breaking ground)"},
        {31, "the Hall of the Mountain King"}, {37, "Argentrock Isle"}, {39, "West Tenebrae"},
        {40, "Central Tenebrae"}, {41, "East Tenebrae"}, {43, "the Ethereal Plane"}, {44, "the Plane of Fire"},
        {45, "the Plane of Water"}, {46, "the Plane of Air"}, {47, "the Plane of Earth"}, {49, "the Catacombs"},
        {50, "the Upper Catacombs"}, {51, "the Lower Catacombs"}, {54, "the Shrine of the Ancient Ones"},
        {57, "the Pit of the Dead"}, {62, "the Ancient Necromancers' catacombs"}, {63, "the ghost area"},
    };
    auto it = names.find(m);
    return it != names.end() ? it->second : "map " + std::to_string(m);
}

// ---------------------------------------------------------------------------------------------
// Conditions

bool CritCond::eval(const CritContext& c) const {
    switch (kind) {
    case kNone: case kFalse: case kUnknown: return false;
    case kTrue: return true;
    case kAll:
        for (const CritCond& s : sub) if (!s.eval(c)) return false;
        return true;
    case kAny:
        for (const CritCond& s : sub) if (s.eval(c)) return true;
        return false;
    case kNot: return !sub.empty() && !sub[0].eval(c);
    case kGlobal: {
        int v = c.global(unsigned(a), unsigned(std::max(1, b)));
        switch (cmp) {
        case kEq: return v == value;
        case kNe: return v != value;
        case kGe: return v >= value;
        default: return v != 0;
        }
    }
    case kHasItem: return c.item_count(unsigned(a), frame, quality) >= std::max(1, qty);
    case kEquipped: return c.equipped(unsigned(a));
    case kOnMap: return c.current_map() == unsigned(a);
    case kVisitedMap: return c.visited_map(unsigned(a)) || c.current_map() == unsigned(a);
    case kNpcDead: return c.npc_dead(unsigned(a));
    case kItemGone: return c.item_gone(unsigned(a), unsigned(map), x, y, z) == 1;
    case kSpellKnown: return c.spell_known(unsigned(a));
    }
    return false;
}

std::string CritCond::describe() const {
    char b2[128];
    auto join = [&](const char* op) {
        std::string s = op;
        s += "(";
        for (size_t i = 0; i < sub.size(); ++i) s += (i ? ", " : "") + sub[i].describe();
        return s + ")";
    };
    switch (kind) {
    case kNone: return "none";
    case kTrue: return "true";
    case kFalse: return "false";
    case kAll: return join("all");
    case kAny: return join("any");
    case kNot: return join("not");
    case kGlobal:
        std::snprintf(b2, sizeof b2, "G[%d:%d]%s%d", a, b, cmp == kEq ? "==" : cmp == kNe ? "!=" : cmp == kGe ? ">=" : "!=",
                      cmp == kNonZero ? 0 : value);
        return b2;
    case kHasItem: std::snprintf(b2, sizeof b2, "has %d%s x%d", a, frame >= 0 ? ("/" + std::to_string(frame)).c_str() : "", qty); return b2;
    case kEquipped: return "equipped " + std::to_string(a);
    case kOnMap: return "on map " + std::to_string(a);
    case kVisitedMap: return "visited " + std::to_string(a);
    case kNpcDead: return "npc " + std::to_string(a) + " dead";
    case kItemGone: std::snprintf(b2, sizeof b2, "item %d gone (map %d %d,%d,%d)", a, map, x, y, z); return b2;
    case kSpellKnown: return "spell " + std::to_string(a);
    case kUnknown: return "?" + text;
    }
    return "?";
}

namespace {

int num(const json& j, const char* key, int def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<int>();
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

CritCond parse_cond(const json& j) {
    CritCond c;
    if (j.is_null()) return c;
    if (j.is_boolean()) { c.kind = j.get<bool>() ? CritCond::kTrue : CritCond::kFalse; return c; }
    if (j.is_array()) {  // a bare list = all of them
        c.kind = CritCond::kAll;
        for (const json& s : j) c.sub.push_back(parse_cond(s));
        return c;
    }
    if (!j.is_object()) { c.kind = CritCond::kUnknown; c.text = j.dump(); return c; }
    if (j.contains("all") || j.contains("any")) {
        c.kind = j.contains("all") ? CritCond::kAll : CritCond::kAny;
        const json& l = j.contains("all") ? j["all"] : j["any"];
        if (l.is_array()) for (const json& s : l) c.sub.push_back(parse_cond(s));
        else c.sub.push_back(parse_cond(l));
        return c;
    }
    if (j.contains("not")) {
        c.kind = CritCond::kNot;
        c.sub.push_back(parse_cond(j["not"]));
        return c;
    }
    if (j.contains("global")) {
        c.kind = CritCond::kGlobal;
        const json& g = j["global"];
        if (g.is_array() && g.size() >= 1) {
            c.a = g[0].get<int>();
            c.b = g.size() >= 2 ? g[1].get<int>() : 1;
        } else if (g.is_number()) {
            c.a = g.get<int>();
            c.b = num(j, "bits", 1);
        }
        if (j.contains("eq")) { c.cmp = CritCond::kEq; c.value = num(j, "eq", 0); }
        else if (j.contains("ne")) { c.cmp = CritCond::kNe; c.value = num(j, "ne", 0); }
        else if (j.contains("ge")) { c.cmp = CritCond::kGe; c.value = num(j, "ge", 0); }
        return c;
    }
    auto simple = [&](const char* key, CritCond::Kind k) {
        if (!j.contains(key)) return false;
        c.kind = k;
        const json& v = j[key];
        if (v.is_number()) c.a = v.get<int>();
        else if (v.is_object()) { c.a = num(v, "shape", num(v, "npc", num(v, "map", 0))); }
        return true;
    };
    if (simple("has_item", CritCond::kHasItem)) {
        c.frame = num(j, "frame", -1);
        c.qty = num(j, "qty", 1);
        c.quality = num(j, "quality", -1);
        return c;
    }
    if (simple("equipped", CritCond::kEquipped) || simple("on_map", CritCond::kOnMap) ||
        simple("visited_map", CritCond::kVisitedMap) || simple("npc_dead", CritCond::kNpcDead) ||
        simple("spell_known", CritCond::kSpellKnown))
        return c;
    if (j.contains("item_gone")) {
        c.kind = CritCond::kItemGone;
        const json& v = j["item_gone"];
        c.a = num(v, "shape", 0);
        c.map = num(v, "map", 0);
        c.x = num(v, "x", 0);
        c.y = num(v, "y", 0);
        c.z = num(v, "z", 0);
        return c;
    }
    c.kind = CritCond::kUnknown;
    c.text = j.dump();
    return c;
}

}  // namespace

CritCond crit_cond_from_text(const std::string& text) {
    try {
        return parse_cond(json::parse(text));
    } catch (const std::exception&) {
        CritCond c;
        c.kind = CritCond::kUnknown;
        c.text = text;
        return c;
    }
}

namespace {

CritTarget parse_target(const json& j) {
    CritTarget t;
    if (!j.is_object()) return t;
    std::string type = str(j, "type");
    t.npc = num(j, "npc", 0);
    t.shape = num(j, "shape", 0);
    t.map = num(j, "map", -1);
    t.x = num(j, "x", 0);
    t.y = num(j, "y", 0);
    t.z = num(j, "z", 0);
    if (type == "npc") t.type = CritTarget::kNpc;
    else if (type == "point") t.type = CritTarget::kPoint;
    else if (type == "item") t.type = CritTarget::kItem;
    else if (t.map >= 0) t.type = CritTarget::kPoint;
    if (j.contains("via") && j["via"].is_array())
        for (const json& v : j["via"]) {
            if (!v.is_object()) continue;
            CritTarget::Via w;
            w.map = num(v, "map", -1);
            w.x = num(v, "x", 0);
            w.y = num(v, "y", 0);
            w.z = num(v, "z", 0);
            w.note = str(v, "note");
            t.via.push_back(w);
        }
    return t;
}

}  // namespace

bool CritPath::load_text(const std::string& text, std::string& err) {
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    const json* steps_j = nullptr;
    const json* links_j = nullptr;
    if (j.is_array()) links_j = &j;
    else if (j.is_object()) {
        if (j.contains("chains") && j["chains"].is_object())
            for (auto it = j["chains"].begin(); it != j["chains"].end(); ++it) {
                std::vector<std::string>& v = chain_after[it.key()];
                if (it.value().is_object() && it.value().contains("after") && it.value()["after"].is_array())
                    for (const json& a : it.value()["after"]) if (a.is_string()) v.push_back(a.get<std::string>());
            }
        if (j.contains("steps") && j["steps"].is_array()) steps_j = &j["steps"];
        if (j.contains("links") && j["links"].is_array()) links_j = &j["links"];
    }
    if (!steps_j && !links_j) { err = "no \"steps\" or \"links\" array"; return false; }
    try {
        if (steps_j)
            for (const json& s : *steps_j) {
                if (!s.is_object()) continue;
                CritStep st;
                st.id = str(s, "id");
                st.chain = str(s, "chain");
                if (s.contains("milestone") && s["milestone"].is_boolean()) {
                    st.has_milestone = true;
                    st.milestone = s["milestone"].get<bool>();
                }
                if (s.contains("optional") && s["optional"].is_boolean()) st.optional = s["optional"].get<bool>();
                if (s.contains("alt_targets") && s["alt_targets"].is_array())
                    for (const json& a : s["alt_targets"])
                        if (a.is_object() && a.contains("target"))
                            st.alt_targets.push_back({a.contains("when") ? parse_cond(a["when"]) : CritCond{CritCond::kTrue},
                                                      parse_target(a["target"])});
                st.act = str(s, "act");
                st.title = str(s, "title");
                st.hint = str(s, "hint");
                st.evidence = str(s, "evidence");
                st.confidence = str(s, "confidence");
                if (s.contains("done_when")) st.done_when = parse_cond(s["done_when"]);
                if (s.contains("target")) st.target = parse_target(s["target"]);
                if (st.title.empty()) st.title = st.id;
                steps.push_back(std::move(st));
            }
        if (links_j)
            for (const json& l : *links_j) {
                if (!l.is_object()) continue;
                MapLink k;
                int fm = num(l, "from_map", 0);
                k.from_any = fm < 0;
                k.from_map = unsigned(std::max(0, fm));
                k.has_point = l.contains("x") && l["x"].is_number();
                k.dead = (l.contains("dead") && l["dead"].is_boolean() && l["dead"].get<bool>()) ||
                         !(l.contains("to_x") && l["to_x"].is_number());  // no resolvable arrival point
                k.to_map = unsigned(num(l, "to_map", 0));
                k.x = num(l, "x", 0);
                k.y = num(l, "y", 0);
                k.z = num(l, "z", 0);
                k.to_x = num(l, "to_x", 0);
                k.to_y = num(l, "to_y", 0);
                k.to_z = num(l, "to_z", 0);
                k.kind = str(l, "kind");
                k.note = str(l, "note");
                if (l.contains("trigger_range") && l["trigger_range"].is_array() && l["trigger_range"].size() == 2 &&
                    l["trigger_range"][0].is_number() && l["trigger_range"][1].is_number()) {
                    k.rx = l["trigger_range"][0].get<int>();
                    k.ry = l["trigger_range"][1].get<int>();
                }
                if (l.contains("condition") && !l["condition"].is_null()) {
                    k.condition = parse_cond(l["condition"]);
                    k.has_condition = true;
                }
                links.push_back(std::move(k));
            }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool CritPath::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!load_text(ss.str(), err)) { err = path + ": " + err; return false; }
    sources.push_back(path);
    return true;
}

int CritPath::step_index(const std::string& id) const {
    for (size_t i = 0; i < steps.size(); ++i) if (steps[i].id == id) return int(i);
    return -1;
}

std::vector<bool> CritPath::done(const CritContext& c) const {
    std::vector<bool> d(steps.size(), false);
    for (size_t i = 0; i < steps.size(); ++i)
        d[i] = forced_done.count(int(i)) || (jump_to >= 0 && int(i) < jump_to) || steps[i].done_when.eval(c);
    return d;
}

bool CritPath::chain_follows(const std::string& later, const std::string& earlier) const {
    std::set<std::string> seen;
    std::vector<std::string> todo{later};
    while (!todo.empty()) {
        std::string c = todo.back();
        todo.pop_back();
        auto it = chain_after.find(c);
        if (it == chain_after.end()) continue;
        for (const std::string& a : it->second) {
            if (a == earlier) return true;
            if (seen.insert(a).second) todo.push_back(a);
        }
    }
    return false;
}

bool CritPath::has_milestones() const {
    for (const CritStep& s : steps) if (s.has_milestone) return true;
    return false;
}

std::vector<bool> CritPath::complete(const std::vector<bool>& d) const {
    std::vector<bool> c = d;
    // Milestones that are done, by chain: the last index per chain, and the set of chains.
    std::map<std::string, int> last_ms;
    for (size_t i = 0; i < d.size() && i < steps.size(); ++i)
        if (d[i] && steps[i].milestone) last_ms[steps[i].chain] = int(i);
    std::map<std::string, bool> later_chain_done;  // memo: some chain after this one has a done milestone
    for (size_t i = 0; i < c.size() && i < steps.size(); ++i) {
        if (c[i]) continue;
        const std::string& ch = steps[i].chain;
        auto m = last_ms.find(ch);
        if (m != last_ms.end() && m->second > int(i)) { c[i] = true; continue; }
        auto memo = later_chain_done.find(ch);
        bool after;
        if (memo != later_chain_done.end()) after = memo->second;
        else {
            after = false;
            for (const auto& kv : last_ms)
                if (kv.first != ch && chain_follows(kv.first, ch)) { after = true; break; }
            later_chain_done[ch] = after;
        }
        if (after) c[i] = true;
    }
    return c;
}

int CritPath::current(const std::vector<bool>& d) const {
    std::vector<bool> c = complete(d);
    for (size_t i = 0; i < c.size(); ++i) if (!c[i]) return int(i);
    return -1;
}

bool CritPath::link_usable(const CritContext& c, const MapLink& l) const {
    return !l.dead && (!l.has_condition || l.condition.eval(c));
}

int CritPath::current(const CritContext& c) const { return current(done(c)); }

std::vector<RouteLink> CritPath::walk_links(const CritContext& c, unsigned map) const {
    std::vector<RouteLink> out;
    for (const MapLink& l : links) {
        if (l.from_any || !l.has_point || l.from_map != map || l.to_map != map || !link_usable(c, l)) continue;
        RouteLink r;
        r.x = l.x; r.y = l.y; r.z = l.z;
        r.rx = l.rx; r.ry = l.ry;
        r.to_x = l.to_x; r.to_y = l.to_y; r.to_z = l.to_z;
        out.push_back(r);
    }
    return out;
}

std::vector<unsigned> CritPath::route(const CritContext& c, unsigned from, unsigned to) const {
    if (from == to) return {from};
    std::map<unsigned, unsigned> prev;
    std::deque<unsigned> q{from};
    prev[from] = from;
    while (!q.empty()) {
        unsigned m = q.front();
        q.pop_front();
        for (const MapLink& l : links) {
            if ((!l.from_any && l.from_map != m) || prev.count(l.to_map)) continue;
            if (!link_usable(c, l)) continue;
            prev[l.to_map] = m;
            if (l.to_map == to) {
                std::vector<unsigned> r{to};
                for (unsigned k = to; k != from; k = prev[k]) r.push_back(prev[k]);
                std::reverse(r.begin(), r.end());
                return r;
            }
            q.push_back(l.to_map);
        }
    }
    return {};
}

bool CritPath::target_location(const CritContext& c, const CritTarget& t, unsigned& map, int& x, int& y, int& z) const {
    switch (t.type) {
    case CritTarget::kNpc: return c.npc_location(unsigned(t.npc), map, x, y, z);
    case CritTarget::kPoint:
    case CritTarget::kItem:
        if (t.map < 0) return false;
        map = unsigned(t.map);
        x = t.x;
        y = t.y;
        z = t.z;
        return true;
    default: return false;
    }
}

CritGuide CritPath::guide(const CritContext& c) const {
    CritGuide g;
    std::vector<bool> d = done(c);
    g.step = current(d);
    if (g.step < 0) {
        g.all_done = !steps.empty();
        g.line1 = steps.empty() ? "No critical path data" : "The critical path is complete";
        return g;
    }
    g.valid = true;
    const CritStep& st = steps[size_t(g.step)];
    g.line1 = "Next: " + st.title;
    unsigned am;
    int ax, ay, az;
    if (!c.avatar_location(am, ax, ay, az)) return g;
    const CritTarget* tgt = &st.target;
    for (const auto& alt : st.alt_targets)
        if (alt.first.eval(c)) { tgt = &alt.second; break; }
    unsigned tm;
    int tx, ty, tz;
    if (!target_location(c, *tgt, tm, tx, ty, tz)) {
        g.line2 = st.hint;
        return g;
    }
    g.target_map = tm;
    if (tm == am) {
        g.have_point = true;
        g.x = tx;
        g.y = ty;
        g.z = tz;
        g.route = {am};
        g.line2 = st.hint;
        return g;
    }
    // A waypoint the data gives for this map (the exit toward the target).
    for (const CritTarget::Via& v : tgt->via)
        if (v.map >= 0 && unsigned(v.map) == am) {
            g.have_point = true;
            g.x = v.x;
            g.y = v.y;
            g.z = v.z;
            g.route = route(c, am, tm);
            g.line2 = "Go to " + map_name(tm) + (v.note.empty() ? "" : " via " + v.note);
            return g;
        }
    // Another map: the shortest chain of usable links; the first hop is the nearest link egg
    // on this map that starts a shortest chain.
    std::map<unsigned, int> dist;  // hops from a map to the target's map
    for (const MapLink& l : links) {
        if (!link_usable(c, l)) continue;
        if (!dist.count(l.to_map)) {
            std::vector<unsigned> r = route(c, l.to_map, tm);
            dist[l.to_map] = r.empty() ? INT_MAX : int(r.size()) - 1;
        }
    }
    int best = -1, best_hops = INT_MAX;
    long long best_d = LLONG_MAX;
    for (size_t i = 0; i < links.size(); ++i) {
        const MapLink& l = links[i];
        if ((!l.from_any && l.from_map != am) || l.to_map == am) continue;
        if (!link_usable(c, l)) continue;
        int hops = dist.count(l.to_map) ? dist[l.to_map] : INT_MAX;
        if (hops == INT_MAX) continue;
        // Links with a place to walk to beat ones without (items, spells) at the same length.
        long long dd = l.has_point ? (long long)(l.x - ax) * (l.x - ax) + (long long)(l.y - ay) * (l.y - ay) : LLONG_MAX / 2;
        if (hops < best_hops || (hops == best_hops && dd < best_d)) {
            best = int(i);
            best_hops = hops;
            best_d = dd;
        }
    }
    if (best < 0) {
        g.unreachable = true;
        g.line2 = "Target in " + map_name(tm) + " (no known way from here)";
        return g;
    }
    const MapLink& l = links[size_t(best)];
    g.link = best;
    g.have_point = l.has_point;
    g.x = l.x;
    g.y = l.y;
    g.z = l.z;
    g.route = route(c, am, tm);
    g.line2 = "Go to " + map_name(l.to_map);
    if (l.to_map != tm) g.line2 += " (on the way to " + map_name(tm) + ")";
    // A short "how": the data's notes are technical (they name eggs and classes).
    std::string how = l.note;
    if (size_t cut = how.find_first_of("(;:"); cut != std::string::npos) how = how.substr(0, cut);
    while (!how.empty() && how.back() == ' ') how.pop_back();
    if (l.kind == "egg") how = "the teleporter";
    else if (l.kind == "maptele") how = "the magic teleporter";
    if (!l.has_point) g.line2 += how.empty() ? "" : " - use the " + how;
    else if (!how.empty()) g.line2 += " via " + how;
    return g;
}

// ---------------------------------------------------------------------------------------------
// The live world

void WorldCritContext::observe() { w_.visited_maps.insert(w_.map().map_number()); }

bool WorldCritContext::visited_map(unsigned map) const { return w_.visited_maps.count(map) != 0; }

int WorldCritContext::global(unsigned pos, unsigned n) const { return w_.vm().get_global(pos, n); }

int WorldCritContext::item_count(unsigned shape, int frame, int quality) const {
    const Actor* av = w_.avatar();
    if (!av) return 0;
    int n = 0;
    std::function<void(const Item&)> walk = [&](const Item& c) {
        for (uint16_t k : c.contents) {
            const Item* it = w_.item(k);
            if (!it) continue;
            if (it->shape == shape && (frame < 0 || int(it->frame) == frame) && (quality < 0 || int(it->quality) == quality)) {
                unsigned fam = w_.data().typeflags.get(it->shape).family();
                n += (fam == kFamQuantity || fam == kFamReagent) ? std::max<int>(1, it->quality) : 1;
            }
            walk(*it);
        }
    };
    walk(*av);
    return n;
}

bool WorldCritContext::equipped(unsigned shape) const {
    for (unsigned s = 1; s <= 7; ++s)
        if (const Item* it = w_.item(w_.equipped(1, s)); it && it->shape == shape) return true;
    return false;
}

unsigned WorldCritContext::current_map() const { return w_.map().map_number(); }

bool WorldCritContext::npc_dead(unsigned npc) const {
    const Actor* a = w_.actor(uint16_t(npc));
    return a && (a->is(Actor::kDead));
}

int WorldCritContext::item_gone(unsigned shape, unsigned map, int x, int y, int z) const {
    char key[64];
    std::snprintf(key, sizeof key, "%u/%u/%d/%d/%d", shape, map, x, y, z);
    if (w_.map().map_number() == map) {
        const Item* it = w_.map().find_item(shape, -1, x, y, 32);
        int r = it ? 0 : 1;
        gone_seen_[key] = r;
        return r;
    }
    auto f = gone_seen_.find(key);
    if (f != gone_seen_.end()) return f->second;
    // Not loaded and not seen: the map's stored nonfixed items, then its fixed data.
    if (const std::vector<MapEntry>* st = w_.stored_items(map))
        for (const MapEntry& e : *st)
            if (e.rec.shape == shape && e.rec.x == x && e.rec.y == y) return 0;
    bool fixed = false;
    for (const ItemRecord& r : parse_item_records(w_.data().fixed.entry(map)))
        if (r.shape == shape && r.x == x && r.y == y) { fixed = true; break; }
    int r = fixed ? 0 : 1;  // a nonfixed item no longer in the stored list is gone
    gone_seen_[key] = r;
    return r;
}

bool WorldCritContext::spell_known(unsigned n) const {
    // Not specified yet (specs/world/critical_path.md): a spell counts as known when the
    // Avatar carries its focus / spellbook is not modelled; always false for now.
    (void)n;
    return false;
}

bool WorldCritContext::npc_location(unsigned npc, unsigned& map, int& x, int& y, int& z) const {
    const Actor* a = w_.actor(uint16_t(npc));
    if (!a) return false;
    map = a->mapnum;
    w_.location(*a, x, y, z);
    // An NPC standing on the current map is there whatever its stored map number says.
    if (a->chunk >= 0) map = w_.map().map_number();
    return true;
}

bool WorldCritContext::avatar_location(unsigned& map, int& x, int& y, int& z) const {
    const Actor* a = w_.avatar();
    if (!a) return false;
    map = w_.map().map_number();
    w_.location(*a, x, y, z);
    return true;
}

}  // namespace pg
