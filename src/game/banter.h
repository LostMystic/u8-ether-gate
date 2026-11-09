// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Travel banter between two companions (PLAN-LEVELING.md 8; the engine's own writing, not from the
// original game). Short back-and-forth exchanges, spoken as barks over the speakers' heads (and so
// into the message log), at banter points: quiet places on the roads of the critical path.
//
// Data: engine/data/banter.json (the exchanges, by pair) and engine/data/banter_points.json (the
// points). Rules (NOTES.md "Travel banter"):
// - a point fires when the Avatar comes within its radius, it has recharged (kRecharge frames of
//   play time since it last fired), no fight, conversation or scene is on, and two companions of a
//   pair are with him (following, alive, on his map, near);
// - each real exchange plays once per save; the pair with the fewest exchanges heard so far goes
//   first (ties at random), its exchanges in file order;
// - once every present pair has heard all of its real exchanges, a point that fires plays a pair's
//   "repeat" exchange (the running-out-of-things-to-say joke) only one time in three;
// - an exchange cut short (a fight, a conversation, a speaker gone) is not counted, so it can come
//   again in full.
// Part of the companions (no option of its own): nothing happens while Party::enabled is off.
// Saved as an optional section BNTR at the end of the save (only when there is something to keep).
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pg {

class World;
class Party;
class SaveWriter;
class SaveReader;

struct BanterLine {
    std::string who;   // companion id
    std::string text;
};

struct BanterExchange {
    std::string id;
    std::vector<BanterLine> lines;
    std::vector<std::string> step_done, step_not_done;  // optional: critical-path steps
    bool repeat = false;
};

struct BanterPair {
    std::string a, b;   // companion ids
    std::string mood;   // friendly / wary / prickly / teasing (for the review document)
    std::vector<BanterExchange> exchanges;
    BanterExchange repeat;  // empty lines: none
};

struct BanterPoint {
    std::string id, place;
    unsigned map = 0;
    int x = 0, y = 0, z = 0, r = 192;
};

class Banter {
public:
    Banter(World& w, Party& p);

    static constexpr uint32_t kRecharge = 5 * 60 * 30;  // 5 minutes of play time, in game frames
    static constexpr int kNear = 640;                   // companions this close to the Avatar take part
    static constexpr int kCheckEvery = 10;              // frames between point checks

    // --- Data
    bool load_text(const std::string& exchanges, const std::string& points, std::string& err);
    bool load_dir(const std::string& dir, std::string& err);  // banter.json + banter_points.json
    std::vector<BanterPair> pairs;
    std::vector<BanterPoint> points;
    bool loaded() const { return !pairs.empty() && !points.empty(); }
    // Unknown companions, pairs listed twice, lines by someone outside the pair, empty lines (tests).
    std::vector<std::string> problems() const;

    // --- State (saved)
    std::set<std::string> played;               // real exchanges heard in full
    std::map<std::string, uint32_t> ready_at;   // point id -> the frame it may fire again

    void reset();   // new game / load: nothing heard, every point ready
    void frame();   // Party::frame
    bool playing() const { return cur_ != nullptr; }
    const BanterExchange* current() const { return cur_; }
    // A point fires now if it can (tests): -1 = not found.
    int point_index(const std::string& id) const;

    void save(SaveWriter& o) const;
    bool load(SaveReader& in);  // the BNTR section (the caller has peeked its tag)
    bool has_state() const { return !played.empty() || !ready_at.empty(); }

    std::vector<std::string> log;  // what happened (tests)
    std::string why_not;           // why the last point reached did not fire (tests)

private:
    World& w_;
    Party& p_;
    const BanterExchange* cur_ = nullptr;
    int cur_pair_ = -1;
    size_t line_ = 0;
    uint16_t last_speaker_ = 0;
    uint32_t next_line_ = 0, next_check_ = 0;
    bool calm() const;
    std::vector<int> present() const;           // def indices of companions who can take part now
    bool exchange_ok(const BanterExchange& e) const;
    int unplayed(const BanterPair& p) const;    // real exchanges left whose conditions hold
    int heard(const BanterPair& p) const;
    void try_point(int pt);
    void start(int pair, const BanterExchange& e);
    void advance();
    void stop(const char* why);
    bool speaker_here(const std::string& id) const;
};

}  // namespace pg
