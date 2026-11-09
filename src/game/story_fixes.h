// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once

#include "core/bugfix.h"
#include "data/compat.h"

namespace pg {

// Engine-side fixes of bugs in the original story scripts (NOTES.md "Original bug fixes"; the
// data-driven ones are in usecode/uc_patches.h). All of them only while the option is On.

// Vividos' reagents (specs/world/bugfixes.md 3): the dead man's elbow he asks for is shape 395
// frame 20, but frames 16-20 all merge as "wooden sticks" and a merged stack keeps the frame of
// the stack dropped on, so his elbow could vanish into a stick pile. With the fix frame 20 is a
// kind of its own (merges only with frame 20). Call for two shape-395 frames of the 16-20 group.
inline bool necro_elbow_apart(unsigned shape, unsigned frame_a, unsigned frame_b) {
    return original_bug_fixes() && compat::usecode_known() && shape == 395 && (frame_a == 20) != (frame_b == 20);
}

}  // namespace pg
