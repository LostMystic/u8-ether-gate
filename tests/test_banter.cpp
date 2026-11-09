// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Travel banter between companions (game/banter.*, data/banter.json, data/banter_points.json) and the
// INT-gated topics of the engine-side NPC dialogue (game/npc_dialog.*, "avatar_int").
//   pagan_banter_tests GAME_DIR DATA_DIR [SAVE_DIR] [ONLY]
//   ONLY = a test's name, or cands (walking routes of the critical path's travel maps, sampled, with how
//   far each sample is from monster eggs: how the banter points were chosen)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/banter.h"
#include "game/leveling.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/route_guide.h"
#include "game/ask_nav.h"
#include "game/game_log.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"

using namespace pg;

int g_fail = 0, g_pass = 0;

namespace {

std::string g_saves, g_data;

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Ui {
    GameMode gm;
    Surface8 surf{320, 200};
    Ui(const GameData& d, bool companions = true, bool leveling = false, bool banter = true) : gm(d) {
        World& w = gm.world();
        w.skip_opening = true;
        w.ask_name = false;
        w.play_intro = false;
        w.auto_ui = false;
        gm.saves().set_dir(g_saves);
        std::string err;
        if (!w.party->load_file(g_data + "/companions.json", err)) std::printf("  companions.json: %s\n", err.c_str());
        w.party->beach_always = true;
        w.party->enabled = companions;
        if (banter && !w.party->banter->load_dir(g_data, err)) std::printf("  banter: %s\n", err.c_str());
        if (!w.leveling->load_dir(g_data, err)) std::printf("  leveling data: %s\n", err.c_str());
        w.leveling->enabled = leveling;
        if (!w.dialog->load_dir(g_data + "/dialog", err)) std::printf("  dialog: %s\n", err.c_str());
        w.dialog->enabled = true;
        gm.start();
    }
    World& w() { return gm.world(); }
    Party& p() { return *gm.world().party; }
    Banter& b() { return *gm.world().party->banter; }
    void frames(int n) { for (int i = 0; i < n; ++i) gm.tick(); }
    void frames_drawn(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    int idx(const std::string& id) { return p().find_def(id); }
    CompanionState& st(const std::string& id) { return p().state[size_t(idx(id))]; }
    Actor* a(const std::string& id) { return w().actor(st(id).id); }
    Actor* av() { return w().avatar(); }
    uint32_t now() { return w().kernel().frame_number(); }
    void warp(unsigned map, int x, int y, int z) {
        w().kernel().kill(1, kProcAny);
        w().teleport_actor(1, x, y, z, map);
        frames(3);
    }
    // The followers beside him (as if they had walked with him).
    void bring_party() {
        for (int m : p().members) {
            Actor* c = w().actor(p().state[size_t(m)].id);
            if (!c) continue;
            w().kernel().kill(c->id, kProcAny);
            w().teleport_actor(c->id, av()->x + 64 + 40 * (m % 3), av()->y + 48 * (m / 3), av()->z, av()->mapnum);
        }
        frames(2);
    }
    // Speeds up the speech: each bark gets at most `n` ticks.
    void hurry(int n = 4) { for (Bark& bk : w().barks) bk.ticks_left = std::min(bk.ticks_left, n); }
    // Runs until the current exchange is over (barks hurried); false on a timeout.
    bool finish(int max = 4000) {
        for (int f = 0; f < max; ++f) {
            if (!b().playing()) return true;
            hurry();
            frames(1);
        }
        return !b().playing();
    }
    bool has_log(const std::string& part) {
        for (const auto& l : w().game_log.lines(GameLog::kMessages))
            if (l.text.find(part) != std::string::npos) return true;
        return false;
    }
};

std::vector<uint8_t> save_bytes(World& w) {
    SaveWriter o;
    w.save_game(o, "test");
    return o.data();
}

bool contains_tag(const std::vector<uint8_t>& v, const char* t) {
    return std::search(v.begin(), v.end(), t, t + 4) != v.end();
}

// A test point on map 3: the open shore east of Devon's camp, where the beach companions stand.
const char* kTestPoints = R"({"points": [
  {"id": "test_shore", "map": 3, "x": 15900, "y": 5750, "z": 8, "r": 256, "place": "test"},
  {"id": "test_far", "map": 3, "x": 14400, "y": 5730, "z": 8, "r": 128, "place": "test"}]})";

