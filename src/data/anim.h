// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/bytes.h"

namespace pg {

// ANIM.DAT (specs/formats/anim.md): per actor shape, 64 actions, each a sequence of
// L frames for each of 8 directions.
struct AnimFrame {
    uint16_t frame = 0;   // shape frame number
    bool special = false; // byte 1 bit 3 (footstep etc.)
    bool unk_bit4 = false;
    bool usecode = false; // byte 1 bit 6
    int8_t dz = 0;
    uint8_t sfx = 0;
    int8_t distance = 0;  // forward step in units of 4 world units per axis
    uint8_t flags_c = 0;
    bool on_ground() const { return flags_c & 0x02; }
    unsigned attack_range() const { return (flags_c >> 2) & 7; }
    bool flipped() const { return flags_c & 0x20; }
};

struct AnimSequence {
    enum : uint8_t { kTwoStep = 0x01, kAttack = 0x02, kLooping = 0x04, kUnstoppable = 0x08,
                     kLooping2 = 0x10, kHanging = 0x80 };
    unsigned length = 0;  // frames per direction
    uint8_t flags_a = 0;
    unsigned repeat = 0;  // each frame lasts repeat + 1 animation ticks
    uint8_t flags_b = 0;  // bit 7: destroy actor at the end
    std::vector<AnimFrame> frames;  // 8 * length, grouped by direction
    const AnimFrame& at(unsigned dir, unsigned i) const { return frames[(dir & 7) * length + i]; }
};

class AnimDat {
public:
    static constexpr unsigned kActions = 64;
    bool load(const Bytes& file);
    // nullptr if the shape has no such action.
    const AnimSequence* get(unsigned shape, unsigned action) const;
    bool has_shape(unsigned shape) const { return shape < shapes_.size() && shapes_[shape]; }
    // A custom shape animates as `from` does: every action copied (data/shapes/).
    bool copy_shape(unsigned from, unsigned to);
    unsigned shape_count() const;     // shapes with animations
    unsigned sequence_count() const;  // non-empty sequences

private:
    using Actions = std::array<std::unique_ptr<AnimSequence>, kActions>;
    std::vector<std::unique_ptr<Actions>> shapes_;
};

}  // namespace pg
