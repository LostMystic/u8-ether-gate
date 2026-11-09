// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Little-endian readers over a byte buffer. All U8 data is little-endian.
namespace pg {

using Bytes = std::vector<uint8_t>;

inline uint8_t  rd8(const uint8_t* p) { return p[0]; }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline int16_t  rds16(const uint8_t* p) { return int16_t(rd16(p)); }
inline uint32_t rd24(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)) | (uint32_t(p[3]) << 24); }

// A non-owning view of bytes (a Flex entry, a file region).
struct ByteView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool empty() const { return size == 0; }
    const uint8_t* begin() const { return data; }
    const uint8_t* end() const { return data + size; }
};

}  // namespace pg