void use_test_points(Ui& u) {
    std::string err;
    CHECK(u.b().load_text(slurp(g_data + "/banter.json"), kTestPoints, err));
}

// The beach has a few hatched creatures that pick fights: out of the way, so only the test decides
// when there is a fight.
void no_fights(Ui& u) {
    for (uint16_t id = 2; id < 256; ++id) {
        Actor* a = u.w().actor(id);
        if (!a || a->chunk < 0 || u.p().is_companion(id) || a->is(Actor::kDead)) continue;
        if (a->is(Actor::kInCombat) || a->target || id >= 60) u.w().destroy_now(id);
    }
    for (int m : u.p().members) {
        Actor* c = u.w().actor(u.p().state[size_t(m)].id);
        if (c) { c->set(Actor::kInCombat, false); c->target = 0; }
        u.p().state[size_t(m)].target = 0;
    }
}

void at_point(Ui& u) {
    u.warp(3, 15900, 5760, 8);
    u.bring_party();
    no_fights(u);
}

const BanterPair* pair_of(Banter& b, const std::string& x, const std::string& y) {
    for (const BanterPair& p : b.pairs)
        if ((p.a == x && p.b == y) || (p.a == y && p.b == x)) return &p;
    return nullptr;
}

// Waits for a point to fire (or not): true if an exchange started within `n` frames.
bool fires(Ui& u, int n = 40) {
    for (int f = 0; f < n; ++f) {
        if (u.b().playing()) return true;
        if (f % 5 == 0 && !u.av()->is(Actor::kInCombat)) no_fights(u);
        u.frames(1);
    }
    return u.b().playing();
}

// --- The data
const std::vector<std::string> kIds = {"morvain", "kalvax", "brannoc", "varn", "ilvaran", "corvin", "wynna", "nerissa"};

struct Near { int monster = 1 << 30, hatch = 1 << 30; };

Near look_round(World& w, int x, int y) {
    Near n;
    for (size_t i = 2; i < w.map().item_slots(); ++i) {
        const Item* it = w.item(uint16_t(i));
        if (!it || it->chunk < 0) continue;
        int dd = int(std::hypot(double(it->x - x), double(it->y - y)));
        int fam = w.data().typeflags.get(it->shape).family();
        if (fam == kFamMonsterEgg) n.monster = std::min(n.monster, dd);
        else if (fam == 4 && (it->quality & 0xFF) == 60) n.hatch = std::min(n.hatch, dd);
    }
    return n;
}

// Open ground the Avatar can stand on within 48 of the spot (a route point is his foot centre, not his origin).
bool standable(World& w, int x, int y, int z) {
    for (int dz = -16; dz <= 16; dz += 8)
        for (int dy = -48; dy <= 48; dy += 16)
            for (int dx = -48; dx <= 48; dx += 16) {
                World::Placement pl = w.can_exist_shape(1, x + dx, y + dy, z + dz, 1);
                if (pl.valid && pl.supported) return true;
            }
    return false;
}

