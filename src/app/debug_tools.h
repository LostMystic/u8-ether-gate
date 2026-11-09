// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Debug mode and the things that hang off the critical path:
// - the debug menu (F10 when "Debug mode" is on in the options), its keyboard shortcuts and
//   overlays (critical-path line and hint, collision boxes, pathfinder routes), pause, frame
//   step and world speed, cheats (noclip, god mode, infinite mana), warps and teleports;
// - the critical-path tracker (game/critpath.h), which runs in normal play too: a few times a
//   second it evaluates the steps, and a step completed in play schedules an autosave;
// - autosaves: three rotating slots (SaveStore::kAutoFirst..), taken at the next safe moment,
//   with a small "Autosaved" note; the death menu offers "Load last autosave".
// The game mode owns one (GameMode::debug()).
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/critpath.h"
#include "game/debug_menu.h"
#include "game/route_guide.h"
#include "game/story_pack.h"

namespace pg {

class GameMode;
class Surface8;
struct InputEvent;

class DebugTools {
public:
    explicit DebugTools(GameMode& gm);

    // Debug mode: the "debug_mode" setting (options page), or forced by --debug.
    bool enabled() const;
    void force(bool on) { forced_ = on; }
    void set_enabled(bool on);  // writes the setting

    // Input first seen by the game mode; true = consumed.
    bool handle(const InputEvent& ev);
    // World frames to run this game frame: 0 paused (or 1 for a frame step), 1, 2 or 4.
    int world_frames();
    // After the world ran: refills (god mode, mana), the crit-path tracker, the autosave.
    void tick();
    // Drawn on the UI layer under the windows (lines, boxes) / above them (texts).
    void draw_under(Surface8& s);
    void draw_over(Surface8& s);
    // The atlas opened for a warp: a click there warps (true = consumed, close the atlas).
    bool atlas_jump(unsigned map, const int at[3]);
    // A new game or a load: the tracker starts afresh (nothing it sees then is "completed").
    void world_reset();

    void open_menu();   // F10 (the controller binding belongs to milestone 6: call this)
    void close_menu();
    DebugMenuGump* menu() const;
    // Runs a shortcut action by name (the menu and the keys share these).
    void toggle(const std::string& what);

    // --- The critical path (loaded at start; specs/data via engine/data).
    CritPath crit;
    WorldCritContext ctx;
    bool load_crit(const std::vector<std::string>& files);  // replaces the data
    CritGuide guide;               // refreshed with the tracker
    // The walking route to the guide's point (game/route_guide.h): planned in slices while the
    // line is shown in route mode ("Crit path: route line / straight line", setting crit_route).
    std::unique_ptr<RouteGuide> route;
    bool route_line() const;
    void set_route_line(bool on);
    double route_budget_ms = 0;    // planning time per frame (0: 1.5 ms on the web, 2 ms native)
    std::string route_hint() const;  // the hint's route line ("" when none)
    std::vector<bool> natural_done;  // done_when results without debug overrides
    int evaluations = 0;

    // --- Autosave
    bool autosave_enabled() const;
    bool no_autosave_run = false;  // --no-autosave
    void set_autosave(bool on);
    bool autosave_pending() const { return autosave_pending_; }
    int autosaves_written = 0;
    std::string last_autosave_label;
    int last_autosave_slot = 0;
    bool last_autosave_safe = false;  // taken at a strictly safe moment (not after the 60 s wait)
    int last_autosave_frame = -1;     // the game mode frame it was taken at
    bool safe_to_save() const;      // the strict rule (not in a scene, combat, falling...)
    std::string unsafe_reason() const;  // "" when safe
    bool autosave_now(const std::string& title);  // writes the next rotating slot
    bool load_last_autosave();
    std::string pending_title() const { return pending_title_; }

    // --- State (public for the tests and the menu)
    bool show_crit = false, show_boxes = false, show_routes = false;
    bool paused = false;
    int step_pending = 0;
    int speed = 1;
    bool god = false, infinite_mana = false;
    enum Pick { kPickNone, kPickWarp, kPickKill } pick = kPickNone;
    bool atlas_warp = false;
    bool editor_requested = false;  // the "Editor" row: main opens the edit mode (editor/edit_mode.h)
    unsigned global_pos = 89, global_width = 1;
    int give_shape = 1, give_frame = 0, give_qty = 1;
    std::string message;   // a short result shown above the hint
    int message_frames = 0;
    void say(const std::string& m, int frames = 90);

