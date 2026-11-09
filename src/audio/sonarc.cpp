// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Sonarc sample decoding, written from specs/formats/sound.md.
#include "audio/sonarc.h"

namespace pg {

namespace {

// Little-endian bit reader over one frame's residual bytes; missing bits read as 0.
struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t bits = 0;
    int held = 0;
    void fill() {
        while (held <= 24 && p < end) {
            bits |= uint32_t(*p++) << held;
            held += 8;
        }
    }
    uint32_t peek(int n) {
        fill();
        return bits & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
    }
    void skip(int n) {
        fill();
        if (n >= held) { bits = 0; held = 0; return; }  // ran out: the rest reads as zeros
        bits >>= n;
        held -= n;
    }
    uint32_t read(int n) {
        uint32_t v = peek(n);
        skip(n);
        return v;
    }
};

// Residuals of one frame (sound.md step 1).
void decode_residuals(BitReader& br, int mode, int count, int8_t* out) {
    int m = mode;
    bool zero_flag = false;
    if (m >= 7) { m -= 7; zero_flag = true; }
    for (int i = 0; i < count; ++i) {
        if (zero_flag && br.read(1) == 0) { out[i] = 0; continue; }
        uint32_t look = br.peek(8);
        int k = 0;
        while (k < 8 && (look >> k & 1)) ++k;
        int v;
        if (k == 0) {
            br.skip(1);
            uint32_t u = br.read(m + 1);
            v = (u & (1u << m)) ? int(u) - (1 << (m + 1)) : int(u);
        } else if (k < 7 - m) {
            br.skip(k + 1);
            int n = m + k;
            uint32_t u = br.read(n);
            v = (u >> (n - 1) & 1) ? int(u) : int(u) - (1 << n);
        } else {
            br.skip(7 - m);
            uint32_t u = br.read(7);
            v = (u & 0x40) ? int(u) : int(u) - 128;
        }
        out[i] = int8_t(v);
    }
}

}  // namespace

bool decode_sonarc(ByteView file, Sample& out, int* bad_frames) {
    out = Sample{};
    if (bad_frames) *bad_frames = 0;
    if (file.size < 0x20) return false;
    const uint8_t* d = file.data;
    uint32_t total = rd32(d);
    out.rate = rd16(d + 4);
    out.loop_start = rd32(d + 8);
    out.loop_len = rd32(d + 12);
    if (out.rate <= 0 || total > 16u * 1024 * 1024) return false;
    size_t pos = 0x20;
    if (file.size >= 0x22 && rd16(d + 0x20) == 0x20 && total > 32767) pos = 0x120;
    out.pcm.reserve(total);
    int8_t resid[65536];
    int8_t smp[65536 + 32];
    while (out.pcm.size() < total && pos + 8 <= file.size) {
        const uint8_t* f = d + pos;
        unsigned fsize = rd16(f);
        unsigned count = rd16(f + 2);
        if (fsize < 8 || pos + fsize > file.size) break;
        uint16_t sum = 0;
        for (unsigned k = 0; k + 1 < fsize; k += 2) sum ^= rd16(f + k);
        int mode = int(f[6]) - 8;
        int order = f[7];
        bool ok = sum == 0xACED && mode >= 0 && mode <= 13 && 8u + 2u * unsigned(order) <= fsize;
        if (!ok) {
            if (bad_frames) ++*bad_frames;
            for (unsigned k = 0; k < count && out.pcm.size() < total; ++k) out.pcm.push_back(128);
            pos += fsize;
            continue;
        }
        int16_t coef[256];
        for (int j = 0; j < order; ++j) coef[j] = rds16(f + 8 + 2 * j);
        BitReader br{f + 8 + 2 * order, f + fsize};
        decode_residuals(br, mode, int(count), resid);
        // Step 2: linear prediction; samples before the frame count as -128 (unsigned 0).
        for (int k = 0; k < 16; ++k) smp[k] = -128;
        int8_t* s = smp + 16;
        for (unsigned i = 0; i < count; ++i) {
            int acc = 2048;
            for (int j = 0; j < order; ++j) {
                int idx = int(i) - 1 - j;
                acc += coef[j] * (idx < 0 ? (idx >= -16 ? s[idx] : -128) : s[idx]);
            }
            int8_t pred = int8_t(uint8_t((acc >> 12) & 0xFF));
            s[i] = int8_t(uint8_t(resid[i] - pred));
        }
        // Step 3: clip repair on the unsigned output.
        size_t base = out.pcm.size();
        for (unsigned i = 0; i < count && out.pcm.size() < total; ++i) out.pcm.push_back(uint8_t(s[i] + 128));
        for (size_t i = base + 1; i < out.pcm.size(); ++i)
            if (out.pcm[i] == 0 && out.pcm[i - 1] > 192) out.pcm[i] = 255;
        pos += fsize;
    }
    // A short file: pad to the header's length so loop points stay valid.
    if (out.pcm.size() < total && out.pcm.size() + 4096 >= total) out.pcm.resize(total, 128);
    return true;
}

}  // namespace pg