void test_data(const GameData& d) {
    std::printf("data: banter.json and banter_points.json\n");
    Ui u(d);
    Banter& b = u.b();
    CHECK(b.loaded());
    std::vector<std::string> pr = b.problems();
    for (const std::string& s : pr) std::printf("  problem: %s\n", s.c_str());
    CHECK(pr.empty());
    // Every one of the 28 pairs, at least one exchange and one repeat; 100+ lines in all.
    int lines = 0, exchanges = 0;
    for (size_t i = 0; i < kIds.size(); ++i)
        for (size_t k = i + 1; k < kIds.size(); ++k) {
            const BanterPair* p = pair_of(b, kIds[i], kIds[k]);
            CHECK(p != nullptr);
            if (!p) { std::printf("  no pair %s + %s\n", kIds[i].c_str(), kIds[k].c_str()); continue; }
            CHECK(!p->exchanges.empty());
            CHECK(!p->repeat.lines.empty());
            for (const BanterExchange& e : p->exchanges) {
                CHECK(e.lines.size() >= 2 && e.lines.size() <= 5);
                lines += int(e.lines.size());
            }
            exchanges += int(p->exchanges.size());
        }
    CHECK_EQ(b.pairs.size(), 28u);
    std::printf("  %zu pairs, %d exchanges, %d lines (repeats aside), %zu points\n", b.pairs.size(), exchanges, lines, b.points.size());
    CHECK(lines >= 100);
    CHECK(b.points.size() >= 30 && b.points.size() <= 50);
    // Each point: its map holds no monster egg within 768 of it nor a hatcher within 512 (quiet), and the
    // spot is open ground the Avatar can stand on; points on one map are at least 1000 apart.
    Ui q(d, false, false, false);
    q.frames(10);
    for (const BanterPoint& p : b.points) {
        q.warp(p.map, p.x, p.y, p.z);
        q.frames(5);
        Near n = look_round(q.w(), p.x, p.y);
        const bool stand = standable(q.w(), p.x, p.y, p.z);
        bool ok = q.w().map().map_number() == p.map;
        std::printf("  %-22s map %2u %5d,%5d,%3d r %d: monster egg %5d, hatcher %5d, stand %d  %s\n", p.id.c_str(), p.map, p.x, p.y,
                    p.z, p.r, n.monster == 1 << 30 ? -1 : n.monster, n.hatch == 1 << 30 ? -1 : n.hatch, stand,
                    p.place.c_str());
        CHECK(ok);
        CHECK(n.monster >= 768);
        CHECK(n.hatch >= 512);
        CHECK(stand);
        CHECK(!p.place.empty());
        for (const BanterPoint& o : b.points)
            if (&o != &p && o.map == p.map) CHECK(std::hypot(double(o.x - p.x), double(o.y - p.y)) >= 1000);
    }
    // Bad data is refused.
    std::string err;
    Banter bad(u.w(), u.p());
    CHECK(!bad.load_text("{", kTestPoints, err));
    CHECK(!bad.load_text(R"({"pairs": [{"pair": ["morvain"]}]})", kTestPoints, err));
    CHECK(!bad.load_text(R"({"pairs": [{"pair": ["morvain", "corvin"], "exchanges": [{"id": "x", "lines": ["no"]}]}]})", kTestPoints, err));
}

// --- A point fires: the lines in order by the right speakers, over their heads and in the log.
void test_fire(const GameData& d, bool leveling) {
    std::printf("fire (Leveling %s): a point with two companions walking with him\n", leveling ? "on" : "off");
    Ui u(d, true, leveling);
    use_test_points(u);
    u.frames(60);
    CHECK(u.p().spawned_once);
    // Alone: nothing. One companion: nothing (and the point stays ready).
    at_point(u);
    CHECK(!fires(u));
    u.p().join(u.idx("morvain"));
    at_point(u);
    CHECK(!fires(u));
    CHECK(!u.b().ready_at.count("test_shore"));
    // Somewhere else on the map: nothing.
    u.p().join(u.idx("corvin"));
    u.warp(3, 15000, 5750, 8);
    u.bring_party();
    CHECK(!fires(u));
    // At the point: Morvain and Corvin's first exchange.
    at_point(u);
    uint32_t t0 = u.now();
    CHECK(fires(u));
    const BanterPair* p = pair_of(u.b(), "morvain", "corvin");
    CHECK(p && u.b().current() == &p->exchanges[0]);
    if (!p || !u.b().current()) return;
    CHECK(u.b().ready_at.count("test_shore") && u.b().ready_at["test_shore"] >= t0 &&
          u.b().ready_at["test_shore"] <= t0 + Banter::kRecharge + 40);
    // Each line by its speaker, one at a time, over their head.
    const BanterExchange& e = p->exchanges[0];
    size_t said = 0;
    for (int f = 0; f < 4000 && u.b().playing(); ++f) {
        for (const Bark& bk : u.w().barks)
            if (bk.speaker == u.st("morvain").id || bk.speaker == u.st("corvin").id) {
                if (said < e.lines.size() && bk.speaker == u.st(e.lines[said].who).id && bk.ticks_left > 4) ++said;
            }
        u.hurry();
        u.frames(1);
    }
    CHECK(!u.b().playing());
    CHECK_EQ(said, e.lines.size());
    for (const BanterLine& l : e.lines) CHECK(u.has_log(l.text.substr(0, 30)));
    CHECK(u.b().played.count(e.id));
    std::printf("  %s: %zu lines said; log: %s\n", e.id.c_str(), said, u.has_log(e.lines[0].text.substr(0, 20)) ? "yes" : "no");
    if (leveling) u.frames_drawn(2);
}

