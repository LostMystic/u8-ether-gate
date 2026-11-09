// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/typeflags.h"

namespace pg {

bool TypeFlags::load(const Bytes& file) {
    if (file.size() < 8) return false;
    info_.resize(file.size() / 8);
    for (size_t i = 0; i < info_.size(); ++i)
        for (int b = 0; b < 8; ++b) info_[i].raw[b] = file[8 * i + b];
    fix_records();
    return true;
}

// Original bug fix: the catacomb moss curtains (shapes 459-464, maps 49-51 and 57) are SOLID in
// TYPEFLAG.DAT, so they block like walls: nothing gets into or out of the alcoves behind them
// (the bodies and bones there cannot be reached, taken or put back). They are hanging moss
// that the player is meant to see through and reach past, so the fixed copy clears SOLID
// (byte 0 bit 1); the shapes stay FIXED and drawn as before.
void TypeFlags::fix_records() {
    fixed_ = info_;
    if (no_fixes_) return;
    for (unsigned s = 459; s <= 464 && s < fixed_.size(); ++s) fixed_[s].raw[0] &= uint8_t(~0x02);
}

}  // namespace pg
