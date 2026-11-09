// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The story checkpoints (debug menu "Jump to story chapter...", NOTES.md "Story jump"): the saves the
// full-story replay (tests/test_story.cpp, ctest `story`) writes after each chapter, packed into one
// file, data/story/checkpoints.pgs. Saves of one game share most of their bytes, so every entry but
// the first is stored as a delta against the one before it (copies from it and new bytes), and each
// entry's delta stream is deflated: about 0.8 MB for twelve saves of 0.5 MB each.
//
// File: "PGSTORY1", u32 count, then per entry: str name, u32 raw size, i32 base (the entry it is a
// delta against, -1 = none), u32 crc of the raw save, bytes (zlib of the delta stream, or of the
// whole save when base is -1). Delta stream: ops until the raw size is reached, each a u8 kind:
// 0 = literal (u32 n, n bytes), 1 = copy (u32 offset in the base, u32 n).
// Game-derived data: never part of a public build (tools/web_pack.py, tools/make_release.sh).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pg {

class StoryPack {
public:
    struct Entry {
        std::string name;
        uint32_t raw_size = 0, crc = 0;
        int base = -1;
        std::vector<uint8_t> z;  // the deflated stream
    };
    std::vector<Entry> entries;

    bool load_file(const std::string& path, std::string& err);
    bool parse(const std::vector<uint8_t>& bytes, std::string& err);
    int find(const std::string& name) const;
    // The save of entry i (decoding the chain of bases before it; cached). False: damaged.
    bool get(int i, std::vector<uint8_t>& out, std::string& err);

    // --- Writing (the story replay): deflate is passed in (stb's writer lives with the screenshots).
    using Deflate = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)>;
    // Builds the pack from named saves in order; each a delta against the one before.
    static std::vector<uint8_t> build(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& saves, const Deflate& deflate);
    // The delta stream of target against base, and its inverse (tests).
    static std::vector<uint8_t> delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& target);
    static bool apply(const std::vector<uint8_t>& base, const std::vector<uint8_t>& delta, uint32_t raw_size, std::vector<uint8_t>& out);
    static uint32_t crc32(const uint8_t* p, size_t n);

private:
    std::vector<std::vector<uint8_t>> cache_;
};

}  // namespace pg
