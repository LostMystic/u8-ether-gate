// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "data/flex.h"

namespace pg {

// USECODE/EUSECODE.FLX (specs/formats/usecode_file.md): class c is Flex entry c + 2, the
// name / event-mask table is entry 1. Code address A of a class is object byte 0x0C + A.
class UsecodeFile {
public:
    static constexpr unsigned kClasses = 1431;
    static constexpr unsigned kEvents = 32;

    bool load(Bytes file);
    const std::string& error() const { return error_; }

    unsigned class_count() const { return kClasses; }
    bool has_class(unsigned c) const { return c < kClasses && code_size(c) > 0x80; }
    // Code space of class c (starting at object byte 0x0C); nullptr if missing.
    const uint8_t* code(unsigned c) const;
    uint32_t code_size(unsigned c) const;
    // Entry point of event e (0 = not handled).
    uint32_t event_entry(unsigned c, unsigned e) const;
    uint32_t event_mask(unsigned c) const;
    std::string class_name(unsigned c) const;
    unsigned existing_classes() const;
    // The file as loaded (no patches). Whether any class's bytes hold `text`.
    const Flex& flex() const { return flex_; }
    bool contains_text(const std::string& text) const;

    // Story patch overlay (usecode/uc_patches.h): a patched copy of a class's code space, used
    // by code() instead of the original while its option is On: gate kGateBugFixes = "Original
    // bug fixes" (core/bugfix.h), kGateV212 = "v2.12 changes" (core/variants.h).
    enum Gate : uint8_t { kGateBugFixes = 0, kGateV212 = 1 };
    void set_patched_code(unsigned c, std::vector<uint8_t> code, Gate gate = kGateBugFixes);
    const std::vector<uint8_t>& patched_code(unsigned c) const;
    Gate patch_gate(unsigned c) const { return c < gates_.size() ? Gate(gates_[c]) : kGateBugFixes; }
    bool patch_active(unsigned c) const;
    void clear_patches() { patched_.clear(); gates_.clear(); }

private:
    Flex flex_;
    std::vector<std::vector<uint8_t>> patched_;
    std::vector<uint8_t> gates_;
    std::string error_;
};

}  // namespace pg
