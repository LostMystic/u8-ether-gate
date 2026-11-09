// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pg {

// Little-endian binary writer for the engine's own savegames.
class SaveWriter {
public:
    void u8(uint32_t v) { buf_.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void b(bool v) { u8(v ? 1 : 0); }
    void str(const std::string& s) { u32(uint32_t(s.size())); buf_.insert(buf_.end(), s.begin(), s.end()); }
    void bytes(const void* p, size_t n) {
        u32(uint32_t(n));
        const uint8_t* q = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), q, q + n);
    }
    void raw(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), q, q + n);
    }
    // A section tag (four characters), checked by the reader to catch format drift early.
    void tag(const char* t) { raw(t, 4); }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// The matching reader. Reading past the end or a wrong tag sets fail() and returns zeros.
class SaveReader {
public:
    SaveReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint32_t u8() { if (pos_ + 1 > n_) { bad_ = true; return 0; } return p_[pos_++]; }
    uint32_t u16() { uint32_t a = u8(); return a | (u8() << 8); }
    uint32_t u32() { uint32_t a = u16(); return a | (u16() << 16); }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() { uint64_t a = u32(); return a | (uint64_t(u32()) << 32); }
    bool b() { return u8() != 0; }
    std::string str() {
        uint32_t n = u32();
        if (bad_ || pos_ + n > n_) { bad_ = true; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), n);
        pos_ += n;
        return s;
    }
    std::vector<uint8_t> bytes() {
        uint32_t n = u32();
        if (bad_ || pos_ + n > n_) { bad_ = true; return {}; }
        std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + n);
        pos_ += n;
        return v;
    }
    void raw(void* out, size_t n) {
        if (pos_ + n > n_) { bad_ = true; std::memset(out, 0, n); return; }
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
    }
    bool tag(const char* t) {
        char got[4];
        raw(got, 4);
        if (std::memcmp(got, t, 4) != 0) bad_ = true;
        return !bad_;
    }
    // The next tag is t (nothing is read): for optional trailing sections.
    bool peek_tag(const char* t) const { return !bad_ && pos_ + 4 <= n_ && std::memcmp(p_ + pos_, t, 4) == 0; }
    bool fail() const { return bad_; }
    void set_fail() { bad_ = true; }
    size_t pos() const { return pos_; }
    size_t size() const { return n_; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool bad_ = false;
};

}  // namespace pg