    // Actions (the menu, the shortcuts and the tests use these).
    bool warp_to(unsigned map, int x, int y, int z);   // with a free-spot search
    bool teleport_to_npc(unsigned npc);
    // Gives the Avatar one item (a stack of qty for coins / reagents), joining a matching stack
    // in his backpack; into the pack whatever he already carries (a cheat), at his feet when the
    // pack has no room. Says what happened (menu status line, message log).
    bool give(unsigned shape, unsigned frame, int qty, int quality = -1, const std::string& name = "");
    static constexpr int kCoinsPerClick = 500;  // money (quantity family: obsidian) per list click
    void heal();
    bool kill(uint16_t id);
    void advance_hours(int h);
    unsigned hour() const;
    bool save_snapshot();
    bool load_snapshot();

    // --- Story jump (data/story_jump.json, data/story/checkpoints.pgs; NOTES.md "Story jump"):
    // a chapter is the world the full-story replay saved there (or a new game for the first),
    // then made realistic: no invulnerability, the pacing level and XP (Leveling on) or the usual
    // stats (off), the chapter's weapons, armour, obsidian and potions. Settings are not in a save,
    // so they stay as they are; the virtues and the companions walking with the Avatar carry over
    // as the page says.
    struct StoryChapter {
        std::string id, title, checkpoint, kit;  // checkpoint "": a new game
        int step = 0, level = 1, xp = 0;
        std::array<int, 3> stats_original{15, 15, 15};
        std::array<int, 8> virtues{};
        std::vector<std::array<int, 3>> equip, pack, remove;  // shape, frame, count (remove: frame -1 any)
    };
    std::vector<StoryChapter> chapters;
    std::string story_pack_path;  // the checkpoints pack (read at the first jump)
    bool load_story(const std::string& json_path);  // story_jump.json; the pack beside it
    bool have_checkpoints() const;
    enum JumpVirtues { kKeepVirtues, kTypicalVirtues, kReplayVirtues } jump_virtues = kKeepVirtues;
    bool jump_companions = true;  // the companions walking with the Avatar come along
    bool jump_to_chapter(int i);  // now (the menu defers it until it has closed)
    int chapter_now() const;      // the chapter the story is in (by the current step), -1 if none
    void typical_virtues(int chapter);
    std::string jump_error;

private:
    DebugPage root_page();
    DebugPage steps_page();
    DebugPage maps_page();
    DebugPage npcs_page();
    DebugPage companions_page();
    DebugPage items_page();
    DebugPage globals_page();
    DebugPage virtues_page();  // the Avatar's virtue scores (game/virtues.h)
    DebugPage levels_page();   // the Avatar's and the companions' levels (game/leveling.h)
    DebugPage story_page();    // jump to a chapter of the story
    void apply_chapter(const StoryChapter& c, const std::array<int, 8>& scores, const std::set<std::string>& paid,
                       const std::vector<std::string>& companions);
    void draw_crit(Surface8& s);
    void draw_boxes(Surface8& s);
    void draw_routes(Surface8& s);
    bool draw_route_line(Surface8& s);  // false: no usable route, draw the straight line
    void to_screen(int x, int y, int z, int& sx, int& sy) const;
    void evaluate(bool baseline);
    bool place_avatar_near(int x, int y, int z);

    GameMode& gm_;
    bool forced_ = false;
    bool was_enabled_ = false;
    bool baseline_ = true;      // the next evaluation only records (after a reset)
    int eval_timer_ = 0;
    int frame_ = 0;
    bool autosave_pending_ = false;
    std::string pending_title_;
    int pending_frames_ = 0;
    int autosaved_note_ = 0;
    bool set_invincible_ = false;
    std::string pending_;  // an action run once the menu has closed (snapshots)
    int pending_jump_ = -1;  // a chapter to jump to once the menu has closed
    std::unique_ptr<StoryPack> story_;
};

}  // namespace pg