// --- Recharge: 5 minutes of play time before the same point fires again; another point is not held.
void test_recharge(const GameData& d) {
    std::printf("recharge: 5 minutes of play time per point\n");
    Ui u(d);
    use_test_points(u);
    u.frames(60);
    u.p().join(u.idx("brannoc"));
    u.p().join(u.idx("wynna"));
    at_point(u);
    CHECK(fires(u));
    const uint32_t fired = u.now();
    CHECK(u.finish());
    // Standing there: nothing until the time is up.
    const uint32_t ready = u.b().ready_at["test_shore"];
    CHECK(ready >= fired + Banter::kRecharge - 40 && ready <= fired + Banter::kRecharge);
    bool early = false;
    while (u.now() + 2 < ready) {
        u.frames(1);
        if (u.b().playing()) early = true;
        if (u.now() % 600 == 0) u.bring_party();  // they idle about; keep them near
    }
    CHECK(!early);
    u.bring_party();
    at_point(u);
    CHECK(fires(u, 60));
    std::printf("  fired at %u, ready at %u (%u frames), fired again at %u\n", fired, ready, ready - fired, u.now());
    CHECK(u.finish());
    // The other point is its own: it fires at once.
    u.warp(3, 14400, 5730, 8);
    u.bring_party();
    CHECK(fires(u, 60));
}

// Fires the test point at once (as if recharged), and plays it through. Returns the exchange id ("" none).
std::string fire_now(Ui& u) {
    u.b().ready_at.erase("test_shore");
    at_point(u);
    if (!fires(u, 30)) {
        if (!u.b().why_not.empty()) std::printf("    (not fired: %s)\n", u.b().why_not.c_str());
        return std::string();
    }
    std::string id = u.b().current()->id;
    u.finish();
    return id;
}

// --- Once per save; the repeat at 1 in 3 once all are heard.
void test_once_and_repeat(const GameData& d) {
    std::printf("once per save, then the repeat one time in three\n");
    Ui u(d);
    use_test_points(u);
    u.frames(60);
    u.p().join(u.idx("kalvax"));
    u.p().join(u.idx("varn"));
    const BanterPair* p = pair_of(u.b(), "kalvax", "varn");
    CHECK(p != nullptr);
    if (!p) return;
    std::vector<std::string> heard;
    for (size_t i = 0; i < p->exchanges.size(); ++i) heard.push_back(fire_now(u));
    for (size_t i = 0; i < p->exchanges.size(); ++i) CHECK(heard[i] == p->exchanges[i].id);  // in file order, each once
    int repeats = 0, quiet = 0, other = 0;
    const int kFires = 90;
    for (int i = 0; i < kFires; ++i) {
        size_t logn = u.b().log.size();
        std::string id = fire_now(u);
        if (id == p->repeat.id) ++repeats;
        else if (id.empty()) {
            bool q = false;
            for (size_t k = logn; k < u.b().log.size(); ++k) q = q || u.b().log[k].find("quiet") != std::string::npos;
            quiet += q;
        } else ++other;
    }
    std::printf("  %zu exchanges once each; then %d fires: %d repeats, %d quiet, %d other\n", p->exchanges.size(), kFires, repeats, quiet,
                other);
    CHECK_EQ(other, 0);
    CHECK_EQ(repeats + quiet, kFires);
    CHECK(repeats >= 18 && repeats <= 42);  // one in three of 90 is 30
    // A repeat is never marked heard.
    CHECK(!u.b().played.count(p->repeat.id));
}

// --- Fairly between pairs: with three companions (three pairs), the pair that has heard least goes first.
void test_fair(const GameData& d) {
    std::printf("fair: three companions, three pairs\n");
    Ui u(d);
    use_test_points(u);
    u.frames(60);
    for (const char* c : {"ilvaran", "nerissa", "wynna"}) u.p().join(u.idx(c));
    std::map<std::string, int> per_pair;
    bool even = true;
    for (int i = 0; i < 6; ++i) {
        std::string id = fire_now(u);
        CHECK(!id.empty());
        std::string key = id.substr(0, id.find_last_of('_'));
        ++per_pair[key];
        int lo = 1 << 30, hi = 0;
        for (const char* k : {"ilvaran_nerissa", "ilvaran_wynna", "wynna_nerissa"}) {
            lo = std::min(lo, per_pair[k]);
            hi = std::max(hi, per_pair[k]);
        }
        even = even && hi - lo <= 1;
    }
    for (auto& kv : per_pair) std::printf("  %s: %d\n", kv.first.c_str(), kv.second);
    CHECK(even);
    CHECK_EQ(per_pair.size(), 3u);
}

