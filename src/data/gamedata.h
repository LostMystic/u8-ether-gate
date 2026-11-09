// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "core/gamefs.h"
#include "data/anim.h"
#include "data/flex.h"
#include "data/mapfile.h"
#include "data/palette.h"
#include "data/shape.h"
#include "data/typeflags.h"
#include "usecode/usecode_file.h"

namespace pg {

// Everything static the engine reads from the game folder at launch
// (specs/world/startup.md section 1). Later milestones add usecode, sound and music here.
struct GameData {
    Palette palette;
    XForm xform;
    ShapeArchive shapes;  // U8SHAPES.FLX
    ShapeArchive gumps;   // U8GUMPS.FLX
    ShapeArchive fonts;   // U8FONTS.FLX
    ShapeArchive mouse;   // U8MOUSE.SHP as shape 0
    TypeFlags typeflags;
    AnimDat anim;
    GlobFile globs;
    Flex fixed;           // STATIC/FIXED.DAT, one entry per map
    Flex nonfixed;        // GAMEDAT/NONFIXED.DAT (new-game state)
    std::vector<NpcRecord> npcs;
    UsecodeFile usecode;  // USECODE/EUSECODE.FLX, or another language's <L>USECODE.FLX
    std::string usecode_path = "USECODE/EUSECODE.FLX";  // the one this copy has
    char language = 'E';  // the first letter of its name; also picks SOUND/<L><n>.FLX, <L>INTRO.SKF ...
    // STATIC/GUMPAGE.DAT: the item area of each gump shape (specs/presentation/gumps.md 2.1).
    struct GumpArea { int left = 0, top = 0, right = 0, bottom = 0; bool empty() const { return right <= left || bottom <= top; } };
    std::vector<GumpArea> gump_areas;  // index = gump shape
    GumpArea gump_area(unsigned shape) const { return shape < gump_areas.size() ? gump_areas[shape] : GumpArea{}; }
    // STATIC/WPNOVLAY.DAT: weapon overlay records (specs/formats/weapons.md).
    Flex wpnovlay;
    // Milestone 5 (all optional: missing files mean silence / no movie).
    Flex sound;            // SOUND/SOUND.FLX (formats/sound.md)
    Flex music;            // SOUND/MUSIC.FLX (formats/music.md)
    Flex intro, endgame;   // STATIC/EINTRO.SKF, STATIC/ENDGAME.SKF (formats/movies.md)
    Bytes credits, quotes; // STATIC/ECREDITS.DAT, STATIC/QUOTES.DAT
    GameFS fs;             // the game folder, for files read on demand (speech packs)

    // Loads all of the above. On failure, error() names the files.
    bool load(const GameFS& fs);
    // The required files, every missing one named (missing_required() after a failed load). The
    // usecode is listed under its English name; any <L>USECODE.FLX will do.
    static const std::vector<std::string>& required_files();
    const std::vector<std::string>& missing_required() const { return missing_; }
    // The optional files this copy lacks, each with what the game does without it.
    const std::vector<std::string>& missing_optional() const { return missing_optional_; }
    // Engine-made actor shapes (NOTES.md "Custom shapes"): reads `dir`/shapes.json, a list of
    // {"number", "file", "anim_from", "typeflags_from"}, and adds each shape file (U8 shape
    // format) at its number, with the ANIM.DAT actions and TYPEFLAG record of another shape.
    // Numbers are past the original 0..2047. Returns how many were added (0 if no manifest).
    int load_custom_shapes(const std::string& dir, std::string* error = nullptr);
    static constexpr unsigned kFirstCustomShape = 2048;
    std::vector<unsigned> custom_shapes;  // the numbers added
    const std::string& error() const { return error_; }

private:
    std::string error_;
    std::vector<std::string> missing_, missing_optional_;
};

}  // namespace pg
