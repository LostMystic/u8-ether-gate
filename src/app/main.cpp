// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Entry point. The program is a per-frame tick from the start (Runner::frame): the native
// build calls it in a loop, the web build hands it to emscripten_set_main_loop, so nothing
// ever blocks and no ASYNCIFY is needed. The game kernel (60 Hz ticks, two per 30 Hz world
// frame) runs inside the same frame function. Without --view-map the game mode starts a new
// game (milestone 2); with it the free-camera map viewer of milestone 1 runs.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "app/atlas_view.h"
#include "app/debug_tools.h"
#include "app/game_mode.h"
#include "app/map_viewer.h"
#include "app/web_update.h"
#include "audio/mixer.h"
#include "audio/wav.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "core/log.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "editor/area_model.h"
#include "editor/area_render.h"
#include "editor/dresser.h"
#include "editor/edit_mode.h"
#include "game/variants.h"
#include "world/map_patches.h"
#include "game/movie.h"
#include "game/banter.h"
#include "game/npc_dialog.h"
#include "game/creatures.h"
#include "game/virtues.h"
#include "game/world_stages.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/turn_combat.h"
#include "game/item_powers.h"
#include "game/servant.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "world/world_layout.h"
#include "world/world_space.h"
#include "usecode/uc_patches.h"
#include "platform/platform.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <SDL.h>
#endif

namespace {

using namespace pg;

struct Options {
    std::string game;
    int view_map = -1;
    std::string view_area;  // a dressed area (editor/dresser.h) shown in the viewer
    bool have_at = false;
    int at[3] = {0, 0, 0};
    std::string screenshot;
    int frames = 1;
    int shot_scale = 1;
    int shot_every = 0;  // also save FILE-<frame>.png every N frames
    bool test_pattern = false;
    bool no_roof = false;
    bool show_editor = false;
    bool list_items = false;
    bool hud = false;
    bool auto_ui = false;
    bool trace_events = false;
    bool dump_npcs = false;
    bool have_warp = false;
    int warp[4] = {0, 0, 0, 0};
    std::string play;  // scripted input (game mode)
    bool skip_opening = false;
    bool no_name = false;
    std::string saves;  // savegame folder
    int load_slot = 0;
    bool have_mouse = false;
    int mouse[2] = {0, 0};
    // Map viewer zoom and atlas (app/atlas_view.h).
    int zoom = 0;  // 0 = 1x, n = 1/2^n
    bool zoom_roundtrip = false;
    bool atlas = false;
    std::string atlas_layout, export_atlas, write_layout;
    int export_level = 3;
    bool export_plain = false;
    PlatformConfig platform;
    // Milestone 5: audio, movies, settings.
    std::string soundfont, record_audio, config;
    std::string export_sound;  // "N:FILE.wav" (a SOUND.FLX entry) or "mTRACK:SECONDS:FILE.wav"
    int intro = -1;  // -1: default (on unless headless / skipped / loading), 0 off, 1 on
    int start_menu = -1;  // -1: default (when saves exist, unless headless / --load / --skip-opening ...), 0 off, 1 on
    bool mute = false;
    // Milestone 6: controls and the composed view (PLAN-M6.md).
    int frame_scale = 0;    // 0: from the window (interactive) / the plain 320x200 path (headless)
    int hud_mode = -1;      // HUD side panel: -1 the setting, 0 Classic, 1 Side panel (PLAN-HUD.md)
    int controls = -1;      // -1 settings, 0 classic, 1 responsive
    int smooth = -1;        // -1 settings, 0 off, 1 on
    int zoom_step = -1;     // -1 settings
    bool debug_assist = false;
    bool bench = false;     // log frame and render timing every 300 frames
    bool edit = false;      // edit mode (editor/edit_mode.h)
    // Debug mode and the critical path (app/debug_tools.h).
    bool debug_mode = false, show_crit = false, no_autosave = false;
    int companions = -1;  // --companions on|off (-1: the setting, default on)
    int bug_fixes = -1;   // --bug-fixes on|off (-1: the setting "Original bug fixes", default on)
    int leveling = -1;    // --leveling on|off (-1: the setting "Leveling", default on)
    int creatures = -1;   // --creatures on|off (-1: the setting "New creatures", default on)
    int virtues = -1;     // --virtues on|off (-1: the setting "Virtues", default on)
    int underworld = -1;  // --underworld-entrance on|off (-1: the setting "Underworld entrance", default on)
    int npc_dialog = -1;  // --npc-dialog on|off (-1: the setting, default on)
    int combat = -1;      // --combat turn|realtime (-1: the setting, default turn-based)
    std::string critpath;  // comma-separated JSON files (default data/critpath.json + data/map_links.json)
    // The stitched world (world/world_space.h, PLAN-WORLD-EDITOR.md section 1).
    int world = -1;                 // --world stitched|original (-1: the setting)
    std::string world_layout;       // --world-layout FILE (default engine/data/world_layout.json)
    std::string write_world_layout; // --write-world-layout FILE: derive it, merge into FILE, exit
    std::string write_known_table;  // --write-known-table FILE: data/known_copies.inc for this folder, exit
};

// "1/8", "8" or "0.125" -> 3 (the power of two).
int parse_scale(const char* v) {
    std::string s = v;
    double d = s.rfind("1/", 0) == 0 ? std::atof(s.c_str() + 2) : std::atof(s.c_str());
    if (d > 0 && d < 1) d = 1 / d;
    int n = 0;
    while (n < 12 && (1 << (n + 1)) <= int(d + 0.5)) ++n;
    return n;
}

// The folder the program itself is in (found however it was started); "" if unknown.
std::string program_dir(const char* argv0) {
#ifndef __EMSCRIPTEN__
    if (char* base = SDL_GetBasePath()) {
        std::string d = base;
        SDL_free(base);
        while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        return d;
    }
#endif
    std::string exe = argv0 ? argv0 : "";
    size_t slash = exe.find_last_of('/');
    return slash == std::string::npos ? "" : exe.substr(0, slash);
}

// Where a file of our data folder may be: next to the program (a release), in a build tree's
// bin/ (../../data), in the working folder; on the web, /data of the virtual file system.
std::vector<std::string> data_candidates(const char* argv0, const std::string& name) {
    std::vector<std::string> out;
#ifdef __EMSCRIPTEN__
    out.push_back("/data/" + name);
#endif
    std::string dir = program_dir(argv0);
    if (!dir.empty()) {
        out.push_back(dir + "/data/" + name);
        out.push_back(dir + "/../../data/" + name);
    }
    out.push_back("data/" + name);
    out.push_back("engine/data/" + name);
    return out;
}

// A file in our data folder; "" if missing.
std::string find_data_file(const char* argv0, const std::string& name) {
    for (const std::string& c : data_candidates(argv0, name))
        if (FILE* f = std::fopen(c.c_str(), "rb")) { std::fclose(f); return c; }
    return "";
}

// Something stops the program before it can show anything: the log, plus a box a player who
// started it from a desktop can see (on the web, the page's message; headless, only the log).
void fatal_message(const std::string& text, bool headless) {
    std::fprintf(stderr, "%s\n", text.c_str());
#ifdef __EMSCRIPTEN__
    (void)headless;
    EM_ASM({ if (Module.pgFatal) Module.pgFatal(UTF8ToString($0)); }, text.c_str());
#else
    if (!headless && !std::getenv("PAGAN_NO_MESSAGE_BOX"))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "U8 Ether Gate", text.c_str(), nullptr);
#endif
}

