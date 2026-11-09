// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Which copy of the game is this, and does it match what our fixes and additions were made for?
//
// The player brings their own copy of Ultima VIII, patched or not. At load (GameData::load) the
// core files are hashed and compared with the copies we know (data/known_copies.inc: hashes only,
// no game data), and the log and the About page say which copy it is, or "unknown version:
// running anyway".
//
// Everything that was written against particular game data asks here before it acts, and switches
// itself off with one log line when its data is different, so a copy we have never seen simply
// plays without that extra instead of breaking:
//   class_known(c)        a usecode class holds the bytes the rule was written for
//   fixed_map_known(m), nonfixed_map_known(m)  a map's FIXED / NONFIXED entry likewise
//   world_known()         FIXED, NONFIXED and GLOB all match (the stitched world's layout)
//   usecode_known()       the whole usecode file matches (rules that hang on story flags in
//                         many classes)
//   file_known(path)      a core file matches
// Usecode byte patches check their own bytes as well (usecode/uc_patches.h). Until a game has
// been loaded every answer is yes (unit tests that build their own data).
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

class GameFS;
struct GameData;

namespace compat {

// Hashes `fs`'s core files and answers the questions below from then on. `d` must be loaded.
void check(const GameFS& fs, const GameData& d);

bool copy_known();                  // all core files match a copy we know
const std::string& copy_name();     // "English v2.10", or "unknown version"
std::string about_line();           // for the log and the About page

bool file_known(const std::string& rel);  // e.g. "STATIC/FIXED.DAT"
bool usecode_known();
bool class_known(unsigned cls);
bool fixed_map_known(unsigned map);
bool nonfixed_map_known(unsigned map);
bool world_known();

// One log line, once per `what`, saying what switched itself off and why.
void off(const std::string& what, const std::string& why);
const std::vector<std::string>& switched_off();

// The core files that differ from the known copy (empty when it is known).
const std::vector<std::string>& differing_files();

// Developer tool (`pagan --write-known-table FILE`): data/known_copies.inc for this folder.
std::string known_table_text(const GameFS& fs, const GameData& d);

}  // namespace compat
}  // namespace pg
