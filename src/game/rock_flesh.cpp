// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// Rock Flesh, remade (game/rock_flesh.h, NOTES.md "Rock Flesh").
#include "game/rock_flesh.h"

#include <algorithm>
#include <array>
#include <memory>

#include "core/bugfix.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/processes.h"
#include "game/spellbook.h"
#include "game/turn_combat.h"
#include "game/world.h"
#include "kernel/kernel.h"

namespace pg {
namespace rock_flesh {

namespace {
constexpr unsigned kToken = 746, kRockFleshFrame = 2;
constexpr unsigned kActCast1 = 27, kActCast2 = 28, kActCast3 = 29;  // actors.md: cast 1..3
constexpr unsigned kDust = 578, kDustFirst = 66, kDustLast = 80;    // the grey dust cloud (creature_facts.md)
constexpr int kSfxCast = 54, kSfxFizzle = 124, kSfxStone = 57;

bool friendly(World& w, uint16_t t) {
    const Actor* a = w.actor(t);
    if (!a || a->is(Actor::kDead)) return false;
    return t == 1 || w.same_party(1, t);
}

class RockFleshProcess : public Process {
public:
    RockFleshProcess(World& w, uint16_t token, bool learn) : w_(w), token_(token), learn_(learn) {
        item = 1;
        prevents_saving = true;  // a few seconds of animation and a cursor
    }
    const char* name() const override { return "rock_flesh"; }
    void run() override {
        Actor* av = w_.avatar();
        if (!av) { terminate(); return; }
        switch (step_++) {
        case 0:
            if (av->mana < kMana) {  // fizzle, as the original: the token is lost
                w_.sound.play_sfx(kSfxFizzle);
                w_.destroy(token_);
                wait_for(w_.do_anim(1, kActCast2, -1));
                step_ = 99;
                return;
            }
            w_.sound.play_sfx(kSfxCast);
            w_.bark(1, "Rel Sanct Ylem");
            wait_for(w_.do_anim(1, kActCast1, -1));
            return;
        case 1: wait_for(w_.target_pick()); return;  // wakes with the clicked object (0: cancelled)
        case 2: {
            const uint16_t t = uint16_t(result);
            if (!t) { wait_for(w_.do_anim(1, kActCast2, -1)); step_ = 99; return; }  // cancelled: nothing spent
            if (!friendly(w_, t)) {
                w_.sound.play_sfx(kSfxFizzle);
                w_.bark(1, w_.actor(t) ? "Not on a foe!" : "That has no flesh to harden.");
                wait_for(w_.do_anim(1, kActCast2, -1));
                step_ = 99;
                return;
            }
            target_ = t;
            av->mana = int16_t(av->mana - kMana);
            w_.train_stat(1, 5 + w_.rand_below(6));  // INT practice U[5,10] (leveling_facts.md 1.1)
            w_.destroy(token_);
            if (learn_ && w_.spellbook) w_.spellbook->learn(Spellbook::kNecromancy * 1000 + int(kRockFleshFrame));
            wait_for(w_.do_anim(1, kActCast3, -1));
            return;
        }
        case 3:
            if (const Actor* t = w_.actor(target_))
                w_.kernel().add(std::make_unique<SpriteProcess>(w_, kDust, kDustFirst, kDustLast, 1, 1, t->x, t->y, t->z));
            w_.sound.play_sfx(kSfxStone);
            apply(w_, target_);
            wait_for(w_.do_anim(1, kActCast2, -1));
            return;
        default: result = 0; terminate(); return;
        }
    }

private:
    World& w_;
    uint16_t token_, target_ = 0;
    bool learn_;
    int step_ = 0;
};

std::array<uint8_t, 256> g_stone{};
const Palette* g_stone_pal = nullptr;

void end(World& w, uint16_t id, const StoneSkin& s) {
    if (Actor* a = w.actor(id); a && !s.had_immortal) a->set(Actor::kImmortal, false);
    if (w.actor(id)) w.sound.play_sfx(kSfxStone);
    PG_LOG("rock flesh: actor %u is flesh again", id);
}
}  // namespace

uint16_t on_use(World& w, uint16_t token) {
    const Item* it = w.item(token);
    if (!original_bug_fixes() || !it || it->shape != kToken || it->frame != kRockFleshFrame) return 0;
    const Actor* av = w.avatar();
    if (!av || av->is(Actor::kDead)) return 0;
    const bool learn = !(w.spellbook && w.spellbook->internal_use);  // a cast from the book (or a debug cast) learns nothing
    return w.kernel().add(std::make_unique<RockFleshProcess>(w, token, learn));
}

bool apply(World& w, uint16_t target) {
    Actor* a = w.actor(target);
    if (!a || !friendly(w, target)) return false;
    auto found = w.stone_skin.find(target);
    StoneSkin s;
    s.had_immortal = found != w.stone_skin.end() ? found->second.had_immortal : a->is(Actor::kImmortal);
    s.frames = kFrames;
    s.turns = kTurns;
    s.last_round = w.turn && w.turn->active() ? w.turn->round() : -1;
    w.stone_skin[target] = s;
    a->set(Actor::kImmortal, true);  // no HP lost from hits (magic.md 7.7), the original's effect
    a->set(Actor::kInvincible, false);
    PG_LOG("rock flesh: actor %u turned to stone (%d frames / %d rounds)", target, kFrames, kTurns);
    return true;
}

bool active(const World& w, uint16_t actor) { return w.stone_skin.count(actor) > 0; }

void frame(World& w) {
    if (w.stone_skin.empty()) return;
    const bool turn = w.turn && w.turn->active();
    for (auto it = w.stone_skin.begin(); it != w.stone_skin.end();) {
        StoneSkin& s = it->second;
        const Actor* a = w.actor(it->first);
        if (turn) {
            const int r = w.turn->round();
            if (s.last_round < 0) s.last_round = r;
            else if (r != s.last_round) { s.turns -= r - s.last_round; s.last_round = r; }
        } else {
            --s.frames;
            s.last_round = -1;
        }
        if (!a || a->is(Actor::kDead) || s.frames <= 0 || s.turns <= 0) {
            end(w, it->first, s);
            it = w.stone_skin.erase(it);
        } else {
            ++it;
        }
    }
}

const uint8_t* remap(World& w, uint16_t actor) {
    if (w.stone_skin.empty() || !w.stone_skin.count(actor)) return nullptr;
    const Palette& pal = w.data().palette;
    if (g_stone_pal != &pal) {
        // Each colour to the nearest grey of a slightly lighter brightness: weathered stone.
        for (int i = 0; i < 256; ++i) {
            const int l = std::min(255, (pal.rgb8[size_t(i)][0] * 3 + pal.rgb8[size_t(i)][1] * 6 + pal.rgb8[size_t(i)][2]) / 10 + 20);
            int best = i, bd = 1 << 30;
            for (int k = 16; k < 255; ++k) {
                const int r = pal.rgb8[size_t(k)][0], g = pal.rgb8[size_t(k)][1], b = pal.rgb8[size_t(k)][2];
                const int sat = std::max({r, g, b}) - std::min({r, g, b});
                const int d = (r - l) * (r - l) + (g - l) * (g - l) + (b - l) * (b - l) + sat * sat * 4;
                if (d < bd) { bd = d; best = k; }
            }
            g_stone[size_t(i)] = uint8_t(i == 255 ? 255 : best);
        }
        g_stone_pal = &pal;
    }
    return g_stone.data();
}

void save(const World& w, SaveWriter& o) {
    if (w.stone_skin.empty()) return;
    o.tag("RKFL");
    o.u8(1);
    o.u32(uint32_t(w.stone_skin.size()));
    for (const auto& [id, s] : w.stone_skin) {
        o.u16(id);
        o.i32(s.frames);
        o.i32(s.turns);
        o.u8(s.had_immortal ? 1 : 0);
    }
}

void load(World& w, SaveReader& in) {
    w.stone_skin.clear();
    if (!in.peek_tag("RKFL") || !in.tag("RKFL") || in.u8() != 1) return;
    const uint32_t n = std::min<uint32_t>(in.u32(), 256);
    for (uint32_t i = 0; i < n && !in.fail(); ++i) {
        StoneSkin s;
        const uint16_t id = in.u16();
        s.frames = in.i32();
        s.turns = in.i32();
        s.had_immortal = in.u8() != 0;
        w.stone_skin[id] = s;
    }
}

}  // namespace rock_flesh
}  // namespace pg
