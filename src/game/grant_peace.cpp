// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Grant Peace (specs/world/magic.md 7.2). See grant_peace.h.
#include "game/grant_peace.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/combat.h"
#include "game/processes.h"
#include "game/virtues.h"
#include "game/world.h"
#include "kernel/kernel.h"

namespace pg {

namespace {

constexpr unsigned kActCast1 = 27, kActCast2 = 28, kActCast3 = 29;  // actors.md: cast 1..3
constexpr uint16_t kPeaceDamage = kDmgMagic | kDmgPierce | kDmgFire;  // 0x00A8
constexpr unsigned kThrone = 289, kGhost = 411, kFlame = 480;

bool is_undead(const Actor& a) {
    const MonsterInfo* m = monster_info(a.shape);
    return m && (m->def_type & kDmgUndead);
}

long long dist2(const Item& a, const Item& b) {
    const long long dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

class GrantPeaceProcess : public Process {
public:
    explicit GrantPeaceProcess(World& w) : w_(w) {
        type = kProcGrantPeace;
        item = 1;
        prevents_saving = true;  // a few seconds of animation and a cursor: not worth a save format
    }
    const char* name() const override { return "grant_peace"; }

    void run() override {
        switch (step_++) {
        case 0: wait_for(w_.do_anim(1, kActCast1, -1)); return;
        case 1: wait_for(w_.do_anim(1, kActCast3, -1)); return;
        case 2: wait_for(w_.target_pick()); return;  // wakes with the clicked object (0: cancelled)
        case 3:
            apply(uint16_t(result));
            wait_for(w_.do_anim(1, kActCast2, -1));
            return;
        default: result = 0; terminate(); return;
        }
    }

private:
    void hit(Actor& v, int damage, const Actor& av) {
        const int d = direction_to(v.x - av.x, v.y - av.y);
        w_.receive_hit(v.id, 1, d, damage, kPeaceDamage);
    }

    void apply(uint16_t target) {
        Actor* av = w_.avatar();
        Actor* t = w_.actor(target);
        if (!av || !t || target == 1) return;
        bool any = false;
        if (is_undead(*t)) {
            // The clicked undead's HP is the damage every undead nearby takes.
            const int damage = t->hp;
            std::vector<uint16_t> near;
            w_.area_search(av->x, av->y, 2048, false, near);
            uint16_t throne = 0;
            for (uint16_t id : near)
                if (const Item* it = w_.item(id); it && it->shape == kThrone && dist2(*it, *av) <= 2048LL * 2048) { throne = id; break; }
            std::vector<uint16_t> victims;
            for (uint16_t id : near) {
                const Item* it = w_.item(id);
                const Actor* a = it ? it->as_actor() : nullptr;
                if (!a || id == 1 || a->is(Actor::kDead) || !is_undead(*a)) continue;
                if (a->mapnum != av->mapnum || dist2(*a, *av) > 768LL * 768) continue;
                victims.push_back(id);
            }
            int laid = 0;
            for (uint16_t id : victims) {
                Actor* a = w_.actor(id);
                if (!a || a->is(Actor::kDead)) continue;
                hit(*a, damage, *av);
                any = true;
                if (Actor* b = w_.actor(id)) {
                    if (b->is(Actor::kDead)) ++laid;
                    const int d = direction_to(b->x - av->x, b->y - av->y);
                    auto roll = [&] { return 5 + w_.rand_below(5); };  // U[5,9]
                    w_.hurl(id, roll() * dir_x(d), roll() * dir_y(d), roll(), 4);
                    if (b->shape == kGhost && throne) {
                        w_.kernel().add(std::make_unique<SpriteProcess>(w_, kFlame, 0, 9, 1, 1, b->x, b->y, b->z));
                        if (Item* th = w_.item(throne)) th->frame = 1;
                    }
                }
            }
            pay_virtue(laid);
        } else if (!t->is(Actor::kDead) && !t->is(Actor::kImmortal) && !t->is(Actor::kInvincible)) {
            if (w_.rand_below(10) == 0) {
                hit(*t, t->hp, *av);
                any = true;
            }
        }
        if (!any) return;
        // Lightning (magic.md 7.4) and one of three sounds.
        PaletteTransform from;
        const int d = 0x3F * 2048 / 255, off = 0xCF * 2048 / 255;
        from.m = {d, 0, 0, off, 0, d, 0, off, 0, 0, d, off};
        w_.start_fade(w_.palette_target, 10, -1, &from);
        static const int sounds[3] = {91, 94, 96};
        w_.sound.play_sfx(sounds[w_.rand_below(3)]);
    }

    // Virtues: 1 Spirituality for each undead laid to rest, up to the cap per game.
    void pay_virtue(int laid) {
        if (!w_.virtues || !w_.virtues->enabled) return;
        bool told = false;
        for (int n = 1; n <= kPeaceVirtueCap && laid > 0; ++n) {
            const std::string key = "peace/" + std::to_string(n);
            if (w_.virtues->has_paid(kSpirituality, key)) continue;
            if (told) w_.virtues->add(kSpirituality, 1, key);
            else told = w_.virtues->earn(kSpirituality, 1, key, "The dead are at peace. Your Spirituality grows.") > 0;
            --laid;
        }
    }

    World& w_;
    int step_ = 0;
};

}  // namespace

uint16_t start_grant_peace(World& w) {
    PG_LOG("grant peace: cast");
    return w.kernel().add(std::make_unique<GrantPeaceProcess>(w));
}

}  // namespace pg
