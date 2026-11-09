// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/compat.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "core/gamefs.h"
#include "core/log.h"
#include "core/sha1.h"
#include "data/gamedata.h"

namespace pg {
namespace compat {

namespace {

struct KnownFile { const char* path; const char* sha1; };
#include "data/known_copies.inc"

// The files that say which copy this is. Optional files (music, movies ...) don't: a copy without
// its music is still that copy, and nothing below depends on them.
const char* const kCoreFiles[] = {
    "STATIC/U8PAL.PAL", "STATIC/XFORMPAL.DAT", "STATIC/U8SHAPES.FLX", "STATIC/U8GUMPS.FLX", "STATIC/U8FONTS.FLX",
    "STATIC/U8MOUSE.SHP", "STATIC/TYPEFLAG.DAT", "STATIC/ANIM.DAT", "STATIC/GLOB.FLX", "STATIC/FIXED.DAT",
    "GAMEDAT/NONFIXED.DAT", "GAMEDAT/ITEMCACH.DAT", "GAMEDAT/NPCDATA.DAT", "USECODE/EUSECODE.FLX"};

struct State {
    bool checked = false, known = true;
    std::string root, name = "English v2.10";
    std::map<std::string, std::string> sha1;     // of the files as they are in this folder
    std::set<std::string> differ;
    std::vector<std::string> differ_list, offs;
    std::vector<bool> class_ok, fixed_ok, nonfixed_ok;
};

State& state() {
    static State s;
    return s;
}

template <size_t N>
std::vector<bool> compare_entries(const Flex& flex, unsigned first, unsigned count, const uint32_t (&table)[N]) {
    std::vector<bool> ok(count, false);
    for (unsigned i = 0; i < count && i < N; ++i) {
        ByteView e = flex.entry(first + i);
        ok[i] = crc32_of(e.data, e.size) == table[i];
    }
    return ok;
}

std::string hash_file(const GameFS& fs, const std::string& rel) {
    Bytes b;
    if (!fs.read(rel, b)) return "";
    return sha1_hex(b.data(), b.size());
}

}  // namespace

void check(const GameFS& fs, const GameData& d) {
    State& s = state();
    // The usecode may have another language's name: it is the same core file.
    const std::string uc = d.usecode_path.empty() ? std::string("USECODE/EUSECODE.FLX") : d.usecode_path;
    auto real = [&](const std::string& f) { return f == "USECODE/EUSECODE.FLX" ? uc : f; };
    if (!(s.checked && s.root == fs.root() && s.sha1.size() == sizeof kCoreFiles / sizeof kCoreFiles[0])) {
        s.sha1.clear();
        for (const char* f : kCoreFiles) s.sha1[f] = hash_file(fs, real(f));
    }
    s.checked = true;
    s.root = fs.root();
    s.differ.clear();
    s.differ_list.clear();
    s.offs.clear();
    for (const KnownFile& k : kV210Files) {
        auto it = s.sha1.find(k.path);
        if (it == s.sha1.end() || it->second != k.sha1 || real(k.path) != k.path) {
            s.differ.insert(k.path);
            s.differ_list.push_back(real(k.path));
        }
    }
    s.known = s.differ.empty();
    s.name = s.known ? "English v2.10" : "unknown version";
    s.class_ok = compare_entries(d.usecode.flex(), 2, UsecodeFile::kClasses, kV210ClassCrc);
    s.fixed_ok = compare_entries(d.fixed, 0, 256, kV210FixedCrc);
    s.nonfixed_ok = compare_entries(d.nonfixed, 0, 256, kV210NonfixedCrc);
    PG_LOG("game copy: %s", about_line().c_str());
    if (fs.new_game_from_save())
        PG_LOG("game copy: new games start from SAVEGAME/U8SAVE.000%s", fs.played_gamedat() ? " (GAMEDAT/ holds a game played in the DOS original, left alone)" : "");
    if (!s.known) {
        for (const std::string& f : s.differ_list) {
            auto it = s.sha1.find(f);
            if (it == s.sha1.end()) it = s.sha1.find("USECODE/EUSECODE.FLX");
            PG_LOG("game copy: %s differs from English v2.10 (sha1 %s)", f.c_str(), it != s.sha1.end() ? it->second.c_str() : "?");
        }
        unsigned classes = 0, maps = 0;
        for (bool b : s.class_ok) classes += !b;
        for (unsigned m = 0; m < 256; ++m) maps += !(s.fixed_ok[m] && s.nonfixed_ok[m]);
        PG_LOG("game copy: %u usecode classes and %u maps differ; fixes made for them switch themselves off", classes, maps);
    }
}

bool copy_known() { return state().known; }
const std::string& copy_name() { return state().name; }

std::string about_line() {
    const State& s = state();
    if (s.known) return s.name;
    return "unknown version: running anyway";
}

bool file_known(const std::string& rel) { return !state().checked || !state().differ.count(rel); }
bool usecode_known() { return file_known("USECODE/EUSECODE.FLX"); }
bool world_known() { return file_known("STATIC/FIXED.DAT") && file_known("GAMEDAT/NONFIXED.DAT") && file_known("STATIC/GLOB.FLX"); }

bool class_known(unsigned cls) {
    const State& s = state();
    return !s.checked || (cls < s.class_ok.size() && s.class_ok[cls]);
}
bool fixed_map_known(unsigned map) {
    const State& s = state();
    return !s.checked || (map < s.fixed_ok.size() && s.fixed_ok[map]);
}
bool nonfixed_map_known(unsigned map) {
    const State& s = state();
    return !s.checked || (map < s.nonfixed_ok.size() && s.nonfixed_ok[map]);
}

void off(const std::string& what, const std::string& why) {
    State& s = state();
    if (std::find(s.offs.begin(), s.offs.end(), what) != s.offs.end()) return;
    s.offs.push_back(what);
    PG_LOG("switched off: %s (%s)", what.c_str(), why.c_str());
}

const std::vector<std::string>& switched_off() { return state().offs; }
const std::vector<std::string>& differing_files() { return state().differ_list; }

std::string known_table_text(const GameFS& fs, const GameData& d) {
    std::string out = "// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.\n";
    out += "// Hashes (no game data) of the copy our fixes were written for: English v2.10. Made by\n";
    out += "// `pagan --game DIR --write-known-table src/data/known_copies.inc`. See data/compat.h.\n";
    out += "const KnownFile kV210Files[] = {\n";
    for (const char* f : kCoreFiles) out += std::string("    {\"") + f + "\", \"" + hash_file(fs, f) + "\"},\n";
    out += "};\n";
    auto table = [&](const char* name, const Flex& flex, unsigned first, unsigned n) {
        out += std::string("const uint32_t ") + name + "[] = {";
        char buf[16];
        for (unsigned i = 0; i < n; ++i) {
            ByteView e = flex.entry(first + i);
            std::snprintf(buf, sizeof buf, "0x%08Xu,", crc32_of(e.data, e.size));
            out += (i % 10 == 0 ? "\n    " : " ");
            out += buf;
        }
        out += "\n};\n";
    };
    table("kV210ClassCrc", d.usecode.flex(), 2, UsecodeFile::kClasses);
    table("kV210FixedCrc", d.fixed, 0, 256);
    table("kV210NonfixedCrc", d.nonfixed, 0, 256);
    return out;
}

}  // namespace compat
}  // namespace pg
