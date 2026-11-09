// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace pg {

// A decoded sound: mono unsigned 8-bit PCM at its own rate.
struct Sample {
    std::vector<uint8_t> pcm;  // unsigned 8-bit, 128 = silence
    int rate = 11111;
    uint32_t loop_start = 0, loop_len = 0;  // from the Sonarc header (uncertain meaning)
    double seconds() const { return rate > 0 ? double(pcm.size()) / rate : 0; }
};

// Sonarc decoding (specs/formats/sound.md): a 32-byte header, then self-contained frames of
// entropy-coded residuals through a 10-tap linear predictor. Returns false on a bad header.
// Frames that fail the 0xACED checksum are skipped. `bad_frames` counts them.
bool decode_sonarc(ByteView file, Sample& out, int* bad_frames = nullptr);

}  // namespace pg