// The game folder: --game, then the setting game_dir, then GameFS::locate ($PAGAN_GAME, next to
// the program). A folder given by name that isn't one is said so. "" = not found; `why` says so.
std::string find_game_folder(const Options& o, const char* argv0, std::string& why) {
    const std::string how = "Tell it where the game folder is (the one holding STATIC, GAMEDAT and USECODE):\n"
                            "  --game PATH on the command line,\n"
                            "  game_dir=PATH in the settings file ~/.pagan/pagan.cfg,\n"
                            "  the environment variable PAGAN_GAME,\n"
                            "  or put the folder next to the program as game or ultima8.";
    if (!o.game.empty()) {
        if (std::string in = GameFS::game_folder_in(o.game); !in.empty()) {
            if (in != o.game) PG_LOG("the game is in %s", in.c_str());
            return in;
        }
        why = "U8 Ether Gate: " + o.game + " holds no Ultima VIII game folder (no STATIC/U8PAL.PAL in it or in the folders below it).\n\n" + how;
        return "";
    }
    const std::string cfg = Config::global().get("game_dir");
    if (!cfg.empty()) {
        if (std::string in = GameFS::game_folder_in(cfg); !in.empty()) return in;
        PG_LOG("the setting game_dir=%s is not a game folder; looking elsewhere", cfg.c_str());
    }
    std::string root = GameFS::locate(program_dir(argv0));
    if (root.empty())
        why = "U8 Ether Gate is an engine for Ultima VIII: Pagan and needs your own copy of the game. It could not find one.\n\n" + how;
    return root;
}

// --write-world-layout: derives the stitched layout (world/world_layout.h) and merges it into the
// file: the groups and the seams / links / variants / layers fields are replaced, the editor's
// areas[], its revision and any other field are kept.
int write_world_layout(const GameData& data, const std::string& path, const char* argv0) {
    std::string links_text;
    std::string lf = find_data_file(argv0, "map_links.json");
    if (FILE* f = lf.empty() ? nullptr : std::fopen(lf.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) links_text.append(buf, n);
        std::fclose(f);
    }
    WorldLayoutFile gen = generate_world_layout(data, links_text);
    WorldLayout doc;  // editor/area_model.h: keeps areas[] and the other fields
    std::string err, old;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) old.append(buf, n);
        std::fclose(f);
        if (!doc.from_json(old, err)) { PG_LOG("%s: %s (not merged)", path.c_str(), err.c_str()); doc = WorldLayout{}; }
    }
    doc.atlas = gen.atlas;
    nlohmann::json x = doc.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.extra, nullptr, false);
    if (!x.is_object()) x = nlohmann::json::object();
    nlohmann::json mine = nlohmann::json::parse(gen.extra_json(), nullptr, false);
    for (auto& [k, v] : mine.items()) x[k] = v;
    doc.extra = x.dump();
    std::vector<std::string> problems = check_world_layout(gen, compute_footprints(data));
    for (const std::string& p : problems) PG_LOG("layout check: %s", p.c_str());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { PG_LOG("cannot write %s", path.c_str()); return 1; }
    std::string text = doc.to_json();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::map<int, int> per_layer;
    for (const AtlasGroup& g : gen.atlas.groups) ++per_layer[g.layer];
    std::string layers;
    for (auto& [l, n] : per_layer) layers += " " + std::to_string(l) + ":" + std::to_string(n);
    PG_LOG("wrote %s: %zu groups (per layer%s), %zu seams, %zu teleport links, %zu variants, %zu problems", path.c_str(),
           gen.atlas.groups.size(), layers.c_str(), gen.seams.size(), gen.links.size(), gen.variants.size(), problems.size());
    return problems.empty() ? 0 : 1;
}

// The atlas layout file: data/atlas_layout.json, found like the other data files (written next
// to the program or the build tree when it isn't there yet).
std::string find_atlas_layout(const char* argv0) {
    std::vector<std::string> cands = data_candidates(argv0, "atlas_layout.json");
    for (const std::string& c : cands)
        if (FILE* f = std::fopen(c.c_str(), "rb")) { std::fclose(f); return c; }
    return cands[0];
}

void usage() {
    std::printf(
        "U8 Ether Gate - an engine for Ultima VIII: Pagan; requires the original game\n"
        "  --version             print the version and exit\n"
        "  --game DIR            game folder (else the setting game_dir, $PAGAN_GAME, or game/ or ultima8/\n"
        "                        next to the program)\n"
        "  (no --view-map)       game mode: a new game running the original usecode\n"
        "  --view-map N          map viewer for map N instead of the game\n"
        "  --view-area FILE      map viewer showing a dressed area (pagan_dress output) at its origin\n"
        "                        (on an empty map, or on --view-map N)\n"
        "  --at X,Y,Z            point to look at (camera = point + 20 in z)\n"
        "  --screenshot FILE     render --frames N frames headless, save the last as PNG\n"
        "  --frames N            frames to run before the screenshot (default 1)\n"
        "  --shot-scale N        enlarge the screenshot N times\n"
        "  --shot-every N        also save FILE-<frame>.png every N frames (with --screenshot)\n"
        "  --mouse X,Y           pointer position (for screenshots)\n"
        "  --test-pattern        show all cursor frames and fonts\n"
        "  --no-roof             don't hide roofs     --show-editor  draw eggs and markers\n"
        "  --list-items          print the painted items of the screenshot frame\n"
        "  --hud                 game mode: status line (H toggles)\n"
        "  --auto-ui             game mode: answer menus and readers close by themselves\n"
        "  --trace-events        game mode: log every usecode event that starts a process\n"
        "  --dump-npcs           game mode: print the actors when the screenshot is taken\n"
        "  --warp X,Y,Z,MAP      game mode (debug): put the Avatar there after the new game starts\n"
        "  --play SCRIPT         game mode: scripted input \"FRAME:ACTION[:ARG];...\" (move:X,Y ldown lup\n"
        "                        rdown rup mdown mup key:K keyup:K; K = up down left right space shift ctrl a-z)\n"
        "  --skip-opening        game mode (debug): no opening scene, the Avatar starts free at the camp\n"
        "  --no-name             game mode: no name entry at the start (the Avatar is \"Avatar\")\n"
        "  --saves DIR           savegame folder (default ~/.pagan/saves; web: IndexedDB)\n"
        "  --load N              game mode: start with diary entry N loaded\n"
        "  --no-video            no SDL video at all (with --screenshot)\n"
        "  --zoom 1/N            map viewer zoomed out (N = 2, 4 ... 128; wheel or +/- in the viewer)\n"
        "  --atlas               world atlas of all maps (M in the viewer)\n"
        "  --edit                edit mode: paint new areas on the world (with --view-map N --at x,y,z: start there)\n"
        "  --atlas-layout FILE   atlas layout JSON (default engine/data/atlas_layout.json)\n"
        "  --write-atlas-layout FILE  derive the layout from the game data, write it, exit\n"
        "  --write-world-layout FILE  derive the stitched world layout, merge it into FILE (keeps the\n"
        "                        editor's areas and unknown fields), check it, exit\n"
        "  --world stitched|original  the World option for this run (default: the setting)\n"
        "  --world-layout FILE   the stitched world layout (default engine/data/world_layout.json)\n"
        "  --export-atlas FILE   write the whole atlas as one PNG and exit\n"
        "  --export-scale 1/N    export scale against native pixels (default 1/8)  --export-plain no labels\n"
        "  --controls responsive|classic  (or --classic)  --smooth / --no-smooth  game mode control settings\n"
        "  --frame-scale N       compose the game frame at N output pixels per game pixel (headless: 1-4)\n"
        "  --zoom-step N         game view zoomed out N steps (needs a frame scale above N)\n"
        "  --debug-assist        draw the movement-assist probes\n"
        "  --debug               game mode: debug mode on (F10 debug menu) without changing the setting\n"
        "  --show-crit           game mode: start with the critical-path line shown (with --debug)\n"
        "  --critpath F[,F]      critical-path JSON files (default data/critpath.json, data/map_links.json)\n"
        "  --no-autosave         game mode: no autosaves this run\n"
        "  --companions on|off   game mode: the recruitable companions (default: the setting, on)\n"
        "  --bug-fixes on|off    the \"Original bug fixes\" option for this run (default: the setting, on)\n"
        "  --leveling on|off     the \"Leveling\" option for this run (default: the setting, on)\n"
        "  --creatures on|off    the \"New creatures\" option for this run (default: the setting, on)\n"
        "  --virtues on|off      the \"Virtues\" option for this run (default: the setting, on)\n"
        "  --underworld-entrance on|off  the \"Underworld entrance\" option for this run (default: the setting, on)\n"
        "  --npc-dialog on|off   game mode: the engine's new topics for original NPCs (default: the setting, on)\n"
        "  --combat turn|realtime  game mode: turn-based or real-time combat (default: the setting, turn)\n"
        "  --intro / --no-intro  game mode: play the intro movie at a new game (default: on, off headless)\n"
        "  --start-menu / --no-start-menu  game mode: when saves exist, start at the main menu over the\n"
        "                        most recent save instead of the intro (default: on, off headless)\n"
        "  --soundfont FILE      General MIDI SoundFont for the music (default data/soundfont/u8_gm.sf2)\n"
        "  --record-audio FILE   with --screenshot: write the mixed sound and music of the run as a WAV\n"
        "  --mute                no audio device\n"
        "  --export-sound N:FILE  write SOUND.FLX entry N as a WAV and exit (mT:SECS:FILE: music track T)\n"
        "  --config FILE         settings file (default ~/.pagan/pagan.cfg; web: localStorage)\n"
        "  --scale N  --scale-mode integer|fit|stretch  --no-aspect  --fullscreen\n"
        "  --window WxH          window size (headless: the size a screenshot shows, with the HUD panel)\n"
        "  --hud panel|classic   the HUD side panel or the classic overlay (default: the setting)\n"
        "Game keys: arrows scroll the view (debug), Home back to the Avatar, 1-9 answers,\n"
        "      click: look, double click: use, H status line, Esc quit\n"
        "Viewer keys: arrows scroll (Shift faster), PgUp/PgDn height, N/P map, R roofs, E eggs,\n"
        "      T test pattern, Home start point, wheel or +/- zoom, M atlas, F9 scale mode, F10 4:3,\n"
        "      F11 fullscreen, Esc quit\n"
        "Atlas keys: wheel or +/- zoom, arrows or drag pan, click opens the map there, Ctrl+drag or\n"
        "      right-drag moves a group, S saves the layout, G re-derives it, L labels, Home all, M back\n");
}

