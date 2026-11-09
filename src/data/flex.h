// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>

#include "core/bytes.h"

namespace pg {

// A Flex archive (specs/formats/flex.md): 0x80-byte header with the entry count at 0x54,
// then a table of {u32 offset, u32 size}. Entries of size 0 do not exist.
// The archive owns the whole file in memory; entries are views into it.
class Flex {
public:
    // Takes ownership of the file bytes. Returns false (with error()) if it isn't a Flex.
    bool load(Bytes file);
    const std::string& error() const { return error_; }

    unsigned count() const { return unsigned(table_.size()); }
    // An entry's bytes; empty if out of range or of size 0.
    ByteView entry(unsigned index) const;
    bool has(unsigned index) const { return !entry(index).empty(); }

private:
    struct Slot { uint32_t offset, size; };
    Bytes file_;
    std::vector<Slot> table_;
    std::string error_;
};

}  // namespace pg
