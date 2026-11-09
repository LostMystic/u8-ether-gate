// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Ledge harness (PLAN-M6.md "Ledges", easy ledges): finds real ledges in the game's maps from
// their geometry (Devon's camp, Tenebrae, the Plateau, the catacombs), puts the Avatar at
// many distances / facings / sideways offsets in front of them, holds jump, and measures how
// often he gets up, and why he does not. Ledges that cannot be reached (too high, no room
// on top, nothing there) must never be climbed, and he must never end inside something or
// unsupported.
//
//   pagan_ledge_tests GAME_DIR [SHOT_DIR] [only]
//   only: survey (real maps), synthetic, pad (controller, hint, option, screenshots).
//   PAGAN_LEDGE_CLASSIC=1: the survey with Easy Ledges off (the earlier hold-jump behaviour),
//   for a before / after comparison. PAGAN_LEDGE_VERBOSE=1: every site and miss.
//   PAGAN_LEDGE_SITE=map,x,y,z,dir[,back] (PAGAN_LEDGE_FROM=x,y,z): one spot traced.
// A ledge counts as reachable when a standable top (a ledge surface: no roof, wall top, water)
// lies 16-72 (ledge::kMaxRise) above within 96 ahead of the face, with headroom above him and a way over the
// edge at the top's height; unreachable when the face rises 136+ or the top is no ledge.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/assist.h"
#include "game/ledge.h"
#include "game/processes.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"
#include "platform/platform.h"

int g_fail = 0, g_pass = 0;

using namespace pg;