bool parse_ints(const char* s, int* out, int n) {
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = int(std::strtol(s, &end, 10));
        if (end == s) return false;
        s = end;
        if (i + 1 < n) {
            if (*s != ',') return false;
            ++s;
        }
    }
    return true;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (a == "--version") {
#ifdef PAGAN_VERSION
            std::printf("U8 Ether Gate %s\n", PAGAN_VERSION);
#else
            std::printf("U8 Ether Gate\n");
#endif
            std::exit(0);
        }
        else if (a == "--game" && (v = next())) o.game = v;
        else if (a == "--write-known-table" && (v = next())) o.write_known_table = v;
        else if (a == "--view-map" && (v = next())) o.view_map = std::atoi(v);
        else if (a == "--view-area" && (v = next())) o.view_area = v;
        else if (a == "--at" && (v = next())) { if (!parse_ints(v, o.at, 3)) return false; o.have_at = true; }
        else if (a == "--screenshot" && (v = next())) o.screenshot = v;
        else if (a == "--frames" && (v = next())) o.frames = std::max(1, std::atoi(v));
        else if (a == "--shot-scale" && (v = next())) o.shot_scale = std::max(1, std::atoi(v));
        else if (a == "--shot-every" && (v = next())) o.shot_every = std::max(0, std::atoi(v));
        else if (a == "--mouse" && (v = next())) { if (!parse_ints(v, o.mouse, 2)) return false; o.have_mouse = true; }
        else if (a == "--test-pattern") o.test_pattern = true;
        else if (a == "--no-roof") o.no_roof = true;
        else if (a == "--show-editor") o.show_editor = true;
        else if (a == "--list-items") o.list_items = true;
        else if (a == "--hud" && i + 1 < argc && (std::string(argv[i + 1]) == "panel" || std::string(argv[i + 1]) == "classic"))
            o.hud_mode = std::string(argv[++i]) == "classic" ? 0 : 1;
        else if (a == "--hud") o.hud = true;
        else if (a == "--auto-ui") o.auto_ui = true;
        else if (a == "--trace-events") o.trace_events = true;
        else if (a == "--dump-npcs") o.dump_npcs = true;
        else if (a == "--warp" && (v = next())) { if (!parse_ints(v, o.warp, 4)) return false; o.have_warp = true; }
        else if (a == "--play" && (v = next())) o.play = v;
        else if (a == "--skip-opening") o.skip_opening = true;
        else if (a == "--no-name") o.no_name = true;
        else if (a == "--saves" && (v = next())) o.saves = v;
        else if (a == "--load" && (v = next())) o.load_slot = std::atoi(v);
        else if (a == "--scale" && (v = next())) o.platform.window_scale = std::max(1, std::atoi(v));
        else if (a == "--scale-mode" && (v = next())) {
            std::string m = v;
            o.platform.scale_mode = m == "fit" ? ScaleMode::kFit : m == "stretch" ? ScaleMode::kStretch : ScaleMode::kInteger;
        }
        else if (a == "--no-aspect") o.platform.aspect_4_3 = false;
        else if (a == "--window" && (v = next())) {
            if (std::sscanf(v, "%dx%d", &o.platform.window_w, &o.platform.window_h) != 2) return false;
        }
        else if (a == "--fullscreen") o.platform.fullscreen = true;
        else if (a == "--no-video") o.platform.no_video = true;
        else if (a == "--zoom" && (v = next())) o.zoom = parse_scale(v);
        else if (a == "--zoom-roundtrip") o.zoom_roundtrip = true;
        else if (a == "--atlas") o.atlas = true;
        else if (a == "--atlas-layout" && (v = next())) o.atlas_layout = v;
        else if (a == "--write-atlas-layout" && (v = next())) o.write_layout = v;
        else if (a == "--write-world-layout" && (v = next())) o.write_world_layout = v;
        else if (a == "--world-layout" && (v = next())) o.world_layout = v;
        else if (a == "--world" && (v = next())) o.world = std::string(v) == "stitched" ? 1 : 0;
        else if (a == "--export-atlas" && (v = next())) o.export_atlas = v;
        else if (a == "--export-scale" && (v = next())) o.export_level = parse_scale(v);
        else if (a == "--export-plain") o.export_plain = true;
        else if (a == "--soundfont" && (v = next())) o.soundfont = v;
        else if (a == "--record-audio" && (v = next())) o.record_audio = v;
        else if (a == "--config" && (v = next())) o.config = v;
        else if (a == "--intro") o.intro = 1;
        else if (a == "--no-intro") o.intro = 0;
        else if (a == "--start-menu") o.start_menu = 1;
        else if (a == "--no-start-menu") o.start_menu = 0;
        else if (a == "--mute") o.mute = true;
        else if (a == "--export-sound" && (v = next())) o.export_sound = v;
        else if (a == "--frame-scale" && (v = next())) o.frame_scale = std::clamp(std::atoi(v), 1, 4);
        else if (a == "--controls" && (v = next())) o.controls = std::string(v) == "classic" ? 0 : 1;
        else if (a == "--classic") o.controls = 0;
        else if (a == "--smooth") o.smooth = 1;
        else if (a == "--no-smooth") o.smooth = 0;
        else if (a == "--zoom-step" && (v = next())) o.zoom_step = std::clamp(std::atoi(v), 0, 6);
        else if (a == "--debug-assist") o.debug_assist = true;
        else if (a == "--debug") o.debug_mode = true;
        else if (a == "--show-crit") o.show_crit = true;
        else if (a == "--no-autosave") o.no_autosave = true;
        else if (a == "--companions" && (v = next())) o.companions = std::string(v) == "off" ? 0 : 1;
        else if (a == "--bug-fixes" && (v = next())) o.bug_fixes = std::string(v) == "off" ? 0 : 1;
        else if (a == "--leveling" && (v = next())) o.leveling = std::string(v) == "off" ? 0 : 1;
        else if (a == "--creatures" && (v = next())) o.creatures = std::string(v) == "off" ? 0 : 1;
        else if (a == "--virtues" && (v = next())) o.virtues = std::string(v) == "off" ? 0 : 1;
        else if (a == "--underworld-entrance" && (v = next())) o.underworld = std::string(v) == "off" ? 0 : 1;
        else if (a == "--npc-dialog" && (v = next())) o.npc_dialog = std::string(v) == "off" ? 0 : 1;
        else if (a == "--combat" && (v = next())) o.combat = std::string(v) == "realtime" ? 0 : 1;
        else if (a == "--critpath" && (v = next())) o.critpath = v;
        else if (a == "--bench") o.bench = true;
        else if (a == "--edit") o.edit = true;
        else { std::fprintf(stderr, "bad argument: %s\n", a.c_str()); return false; }
    }
    return true;
}

