// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/anim.h"

namespace pg {

bool AnimDat::load(const Bytes& f) {
    shapes_.clear();
    if (f.size() < 2048 * 4) return false;
    shapes_.resize(2048);
    for (unsigned s = 0; s < 2048; ++s) {
        uint32_t table = rd32(&f[4 * s]);
        if (!table) continue;
        if (table + kActions * 4 > f.size()) return false;
        auto actions = std::make_unique<Actions>();
        for (unsigned a = 0; a < kActions; ++a) {
            uint32_t off = rd32(&f[table + 4 * a]);
            if (!off) continue;
            if (off + 4 > f.size()) return false;
            auto seq = std::make_unique<AnimSequence>();
            seq->length = f[off];
            seq->flags_a = f[off + 1];
            seq->repeat = f[off + 2] & 0x0F;
            seq->flags_b = f[off + 3];
            size_t n = size_t(seq->length) * 8;
            if (off + 4 + n * 6 > f.size()) return false;
            seq->frames.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* r = &f[off + 4 + 6 * i];
                AnimFrame& fr = seq->frames[i];
                fr.frame = uint16_t(r[0] | ((r[1] & 7) << 8));
                fr.special = r[1] & 0x08;
                fr.unk_bit4 = r[1] & 0x10;
                fr.usecode = r[1] & 0x40;
                fr.dz = int8_t(r[2]);
                fr.sfx = r[3];
                fr.distance = int8_t(r[4]);
                fr.flags_c = r[5];
            }
            (*actions)[a] = std::move(seq);
        }
        shapes_[s] = std::move(actions);
    }
    return true;
}

const AnimSequence* AnimDat::get(unsigned shape, unsigned action) const {
    if (shape >= shapes_.size() || !shapes_[shape] || action >= kActions) return nullptr;
    return (*shapes_[shape])[action].get();
}

bool AnimDat::copy_shape(unsigned from, unsigned to) {
    if (!has_shape(from)) return false;
    auto actions = std::make_unique<Actions>();
    for (unsigned a = 0; a < kActions; ++a)
        if (const AnimSequence* s = get(from, a)) (*actions)[a] = std::make_unique<AnimSequence>(*s);
    if (to >= shapes_.size()) shapes_.resize(to + 1);
    shapes_[to] = std::move(actions);
    return true;
}

unsigned AnimDat::shape_count() const {
    unsigned n = 0;
    for (auto& s : shapes_) n += s ? 1 : 0;
    return n;
}

unsigned AnimDat::sequence_count() const {
    unsigned n = 0;
    for (auto& s : shapes_)
        if (s)
            for (auto& a : *s) n += a ? 1 : 0;
    return n;
}

}  // namespace pg
