// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "audio/wav.h"

#include <cstdio>

namespace pg {

bool write_wav(const std::string& path, const int16_t* pcm, size_t frames, int channels, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [&](uint32_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    uint32_t data = uint32_t(frames * size_t(channels) * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(uint32_t(channels)); u32(uint32_t(rate)); u32(uint32_t(rate * channels * 2));
    u16(uint32_t(channels * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (size_t i = 0; i < frames * size_t(channels); ++i) u16(uint16_t(pcm[i]));
    return std::fclose(f) == 0;
}

}  // namespace pg
