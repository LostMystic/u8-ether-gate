// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Engine-side dialogue added to the original NPCs (the engine's own writing, not from the game).
// Each NPC with extra talk has a data file in engine/data/dialog/ (e.g. gwillim.json): an extra
// answer ("the hook") that is put into that NPC's own answer menu, and a tree of topics behind it
// whose lines are the NPC's barks and whose choices are the answer menu, like the companions'
// talk. Topics depend on the story so far: "stages" (defined in the file from critical-path steps,
// data/critpath.json, and verified globals) and per-topic conditions.
//
// How it works. Each top-level topic is an answer of its own in the NPC's own menu, labelled by
// its subject ("Mordea", "The lava river"); it appears at its point in the story and is replaced by
// the newer one: topics in the same "slot" replace each other (the last one in the file whose
// condition holds wins), and at most `max_entries` slots (the newest) are shown at a time. The
// original conversation's usecode process asks with item_ask (0x04A) and waits on the menu's
// notify process. World::ask calls on_ask(): for an NPC with a file, on the first menu of a
// conversation (and on a menu holding only exits, like "Farewell."), the entries are added (new
// strings in the menu's private list, before the exits). When the player picks one,
// World::choose_answer hands it to intercept(): the original menu is put aside (its process keeps
// waiting), the NPC says the topic's lines (barks), its sub-answers are the answer menu, and the
// leave answer (or a topic without sub-answers) puts the original menu back as it was (the usecode
// never sees the added answers). Combat (World::end_conversation), the conversation's process
// ending, a map change or a load all end the tree cleanly; saving is refused while the tree is up
// (World::can_save). Entries are matched by their text, so a menu saved with one still works after
// loading. Nothing is saved.
//
// With `enabled` off (the default of a bare World; main sets it from the "npc_dialog" setting)
// no menu is changed: the original game. Scene patches (`scenes`: new lines that fill a state an
// original scene has no lines for, e.g. Beren at the Mouth of the Daemon) are bug fixes: they follow
// the "Original bug fixes" option (core/bugfix.h) instead, whenever the data is loaded.
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/avatar_title.h"
#include "game/critpath.h"

