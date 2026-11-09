// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The Grant Peace spell, intrinsic 0x0B8 (specs/world/magic.md 7.2): cast animations, a target
// cursor, then every undead near the Avatar is laid to rest (or, on a living target, a 1 in 10
// chance of death). Kills pay XP through Leveling::on_kill like any kill by the Avatar.
//
// Virtues (PLAN-VIRTUES.md 3.10): each undead laid to rest pays 1 Spirituality, up to
// kPeaceVirtueCap from this spell in a game (keys "peace/1" .. "peace/N").
#include <cstdint>

namespace pg {

class World;

constexpr int kPeaceVirtueCap = 20;

// Starts the spell; returns the pid of its process (usecode may wait on it).
uint16_t start_grant_peace(World& w);

}  // namespace pg
