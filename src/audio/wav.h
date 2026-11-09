// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

// Writes 16-bit PCM (interleaved if channels == 2) as a RIFF WAVE file.
bool write_wav(const std::string& path, const int16_t* pcm, size_t frames, int channels, int rate);

}  // namespace pg
