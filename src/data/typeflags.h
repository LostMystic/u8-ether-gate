// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "core/bugfix.h"
#include "core/bytes.h"

namespace pg {

// Shape families (TYPEFLAG byte 1 high nibble, specs/formats/typeflags.md).
enum Family : uint8_t {
    kFamGeneric = 0, kFamQuality = 1, kFamQuantity = 2, kFamGlobEgg = 3, kFamUsecodeEgg = 4,
    kFamBreakable = 5, kFamContainer = 6, kFamMonsterEgg = 7, kFamTeleportEgg = 8, kFamReagent = 9,
};

// One 8-byte TYPEFLAG.DAT record. The raw bytes are kept so usecode can read any bit by
// number (bit n = bit n%8 of byte n/8).
struct ShapeInfo {
    std::array<uint8_t, 8> raw{};

    // Combined 16-bit flag word (byte 0 | byte 1 low nibble << 8 | byte 5 high nibble << 12),
    // using the numbering of specs/presentation/rendering.md (SOLID 0x0002, DRAW 0x0100 ...).
    enum : uint16_t {
        kFixed = 0x0001, kSolid = 0x0002, kSea = 0x0004, kLand = 0x0008, kOccl = 0x0010,
        kBag = 0x0020, kDamaging = 0x0040, kNoisy = 0x0080, kDraw = 0x0100, kIgnore = 0x0200,
        kRoof = 0x0400, kTransl = 0x0800, kEditor = 0x1000, kExplode = 0x2000,
    };
    uint16_t flags() const { return uint16_t(raw[0] | ((raw[1] & 0x0F) << 8) | ((raw[5] & 0xF0) << 8)); }
    bool is(uint16_t f) const { return (flags() & f) != 0; }

    unsigned family() const { return raw[1] >> 4; }
    unsigned equip_type() const { return raw[2] & 0x0F; }
    unsigned x() const { return raw[2] >> 4; }  // footprint, 32-unit steps
    unsigned y() const { return raw[3] & 0x0F; }
    unsigned z() const { return raw[3] >> 4; }  // 8-unit steps
    unsigned anim_type() const { return raw[4] & 0x0F; }
    unsigned anim_data() const { return raw[4] >> 4; }
    unsigned anim_speed() const { return raw[5] & 0x0F; }
    unsigned weight() const { return raw[6]; }
    unsigned volume() const { return raw[7]; }
    bool bit(unsigned n) const { return n < 64 && (raw[n / 8] >> (n % 8)) & 1; }

    // Footprint in world units; x and y swap for flipped items.
    void footprint(bool flipped, int& xd, int& yd, int& zd) const {
        xd = int(flipped ? y() : x()) * 32;
        yd = int(flipped ? x() : y()) * 32;
        zd = int(z()) * 8;
    }
};

class TypeFlags {
public:
    bool load(const Bytes& file);
    unsigned count() const { return unsigned(info_.size()); }
    // With the "Original bug fixes" option on (core/bugfix.h) the shapes the data has wrong come
    // from the corrected copy (see fix_records()); everything else is the file's record.
    const ShapeInfo& get(unsigned shape) const {
        if (shape >= info_.size()) return empty_;
        return original_bug_fixes() && shape < fixed_.size() ? fixed_[shape] : info_[shape];
    }
    // The record exactly as TYPEFLAG.DAT has it (whatever the option says).
    const ShapeInfo& original(unsigned shape) const { return shape < info_.size() ? info_[shape] : empty_; }
    // This copy's file isn't the one the data fixes were made for: use its records as they are.
    void drop_fixes() { no_fixes_ = true; fix_records(); }
    // A custom shape takes another shape's record (data/shapes/).
    void copy_shape(unsigned from, unsigned to) {
        ShapeInfo i = get(from);
        if (to >= info_.size()) info_.resize(to + 1);
        info_[to] = i;
        fix_records();
    }

private:
    void fix_records();
    std::vector<ShapeInfo> info_;
    std::vector<ShapeInfo> fixed_;  // info_ with the data fixes (specs/formats/typeflags.md)
    ShapeInfo empty_;
    bool no_fixes_ = false;
};

}  // namespace pg