struct Runner {
    Options opt;
    Platform platform;
    GameData data;
    AudioMixer mixer;
    std::vector<int16_t> recorded;  // --record-audio
    std::unique_ptr<MapViewer> viewer;
    std::unique_ptr<GameMode> game;
    std::unique_ptr<EditMode> editor;  // edit mode (--edit, or the debug menu's "Editor")
    bool editing = false;
    std::string data_dir, layout_path;

    void make_editor() {
        if (editor) return;
        editor = std::make_unique<EditMode>(data);
        AtlasLayout l;
        std::string err;
        if (!l.load(layout_path, err)) l = generate_atlas_layout(data);
        editor->set_fallback_layout(l);
        setup_editor_storage(*editor, data_dir);
        editor->set_tilekit_path(data_dir + "/tilekit.json");
        if (!opt.screenshot.empty()) editor->set_budget(-1);
    }
    // The debug menu's "Editor": the game waits (not ticked) while the editor runs.
    void open_editor_from_game() {
        make_editor();
        editor->set_from_game(true);
        if (!editor->loaded()) editor->load_all();
        if (const Actor* a = game->world().actor(1)) editor->show_map_point(a->mapnum, a->x, a->y, a->z, 1);
        editing = true;
    }
    Surface8 screen{320, 200};
    std::vector<uint32_t> rgba, panel_rgba;
    std::vector<InputEvent> events;
    uint64_t last_ms = 0, acc_ms = 0, last_frame_ms = 0;
    double bench_world = 0, bench_compose = 0, bench_dt_max = 0;
    int bench_n = 0, bench_ticks = 0;
    uint64_t bench_t0 = 0;
    int fw = 320, fh = 200;  // size of the presented frame
    int frames_done = 0;
    bool quit = false;
    int exit_code = 0;
    std::function<void()> boot;  // the game's start, while it waits for the web saves

    // One iteration of the main loop: input, fixed 30 Hz world ticks, draw, present.
    void frame() {
        events.clear();
        platform.set_f10_to_app(game && game->debug().enabled());
        platform.poll(events);
        if (opt.zoom_roundtrip && viewer && frames_done < 2) {
            // Test: zoom out three steps about the pointer, then back in, before the shot.
            InputEvent w;
            w.type = InputEvent::kWheel;
            w.wheel = frames_done == 0 ? -1 : 1;
            for (int k = 0; k < 3; ++k) events.push_back(w);
        }
        if (game && game->debug().editor_requested) {
            game->debug().editor_requested = false;
            open_editor_from_game();
        }
        for (const InputEvent& e : events) {
            if (editing) {
                if (e.type == InputEvent::kQuit) quit = true;
                editor->handle(e);
                continue;
            }
            bool esc = e.type == InputEvent::kKeyDown && e.key == kKeyEscape;
            if (e.type == InputEvent::kQuit || (esc && !game)) quit = true;
            if (game) game->handle(e);
            else viewer->handle(e);
        }
        auto tick = [&]() {
            if (editing) {
                editor->tick();
                if (editor->take_exit()) {
                    if (game) editing = false;  // back to the game
                    else quit = true;
                }
            } else if (game) game->tick();
            else viewer->tick();
        };
        if (game && game->wants_quit()) quit = true;
        if (!opt.screenshot.empty()) {
            tick();  // headless: exactly one game frame per loop
            if (!opt.record_audio.empty()) {
                // Offline mixing: one game frame of sound per tick.
                size_t n = size_t(mixer.rate() / 30), at = recorded.size();
                recorded.resize(at + n * 2, 0);
                mixer.render(&recorded[at], int(n));
            }
        } else {
            // kernel.md 1: time in 1/3000 s units, a frame every 100; resync when 2+ frames late.
            uint64_t now = platform.ticks_ms();
            acc_ms += (now - last_ms) * 3;
            last_ms = now;
            int n = 0;
            while (acc_ms >= 100 && n < 2) { tick(); acc_ms -= 100; ++n; ++bench_ticks; }
            if (acc_ms >= 200) acc_ms = 0;
        }
        fw = screen.width();
        fh = screen.height();
        bool log_items = opt.list_items && !opt.screenshot.empty() && frames_done + 1 >= opt.frames;
        MovieGump* movie = game ? game->world().gumps.find<MovieGump>() : nullptr;
        const bool sized_shot = !opt.screenshot.empty() && opt.platform.window_w > 0 && opt.platform.window_h > 0;
        bool composed = game && !movie && (opt.screenshot.empty() || opt.frame_scale > 0 || sized_shot);
        // The HUD side panel (PLAN-HUD.md): the game only, not the editor, the atlas or a movie;
        // headless runs only with --window (every older screenshot stays 320 x 200).
        bool want_panel = game && composed && !editing && !game->atlas_open() && (opt.screenshot.empty() || sized_shot) &&
                          (opt.hud_mode >= 0 ? opt.hud_mode == 1 : GameMode::side_panel_setting());
        platform.set_side_panel(want_panel);
        if (game) game->set_panel_width(want_panel ? platform.layout().panel_px : 0);
        if (editing) {
            editor->render(rgba, fw, fh, opt.frame_scale > 0 ? opt.frame_scale : platform.frame_scale(4));
        } else if (composed) {
            // Milestone 6: the world at the zoom level from the smooth camera, the UI on top,
            // composed at the window's output scale (game_view.cpp).
            game->set_log_items(log_items);
            uint64_t now = platform.ticks_ms();
            double dt = opt.screenshot.empty() ? (now - last_frame_ms) / 1000.0 : 1.0 / 30;
            last_frame_ms = now;
            double alpha = opt.screenshot.empty() ? std::min(1.0, acc_ms / 100.0) : 0.0;
            int S = opt.frame_scale > 0 ? opt.frame_scale : platform.frame_scale(4);
            game->render_frame(rgba, fw, fh, S, alpha, dt);
            bench_world += game->world_ms();
            bench_compose += game->compose_ms();
            ++bench_n;
            bench_dt_max = std::max(bench_dt_max, dt);
            if (bench_n % 300 == 0 && (opt.bench || std::getenv("PAGAN_BENCH"))) {
                double secs = (now - bench_t0) / 1000.0;
                char line[256];
                std::snprintf(line, sizeof line,
                              "bench: %d frames in %.1f s (%.1f fps, %d world ticks = %.1f/s), world %.2f ms, compose %.2f ms"
                              " (average), longest frame gap %.1f ms, scale %d, zoom step %d", 300, secs,
                              300 / std::max(0.001, secs), bench_ticks, bench_ticks / std::max(0.001, secs),
                              bench_world / 300, bench_compose / 300, bench_dt_max * 1000, S, game->zoom_step());
                PG_LOG("%s", line);
#ifdef __EMSCRIPTEN__
                EM_ASM({ Module.pgBench = UTF8ToString($0); }, line);  // for test scripts
#endif
                if (const World& w = game->world(); w.neighbors) {
                    // The stitched world (game/world_stitch.cpp): streaming and seam crossings.
                    std::snprintf(line, sizeof line,
                                  "bench: world map %u, %u neighbour chunks live, %u chunk builds (%.3f ms average, %.3f ms longest), "
                                  "stream update %.3f ms (last), %u seam crossings (%.2f ms last, %.2f ms longest)",
                                  w.map_number_now(), w.neighbors->chunks_live, w.neighbors->chunk_builds,
                                  w.neighbors->build_ms_total / std::max(1u, w.neighbors->chunk_builds), w.neighbors->build_ms_max,
                                  w.neighbors->update_ms_last, w.seam_stats.crossings, w.seam_stats.last_ms, w.seam_stats.max_ms);
                    PG_LOG("%s", line);
#ifdef __EMSCRIPTEN__
                    EM_ASM({ Module.pgWorldBench = UTF8ToString($0); }, line);
#endif
                }
                bench_world = bench_compose = 0;
                bench_t0 = now;
                bench_ticks = 0;
                bench_dt_max = 0;
            }
        } else if (game) {
            game->set_log_items(log_items);
            game->render(screen);
            screen.to_rgba(data.palette, game->palette(), rgba);
            // A movie draws through its own palette (game/movie.h).
            if (movie) movie->compose(rgba, data.palette);
        } else if (viewer->wants_rgba()) {
            viewer->render_rgba(rgba, fw, fh);  // zoomed view or atlas, 640x400
        } else {
            viewer->set_log_items(log_items);
            viewer->render(screen);
            screen.to_rgba(data.palette, PaletteTransform{}, rgba);
        }
        int pw = 0, ph = 0;
        if (game && game->panel_on() && composed && !editing) game->render_panel_rgba(panel_rgba, pw, ph);
        platform.present(rgba.data(), fw, fh, pw ? panel_rgba.data() : nullptr, pw, ph);
        ++frames_done;
        // Screenshots of a sized window (--window): the whole window, picture and panel.
        auto write_shot = [&](const std::string& path, int scale) {
            if (!sized_shot) return save_png(path, rgba.data(), fw, fh, scale);
            std::vector<uint32_t> win;
            compose_window(platform.layout(), opt.platform.window_w, opt.platform.window_h, rgba.data(), fw, fh,
                           pw ? panel_rgba.data() : nullptr, pw, ph, win);
            return save_png(path, win.data(), opt.platform.window_w, opt.platform.window_h, 1);
        };
        if (game) {
            for (const std::string& f : game->shot_requests) {
                write_shot(f, opt.shot_scale);
                PG_LOG("screenshot: %s (scripted)", f.c_str());
            }
            game->shot_requests.clear();
            if (game->wants_quit()) quit = true;
        }
        if (opt.shot_every > 0 && !opt.screenshot.empty() && frames_done % opt.shot_every == 0 && frames_done < opt.frames) {
            std::string base = opt.screenshot;
            size_t dot = base.rfind('.');
            if (dot != std::string::npos) base.resize(dot);
            save_png(base + "-" + std::to_string(frames_done) + ".png", rgba.data(), screen.width(), screen.height(), opt.shot_scale);
        }
        if (!opt.screenshot.empty() && frames_done >= opt.frames) {
            if (write_shot(opt.screenshot, opt.shot_scale)) {
                if (game) {
                    int cam[3];
                    game->world().camera_point(cam);
                    if (opt.dump_npcs)
                        for (unsigned n = 1; n < 256; ++n)
                            if (const Actor* a = game->world().actor(uint16_t(n)))
                                PG_LOG("actor %3u %-8s shape %3u frame %4u map %3u at %5d,%5d,%3d dir %u anim %u%s%s", n,
                                       a->name.c_str(), a->shape, a->frame, a->mapnum, a->x, a->y, a->z, a->dir,
                                       a->last_anim, (a->flags & kItemFastArea) ? " fast" : "",
                                       a->is(Actor::kDead) ? " dead" : "");
                    PG_LOG("screenshot: %s (game, map %u, camera %d,%d,%d, %zu processes)", opt.screenshot.c_str(),
                           game->world().map().map_number(), cam[0], cam[1], cam[2], game->world().kernel().size());
                } else if (editing) {
                    PG_LOG("screenshot: %s (edit mode, %zu areas)", opt.screenshot.c_str(), editor->doc.areas.size());
                } else {
                    PG_LOG("screenshot: %s (map %u, at %d,%d,%d)", opt.screenshot.c_str(), viewer->map(),
                           viewer->focus()[0], viewer->focus()[1], viewer->focus()[2]);
                }
            } else {
                PG_LOG("could not write %s", opt.screenshot.c_str());
                exit_code = 1;
            }
            quit = true;
            if (!opt.record_audio.empty()) {
                bool ok = write_wav(opt.record_audio, recorded.data(), recorded.size() / 2, 2, mixer.rate());
                PG_LOG("%s %s (%.1f s)", ok ? "recorded" : "could not write", opt.record_audio.c_str(),
                       double(recorded.size() / 2) / mixer.rate());
            }
        }
    }
};