// --- No banter in a fight, a conversation or a scene; one cut short is not counted.
void test_calm(const GameData& d) {
    std::printf("calm: no banter in combat, in a conversation, in a scene\n");
    Ui u(d, true, false);
    use_test_points(u);
    u.frames(60);
    u.p().join(u.idx("morvain"));
    u.p().join(u.idx("kalvax"));
    u.b().ready_at["test_shore"] = 1u << 30;  // held while the scene is set up
    at_point(u);
    u.w().set_in_combat(1);
    u.frames(2);
    CHECK(u.av()->is(Actor::kInCombat));
    u.b().ready_at.erase("test_shore");
    CHECK(!fires(u, 60));
    std::printf("  in combat: %s\n", u.b().why_not.c_str());
    CHECK(!u.b().ready_at.count("test_shore"));  // not spent
    u.w().clear_in_combat(1);
    u.w().stasis = true;
    CHECK(!fires(u, 30));
    std::printf("  in a scene: %s\n", u.b().why_not.c_str());
    u.w().stasis = false;
    u.p().start_talk(u.idx("morvain"));
    u.frames(2);
    CHECK(u.p().talking());
    CHECK(!fires(u, 30));
    std::printf("  in a conversation: %s\n", u.b().why_not.c_str());
    u.p().end_talk();
    u.frames(5);
    // It fires now; a fight in the middle stops it, and it is not counted as heard.
    CHECK(fires(u, 600));
    if (!u.b().playing()) std::printf("  not fired: %s\n", u.b().why_not.c_str());
    std::string id = u.b().current() ? u.b().current()->id : std::string();
    u.frames(3);
    u.w().set_in_combat(1);
    u.frames(3);
    CHECK(!u.b().playing());
    CHECK(!u.b().played.count(id));
    u.w().clear_in_combat(1);
    // A speaker sent to wait: no banter with only one following.
    u.p().wait(u.idx("kalvax"));
    u.b().ready_at.clear();
    u.frames(5);
    CHECK(!fires(u, 40));
    u.p().follow(u.idx("kalvax"));
    u.bring_party();
    CHECK(fires(u, 60));
    CHECK(u.b().current() && u.b().current()->id == id);  // the same one again, in full
    CHECK(u.finish());
    CHECK(u.b().played.count(id));
    // Companions off: nothing at all.
    Ui off(d, false);
    use_test_points(off);
    off.frames(60);
    off.warp(3, 15900, 5760, 8);
    CHECK(!fires(off, 60));
    std::vector<uint8_t> s = save_bytes(off.w());
    CHECK(!contains_tag(s, "BNTR"));
}

// --- Saves: what was heard and the points' timers survive a round trip; older saves load.
void test_saves(const GameData& d) {
    std::printf("saves: round trip, older saves\n");
    Ui u(d);
    use_test_points(u);
    u.frames(60);
    // Before anything is heard: no section.
    CHECK(!contains_tag(save_bytes(u.w()), "BNTR"));
    u.p().join(u.idx("corvin"));
    u.p().join(u.idx("varn"));
    std::string first = fire_now(u);
    std::string second = fire_now(u);
    CHECK(!first.empty() && !second.empty() && first != second);
    std::vector<uint8_t> s1 = save_bytes(u.w());
    CHECK(contains_tag(s1, "BNTR"));
    std::set<std::string> played = u.b().played;
    std::map<std::string, uint32_t> ready = u.b().ready_at;
    Ui v(d);
    use_test_points(v);
    v.frames(10);
    SaveReader in(s1.data(), s1.size());
    CHECK(v.w().load_game(in));
    CHECK(v.b().played == played);
    CHECK(v.b().ready_at == ready);
    CHECK(save_bytes(v.w()) == s1);
    // The next one is the third, not the first again.
    v.frames(5);
    v.b().ready_at.erase("test_shore");
    v.bring_party();
    at_point(v);
    CHECK(fires(v, 60));
    const BanterPair* p = pair_of(v.b(), "varn", "corvin");
    CHECK(p && v.b().current() && v.b().current()->id == p->exchanges[2].id);
    // A save without the section (an older build): nothing heard.
    Ui w2(d);
    use_test_points(w2);
    w2.frames(60);
    std::vector<uint8_t> s0 = save_bytes(w2.w());
    CHECK(!contains_tag(s0, "BNTR"));
    SaveReader in0(s0.data(), s0.size());
    CHECK(v.w().load_game(in0));
    CHECK(v.b().played.empty() && v.b().ready_at.empty() && !v.b().playing());
    // A new game forgets it all.
    SaveReader in1(s1.data(), s1.size());
    CHECK(v.w().load_game(in1));
    CHECK(!v.b().played.empty());
    v.w().new_game();
    CHECK(v.b().played.empty() && v.b().ready_at.empty());
}

