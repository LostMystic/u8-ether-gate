// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 3 tests: the Avatar in the world. Movement and collision on known terrain
// (walls stop him, step-up, climbing, falling and fall damage, doors), the swept-box test,
// reach with line of sight, the pathfinder, a scripted fight to the death on both sides,
// and an input-driven walk from Devon's camp to the Tenebrae gate.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "play.h"
#include "data/gamedata.h"
#include "game/anim_tracker.h"
#include "game/pathfinder.h"
#include "game/processes.h"
#include "game/world.h"

using namespace pg;

namespace {

}  // namespace

void test_milestone3(const GameData& d) {
    // --- Walls stop him, small steps are climbed automatically, ledges need a climb.
    {
        Play p(d);
        CHECK(!p.w.stasis);
        CHECK(p.w.mover_pid != 0);
        CHECK_EQ(p.av().x, 14527);
        CHECK_EQ(p.av().z, 8);
        // Walk north from the camp (walk actions, no jumping): he steps up the low bank to
        // z 16 on his own, then the 32-unit ledge stops him.
        int max_z = p.av().z;
        bool stepped_up = false;
        for (int i = 0; i < 40; ++i) {
            int z0 = p.av().z;
            p.act(0, 0);
            if (p.av().z > z0 && p.av().z - z0 <= 8) stepped_up = true;
            max_z = std::max(max_z, p.av().z);
        }
        CHECK(stepped_up);
        CHECK_EQ(max_z, 16);
        CHECK(p.av().y < 5000);
        int stopped_y = p.av().y;
        p.act(0, 0);
        CHECK_EQ(p.av().y, stopped_y);  // a further walk north goes nowhere
        // Trials of the climbs (actions 19-26, 16..72 units): the low ones hit the bank face.
        int lowest = -1;
        for (int k = 0; k < 8 && lowest < 0; ++k) {
            AnimTracker t(p.w);
            if (t.init(1, 19 + unsigned(k), 0) && t.run_all(true) == AnimTracker::kOk) lowest = k;
        }
        CHECK(lowest >= 1);
        int z0 = p.av().z;
        // Jump (Space, standing still) climbs it with the lowest climb that works.
        p.w.input.key_jump = true;
        p.step();
        p.w.input.key_jump = false;
        CHECK_EQ(p.av().last_anim, 19u + unsigned(lowest));
        p.settle();
        CHECK_EQ(p.av().z, 48);
        CHECK_EQ(p.av().z - z0, 16 + 8 * lowest);
        CHECK(p.av().y < stopped_y);
        std::printf("  walk north from the camp: stepped up to z 16, stopped at y %d, climbed to z %d\n", stopped_y,
                    p.av().z);
        // Between two gravestones (shape 65) he cannot go north or west.
        p.warp(14559, 2835, 48, 3);
        CHECK_EQ(p.av().x, 14559);
        p.act(0, 6);
        CHECK_EQ(p.av().x, 14559);
        p.act(0, 0);
        CHECK(p.av().y >= 2815);
        World::Placement pl = p.w.can_exist(14559 - 24, 2835, 48, 64, 64, 40, p.w.shape_flags(p.av()), 1);
        CHECK(!pl.valid);
        const Item* stone = p.w.item(pl.blocker);
        CHECK(stone && stone->shape == 65);
        // The swept test reports the stone as a blocking contact.
        std::vector<SweepHit> hits;
        p.w.sweep(p.w.box_of(p.av()), 14559 - 100, 2835, 48, p.w.shape_flags(p.av()), 1, true, hits);
        bool stone_hit = false;
        for (const SweepHit& h : hits)
            if (h.item == pl.blocker && h.blocking && !h.touching) stone_hit = true;
        CHECK(stone_hit);
    }

    // --- Falling: gravity, landing and fall damage (physics.md 7).
    {
        Play p(d);
        CHECK_EQ(p.av().hp, 30u);
        p.w.move_to(1, 14527, 5887, 40, false);  // 32 above the camp ground
        CHECK(p.w.fall(1) != 0);
        bool landed_anim = false;
        for (int i = 0; i < 600 && !landed_anim; ++i) {
            p.step();
            landed_anim = p.av().last_anim == 16;  // the land animation
        }
        CHECK(landed_anim);
        p.settle(600);
        CHECK_EQ(p.av().z, 8);
        CHECK_EQ(p.av().hp, 30u);  // a short drop does no harm
        p.w.move_to(1, 14527, 5887, 100, false);  // 92 above: (92 - 72) / 4 = 5 damage
        p.w.fall(1);
        p.settle(600);
        CHECK_EQ(p.av().z, 8);
        CHECK_EQ(p.av().hp, 25u);
        // A dropped item bounces and comes to rest on the ground.
        uint16_t pot = p.w.create_item(537, 2);  // meat
        p.w.move_to(pot, 14460, 5880, 60, false);
        uint16_t gp = p.w.fall(pot);
        CHECK(gp != 0);
        p.run(300);
        const Item* it = p.w.item(pot);
        CHECK(it && it->z <= 16 && it->gravity_pid == 0);
        if (it) std::printf("  fall: avatar landed at z 8, hp 30 -> 25 from 92 units; dropped meat came to rest at z %d\n", it->z);
        // Throwing it (physics.md 10): launched from the Avatar + 24 in z at a point some 270
        // away whose missile path is clear (the camp's rope and Devon are in some lines).
        int r = std::max(1, 64 - int(p.w.total_weight(*p.w.item(pot))) + p.av().str);
        static const int cand[][2] = {{240, 120}, {120, 240}, {-200, 220}, {260, -60}, {0, 280}};
        int tx = 0, ty = 0, tz = 8;
        bool clear = false;
        for (const auto& c : cand) {
            tx = p.av().x + c[0];
            ty = p.av().y + c[1];
            if ((clear = p.w.missile_path_clear(pot, p.av().x, p.av().y, p.av().z + 24, tx, ty, tz, r, 4, 1))) break;
        }
        CHECK(clear);
        p.w.collide_move(pot, p.av().x, p.av().y, p.av().z + 24, true, true);
        int vx, vy, vz, n;
        p.w.missile_velocity(p.av().x, p.av().y, p.av().z + 24, tx, ty, tz, r, 4, vx, vy, vz, n);
        CHECK(n > 0);
        CHECK(p.w.hurl(pot, vx, vy, vz, 4) != 0);
        // It touches down on the target point after n frames (then bounces on).
        int first_x = -1, first_y = -1;
        for (int i = 0; i < 400; ++i) {
            p.step();
            it = p.w.item(pot);
            if (it && first_x < 0 && it->z <= tz + 8) { first_x = it->x; first_y = it->y; }  // (or on the rope)
        }
        it = p.w.item(pot);
        CHECK(it && it->gravity_pid == 0);
        CHECK(std::abs(first_x - tx) < 48 && std::abs(first_y - ty) < 48);
        if (it)
            std::printf("  throw: speed %d, %d frames, aimed at %d,%d, touched down at %d,%d, came to rest at %d,%d,%d\n", r,
                        n, tx, ty, first_x, first_y, it->x, it->y, it->z);
    }

    // --- Doors (use event) and line of sight in Tenebrae (map 39).
    {
        Play p(d);
        p.warp(16300, 12300, 48, 39);
        CHECK_EQ(p.w.map().map_number(), 39u);
        CHECK(p.walk_to(16130, 11900));
        uint16_t door = p.nearest(69, 256);
        CHECK(door != 0);
        const Item* di = p.w.item(door);
        CHECK(di && di->x == 16191 && di->y == 11807);
        unsigned closed = di ? di->frame : 0;
        // Something on the far side of the closed door is out of sight.
        uint16_t coin = p.w.create_item(143, 0);
        p.w.item(coin)->quality = 5;
        p.w.move_to(coin, 16140, 11740, 48, false);
        CHECK(!p.w.can_reach(1, coin, 128));
        // Walking into the closed door stops him.
        CHECK(!p.walk_to(16130, 11650, 300));
        CHECK(p.av().y >= 11871);
        p.w.move_to(1, 16152, 11944, 48, false);  // back in front of it
        // Use it (a double-click does this when in reach), then walk through.
        CHECK(p.w.can_reach(1, door, 128));
        p.w.call_event(door, kEvUse);
        p.run(60);
        CHECK(p.w.item(door) && p.w.item(door)->frame != closed);
        CHECK(p.w.can_reach(1, coin, 128));
        CHECK(p.walk_to(16130, 11550));
        CHECK(p.av().y < 11700);
        std::printf("  door: closed door (frame %u) stopped him at y %d; used it (frame %u) and walked through\n", closed,
                    11871, p.w.item(door) ? p.w.item(door)->frame : 0);
        // Picking the coins up puts them in the backpack (owned).
        CHECK(p.w.give_to_avatar(coin));
        CHECK(p.w.root_container(*p.w.item(coin))->id == 1);
        CHECK(p.w.item(coin)->flags & kItemOwned);
    }

    // --- The pathfinder (actors.md 8) around the gravestones north of the camp.
    {
        Play p(d);
        p.warp(14559, 2835, 48, 3);
        Pathfinder pf(p.w);
        pf.set_actor(1);
        pf.set_target_point(13483, 2371, 48);
        std::vector<PathStep> path;
        CHECK(pf.plan(path));
        CHECK(path.size() >= 5);
        CHECK(pf.expanded() < Pathfinder::kMaxExpanded);
        uint16_t pid = p.w.pathfind(1, 13483, 2371, 48, 0, 0);
        CHECK(pid != 0);
        for (int i = 0; i < 2000 && p.w.kernel().get(pid); ++i) p.step();
        CHECK(!p.w.kernel().get(pid));
        int dx = p.av().x - 13483, dy = p.av().y - 2371;
        CHECK(dx * dx + dy * dy < 48 * 48 + 64 * 64);
        std::printf("  pathfinder: %zu steps planned with %u expansions; walked to %d,%d\n", path.size(),
                    pf.expanded(), p.av().x, p.av().y);
        // An unreachable point (inside the town wall) fails within the node limit.
        Pathfinder pf2(p.w);
        pf2.set_actor(1);
        pf2.set_target_point(12400, 2000, 48);
        CHECK(!pf2.plan(path));
    }

    // --- Combat: the Avatar with a sword kills a ghoul; a torax kills a passive Avatar.
    {
        Play p(d);
        uint16_t sword = p.w.create_item(420, 0);
        Item* si = p.w.item(sword);
        si->parent = 1;
        si->flags |= kItemEquipped | kItemOwned;
        si->z = 6;
        p.av().contents.push_back(sword);
        CHECK_EQ(p.w.equipped(1, 6), sword);
        uint16_t g = p.w.create_monster(142, 0);
        CHECK(g != 0 && g < 256);
        p.w.item(g)->flags |= kItemFastOnly | kItemDisposable;
        p.w.move_to(g, p.av().x + 160, p.av().y - 200, p.av().z);
        p.w.set_activity(g, 1);  // combat, as a monster egg would
        CHECK(p.w.actor(g)->is(Actor::kInCombat));
        p.w.set_in_combat(1);
        CHECK(p.av().is(Actor::kInCombat));
        bool dead = false;
        int swings = 0;
        for (int i = 0; i < 3000 && !dead; ++i) {
            Actor* m = p.w.actor(g);
            if (!m) break;
            dead = m->is(Actor::kDead);
            p.w.input.cursor_dir = direction_to(m->x - p.av().x, m->y - p.av().y);
            if (i % 20 == 0) {  // a left double-click: attack
                uint32_t t = p.w.input.now_ms;
                p.w.input.button[0].press(t, 0, 0);
                p.w.input.button[0].down = false;
                p.w.input.button[0].press(t, 0, 0);
                p.w.input.button[0].down = false;
                ++swings;
            }
            p.step();
        }
        CHECK(dead);
        const Actor* corpse = p.w.actor(g);
        CHECK(corpse && corpse->is(Actor::kDead) && corpse->hp == 0);
        CHECK(corpse && !corpse->is(Actor::kInCombat));
        p.run(60);
        CHECK(p.w.actor(g) && p.w.actor(g)->last_anim == 14);  // the body stays, in its death pose
        CHECK(!p.av().is(Actor::kDead));
        std::printf("  combat: the ghoul died after %d swings; the Avatar has %u hp\n", swings, p.av().hp);

        // A torax (armour 28) against a passive Avatar: he dies and the game ends.
        Play q(d);
        uint16_t t = q.w.create_monster(120, 0);
        q.w.item(t)->flags |= kItemFastOnly | kItemDisposable;
        q.w.move_to(t, q.av().x + 160, q.av().y - 200, q.av().z);
        q.w.set_activity(t, 1);
        int died_at = -1;
        for (int i = 0; i < 6000 && !q.w.game_over; ++i) {
            q.step();
            if (died_at < 0 && q.av().is(Actor::kDead)) died_at = q.frames;
        }
        CHECK(died_at > 0);
        CHECK(q.w.stasis);
        CHECK(q.w.game_over);
        std::printf("  combat: a torax killed the passive Avatar at frame %d; gravestone, game over at frame %d\n",
                    died_at, q.frames);
        q.w.new_game();  // what the game mode does then
        CHECK(!q.w.game_over && q.av().hp == 30 && !q.av().is(Actor::kDead));
    }

    // --- A real opening-area monster: the spider the monster egg east of the camp hatches.
    // The Avatar walks over, enters combat and kills it with his fists (left double-clicks).
    {
        Play p(d);
        uint16_t spider = 0;
        for (unsigned n = 62; n < 256 && !spider; ++n)
            if (const Actor* a = p.w.actor(uint16_t(n)); a && a->shape == 807) spider = uint16_t(n);
        static const int route[][2] = {{14600, 4700}, {15200, 4300}, {16000, 4700}, {16600, 5000}};
        for (const auto& r : route) p.walk_to(r[0], r[1]);
        if (!spider)
            for (unsigned n = 62; n < 256 && !spider; ++n)
                if (const Actor* a = p.w.actor(uint16_t(n)); a && a->shape == 807) spider = uint16_t(n);
        CHECK(spider != 0);
        CHECK(spider && p.w.actor(spider)->is(Actor::kInCombat));  // the egg gave it the combat activity
        p.w.set_in_combat(1);
        bool hurt = false, killed = false;
        for (int i = 0; i < 2000 && spider && !killed; ++i) {
            const Actor* m = p.w.actor(spider);
            if (!m) break;
            killed = m->is(Actor::kDead);
            int dist = std::abs(m->x - p.av().x) + std::abs(m->y - p.av().y);
            p.w.input.cursor_dir = direction_to(m->x - p.av().x, m->y - p.av().y);
            p.w.input.cursor_len = 1;
            if (dist > 200) {
                if (!p.w.input.button[2].down) p.w.input.button[2].press(p.w.input.now_ms, 0, 0);
            } else {
                p.release();
                if (i % 20 == 0) {
                    uint32_t t = p.w.input.now_ms;
                    p.w.input.button[0].press(t, 0, 0);
                    p.w.input.button[0].down = false;
                    p.w.input.button[0].press(t, 0, 0);
                    p.w.input.button[0].down = false;
                }
            }
            p.step();
            hurt |= p.av().hp < 30;
        }
        CHECK(killed);
        CHECK(!p.av().is(Actor::kDead));
        std::printf("  opening monster: the egg's spider %s the Avatar and %s (Avatar hp %u)\n",
                    hurt ? "bit" : "did not hurt", killed ? "was killed with fists" : "survived", p.av().hp);
    }

    // --- The player's walk from Devon's camp to the Tenebrae gate, steering the controller
    // through the input like a player holding the right button (jumping up the bank).
    {
        Play p(d);
        static const int route[][2] = {{14600, 4700}, {14559, 3000}, {14100, 3100}, {13483, 2371}, {12031, 2311}};
        bool all = true;
        for (const auto& r : route) all &= p.walk_to(r[0], r[1]);
        CHECK(all);
        CHECK_EQ(p.av().z, 48);
        // Near the gate the guard stops him and talks (usecode, avatar in stasis).
        p.walk_to(11750, 2170, 600, true);
        bool halted = p.w.stasis;
        CHECK(halted);
        std::printf("  camp to the Tenebrae gate: arrived at %d,%d,%d by frame %d; the guard halts him\n", p.av().x,
                    p.av().y, p.av().z, p.frames);
        CHECK_EQ(p.w.vm().errors(), 0u);
    }
}
