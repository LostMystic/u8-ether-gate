// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// Game variants: the v2.12 option, the restored undead invasion, the underworld entrance, the targeted jump lives in avatar_mover.cpp (variants.h).
#include "game/variants.h"

#include <functional>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "data/compat.h"
#include "game/gumps.h"
#include "game/world.h"
#include "world/map_patches.h"
#include "world/world_space.h"

namespace pg {

namespace {
void save_config() {
    Config& c = Config::global();
    if (!c.path().empty()) c.save();
}
}  // namespace

void register_variant_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World&) {
        MenuGump::OptionRow r{"v2.12 changes:", [] { return std::string(v212_changes() ? "On" : "Off"); },
                              [](int) {
                                  v212_changes() = !v212_changes();
                                  Config::global().set_bool("v212", v212_changes());
                                  save_config();
                              }};
        r.group = "Game";
        return r;
    });
    World::extra_option_rows().push_back([](World& w) {
        MenuGump::OptionRow r{"Undead invasion:", [] { return std::string(!compat::usecode_known() ? "N/A" : restored_invasion() ? "On" : "Off"); },
                              [&w](int) {
                                  restored_invasion() = !restored_invasion();
                                  Config::global().set_bool("restore_invasion", restored_invasion());
                                  save_config();
                                  invasion::option_changed(w);
                              }};
        r.group = "Game";
        return r;
    });
    World::extra_option_rows().push_back([](World& w) {
        MenuGump::OptionRow r{"Underworld entrance:", [] {
                                  bool fits = true;
                                  for (const map_patches::Group& g : map_patches::groups()) fits = fits && map_patches::data_matches(g);
                                  return std::string(!fits ? "N/A" : underworld_entrance() ? "On" : "Off");
                              },
                              [&w](int) {
                                  underworld_entrance() = !underworld_entrance();
                                  Config::global().set_bool("underworld_entrance", underworld_entrance());
                                  save_config();
                                  underworld::sync_entrance(w);
                              }};
        r.group = "Game";
        return r;
    });
}

void apply_variant_options(World& w) {
    const Config& c = Config::global();
    v212_changes() = c.get_bool("v212", true);
    restored_invasion() = c.get_bool("restore_invasion", true);
    underworld_entrance() = c.get_bool("underworld_entrance", true);
    w.engine_quake = 0;  // (a loaded game has no engine quake running: its quake was saved as none)
    if (!restored_invasion() && w.vm().get_global(invasion::kFlag, 1)) {
        w.vm().set_global(invasion::kFlag, 1, 0);
        PG_LOG("invasion: option Off, G[58:1] cleared");
    }
}

namespace underworld {

void sync_entrance(World& w) {
    CurrentMap& m = w.map();
    for (const map_patches::Group& g : map_patches::groups()) {
        if (!map_patches::data_matches(g)) continue;  // never added, nothing to take away
        if (w.stitch) w.stitch->forget_statics(g.map);
        if (g.map != m.map_number()) continue;
        const bool on = map_patches::group_on(g);
        int added = 0, removed = 0;
        for (const ItemRecord& r : g.items) {
            uint16_t found = 0;
            for (uint16_t id : m.chunk(r.x / CurrentMap::kChunkSize, r.y / CurrentMap::kChunkSize)) {
                const Item* it = w.item(id);
                if (it && it->origin == Item::kFixed && it->shape == r.shape && it->frame == r.frame && it->x == r.x &&
                    it->y == r.y && it->z == r.z) {
                    found = id;
                    break;
                }
            }
            if (on && !found) {
                m.create_tree({MapEntry{r, -1}}, Item::kFixed);
                ++added;
            } else if (!on && found) {
                w.destroy_now(found);
                ++removed;
            }
        }
        // Moves, either way: an item the player has since carried off is simply not found.
        for (const map_patches::Move& mv : g.moves) {
            const bool moved = map_patches::moves_on(g);
            const int from[3] = {moved ? mv.x : mv.to_x, moved ? mv.y : mv.to_y, moved ? mv.z : mv.to_z};
            const int to[3] = {moved ? mv.to_x : mv.x, moved ? mv.to_y : mv.y, moved ? mv.to_z : mv.z};
            std::vector<uint16_t> here = m.chunk(from[0] / CurrentMap::kChunkSize, from[1] / CurrentMap::kChunkSize);
            for (uint16_t id : here) {
                const Item* it = w.item(id);
                if (it && it->shape == mv.shape && it->frame == mv.frame && it->x == from[0] && it->y == from[1] && it->z == from[2]) {
                    w.move_to(id, to[0], to[1], to[2], false);
                    break;
                }
            }
        }
        if (added || removed) PG_LOG("map patch %s: %d items added, %d removed on map %u", g.id.c_str(), added, removed, g.map);
    }
}

}  // namespace underworld

