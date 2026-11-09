# U8 Ether Gate

An engine for **Ultima VIII: Pagan**. It requires the original game.

U8 Ether Gate is a new, clean-room engine that runs the data files of Ultima VIII, on Linux and
in a web browser, with bug fixes and optional additions. **It contains none of the game's files.
You need your own copy of Ultima VIII: Pagan**, patched or not.

Not affiliated with or endorsed by Electronic Arts or Origin Systems. Ultima is a trademark of its
owner.

## What you need from your copy

The folder holding `STATIC`, `GAMEDAT` and `USECODE` (any upper or lower case). You can also give the folder
around it (a store's or a DOSBox package's): the game is found up to three folders down. Any release of the
game will do (floppy, CD, a store's copy), patched or not, played in or not: new games start from the copy's own
new-game save, `SAVEGAME/U8SAVE.000`, as the original does, so a game you played in DOS doesn't carry over.
No saved games or game files of any kind come with U8 Ether Gate.

Required: `STATIC/U8PAL.PAL`, `XFORMPAL.DAT`, `U8SHAPES.FLX`, `U8GUMPS.FLX`, `U8FONTS.FLX`,
`U8MOUSE.SHP`, `TYPEFLAG.DAT`, `ANIM.DAT`, `GLOB.FLX`, `FIXED.DAT`; `GAMEDAT/NONFIXED.DAT`,
`ITEMCACH.DAT`, `NPCDATA.DAT` (or `SAVEGAME/U8SAVE.000`, which holds them); and `USECODE/EUSECODE.FLX` (or another
language's `?USECODE.FLX`).

Optional, skipped when missing: `SOUND/SOUND.FLX` (sound effects), `SOUND/MUSIC.FLX` (music),
`SOUND/E*.FLX` (speech), `STATIC/EINTRO.SKF` and `ENDGAME.SKF` (movies), `ECREDITS.DAT`, `QUOTES.DAT`,
`GUMPAGE.DAT`, `WPNOVLAY.DAT`.

## Running it

**Linux** (`U8-Ether-Gate-<version>-linux-x86_64.zip`, x86-64, built against glibc 2.38; on older
systems build from source): unzip, then

    ./u8-ether-gate --game /path/to/ultima8

The game folder is looked for in this order: `--game PATH`; the line `game_dir=PATH` in
`~/.pagan/pagan.cfg`; the environment variable `PAGAN_GAME`; a folder `game` or `ultima8` next to the
program. Saves and settings are in `~/.pagan/`.

**Web** (`U8-Ether-Gate-<version>-web.zip`): put the folder on any static web host (see `HOSTING.md`
inside it). The first time, the page asks you to choose your game folder. The files are read locally
and kept in your browser; **nothing is uploaded**. Later visits skip the question; "Change game
folder" is on the page you reach with Quit, and on the loading screen. Saves and settings stay in
the browser too.

**From source** (`U8-Ether-Gate-<version>-source.zip`, also the way to Windows and macOS builds, which
have not been made or tested): CMake 3.16+, a C++17 compiler and SDL2.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPAGAN_GAME_DIR=/path/to/ultima8
    cmake --build build -j8
    ctest --test-dir build              # the tests run against your copy of the game
    build/bin/pagan

Web build (Emscripten SDK; it downloads nothing by itself):

    emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DPAGAN_WEB_PUBLIC=ON
    cmake --build build-web -j8         # the result is build-web/web/

## Which copy is this?

At start the core files are fingerprinted (SHA-1). The log and the **About** page of the help (F1)
say which known copy this is, or `unknown version: running anyway`.

**Only English v2.10 has been tested.** That is the only copy known so far. Everything written against
its exact data (byte patches of the story scripts, text corrections, map fixes, the rubble in the Pit
of the Dead, new dialogue for original NPCs, item powers, the stitched world, the Undead invasion)
checks that its data is the same and switches itself off, with one line in the log, when it isn't.
The game itself runs on. Other languages' usecode (`GUSECODE.FLX` and so on) and movie and speech
files are picked up by name, but no other language has been played, and other versions may well show
problems in the engine itself. Reports are welcome.

## What's added to the original, and what turns it off

All in the game's Options (O, or Esc then Options); most have a command-line switch for one run too.

| Addition | Default | Option | Command line |
| --- | --- | --- | --- |
| Original bug fixes (scripts, text, map holes, falling out of the world) | On | Game: Original bug fixes | `--bug-fixes off` |
| Levels and XP instead of training by use | On | Game: Leveling | `--leveling off` |
| Changes from the official v2.12 patch (targeted jump, recall, stacking) | On | Game: v2.12 changes | |
| The cut undead invasion of Tenebrae | On | Game: Undead invasion | |
| The buried cave mouth in the Pit of the Dead | On | Game: Underworld entrance | `--underworld-entrance off` |
| Companions, including Nerissa | On | Companions | `--companions off` |
| New dialogue for original NPCs | On | New Dialogue | `--npc-dialog off` |
| Turn-based combat | On | Controls: Combat | `--combat realtime` |
| The stitched world (maps joined without loading screens; in progress: parts of Tenebrae overlap) | Off | Game: World | `--world stitched` |
| Item powers (Skull of Quakes) | On | Game: Item powers | |
| Responsive controls, smooth scrolling, zoom, controller | On | Controls | `--controls classic`, `--no-smooth` |
| HUD side panel | Setting | Display | `--hud classic` |
| World atlas (M), world editor (`--edit`, or `?edit` on the web) | | | |

## Licence and credits

U8 Ether Gate is free software: you may share and change it under the **GNU General Public License,
version 3 or (at your option) any later version**: see `LICENSE`. Copyright (c) 2026 Mark B.

Third-party parts keep their own licences: see `THIRD_PARTY.md` (SDL2, stb, nlohmann/json,
TinySoundFont, and the GeneralUser GS SoundFont by S. Christian Collins).

Acknowledgements: the engine was written clean-room, from plain-language descriptions of the file formats
and behaviour and from experiments with the game's data; no code from other engines is in it. Those
descriptions were written by others who studied the ScummVM and Pentagram projects' work on Ultima 8, and
the ScummVM build was run as a black box to compare behaviour. Thanks to those projects, to everyone who has
kept this game alive, and to Origin Systems for making it.
