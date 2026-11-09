// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once

namespace pg {

// The "Original bug fixes: On/Off" Game option (default On; Config key "bug_fixes").
// Every fix of a bug in the original game that a purist would notice checks this one switch:
// engine-side behaviour fixes, script (usecode) fixes and the extra map data fixes. The option
// row is registered by World::register_bugfix_option() (game/world.cpp); data-only code (the
// map loader, tools) just reads the flag. Map data fixes take effect at the next map load.
inline bool& original_bug_fixes() {
    static bool on = true;
    return on;
}

}  // namespace pg
