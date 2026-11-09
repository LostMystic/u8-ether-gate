// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Pagan transformed as the virtues grow (PLAN-VIRTUES.md section 5; the engine's own design, not in
// the original game; NOTES.md "World stages"). The virtue total (Virtues::total) picks a stage 0..3 by
// the thresholds in data/stages.json (in points, since only about 110-130 of the 400 can be earned).
// Stage 0 is the original game: it is the stage whenever the Virtues option is off.
//
// What the stages change:
// - the look of the outdoor maps (game_view.cpp asks): a warmer, brighter colour filter on the world
//   picture (never the UI), and now and then soft sun rays falling across it (5.1, 5.2);
// - monster eggs near the towns and roads skip some hatches, and the Cemetery's dead rise less once
//   enough Grant Peace is done (5.15, 5.17; intrinsic monster_egg_hatch asks skip_hatch);
// - Tenebrae and Herdsman's Valley take a lighter track (5.9; SoundSystem::music_play asks music);
// - townsfolk greet the Avatar by his deeds (5.11; data/greetings.json, barks from frame());
// - the rain storm is milder near the towns, with fewer flashes and clear mornings (5.3; intrinsics
//   0x0E0 / 0x0E1 ask storm_transform / skip_storm_flash);
// - lamps, braziers and hearths in Tenebrae cast a warm glow at night (5.4; game_view.cpp);
// - birdsong and the sea near the towns, fewer moans in the Cemetery (5.10; SoundSystem::play_sfx
//   asks hush_sfx);
// - the merchants give a little back on what they take (5.12, a rebate at the till);
// - the XP of skipped heartland hatches waits in a pool paid out on frontier kills (5.16;
//   Leveling::on_kill asks frontier_bonus);
// - grazing torax and small creatures in the valley, children playing in Tenebrae (5.18, 5.11).
// Nothing here is saved but the frontier pool (a key in Virtues::paid): everything else follows from
// the scores, or (hatch counts, greetings, creatures out) starts again after a load.
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "data/palette.h"

namespace pg {

class World;
class Item;

class WorldStages {
public:
    explicit WorldStages(World& w) : w_(&w) {}

    // --- Data (data/stages.json, data/greetings.json; either may be missing: that part does nothing)
    bool load_dir(const std::string& dir, std::string& err);
    bool load_text(const std::string& stages, const std::string& greetings, std::string& err);

    static constexpr int kStages = 4;  // 0 (the original) .. 3
    std::vector<int> thresholds{20, 40, 60};
    int forced = -1;  // debug menu / tests: this stage whatever the scores (still 0 with Virtues off)

    int stage() const;           // 0..3
    int natural_stage() const;   // by the scores alone (ignores `forced`; 0 with Virtues off)

    // --- Look (outdoor maps)
    struct Look { float palette = 0, rays = 0; };
    std::map<unsigned, Look> looks;  // by map
    struct PaletteShift { float bright = 0.07f, warm = 0.07f, saturation = 0.22f, lift = 0.12f; } palette;
    struct Rays {
        float episode = 26, fade = 5;
        std::array<float, kStages> chance{0, 0.3f, 0.5f, 0.75f};
        float slant = 0.55f, spacing = 150, width0 = 14, width1 = 46, drift = 2.5f, strength = 0.2f;
        int colour[3] = {255, 232, 170};
    } rays;
    // Once per presented frame, before the lookups below: advances the rays' clock by dt seconds
    // (not while the world is paused) and eases the colour shift toward the stage's and map's (over
    // ~2 s, also across a map change: the stitched world's seams are seamless).
    void view_frame(double dt);
    // The colour shift now (0 = none), and the filtered colour of an 0xAARRGGBB pixel.
    float shift() const { return shift_; }
    uint32_t filter(uint32_t c) const;
    // The world's lookup table through the filter; false (out untouched) when there is no shift.
    bool world_lut(const uint32_t in[256], uint32_t out[256]) const;
    // The sun rays' overall brightness now (0 = none showing), for the map and stage.
    float rays_level() const { return rays_level_; }
    // Adds the rays to one output row of n pixels. Output pixel x shows the world's screen point
    // (wx0 + x * wdx, wy) (world screen pixels, the camera's frame); top: 0 at the top of the
    // picture .. 1 at the bottom (the rays fade toward the bottom).
    void rays_row(uint32_t* row, int n, double wx0, double wdx, double wy, double top) const;
    double clock() const { return t_; }
    void set_clock(double t) { t_ = t; }