// --- INT-gated topics (PLAN-LEVELING.md 5): hidden below the Avatar's INT, shown at it, in the NPC's
// own menu in a real conversation, and not with New Dialogue off.
// Talks to the NPC (several times if need be), going through its own answers, until `label` shows in a
// menu. Returns the number of the talk it showed in (0 = never).
int find_entry(Ui& u, unsigned npc, const std::string& label, std::vector<std::string>* seen = nullptr) {
    World& w = u.w();
    std::set<std::string> chosen;
    for (int talk = 1; talk <= 3; ++talk) {
        if (const Actor* g = w.actor(uint16_t(npc))) {
            if (w.map().map_number() != g->mapnum || std::abs(w.avatar()->x - g->x) + std::abs(w.avatar()->y - g->y) > 160) {
                w.teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
                u.frames(30);
            }
        }
        w.call_event(uint16_t(npc), kEvUse);
        int picks = 0;
        for (int f = 0; f < 6000; ++f) {
            u.frames(1);
            for (Bark& b : w.barks) b.ticks_left = std::min(b.ticks_left, 2);
            if (w.ask_menu && w.ask_menu->frames_open > 5) {
                std::vector<std::string> m;
                for (uint16_t sid : w.ask_menu->answers) m.push_back(w.vm().string(sid));
                if (seen) { std::string all; for (auto& x : m) all += x + " | "; seen->push_back(all); }
                for (size_t xi = 0; xi < m.size(); ++xi)
                    if (m[xi] == label) {
                        // The entry's own topic, then back out and goodbye.
                        w.choose_answer(unsigned(xi));
                        for (int k = 0; k < 3000 && (w.in_conversation() || !w.barks.empty()); ++k) {
                            u.frames(1);
                            for (Bark& b : w.barks) b.ticks_left = std::min(b.ticks_left, 2);
                            if (w.ask_menu && w.ask_menu->frames_open > 5) {
                                std::vector<std::string> m2;
                                for (uint16_t sid : w.ask_menu->answers) m2.push_back(w.vm().string(sid));
                                int e = ask_exit_index(m2);
                                w.choose_answer(unsigned(e >= 0 ? e : int(m2.size()) - 1));
                            }
                        }
                        return talk;
                    }
                int pick = -1;
                static const char* kAvoid[] = {"need to know", "not your concern", "none of your business", "not wish to say", "Dungbreath",
                                               "won't do your dirty", "not be your spy", "not trust"};
                for (size_t i = 0; i < m.size() && pick < 0; ++i) {
                    bool bad = ask_exit_kind(m[i]) != 0 || chosen.count(m[i]);
                    for (const char* av : kAvoid) bad = bad || m[i].find(av) != std::string::npos;
                    if (!bad) pick = int(i);
                }
                if (pick < 0 || ++picks > 25) pick = ask_exit_index(m) >= 0 ? ask_exit_index(m) : int(m.size()) - 1;
                chosen.insert(m[size_t(pick)]);
                w.choose_answer(unsigned(pick));
            }
            if (!w.ask_menu && !w.in_conversation() && w.barks.empty() && f > 30) break;
        }
        u.frames(20);
    }
    return 0;
}

