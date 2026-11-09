// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Banish Daemon (sorcery spell 11, specs/world/magic.md 3.2, leveling_facts.md 1.3). The spell
// itself is usecode: class 1417 DAEMSPEL from 0x0AD8 picks a target and, on a daemon, plays a fire
// burst and destroys it (intrinsic 0x03A). The engine only watches for that destroy:
//
// - A daemon the Avatar banishes pays the normal kill XP through Leveling::on_kill (before it is
//   destroyed), as if he had slain it (PLAN-VIRTUES.md 22: "full XP").
// - Virtues: each one pays kBanishVirtue Spirituality, up to kBanishVirtueCap daemons a game
//   (keys "banish/1" .. "banish/N"), with a line in the message log.
// - A daemon the Avatar summoned himself (1417 from 0x0080, process item 1) is marked as already
//   paid (kExtXpPaid), so summoning and banishing your own pays neither XP nor virtue.
#include <cstdint>

namespace pg {

class World;

constexpr int kBanishVirtue = 2, kBanishVirtueCap = 5;
constexpr unsigned kDaemonSpellClass = 1417;
constexpr unsigned kSummonDaemonEntry = 0x0080, kBanishDaemonEntry = 0x0AD8, kConflagrationEntry = 0x122E;

// From intrinsic 0x0AD npc_create: the caller's class, function start and process item.
void on_npc_created(World& w, uint16_t id, unsigned cls, unsigned start_addr, uint16_t caster);
// From intrinsic 0x03A item_destroy, before the destroy.
void on_item_destroyed(World& w, uint16_t id, unsigned cls, unsigned ip, uint16_t caster);

}  // namespace pg
