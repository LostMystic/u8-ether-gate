// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

class UsecodeFile;

// The story patch layer (NOTES.md "Original bug fixes"): fixes for bugs in the original game's
// scripts and text, kept in data files (engine/data/patches/*.json) and applied at run time, so
// the original EUSECODE.FLX is never rewritten. Everything here is active only while the
// "Original bug fixes" option is On (core/bugfix.h); Off runs the scripts exactly as shipped.
//
//   usecode.json  {"patches": [{"id", "bug", "class", "at", "expect", "replace", "why"}]}
//     Same-length byte patches of one class's code: at code address `at` (hex string or number,
//     the address the disassembly shows) the bytes `expect` (hex) are replaced by `replace`.
//     A patch whose expected bytes are not there (other data) is refused and reported.
//     {"id", "bug", "class", "at", "append", "why"} instead adds new code at the end of the
//     class (`at` must be the class's code size at that point: the address jumps are made for),
//     for a fix that needs more room: a same-length patch then jumps there and back.
//   text.json     {"replace": [{"find", "replace", "word" (default true), "bug"}]}
//     Text corrections applied to every string a script creates (push.str: barks, books,
//     scrolls, conversation answers alike, so menus and their comparisons stay consistent).
//     word = true: only whole words are replaced (letters / digits / ' around it end a word).
struct StoryPatches {
    struct Bytes {
        std::string id, bug, why;
        unsigned cls = 0, at = 0;
        std::vector<uint8_t> expect, replace;  // append: the new code is in `replace`
        bool append = false;
        // "option": "v212" = active with the "v2.12 changes" option (core/variants.h) instead
        // of "Original bug fixes". All patches of one class must share their option.
        std::string option;
    };
    struct Text {
        std::string find, replace, bug;
        bool word = true;
    };
    std::vector<Bytes> bytes;
    std::vector<Text> texts;

    // Reads usecode.json and text.json from `dir` (a missing file is fine). False + err on bad JSON.
    bool load_dir(const std::string& dir, std::string& err);
    bool load_usecode_json(const std::string& text, std::string& err);
    bool load_text_json(const std::string& text, std::string& err);
    // Installs the byte patches into the usecode file (as an overlay used while the option is
    // On). A fix goes in whole or not at all: patches sharing a class or a bug stand or fall
    // together. Returns the number installed; `problems` lists refused ones.
    int install(UsecodeFile& uc, std::vector<std::string>& problems) const;
    // Drops the corrections whose words aren't in this usecode; returns how many (another copy).
    int keep_texts_found_in(const UsecodeFile& uc);
    // The text with every correction applied (no change while the option is Off).
    std::string fix_text(const std::string& s) const;
};

// The patches the game uses (loaded by main / the tests).
StoryPatches& story_patches();

// Convenience: loads `dir` into story_patches() and installs it into `uc`; logs problems.
bool load_story_patches(UsecodeFile& uc, const std::string& dir, std::string& err);

}  // namespace pg
