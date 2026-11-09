// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Banish Daemon: XP and virtue (see banish.h).
#include "game/banish.h"

#include <string>

#include "core/log.h"
#include "game/leveling.h"
#include "game/virtues.h"
#include "game/world.h"

namespace pg {

void on_npc_created(World& w, uint16_t id, unsigned cls, unsigned start_addr, uint16_t caster) {
    if (cls != kDaemonSpellClass || start_addr != kSummonDaemonEntry || caster != 1) return;
    if (Actor* a = w.actor(id)) a->ext_flags |= kExtXpPaid;  // the Avatar's own daemon
}

void on_item_destroyed(World& w, uint16_t id, unsigned cls, unsigned ip, uint16_t caster) {
    if (cls != kDaemonSpellClass || ip < kBanishDaemonEntry || ip >= kConflagrationEntry || caster != 1) return;
    Actor* a = w.actor(id);
    if (!a || id == 1 || a->is(Actor::kDead)) return;
    const bool own = (a->ext_flags & kExtXpPaid) != 0;
    PG_LOG("banish: actor %u (shape %u)%s", id, a->shape, own ? ", the Avatar's own" : "");
    if (own) return;
    w.leveling->on_kill(id, 1);  // full kill XP (and the kill's Valor rule)
    if (!w.virtues || !w.virtues->enabled) return;
    for (int n = 1; n <= kBanishVirtueCap; ++n) {
        const std::string key = "banish/" + std::to_string(n);
        if (w.virtues->has_paid(kSpirituality, key)) continue;
        w.virtues->earn(kSpirituality, kBanishVirtue, key, "The daemon is sent home, not slain. Your Spirituality grows.");
        return;
    }
}

}  // namespace pg
