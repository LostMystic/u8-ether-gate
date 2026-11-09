// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Pagan transformed as the virtues grow (PLAN-VIRTUES.md 5; NOTES.md "World stages"). See world_stages.h.
#include "game/world_stages.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/leveling.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/turn_combat.h"
#include "game/virtues.h"
#include "game/world.h"
#include "json/json.hpp"
#include "usecode/uc_machine.h"

namespace pg {

using json = nlohmann::json;

namespace {

uint32_t hash32(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
float unit(uint32_t h) { return float(h & 0xFFFFFF) / float(0x1000000); }  // 0..1

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <size_t N>
bool read_array(const json& j, const char* key, std::array<float, N>& out) {
    if (!j.contains(key)) return true;
    const json& a = j[key];
    if (!a.is_array()) return false;
    for (size_t i = 0; i < N && i < a.size(); ++i) {
        if (!a[i].is_number()) return false;
        out[i] = a[i].get<float>();
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Data

bool WorldStages::load_dir(const std::string& dir, std::string& err) {
    std::string s = read_file(dir + "/stages.json"), g = read_file(dir + "/greetings.json");
    if (s.empty() && g.empty()) { err = dir + "/stages.json not found"; return false; }
    return load_text(s, g, err);
}

bool WorldStages::load_text(const std::string& stages, const std::string& greet, std::string& err) {
    if (!stages.empty()) {
        json j = json::parse(stages, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { err = "stages.json: not valid JSON"; return false; }
        if (j.contains("thresholds")) {
            thresholds.clear();
            for (const json& t : j["thresholds"])
                if (t.is_number_integer()) thresholds.push_back(t.get<int>());
            if (thresholds.size() != kStages - 1 || !std::is_sorted(thresholds.begin(), thresholds.end())) {
                err = "stages.json: \"thresholds\" must be three rising whole numbers";
                return false;
            }
        }
        looks.clear();
        for (const json& l : j.value("looks", json::array())) {
            if (!l.contains("map") || !l["map"].is_number_integer()) { err = "stages.json: a look needs \"map\""; return false; }
            Look k;
            k.palette = std::clamp(l.value("palette", 0.0f), 0.0f, 1.0f);
            k.rays = std::clamp(l.value("rays", 0.0f), 0.0f, 1.0f);
            looks[l["map"].get<unsigned>()] = k;
        }
        const json p = j.value("palette", json::object());
        palette.bright = p.value("bright", palette.bright);
        palette.warm = p.value("warm", palette.warm);
        palette.saturation = p.value("saturation", palette.saturation);
        palette.lift = p.value("lift", palette.lift);
        const json r = j.value("rays", json::object());
        rays.episode = std::max(4.0f, r.value("episode", rays.episode));
        rays.fade = std::clamp(r.value("fade", rays.fade), 0.5f, rays.episode / 2);
        if (!read_array(r, "chance", rays.chance)) { err = "stages.json: rays \"chance\" must be numbers"; return false; }
        rays.slant = r.value("slant", rays.slant);
        rays.spacing = std::max(20.0f, r.value("spacing", rays.spacing));
        if (r.contains("width") && r["width"].is_array() && r["width"].size() == 2) {
            rays.width0 = std::max(2.0f, r["width"][0].get<float>());
            rays.width1 = std::max(rays.width0, r["width"][1].get<float>());
        }
        rays.drift = r.value("drift", rays.drift);
        rays.strength = std::clamp(r.value("strength", rays.strength), 0.0f, 1.0f);
        if (r.contains("colour") && r["colour"].is_array() && r["colour"].size() == 3)
            for (int i = 0; i < 3; ++i) rays.colour[i] = std::clamp(r["colour"][size_t(i)].get<int>(), 0, 255);

        const json h = j.value("heartland", json::object());
        if (h.contains("one_in")) {
            std::array<float, kStages> f{};
            if (!read_array(h, "one_in", f)) { err = "stages.json: heartland \"one_in\" must be numbers"; return false; }
            for (int i = 0; i < kStages; ++i) heartland_one_in[size_t(i)] = std::max(0, int(f[size_t(i)]));
        }
        heartland.clear();
        for (const json& a : h.value("areas", json::array())) {
            const json rc = a.value("rect", json::array());
            if (!a.contains("map") || rc.size() != 4) { err = "stages.json: a heartland area needs \"map\" and \"rect\""; return false; }
            heartland.push_back({a["map"].get<unsigned>(), rc[0].get<int>(), rc[1].get<int>(), rc[2].get<int>(), rc[3].get<int>()});
        }
        heartland_not_shapes.clear();
        for (const json& s : h.value("not_shapes", json::array())) heartland_not_shapes.insert(s.get<unsigned>());
        const json pc = j.value("peace", json::object());
        peace_maps.clear();
        peace_shapes.clear();
        peace_steps.clear();
        for (const json& m : pc.value("maps", json::array())) peace_maps.insert(m.get<unsigned>());
        for (const json& s : pc.value("shapes", json::array())) peace_shapes.insert(s.get<unsigned>());
        for (const json& s : pc.value("steps", json::array()))
            peace_steps.emplace_back(s.value("peace", 0), std::max(0, s.value("one_in", 0)));
        std::sort(peace_steps.begin(), peace_steps.end());

        music_swaps.clear();
        for (const json& m : j.value("music", json::array())) {
            MusicSwap s;
            for (const json& n : m.value("maps", json::array())) s.maps.insert(n.get<unsigned>());
            s.track = m.value("track", 0);
            s.to = m.value("to", 0);
            s.from_stage = std::clamp(m.value("from_stage", 3), 1, kStages - 1);
            if (s.maps.empty() || s.track <= 0 || s.to <= 0) { err = "stages.json: a music swap needs maps, track and to"; return false; }
            music_swaps.push_back(s);
        }

        // Time of day, weather, lamps, ambience, market, frontier, life (5.3, 5.4, 5.10, 5.12, 5.16, 5.18).
        auto int_set = [](const json& a, std::set<int>& out) {
            if (!a.is_array()) return;
            out.clear();
            for (const json& v : a) if (v.is_number_integer()) out.insert(v.get<int>());
        };
        auto uint_set = [](const json& a, std::set<unsigned>& out) {
            out.clear();
            if (!a.is_array()) return;
            for (const json& v : a) if (v.is_number_integer()) out.insert(v.get<unsigned>());
        };
        auto when_of = [](const json& o) {
            const std::string w = o.value("when", std::string("any"));
            return w == "day" ? 1 : w == "night" ? 2 : 0;
        };
        if (j.contains("night_periods")) int_set(j["night_periods"], night_periods);
        if (j.contains("morning_periods")) int_set(j["morning_periods"], morning_periods);
        const json wx = j.value("weather", json::object());
        clear_maps.clear();
        for (const json& m : wx.value("maps", json::array()))
            clear_maps[m.value("map", 0u)] = std::clamp(m.value("weight", 1.0f), 0.0f, 1.0f);
        if (!read_array(wx, "clear", clear_by_stage) || !read_array(wx, "skip_flash", skip_flash)) {
            err = "stages.json: weather \"clear\" and \"skip_flash\" must be numbers";
            return false;
        }
        clear_mornings_from = wx.value("clear_mornings_from", clear_mornings_from);
        if (wx.contains("thunder")) int_set(wx["thunder"], thunder_sfx);
        const json lp = j.value("lamps", json::object());
        uint_set(lp.value("maps", json::array()), glow_maps);
        if (!read_array(lp, "by_stage", glow_by_stage)) { err = "stages.json: lamps \"by_stage\" must be numbers"; return false; }
        if (lp.contains("colour") && lp["colour"].is_array() && lp["colour"].size() == 3)
            for (int i = 0; i < 3; ++i) glow_colour[i] = std::clamp(lp["colour"][size_t(i)].get<int>(), 0, 255);
        glow_shapes.clear();
        for (const json& g : lp.value("shapes", json::array())) {
            GlowShape gs;
            gs.radius = std::max(4.0f, g.value("radius", gs.radius));
            gs.strength = std::clamp(g.value("strength", gs.strength), 0.0f, 1.0f);
            gs.dz = g.value("dz", 0);
            for (const json& sh : g.value("shapes", json::array())) glow_shapes[sh.get<unsigned>()] = gs;
        }
        ambience.clear();
        for (const json& a : j.value("ambience", json::array())) {
            Ambience am;
            uint_set(a.value("maps", json::array()), am.maps);
            for (const json& v : a.value("sfx", json::array())) am.sfx.push_back(v.get<int>());
            if (!read_array(a, "chance", am.chance)) { err = "stages.json: ambience \"chance\" must be numbers"; return false; }
            am.every = std::max(10, a.value("every", am.every));
            am.volume = std::clamp(a.value("volume", am.volume), 1, 255);
            am.when = when_of(a);
            am.min_peace = std::max(0, a.value("min_peace", 0));
            if (am.maps.empty() || am.sfx.empty()) { err = "stages.json: an ambience needs maps and sfx"; return false; }
            ambience.push_back(am);
        }
        const json hu = j.value("hush", json::object());
        uint_set(hu.value("maps", json::array()), hush_maps);
        hush_sfx_set.clear();
        if (hu.contains("sfx")) int_set(hu["sfx"], hush_sfx_set);
        const json mk = j.value("market", json::object());
        uint_set(mk.value("merchants", json::array()), merchants);
        if (mk.contains("rebate_pct")) {
            std::array<float, kStages> f{};
            if (!read_array(mk, "rebate_pct", f)) { err = "stages.json: market \"rebate_pct\" must be numbers"; return false; }
            for (int i = 0; i < kStages; ++i) rebate_pct[size_t(i)] = std::clamp(int(f[size_t(i)]), 0, 50);
        }
        const json fr = j.value("frontier", json::object());
        uint_set(fr.value("maps", json::array()), frontier_maps);
        frontier_share = std::clamp(fr.value("share", frontier_share), 0.0f, 2.0f);
        frontier_cap = std::max(0, fr.value("cap", frontier_cap));
        life.clear();
        for (const json& l : j.value("life", json::array())) {
            Life lf;
            lf.id = l.value("id", std::string());
            lf.shape = l.value("shape", 0u);
            lf.activity = l.value("activity", 0);
            lf.when = when_of(l);
            lf.no_xp = l.value("no_xp", true);
            std::array<float, kStages> c{};
            if (!read_array(l, "count", c)) { err = "stages.json: life \"count\" must be numbers"; return false; }
            for (int i = 0; i < kStages; ++i) lf.count[size_t(i)] = std::max(0, int(c[size_t(i)]));
            for (const json& sp : l.value("spots", json::array())) {
                if (!sp.is_array() || sp.size() != 4) { err = "stages.json: a life spot is [map, x, y, z]"; return false; }
                lf.spots.push_back({sp[0].get<int>(), sp[1].get<int>(), sp[2].get<int>(), sp[3].get<int>()});
                lf.maps.insert(sp[0].get<unsigned>());
            }
            if (lf.id.empty() || !lf.shape || lf.spots.empty()) { err = "stages.json: a life entry needs id, shape and spots"; return false; }
            life.push_back(lf);
        }
    }
    if (!greet.empty()) {
        json j = json::parse(greet, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { err = "greetings.json: not valid JSON"; return false; }
        greet_maps.clear();
        guard_shapes.clear();
        greet_not_shapes.clear();
        for (const json& m : j.value("maps", json::array())) greet_maps.insert(m.get<unsigned>());
        for (const json& m : j.value("guard_shapes", json::array())) guard_shapes.insert(m.get<unsigned>());
        for (const json& m : j.value("not_shapes", json::array())) greet_not_shapes.insert(m.get<unsigned>());
        people_shapes.clear();
        greet_not_npcs.clear();
        for (const json& m : j.value("not_npcs", json::array())) greet_not_npcs.insert(m.get<unsigned>());
        for (const json& m : j.value("people_shapes", json::array())) people_shapes.insert(m.get<unsigned>());
        greet_range = j.value("range", greet_range);
        greet_check = std::max(1, j.value("check", greet_check));
        greet_wait = j.value("wait", greet_wait);
        greet_person_wait = j.value("person_wait", greet_person_wait);
        if (!read_array(j, "chance", greet_chance)) { err = "greetings.json: \"chance\" must be numbers"; return false; }
        greetings.clear();
        for (const json& l : j.value("lines", json::array())) {
            Greeting g;
            g.key = l.value("key", std::string());
            g.text = l.value("text", std::string());
            g.min_stage = std::clamp(l.value("min_stage", 1), 1, kStages - 1);
            const std::string who = l.value("who", std::string("town"));
            g.who = who == "guard" ? 1 : who == "any" ? 2 : 0;
            if (g.text.empty()) { err = "greetings.json: a line needs \"text\""; return false; }
            greetings.push_back(g);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The stage

int WorldStages::natural_stage() const {
    const Virtues* v = w_->virtues.get();
    if (!v || !v->enabled) return 0;
    const int total = v->total();
    int s = 0;
    for (int t : thresholds)
        if (total >= t) ++s;
    return std::min(s, kStages - 1);
}

int WorldStages::stage() const {
    const Virtues* v = w_->virtues.get();
    if (!v || !v->enabled) return 0;
    if (forced >= 0) return std::min(forced, kStages - 1);
    return natural_stage();
}

void WorldStages::reset() {
    hatch_count_.clear();
    greeted_.clear();
    heard_.clear();
    greeted_once_ = false;
    last_greet_ = frames_;
    flash_skipped_ = false;
    ambience_next_.clear();
    money_ = -1;
    money_talk_ = 0;
    life_out.clear();
    life_done_.clear();
    life_map_ = 0xFFFF;
}

// ---------------------------------------------------------------------------------------------
// Look

void WorldStages::view_frame(double dt) {
    const unsigned map = w_->map_number_now();
    const int s = stage();
    auto it = looks.find(map);
    const float target = it == looks.end() ? 0.0f : it->second.palette * float(s) / float(kStages - 1);
    // Eased both on a stage change and on a map change: in the stitched world a seam crossing is seamless.
    // Only the very first frame (nothing shown yet) jumps.
    if (shift_map_ == 0xFFFF) {
        shift_ = target;
        shift_map_ = map;
    } else if (shift_ != target) {
        const float step = float(std::max(0.0, dt)) * 0.5f;  // the whole shift in about 2 s
        shift_ = shift_ < target ? std::min(target, shift_ + step) : std::max(target, shift_ - step);
    }
    if (!w_->kernel().paused()) t_ += std::clamp(dt, 0.0, 0.25);
    view_glows(s, map);
    // The rays: lit episodes by stage, on this map's weight, dimmed with the palette (fades to black).
    rays_level_ = 0;
    if (it == looks.end() || it->second.rays <= 0 || s <= 0) return;
    const double ep = rays.episode;
    const uint32_t n = uint32_t(int64_t(std::floor(t_ / ep)) & 0x7FFFFFFF);
    if (unit(hash32(n, map, 77)) >= rays.chance[size_t(s)]) return;
    const double in = t_ - double(n) * ep;
    const double edge = std::min(in, ep - in) / rays.fade;
    const double env = edge >= 1 ? 1 : 0.5 - 0.5 * std::cos(std::max(0.0, edge) * M_PI);
    uint8_t white[3] = {255, 255, 255}, lit[3];
    w_->palette.apply(white, lit);
    const double bright = (lit[0] + lit[1] + lit[2]) / (3.0 * 255.0);
    rays_level_ = float(env * it->second.rays * bright * bright);
}

uint32_t WorldStages::filter(uint32_t c) const {
    if (shift_ <= 0) return c;
    const float s = shift_;
    float r = float((c >> 16) & 255) / 255.0f, g = float((c >> 8) & 255) / 255.0f, b = float(c & 255) / 255.0f;
    // Lift the darks (a gamma below 1), then more colour around the grey, then warmth and light.
    const float gam = 1.0f - palette.lift * s;
    r = std::pow(r, gam);
    g = std::pow(g, gam);
    b = std::pow(b, gam);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float sat = 1.0f + palette.saturation * s;
    r = y + (r - y) * sat;
    g = y + (g - y) * sat;
    b = y + (b - y) * sat;
    const float br = 1.0f + palette.bright * s, wm = palette.warm * s;
    r *= br * (1.0f + wm);
    g *= br * (1.0f + wm * 0.35f);
    b *= br * (1.0f - wm * 0.6f);
    auto q = [](float v) { return uint32_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); };
    return (c & 0xFF000000u) | (q(r) << 16) | (q(g) << 8) | q(b);
}

bool WorldStages::world_lut(const uint32_t in[256], uint32_t out[256]) const {
    if (shift_ <= 0) return false;
    for (int i = 0; i < 256; ++i) out[i] = filter(in[i]);
    return true;
}

void WorldStages::rays_row(uint32_t* row, int n, double wx0, double wdx, double wy, double top) const {
    if (rays_level_ <= 0 || n <= 0 || wdx <= 0) return;
    const double P = rays.spacing, t = t_;
    const double vert = 1.0 - 0.6 * std::clamp(top, 0.0, 1.0);  // brighter toward the top
    const double shift = wy * rays.slant + t * rays.drift;     // a beam's centre: u_k + shift
    const double wx1 = wx0 + wdx * n;
    const int k0 = int(std::floor((wx0 - shift - rays.width1) / P)) - 1;
    const int k1 = int(std::floor((wx1 - shift + rays.width1) / P)) + 1;
    const uint32_t ep = uint32_t(int64_t(std::floor(t / rays.episode)) & 0x7FFFFFFF);
    for (int k = k0; k <= k1; ++k) {
        const uint32_t h = hash32(uint32_t(k), ep, 11);
        if (unit(h) > 0.75f) continue;  // about three beams in four places
        const double u = k * P + unit(hash32(uint32_t(k), 3)) * P * 0.5;
        const double half = 0.5 * (rays.width0 + (rays.width1 - rays.width0) * unit(hash32(uint32_t(k), ep, 5)));
        // Each beam swells and fades on its own slow beat, and is patchy along its length.
        const double period = 7.0 + 8.0 * unit(hash32(uint32_t(k), 9));
        const double beat = 0.65 - 0.35 * std::cos(2 * M_PI * (t / period + unit(hash32(uint32_t(k), ep, 13))));
        const double along = 0.7 + 0.3 * std::sin(wy * 0.012 + k * 2.3 + t * 0.15);
        const double I = rays.strength * rays_level_ * beat * along * vert;
        if (I <= 0.004) continue;
        const double c = u + shift;
        int xa = int(std::ceil((c - half - wx0) / wdx)), xb = int(std::floor((c + half - wx0) / wdx));
        xa = std::max(xa, 0);
        xb = std::min(xb, n - 1);
        for (int x = xa; x <= xb; ++x) {
            const double d = (wx0 + wdx * x - c) / half;
            const double p = (1 - d * d);
            const int a = int(I * p * p * 256.0);
            if (a <= 0) continue;
            uint32_t px = row[x];
            int r = int((px >> 16) & 255) + ((rays.colour[0] * a) >> 8);
            int g = int((px >> 8) & 255) + ((rays.colour[1] * a) >> 8);
            int b = int(px & 255) + ((rays.colour[2] * a) >> 8);
            row[x] = (px & 0xFF000000u) | (uint32_t(std::min(r, 255)) << 16) | (uint32_t(std::min(g, 255)) << 8) |
                     uint32_t(std::min(b, 255));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Monsters

int WorldStages::peace_count() const {
    const Virtues* v = w_->virtues.get();
    if (!v) return 0;
    int n = 0;
    while (n < 1000 && v->has_paid(kSpirituality, "peace/" + std::to_string(n + 1))) ++n;
    return n;
}

int WorldStages::skip_one_in(const Item& egg) const {
    const Virtues* v = w_->virtues.get();
    if (!v || !v->enabled) return 0;
    if (egg.frame != 0) return 0;  // one-shot eggs (destroyed after their hatch) always hatch
    const unsigned map = w_->map_number_now();
    const unsigned shape = egg.quality & 0x7FF;
    int best = 0;
    auto take = [&best](int n) { if (n > 0 && (best == 0 || n < best)) best = n; };
    const int s = stage();
    if (heartland_one_in[size_t(s)] > 0 && !heartland_not_shapes.count(shape))
        for (const Area& a : heartland)
            if (a.map == map && egg.x >= a.x0 && egg.x <= a.x1 && egg.y >= a.y0 && egg.y <= a.y1) {
                take(heartland_one_in[size_t(s)]);
                break;
            }
    if (peace_maps.count(map) && peace_shapes.count(shape) && !peace_steps.empty()) {
        const int pc = peace_count();
        int n = 0;
        for (const auto& st : peace_steps)
            if (pc >= st.first) n = st.second;
        take(n);
    }
    return best;
}

bool WorldStages::skip_hatch(const Item& egg) {
    const int n = skip_one_in(egg);
    if (n <= 0) return false;
    const uint64_t key = (uint64_t(w_->map_number_now()) << 48) | (uint64_t(uint16_t(egg.x)) << 32) |
                         (uint64_t(uint16_t(egg.y)) << 16) | uint64_t(egg.z & 0xFFFF);
    const unsigned c = ++hatch_count_[key];
    if (c % unsigned(n) != 0) return false;
    ++hatches_skipped;
    // 5.16: the XP that monster would have paid waits for the frontier.
    if (w_->leveling && w_->leveling->active()) {
        auto m = w_->leveling->monsters.find(egg.quality & 0x7FF);
        if (m != w_->leveling->monsters.end())
            if (const MonsterVariant* mv = m->second.variant(m->second.level_on_map(w_->map_number_now())))
                add_frontier_pool(mv->xp);
    }
    PG_LOG("world stages: monster egg %u (shape %u) at %d,%d,%d stays empty (one hatch in %d)", egg.id,
           egg.quality & 0x7FF, egg.x, egg.y, egg.z, n);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Music

int WorldStages::music(int track) const {
    const int s = stage();
    if (s <= 0 || track <= 0) return track;
    const unsigned map = w_->map_number_now();
    for (const MusicSwap& m : music_swaps)
        if (m.track == track && s >= m.from_stage && m.maps.count(map)) return m.to;
    return track;
}

// ---------------------------------------------------------------------------------------------
// Greetings

bool WorldStages::key_done(const std::string& key) const {
    const Virtues* v = w_->virtues.get();
    return key.empty() || (v && v->paid.count(key));
}

std::string WorldStages::fill(const std::string& text) const {
    const Virtues* v = w_->virtues.get();
    const bool known = v && v->marked("known_avatar");
    std::string out = text;
    for (const char* tag : {"{you}", "{You}"}) {
        const std::string rep = known ? "Avatar" : (tag[1] == 'Y' ? "Stranger" : "stranger");
        for (size_t p; (p = out.find(tag)) != std::string::npos;) out.replace(p, 5, rep);
    }
    return out;
}

bool WorldStages::greeter_ok(uint16_t id, int ax, int ay, int az) const {
    const Actor* a = w_->actor(id);
    if (!a || a->parent || !(a->ext_flags & kExtInChunk) || a->mapnum != w_->map_number_now()) return false;
    if (a->is(Actor::kDead) || a->is(Actor::kInCombat) || a->is(Actor::kFeignDeath) || (a->flags & kItemInvisible))
        return false;
    if (greet_not_shapes.count(a->shape) || greet_not_npcs.count(id)) return false;
    // People only: the named townsfolk (no row in the monster table) and the listed people shapes
    // (guards, peasants); never a monster. Anyone fighting is out (above); the alignment bytes are no
    // guide here (Tenebrae's guards carry the Avatar's alignment as an enemy from the start).
    if (monster_info(a->shape) && !people_shapes.count(a->shape)) return false;
    if (w_->party && w_->party->is_companion(id)) return false;
    int x, y, z;
    w_->location(*a, x, y, z);
    if (std::abs(x - ax) > greet_range || std::abs(y - ay) > greet_range || std::abs(z - az) > 48) return false;
    return w_->map().point_is_fast(x, y);
}

bool WorldStages::greeter_ok(uint16_t id) const {
    const Actor* av = w_->avatar();
    if (!av) return false;
    int ax, ay, az;
    w_->location(*av, ax, ay, az);
    return greeter_ok(id, ax, ay, az);
}

std::vector<const WorldStages::Greeting*> WorldStages::lines_for(uint16_t id) const {
    std::vector<const Greeting*> out;
    const Actor* a = w_->actor(id);
    if (!a) return out;
    const bool guard = guard_shapes.count(a->shape) != 0;
    const int s = stage();
    for (const Greeting& g : greetings) {
        if (s < g.min_stage || !key_done(g.key)) continue;
        if ((g.who == 0 && guard) || (g.who == 1 && !guard)) continue;
        out.push_back(&g);
    }
    return out;
}

uint16_t WorldStages::try_greet(bool force) {
    const int s = stage();
    if (s <= 0 || greetings.empty() || !greet_maps.count(w_->map_number_now())) return 0;
    if (w_->stasis || w_->in_conversation() || !w_->barks.empty() || (w_->turn && w_->turn->active())) return 0;
    const Actor* av = w_->avatar();
    if (!av || av->is(Actor::kDead) || av->is(Actor::kInCombat)) return 0;
    if (!force) {
        if (greeted_once_ && frames_ - last_greet_ < uint32_t(greet_wait)) return 0;
        std::uniform_real_distribution<float> u(0, 1);
        if (u(rng_) >= greet_chance[size_t(s)]) return 0;
    }
    int ax, ay, az;
    w_->location(*av, ax, ay, az);
    std::vector<uint16_t> who;
    for (uint16_t id = 2; id < 256; ++id) {
        if (!greeter_ok(id, ax, ay, az)) continue;
        auto g = greeted_.find(id);
        if (!force && g != greeted_.end() && frames_ - g->second < uint32_t(greet_person_wait)) continue;
        if (!lines_for(id).empty()) who.push_back(id);
    }
    if (who.empty()) return 0;
    const uint16_t id = who[std::uniform_int_distribution<size_t>(0, who.size() - 1)(rng_)];
    std::vector<const Greeting*> lines = lines_for(id), fresh;
    for (const Greeting* g : lines)
        if (!g->key.empty() && !heard_.count(g->text)) fresh.push_back(g);
    const std::vector<const Greeting*>& pool = fresh.empty() ? lines : fresh;
    const Greeting* pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng_)];
    if (pool.size() > 1 && pick->text == last_greeting)
        pick = pool[(size_t(std::find(pool.begin(), pool.end(), pick) - pool.begin()) + 1) % pool.size()];
    heard_.insert(pick->text);
    last_greeting = pick->text;
    greeted_[id] = frames_;
    last_greet_ = frames_;
    greeted_once_ = true;
    ++greetings_said;
    w_->bark(id, fill(pick->text));
    return id;
}

// ---------------------------------------------------------------------------------------------
// Time of day

int WorldStages::period() const {
    if (forced_period >= 0) return forced_period % 6;
    return int((w_->game_seconds() / 900) % 6);
}

// ---------------------------------------------------------------------------------------------
// Weather (5.3)

float WorldStages::storm_clearing() const {
    const int s = stage();
    if (s <= 0) return 0;
    auto it = clear_maps.find(w_->map_number_now());
    if (it == clear_maps.end() || it->second <= 0) return 0;
    if (s >= clear_mornings_from && morning_periods.count(period())) return 1;
    return std::clamp(it->second * clear_by_stage[size_t(s)], 0.0f, 1.0f);
}

PaletteTransform WorldStages::storm_transform() const {
    const PaletteTransform storm = PaletteTransform::preset(3), clear;
    const float c = storm_clearing();
    if (c <= 0) return storm;
    PaletteTransform out;
    for (size_t i = 0; i < out.m.size(); ++i)
        out.m[i] = int32_t(std::lround(storm.m[i] + (clear.m[i] - storm.m[i]) * double(c)));
    return out;
}

bool WorldStages::skip_storm_flash(uint16_t cls) {
    flash_skipped_ = false;
    if (cls != 1393) return false;  // STORM's loop only: never a spell's or a story flash
    const int s = stage();
    if (s <= 0) return false;
    auto it = clear_maps.find(w_->map_number_now());
    if (it == clear_maps.end()) return false;
    float p = it->second * skip_flash[size_t(s)];
    if (s >= clear_mornings_from && morning_periods.count(period())) p = 1;
    if (std::uniform_real_distribution<float>(0, 1)(rng_) >= p) return false;
    flash_skipped_ = true;
    skipped_flash_at_ = frames_;
    ++flashes_skipped;
    return true;
}

void WorldStages::weather_tick() {
    // A stage or a time of day that changes while the storm is up: ease to the new look.
    if (w_->palette_xform != 3) return;
    const PaletteTransform t = storm_transform();
    if (t.m == w_->palette_target.m) return;
    w_->palette_target = t;
    w_->start_fade(t, 90, 0);
}

// ---------------------------------------------------------------------------------------------
// Lamps (5.4)

void WorldStages::view_glows(int s, unsigned map) {
    glow_level_ = 0;
    glows_.clear();
    if (s <= 0 || !glow_maps.count(map) || !night() || glow_shapes.empty()) return;
    const Actor* av = w_->avatar();
    if (!av) return;
    uint8_t white[3] = {255, 255, 255}, lit[3];
    w_->palette.apply(white, lit);
    const double bright = (lit[0] + lit[1] + lit[2]) / (3.0 * 255.0);
    glow_level_ = float(glow_by_stage[size_t(s)] * bright * bright);
    if (glow_level_ <= 0.01f) return;
    int ax, ay, az;
    w_->location(*av, ax, ay, az);
    std::vector<uint16_t> near;
    w_->area_search(ax, ay, 2048, false, near);
    for (uint16_t id : near) {
        const Item* it = w_->item(id);
        if (!it || it->parent || (it->flags & kItemInvisible)) continue;
        auto g = glow_shapes.find(it->shape);
        if (g == glow_shapes.end()) continue;
        const ShapeInfo& si = w_->data().typeflags.get(it->shape);
        // The middle of the footprint (x, y are its far corner), half way up, then the shape's offset.
        const double x = it->x - si.x() * 16.0, y = it->y - si.y() * 16.0, z = it->z + si.z() * 4.0 + g->second.dz;
        glows_.push_back({x / 4 - y / 4, x / 8 + y / 8 - z, g->second.radius, g->second.strength, id});
    }
}

void WorldStages::glows_row(uint32_t* row, int n, double wx0, double wdx, double wy) const {
    if (glow_level_ <= 0 || glows_.empty() || n <= 0 || wdx <= 0) return;
    for (const GlowAt& g : glows_) {
        const double dy = wy - g.y;
        if (std::abs(dy) >= g.r) continue;
        const double half = std::sqrt(g.r * g.r - dy * dy);
        // A slow flicker of its own.
        const double flick = 0.88 + 0.12 * std::sin(t_ * 5.3 + (g.seed % 97) * 0.7) * std::sin(t_ * 1.7 + g.seed * 0.31);
        const double I = g.a * glow_level_ * flick;
        int xa = int(std::ceil((g.x - half - wx0) / wdx)), xb = int(std::floor((g.x + half - wx0) / wdx));
        xa = std::max(xa, 0);
        xb = std::min(xb, n - 1);
        for (int x = xa; x <= xb; ++x) {
            const double dx = wx0 + wdx * x - g.x;
            const double q = 1.0 - (dx * dx + dy * dy) / (g.r * g.r);
            if (q <= 0) continue;
            const int a = int(I * q * q * 256.0);
            if (a <= 0) continue;
            const uint32_t px = row[x];
            const int r = int((px >> 16) & 255) + ((glow_colour[0] * a) >> 8);
            const int gg = int((px >> 8) & 255) + ((glow_colour[1] * a) >> 8);
            const int b = int(px & 255) + ((glow_colour[2] * a) >> 8);
            row[x] = (px & 0xFF000000u) | (uint32_t(std::min(r, 255)) << 16) | (uint32_t(std::min(gg, 255)) << 8) |
                     uint32_t(std::min(b, 255));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Ambience (5.10)

bool WorldStages::hush_sfx(int sfx) {
    ++sfx_heard[sfx];
    // The thunder after a storm flash that was left out.
    if (flash_skipped_ && thunder_sfx.count(sfx) && frames_ - skipped_flash_at_ < 90) {
        ++sfx_hushed;
        return true;
    }
    // The Cemetery's moans, as its dead are laid to rest (the same steps as the hatches, 5.17).
    if (!hush_sfx_set.count(sfx) || !hush_maps.count(w_->map_number_now()) || peace_steps.empty()) return false;
    const Virtues* v = w_->virtues.get();
    if (!v || !v->enabled) return false;
    const int pc = peace_count();
    int n = 0;
    for (const auto& st : peace_steps)
        if (pc >= st.first) n = st.second;
    if (n <= 0) return false;
    static_assert(sizeof(int) == 4, "");
    const unsigned c = ++hatch_count_[0xFFFF000000000000ull | uint64_t(uint32_t(sfx))];
    if (c % unsigned(n) != 0) return false;
    ++sfx_hushed;
    return true;
}

void WorldStages::ambience_tick(bool force) {
    const int s = stage();
    if (s <= 0 || ambience.empty()) return;
    const unsigned map = w_->map_number_now();
    for (size_t i = 0; i < ambience.size(); ++i) {
        const Ambience& a = ambience[i];
        if (!a.maps.count(map)) continue;
        if ((a.when == 1 && night()) || (a.when == 2 && !night())) continue;
        if (a.min_peace > 0 && peace_count() < a.min_peace) continue;
        if (!force) {
            auto nx = ambience_next_.find(i);
            if (nx == ambience_next_.end()) {
                // The first try comes at a random point of the interval.
                ambience_next_[i] = frames_ + uint32_t(std::uniform_int_distribution<int>(1, a.every)(rng_));
                continue;
            }
            if (frames_ < nx->second) continue;
            // Each try lands somewhere in the next interval, so the sounds never keep time.
            nx->second = frames_ + uint32_t(a.every / 2 + std::uniform_int_distribution<int>(0, a.every)(rng_));
            if (std::uniform_real_distribution<float>(0, 1)(rng_) >= a.chance[size_t(s)]) continue;
        }
        const int sfx = a.sfx[std::uniform_int_distribution<size_t>(0, a.sfx.size() - 1)(rng_)];
        w_->sound.play_sfx(sfx, 0x30, 0, 0, a.volume, w_->sound.random_pitch());
        last_ambience = sfx;
        ++ambience_played;
        if (force) return;
    }
}

// ---------------------------------------------------------------------------------------------
// Market (5.12)

int WorldStages::obsidian_carried() const {
    int n = 0;
    std::vector<uint16_t> todo;
    if (const Item* av = w_->item(1)) todo = av->contents;
    while (!todo.empty()) {
        const Item* it = w_->item(todo.back());
        todo.pop_back();
        if (!it) continue;
        if (it->shape == 143) n += it->quality;
        todo.insert(todo.end(), it->contents.begin(), it->contents.end());
    }
    return n;
}

uint16_t WorldStages::merchant_now() const {
    // Whoever the Avatar is dealing with: the process behind an answer menu or a conversation (a
    // permanent NPC's class is 1024 + its number, kernel.md 6), or the engine's own dialogue.
    auto of = [this](uint16_t pid) -> uint16_t {
        const UCProcess* p = pid ? dynamic_cast<const UCProcess*>(w_->kernel().get(pid)) : nullptr;
        if (!p || !p->live()) return 0;
        for (uint16_t c : {p->cls, p->start_cls})
            if (c >= 1024 && c < 1024 + 256 && merchants.count(c - 1024u)) return uint16_t(c - 1024);
        return merchants.count(p->item) ? p->item : 0;
    };
    if (w_->ask_menu)
        if (uint16_t m = of(w_->ask_menu->pid)) return m;
    if (uint16_t m = of(w_->talk_pid)) return m;
    if (w_->dialog && w_->dialog->active() && merchants.count(w_->dialog->speaker())) return w_->dialog->speaker();
    return 0;
}

void WorldStages::market_tick() {
    const int s = stage();
    if (s <= 0 || rebate_pct[size_t(s)] <= 0 || merchants.empty()) {
        money_ = -1;
        return;
    }
    // A sale may be paid just after the last answer: the till stays open a second and a half.
    if (uint16_t m = merchant_now()) {
        if (money_ < 0 || m != money_talk_) money_ = obsidian_carried();
        money_talk_ = m;
        money_seen_ = frames_;
    } else if (money_ < 0 || frames_ - money_seen_ > 45) {
        money_ = -1;
        money_talk_ = 0;
        return;
    }
    const int now = obsidian_carried();
    if (now < money_) {
        rebate_carry_ += (money_ - now) * rebate_pct[size_t(s)];
        const int back = rebate_carry_ / 100;
        rebate_carry_ %= 100;
        if (back > 0) {
            const uint16_t id = w_->create_item(143, 0);
            const uint16_t pack = w_->equipped(1, 7);
            if (Item* c = w_->item(id)) c->quality = uint16_t(back);
            if (id && w_->move_to_container(id, pack ? pack : 1)) {
                w_->call_event(id, kEvCombine);
                rebated += unsigned(back);
                w_->game_log.add(GameLog::kMessages, GameLog::kInfo,
                                 "Trade is good again: " + std::to_string(back) + " obsidian back.", money_talk_);
                PG_LOG("world stages: merchant %u gives %d obsidian back (%d%%)", money_talk_, back, rebate_pct[size_t(s)]);
            } else if (id) {
                w_->destroy_now(id);
            }
        }
        money_ = obsidian_carried();
        return;
    }
    money_ = now;
}

// ---------------------------------------------------------------------------------------------
// XP to the frontier (5.16)

namespace {
const std::string kPoolKey = "stages/pool/";
}

int WorldStages::frontier_pool() const {
    const Virtues* v = w_->virtues.get();
    if (!v) return 0;
    auto it = v->paid.lower_bound(kPoolKey);
    if (it == v->paid.end() || it->compare(0, kPoolKey.size(), kPoolKey) != 0) return 0;
    return std::atoi(it->c_str() + kPoolKey.size());
}

void WorldStages::add_frontier_pool(int xp) {
    Virtues* v = w_->virtues.get();
    if (!v || xp == 0) return;
    const int now = std::clamp(frontier_pool() + xp, 0, frontier_cap);
    for (auto it = v->paid.lower_bound(kPoolKey); it != v->paid.end() && it->compare(0, kPoolKey.size(), kPoolKey) == 0;)
        it = v->paid.erase(it);
    if (now > 0) v->paid.insert(kPoolKey + std::to_string(now));
}

int WorldStages::frontier_bonus(int paid) {
    if (paid <= 0 || !frontier_maps.count(w_->map_number_now())) return 0;
    const int pool = frontier_pool();
    const int b = std::min(pool, int(std::lround(paid * double(frontier_share))));
    if (b <= 0) return 0;
    add_frontier_pool(-b);
    return b;
}

// ---------------------------------------------------------------------------------------------
// Life (5.18, 5.11)

bool WorldStages::is_life(uint16_t id) const {
    for (const auto& kv : life_out)
        for (uint16_t a : kv.second)
            if (a == id) return true;
    return false;
}

void WorldStages::life_tick() {
    const unsigned map = w_->map_number_now();
    if (map != life_map_) {
        life_out.clear();
        life_done_.clear();
        life_map_ = map;
    }
    const int s = stage();
    const Actor* av = w_->avatar();
    if (!av || w_->stasis) return;
    int ax, ay, az;
    w_->location(*av, ax, ay, az);
    for (const Life& lf : life) {
        if (!lf.maps.count(map)) continue;
        std::vector<uint16_t>& out = life_out[lf.id];
        std::vector<uint8_t>& done = life_done_[lf.id];
        out.resize(lf.spots.size(), 0);
        done.resize(lf.spots.size(), 0);
        const int want = s > 0 ? lf.count[size_t(s)] : 0;
        for (size_t i = 0; i < lf.spots.size(); ++i) {
            uint16_t& id = out[i];
            if (id) {
                const Actor* a = w_->actor(id);
                if (!a || a->shape != lf.shape) id = 0;          // gone with the fast area: may come back
                else if (a->is(Actor::kDead)) { done[i] = 1; id = 0; }  // slain: not again this visit
                continue;
            }
            const auto& sp = lf.spots[i];
            if (done[i] || int(i) >= want || unsigned(sp[0]) != map) continue;
            if ((lf.when == 1 && night()) || (lf.when == 2 && !night())) continue;
            if (!w_->map().point_is_fast(sp[1], sp[2])) continue;
            if (std::abs(sp[1] - ax) < 192 && std::abs(sp[2] - ay) < 192) continue;  // not under his feet
            // The spot, or a free and supported place close by (a barrel or a passer-by may stand there).
            static const int kNear[][2] = {{0, 0}, {64, 0}, {0, 64}, {-64, 0}, {0, -64}, {128, 128}, {-128, -128}, {128, -128}, {-128, 128}};
            int px = 0, py = 0, pz = -1;
            for (const auto& o : kNear) {
                for (int dz = 0; dz <= 16 && pz < 0; dz += 8) {
                    const World::Placement pl = w_->can_exist_shape(lf.shape, sp[1] + o[0], sp[2] + o[1], sp[3] + dz, 0);
                    if (pl.valid && pl.supported) { px = sp[1] + o[0]; py = sp[2] + o[1]; pz = sp[3] + dz; }
                }
                if (pz >= 0) break;
            }
            if (pz < 0) continue;
            const uint16_t nid = w_->create_monster(lf.shape, 0);
            Actor* a = w_->actor(nid);
            if (!a) continue;
            a->flags |= kItemFastOnly | kItemDisposable | kItemInNpcList;
            if (lf.no_xp) a->ext_flags |= kExtXpPaid;  // nothing to farm
            w_->move_to(nid, px, py, pz);
            w_->set_activity(nid, unsigned(lf.activity));
            id = nid;
            ++life_spawned;
            PG_LOG("world stages: %s %u (shape %u) at %d,%d,%d", lf.id.c_str(), nid, lf.shape, px, py, pz);
        }
    }
}

void WorldStages::frame() {
    ++frames_;
    if (frames_ % uint32_t(greet_check) == 0) try_greet(false);
    if (frames_ % 30 == 0) weather_tick();
    if (frames_ % 30 == 11) life_tick();
    ambience_tick(false);
    market_tick();
}

}  // namespace pg