// The music SoundFont: --soundfont, else data/soundfont/u8_gm.sf2.
std::string find_soundfont(const char* argv0) { return find_data_file(argv0, "soundfont/u8_gm.sf2"); }

bool load_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n > 0 ? n : 0));
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

Runner* g_runner = nullptr;
#ifdef __EMSCRIPTEN__
bool web_saves_mounted = false;
#endif

#ifdef __EMSCRIPTEN__
// Saves live in an IndexedDB-backed folder (Emscripten IDBFS), synced after every write so
// they survive a page reload. The initial sync from IndexedDB is asynchronous; the diary
// re-reads the folder each time it opens.
void web_mount_saves() {
    EM_ASM({
        try { FS.mkdir('/saves'); } catch (e) {}
        FS.mount(IDBFS, {}, '/saves');
        Module.pgSavesReady = 0;
        FS.syncfs(true, function(err) {
            Module.pgSavesReady = err ? -1 : 1;
            if (err) console.log('saves: IndexedDB sync failed', err);
            else console.log('saves: loaded from IndexedDB');
        });
    });
}
void web_sync_saves() {
    EM_ASM({
        FS.syncfs(false, function(err) {
            if (err) console.log('saves: IndexedDB write failed', err);
            else { console.log('saves: written to IndexedDB'); Module.pgSavesSynced = (Module.pgSavesSynced || 0) + 1; }
        });
    });
}
#endif

std::string saves_dir(const std::string& opt) {
    if (!opt.empty()) return opt;
#ifdef __EMSCRIPTEN__
    return "/saves";
#else
    if (const char* home = std::getenv("HOME")) {
        std::string base = std::string(home) + "/.pagan";
        mkdir(base.c_str(), 0755);
        return base + "/saves";
    }
    return "saves";
#endif
}

#ifdef __EMSCRIPTEN__
// Performance figures for the page (window.Module.pgPerf): engine frame and audio callback
// cost in ms, averaged / maximum over the last 60 frames.
double g_audio_ms_sum = 0, g_audio_ms_max = 0;
int g_audio_calls = 0;
void web_frame() {
    static double sum = 0, mx = 0;
    static int n = 0;
    if (g_runner->boot) {
        // The start waits (up to 5 s) for the saves to come in from IndexedDB, so that the
        // start menu knows about them.
        static double t_wait = emscripten_get_now();
        int ready = EM_ASM_INT({ return Module.pgSavesReady | 0; });
        if (ready == 0 && emscripten_get_now() - t_wait < 5000) return;
        PG_LOG("start: saves %s after %.0f ms", ready > 0 ? "ready" : ready < 0 ? "failed" : "not ready", emscripten_get_now() - t_wait);
        g_runner->game->saves().refresh();
        auto boot = std::move(g_runner->boot);
        g_runner->boot = nullptr;
        boot();
        g_runner->last_ms = g_runner->platform.ticks_ms();
    }
    double t0 = emscripten_get_now();
    g_runner->frame();
    double dt = emscripten_get_now() - t0;
    sum += dt;
    mx = std::max(mx, dt);
    if (++n == 60) {
        EM_ASM({ Module.pgPerf = ({frameAvg: $0, frameMax: $1, audioAvg: $2, audioMax: $3, audioCalls: $4}); },
               sum / n, mx, g_audio_calls ? g_audio_ms_sum / g_audio_calls : 0, g_audio_ms_max, g_audio_calls);
        sum = mx = 0;
        n = 0;
        g_audio_ms_sum = g_audio_ms_max = 0;
        g_audio_calls = 0;
    }
    web_update_poll(g_runner->game.get());  // looks once a second (app/web_update.h)
    if (g_runner->quit) {
        emscripten_cancel_main_loop();
        web_exit_page();  // settings saved, then the update / patch notes page
    }
}
#endif

}  // namespace