void test_int_dialog(const GameData& d) {
    std::printf("int: INT-gated topics of the original NPCs\n");
    std::vector<std::pair<std::string, const DialogTopic*>> gated;
    Ui u(d, false);
    u.frames(10);
    NpcDialog& nd = *u.w().dialog;
    std::vector<std::string> pr = nd.problems();
    for (const std::string& s : pr) std::printf("  problem: %s\n", s.c_str());
    CHECK(pr.empty());
    std::set<unsigned> npcs;
    for (const NpcDialogDef& def : nd.defs)
        for (const DialogTopic& t : def.topics)
            if (t.when.avatar_int > 0) {
                gated.push_back({def.id, &t});
                CHECK(npcs.insert(def.npc).second);  // one each
                CHECK(t.when.avatar_int >= 18 && t.when.avatar_int <= 25);
                CHECK(t.feature.empty());  // behind New Dialogue
                CHECK(t.slot == "mind");
            }
    std::printf("  %zu gated topics for %zu NPCs\n", gated.size(), npcs.size());
    CHECK(gated.size() >= 8 && gated.size() <= 15);
    // The JSON, for the story state each topic names (its globals and steps).
    for (const auto& gt : gated) {
        const std::string& fid = gt.first;
        const DialogTopic& t = *gt.second;
        nlohmann::json j = nlohmann::json::parse(slurp(g_data + "/dialog/" + fid + ".json"));
        nlohmann::json when;
        for (const auto& x : j["topics"]) if (x["id"] == t.id) when = x["when"];
        Ui s(d, false);
        s.frames(10);
        NpcDialog& sd = *s.w().dialog;
        int def = -1;
        for (size_t k = 0; k < sd.defs.size(); ++k) if (sd.defs[k].id == fid) def = int(k);
        CHECK(def >= 0);
        if (def < 0) continue;
        const unsigned npc = sd.defs[size_t(def)].npc;
        for (const auto& c : when.value("cond", nlohmann::json::array())) {
            if (!c.contains("global")) continue;
            unsigned pos = c["global"][0], n = c["global"][1];
            unsigned v = c.contains("eq") ? unsigned(c["eq"]) : 0;
            if (c.contains("ne") && unsigned(c["ne"]) == 0) v = 1;
            s.w().vm().set_global(pos, n, uint16_t(v));
        }
        // Darion leaves the execution site for town once his schedule runs: go there first.
        if (npc == 5) {
            s.w().teleport_actor(1, 15583, 13700, 48, 41);
            s.frames(30);
        }
        // Steps the topic needs: a visit to the map stands in for the arrival steps (to_daemons_crag).
        if (const Actor* g = s.w().actor(uint16_t(npc))) {
            s.w().visited_maps.insert(g->mapnum);
            s.w().teleport_actor(1, g->x + 96, g->y, g->z, g->mapnum);
            s.frames(30);
        }
        auto shown = [&]() {
            for (const DialogTopic* e : sd.entries(def)) if (e->id == t.id) return true;
            return false;
        };
        Actor* av = s.w().avatar();
        av->intel = int16_t(t.when.avatar_int - 1);
        const bool below = shown();
        av->intel = int16_t(t.when.avatar_int);
        const bool at = shown();
        CHECK(!below);
        CHECK(at);
        // A real conversation: the entry in the NPC's own menu, and its lines.
        std::vector<std::string> seen;
        int talk = at ? find_entry(s, npc, t.label, &seen) : 0;
        std::printf("  %-9s npc %2u INT %d: below %s, at %s, in the menu: %s\n", fid.c_str(), npc, t.when.avatar_int, below ? "SHOWN" : "hidden",
                    at ? "shown" : "HIDDEN", talk ? ("talk " + std::to_string(talk)).c_str() : "NOT FOUND");
        if (!talk) for (const std::string& m : seen) std::printf("      menu: %s\n", m.c_str());
        CHECK(talk > 0);
        CHECK(s.has_log(t.say.front().substr(0, 40)));
        CHECK(s.has_log(t.say.back().substr(0, 40)));
        CHECK(!s.w().in_conversation());
        // New Dialogue off: gone.
        sd.enabled = false;
        CHECK(!shown());
        sd.enabled = true;
    }
}

// --- How the points were chosen: the walking routes of the travel maps, sampled.
struct Leg { unsigned map; int x0, y0, z0, x1, y1, z1; const char* what; };