namespace pg {

class World;
struct AskMenu;

// A condition: every field that is set must hold.
struct DialogCond {
    std::vector<std::string> stage_in;          // the current stage is one of these
    std::string from_stage, before_stage;       // at or after / strictly before a stage (file order)
    std::vector<std::string> step_done, step_not_done;  // critical-path step ids
    std::vector<std::string> asked, not_asked;  // topic ids asked this session
    std::vector<std::string> marked, not_marked;  // Virtues marks (saved; game/virtues.h): "mark", "not_mark"
    std::vector<CritCond> cond;                 // critical-path conditions (globals, npc_dead, ...)
    int avatar_int = 0;                         // "avatar_int": the Avatar's INT is at least this
    int virtue_total = 0;                       // "virtue_total": the eight virtues add up to at least this
    bool empty = true;
};

struct DialogTopic {
    std::string id, label;
    DialogCond when;
    std::vector<std::string> say;
    std::vector<std::pair<DialogCond, std::vector<std::string>>> alt;  // the first that holds replaces say
    std::vector<DialogTopic> topics;  // a sub-menu (the file's back / leave label is added)
    bool leave = false;               // after the lines: back to the NPC's own menu
    std::string slot;                 // top level: entries of one slot replace each other ("" = the id)
    // "feature": the topic belongs to an engine feature and shows while it is on, whatever "New
    // Dialogue" says ("leveling": Leveling active; game/stat_growth.h). "" = New Dialogue.
    std::string feature;
    // "action": the lines and sub-answers come from the engine when it is picked
    // (StatGrowth::dialog_action), e.g. a trainer's session.
    std::string action;
    // "virtue": {"Honesty": 4, ...}: picking it pays those virtues once per game (game/virtues.h;
    // key "dialog/<file id>:<topic id>"). PLAN-VIRTUES.md 3: the Avatar's kinder or truer choices.
    std::vector<std::pair<int, int>> virtue;
    // "cost": obsidian the Avatar gives when picking it (the party's, as a trainer takes it). Without
    // enough, "cant_pay" is said instead (or a default line) and nothing else happens.
    int cost = 0;
    std::vector<std::string> cant_pay;
    // "mark": a saved note that the Avatar did this (Virtues marks: a promise a later deed checks,
    // an epilogue line). Set only while Virtues is on.
    std::string mark;
    // "vice": infamy points for a cruel answer (Virtues::sin, key "dialog/<file id>:<topic id>");
    // the tyrant ending needs mostly bad acts (PLAN-VIRTUES.md 6).
    int vice = 0;
};

// What an action topic says and offers (built by the engine when it is picked).
struct DialogAction {
    std::vector<std::string> say;
    std::vector<DialogTopic> topics;  // the next menu; empty = back to the NPC's own menu after the lines
};

struct DialogStage {
    std::string id, title;
    DialogCond when;  // the current stage is the last one in the file whose condition holds
};

// A scene patch (a data fix for an original scene): when a script of usecode class `cls` makes the
// NPC speak for the first time and `when` holds, the scene's lines (and its answer menu, `topics`)
// come first, then the script's own line; the script waits meanwhile (it waits on the speech).
struct DialogScene {
    std::string id, cls;
    DialogCond when;
    int from_ip = -1;   // the first line must be spoken at or after this code offset of the class
    DialogTopic topic;  // say + topics (each answer ends the patch after its lines)
    // "line": the script's line the scene goes before (text, trailing spaces ignored); "" = any.
    std::string line;
    // "feature": a scene of an engine feature ("virtues"), shown while it is on, instead of a bug
    // fix (which follows "Original bug fixes").
    std::string feature;
    // "replace": the scene takes the place of the script's lines: its own line is not said, and
    // every later line of that script process is silent (the script goes on and ends as usual).
    // Its answers are a full tree (a topic's "leave" ends the scene).
    bool replace = false;
};

struct NpcDialogDef {
    std::string id, name;
    unsigned npc = 0;                  // the NPC's actor number
    unsigned shape = 0;                // or "shape": every actor of that shape without a file of its own
                                       // (e.g. the egg-spawned children, 708)
    int max_entries = 1;               // extra answers in the NPC's menu at a time
    bool hook_first = true;            // on the first menu of each conversation that has a goodbye
    bool hook_exits = true;            // on a menu holding only exits ("Farewell.")
    std::string back_label = "Back.", leave_label = "Something else.";
    std::vector<DialogStage> stages;
    std::vector<DialogTopic> topics;
    std::vector<DialogScene> scenes;
    // "hide": answers of the NPC's own menu taken out while a feature is on (text, feature), e.g.
    // Darion's sparring session, which gives nothing with Leveling on.
    std::vector<std::pair<std::string, std::string>> hide;
};

class NpcDialog {
public:
    explicit NpcDialog(World& w);
    ~NpcDialog();

    bool enabled = false;
    std::vector<NpcDialogDef> defs;
    std::string data_dir;  // where critpath.json is found (the dialog folder's parent)

    // Loading: one NPC per file; a folder loads every *.json in it (files listed in index.json
    // first if there is one). False with `err` on a parse error.
    bool load_text(const std::string& text, std::string& err);
    bool load_file(const std::string& path, std::string& err);
    bool load_dir(const std::string& dir, std::string& err);
    int find_def(unsigned npc) const;

    // --- World hooks (world_ui.cpp)
    // A menu is being built for `owner` (the item the asking process runs for) by process `pid`:
    // adds the hook's answer to m.answers and its private list (before the exits) when it belongs.
    void on_ask(uint16_t owner, uint16_t pid, AskMenu& m);
    // The item whose conversation owns the current menu (the usecode process waiting on it), 0 if
    // none (an engine menu).
    uint16_t menu_owner(const AskMenu& m) const;
    // The player picked answer `index` of the current menu: true if it was the hook's (the tree
    // starts; the original menu is set aside).
    bool intercept(unsigned index);
    // The NPC is about to speak `text` (World::bark, from the running script): a scene patch may
    // take over; returns the pid the script should wait on (0 = speak as usual).
    uint16_t on_bark(uint16_t speaker, const std::string& text);
    void frame();      // runs the tree (World::frame)
    void abort(const char* why);  // ends the tree at once; the original menu is put back
    void reset();      // the world was replaced (load, new game): forget everything
    bool active() const { return talk_.def >= 0; }
    bool in_scene() const { return talk_.def >= 0 && talk_.scene_notify != 0; }
    std::vector<std::string> scenes_played;  // ids (tests)
    uint16_t speaker() const { return talk_.npc; }

