// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/flex.h"

namespace pg {

bool Flex::load(Bytes file) {
    file_ = std::move(file);
    table_.clear();
    error_.clear();
    if (file_.size() < 0x80) { error_ = "file too short for a Flex header"; return false; }
    // Title area: some text, then nothing but 0x1A padding up to 0x52.
    size_t pad = 0;
    while (pad < 0x52 && file_[pad] != 0x1A) ++pad;
    if (pad == 0x52) { error_ = "no 0x1A padding in the title"; return false; }
    for (size_t i = pad; i < 0x52; ++i)
        if (file_[i] != 0x1A) { error_ = "bad title padding"; return false; }
    uint32_t n = rd32(&file_[0x54]);
    if (n > 65536 || 0x80 + size_t(n) * 8 > file_.size()) { error_ = "entry table does not fit"; return false; }
    table_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Slot s{rd32(&file_[0x80 + 8 * i]), rd32(&file_[0x84 + 8 * i])};
        if (s.size && (uint64_t(s.offset) + s.size > file_.size())) { error_ = "entry outside the file"; return false; }
        table_[i] = s;
    }
    return true;
}

ByteView Flex::entry(unsigned index) const {
    if (index >= table_.size() || table_[index].size == 0) return {};
    return {file_.data() + table_[index].offset, table_[index].size};
}

}  // namespace pg