    // --- Monsters
    struct Area { unsigned map; int x0, y0, x1, y1; };
    std::array<int, kStages> heartland_one_in{0, 4, 3, 2};
    std::vector<Area> heartland;
    std::set<unsigned> heartland_not_shapes;
    std::set<unsigned> peace_maps, peace_shapes;
    std::vector<std::pair<int, int>> peace_steps;  // Grant Peace count, one_in
    int peace_count() const;  // undead laid to rest so far (keys Spirituality/peace/N)
    // The one-in-N rule for a monster egg (0 = never skipped) on the current map.
    int skip_one_in(const Item& egg) const;
    // Intrinsic monster_egg_hatch, before it creates anything: true = this hatch makes no monster.
    bool skip_hatch(const Item& egg);
    unsigned hatches_skipped = 0;  // tests

    // --- Music
    struct MusicSwap { std::set<unsigned> maps; int track = 0, to = 0, from_stage = 3; };
    std::vector<MusicSwap> music_swaps;
    int music(int track) const;  // the track a normal request on the current map plays instead

    // --- Greetings
    struct Greeting { std::string key, text; int min_stage = 1; int who = 0; };  // who: 0 town, 1 guard, 2 any
    std::vector<Greeting> greetings;
    std::set<unsigned> greet_maps, guard_shapes, greet_not_shapes, people_shapes, greet_not_npcs;
    int greet_range = 200, greet_check = 60, greet_wait = 900, greet_person_wait = 9000;
    std::array<float, kStages> greet_chance{0, 0.2f, 0.3f, 0.4f};
    unsigned greetings_said = 0;  // tests
    std::string last_greeting;    // tests
    // The lines actor `id` could say now (text before {you} is filled in).
    std::vector<const Greeting*> lines_for(uint16_t id) const;
    // Tries a greeting now (the chance and the waits skipped when `force`); returns the speaker or 0.
    uint16_t try_greet(bool force);
    std::string fill(const std::string& text) const;  // {you} / {You}
    // Could actor `id` greet now (a person near the Avatar on this map, free, not hostile)?
    bool greeter_ok(uint16_t id) const;


    // --- Time of day: the schedules' six periods (game hour mod 6; time.md 3, leveling_facts.md 4).
    int period() const;
    std::set<int> night_periods{5, 0}, morning_periods{1};
    bool night() const { return night_periods.count(period()) != 0; }
    int forced_period = -1;  // tests, the debug menu

    // --- Weather (5.3): the rain storm's grey transform (preset 3) eased toward clear on these maps.
    std::map<unsigned, float> clear_maps;                 // map -> weight 0..1
    std::array<float, kStages> clear_by_stage{0, 0.35f, 0.6f, 0.85f};
    std::array<float, kStages> skip_flash{0, 0.3f, 0.5f, 0.7f};  // storm flashes left out
    int clear_mornings_from = 2;                          // stage: no storm at all in the morning
    std::set<int> thunder_sfx{89, 91, 94, 96};
    float storm_clearing() const;                         // 0 (as the original) .. 1 (clear) here, now
    PaletteTransform storm_transform() const;             // preset 3 eased by storm_clearing()
    // Intrinsic 0x0E1 from usecode class `cls`: true = this storm flash (and its thunder) is skipped.
    bool skip_storm_flash(uint16_t cls);
    unsigned flashes_skipped = 0;

