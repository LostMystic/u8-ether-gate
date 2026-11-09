// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "core/bytes.h"

namespace pg {

// Read-only access to the original game folder. The original data uses upper-case DOS
// names ("STATIC/U8SHAPES.FLX"); lookups match each path component case-insensitively,
// so callers may use any case and the folder may have been copied with any case.
class GameFS {
public:
    // root: the folder that holds STATIC/, GAMEDAT/, USECODE/, SOUND/ ...
    bool open(const std::string& root);
    const std::string& root() const { return root_; }

    // Real path of a file relative to the root ("static/u8pal.pal"), or "" if missing.
    std::string find(const std::string& rel) const;
    bool exists(const std::string& rel) const { return virtual_of(rel) || !find(rel).empty(); }
    // Reads a whole file. False (and `out` empty) if it is missing or unreadable.
    bool read(const std::string& rel, Bytes& out) const;

    // The names of the files in a folder relative to the root (any case), sorted; empty if missing.
    std::vector<std::string> list(const std::string& rel_dir) const;

    // Whether `dir` looks like a game folder (it holds STATIC/U8PAL.PAL in any case).
    static bool is_game_folder(const std::string& dir);
    // `dir` itself if it is a game folder, else the shallowest game folder below it (up to `depth`
    // levels: an installer's or a store's folder around the game, such as a DOSBox package with
    // the game in ultima8/), else "".
    static std::string game_folder_in(const std::string& dir, int depth = 3);

    // The new-game state. The original starts every new game by unpacking SAVEGAME/U8SAVE.000 into
    // GAMEDAT/ (formats/savegame.md), so GAMEDAT/ is whatever the DOS game last left there: a
    // copy that has been played holds that game's world. open() reads U8SAVE.000 when the copy has
    // one and serves its members in place of GAMEDAT/'s files of the same names (read, exists).
    // `new_game_from_save()`: it did; `played_gamedat()`: and GAMEDAT/ held something else.
    bool new_game_from_save() const { return !virtual_.empty(); }
    bool played_gamedat() const { return played_; }
    std::string where(const std::string& rel) const;  // for messages: "SAVEGAME/U8SAVE.000" or the path
    // Finds the game folder: $PAGAN_GAME, then ./game and ./ultima8 next to the executable
    // (`exe_dir`, may be empty), the working folder itself and the same names in it, and the
    // developer tree's ../game, ../../game, ... Returns "" if none holds the game; `tried` (if
    // given) lists where it looked.
    static std::string locate(const std::string& exe_dir, std::vector<std::string>* tried = nullptr);

private:
    std::string root_;
    std::vector<std::pair<std::string, Bytes>> virtual_;  // upper-case rel path -> data
    bool played_ = false;
    const Bytes* virtual_of(const std::string& rel) const;
    void read_new_game_save();
};

}  // namespace pg
