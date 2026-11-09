// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Small hashes for telling copies of the game data apart (data/compat.h): SHA-1 (hex, the same
// value `sha1sum` prints, so a player can report it) and CRC-32. Neither is used for security.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace pg {

inline std::string sha1_hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    size_t at = 0;
    for (; at + 64 <= size; at += 64) block(data + at);
    uint8_t tail[128] = {0};
    size_t rest = size - at;
    if (rest) std::memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    size_t len = rest < 56 ? 64 : 128;
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[len - 1 - size_t(i)] = uint8_t(bits >> (8 * i));
    block(tail);
    if (len == 128) block(tail + 64);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h)
        for (int i = 28; i >= 0; i -= 4) out += hex[(v >> i) & 15];
    return out;
}

inline uint32_t crc32_of(const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace pg