int main(int argc, char** argv) {
    auto* r = new Runner;  // lives for the whole program (the web main loop outlives main)
    g_runner = r;
    if (!parse_args(argc, argv, r->opt)) { usage(); return 2; }
    const bool headless_run = !r->opt.screenshot.empty();
    Config::global().load(r->opt.config.empty() ? Config::default_path() : r->opt.config);
    std::string why;
    std::string root = find_game_folder(r->opt, argc > 0 ? argv[0] : nullptr, why);
    GameFS fs;
    if (root.empty() || !fs.open(root)) {
        fatal_message(why.empty() ? "U8 Ether Gate: the game folder could not be opened." : why, headless_run);
        return 1;
    }
    if (!r->data.load(fs)) {
        std::string text = "U8 Ether Gate cannot start from the game folder " + root + ".\n\n";
        if (!r->data.missing_required().empty()) {
            text += "These files of Ultima VIII are missing, and the game cannot run without them:\n";
            for (const std::string& f : r->data.missing_required()) text += "  " + f + "\n";
        } else {
            text += "A required file is damaged or is not the file it should be: " + r->data.error() + "\n";
        }
        fatal_message(text, headless_run);
        return 1;
    }
    PG_LOG("game data loaded from %s", root.c_str());
#ifdef __EMSCRIPTEN__
    // The web pages (exit.html) show which copy the browser holds.
    EM_ASM({ try { localStorage.setItem('pagan.copy', UTF8ToString($0)); } catch (e) {} }, compat::about_line().c_str());
#endif
    if (!r->opt.write_known_table.empty()) {
        FILE* f = std::fopen(r->opt.write_known_table.c_str(), "wb");
        if (!f) { PG_LOG("cannot write %s", r->opt.write_known_table.c_str()); return 1; }
        std::string t = compat::known_table_text(fs, r->data);
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
        PG_LOG("wrote %s", r->opt.write_known_table.c_str());
        return 0;
    }
    {
        // Engine-made actor shapes (data/shapes/, NOTES.md "Custom shapes"; Nerissa's own art).
        std::string m = find_data_file(argc > 0 ? argv[0] : nullptr, "shapes/shapes.json"), err;
        if (!m.empty()) {
            int n = r->data.load_custom_shapes(m.substr(0, m.size() - std::string("/shapes.json").size()), &err);
            if (!err.empty()) PG_LOG("custom shapes: %s", err.c_str());
            else PG_LOG("custom shapes: %d loaded", n);
        }
    }
    {
        // Script and text fixes of original bugs (usecode/uc_patches.h, data/patches/; used while
        // the "Original bug fixes" option is On).
        std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "patches/usecode.json"), err;
        if (f.empty()) PG_LOG("story patches: data/patches/ not found");
        else if (!load_story_patches(r->data.usecode, f.substr(0, f.find_last_of('/')), err)) PG_LOG("story patches: %s", err.c_str());
        // New map content (world/map_patches.h); without the file the built-in copy stays.
        f = find_data_file(argc > 0 ? argv[0] : nullptr, "patches/map_content.json");
        if (!f.empty() && !map_patches::load_file(f, err)) PG_LOG("map patches: %s: %s", f.c_str(), err.c_str());
    }

    std::string layout_path = r->opt.atlas_layout.empty() ? find_atlas_layout(argc > 0 ? argv[0] : nullptr) : r->opt.atlas_layout;
    if (r->opt.world == 1 && r->opt.atlas_layout.empty()) {
        // --world stitched with the viewer / atlas / export: the stitched world's layout (layers).
        std::string wl = r->opt.world_layout.empty() ? find_data_file(argc > 0 ? argv[0] : nullptr, "world_layout.json") : r->opt.world_layout;
        if (!wl.empty()) layout_path = wl;
    }
    if (!r->opt.write_layout.empty()) {
        AtlasLayout l = generate_atlas_layout(r->data);
        bool ok = l.save(r->opt.write_layout);
        int data_n = 0;
        for (const AtlasGroup& g : l.groups) data_n += g.from_data;
        PG_LOG("%s %s: %zu groups, %d placed from data", ok ? "wrote" : "could not write", r->opt.write_layout.c_str(),
               l.groups.size(), data_n);
        return ok ? 0 : 1;
    }
    if (!r->opt.write_world_layout.empty()) return write_world_layout(r->data, r->opt.write_world_layout, argc > 0 ? argv[0] : nullptr);
    if (!r->opt.export_atlas.empty()) {
        MapViewer v(r->data);
        v.set_atlas_layout_path(layout_path);
        bool ok = v.atlas().export_png(r->opt.export_atlas, r->opt.export_level, r->opt.export_plain);
        PG_LOG("%s %s", ok ? "exported" : "could not export", r->opt.export_atlas.c_str());
        return ok ? 0 : 1;
    }

    if (!r->opt.export_sound.empty()) {
        std::string spec = r->opt.export_sound;
        bool music = spec[0] == 'm';
        size_t c1 = spec.find(':'), c2 = spec.find(':', c1 + 1);
        int num = std::atoi(spec.c_str() + (music ? 1 : 0));
        if (music) {
            double secs = c2 == std::string::npos ? 30 : std::atof(spec.substr(c1 + 1).c_str());
            std::string out = spec.substr((c2 == std::string::npos ? c1 : c2) + 1);
            std::string sf = r->opt.soundfont.empty() ? find_soundfont(argc > 0 ? argv[0] : nullptr) : r->opt.soundfont;
            std::vector<uint8_t> sfdata;
            r->mixer.set_rate(22050);
            if (sf.empty() || !load_file(sf, sfdata) || !r->mixer.load_soundfont(sfdata.data(), sfdata.size())) {
                PG_LOG("no SoundFont");
                return 1;
            }
            r->mixer.set_music_flex(&r->data.music);
            r->mixer.music_change(num);
            std::vector<int16_t> pcm(size_t(secs * 22050) * 2, 0);
            r->mixer.render(pcm.data(), int(pcm.size() / 2));
            bool ok = write_wav(out, pcm.data(), pcm.size() / 2, 2, 22050);
            PG_LOG("%s music track %d (%.0f s) to %s", ok ? "wrote" : "could not write", num, secs, out.c_str());
            return ok ? 0 : 1;
        }
        std::string out = spec.substr(c1 + 1);
        Sample s;
        if (c1 == std::string::npos || !decode_sonarc(r->data.sound.entry(unsigned(num)), s)) {
            PG_LOG("no sound %d", num);
            return 1;
        }
        std::vector<int16_t> pcm(s.pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((int(s.pcm[i]) - 128) << 8);
        bool ok = write_wav(out, pcm.data(), pcm.size(), 1, s.rate);
        PG_LOG("%s sound %d (%zu samples, %d Hz) to %s", ok ? "wrote" : "could not write", num, s.pcm.size(), s.rate, out.c_str());
        return ok ? 0 : 1;
    }
    r->opt.platform.headless = !r->opt.screenshot.empty();
    r->opt.platform.audio = r->opt.screenshot.empty() && !r->opt.mute;
    // The HUD side panel (PLAN-HUD.md): a new window opens 16:9, so the picture keeps its whole
    // scale with the panel beside it (960 x 720 would leave no room at 3x).
    if (!r->opt.platform.headless && r->opt.platform.window_w <= 0 && !r->opt.platform.fullscreen && r->opt.view_map < 0 &&
        !r->opt.edit && !r->opt.atlas &&
        (r->opt.hud_mode >= 0 ? r->opt.hud_mode == 1 : GameMode::side_panel_setting())) {
        r->opt.platform.window_h = int(r->opt.platform.game_h * r->opt.platform.window_scale * (r->opt.platform.aspect_4_3 ? 1.2 : 1.0));
        r->opt.platform.window_w = r->opt.platform.window_h * 16 / 9;
    }
    if (!r->platform.init(r->opt.platform)) return 1;
    bool want_audio = r->opt.platform.audio || !r->opt.record_audio.empty();
    if (want_audio) {
        // Audio: the mixer at the device's rate (22050 for offline recordings).
        bool live = r->opt.platform.audio &&
                    r->platform.open_audio(44100, [r](int16_t* out, int frames) {
#ifdef __EMSCRIPTEN__
                        double t0 = emscripten_get_now();
                        r->mixer.render(out, frames);
                        double dt = emscripten_get_now() - t0;
                        g_audio_ms_sum += dt;
                        g_audio_ms_max = std::max(g_audio_ms_max, dt);
                        ++g_audio_calls;
#else
                        r->mixer.render(out, frames);
#endif
                    });
        r->mixer.set_rate(live ? r->platform.audio_rate() : 22050);
        std::string sf = r->opt.soundfont.empty() ? find_soundfont(argc > 0 ? argv[0] : nullptr) : r->opt.soundfont;
        std::vector<uint8_t> sfdata;
        if (!sf.empty() && load_file(sf, sfdata) && r->mixer.load_soundfont(sfdata.data(), sfdata.size()))
            PG_LOG("music: SoundFont %s (%zu KB), output %d Hz%s", sf.c_str(), sfdata.size() / 1024, r->mixer.rate(),
                   live ? "" : " (offline)");
        else
            PG_LOG("music: no SoundFont found (data/soundfont/u8_gm.sf2 or --soundfont): music is silent");
    }

#ifdef __EMSCRIPTEN__
    if (r->opt.view_map < 0 && !r->opt.test_pattern && !r->opt.atlas && r->opt.zoom == 0 && r->opt.saves.empty() && !r->opt.edit) {
        web_mount_saves();
        web_saves_mounted = true;
    }
#endif
    r->layout_path = layout_path;
    {
        size_t slash = layout_path.find_last_of('/');
        r->data_dir = slash == std::string::npos ? "data" : layout_path.substr(0, slash);
    }
    if (r->opt.edit) {
        // Edit mode on its own (editor/edit_mode.h): Quit / Esc ends the program.
        r->make_editor();
        r->editing = true;
        r->editor->load_all();
        int lv = r->opt.zoom > 0 ? r->opt.zoom : 2;
        if (r->opt.view_map >= 0) {
            int at[3] = {16384, 16384, 0};
            if (r->opt.have_at) std::copy(r->opt.at, r->opt.at + 3, at);
            r->editor->show_map_point(unsigned(r->opt.view_map), at[0], at[1], at[2], lv);
        } else {
            r->editor->show_map_point(40, 13038, 15042, 48, lv);  // the Tenebrae docks gate
        }
    } else if (r->opt.view_map >= 0 || r->opt.test_pattern || r->opt.atlas || r->opt.zoom > 0 || !r->opt.view_area.empty()) {
        if (r->opt.bug_fixes >= 0) original_bug_fixes() = r->opt.bug_fixes == 1;  // before any map loads
        if (r->opt.underworld >= 0) underworld_entrance() = r->opt.underworld == 1;
        r->viewer = std::make_unique<MapViewer>(r->data);
        unsigned map = 0;
        if (r->opt.view_map >= 0) map = unsigned(r->opt.view_map);
        else if (!r->opt.view_area.empty()) map = 254;  // an empty map
        else {
            for (const NpcRecord& n : r->data.npcs)
                if (n.number == 1) map = n.mapnum;
        }
        r->viewer->open_map(map, r->opt.have_at ? r->opt.at : nullptr);
        if (!r->opt.view_area.empty()) {
            // A dressed area (editor/dresser.h): its items at its origin (or 4096, 4096 when it has none).
            std::vector<ItemRecord> items;
            int o[3] = {0, 0, 0};
            std::string err, text;
            if (FILE* f = std::fopen(r->opt.view_area.c_str(), "rb")) {
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                std::fclose(f);
            }
            if (!parse_dressed(text, items, o, err)) {
                std::fprintf(stderr, "--view-area %s: %s\n", r->opt.view_area.c_str(), err.empty() ? "cannot read" : err.c_str());
                return 1;
            }
            if (!o[0] && !o[1]) o[0] = o[1] = 4096;
            place_items(r->viewer->current_map(), items, o[0], o[1], o[2]);
            if (!r->opt.have_at) {
                int x1 = 0, y1 = 0;
                for (const ItemRecord& it : items) x1 = std::max(x1, int(it.x)), y1 = std::max(y1, int(it.y));
                int at[3] = {o[0] + x1 / 2, o[1] + y1 / 2, o[2]};
                r->viewer->open_map(map, at);
                place_items(r->viewer->current_map(), items, o[0], o[1], o[2]);
            }
        }
        r->viewer->set_test_pattern(r->opt.test_pattern);
        r->viewer->set_roof_hiding(!r->opt.no_roof);
        r->viewer->set_show_editor(r->opt.show_editor);
        if (r->opt.have_mouse) r->viewer->set_mouse(r->opt.mouse[0], r->opt.mouse[1]);
        r->viewer->set_atlas_layout_path(layout_path);
        if (!r->opt.screenshot.empty()) r->viewer->set_render_budget(-1);  // complete pictures
        if (r->opt.zoom > 0) r->viewer->set_zoom(r->opt.zoom);
        if (r->opt.atlas) r->viewer->open_atlas(r->opt.view_map < 0 && !r->opt.have_at);
    } else {
        r->game = std::make_unique<GameMode>(r->data);
        r->game->world().auto_ui = r->opt.auto_ui;
        r->game->world().trace_events = r->opt.trace_events;
        r->game->world().skip_opening = r->opt.skip_opening;
        r->game->world().ask_name = !r->opt.no_name && !r->opt.skip_opening;
        r->game->world().play_intro = r->opt.intro == 1 ||
            (r->opt.intro < 0 && r->opt.screenshot.empty() && !r->opt.skip_opening && r->opt.load_slot <= 1);
        r->game->world().intro_on_new_game =
            r->opt.intro == 1 || (r->opt.intro < 0 && r->opt.screenshot.empty() && !r->opt.skip_opening);
        GameMode::register_options();
        World::register_bugfix_option();
        register_variant_options();  // v2.12 changes, Undead invasion (game/variants.h)
        item_powers::register_options();
        {
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "items/abilities.json"), err;
            if (!f.empty() && !item_powers::load_file(f, err)) PG_LOG("item powers: %s: %s", f.c_str(), err.c_str());
            f = find_data_file(argc > 0 ? argv[0] : nullptr, "items/servant.json");  // the fighting Aerial Servant
            if (!f.empty() && !servant::load_file(f, err)) PG_LOG("aerial servant: %s: %s", f.c_str(), err.c_str());
        }
        if (want_audio) r->game->world().sound.set_mixer(&r->mixer);
        r->game->world().load_options();
        if (r->opt.bug_fixes >= 0) original_bug_fixes() = r->opt.bug_fixes == 1;
        if (r->opt.underworld >= 0) underworld_entrance() = r->opt.underworld == 1;
        {
            // Companions (game/party.h): the data file and the "Companions" / "Party bar" settings.
            Party& party = *r->game->world().party;
            Party::register_options();
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "companions.json"), err;
            if (f.empty()) PG_LOG("party: data/companions.json not found (no companions)");
            else if (!party.load_file(f, err)) PG_LOG("party: %s: %s", f.c_str(), err.c_str());
            // Travel banter between companions (game/banter.h): data/banter.json, banter_points.json.
            else if (!party.banter->load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("banter: %s", err.c_str());
            party.enabled = r->opt.companions >= 0 ? r->opt.companions == 1 : Config::global().get_bool("companions", true);
            party.show_bar = Config::global().get_bool("party_bar", true);
        }
        {
            // Levels and XP (game/leveling.h): the data files and the "Leveling" setting.
            Leveling& lv = *r->game->world().leveling;
            Leveling::register_options();
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "monster_levels.json"), err;
            if (f.empty()) PG_LOG("leveling: data/monster_levels.json not found");
            else if (!lv.load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("leveling: %s", err.c_str());
            lv.enabled = r->opt.leveling >= 0 ? r->opt.leveling == 1 : Config::global().get_bool("leveling", true);
        }
        {
            // The new creatures (game/creatures.h, data/creatures.json): "New creatures" (after Leveling: its rows).
            Creatures& cr = *r->game->world().creatures;
            Creatures::register_options();
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "creatures.json"), err;
            if (f.empty()) PG_LOG("creatures: data/creatures.json not found (no new creatures)");
            else if (!cr.load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("creatures: %s", err.c_str());
            cr.enabled = r->opt.creatures >= 0 ? r->opt.creatures == 1 : Config::global().get_bool("new_creatures", true);
        }
        {
            // The Avatar's virtue scores (game/virtues.h, data/virtues.json): "Virtues".
            Virtues& vs = *r->game->world().virtues;
            Virtues::register_options();
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "virtues.json"), err;
            if (f.empty()) PG_LOG("virtues: data/virtues.json not found (maximum %d)", vs.max_score);
            else if (!vs.load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("virtues: %s", err.c_str());
            vs.enabled = r->opt.virtues >= 0 ? r->opt.virtues == 1 : Config::global().get_bool("virtues", true);
        }
        {
            // Pagan transformed as the virtues grow (game/world_stages.h, data/stages.json, greetings.json).
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "stages.json"), err;
            if (f.empty()) PG_LOG("world stages: data/stages.json not found");
            else if (!r->game->world().stages->load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("world stages: %s", err.c_str());
        }
        {
            // New topics for original NPCs (game/npc_dialog.h, data/dialog/*.json): "New Dialogue".
            NpcDialog& nd = *r->game->world().dialog;
            NpcDialog::register_options();
            std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, "dialog/index.json"), err;
            if (f.empty()) PG_LOG("dialog: data/dialog/ not found (no new topics)");
            else if (!nd.load_dir(f.substr(0, f.find_last_of('/')), err)) PG_LOG("dialog: %s", err.c_str());
            nd.enabled = r->opt.npc_dialog >= 0 ? r->opt.npc_dialog == 1 : Config::global().get_bool("npc_dialog", true);
        }
        {
            // Turn-based combat (game/turn_combat.h): "Combat", "Turns When Hit", "Turn Speed".
            TurnCombat& tc = *r->game->world().turn;
            TurnCombat::register_options();
            tc.load_settings();
            if (r->opt.combat >= 0) tc.enabled = r->opt.combat == 1;
        }
        {
            // The stitched world (game/world_stitch.cpp): "World: Stitched / Original maps".
            World& w = r->game->world();
            World::register_world_options();
            w.world_layout_path = r->opt.world_layout.empty() ? find_data_file(argc > 0 ? argv[0] : nullptr, "world_layout.json")
                                                              : r->opt.world_layout;
            std::string def = World::stitched_by_default() ? "stitched" : "original";
            bool on = r->opt.world >= 0 ? r->opt.world == 1 : Config::global().get("world_mode", def) == "stitched";
            w.set_world_mode(on, w.world_layout_path);
        }
        {
            DebugTools& dt = r->game->debug();
            dt.force(r->opt.debug_mode);
            dt.show_crit = r->opt.show_crit;
            dt.no_autosave_run = r->opt.no_autosave;
            std::vector<std::string> files;
            if (!r->opt.critpath.empty()) {
                size_t p = 0;
                while (p <= r->opt.critpath.size()) {
                    size_t c = r->opt.critpath.find(',', p);
                    if (c == std::string::npos) c = r->opt.critpath.size();
                    if (c > p) files.push_back(r->opt.critpath.substr(p, c - p));
                    p = c + 1;
                }
            } else {
                for (const char* n : {"critpath.json", "map_links.json"}) {
                    std::string f = find_data_file(argc > 0 ? argv[0] : nullptr, n);
                    if (!f.empty()) files.push_back(f);
                }
            }
            if (files.empty()) PG_LOG("critpath: no data (engine/data/critpath.json not found)");
            else dt.load_crit(files);
            // The debug menu's story jump (data/story_jump.json, data/story/checkpoints.pgs).
            std::string sj = find_data_file(argc > 0 ? argv[0] : nullptr, "story_jump.json");
            if (!sj.empty()) dt.load_story(sj);
        }
        {
            World::Controls& c = r->game->world().controls;
            if (r->opt.controls >= 0) c.responsive = r->opt.controls == 1;
            if (r->opt.smooth >= 0) c.smooth_scroll = r->opt.smooth == 1;
            if (r->opt.zoom_step >= 0) c.zoom_step = r->opt.zoom_step;
            r->game->world().debug_assist = r->opt.debug_assist;
            GameMode* g = r->game.get();
            r->game->on_settings_changed = [g]() { g->world().store_options(); };
            r->game->set_atlas_layout_path(layout_path);
        }
        r->game->saves().set_dir(saves_dir(r->opt.saves));
