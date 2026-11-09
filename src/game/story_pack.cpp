// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The story checkpoints pack: see story_pack.h.
#include "game/story_pack.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/serial.h"
#include "stb/stb_image.h"  // its zlib decoder (the implementation is in data/picture.cpp)

namespace pg {

namespace {
const char kMagic[8] = {'P', 'G', 'S', 'T', 'O', 'R', 'Y', '1'};

bool inflate(const std::vector<uint8_t>& z, uint32_t size_hint, std::vector<uint8_t>& out) {
    int n = 0;
    char* p = stbi_zlib_decode_malloc_guesssize(reinterpret_cast<const char*>(z.data()), int(z.size()),
                                                int(std::max<uint32_t>(size_hint, 1024)), &n);
    if (!p) return false;
    out.assign(reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + n);
    std::free(p);
    return true;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void wr32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
}  // namespace

uint32_t StoryPack::crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool StoryPack::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = path + " not found"; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    return parse(std::vector<uint8_t>(s.begin(), s.end()), err);
}

bool StoryPack::parse(const std::vector<uint8_t>& bytes, std::string& err) {
    entries.clear();
    cache_.clear();
    SaveReader in(bytes.data(), bytes.size());
    char m[8];
    in.raw(m, 8);
    if (in.fail() || std::memcmp(m, kMagic, 8) != 0) { err = "not a story checkpoints pack"; return false; }
    const uint32_t n = std::min<uint32_t>(in.u32(), 256);
    for (uint32_t i = 0; i < n && !in.fail(); ++i) {
        Entry e;
        e.name = in.str();
        e.raw_size = in.u32();
        e.base = in.i32();
        e.crc = in.u32();
        e.z = in.bytes();
        if (e.base >= int(i)) { err = "bad base in entry " + e.name; return false; }
        entries.push_back(std::move(e));
    }
    if (in.fail()) { err = "truncated story checkpoints pack"; entries.clear(); return false; }
    cache_.resize(entries.size());
    return true;
}

int StoryPack::find(const std::string& name) const {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].name == name) return int(i);
    return -1;
}

bool StoryPack::get(int i, std::vector<uint8_t>& out, std::string& err) {
    if (i < 0 || size_t(i) >= entries.size()) { err = "no such checkpoint"; return false; }
    cache_.resize(entries.size());
    if (!cache_[size_t(i)].empty()) { out = cache_[size_t(i)]; return true; }
    const Entry& e = entries[size_t(i)];
    std::vector<uint8_t> stream;
    if (!inflate(e.z, e.base < 0 ? e.raw_size : e.raw_size / 4, stream)) { err = "checkpoint " + e.name + " does not inflate"; return false; }
    std::vector<uint8_t> raw;
    if (e.base < 0) {
        raw.swap(stream);
    } else {
        std::vector<uint8_t> base;
        if (!get(e.base, base, err)) return false;
        if (!apply(base, stream, e.raw_size, raw)) { err = "checkpoint " + e.name + ": bad delta"; return false; }
    }
    if (raw.size() != e.raw_size || crc32(raw.data(), raw.size()) != e.crc) { err = "checkpoint " + e.name + ": checksum differs"; return false; }
    // Keep only this one and its base decoded (the next jump is usually near).
    for (size_t k = 0; k < cache_.size(); ++k)
        if (int(k) != e.base) cache_[k].clear();
    cache_[size_t(i)] = raw;
    out.swap(raw);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Writing

std::vector<uint8_t> StoryPack::delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& t) {
    // Greedy matching against the base through a hash of 8 bytes at every base position
    // (the last position with that hash wins, with up to 4 candidates kept per bucket).
    const size_t kKey = 8, kMin = 12, kBits = 20, kWays = 4;
    std::vector<int32_t> table(size_t(1) << kBits << 2, -1);
    auto hash = [&](const uint8_t* p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return size_t((v * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    };
    if (base.size() >= kKey)
        for (size_t j = 0; j + kKey <= base.size(); ++j) {
            int32_t* b = &table[hash(&base[j]) * kWays];
            std::memmove(b + 1, b, (kWays - 1) * sizeof(int32_t));
            b[0] = int32_t(j);
        }
    std::vector<uint8_t> out;
    std::vector<uint8_t> lit;
    auto flush = [&]() {
        if (lit.empty()) return;
        out.push_back(0);
        wr32(out, uint32_t(lit.size()));
        out.insert(out.end(), lit.begin(), lit.end());
        lit.clear();
    };
    size_t i = 0, expect = 0;  // expect: where the last copy ended in the base (a cheap guess)
    while (i < t.size()) {
        size_t best = 0, best_j = 0;
        auto try_at = [&](size_t j) {
            if (j >= base.size()) return;
            size_t n = 0;
            while (i + n < t.size() && j + n < base.size() && t[i + n] == base[j + n]) ++n;
            if (n > best) { best = n; best_j = j; }
        };
        try_at(expect);
        if (best < 64 && i + kKey <= t.size()) {
            const int32_t* b = &table[hash(&t[i]) * kWays];
            for (size_t k = 0; k < kWays; ++k)
                if (b[k] >= 0) try_at(size_t(b[k]));
        }
        if (best >= kMin) {
            flush();
            out.push_back(1);
            wr32(out, uint32_t(best_j));
            wr32(out, uint32_t(best));
            i += best;
            expect = best_j + best;
        } else {
            lit.push_back(t[i]);
            ++i;
            if (expect < base.size()) ++expect;
        }
    }
    flush();
    return out;
}

bool StoryPack::apply(const std::vector<uint8_t>& base, const std::vector<uint8_t>& d, uint32_t raw_size, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(raw_size);
    size_t p = 0;
    while (out.size() < raw_size) {
        if (p + 5 > d.size()) return false;
        const uint8_t kind = d[p];
        const uint32_t a = rd32(&d[p + 1]);
        p += 5;
        if (kind == 0) {
            if (p + a > d.size()) return false;
            out.insert(out.end(), d.begin() + long(p), d.begin() + long(p + a));
            p += a;
        } else if (kind == 1) {
            if (p + 4 > d.size()) return false;
            const uint32_t n = rd32(&d[p]);
            p += 4;
            if (size_t(a) + n > base.size()) return false;
            out.insert(out.end(), base.begin() + long(a), base.begin() + long(a + n));
        } else {
            return false;
        }
    }
    return out.size() == raw_size && p == d.size();
}

std::vector<uint8_t> StoryPack::build(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& saves, const Deflate& deflate) {
    SaveWriter o;
    o.raw(kMagic, 8);
    o.u32(uint32_t(saves.size()));
    for (size_t i = 0; i < saves.size(); ++i) {
        const std::vector<uint8_t>& raw = saves[i].second;
        const int base = i == 0 ? -1 : int(i) - 1;
        std::vector<uint8_t> z = deflate(base < 0 ? raw : delta(saves[size_t(base)].second, raw));
        o.str(saves[i].first);
        o.u32(uint32_t(raw.size()));
        o.i32(base);
        o.u32(crc32(raw.data(), raw.size()));
        o.bytes(z.data(), z.size());
    }
    return o.data();
}

}  // namespace pg