    // --- Queries (tests, debug)
    int stage(int def) const;  // index into defs[def].stages, -1 if none holds
    std::string stage_id(int def) const;
    bool eval(int def, const DialogCond& c) const;
    std::vector<const DialogTopic*> visible_topics() const;  // the tree's current menu
    // The top-level topics shown in the NPC's own menu now (newest last).
    std::vector<const DialogTopic*> entries(int def) const;
    std::vector<const DialogTopic*> visible_topics(int def, const std::vector<DialogTopic>& menu) const;
    // Unknown critical-path steps or stages, unknown condition kinds, empty labels (tests).
    std::vector<std::string> problems() const;
    std::vector<std::string> log;  // what happened (tests)

    // "Everyone knows" (game/avatar_title.h, data/avatar_title.json): loaded by load_dir from the
    // data folder; while Virtues is on and its mark is set, the usecode's own words for the Avatar
    // ("stranger") are rewritten as each script creates them (UCMachine::text_hook), and NPC lines
    // say the title for the Avatar's name (speech()).
    AvatarTitle title;
    bool title_on() const;
    std::string speech(uint16_t speaker, const std::string& text) const;  // World::bark

    static void register_options();
    // Whether topics of a feature show now ("" = New Dialogue's own topics).
    bool feature_on(const std::string& feature) const;

private:
    World& w_;
    struct Talk {
        int def = -1;
        uint16_t npc = 0;
        std::unique_ptr<AskMenu> held;  // the NPC's own menu, set aside
        enum Stage { kNone, kSaying, kMenu } stage = kNone;
        std::vector<std::string> queue;
        uint16_t bark_pid = 0, ask_pid = 0;
        std::vector<uint16_t> answer_sids;
        std::vector<const DialogTopic*> answers;  // nullptr = back / leave
        std::vector<const std::vector<DialogTopic>*> menu_stack;
        bool leave_after = false;
        int picked = -1;  // the answer picked in the tree's menu (World::choose_answer)
        // A scene patch: the notify process the script waits on, the script, its own line.
        uint16_t scene_notify = 0, scene_proc = 0, final_bark = 0;
        std::string scene_text;
        bool final_said = false;
        bool scene_replace = false;  // a "replace" scene (DialogScene::replace)
        uint64_t scene_serial = 0;   // its script process (Process::serial)
        std::vector<std::unique_ptr<std::vector<DialogTopic>>> dyn;  // menus built by actions
    } talk_;
    std::set<uint64_t> scenes_done_;  // script processes already patched (Process::serial)
    std::set<uint64_t> silenced_;     // script processes a "replace" scene took over (Process::serial)
    bool hooked_ = false;             // this object installed the VM's text hook
    bool own_bark_ = false;
    void finish_scene();
    std::set<std::string> asked_;  // "def:topic" this session
    uint64_t conv_serial_ = 0;     // the last conversation process seen asking (Process::serial)
    bool conv_offered_ = false;    // its first menu with a goodbye has had the entries
    void say_lines(const std::vector<std::string>& lines);
    void show_menu();
    void choose(const DialogTopic& t);
    void restore();
    void free_menu();
    const std::vector<std::string>* pick_lines(int def, const std::vector<std::pair<DialogCond, std::vector<std::string>>>& alts,
                                               const std::vector<std::string>* fallback) const;
    std::string fill(const std::string& s) const;
    // Critical path (loaded on first use from data_dir).
    bool step_done(const std::string& id) const;
    mutable bool crit_tried_ = false;
    mutable std::unique_ptr<CritPath> crit_;
    mutable std::vector<bool> crit_done_;
    mutable uint32_t crit_frame_ = 0xFFFFFFFF;
};

}  // namespace pg