#ifdef __EMSCRIPTEN__
        r->game->saves().on_write = []() { web_sync_saves(); };
#endif
        r->game->set_hud(r->opt.hud);
        r->game->set_roof_hiding(!r->opt.no_roof);
        r->game->set_show_editor(r->opt.show_editor);
        if (r->opt.have_mouse) r->game->set_mouse(r->opt.mouse[0], r->opt.mouse[1]);
        if (!r->opt.play.empty() && !r->game->set_script(r->opt.play)) { PG_LOG("bad --play script"); return 2; }
        // The start: a new game (with the intro), a --load slot, or, when saves exist, the
        // start menu over the most recent one (GameMode::launch). On the web the saves come
        // from IndexedDB asynchronously, so the start waits for them (web_frame).
        const Options& o = r->opt;
        bool start_menu = o.start_menu == 1 ||
                          (o.start_menu < 0 && o.load_slot <= 1 && o.intro != 1 && o.screenshot.empty() &&
                           !o.skip_opening && !o.have_warp && o.play.empty());
        r->boot = [r, start_menu]() {
            int loaded = r->game->launch(start_menu && r->opt.load_slot <= 1);
            if (r->opt.load_slot > 1 && !r->game->load_slot(r->opt.load_slot)) PG_LOG("--load %d failed", r->opt.load_slot);
            if (r->opt.screenshot.empty() && !loaded) r->game->toast("F1: help", 150);
            if (r->opt.have_warp) {
                World& w = r->game->world();
                w.teleport_actor(1, r->opt.warp[0], r->opt.warp[1], r->opt.warp[2], unsigned(r->opt.warp[3]));
                w.stasis = false;
            }
        };
#ifdef __EMSCRIPTEN__
        if (!(web_saves_mounted && (start_menu || r->opt.load_slot > 1))) { r->boot(); r->boot = nullptr; }
#else
        r->boot();
        r->boot = nullptr;
#endif
    }
    r->last_ms = r->platform.ticks_ms();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(web_frame, 0, false);
    return 0;
#else
    while (!r->quit) {
        uint64_t start = r->platform.ticks_ms();
        r->frame();
        if (r->opt.screenshot.empty()) {
            uint64_t spent = r->platform.ticks_ms() - start;
            if (spent < 16) SDL_Delay(Uint32(16 - spent));
        }
    }
    int code = r->exit_code;
    r->viewer.reset();
    r->game.reset();
    r->platform.shutdown();
    delete r;
    return code;
#endif
}