namespace invasion {

Stats& stats() {
    static Stats s;
    return s;
}

bool avatar_has(const World& w, unsigned shape, unsigned frame) {
    const Actor* av = w.avatar();
    if (!av) return false;
    std::function<bool(const Item&, int)> find = [&](const Item& c, int depth) {
        if (depth > 8) return false;
        for (uint16_t k : c.contents)
            if (const Item* it = w.item(k)) {
                if (it->shape == shape && it->frame == frame) return true;
                if (!it->contents.empty() && find(*it, depth + 1)) return true;
            }
        return false;
    };
    return find(*av, 0);
}

namespace {
void stop_engine_quake(World& w) {
    if (w.engine_quake > 0) {
        w.engine_quake = 0;
        w.quake = 0;
    }
}
}  // namespace

void option_changed(World& w) {
    if (restored_invasion()) return;  // switched On: the next check sets the flag if he has the Heart
    stop_engine_quake(w);
    if (w.vm().get_global(kFlag, 1)) {
        w.vm().set_global(kFlag, 1, 0);
        ++stats().clears;
        PG_LOG("invasion: option switched Off, G[58:1] cleared");
    }
}

void frame(World& w) {
    if (!restored_invasion()) return;
    // Its story lives in many classes of the shipped usecode, behind a flag nothing else writes.
    if (!compat::usecode_known()) {
        compat::off("the Undead invasion", "this copy's usecode differs");
        return;
    }
    UCMachine& vm = w.vm();
    const uint32_t f = w.game_frames();
    if (f % kCheckFrames == 0) {
        bool on = vm.get_global(kFlag, 1) != 0;
        if (!on) {
            if (!vm.get_global(kPlacedFlag, 1) && avatar_has(w, kHeartShape, kHeartFrame) &&
                !avatar_has(w, kAbsorbedShape, kHeartFrame)) {
                vm.set_global(kFlag, 1, 1);
                ++stats().sets;
                PG_LOG("invasion: the Heart of Earth is in the Avatar's inventory: G[58:1] set (map %u)", w.map_number_now());
            }
        } else if (vm.get_global(kPlacedFlag, 1) || avatar_has(w, kAbsorbedShape, kHeartFrame)) {
            vm.set_global(kFlag, 1, 0);
            ++stats().clears;
            stop_engine_quake(w);
            PG_LOG("invasion: Lithos absorbed or the Heart placed: G[58:1] cleared");
        }
    }
    // Earthquakes in Tenebrae (the lines speak of quaking ground; no original code starts one).
    if (w.engine_quake > 0) {
        if (--w.engine_quake == 0 || w.quake != kQuakeStrength) {
            if (w.quake == kQuakeStrength) w.quake = 0;  // (a script's own quake is left alone)
            w.engine_quake = 0;
        }
        return;
    }
    unsigned m = w.map_number_now();
    if (vm.get_global(kFlag, 1) && (m == 39 || m == 40 || m == 41) && f % kQuakePeriod == kQuakePeriod / 2 && w.quake == 0 &&
        !w.stasis) {
        w.quake = kQuakeStrength;
        w.engine_quake = kQuakeFrames;
        ++stats().quakes;
        PG_LOG("invasion: an earthquake in Tenebrae (map %u)", m);
    }
}

}  // namespace invasion

}  // namespace pg
