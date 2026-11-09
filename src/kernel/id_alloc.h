// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "core/serial.h"

namespace pg {

// Hands out 16-bit ids from [first, last] (specs/world/kernel.md section 6): the free list
// starts in ascending order, freed ids go to the back so they are not reused at once, and
// specific ids can be reserved.
class IdAllocator {
public:
    IdAllocator() = default;
    IdAllocator(unsigned first, unsigned last) { reset(first, last); }

    void reset(unsigned first, unsigned last) {
        first_ = first;
        last_ = last;
        used_.assign(last + 1, false);
        free_.clear();
        for (unsigned i = first; i <= last; ++i) free_.push_back(uint16_t(i));
    }
    // 0 if the range is exhausted.
    uint16_t allocate() {
        while (!free_.empty()) {
            uint16_t id = free_.front();
            free_.pop_front();
            if (used_[id]) continue;  // reserved while queued
            used_[id] = true;
            return id;
        }
        return 0;
    }
    // Marks a specific id as used. False if it is out of range or already taken.
    bool reserve(unsigned id) {
        if (id < first_ || id > last_ || used_[id]) return false;
        used_[id] = true;  // stays in the queue; allocate() skips it
        return true;
    }
    void release(unsigned id) {
        if (id < first_ || id > last_ || !used_[id]) return;
        used_[id] = false;
        free_.push_back(uint16_t(id));
    }
    bool used(unsigned id) const { return id >= first_ && id <= last_ && used_[id]; }
    unsigned first() const { return first_; }
    // Savegames: the whole state (free queue order included, so ids repeat exactly).
    void save(SaveWriter& out) const {
        out.u32(first_);
        out.u32(last_);
        // The free queue as runs of consecutive ids (it is mostly one ascending run).
        std::vector<std::pair<uint16_t, uint16_t>> runs;
        for (uint16_t id : free_) {
            if (!runs.empty() && runs.back().first + runs.back().second == id && runs.back().second < 65535) ++runs.back().second;
            else runs.push_back({id, 1});
        }
        out.u32(uint32_t(runs.size()));
        for (auto& r : runs) { out.u16(r.first); out.u16(r.second); }
        std::vector<uint8_t> bits((last_ - first_ + 8) / 8, 0);
        for (unsigned i = first_; i <= last_; ++i)
            if (used_[i]) bits[(i - first_) / 8] |= uint8_t(1u << ((i - first_) % 8));
        out.bytes(bits.data(), bits.size());
    }
    void load(SaveReader& in) {
        unsigned f = in.u32(), l = in.u32();
        if (l < f || l > 65535) { in.set_fail(); return; }
        reset(f, l);
        free_.clear();
        unsigned n = in.u32();
        for (unsigned i = 0; i < n && !in.fail(); ++i) {
            unsigned a = in.u16(), len = in.u16();
            for (unsigned k = 0; k < len; ++k) free_.push_back(uint16_t(a + k));
        }
        std::vector<uint8_t> bits = in.bytes();
        if (bits.size() != (last_ - first_ + 8) / 8) { in.set_fail(); return; }
        for (unsigned i = first_; i <= last_; ++i) used_[i] = (bits[(i - first_) / 8] >> ((i - first_) % 8)) & 1;
    }
    unsigned last() const { return last_; }

private:
    unsigned first_ = 0, last_ = 0;
    std::vector<bool> used_;
    std::deque<uint16_t> free_;
};

}  // namespace pg