    // --- Lamps (5.4): warm pools of light round the lamps at night.
    struct GlowShape { float radius = 40, strength = 0.5f; int dz = 0; };
    std::map<unsigned, GlowShape> glow_shapes;
    std::set<unsigned> glow_maps;
    std::array<float, kStages> glow_by_stage{0, 0.6f, 0.8f, 1.0f};
    int glow_colour[3] = {255, 190, 110};
    float glow_level() const { return glow_level_; }
    size_t glows_in_view() const { return glows_.size(); }
    struct GlowAt { double x, y; float r, a; uint32_t seed; };  // world screen pixels
    const std::vector<GlowAt>& glows() const { return glows_; }
    // Adds the glows to one output row (the same frame as rays_row).
    void glows_row(uint32_t* row, int n, double wx0, double wdx, double wy) const;

    // --- Ambience (5.10)
    struct Ambience { std::set<unsigned> maps; std::vector<int> sfx; std::array<float, kStages> chance{}; int every = 240, volume = 90, when = 0, min_peace = 0; };  // when: 0 any, 1 day, 2 night; min_peace: Grant Peace count
    std::vector<Ambience> ambience;
    std::set<unsigned> hush_maps;
    std::set<int> hush_sfx_set;
    // SoundSystem::play_sfx: true = drop this request (Cemetery moans once the dead rest, a skipped
    // flash's thunder). Also counts what was asked for on this map (tests).
    bool hush_sfx(int sfx);
    unsigned ambience_played = 0, sfx_hushed = 0;
    int last_ambience = 0;
    std::map<int, unsigned> sfx_heard;
    void ambience_tick(bool force);

    // --- Market (5.12): a share of the obsidian a merchant takes in a conversation comes back.
    std::set<unsigned> merchants;
    std::array<int, kStages> rebate_pct{0, 5, 10, 15};
    int obsidian_carried() const;
    unsigned rebated = 0;  // obsidian given back so far (tests)

    // --- XP to the frontier (5.16)
    std::set<unsigned> frontier_maps;
    float frontier_share = 0.5f;  // a kill pays at most this share of its XP again from the pool
    int frontier_cap = 3000;
    int frontier_pool() const;
    void add_frontier_pool(int xp);
    // Leveling::on_kill on a frontier map: the bonus XP for a kill that paid `paid` (taken from the pool).
    int frontier_bonus(int paid);

    // --- Life (5.18, 5.11): creatures and children brought in by stage.
    struct Life {
        std::string id;
        std::set<unsigned> maps;
        unsigned shape = 0;
        int activity = 0, when = 0;  // when as Ambience
        std::array<int, kStages> count{};
        std::vector<std::array<int, 4>> spots;  // map, x, y, z
        bool no_xp = true;
    };
    std::vector<Life> life;
    std::map<std::string, std::vector<uint16_t>> life_out;  // by life id: spot -> actor (0 none)
    bool is_life(uint16_t id) const;
    void life_tick();
    unsigned life_spawned = 0;

    void frame();   // World::frame
    void reset();   // a new game, a load: the counters

private:
    bool key_done(const std::string& key) const;
    bool greeter_ok(uint16_t id, int ax, int ay, int az) const;
    World* w_;
    double t_ = 0;
    float shift_ = 0, rays_level_ = 0, glow_level_ = 0;
    std::vector<GlowAt> glows_;
    uint32_t skipped_flash_at_ = 0;
    bool flash_skipped_ = false;
    std::map<size_t, uint32_t> ambience_next_;
    int money_ = -1, rebate_carry_ = 0;
    uint16_t money_talk_ = 0;
    uint32_t money_seen_ = 0;
    uint16_t merchant_now() const;
    unsigned life_map_ = 0xFFFF;
    std::map<std::string, std::vector<uint8_t>> life_done_;  // spot killed this visit
    void market_tick();
    void weather_tick();
    void view_glows(int stage, unsigned map);
    unsigned shift_map_ = 0xFFFF;
    std::map<uint64_t, unsigned> hatch_count_;  // map:x:y:z of a monster egg -> hatches tried
    uint32_t frames_ = 0, last_greet_ = 0;
    bool greeted_once_ = false;
    std::map<uint16_t, uint32_t> greeted_;
    std::set<std::string> heard_;
    std::mt19937 rng_{0x5eed};
};

}  // namespace pg