namespace {

std::string g_shots;

enum : unsigned { kStand = 2, kJumpUp = 3, kHang = 45 };

std::vector<uint8_t> snap(const World& w) {
    SaveWriter out;
    w.save_game(out, "ledge");
    return out.data();
}

// A world past the opening, saved once and loaded for each map.
struct Base {
    const GameData& d;
    std::vector<uint8_t> save;
    explicit Base(const GameData& data) : d(data) {
        World w(d);
        w.auto_ui = true;
        w.new_game();
        for (int f = 0; f < 8400; ++f) { w.input.now_ms = uint32_t(f * 1000 / 30); w.frame(); }
        save = snap(w);
    }
};

struct Env {
    World w;
    int frames = 0;
    int xd = 0, yd = 0, zd = 0;
    Env(const Base& b, bool easy, bool responsive = true) : w(b.d) {
        w.auto_ui = true;
        w.new_game();
        SaveReader in(b.save.data(), b.save.size());
        w.load_game(in);
        w.controls.responsive = responsive;
        w.controls.assist = responsive;
        w.controls.easy_ledges = easy;
        // The fast area (where glob eggs are expanded: the houses' walls and roofs) covers the
        // whole surveyed region, as a zoomed-out view does.
        w.view_w = w.view_h = 1600;
        frames = int(w.kernel().frame_number());
        w.footprint(*w.avatar(), xd, yd, zd);
        run(2);
    }
    void step() {
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    void run(int n) { for (int i = 0; i < n; ++i) step(); }
    Actor& av() { return *w.avatar(); }
    uint16_t flags() { return w.shape_flags(av()); }
    bool valid(int x, int y, int z) { return w.can_exist(x, y, z, xd, yd, zd, flags(), 1).valid; }
    bool standable(int x, int y, int z) {
        World::Placement p = w.can_exist(x, y, z, xd, yd, zd, flags(), 1);
        return p.valid && p.supported && !assist::dangerous(w, av(), x, y, z);
    }
    void clear_input() {
        PlayerInput& in = w.input;
        in.key_jump = in.jump_held = in.pad_jump = in.pad_move = false;
        in.key_forward = in.key_back = in.key_step = in.key_run = false;
        in.button[0].down = in.button[2].down = false;
        in.button[0].handled = in.button[2].handled = true;
        in.jump_buffer_ms = 0;
        in.no_grab_until_ms = 0;
    }
    // Puts him at a spot, standing, nothing running.
    bool place(int x, int y, int z, int dir) {
        clear_input();
        w.kernel().kill(1, kProcActorAnim);
        w.kernel().kill(1, kProcGravity);
        run(1);
        Actor& a = av();
        a.flags &= uint16_t(~kItemHanging);
        a.set(Actor::kAnimLock, false);
        a.last_anim = kStand;
        a.dir = uint8_t(dir & 7);
        unsigned m = w.map_number_now();
        w.move_to(1, x, y, z, false);
        run(2);
        return av().x == x && av().y == y && av().z == z && !w.stasis && w.map_number_now() == m;
    }
};

// --- Ledges found from the geometry ----------------------------------------------------------

// A roof piece (ROOF) within 64 of the spot on a top (beside it or up to 96 above): the top
// belongs to a building (an eave, a wall under the roof), not a ledge.
bool building(Env& e, int x, int y, int top) {
    const CurrentMap& m = e.w.map();
    for (int cy = std::max(0, (y - 192) / 512); cy <= std::min(CurrentMap::kChunks - 1, (y + 64) / 512); ++cy)
        for (int cx = std::max(0, (x - 192) / 512); cx <= std::min(CurrentMap::kChunks - 1, (x + 64) / 512); ++cx)
            for (uint16_t id : m.chunk(cx, cy)) {
                const Item* o = e.w.item(id);
                if (!o || !(e.w.data().typeflags.get(o->shape).flags() & ShapeInfo::kRoof)) continue;
                int oxd, oyd, ozd;
                e.w.footprint(*o, oxd, oyd, ozd);
                if (o->x <= x - e.xd - 64 || o->x - oxd >= x + 64 || o->y <= y - e.yd - 64 || o->y - oyd >= y + 64) continue;
                if (o->z + ozd < top - 16 || o->z > top + 96) continue;
                return true;
            }
    return false;
}

struct Site {
    unsigned map = 0;
    int fx = 0, fy = 0, z = 0;  // flush against the face, standing
    int dir = 0;
    int h = 0;                  // the top above his feet (0: none in reach)
    bool reachable = false;
    bool roof = false;          // the top is not a ledge surface (a roof, water, a wall's top)
    unsigned top_shape = 0;
    bool covered = false;
};

int bucket(int h) { return h <= 40 ? 0 : h <= 72 ? 1 : 2; }
const char* bucket_name(int b) { return b == 0 ? "16-40" : b == 1 ? "48-72" : "80-120"; }

// From a standing spot, walks the box forward (4-unit steps, same height, supported) until it
// is blocked. Then looks for a standable top beyond the face (a surface up to 200 higher with
// room for him on it) within 96 units.
bool probe_site(Env& e, int x, int y, int z, int dir, Site& s) {
    int dx = dir_x(dir), dy = dir_y(dir);
    int last = -1;
    for (int t = 0; t <= 160; t += 4) {
        int px = x + t * dx, py = y + t * dy;
        World::Placement p = e.w.can_exist(px, py, z, e.xd, e.yd, e.zd, e.flags(), 1);
        if (!p.valid) break;
        if (!p.supported || p.support_z != z) return false;  // a drop or a step on the way
        last = t;
    }
    if (last < 0 || last >= 160) return false;
    s.fx = x + last * dx;
    s.fy = y + last * dy;
    s.z = z;
    s.dir = dir;
    s.h = 0;
    int qa = 0;
    for (int ahead = 8; ahead <= 96 && !s.h; ahead += 4) {
        int qx = s.fx + ahead * dx, qy = s.fy + ahead * dy;
        int top;
        uint16_t f, id;
        if (!assist::surface_under(e.w, qx - e.xd / 2, qy - e.yd / 2, z + 200, 1, top, f, id)) continue;
        if (top - z < 16) continue;
        if (e.standable(qx, qy, top)) {
            s.h = top - z; qa = ahead;
            s.roof = !ledge::ledge_surface(e.w, id) || !ledge::ledge_spot(e.w, e.av(), qx, qy, top) || building(e, qx, qy, top);
            const Item* it = e.w.item(id);
            s.top_shape = it ? it->shape : 0;
            // Anything (a roof, an eave) right over the spot on the top?
            int rt;
            uint16_t rf, rid;
            s.covered = assist::surface_under(e.w, qx - e.xd / 2, qy - e.yd / 2, top + 64, 1, rt, rf, rid) && rt > top + e.zd;
        }
    }
    if (s.h >= 16 && s.h <= ledge::kMaxRise) {
        // Headroom above him up to the top (he has to rise that far to get over).
        for (int zz = z; zz <= z + s.h; zz += 8)
            if (!e.valid(s.fx, s.fy, zz)) return false;  // a ceiling: ambiguous, not used
        // And a way over the edge at the top's height to the spot on it. Without one (a wall
        // or parapet on the edge, a wall in front of a floor behind) no climb can get there:
        // an unreachable case.
        bool over = true;
        for (int t = 4; t <= qa && over; t += 4) over = e.valid(s.fx + t * dx, s.fy + t * dy, z + s.h);
        // (A roof, water, a damaging top or a wall's top is not a ledge: unreachable on purpose.)
        s.reachable = over && !s.roof;
        return true;
    }
    if (s.h == 0 || s.h > 136) {
        // Unreachable: the face goes up at least 136 right in front of him.
        for (int zz = z; zz <= z + 136; zz += 8)
            if (e.valid(s.fx + 8 * dx, s.fy + 8 * dy, zz)) return false;
        s.reachable = false;
        return true;
    }
    return false;
}

std::vector<Site> find_sites(Env& e, unsigned map, int cx, int cy, int cz, int radius, int per_bucket) {
    std::vector<Site> all;
    for (int gy = cy - radius; gy <= cy + radius; gy += 48)
        for (int gx = cx - radius; gx <= cx + radius; gx += 48) {
            int top;
            uint16_t f, id;
            if (!assist::surface_under(e.w, gx - e.xd / 2, gy - e.yd / 2, cz + 120, 1, top, f, id)) continue;
            if (top < cz - 160 || !e.standable(gx, gy, top)) continue;
            for (int dir = 0; dir < 8; dir += 1) {
                Site s;
                s.map = map;
                if (!probe_site(e, gx, gy, top, dir, s)) continue;
                bool near = false;
                for (const Site& o : all)
                    if (std::abs(o.fx - s.fx) + std::abs(o.fy - s.fy) < 200 && o.reachable == s.reachable) near = true;
                if (!near) all.push_back(s);
            }
        }
    // Spread over the height buckets (and some unreachable ones).
    std::vector<Site> out;
    int count[4] = {};
    for (const Site& s : all) {
        int b = s.reachable ? bucket(s.h) : 3;
        if (count[b] < (b == 3 ? 2 : per_bucket)) { ++count[b]; out.push_back(s); }
    }
    return out;
}

// --- One attempt -------------------------------------------------------------------------------

struct Attempt {
    bool climbed = false, hung = false, jumped = false, bad_end = false;
    bool reached = false;  // stood on something >= 16 higher at some point
    bool teleported = false;  // an egg took him to another map (not counted)
    int rise = 0, reached_rise = 0;
    int frames = 0;
};

enum HoldKind { kHoldKey, kHoldPad, kHoldMouse };

// Holds jump (from the press on) for up to `hold` frames with no movement input, then lets go
// and lets him settle.
Attempt attempt(Env& e, int z0, HoldKind kind, int hold = 90) {
    Attempt r;
    PlayerInput& in = e.w.input;
    unsigned map0 = e.w.map_number_now();
    in.jump_held = true;
    in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
    if (kind == kHoldKey) in.key_jump = true;
    if (kind == kHoldPad) in.pad_jump = true;
    if (kind == kHoldMouse) {
        in.cursor_dir = e.av().dir;
        in.cursor_len = 1;
        in.button[0].press(in.now_ms, 0, 0);
        in.button[2].press(in.now_ms, 0, 0);
    }
    int up_frames = 0;
    for (int f = 0; f < hold; ++f) {
        e.step();
        unsigned la = e.av().last_anim;
        if (la == kHang) r.hung = true;
        if (la == 3 || la == 17 || la == 10) r.jumped = true;
        if (std::getenv("PAGAN_LEDGE_TRACE"))
            std::printf("      f%d anim %u dir %d at %d,%d,%d\n", f, la, e.av().dir, e.av().x, e.av().y, e.av().z);
        bool busy = e.w.kernel().count_exact(1, kProcActorAnim) || (e.av().gravity_pid && e.w.kernel().get(e.av().gravity_pid));
        // Got up: standing on something at least 16 higher (counted even if a held key then
        // hops him on and off it again).
        if (e.av().z - z0 >= 16 && !(e.av().flags & kItemHanging) && la != kHang) {
            World::Placement p = e.w.can_exist(e.av().x, e.av().y, e.av().z, e.xd, e.yd, e.zd, e.flags(), 1);
            if (p.valid && (p.supported || (p.support_z > -1000 && e.av().z - p.support_z <= 8))) { r.reached = true; r.reached_rise = std::max(r.reached_rise, e.av().z - z0); }
        }
        if (!busy && e.av().z - z0 >= 16) { if (++up_frames > 6) break; }
        else up_frames = 0;
        // the pad's "held" is the X button; its press flag is consumed by the controller
    }
    e.clear_input();
    int still = 0, lx = -1, ly = -1, lz = -1;
    for (int f = 0; f < 240; ++f) {
        bool busy = e.w.kernel().count_exact(1, kProcActorAnim) || (e.av().gravity_pid && e.w.kernel().get(e.av().gravity_pid)) ||
                    (e.av().flags & kItemHanging);
        still = (e.av().x == lx && e.av().y == ly && e.av().z == lz) ? still + 1 : 0;
        lx = e.av().x; ly = e.av().y; lz = e.av().z;
        if (!busy && still >= 4) break;
        if (std::getenv("PAGAN_LEDGE_TRACE"))
            std::printf("      settle f%d anim %u at %d,%d,%d flags %x\n", f, e.av().last_anim, e.av().x, e.av().y, e.av().z, e.av().flags);
        e.step();
    }
    e.run(4);
    Actor& a = e.av();
    r.teleported = e.w.map_number_now() != map0;
    r.rise = a.z - z0;
    r.climbed = r.rise >= 16 || r.reached;
    World::Placement p = e.w.can_exist(a.x, a.y, a.z, e.xd, e.yd, e.zd, e.flags(), 1);
    // (The engine lets a climb end up to 8 above a surface: avatar.md 5.)
    r.bad_end = !p.valid || !(p.supported || a.z <= 0 || (p.support_z > -1000 && a.z - p.support_z <= 8));
    if (r.bad_end && std::getenv("PAGAN_LEDGE_VERBOSE")) {
        const Item* bl = e.w.item(p.blocker);
        std::printf("      bad end at %d,%d,%d: valid %d supported %d (support z %d) blocker %u shape %d, anim %u flags %x\n", a.x, a.y,
                    a.z, p.valid, p.supported, p.support_z, p.blocker, bl ? int(bl->shape) : -1, a.last_anim, a.flags);
    }
    return r;
}

struct Tally {
    int tried = 0, ok = 0, bad = 0;
    void add(bool success, bool bad_end) { ++tried; ok += success; bad += bad_end; }
    int pct() const { return tried ? (100 * ok + tried / 2) / tried : 0; }
};

struct Survey {
    Tally all, unreach, by_back[6], by_face[3], by_bucket[3], by_map[16];
    int unreach_bad = 0, why_nothing = 0, why_hop = 0, why_hang = 0, why_bad = 0;
    int legit_other = 0;
};

const int kBacks[] = {0, 16, 32, 64, 96};
const int kFaces[] = {0, 1, 2};  // facing off by 0, 45, 90 degrees

void run_sites(Env& e, const std::vector<Site>& sites, int map_index, Survey& sv, bool verbose, const int home[3]) {
    for (const Site& s : sites) {
        int dx = dir_x(s.dir), dy = dir_y(s.dir);
        int px = -dy, py = dx;  // sideways
        for (int bi = 0; bi < 5; ++bi)
            for (int fi = 0; fi < 3; ++fi)
                for (int lat : {0, 24}) {
                    if (lat && (bi > 1 || fi > 0)) continue;
                    int back = kBacks[bi];
                    int sx = s.fx - back * dx + lat * px, sy = s.fy - back * dy + lat * py;
                    if (e.w.map_number_now() != s.map) {  // an egg took him away: back to the map
                        e.w.teleport_actor(1, home[0], home[1], home[2], s.map);
                        e.run(12);
                    }
                    if (!e.standable(sx, sy, s.z)) continue;
                    int face = (s.dir + kFaces[fi] * ((bi + lat) % 2 ? -1 : 1)) & 7;
                    if (!e.place(sx, sy, s.z, face)) continue;
                    Attempt a = attempt(e, s.z, kHoldKey);
                    if (a.teleported) continue;
                    if (s.reachable) {
                        bool ok = a.climbed && !a.bad_end;
                        sv.all.add(ok, a.bad_end);
                        sv.by_back[bi].add(ok, a.bad_end);
                        sv.by_face[fi].add(ok, a.bad_end);
                        sv.by_bucket[bucket(s.h)].add(ok, a.bad_end);
                        sv.by_map[map_index].add(ok, a.bad_end);
                        if (!ok) {
                            if (a.bad_end) ++sv.why_bad;
                            else if (a.hung) ++sv.why_hang;
                            else if (a.jumped) ++sv.why_hop;
                            else ++sv.why_nothing;
                        }
                        if (verbose && !ok)
                            std::printf("      miss: map %u %d,%d,%d dir %d h %d back %d face %+d lat %d: %s (rise %d)\n", s.map, s.fx,
                                        s.fy, s.z, s.dir, s.h, back, kFaces[fi], lat,
                                        a.bad_end ? "bad end" : a.hung ? "hung" : a.jumped ? "jumped" : "nothing", a.rise);
                    } else {
                        // Unreachable straight ahead. Climbing some other ledge nearby (the
                        // all-round search) is fine if it is a real, standable spot in reach.
                        bool up = a.climbed && !a.bad_end;
                        if (up && a.rise <= ledge::kMaxRise) ++sv.legit_other;
                        bool wrong = a.bad_end || a.rise > ledge::kMaxRise;
                        sv.unreach.add(wrong, a.bad_end);
                        if (wrong) ++sv.unreach_bad;
                        if (wrong && verbose)
                            std::printf("      wrong: map %u %d,%d,%d dir %d h %d back %d face %+d lat %d (start %d,%d facing %d): rise %d\n",
                                        s.map, s.fx, s.fy, s.z, s.dir, s.h, back, kFaces[fi], lat, sx, sy, face, a.rise);
                    }
                }
    }
}

struct MapSpot { unsigned map; int x, y, z; const char* name; };

void test_survey(const Base& b, bool easy, bool verbose) {
    // The camp: where the opening leaves him (Devon's camp and the bank north of it).
    MapSpot spots[] = {
        {0, 0, 0, 0, "Devon's camp"},
        {40, 14700, 3298, 48, "Central Tenebrae"},
        {40, 13087, 8800, 104, "Tenebrae (north)"},
        {40, 14900, 14070, 48, "Tenebrae (south)"},
        {39, 16300, 12300, 48, "West Tenebrae"},
        {41, 22700, 11007, 48, "East Tenebrae"},
        {7, 9498, 20000, 8, "Herdsman's Valley"},
        {8, 2783, 1823, 56, "the Plateau"},
        {8, 31583, 19135, 0, "the Plateau (east)"},
        {50, 2948, 10612, 48, "Upper Catacombs"},
        {50, 21311, 10527, 0, "Upper Catacombs (east)"},
        {49, 27367, 19719, 48, "the Catacombs"},
    };
    const int kSpots = int(sizeof spots / sizeof spots[0]);
    Survey sv;
    int sites_total = 0, unreach_total = 0;
    for (int mi = 0; mi < kSpots; ++mi) {
        Env e(b, easy);
        MapSpot sp = spots[mi];
        if (sp.map == 0) { sp.map = e.w.map_number_now(); sp.x = e.av().x; sp.y = e.av().y; sp.z = e.av().z; }
        else {
            e.w.teleport_actor(1, sp.x, sp.y, sp.z, sp.map);
            e.run(12);
        }
        std::vector<Site> sites = find_sites(e, sp.map, sp.x, sp.y, sp.z, 720, 4);
        int reach = 0;
        for (const Site& s : sites) reach += s.reachable;
        sites_total += reach;
        unreach_total += int(sites.size()) - reach;
        std::printf("    %s (map %u): %d reachable ledges, %d unreachable faces:", sp.name, sp.map, reach, int(sites.size()) - reach);
        for (const Site& s : sites) std::printf(" %s%d", s.reachable ? "" : "!", s.reachable ? s.h : s.h);
        if (verbose)
            for (const Site& s : sites)
                std::printf("\n      site %d,%d,%d dir %d h %d top shape %u%s%s", s.fx, s.fy, s.z, s.dir, s.h, s.top_shape,
                            s.covered ? " (covered above)" : "", s.reachable ? "" : " unreachable");
        std::printf("\n");
        const int home[3] = {sp.x, sp.y, sp.z};
        run_sites(e, sites, mi, sv, verbose, home);
    }
    std::printf("  ledge survey (%s): %d reachable ledges on %d spots, %d attempts: %d%% got up (%d bad ends)\n",
                easy ? "Easy Ledges on" : "Easy Ledges off = before", sites_total, kSpots, sv.all.tried, sv.all.pct(), sv.all.bad);
    std::printf("    by distance back from the face:");
    for (int i = 0; i < 5; ++i) std::printf(" %d: %d%% (%d)", kBacks[i], sv.by_back[i].pct(), sv.by_back[i].tried);
    std::printf("\n    by facing off:");
    for (int i = 0; i < 3; ++i) std::printf(" %d deg: %d%% (%d)", 45 * kFaces[i], sv.by_face[i].pct(), sv.by_face[i].tried);
    std::printf("\n    by height:");
    for (int i = 0; i < 3; ++i) std::printf(" %s: %d%% (%d)", bucket_name(i), sv.by_bucket[i].pct(), sv.by_bucket[i].tried);
    std::printf("\n    by spot:");
    for (int i = 0; i < kSpots; ++i) std::printf(" %s %d%% (%d);", spots[i].name, sv.by_map[i].pct(), sv.by_map[i].tried);
    std::printf("\n    misses: nothing happened %d, jumped and came down %d, hung but not pulled up %d, bad end %d\n",
                sv.why_nothing, sv.why_hop, sv.why_hang, sv.why_bad);
    std::printf("    unreachable faces (%d): %d attempts, %d wrong (ended inside / unsupported / too high), %d climbed another "
                "ledge in reach instead\n",
                unreach_total, sv.unreach.tried, sv.unreach_bad, sv.legit_other);
    CHECK(sites_total >= 12);
    CHECK_EQ(sv.all.bad, 0);
    CHECK_EQ(sv.unreach_bad, 0);
    if (easy) {
        CHECK(sv.all.pct() >= 95);
        for (int i = 0; i < 2; ++i) CHECK(sv.by_bucket[i].pct() >= 90);
        CHECK_EQ(sv.by_bucket[2].tried, 0);  // nothing above kMaxRise counts as reachable any more
    }
}

// --- Synthetic ledges in the air above the camp ------------------------------------------------

uint16_t place_floor(World& w, unsigned shape, int cx, int cy, int z) {
    uint16_t p = w.create_item(shape, 0);
    int xd, yd, zd;
    w.footprint(*w.item(p), xd, yd, zd);
    w.move_to(p, cx + xd / 2, cy + yd / 2, z, false);
    return p;
}

enum Synth { kPillar160, kCeiling, kOpen, kLedge56, kLedge96, kLedge112, kFallPast };

// A course in the air 100 above the camp ground (nothing else near): a floor of four
// shape-70 tiles, a block (the ledge) on the third, facing west. Returns the floor top g and
// the Avatar's spot flush with the block's west face, `back` further west.
struct Course { int g, ax, ay; };
Course build_course(World& w, Synth c, int back) {
    const Actor& av = *w.avatar();
    int fz = av.z + 100;
    int fx = av.x - 900, fy = av.y - 900;
    for (int k = -1; k < 3; ++k) place_floor(w, 70, fx + 128 * k, fy, fz);
    int g = fz + 8;
    auto put = [&](unsigned shape, int z) { place_floor(w, shape, fx + 128 + 8, fy, z); };
    switch (c) {
    case kPillar160: for (int k = 0; k < 4; ++k) put(193, g + 40 * k); break;  // 40 x 4: too high
    case kCeiling:
        put(740, g);                       // a 56 ledge with no room on top (24 under a
        put(193, g + 56 + 24);             // block whose own top, 160 up, is out of reach)
        put(193, g + 56 + 64);
        break;
    case kOpen: break;
    case kLedge56: put(740, g); break;
    case kLedge96: case kFallPast: put(193, g); put(740, g + 40); break;
    case kLedge112: put(740, g); put(740, g + 56); break;
    }
    return {g, fx + 64 - 8 + 8 - back, fy + 32};
}

// Holds jump on a course; returns the height reached above the floor (-1000: a bad end).
int synthetic(const Base& b, Synth c, bool easy, int back, int face_off, bool responsive = true, Attempt* out = nullptr) {
    Env e(b, easy, responsive);
    Course cs = build_course(e.w, c, back);
    int g = cs.g, ax = cs.ax, ay = cs.ay;
    int z0 = c == kFallPast ? g + 90 : g;
    if (!e.place(ax, ay, z0, (2 + face_off) & 7)) { std::printf("    (could not place for case %d)\n", int(c)); return -999; }
    Attempt a;
    if (c == kFallPast) {
        PlayerInput& in = e.w.input;
        in.jump_held = true;
        e.w.fall(1);
        for (int f = 0; f < 120; ++f) { e.step(); in.jump_held = true; }
        e.clear_input();
        e.run(40);
        a.rise = e.av().z - g;
        World::Placement p = e.w.can_exist(e.av().x, e.av().y, e.av().z, e.xd, e.yd, e.zd, e.flags(), 1);
        a.bad_end = !p.valid || !p.supported;
    } else {
        a = attempt(e, g, kHoldKey);
    }
    if (out) *out = a;
    if (a.bad_end) return -1000;
    return a.rise;
}

void test_synthetic(const Base& b) {
    int pillar = synthetic(b, kPillar160, true, 0, 0);
    int ceil = synthetic(b, kCeiling, true, 0, 0);
    int open = synthetic(b, kOpen, true, 0, 0);
    std::printf("  unreachable (Easy Ledges on, holding jump): a 160 pillar: rose %d, a 56 ledge under a slab: rose %d, open "
                "floor: rose %d\n", pillar, ceil, open);
    CHECK(pillar < 16 && pillar > -1000);
    CHECK(ceil < 16 && ceil > -1000);
    CHECK(open < 16 && open > -1000);
    // Reachable ones from awkward places.
    struct C { Synth s; int back, face; int want; };
    const C cs[] = {{kLedge56, 0, 0, 56}, {kLedge56, 64, 0, 56}, {kLedge56, 96, 1, 56}, {kLedge56, 32, -1, 56},
                    {kFallPast, 0, 0, 96}};
    int ok = 0, n = 0;
    for (const C& c : cs) {
        int r = synthetic(b, c.s, true, c.back, c.face);
        std::printf("    ledge case %d, %d back, facing %+d: reached %d (want %d)\n", int(c.s), c.back, 45 * c.face, r, c.want);
        ++n;
        ok += r == c.want;
        CHECK_EQ(r, c.want);
    }
    std::printf("  synthetic reachable ledges: %d of %d climbed\n", ok, n);
    // Two storeys are out of reach (user: "you can jump up 2 stories now"), from any distance.
    const C tall[] = {{kLedge96, 0, 0, 0}, {kLedge96, 48, 0, 0}, {kLedge96, 80, 1, 0}, {kLedge112, 0, 0, 0}, {kLedge112, 40, -1, 0}};
    for (const C& c : tall) {
        int r = synthetic(b, c.s, true, c.back, c.face);
        std::printf("    too high, case %d, %d back, facing %+d: rose %d\n", int(c.s), c.back, 45 * c.face, r);
        CHECK(r < 16 && r > -1000);
    }
    // Classic keeps avatar.md 5: the 96 ledge is not reached, the flush 56 one is climbed.
    int c96 = synthetic(b, kLedge96, true, 0, 0, false);
    int c56far = synthetic(b, kLedge56, true, 64, 0, false);
    std::printf("  Classic: the 96 ledge %d, the 56 ledge from 64 back %d\n", c96, c56far);
    CHECK(c96 < 96);
    CHECK(c56far < 56);
}

// --- The controller: hold X through the game mode (a virtual pad), the hint, screenshots ----

struct UI {
    GameMode gm;
    Surface8 surf{320, 200};
    UI(const Base& b, bool easy) : gm(b.d) {
        gm.world().auto_ui = true;
        gm.world().ask_name = false;
        gm.start();
        SaveReader in(b.save.data(), b.save.size());
        gm.world().load_game(in);
        gm.world().controls.responsive = true;
        gm.world().controls.assist = true;
        gm.world().controls.easy_ledges = easy;
        InputEvent add;
        add.type = InputEvent::kPadAdded;
        std::strcpy(add.name, "Test pad");
        gm.handle(add);
        pad(kPadGuide, true);  // a controller acts only after its first press
        pad(kPadGuide, false);
        frames(3);
    }
    void frames(int n) { for (int i = 0; i < n; ++i) { gm.tick(); gm.render(surf); } }
    void pad(int b, bool down) { InputEvent e; e.type = down ? InputEvent::kPadDown : InputEvent::kPadUp; e.button = b; gm.handle(e); }
    void shot(const std::string& name) {
        if (g_shots.empty()) return;
        std::vector<uint32_t> px;
        int fw, fh;
        gm.render_frame(px, fw, fh, 3, 0, 1.0 / 30);
        save_png(g_shots + "/" + name + ".png", px.data(), fw, fh, 1);
        std::printf("  shot %s (%dx%d)\n", name.c_str(), fw, fh);
    }
};

// Holds X in front of a ledge 96 up (48 back, facing 45 degrees off), through the game mode.
// Returns the height reached; notes whether the highlight and the hang were seen.
// `real`: instead, a 96-high ledge in the Upper Catacombs (map 50; its face at 2784,9912 going
// south-west), 48 back, facing west (45 degrees off; for the screenshots).
int pad_hold(const Base& b, bool easy, bool shots, bool& lit, bool& hung, bool real = false) {
    UI u(b, easy);
    World& w = u.gm.world();
    Course cs{0, 0, 0};
    if (real) {
        w.teleport_actor(1, 2784 + 48, 9912 - 48, 48, 50);
        u.frames(12);
        w.move_to(1, 2784 + 48, 9912 - 48, 48, false);
        cs = {48, 2784 + 48, 9912 - 48};
        w.avatar()->dir = 6;
    } else {
        cs = build_course(w, easy ? kLedge56 : kLedge96, 48);
        w.move_to(1, cs.ax, cs.ay, cs.g, false);
        w.avatar()->dir = 3;
    }
    w.camera_follow(1);
    u.frames(shots ? 130 : 20);  // (shots: after the "controller connected" note has gone)
    lit = hung = false;
    bool shot_lit = false, shot_hang = false;
    u.pad(kPadX, true);
    for (int f = 0; f < 120; ++f) {
        u.frames(1);
        if (w.ledge_hint.frames > 0) {
            lit = true;
            if (shots && !shot_lit && f >= 1) { u.shot("ledge_highlight"); shot_lit = true; }
        }
        if (w.avatar()->last_anim == 45) {
            hung = true;
            if (shots && !shot_hang) { u.shot("ledge_mid_grab"); shot_hang = true; }
        }
    }
    u.pad(kPadX, false);
    u.frames(40);
    if (shots) u.shot("ledge_pulled_up");
    if (std::getenv("PAGAN_LEDGE_VERBOSE")) {
        const Actor& av = *w.avatar();
        int xd, yd, zd;
        w.footprint(av, xd, yd, zd);
        World::Placement pl = w.can_exist(av.x, av.y, av.z, xd, yd, zd, w.shape_flags(av), 1);
        int top;
        uint16_t f, id;
        assist::surface_under(w, av.x - xd / 2, av.y - yd / 2, av.z, 1, top, f, id);
        const Item* it = w.item(id);
        const Item* land = w.item(pl.land);
        std::printf("  ended at %d,%d,%d on item %u shape %d flags %x (top %d); land item %u shape %d flags %x\n", av.x, av.y, av.z, id,
                    it ? int(it->shape) : -1, f, top, pl.land, land ? int(land->shape) : -1,
                    land ? w.data().typeflags.get(land->shape).flags() : 0);
    }
    return w.avatar()->z - cs.g;
}

void test_pad_and_hint(const Base& b) {
    bool lit, hung;
    int easy = pad_hold(b, true, false, lit, hung);
    std::printf("  controller, X held 48 back from a 56 ledge, facing 45 degrees off: reached %d (highlight %d, hung %d)\n", easy,
                int(lit), int(hung));
    CHECK_EQ(easy, 56);
    CHECK(lit);
    bool lit2, hung2;
    int off = pad_hold(b, false, false, lit2, hung2);
    std::printf("  ... with Easy Ledges off: reached %d\n", off);
    CHECK(off < 96);
    bool lit3, hung3;
    int real = pad_hold(b, true, true, lit3, hung3, true);
    std::printf("  controller, X held 48 back from a 96 ledge in the Upper Catacombs (too high now), facing 45 degrees off: reached %d\n", real);
    CHECK(real <= ledge::kMaxRise);
    // Nothing in reach: the hint shows once in a hold, not again in the same hold or the next.
    UI u(b, true);
    World& w = u.gm.world();
    Course cs = build_course(w, kOpen, 0);
    w.move_to(1, cs.ax, cs.ay, cs.g, false);
    w.avatar()->dir = 6;  // west: the floor goes on that way (the hops stay on it)
    w.camera_follow(1);
    u.frames(10);
    int hints = 0;
    std::string text;
    auto orig = w.hint_ui;
    w.hint_ui = [&](const std::string& t) { ++hints; text = t; if (orig) orig(t); };
    u.pad(kPadX, true);
    u.frames(20);
    int early = hints;  // a quick press says nothing
    u.frames(70);
    int held = hints;
    u.pad(kPadX, false);
    u.frames(30);
    u.pad(kPadX, true);
    u.frames(60);
    u.pad(kPadX, false);
    u.frames(30);
    std::printf("  no ledge in reach, X held: hint after 0.6 s %d, after 3 s %d (\"%s\"), after a second hold %d; rose %d\n", early,
                held, text.c_str(), hints, w.avatar()->z - cs.g);
    CHECK_EQ(early, 0);
    CHECK_EQ(held, 1);
    CHECK_EQ(hints, 1);
    CHECK(w.avatar()->z - cs.g < 16);
    // The option row and its setting.
    World& o = u.gm.world();
    o.controls.easy_ledges = false;
    o.store_options();
    o.controls.easy_ledges = true;
    o.load_options();
    CHECK(!o.controls.easy_ledges);
    o.controls.easy_ledges = true;
    o.store_options();
    bool row = false;
    for (const auto& f : World::extra_option_rows()) {
        MenuGump::OptionRow r = f(o);
        if (r.label == "Easy Ledges:") {
            row = r.value() == "On";
            r.change(1);
            row = row && r.value() == "Off" && !o.controls.easy_ledges;
            r.change(1);
        }
    }
    std::printf("  options: Easy Ledges row %s\n", row ? "toggles On / Off" : "missing");
    CHECK(row);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = argc > 1 ? argv[1] : GameFS::locate(argv[0]);
    g_shots = argc > 2 ? argv[2] : "";
    std::string only = argc > 3 ? argv[3] : "";
    if (!g_shots.empty()) mkdir(g_shots.c_str(), 0755);
    if (!g_shots.empty()) Config::global().load(g_shots + "/pagan_ledge_test.cfg");  // never the player's settings
    GameFS fs;
    if (root.empty() || !fs.open(root)) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    Base base(d);
    bool before = std::getenv("PAGAN_LEDGE_CLASSIC") != nullptr;
    bool verbose = std::getenv("PAGAN_LEDGE_VERBOSE") != nullptr;
    if (const char* site = std::getenv("PAGAN_LEDGE_SITE")) {
        // Debug one spot: map,x,y,z,dir[,back]: the search's answer and a traced attempt.
        unsigned m; int x, y, z, dir, back = 0;
        if (std::sscanf(site, "%u,%d,%d,%d,%d,%d", &m, &x, &y, &z, &dir, &back) >= 5) {
            Env e(base, !before);
            if (m != e.w.map_number_now()) { e.w.teleport_actor(1, x, y, z, m); e.run(12); }
            int sx = x - back * dir_x(dir), sy = y - back * dir_y(dir);
            std::printf("site: placed %d\n", int(e.place(sx, sy, z, dir)));
            {
                if (const char* from = std::getenv("PAGAN_LEDGE_FROM")) {
                    int fx2, fy2, fz2;
                    if (std::sscanf(from, "%d,%d,%d", &fx2, &fy2, &fz2) == 3) { e.w.teleport_actor(1, fx2, fy2, fz2, m); e.run(12); }
                }
                Site st;
                bool ok = probe_site(e, x, y, z, dir, st);
                std::printf("site: probe %d flush %d,%d h %d reachable %d not-a-ledge %d top shape %u\n", int(ok), st.fx, st.fy, st.h,
                            int(st.reachable), int(st.roof), st.top_shape);
            }
            {
                // The geometry: items near the face, the climbs as they are, standable tops.
                std::vector<uint16_t> near;
                e.w.area_search(x - 32, y - 32, 600, false, near);
                for (uint16_t id : near) {
                    const Item* it = e.w.item(id);
                    if (!it || id == 1) continue;
                    int ixd, iyd, izd;
                    e.w.footprint(*it, ixd, iyd, izd);
                    int qx = x - 32 + 48 * dir_x(dir), qy = y - 32 + 48 * dir_y(dir);  // ahead of him
                    if (qx + 48 <= it->x - ixd || qx - 48 > it->x || qy + 48 <= it->y - iyd || qy - 48 > it->y) continue;
                    uint16_t fl = e.w.data().typeflags.get(it->shape).flags();
                    if (!(fl & (ShapeInfo::kSolid | ShapeInfo::kLand | ShapeInfo::kRoof))) continue;
                    std::printf("  item %u shape %u at %d,%d,%d size %dx%dx%d flags %x\n", id, it->shape, it->x, it->y, it->z, ixd,
                                iyd, izd, fl);
                }
                for (unsigned k = 19; k <= 26; ++k) {
                    ActorState st;
                    st.x = x; st.y = y; st.z = z; st.dir = uint8_t(dir); st.last_anim = 2; st.last_anim_dir = uint16_t(dir);
                    AnimTracker t(e.w);
                    t.init(1, k, dir, &st);
                    int dx2, dy2, dz2;
                    t.travel(dx2, dy2, dz2);
                    int r = t.run_all(true);
                    std::printf("  climb %u: travel %d,%d,%d result %d end %d,%d,%d blocked %d\n", k, dx2, dy2, dz2, r, t.x(), t.y(),
                                t.z(), int(t.blocked()));
                }
            }
            for (int ahead = 0; ahead <= 96; ahead += 8) {
                int qx = x + ahead * dir_x(dir), qy = y + ahead * dir_y(dir);
                int top;
                uint16_t f, id;
                bool su = assist::surface_under(e.w, qx - e.xd / 2, qy - e.yd / 2, z + 200, 1, top, f, id);
                const Item* it = e.w.item(id);
                std::printf("  ahead %d: surface %d top %d item %u shape %d valid@z %d valid@top %d stand@top %d\n", ahead, int(su),
                            top, id, it ? int(it->shape) : -1, int(e.valid(qx, qy, z)), int(su && e.valid(qx, qy, top)),
                            int(su && e.standable(qx, qy, top)));
                if (it) {
                    int ixd, iyd, izd;
                    e.w.footprint(*it, ixd, iyd, izd);
                    std::printf("     item at %d,%d,%d size %dx%dx%d\n", it->x, it->y, it->z, ixd, iyd, izd);
                }
            }
            {
                auto t0 = std::chrono::steady_clock::now();
                ledge::Plan q;
                int hits = 0;
                for (int i = 0; i < 50; ++i) hits += ledge::find(e.w, e.av(), dir, true, q);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50;
                std::printf("site: find takes %.2f ms (found %d)\n", ms, hits);
            }
            ledge::Plan p;
            bool f = ledge::find(e.w, e.av(), dir, true, p);
            std::printf("site: find %d grab %d action %u rise %d dir %d spot %d,%d travel %d hang %d,%d,%d\n", int(f), int(p.grab),
                        p.move.action, p.rise, p.dir, p.sx, p.sy, p.travel, p.hx, p.hy, p.hz);
            setenv("PAGAN_LEDGE_TRACE", "1", 1);
            Attempt a = attempt(e, z, kHoldKey);
            std::printf("site: climbed %d rise %d bad %d\n", int(a.climbed), a.rise, int(a.bad_end));
        }
        return 0;
    }
    if (only.empty() || only == "survey") test_survey(base, !before, verbose);
    if (only.empty() || only == "synthetic") test_synthetic(base);
    if (only.empty() || only == "pad") test_pad_and_hint(base);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
