// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once

namespace pg {

// Game-variant switches (NOTES.md "v2.12 features" and "Restored content"). Like
// original_bug_fixes() (core/bugfix.h) these are process-wide flags that data-only code can
// read; the option rows are registered by register_variant_options() (game/variants.h).

// "v2.12 changes: On/Off" (Game group, default On; Config key "v212"): behaviour of the
// official v2.12 patch that differs from the v2.10 game we run (specs/world/v212_features.md):
// the targeted standing jump, the recall item's looser map rule, automatic stacking of reagents
// and other stacks put into a container. Pure additions (the F7 / F8 / B / R / K keys) are
// always there.
inline bool& v212_changes() {
    static bool on = true;
    return on;
}

// "Restored content: Undead invasion" (Game group, default On; Config key "restore_invasion"):
// the cut invasion of Tenebrae (specs/world/cut_undead_invasion.md), switched on by G[58:1]
// once the Heart of Earth is in the Avatar's inventory (game/invasion.cpp).
inline bool& restored_invasion() {
    static bool on = true;
    return on;
}

// "Underworld entrance: On/Off" (Game group, default On; Config key "underworld_entrance"): the
// buried cave mouth in the Pit of the Dead, the first piece of the underworld (NOTES.md "The buried
// cave mouth"). Off: map 57 loads as the original data plus the documented fixes.
inline bool& underworld_entrance() {
    static bool on = true;
    return on;
}

// The Breach is open: the rubble is gone. Nothing sets this yet; the Skull's Stirring power will,
// and then call underworld::sync_entrance (game/variants.h).
inline bool& underworld_breach_open() {
    static bool open = false;
    return open;
}

}  // namespace pg