void cands(const GameData& d) {
    const Leg legs[] = {
        {3, 14431, 5727, 8, 11532, 1980, 48, "camp to the gate"},
        {40, 13038, 15042, 48, 13054, 2322, 48, "Central Tenebrae, south gate to north gate"},
        {40, 13070, 2618, 48, 10506, 14070, 48, "Central Tenebrae, north to south-west"},
        {39, 19130, 14070, 48, 8888, 11000, 48, "West Tenebrae to the reef"},
        {39, 19116, 3316, 48, 9272, 11016, 48, "West Tenebrae, north to the reef gate"},
        {4, 15464, 6912, 48, 22220, 21640, 8, "the reef to the far shore"},
        {41, 12828, 14092, 48, 23103, 11007, 48, "East Tenebrae to the East Road"},
        {41, 12767, 3327, 48, 23103, 11007, 48, "East Tenebrae, north to the East Road"},
        {5, 5040, 22784, 48, 7423, 10943, 48, "East Road to the cemetery"},
        {6, 5375, 29823, 8, 7360, 27120, 8, "cemetery: the gate to the old plot"},
        {6, 7360, 27120, 8, 8223, 24287, 64, "cemetery: the old plot to the tomb"},
        {7, 15071, 26239, 8, 9498, 19702, 8, "the valley to the plateau cave"},
        {7, 15071, 26239, 8, 6918, 23418, 8, "the valley to Brannoc's ground"},
        {8, 11000, 14500, 48, 13988, 15084, 48, "plateau: the pond to Mythran's"},
        {37, 26776, 26912, 56, 25055, 19903, 80, "Argentrock: the shore to the waypoint"},
        {37, 25055, 19903, 80, 18438, 18474, 72, "Argentrock: waypoint to the altar"},
        {12, 21279, 15647, 72, 17382, 19549, 56, "Crag: waypoint to the ceremony"},
        {12, 21279, 15647, 72, 21340, 17000, 56, "Crag: waypoint to the east bluff"},
        {16, 19263, 9631, 104, 17764, 18124, 104, "Carthax: the north to the breaking ground"},
        {21, 3359, 2079, 8, 5374, 1890, 0, "Stone Cove"},
    };
    Ui u(d, false, false, false);
    u.frames(10);
    for (const Leg& l : legs) {
        u.warp(l.map, l.x0, l.y0, l.z0);
        u.frames(10);
        RouteGuide rg(u.w());
        rg.set_target(l.map, l.x1, l.y1, l.z1);
        const Route& r = rg.plan_now(4.0);
        std::printf("== map %u %s: route status %d (%s), %zu points, length %d; from %d,%d,%d\n", l.map, l.what, int(r.status), r.reason.c_str(),
                    r.pts.size(), r.length, u.av()->x, u.av()->y, u.av()->z);
        if (!r.usable()) continue;
        int acc = 0;
        for (size_t i = 1; i < r.pts.size(); ++i) {
            const RoutePoint& a = r.pts[i - 1];
            const RoutePoint& b = r.pts[i];
            acc += int(std::hypot(double(b.x - a.x), double(b.y - a.y)));
            if (acc < 400) continue;
            acc = 0;
            Near n = look_round(u.w(), b.x, b.y);
            const bool stand = standable(u.w(), b.x, b.y, b.z);
            std::printf("  %5d,%5d,%3d monster %5d hatch %5d stand %d1%s\n", b.x, b.y, b.z, n.monster == 1 << 30 ? -1 : n.monster,
                        n.hatch == 1 << 30 ? -1 : n.hatch, stand, n.monster >= 768 && n.hatch >= 512 ? "  QUIET" : "");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_data = argc > 2 ? argv[2] : "data";
    g_saves = argc > 3 ? argv[3] : "/tmp/pagan_banter_saves";
    mkdir(g_saves.c_str(), 0755);
    Config::global().load(g_saves + "/pagan_test.cfg");
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    d.load_custom_shapes(g_data + "/shapes");
    std::string only = argc > 4 ? argv[4] : "";
    if (only == "cands") { cands(d); return 0; }
    auto want = [&](const char* n) { return only.empty() || only == n; };
    if (want("data")) test_data(d);
    if (want("fire")) { test_fire(d, false); test_fire(d, true); }
    if (want("recharge")) test_recharge(d);
    if (want("once")) test_once_and_repeat(d);
    if (want("fair")) test_fair(d);
    if (want("calm")) test_calm(d);
    if (want("saves")) test_saves(d);
    if (want("int")) test_int_dialog(d);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
