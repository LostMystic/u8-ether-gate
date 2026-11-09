// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "usecode/usecode_file.h"

#include <algorithm>

#include "core/bugfix.h"
#include "core/variants.h"

namespace pg {

bool UsecodeFile::load(Bytes file) {
    if (!flex_.load(std::move(file))) { error_ = flex_.error(); return false; }
    if (flex_.count() < kClasses + 2) { error_ = "too few entries"; return false; }
    if (flex_.entry(1).size < kClasses * 13) { error_ = "short name table"; return false; }
    return true;
}

const uint8_t* UsecodeFile::code(unsigned c) const {
    if (c >= kClasses) return nullptr;
    if (patch_active(c)) return patched_[c].data();
    ByteView v = flex_.entry(c + 2);
    return v.size > 0x0C ? v.data + 0x0C : nullptr;
}

uint32_t UsecodeFile::code_size(unsigned c) const {
    if (c >= kClasses) return 0;
    if (patch_active(c)) return uint32_t(patched_[c].size());
    ByteView v = flex_.entry(c + 2);
    return v.size > 0x0C ? uint32_t(v.size - 0x0C) : 0;
}

uint32_t UsecodeFile::event_entry(unsigned c, unsigned e) const {
    if (e >= kEvents || code_size(c) < 0x80) return 0;
    return rd32(code(c) + 4 * e);
}

uint32_t UsecodeFile::event_mask(unsigned c) const {
    if (c >= kClasses) return 0;
    return rd32(flex_.entry(1).data + 13 * c);
}

bool UsecodeFile::contains_text(const std::string& text) const {
    if (text.empty()) return false;
    for (unsigned i = 0; i < flex_.count(); ++i) {
        ByteView e = flex_.entry(i);
        const uint8_t* b = e.data;
        if (std::search(b, b + e.size, text.begin(), text.end()) != b + e.size) return true;
    }
    return false;
}

std::string UsecodeFile::class_name(unsigned c) const {
    if (c >= kClasses) return {};
    const char* p = reinterpret_cast<const char*>(flex_.entry(1).data + 13 * c + 4);
    std::string s;
    for (int i = 0; i < 9 && p[i]; ++i) s += p[i];
    return s;
}

bool UsecodeFile::patch_active(unsigned c) const {
    if (c >= patched_.size() || patched_[c].empty()) return false;
    return patch_gate(c) == kGateV212 ? v212_changes() : original_bug_fixes();
}

void UsecodeFile::set_patched_code(unsigned c, std::vector<uint8_t> code, Gate gate) {
    if (c >= kClasses) return;
    if (patched_.size() <= c) patched_.resize(c + 1);
    if (gates_.size() <= c) gates_.resize(c + 1, kGateBugFixes);
    patched_[c] = std::move(code);
    gates_[c] = gate;
}

const std::vector<uint8_t>& UsecodeFile::patched_code(unsigned c) const {
    static const std::vector<uint8_t> none;
    return c < patched_.size() ? patched_[c] : none;
}

unsigned UsecodeFile::existing_classes() const {
    unsigned n = 0;
    for (unsigned c = 0; c < kClasses; ++c) n += code_size(c) > 0 ? 1 : 0;
    return n;
}

}  // namespace pg
