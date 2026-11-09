// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// A headless player for the tests: a new game run past the opening, input steering.
#pragma once
#include <cstdlib>
#include <vector>

#include "data/gamedata.h"
#include "game/processes.h"
#include "game/world.h"

namespace {

using namespace pg;

// A new game played past the opening conversation, with a 30 fps input clock.
struct Play {
    World w;
    int frames = 0;
    explicit Play(const GameData& d) : w(d) {
        w.auto_ui = true;
        w.new_game();
        run(8400);  // the opening scene ends and stasis is released at about frame 8300
    }
    void step() {
        w.input.now_ms = uint32_t(frames * 1000 / 30);
        w.frame();
        ++frames;
    }
    void run(int n) { for (int i = 0; i < n; ++i) step(); }
    Actor& av() { return *w.avatar(); }
    void warp(int x, int y, int z, unsigned map) {
        w.teleport_actor(1, x, y, z, map);
        run(10);
    }
    void release() {
        w.input.button[2].down = false;
        w.input.button[2].handled = true;
        w.input.key_jump = false;
    }
    // Walks toward (x, y) holding the right button, like the player; when stuck, lets go
    // and jumps (which climbs a ledge in front), or sidesteps for a moment. True if he got
    // within 48.
    bool walk_to(int x, int y, int max_frames = 3000, bool stop_on_stasis = false) {
        int stuck = 0, lx = -1, ly = -1, rel = 0, jumps = 0, side = 0, sides = 0;
        for (int f = 0; f < max_frames; ++f) {
            int dx = x - av().x, dy = y - av().y;
            if (std::abs(dx) < 48 && std::abs(dy) < 48) { release(); return true; }
            if (stop_on_stasis && w.stasis) { release(); return true; }
            PlayerInput& in = w.input;
            in.cursor_dir = direction_to(dx, dy);
            in.cursor_len = std::abs(dx) + std::abs(dy) > 400 ? 2 : 1;
            if (side > 0) {
                --side;
                in.cursor_dir = (in.cursor_dir + ((sides & 1) ? 2 : 6)) & 7;
                in.cursor_len = 1;
            }
            in.key_jump = false;
            if (rel > 0) {
                --rel;
                in.button[2].down = false;
                in.button[2].handled = true;
                if (rel == 10) in.key_jump = true;
            } else if (!in.button[2].down && !w.stasis) {
                in.button[2].press(in.now_ms, 0, 0);
            }
            if (av().x == lx && av().y == ly) ++stuck;
            else stuck = 0;
            lx = av().x;
            ly = av().y;
            if (stuck > 40 && !w.stasis) {
                stuck = 0;
                if (++jumps > 12) break;
                if (jumps % 2) rel = 20;
                else { side = 40; ++sides; }
            }
            step();
        }
        release();
        return false;
    }
    // Runs until the Avatar is idle (no animation, not falling).
    void settle(int max = 400) {
        for (int i = 0; i < max; ++i) {
            if (!w.kernel().count_exact(1, kProcActorAnim) && !av().gravity_pid && !av().is(Actor::kAnimLock)) return;
            step();
        }
    }
    // Plays one action on the Avatar and waits for it.
    void act(unsigned action, int dir) {
        w.do_anim(1, action, dir);
        settle();
    }
    uint16_t nearest(unsigned shape, int range = 512) {
        std::vector<uint16_t> near;
        w.area_search(av().x, av().y, range, false, near);
        uint16_t best = 0;
        int bd = 1 << 30;
        for (uint16_t k : near)
            if (const Item* it = w.item(k); it && it->shape == shape) {
                int d = std::abs(it->x - av().x) + std::abs(it->y - av().y);
                if (d < bd) { bd = d; best = k; }
            }
        return best;
    }
};

}  // namespace
