// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The critical path through the game (specs/data/critpath.json, specs/data/map_links.json,
// specs/world/critical_path.md): story steps with completion conditions and targets, and the
// links between maps. The data is evaluated against a CritContext (the live world, or a fake
// one in the tests) to find the current step and where the Avatar should go next.
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/route_guide.h"

namespace pg {

class World;

// What the conditions ask about the game state.
class CritContext {
public:
    virtual ~CritContext() = default;
    virtual int global(unsigned pos, unsigned n) const = 0;
    // Count of items of `shape` (and `frame` / `quality` if >= 0) the Avatar carries; stacks count
    // their quantity.
    virtual int item_count(unsigned shape, int frame, int quality = -1) const = 0;
    virtual bool equipped(unsigned shape) const = 0;
    virtual unsigned current_map() const = 0;
    virtual bool visited_map(unsigned map) const = 0;
    virtual bool npc_dead(unsigned npc) const = 0;
    // 1: gone, 0: still there, -1: not known (the map is not loaded and was never seen).
    virtual int item_gone(unsigned shape, unsigned map, int x, int y, int z) const = 0;
    virtual bool spell_known(unsigned n) const = 0;
    virtual bool npc_location(unsigned npc, unsigned& map, int& x, int& y, int& z) const = 0;
    virtual bool avatar_location(unsigned& map, int& x, int& y, int& z) const = 0;
};

// A condition tree: all / any / not, or one test.
struct CritCond {
    enum Kind {
        kNone,  // missing: false
        kTrue, kFalse, kAll, kAny, kNot, kGlobal, kHasItem, kEquipped, kOnMap, kVisitedMap, kNpcDead, kItemGone,
        kSpellKnown, kUnknown,
    } kind = kNone;
    std::vector<CritCond> sub;
    int a = 0, b = 0;       // global pos / width; shape; map; npc; spell
    int frame = -1, qty = 1, quality = -1;
    enum Cmp { kNonZero, kEq, kNe, kGe } cmp = kNonZero;
    int value = 0;
    int map = 0, x = 0, y = 0, z = 0;  // item_gone
    std::string text;       // unknown conditions: the JSON (for logs)
    bool eval(const CritContext& c) const;
    std::string describe() const;
};

// Parses one condition from its JSON text (the done_when syntax); kUnknown on errors.
CritCond crit_cond_from_text(const std::string& text);

struct CritTarget {
    enum Type { kNone, kNpc, kPoint, kItem } type = kNone;
    int npc = 0, shape = 0;
    int map = -1, x = 0, y = 0, z = 0;
    // Waypoints on the way (another map's exit toward the target), used when the Avatar is on
    // that map: {map, x, y, z, note}.
    struct Via { int map = -1, x = 0, y = 0, z = 0; std::string note; };
    std::vector<Via> via;
};

struct CritStep {
    std::string id, chain, act, title, hint, evidence, confidence;
    bool milestone = true, has_milestone = false, optional = false;
    CritCond done_when;
    CritTarget target;
    // Alternative targets: the first whose condition holds replaces `target`.
    std::vector<std::pair<CritCond, CritTarget>> alt_targets;
};

struct MapLink {
    unsigned from_map = 0, to_map = 0;
    bool from_any = false;   // from_map -1: usable from any map (an item or a spell)
    bool has_point = true;   // x / y / z given (null = no fixed source point)
    bool dead = false;       // a link that does not work (never routed through)
    int x = 0, y = 0, z = 0, to_x = 0, to_y = 0, to_z = 0;
    int rx = -1, ry = -1;    // trigger range in 32-unit steps (trigger_range; -1: the egg's own)
    std::string kind, note;
    CritCond condition;
    bool has_condition = false;
};

// The current step and how to get there.
struct CritGuide {
    bool valid = false;        // there is a current step
    int step = -1;
    bool all_done = false;
    bool have_point = false;   // (x, y, z) on the current map to point at
    int x = 0, y = 0, z = 0;
    unsigned target_map = 0;
    int link = -1;             // the link used for the first hop (index into links), -1 none
    std::vector<unsigned> route;  // maps from the current one to the target's
    bool unreachable = false;  // the target's map cannot be reached through the known links
    std::string line1, line2;  // "Next: ...", "Go to ... via ..."
};

// Map names (specs/world/map_index.md 1).
std::string map_name(unsigned map);

class CritPath {
public:
    // Loads steps and / or links from a JSON text ("steps": [...], "links": [...]; a top-level
    // array is taken as links). Returns false with `err` on a parse error.
    bool load_text(const std::string& text, std::string& err);
    bool load_file(const std::string& path, std::string& err);
    void clear() { steps.clear(); links.clear(); }
    bool empty() const { return steps.empty(); }

    std::vector<CritStep> steps;
    std::vector<MapLink> links;
    std::vector<std::string> sources;  // files loaded

    // Story threads: chain -> the chains it comes after ("chains" in critpath.json).
    std::map<std::string, std::vector<std::string>> chain_after;
    // Each step's done_when (debug overrides applied).
    std::vector<bool> done(const CritContext& c) const;
    // specs/world/critical_path.md 2: a step is complete if it is done, or a later milestone of
    // its chain is done, or a milestone of a chain that (transitively) comes after its chain is.
    std::vector<bool> complete(const std::vector<bool>& done) const;
    // The current step: the first one not complete, in file order (-1 = all complete).
    int current(const CritContext& c) const;
    int current(const std::vector<bool>& done) const;
    // True if chain `later` comes after chain `earlier`, directly or through other chains.
    bool chain_follows(const std::string& later, const std::string& earlier) const;
    // The data marks milestones (then only milestones count as major steps for autosaves).
    bool has_milestones() const;
    CritGuide guide(const CritContext& c) const;
    // Maps from `from` to `to` through links whose conditions hold (BFS); empty if none.
    std::vector<unsigned> route(const CritContext& c, unsigned from, unsigned to) const;
    int step_index(const std::string& id) const;
    // The usable links from `map` back into `map` (teleporters inside one map), as jumps for
    // the walking route (game/route_guide.h): the guide can then lead through them.
    std::vector<RouteLink> walk_links(const CritContext& c, unsigned map) const;

    // Debug: steps marked done by hand, and a jump (every step before it counts as done).
    std::set<int> forced_done;
    int jump_to = -1;

private:
    bool target_location(const CritContext& c, const CritTarget& t, unsigned& map, int& x, int& y, int& z) const;
    bool link_usable(const CritContext& c, const MapLink& l) const;
};

// The live world as a CritContext, with the record of visited maps and of items seen gone.
class WorldCritContext : public CritContext {
public:
    explicit WorldCritContext(World& w) : w_(w) {}
    // Called every evaluation: remembers the current map as visited.
    void observe();
    void reset() { gone_seen_.clear(); }
    int global(unsigned pos, unsigned n) const override;
    int item_count(unsigned shape, int frame, int quality = -1) const override;
    bool equipped(unsigned shape) const override;
    unsigned current_map() const override;
    bool visited_map(unsigned map) const override;
    bool npc_dead(unsigned npc) const override;
    int item_gone(unsigned shape, unsigned map, int x, int y, int z) const override;
    bool spell_known(unsigned n) const override;
    bool npc_location(unsigned npc, unsigned& map, int& x, int& y, int& z) const override;
    bool avatar_location(unsigned& map, int& x, int& y, int& z) const override;

private:
    World& w_;
    mutable std::map<std::string, int> gone_seen_;
};

}  // namespace pg
