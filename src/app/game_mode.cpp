// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The game mode: draws the world, the gumps and the UI, and routes mouse and keyboard input
// to the gumps (gumps.md 1.2-1.6) or to the player input the Avatar controller reads, plus
// looking, using, combat toggling, drag and drop between the world and the gumps, stack
// splitting, throwing (specs/world/avatar.md 7-8, specs/presentation/input.md), the weapon
// overlay (formats/weapons.md), the menu requests (new game, load, save, quit).
#include "app/game_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/atlas_view.h"
#include "app/debug_tools.h"
#include "app/hud_panel.h"
#include "app/turn_ui.h"
#include "core/config.h"
#include "core/log.h"
#include "core/variants.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/virtues.h"
#include "game/spellbook.h"
#include "game/rock_flesh.h"
#include "game/creatures.h"
#include "game/story_fixes.h"
#include "game/ask_nav.h"
#include "game/combat.h"
#include "game/companion_magic.h"
#include "game/container_sort.h"
#include "game/leveling.h"
#include "game/stat_growth.h"
#include "game/party.h"
#include "game/processes.h"
#include "gfx/surface.h"
#include "world/world_space.h"

namespace pg {

GameMode::GameMode(const GameData& data) : data_(data), world_(std::make_unique<World>(data)), view_(data) {
    world_->saves = &saves_;
    world_->ask_name = true;
    debug_ = std::make_unique<DebugTools>(*this);
    turn_ui_ = std::make_unique<TurnUI>(*this);
    hud_panel_ = std::make_unique<HudPanel>(*this);  // the HUD side panel (app/hud_panel.h)
    // Milestone 6: the main menu's Map and Controls entries, and the control settings on the
    // options page (registered once for every world).
    world_->open_map_ui = [this]() { open_atlas(); };
    world_->open_help_ui = [this]() { open_help(); };
    world_->take_item_ui = [this](uint16_t id) { return take_item(id); };
    // The options page's Zoom row (game/options_page.cpp): the levels of zoom_by, wrapping.
    world_->zoom_value_ui = [this]() {
        int n, d;
        zoom_fraction(n, d);
        return std::to_string(100 * n / d) + "%";
    };
    world_->hint_ui = [this](const std::string& text) { toast(text, 60); };
    world_->zoom_change_ui = [this](int dir) {
        int limit = std::min(zoom_limit(std::max(1, vs_.S)), max_zoom_step(std::max(1, vs_.S)));
        int cur = std::min(world_->controls.zoom_step, limit);
        if (limit > 0 && dir > 0 && cur == limit) zoom_by(-limit);
        else if (limit > 0 && dir < 0 && cur == 0) zoom_by(limit);
        else zoom_by(dir > 0 ? 1 : -1);
    };
    static bool rows_added = false;
    if (!rows_added) {
        rows_added = true;
        auto onoff = [](bool v) { return std::string(v ? "On" : "Off"); };
        auto& rows = World::extra_option_rows();
        rows.push_back([](World& w) {
            World::Controls& c = w.controls;
            return MenuGump::OptionRow{"Controls:", [&c] { return std::string(c.responsive ? "Responsive" : "Classic"); },
                                       [&c](int) { c.responsive = !c.responsive; }};
        });
        rows.push_back([onoff](World& w) {
            World::Controls& c = w.controls;
            return MenuGump::OptionRow{"Smooth Scroll:", [&c, onoff] { return onoff(c.smooth_scroll); },
                                       [&c](int) { c.smooth_scroll = !c.smooth_scroll; }};
        });
        rows.push_back([onoff](World& w) {
            World::Controls& c = w.controls;
            return MenuGump::OptionRow{"Click Text:", [&c, onoff] { return onoff(c.click_text); },
                                       [&c](int) { c.click_text = !c.click_text; }};
        });
        rows.push_back([onoff](World& w) {
            World::Controls& c = w.controls;
            return MenuGump::OptionRow{"Move Assist:", [&c, onoff] { return onoff(c.assist); },
                                       [&c](int) { c.assist = !c.assist; }};
        });
        // The HUD side panel (PLAN-HUD.md): applied by the application each frame.
        rows.push_back([](World&) {
            return MenuGump::OptionRow{"HUD:", [] { return std::string(side_panel_setting() ? "Side panel" : "Classic"); },
                                       [](int) {
                                           Config& c = Config::global();
                                           c.set("hud", side_panel_setting() ? "classic" : "panel");
                                           c.save();
                                       },
                                       "Display"};
        });
        rows.push_back([onoff](World& w) {
            World::Controls& c = w.controls;
            return MenuGump::OptionRow{"Easy Ledges:", [&c, onoff] { return onoff(c.easy_ledges); },
                                       [&c](int) { c.easy_ledges = !c.easy_ledges; }};
        });
    }
}

void GameMode::open_debug_menu() { debug_->open_menu(); }

void GameMode::start() {
    world_->new_game();
    debug_->world_reset();
    saves_.refresh();
}

int GameMode::launch(bool start_menu) {
    start();
    if (!start_menu) return 0;
    std::string how = Config::global().get("on_start", "menu");
    if (how == "intro") return 0;
    saves_.refresh();
    int slot = saves_.latest();
    if (!slot) {
        PG_LOG("start: no saves, a new game");
        return 0;
    }
    if (!load_slot(slot)) return 0;  // a broken save: load_slot has started a new game
    if (how == "continue") {
        PG_LOG("start: continued slot %d", slot);
        toast("Continued: " + saves_.description(slot), 120);
        return slot;
    }
    MenuGump* m = world_->open_menu(MenuGump::kMenu);
    m->start_menu = true;
    m->set_mode(MenuGump::kMenu);  // highlights Continue
    PG_LOG("start: start menu over slot %d \"%s\"", slot, saves_.description(slot).c_str());
    return slot;
}

void GameMode::register_options() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World&) {
        static const char* const kKeys[3] = {"menu", "continue", "intro"};
        static const char* const kNames[3] = {"Main menu", "Continue", "Intro"};
        auto index = [] {
            std::string v = Config::global().get("on_start", "menu");
            for (int i = 0; i < 3; ++i)
                if (v == kKeys[i]) return i;
            return 0;
        };
        MenuGump::OptionRow r{"On Start:", [index] { return std::string(kNames[index()]); },
                              [index](int dir) {
                                  Config& c = Config::global();
                                  c.set("on_start", kKeys[(index() + (dir < 0 ? 2 : 1)) % 3]);
                                  c.save();
                              }};
        r.group = "Game";
        r.kind = MenuGump::OptionRow::kChoice;
        return r;
    });
}

bool GameMode::ask_hit(int mx, int my, unsigned& index) const {
    const AskMenu* m = world_->ask_menu.get();
    if (!m) return false;
    for (size_t i = 0; i < m->buttons.size(); ++i) {
        const auto& b = m->buttons[i];
        if (mx >= ask_x_ + b.x && mx < ask_x_ + b.x + b.w && my >= ask_y_ + b.y && my < ask_y_ + b.y + b.h) {
            index = unsigned(i);
            return true;
        }
    }
    return false;
}

bool GameMode::world_point(int px, int py, uint16_t& id, int pt[3]) {
    World& w = *world_;
    // The pointer itself is known to a fraction of a pixel (a zoomed view needs it).
    float fx = px + 0.5f, fy = py + 0.5f;
    if (px == mouse_x_ && py == mouse_y_ && mouse_fx_ >= 0) { fx = mouse_fx_; fy = mouse_fy_; }
    int wx, wy;
    screen_to_world_px(fx, fy, wx, wy);
    id = pick_f(fx, fy);  // through the view (zoom, far zoom)
    const Item* it = w.item(id);
    if (!it) return false;
    Box b = w.box_of(*it);
    int camSx, camSy;
    if (vs_.layered) {
        camSx = vs_.cam_sx;
        camSy = vs_.cam_sy;
    } else {
        int cam[3];
        w.camera_point(cam);
        camSx = (cam[0] - cam[1]) / 4;
        camSy = (cam[0] + cam[1]) / 8 - cam[2];
    }
    int u = wx - vs_.ww / 2 + camSx, v = wy - vs_.wh / 2 + camSy, z = b.z + b.zd;
    int x = 2 * u + 4 * (v + z), y = 4 * (v + z) - 2 * u;
    pt[0] = std::clamp(x, b.x - b.xd + 1, b.x);
    pt[1] = std::clamp(y, b.y - b.yd + 1, b.y);
    pt[2] = z;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Savegames

bool GameMode::save_slot(int slot, const std::string& description) {
    World& w = *world_;
    SaveWriter out;
    w.save_game(out, description);
    bool ok = saves_.write(slot, out.data());
    PG_LOG("save: slot %d \"%s\" (%zu bytes) %s", slot, description.c_str(), out.data().size(), ok ? "ok" : "FAILED");
    return ok;
}

bool GameMode::load_slot(int slot) {
    std::vector<uint8_t> data;
    SaveHeader h;
    if (!saves_.read(slot, data) || !SaveHeader::read(data, h)) {
        PG_LOG("load: slot %d is empty or not a savegame", slot);
        return false;
    }
    if (!load_bytes(data, "slot " + std::to_string(slot))) return false;
    saves_.last_slot = slot;
    PG_LOG("load: slot %d \"%s\"", slot, h.description.c_str());
    return true;
}

bool GameMode::load_bytes(const std::vector<uint8_t>& data, const std::string& what) {
    World& w = *world_;
    cancel_drag();
    walk_ = AutoWalk{};
    for (Gump*& g : press_gump_) g = nullptr;
    hover_gump_ = nullptr;
    SaveReader in(data.data(), data.size());
    bool ok = w.load_game(in);
    if (!ok) {
        PG_LOG("load: %s failed; starting a new game", what.c_str());
        w.new_game();
        debug_->world_reset();
        return false;
    }
    debug_->world_reset();
    return true;
}

void GameMode::handle_requests() {
    World& w = *world_;
    World::Request r = w.request;
    if (r.kind == World::Request::kNone) return;
    w.request = World::Request{};
    switch (r.kind) {
    case World::Request::kQuit: quit_ = true; break;
    case World::Request::kNewGame:
        // "New Game" on the start menu plays the intro; diary entry 1 does not (gumps.md 2.4).
        w.play_intro = r.slot == World::Request::kNewGameIntro && w.intro_on_new_game;
        cancel_drag();
        walk_ = AutoWalk{};
        for (Gump*& g : press_gump_) g = nullptr;
        hover_gump_ = nullptr;
        w.new_game();
        debug_->world_reset();
        break;
    case World::Request::kLoad: load_slot(r.slot); break;
    case World::Request::kSave:
        w.gumps.reap();
        if (w.can_save()) save_slot(r.slot, r.description);
        else PG_LOG("save refused: not allowed now");
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Input scripts

namespace {
int key_from_name(const std::string& k) {
    if (k == "up") return kKeyUp;
    if (k == "down") return kKeyDown;
    if (k == "left") return kKeyLeft;
    if (k == "right") return kKeyRight;
    if (k == "space") return kKeySpace;
    if (k == "esc") return kKeyEscape;
    if (k == "return" || k == "enter") return kKeyReturn;
    if (k == "backspace") return kKeyBackspace;
    if (k == "shift") return -1;
    if (k == "ctrl") return -2;
    if (k == "alt") return -3;
    if (k.size() >= 2 && k[0] == 'f' && std::isdigit(static_cast<unsigned char>(k[1]))) return kKeyF1 + std::atoi(k.c_str() + 1) - 1;
    return k.empty() ? 0 : k[0];
}

bool mergeable(const World& w, const Item& a, const Item& b) {
    if (a.shape != b.shape || a.id == b.id) return false;
    unsigned fam = w.data().typeflags.get(a.shape).family();
    if (fam == kFamQuantity) return true;
    if (fam != kFamReagent) return false;
    auto kind = [&](unsigned f) -> int {
        // Reagent frames counted as one kind (avatar.md 8.4).
        static const int necro[][2] = {{0, 5}, {6, 7}, {10, 12}, {14, 15}, {16, 20}};
        static const int sorc[][2] = {{0, 1}, {2, 5}, {6, 9}, {10, 13}, {14, 17}, {18, 20}};
        if (a.shape == 395)
            for (int i = 0; i < 5; ++i) if (int(f) >= necro[i][0] && int(f) <= necro[i][1]) return 100 + i;
        if (a.shape == 398)
            for (int i = 0; i < 6; ++i) if (int(f) >= sorc[i][0] && int(f) <= sorc[i][1]) return 200 + i;
        return int(f);
    };
    if (necro_elbow_apart(a.shape, a.frame, b.frame)) return false;  // Vividos' elbow (game/story_fixes.h)
    return kind(a.frame) == kind(b.frame);
}

bool is_stack(const World& w, const Item& it) {
    unsigned fam = w.data().typeflags.get(it.shape).family();
    return fam == kFamQuantity || fam == kFamReagent;
}

// v2.12 stacking (specs/world/v212_features.md 3.3, "v2.12 changes" On): a reagent or other
// stack the player puts into a container joins the first compatible stack already there (up to
// 666; the rest stays a stack of its own); the receiving stack keeps its frame and gets the
// combine event. The dead man's elbow (395:20) never joins the wooden sticks (v2.12 #11).
// Only the player's own moves: a script that moves an item keeps its id.
uint16_t v212_auto_merge(World& w, uint16_t id) {
    if (!v212_changes()) return 0;
    Item* it = w.item(id);
    if (!it || !it->parent || !is_stack(w, *it)) return 0;
    const Item* cont = w.item(it->parent);
    if (!cont) return 0;
    for (uint16_t k : cont->contents) {
        Item* t = w.item(k);
        if (!t || k == id || !mergeable(w, *it, *t) || t->quality >= 666) continue;
        if (t->shape == 395 && (t->frame == 20) != (it->frame == 20)) continue;
        unsigned total = unsigned(t->quality) + std::max<unsigned>(1, it->quality);
        t->quality = uint16_t(std::min(666u, total));
        unsigned left = total > 666 ? total - 666 : 0;
        PG_LOG("v2.12 stacking: item %u (shape %u frame %u) joined stack %u in container %u (now %u)", id,
               unsigned(it->shape), unsigned(it->frame), k, unsigned(cont->id), unsigned(t->quality));
        w.call_event(k, kEvCombine);
        if (left == 0) w.destroy(id);
        else if ((it = w.item(id))) it->quality = uint16_t(left);
        return k;
    }
    return 0;
}
// Obsidian given to a companion (their portrait, or onto them) joins the pile already in their
// pack, whatever the v2.12 option says, so their pack does not fill with piles of coins (the
// reagent restock counts them all anyway; game/companion_magic.h). Companions are the engine's own.
void join_companion_money(World& w, uint16_t id) {
    Item* it = w.item(id);
    if (!it || it->shape != StatGrowth::kMoneyShape || !it->parent) return;
    const Item* cont = w.item(it->parent);
    if (!cont) return;
    for (uint16_t k : cont->contents) {
        Item* t = w.item(k);
        if (!t || k == id || t->shape != it->shape || t->quality >= 666) continue;
        unsigned total = unsigned(t->quality) + std::max<unsigned>(1, it->quality);
        t->quality = uint16_t(std::min(666u, total));
        unsigned left = total > 666 ? total - 666 : 0;
        w.call_event(k, kEvCombine);
        if (left == 0) w.destroy(id);
        else if ((it = w.item(id))) { it->quality = uint16_t(left); w.call_event(id, kEvCombine); }
        return;
    }
}
}  // namespace

bool GameMode::set_script(const std::string& s) {
    script_.clear();
    script_next_ = 0;
    size_t pos = 0;
    unsigned mods = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string e = s.substr(pos, end - pos);
        pos = end + 1;
        if (e.empty()) continue;
        size_t c1 = e.find(':');
        if (c1 == std::string::npos) return false;
        int frame = std::atoi(e.c_str());
        size_t c2 = e.find(':', c1 + 1);
        std::string act = e.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        std::string arg = c2 == std::string::npos ? "" : e.substr(c2 + 1);
        InputEvent ev;
        ev.x = mouse_x_;
        ev.y = mouse_y_;
        if (act == "move") {
            ev.type = InputEvent::kMouseMove;
            if (std::sscanf(arg.c_str(), "%d,%d", &ev.x, &ev.y) != 2) return false;
        } else if (act == "ldown" || act == "rdown" || act == "mdown" || act == "lup" || act == "rup" || act == "mup") {
            ev.type = act[1] == 'd' ? InputEvent::kMouseDown : InputEvent::kMouseUp;
            ev.button = act[0] == 'l' ? 1 : act[0] == 'm' ? 2 : 3;
            ev.x = ev.y = -9999;  // at the current pointer
        } else if (act == "key" || act == "keyup") {
            ev.type = act == "key" ? InputEvent::kKeyDown : InputEvent::kKeyUp;
            int k = key_from_name(arg);
            unsigned bit = k == -1 ? kModShift : k == -2 ? kModCtrl : k == -3 ? kModAlt : 0;
            if (bit) { mods = act == "key" ? (mods | bit) : (mods & ~bit); k = 0; }
            ev.key = k;
            ev.mods = mods;
        } else if (act == "type") {
            ev.type = InputEvent::kText;
            std::strncpy(ev.text, arg.c_str(), sizeof(ev.text) - 1);
            // Longer text: one event per 7 characters.
            for (size_t k = 7; k < arg.size(); k += 7) {
                script_.push_back({frame, ev, ""});
                std::memset(ev.text, 0, sizeof ev.text);
                std::strncpy(ev.text, arg.c_str() + k, sizeof(ev.text) - 1);
            }
        } else if (act == "warp" || act == "spawn" || act == "equip" || act == "give" || act == "save" ||
                   act == "load" || act == "shot" || act == "debug" || act == "global") {
            script_.push_back({frame, ev, act + ":" + arg});
            continue;
        } else {
            return false;
        }
        script_.push_back({frame, ev, ""});
    }
    std::stable_sort(script_.begin(), script_.end(), [](const Scripted& a, const Scripted& b) { return a.frame < b.frame; });
    return true;
}

// Debug actions of the input script (play-testing and screenshots only).
void GameMode::run_debug(const std::string& cmd) {
    World& w = *world_;
    Actor* av = w.avatar();
    if (!av) return;
    int a = 0, b = 0, c = 0, m = 0;
    char text[128] = {};
    if (cmd.rfind("warp:", 0) == 0 && std::sscanf(cmd.c_str() + 5, "%d,%d,%d,%d", &a, &b, &c, &m) == 4) {
        w.teleport_actor(1, a, b, c, unsigned(m));
        w.camera_follow(1);
    } else if (cmd.rfind("spawn:", 0) == 0 && std::sscanf(cmd.c_str() + 6, "%d,%d,%d", &a, &b, &c) == 3) {
        uint16_t id = w.create_monster(unsigned(a), 0);
        if (Item* it = w.item(id)) {
            it->flags |= kItemFastOnly | kItemDisposable;
            w.move_to(id, av->x + b, av->y + c, av->z);
            w.set_activity(id, 1);
        }
    } else if (cmd.rfind("equip:", 0) == 0 && std::sscanf(cmd.c_str() + 6, "%d", &a) == 1) {
        uint16_t id = w.create_item(unsigned(a), 0);
        if (!w.equip(1, id)) w.destroy_now(id);
    } else if (cmd.rfind("give:", 0) == 0 && std::sscanf(cmd.c_str() + 5, "%d,%d", &a, &b) >= 1) {
        uint16_t id = w.create_item(unsigned(a), 0);
        if (Item* it = w.item(id); it && b > 0) it->quality = uint16_t(b);
        if (!w.give_to_avatar(id)) w.destroy_now(id);
    } else if (cmd.rfind("save:", 0) == 0 && std::sscanf(cmd.c_str() + 5, "%d,%127[^\n]", &a, text) >= 1) {
        save_slot(a, text[0] ? text : "Script save");
    } else if (cmd.rfind("load:", 0) == 0 && std::sscanf(cmd.c_str() + 5, "%d", &a) == 1) {
        load_slot(a);
    } else if (cmd.rfind("shot:", 0) == 0) {
        shot_requests.push_back(cmd.substr(5));
    } else if (cmd.rfind("global:", 0) == 0 && std::sscanf(cmd.c_str() + 7, "%d,%d,%d", &a, &b, &c) == 3) {
        w.vm().set_global(unsigned(a), unsigned(b), uint16_t(c));  // a story flag (G[a:b] = c)
    } else if (cmd.rfind("debug:", 0) == 0) {
        debug_->toggle(cmd.substr(6));  // a debug-menu action by name (app/debug_tools.cpp)
    }
    PG_LOG("script: %s", cmd.c_str());
}

// ---------------------------------------------------------------------------------------------
// Input

void GameMode::look_item(uint16_t id) {
    World& w = *world_;
    if (!id || !w.item(id)) return;
    w.call_event(id, kEvLook);
}

void GameMode::toggle_combat(bool with_anim) {
    World& w = *world_;
    Actor* av = w.avatar();
    if (!av || w.stasis || av->is(Actor::kDead)) return;
    if (av->is(Actor::kInCombat) || av->is(Actor::kCombatRun)) {
        av->set(Actor::kCombatRun, false);
        w.clear_in_combat(1);
        if (with_anim && !av->is(Actor::kAnimLock)) w.do_anim(1, 6, -1);
    } else {
        w.set_in_combat(1);
        if (with_anim && !av->is(Actor::kAnimLock)) w.do_anim(1, 5, -1);
    }
}

void GameMode::hotkey_use(unsigned shape) { use_first_item(shape); }

bool GameMode::take_item(uint16_t id) {
    World& w = *world_;
    if (!w.item(id) || !w.avatar()) return false;
    const uint16_t pack = w.equipped(1, 7);
    std::string why = pack ? w.put_in_refusal(id, pack) : "No backpack";
    if (why.empty() && !w.move_to_container(id, pack, true)) why = "It won't go in your backpack";
    if (!why.empty()) { w.refuse(why); return false; }
    const std::string name = GameLog::name_of(w, id, false);
    w.theft_check(id);
    v212_auto_merge(w, id);
    w.game_log.add(GameLog::kMessages, GameLog::kInfo, "Taken: " + name + ".");
    return true;
}

uint16_t GameMode::use_first_item(unsigned shape) {
    World& w = *world_;
    if (w.stasis || !w.avatar() || w.avatar()->is(Actor::kDead)) return 0;
    // Inventory order, depth first (a bag's contents before the next item of the backpack).
    std::function<uint16_t(const Item&, int)> find = [&](const Item& c, int depth) -> uint16_t {
        if (depth > 8) return 0;
        for (uint16_t k : c.contents)
            if (const Item* it = w.item(k)) {
                if (it->shape == shape) return k;
                if (uint16_t r = find(*it, depth + 1)) return r;
            }
        return 0;
    };
    uint16_t id = find(*w.avatar(), 0);
    if (!id) return 0;
    PG_LOG("hotkey: use item %u (shape %u)", id, shape);
    w.call_event(id, kEvUse);
    return id;
}

uint16_t GameMode::auto_merge(World& w, uint16_t id) { return v212_auto_merge(w, id); }

bool GameMode::quick_save() {
    World& w = *world_;
    w.gumps.reap();
    const Actor* av = w.avatar();
    if (w.stasis || !av || av->is(Actor::kDead) || !w.can_save()) {
        w.red_cross_frames = 8;  // 250 ms (specs/world/v212_features.md 2.4)
        toast("Cannot quick-save now", 45);
        PG_LOG("quick-save refused (%s)", w.stasis ? "stasis" : !w.can_save() ? "saving not allowed" : "no Avatar");
        return false;
    }
    char desc[96];
    uint32_t t = w.game_seconds();
    std::snprintf(desc, sizeof desc, "Quick save, day %u %02u:%02u", unsigned(t / 86400 + 1), unsigned(t / 3600 % 24),
                  unsigned(t / 60 % 60));
    if (!save_slot(SaveStore::kQuickSlot, desc)) {
        w.red_cross_frames = 8;
        toast("Quick-save failed", 60);
        return false;
    }
    toast("Quick-saved (F8 loads it)", 60);
    return true;
}

bool GameMode::quick_load() {
    World& w = *world_;
    saves_.refresh();
    if (!saves_.exists(SaveStore::kQuickSlot)) {
        w.red_cross_frames = 8;
        toast("No quick-save yet (F7 saves)", 60);
        return false;
    }
    if (!load_slot(SaveStore::kQuickSlot)) return false;
    toast("Quick-loaded", 60);
    return true;
}

void GameMode::key_down(const InputEvent& ev) {
    World& w = *world_;
    PlayerInput& in = w.input;
    in.key_run = ev.mods & kModShift;
    in.key_step = ev.mods & kModCtrl;
    // A modal gump takes every key (gumps.md 1.2-1.3); the target cursor only Escape.
    if (Gump* modal = w.gumps.top_modal()) {
#ifdef __EMSCRIPTEN__
        // The page stops the browser's default for Space (scrolling), so no text event
        // follows it there: type the space here.
        if (ev.key == kKeySpace && modal->text(" ")) return;
#endif
        modal->key(ev.key, ev.mods);
        w.gumps.reap();
        return;
    }
    // The answer menu: the arrow keys move the highlight like the controller's D-pad (the
    // first press only shows it when there is no controller); once they have, Enter answers.
    if (w.ask_menu && !w.ask_menu->answers.empty() && !(ev.mods & (kModAlt | kModCtrl))) {
        ask_sync();
        if (ev.key == kKeyUp || ev.key == kKeyDown || ev.key == kKeyLeft || ev.key == kKeyRight) {
            if (ask_sel_ < 0) ask_sel_ = 0;
            else ask_nav(ev.key == kKeyUp ? kAskUp : ev.key == kKeyDown ? kAskDown : ev.key == kKeyLeft ? kAskLeft : kAskRight);
            ask_keys_ = true;
            return;
        }
        if (ev.key == kKeyReturn && ask_keys_ && !ev.repeat && ask_sel_ >= 0) {
            if (w.ask_menu->frames_open >= kMenuGuardFrames) choose_ask_selection();
            return;
        }
    }
    if (ev.key == kKeyEscape) {
        if (drag_ != kDragNone) cancel_drag();
        else if (w.target_pid) {
            if (Process* p = w.kernel().get(w.target_pid)) { p->result = 0; p->terminate(); }
        } else if (!ev.repeat) {
            w.open_menu(MenuGump::kMenu);
        }
        return;
    }
    if (ev.key == kKeyBackspace) { w.gumps.close_item_dependent(); return; }
    // Milestone 6 keys: help, the atlas, zoom, click-through for barks.
    if (ev.key == kKeyF1 && !ev.repeat) { open_help(); return; }
    // v2.12 hotkeys: F7 quick-save, F8 quick-load (one reserved slot outside the diary).
    if (ev.key == kKeyF7 && !ev.repeat) { quick_save(); return; }
    if (ev.key == kKeyF8 && !ev.repeat) { quick_load(); return; }
    if ((ev.key == 'm' || ev.key == kKeyTab) && !ev.repeat && !(ev.mods & (kModAlt | kModCtrl))) { open_atlas(); return; }
    if ((ev.key == '=' || ev.key == '+') && !(ev.mods & (kModAlt | kModCtrl))) { zoom_by(-1); return; }
    if (ev.key == '-' && !(ev.mods & (kModAlt | kModCtrl))) { zoom_by(1); return; }
    if ((ev.key == kKeyReturn || (ev.key == kKeySpace && scene_running())) && w.controls.click_text && !w.barks.empty()) {
        if (!ev.repeat) bark_click(true);
        text_hold_ = true;
        if (ev.key == kKeyReturn || scene_running()) return;
    }
    if (ev.key == kKeyUp || ev.key == kKeyDown || ev.key == kKeyLeft || ev.key == kKeyRight) {
        int i = ev.key - kKeyUp;
        if (ev.mods & kModAlt) { scroll_[i] = true; return; }
        scroll_[i] = false;
        if (ev.key == kKeyUp) in.key_forward = true;
        if (ev.key == kKeyDown) in.key_back = true;
        if (ev.key == kKeyLeft) in.key_turn_left = true;
        if (ev.key == kKeyRight) in.key_turn_right = true;
    }
    if (ev.key == kKeySpace) {
        in.key_jump = true;
        if (!ev.repeat) in.jump_buffer_ms = std::max<uint32_t>(1, in.now_ms);
    }
    if (ev.repeat) return;
    if ((ev.mods & kModAlt) && ev.key == 'x') { w.gumps.add(std::make_unique<QuitGump>(w)); return; }
    if (ev.mods & (kModAlt | kModCtrl)) return;
    if (ev.key == kKeyHome) w.camera_follow(1);
    if (ev.key == 'h') hud_ = !hud_;
    // R is the recall item (v2.12); roof hiding moved to Shift+R (and the debug menu).
    if (ev.key == 'r' && (ev.mods & kModShift)) opt_.roof_hiding = !opt_.roof_hiding;
    else if (ev.key == 'r') hotkey_use(kRecall);
    if (ev.key == 'e') opt_.show_editor = !opt_.show_editor;
    if (ev.key == 'c') toggle_combat(false);
    if (ev.key == 'b') hotkey_use(kBedroll);
    if (ev.key == 'k') hotkey_use(kKeyring);
    if (ev.key == 'o') w.open_menu(MenuGump::kOptions);
    if (ev.key == 'g' && !w.gumps.top_modal()) w.gumps.add(std::make_unique<SpellbookGump>(w));  // the spellbook (game/spellbook.h)
    if (ev.key == 'p' && w.party && w.party->enabled) { w.party->show_bar = !w.party->show_bar; w.party->update_bar(); }  // companions' party bar
    if (ev.key == 'z' && !w.stasis) w.call_event(1, kEvUse);
    if (ev.key == 'i' && !w.stasis) if (uint16_t p = w.equipped(1, 7)) w.call_event(p, kEvUse);
    // Container sort: the window under the mouse, else the topmost one (container_sort.h).
    if (ev.key == 's' && drag_ == kDragNone) if (ContainerGump* g = sort_target(w, mouse_x_, mouse_y_)) sort_container(w, *g);
    if (ev.key >= '1' && ev.key <= '9' && w.ask_menu) w.choose_answer(unsigned(ev.key - '1'));  // keys are deliberate: no guard
    else if (ev.key >= '1' && ev.key <= '3' && w.gumps.find<LevelPromptGump>()) w.leveling->spend_point(ev.key - '1');  // the level-up point
}

void GameMode::handle(const InputEvent& ev) {
    World& w = *world_;
    PlayerInput& in = w.input;
    auto_walk_input(ev);  // a new press cancels a walk to act (app/auto_walk.cpp)
    if (debug_->handle(ev)) return;  // the debug menu, its shortcuts and pick modes; the death menu's autosave
    if (panel_w_ > 0 && !atlas_on_ && help_page_ < 0 && hud_route(ev)) return;  // the HUD side panel (app/hud_panel.h)
    if (turn_ui_->handle(ev)) return;  // turn-based combat: the action menu, picking (app/turn_ui.h)
    if (ev.type == InputEvent::kMouseMove || ev.type == InputEvent::kMouseDown || ev.type == InputEvent::kMouseUp) {
        if (ev.x != -9999) {
            mouse_fx_ = ev.fx >= 0 ? ev.fx : ev.x + 0.5f;
            mouse_fy_ = ev.fy >= 0 ? ev.fy : ev.y + 0.5f;
        }
        if (ev.type == InputEvent::kMouseMove) pointer_frames_ = 1000;  // the real mouse took over
    }
    if (ev.type >= InputEvent::kPadDown && ev.type <= InputEvent::kPadRemoved) { pad_event(ev); return; }
    if (atlas_on_) {
        bool close = ev.type == InputEvent::kKeyDown && !ev.repeat &&
                     (ev.key == kKeyEscape || ev.key == kKeyTab || ev.key == 'm');
        if (close) { close_atlas(); return; }
        if (ev.type == InputEvent::kMouseMove || ev.type == InputEvent::kMouseDown || ev.type == InputEvent::kMouseUp ||
            ev.type == InputEvent::kWheel) {
            if (ev.x != -9999) { mouse_x_ = ev.x; mouse_y_ = ev.y; }
        }
        game_atlas_input(ev);
        return;
    }
    if (help_page_ >= 0) {
        if (ev.type == InputEvent::kKeyDown) {
            if (ev.key == kKeyRight || ev.key == kKeySpace || ev.key == kKeyPageDown) help_page_ = (help_page_ + 1) % help_pages();
            else if (ev.key == kKeyLeft || ev.key == kKeyPageUp) help_page_ = (help_page_ + help_pages() - 1) % help_pages();
            else if (!ev.repeat) help_page_ = -1;
            return;
        }
        if (ev.type == InputEvent::kMouseDown) {
            if (ev.button == 1) help_page_ = (help_page_ + 1) % help_pages();
            else help_page_ = -1;
            return;
        }
        if (ev.type == InputEvent::kMouseMove) { mouse_x_ = ev.x; mouse_y_ = ev.y; }
        return;
    }
    switch (ev.type) {
    case InputEvent::kWheel:
        if (!w.gumps.top_modal() && ev.wheel) zoom_by(ev.wheel > 0 ? -1 : 1);
        else if (auto* book = dynamic_cast<MenuGump*>(w.gumps.top_modal())) book->wheel(ev.wheel);
        break;
    case InputEvent::kKeyDown: key_down(ev); break;
    case InputEvent::kKeyUp:
        if (ev.key == kKeySpace || ev.key == kKeyReturn) text_hold_ = false;
        in.key_run = ev.mods & kModShift;
        in.key_step = ev.mods & kModCtrl;
        if (ev.key == kKeyUp) in.key_forward = false;
        if (ev.key == kKeyDown) in.key_back = false;
        if (ev.key == kKeyLeft) in.key_turn_left = false;
        if (ev.key == kKeyRight) in.key_turn_right = false;
        if (ev.key == kKeySpace) in.key_jump = false;
        if (ev.key == kKeyUp || ev.key == kKeyDown || ev.key == kKeyLeft || ev.key == kKeyRight)
            scroll_[ev.key - kKeyUp] = false;
        break;
    case InputEvent::kText:
        if (Gump* modal = w.gumps.top_modal()) modal->text(ev.text);
        break;
    case InputEvent::kMouseMove:
        mouse_x_ = ev.x;
        mouse_y_ = ev.y;
        mouse_move();
        break;
    case InputEvent::kMouseDown:
        if (ev.x != -9999) { mouse_x_ = ev.x; mouse_y_ = ev.y; }
        mouse_down(ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2);
        break;
    case InputEvent::kMouseUp:
        if (ev.x != -9999) { mouse_x_ = ev.x; mouse_y_ = ev.y; }
        mouse_up(ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2);
        break;
    default: break;
    }
}

void GameMode::mouse_down(int b) {
    World& w = *world_;
    PlayerInput& in = w.input;
    const uint32_t now = now_ms();
    in.button[b].press(now, mouse_x_, mouse_y_);
    press_gump_[b] = nullptr;
    press_item_[b] = 0;
    if (drag_ != kDragNone) return;
    Gump* g = w.gumps.at(mouse_x_, mouse_y_);
    // The target cursor (ui.md 0x000): a left click on an item in the world or a window.
    if (w.target_pid && b == 0 && (!g || !g->modal)) {
        uint16_t id = g ? g->trace_item(mouse_x_ - g->x, mouse_y_ - g->y) : pick_f(mfx(), mfy());
        if (id)
            if (Process* p = w.kernel().get(w.target_pid)) { p->result = id; p->terminate(); }
        in.button[0].handled = true;
        return;
    }
    if (!w.gumps.top_modal() && b == 0) {
        // Answer menu and barks (layer "above normal").
        unsigned idx;
        if (ask_hit(mouse_x_, mouse_y_, idx)) {
            // Not in the first 0.5 s (a click meant for the line before).
            if (w.ask_menu->frames_open >= kMenuGuardFrames) w.choose_answer(idx);
            in.use_double(0);
            return;
        }
        // Click-through (milestone 6): during a scene any click, otherwise a click on the
        // text, ends the bark page; the click does nothing else.
        if (w.controls.click_text && !w.barks.empty() && (!g || scene_running()) && bark_click(scene_running())) {
            in.use_double(0);
            swallow_left_ = true;
            text_hold_ = scene_running();
            return;
        }
    }
    if (g) {
        int lx = mouse_x_ - g->x, ly = mouse_y_ - g->y;
        press_gump_[b] = g;
        press_lx_[b] = lx;
        press_ly_[b] = ly;
        press_item_[b] = g->trace_item(lx, ly);
        if (!g->modal) w.gumps.raise(g);
        g->press(b == 0 ? 1 : b == 1 ? 2 : 3, lx, ly);
        if (in.double_click(b) && gump_alive(g)) {
            in.use_double(b);
            g->double_click(b == 0 ? 1 : b == 1 ? 2 : 3, lx, ly);
        }
        w.gumps.reap();
        if (!gump_alive(press_gump_[b])) press_gump_[b] = nullptr;
        return;
    }
    uint16_t id = pick_f(mfx(), mfy());  // through the view (zoom)
    press_item_[b] = id;
    if (b == 0) {
        picked_ = id;
        for (const Bark& bk : w.barks)
            if (bk.speaker == id) { w.click_bark(id); break; }
        // Double-click: use (not in combat, not while moving with the right button).
        const Actor* av = w.avatar();
        bool combat = av && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
        if (in.double_click(0) && !combat && !in.button[2].down) {
            in.use_double(0);
            if (!w.stasis && !auto_walk_begin(AutoWalk::kUse, id, nullptr)) w.use_item(id);
        }
    } else if (b == 2) {
        // A right double-click on the Avatar toggles combat (avatar.md 6.1).
        if (in.double_click(2) && id == 1 && !w.stasis) {
            in.use_double(2);
            toggle_combat(true);
        }
    } else if (b == 1 && !w.stasis) {
        // Middle click: walk there with the pathfinder (an extra, not original).
        uint16_t tid;
        int pt[3];
        in.button[1].handled = true;
        if (world_point(mouse_x_, mouse_y_, tid, pt) && w.avatar()) {
            Box ab = w.box_of(*w.avatar());
            w.pathfind(1, pt[0] + ab.xd / 2, pt[1] + ab.yd / 2, pt[2], 0, 0);
        }
    }
}

void GameMode::mouse_up(int b) {
    World& w = *world_;
    PlayerInput& in = w.input;
    in.button[b].down = false;
    in.button[b].release_ms = now_ms();
    if (b == 0 && swallow_left_) { swallow_left_ = false; text_hold_ = false; return; }
    if (b == 0 && drag_ != kDragNone) { finish_drag(); return; }
    if (Gump* g = press_gump_[b]; gump_alive(g)) {
        g->release(b == 0 ? 1 : b == 1 ? 2 : 3, mouse_x_ - g->x, mouse_y_ - g->y);
        w.gumps.reap();
        // The release closed its window (a menu choice, the quit box's No): the press is used
        // up, the world behind must not take it as a look or a step.
        if (!gump_alive(g)) { in.button[b].handled = true; press_gump_[b] = nullptr; }
    }
}

void GameMode::mouse_move() {
    World& w = *world_;
    PlayerInput& in = w.input;
    if (drag_ == kDragGump) {
        if (gump_alive(drag_gump_)) {
            int nx = mouse_x_ - grab_x_, ny = mouse_y_ - grab_y_;
            if (auto* cg = dynamic_cast<ContainerGump*>(drag_gump_)) { cg->off_x += nx - cg->x; cg->off_y += ny - cg->y; }
            drag_gump_->x = nx;
            drag_gump_->y = ny;
        }
        return;
    }
    if (drag_ == kDragItem) { update_drag(); return; }
    if (drag_ == kDragInvalid) return;
    Gump* pg = press_gump_[0];
    if (in.button[0].down && gump_alive(pg) && pg->modal) {
        pg->hover(mouse_x_ - pg->x, mouse_y_ - pg->y);  // e.g. the slider knob
        return;
    }
    // A press that completed a double-click may still start a drag (gumps.md 1.5).
    if (in.button[0].down && (!in.button[0].handled || (in.button[0].pair_used && !swallow_left_)) &&
        (std::abs(mouse_x_ - in.button[0].press_x) > 2 || std::abs(mouse_y_ - in.button[0].press_y) > 2)) {
        start_drag();
        return;
    }
    Gump* g = w.gumps.at(mouse_x_, mouse_y_);
    hover_gump_ = g;
    if (g) g->hover(mouse_x_ - g->x, mouse_y_ - g->y);
}

// ---------------------------------------------------------------------------------------------
// Drag and drop (avatar.md 8, gumps.md 1.6)

void GameMode::start_drag() {
    World& w = *world_;
    PlayerInput& in = w.input;
    in.button[0].handled = true;
    drag_ = kDragInvalid;
    drag_item_ = 0;
    invalid_why_.clear();
    drop_why_.clear();
    drag_from_ = nullptr;
    drop_gump_ = nullptr;
    drop_ = kDropNone;
    gump_drop_ = Gump::kDropNo;
    Gump* g = press_gump_[0];
    if (gump_alive(g)) {
        if (g->modal) { drag_ = kDragNone; return; }
        uint16_t id = press_item_[0];
        const Item* it = w.item(id);
        if (it && it->shape == 529 && dynamic_cast<PaperdollGump*>(g)) it = nullptr, id = 0;  // the backpack stays
        if (it) {
            if (!g->can_drag_out(id) || w.ask_menu) {  // invalid: red cross
                invalid_why_ = w.stasis || w.ask_menu ? "Not now" : "Too far away: walk closer to it first";
                return;
            }
            int ax = 0, ay = 0;
            g->item_anchor(id, ax, ay);
            grab_x_ = in.button[0].press_x - ax;
            grab_y_ = in.button[0].press_y - ay;
            drag_ = kDragItem;
            drag_item_ = id;
            drag_from_ = g;
            if (auto* cg = dynamic_cast<ContainerGump*>(g)) cg->hidden_item = id;
            // Equipment is taken off at drag start (gumps.md 2.5 'Unequip timing'): the stats
            // show it at once; a refused or cancelled drag puts it back on.
            if (auto* pd = dynamic_cast<PaperdollGump*>(g))
                if (Item* e = w.item(id); e && e->parent == pd->owner && (e->flags & kItemEquipped)) {
                    e->flags = uint16_t((e->flags & ~kItemEquipped) | kItemContained);
                    drag_unequipped_ = id;
                }
            w.kernel().pause();  // the world stands still while dragging
            update_drag();
            return;
        }
        if (g->draggable) {
            drag_ = kDragGump;
            drag_gump_ = g;
            grab_x_ = in.button[0].press_x - g->x;
            grab_y_ = in.button[0].press_y - g->y;
            w.gumps.raise(g);
            mouse_move();
        }
        return;
    }
    drag_item_ = press_item_[0];
    Item* it = w.item(drag_item_);
    if (!it || w.stasis || w.ask_menu || w.gumps.top_modal()) { drag_item_ = 0; return; }
    const ShapeInfo& si = data_.typeflags.get(it->shape);
    const Actor* a = it->as_actor();
    bool draggable = !si.is(ShapeInfo::kFixed) && si.weight() > 0 && (!a || a->is(Actor::kDead)) && drag_item_ != 1;
    if (!draggable) { drag_item_ = 0; return; }
    // Out of reach: a drag that only a pack can take, if he can walk there (walk to act).
    far_drag_ = !w.can_reach(1, drag_item_, 128);
    if (far_drag_ && !(auto_walk_eligible(drag_item_) && auto_walk_path(drag_item_))) { far_drag_ = false; drag_item_ = 0; return; }
    int ax, ay;
    w.screen_point(*it, ax, ay);
    grab_x_ = in.button[0].press_x - ax;
    grab_y_ = in.button[0].press_y - ay;
    drag_ = kDragItem;
    opt_.hide_id = drag_item_;
    w.kernel().pause();
    update_drag();
}

void GameMode::update_drag() {
    World& w = *world_;
    if (drag_ != kDragItem) return;
    const Item* it = w.item(drag_item_);
    if (!it) { cancel_drag(); return; }
    if (far_drag_) { update_far_drop(); return; }
    if (panel_w_ > 0 && mouse_x_ >= 320) {
        // Over the HUD side panel: a party portrait takes it into that member's backpack.
        if (gump_alive(drop_gump_)) drop_gump_->drag_leave();
        drop_gump_ = nullptr;
        gump_drop_ = Gump::kDropNo;
        drop_ = kDropNone;
        if (uint16_t id = hud_panel_->portrait_actor(mouse_x_ - 320, mouse_y_)) {
            uint16_t pack = w.equipped(id, 7);
            drop_why_ = pack ? w.put_in_refusal(drag_item_, pack) : "No backpack";
            if (drop_why_.empty()) { drop_ = kDropAvatar; drop_target_ = id; }
        }
        return;
    }
    drop_why_.clear();
    Gump* g = w.gumps.at(mouse_x_, mouse_y_);
    if (g && g->modal) g = nullptr;
    if (g != drop_gump_ && gump_alive(drop_gump_)) drop_gump_->drag_leave();
    drop_gump_ = g;
    drop_ = kDropNone;
    gump_drop_ = Gump::kDropNo;
    if (g) {
        int lx = mouse_x_ - g->x, ly = mouse_y_ - g->y;
        uint16_t under = g->trace_item(lx, ly);
        const Item* u = w.item(under);
        if (u && u->id != drag_item_ && mergeable(w, *it, *u) && !dynamic_cast<PaperdollGump*>(g)) {
            g->drag_leave();
            gump_drop_ = Gump::kDropInto;
            drop_ = kDropMerge;
            drop_target_ = under;
            return;
        }
        gump_drop_ = g->drag_over(drag_item_, lx, ly, grab_x_, grab_y_);
        return;
    }
    update_world_drop();
}

void GameMode::update_world_drop() {
    World& w = *world_;
    drop_ = kDropNone;
    const Item* it = w.item(drag_item_);
    const Actor* av = w.avatar();
    if (!it || !av) return;
    uint16_t tid;
    int pt[3];
    if (!world_point(mouse_x_, mouse_y_, tid, pt)) return;
    drop_target_ = tid;
    if (tid == drag_item_) return;
    // Onto the Avatar or a companion: into his backpack.
    if (tid == 1 || (w.party && w.party->party_member(tid))) {
        uint16_t pack = w.equipped(tid, 7);
        drop_why_ = pack ? w.put_in_refusal(drag_item_, pack) : "No backpack";
        if (drop_why_.empty()) drop_ = kDropAvatar;
        return;
    }
    // Merging onto a matching stack.
    const Item* t = w.item(tid);
    if (t && mergeable(w, *it, *t) && w.can_reach(1, tid, 128)) {
        drop_ = kDropMerge;
        return;
    }
    int xd, yd, zd;
    w.footprint(*it, xd, yd, zd);
    int x = pt[0] + xd / 2, y = pt[1] + yd / 2, z = pt[2];
    drop_pt_[0] = x; drop_pt_[1] = y; drop_pt_[2] = z;
    Box db{x, y, z, xd, yd, zd};
    bool fits = w.can_exist(x, y, z, xd, yd, zd, w.shape_flags(*it), drag_item_).valid;
    if (!fits) { drop_why_ = "No room for it there"; return; }
    if (w.can_reach_box(1, db, 0, 128, drag_item_)) { drop_ = kDropPlace; return; }
    // Out of reach: a throw, if in throw range with a clear path (physics.md 10).
    int r = std::max(1, 64 - int(w.total_weight(*it)) + w.leveling->reach_str(*av));
    int range = r * r / 2;
    if (w.can_reach_box(1, db, 0, range, drag_item_) &&
        w.missile_path_clear(drag_item_, av->x, av->y, av->z + 24, x, y, z, r, 4, 1))
        drop_ = kDropThrow;
    else drop_why_ = w.can_reach_box(1, db, 0, range, drag_item_) ? "Something is in the way" : "Too far to throw it there";
}

void GameMode::cancel_drag() {
    World& w = *world_;
    if (drag_ == kDragItem) w.kernel().unpause();
    if (gump_alive(drop_gump_)) drop_gump_->drag_leave();
    if (auto* cg = dynamic_cast<ContainerGump*>(drag_from_); cg && gump_alive(cg)) cg->hidden_item = 0;
    opt_.hide_id = 0;
    far_drag_ = false;
    drag_ = kDragNone;
    drag_item_ = 0;
    drag_from_ = drag_gump_ = drop_gump_ = nullptr;
    drop_ = kDropNone;
    gump_drop_ = Gump::kDropNo;
}

std::string GameMode::drop_reason() const {
    if (drag_ == kDragInvalid) return invalid_why_;
    if (drag_ != kDragItem) return "";
    if (drop_gump_ && gump_drop_ == Gump::kDropNo && gump_alive(drop_gump_)) return drop_gump_->drop_why;
    if (!drop_gump_ && drop_ == kDropNone) return drop_why_;
    return "";
}

bool GameMode::perform_drop(uint16_t id, const DropAt& at) {
    World& w = *world_;
    Item* it = w.item(id);
    Actor* av = w.avatar();
    if (!it || !av) return false;
    bool from_world = it->chunk >= 0 && !it->parent;
    if (at.kind == kDropMerge) {
        Item* t = w.item(at.target);
        if (!t) return false;
        if (from_world) w.grab(id);
        unsigned total = unsigned(t->quality) + std::max<unsigned>(1, it->quality);
        t->quality = uint16_t(std::min(666u, total));
        unsigned left = total > 666 ? total - 666 : 0;
        w.call_event(at.target, kEvCombine);
        if (left == 0) w.destroy(id);
        else if ((it = w.item(id))) it->quality = uint16_t(left);
        return true;
    }
    if (at.gump) {
        if (!gump_alive(at.gump)) return false;
        if (from_world) w.grab(id);
        if (!at.gump->drop(id, at.lx, at.ly, at.gx, at.gy)) return false;
        v212_auto_merge(w, id);
        return true;
    }
    switch (at.kind) {
    case kDropAvatar:
        if (from_world) w.grab(id);
        if (at.target > 1 && at.target != id) {  // a companion's backpack
            uint16_t pack = w.equipped(at.target, 7);
            if (!pack || !w.move_to_container(id, pack, true)) {
                w.refuse(pack ? w.put_in_refusal(id, pack) : "No backpack");
                return false;
            }
            if (!v212_auto_merge(w, id)) join_companion_money(w, id);  // (a merged item is gone at the frame's end)
            w.leveling->growth->on_given(id, at.target);  // a tome or an elixir: they read or drink it
            return true;
        }
        if (!w.give_to_avatar(id)) {
            const uint16_t pack = w.equipped(1, 7);
            std::string why = pack ? w.put_in_refusal(id, pack) : "No backpack";
            w.refuse(why.empty() ? "Too heavy for you to carry" : why);  // (give_to_avatar also counts the whole load)
            return false;
        }
        v212_auto_merge(w, id);
        if (from_world) w.game_log.add(GameLog::kMessages, GameLog::kInfo, "Taken: " + GameLog::name_of(w, id, false) + ".");
        return true;
    case kDropPlace:
        if (from_world) w.grab(id);
        w.collide_move(id, at.pt[0], at.pt[1], at.pt[2], true, true);
        w.fall(id);
        return true;
    case kDropThrow: {
        if (from_world) w.grab(id);
        int r = std::max(1, 64 - int(w.total_weight(*it)) + w.leveling->reach_str(*av));
        int err = std::max(20, 4 * (30 - av->dex));
        int tx = at.pt[0] + w.rand_below(2 * err + 1) - err, ty = at.pt[1] + w.rand_below(2 * err + 1) - err;
        w.collide_move(id, av->x, av->y, av->z + 24, true, true);
        int vx, vy, vz, n;
        w.missile_velocity(av->x, av->y, av->z + 24, tx, ty, at.pt[2], r, 4, vx, vy, vz, n);
        w.hurl(id, vx, vy, vz, 4);
        int d = direction_to(tx - av->x, ty - av->y);
        if (!av->is(Actor::kAnimLock)) w.do_anim(1, 2, d);
        return true;
    }
    default: return false;
    }
}

// How many of a stack the slider offers first: all of it, except obsidian given to a companion who
// buys reagents (Leveling stage 5): what their full restock still needs beyond what they carry.
int GameMode::slider_start(uint16_t id, const DropAt& at) const {
    World& w = *world_;
    const Item* it = w.item(id);
    const int qty = it ? it->quality : 1;
    if (!it || it->shape != StatGrowth::kMoneyShape || at.kind != kDropAvatar || at.gump || at.target <= 1) return qty;
    if (!w.party || !w.leveling || !w.leveling->active() || !w.leveling->magic->loaded) return qty;
    const int ci = w.party->index_of(at.target);
    if (ci < 0) return qty;
    const int need = w.leveling->magic->restock_cost(ci) - w.leveling->growth->obsidian(at.target);
    return need > 0 ? std::clamp(need, 1, qty) : qty;
}

void GameMode::finish_drag() {
    World& w = *world_;
    if (drag_ == kDragInvalid) {
        if (!invalid_why_.empty()) w.refuse(invalid_why_);
        cancel_drag();
        return;
    }
    if (drag_ == kDragGump) { cancel_drag(); return; }
    update_drag();
    const std::string why = drop_reason();
    uint16_t id = drag_item_;
    DropAt at;
    at.gump = drop_gump_;
    at.lx = mouse_x_ - (drop_gump_ ? drop_gump_->x : 0);
    at.ly = mouse_y_ - (drop_gump_ ? drop_gump_->y : 0);
    at.gx = grab_x_;
    at.gy = grab_y_;
    at.kind = drop_;
    std::copy(drop_pt_, drop_pt_ + 3, at.pt);
    at.target = drop_target_;
    bool ok = drop_gump_ ? (gump_drop_ != Gump::kDropNo) : (drop_ != kDropNone);
    bool far = far_drag_;
    cancel_drag();
    static const char* kinds[] = {"refused", "place", "throw", "into the backpack", "merge"};
    PG_LOG("drop %u: %s%s at %d,%d,%d (target %u)", id, at.gump ? at.gump->kind() : "world ",
           at.gump ? "" : kinds[at.kind], at.pt[0], at.pt[1], at.pt[2], at.target);
    if (!w.item(id)) return;
    if (!ok) {
        if (!why.empty()) w.refuse(why);
        return;
    }
    if (far) {  // walk to act: walk there, then this drop (the red cross if he cannot)
        if (!auto_walk_begin(AutoWalk::kDrop, id, &at)) w.red_cross_frames = 8;
        return;
    }
    drop_now(id, at);
}

void GameMode::drop_now(uint16_t id, const DropAt& at_in) {
    World& w = *world_;
    DropAt at = at_in;
    Item* it = w.item(id);
    if (!it) return;
    if (at.kind == kDropMerge) at.gump = nullptr;
    // A stack of more than one: ask how many (gumps.md 1.6). The split happens when the
    // slider closes.
    if (is_stack(w, *it) && it->quality > 1 && !w.auto_ui) {
        int qty = it->quality;
        auto s = std::make_unique<SliderGump>(w, 0, qty, 1, slider_start(id, at));
        s->done = [this, id, at, qty](int v) {
            World& ww = *world_;
            Item* orig = ww.item(id);
            if (!orig || v <= 0) return;
            if (v >= qty) {
                if (perform_drop(id, at)) ww.theft_check(id);
                return;
            }
            uint16_t copy = ww.create_item(orig->shape, orig->frame);
            Item* c = ww.item(copy);
            if (!c) return;
            c->quality = uint16_t(v);
            c->flags = uint16_t(orig->flags & (kItemDisposable | kItemOwned | kItemInvisible | kItemFlipped |
                                               kItemFastOnly | kItemLowFriction));
            orig->quality = uint16_t(qty - v);
            DropAt a2 = at;
            if (perform_drop(copy, a2)) {
                ww.theft_check(copy);
            } else if ((orig = ww.item(id))) {
                orig->quality = uint16_t(orig->quality + v);
                ww.destroy_now(copy);
            }
        };
        w.gumps.add(std::move(s));
        return;
    }
    if (perform_drop(id, at)) w.theft_check(id);
}

// ---------------------------------------------------------------------------------------------
// Per frame

void GameMode::update_cursor() {
    World& w = *world_;
    PlayerInput& in = w.input;
    in.now_ms = now_ms();
    in.mouse_x = mouse_x_;
    in.mouse_y = mouse_y_;
    // avatar.md 2.1-2.2: the sector of (dx, 2dy) from the feet point; length by distance.
    // A zoomed view measures the direction in world pixels from the view centre.
    int dx = mouse_x_ - 160, dy = 114 - mouse_y_;
    if (vs_.layered && vs_.kf != vs_.S) {
        float fx = mouse_fx_ >= 0 ? mouse_fx_ : mouse_x_ + 0.5f, fy = mouse_fy_ >= 0 ? mouse_fy_ : mouse_y_ + 0.5f;
        double sc = double(vs_.S) / vs_.kf;
        dx = int(std::lround((fx - 160) * sc));
        dy = int(std::lround(14 - (fy - 100) * sc));
    }
    int sdir;
    if (dx == 0) {
        sdir = dy > 0 ? 0 : 4;
    } else {
        long t = (1024L * 2 * dy) / dx;
        if (dx < 0) t = -t;
        int right = t > 2472 ? 0 : t > 424 ? 1 : t >= -424 ? 2 : t >= -2472 ? 3 : 4;
        sdir = dx > 0 ? right : (8 - right) & 7;
    }
    in.cursor_dir = (sdir + 7) & 7;  // screen direction -> world direction
    in.cursor_len = (std::abs(mouse_x_ - 160) > 130 || std::abs(mouse_y_ - 100) > 81) ? 2
                    : (std::abs(dx) > 30 || std::abs(dy) > 19) ? 1 : 0;
    uint16_t id = 0;
    int pt[3] = {0, 0, 0};
    world_point(mouse_x_, mouse_y_, id, pt);
    in.cursor_item = id;
    std::copy(pt, pt + 3, in.cursor_point);
}

void GameMode::tick() {
    World& w = *world_;
    if (panel_w_ > 0) hud_panel_->tick();  // the HUD side panel (app/hud_panel.h)
    if (atlas_on_) { atlas_tick(); return; }  // the game stands still behind the atlas
    // Equipment taken off at drag start that did not go anywhere goes back on.
    if (drag_unequipped_ && drag_ == kDragNone && !w.gumps.find<SliderGump>()) {
        Item* e = w.item(drag_unequipped_);
        if (e && e->parent && e->parent < 256 && !(e->flags & kItemEquipped) && (e->flags & kItemContained)) {  // the Avatar or a companion
            e->flags = uint16_t((e->flags & ~kItemContained) | kItemEquipped);
        }
        drag_unequipped_ = 0;
    }
    // Scripted input (headless play-testing).
    while (script_next_ < script_.size() && script_[script_next_].frame <= frames_) {
        const Scripted& sc = script_[script_next_++];
        if (sc.debug.empty()) handle(sc.ev);
        else run_debug(sc.debug);
    }
    handle_requests();
    if (auto* lp = w.gumps.find<LevelPromptGump>()) lp->pad_hint = pads_ > 0;
    if (help_page_ >= 0) { pad_tick(); return; }  // paused behind the help page
    pad_tick();
    update_cursor();
    PlayerInput& in = w.input;
    in.jump_held = in.key_jump || pad_btn_[kPadX] || (in.button[0].down && in.button[2].down);
    // Single clicks are delivered once the double-click time has passed (gumps.md 1.5).
    const Actor* av = w.avatar();
    bool combat = av && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    for (int b : {0, 2}) {
        // Responsive mode: a click acts on release, except in modal windows (books, the menu).
        bool instant = w.controls.responsive && !in.button[b].handled &&
                       !(gump_alive(press_gump_[b]) && press_gump_[b]->modal);
        if (drag_ != kDragNone || in.button[b].down || !(in.past_timeout(b) || instant)) continue;
        Gump* g = press_gump_[b];
        if (gump_alive(g)) {
            in.button[b].handled = true;
            g->click(b == 0 ? 1 : 3, press_lx_[b], press_ly_[b]);
            press_gump_[b] = nullptr;
            w.gumps.reap();
            continue;
        }
        press_gump_[b] = nullptr;
        if (b == 0 && !combat) {
            in.button[0].handled = true;
            // A left click that nobody used within the double-click time is a look (avatar.md 7).
            if (!in.button[2].down && !w.stasis && !w.gumps.top_modal()) look_item(press_item_[0]);
        }
        if (b == 2 && !combat) in.button[2].handled = true;
    }
    // Debug scrolling: moving the view detaches the camera from the Avatar.
    if (scroll_[0] || scroll_[1] || scroll_[2] || scroll_[3]) {
        int step = 32, dx = 0, dy = 0;
        if (scroll_[0]) { dx -= step; dy -= step; }
        if (scroll_[1]) { dx += step; dy += step; }
        if (scroll_[2]) { dx -= step; dy += step; }
        if (scroll_[3]) { dx += step; dy -= step; }
        int cam[3];
        w.camera_point(cam);
        w.camera_stay(std::clamp(cam[0] + dx, 0, 32767), std::clamp(cam[1] + dy, 0, 32767), cam[2]);
    }
    // While a modal gump is up the Avatar gets no input.
    // A press that went to a window (the menu book, a reader) is that window's: marking it
    // used here would cancel its single click, so a click in a modal window acted only when
    // the button went down and up within one frame (or as a double-click).
    if (w.gumps.top_modal()) {
        for (int b = 0; b < 3; ++b)
            if (in.button[b].down && !gump_alive(press_gump_[b])) in.button[b].handled = true;
    }
    const int world_frames = debug_->world_frames();  // debug: 0 paused, 2 / 4 faster
    turn_ui_->tick();  // turn-based combat: menu state, the speed-up button
    if (world_frames > 0) w.frame();
    auto_walk_tick();  // walk to act (app/auto_walk.cpp)
    // Turn-based combat: enemy and Auto turns may run faster (Turn Speed, or a button held).
    for (int extra = 0, n = world_frames > 0 ? turn_ui_->extra_world_frames() : 0; extra < n; ++extra) {
        in.now_ms = now_ms();
        w.frame();
        ++frames_;
        record_motion();
        bark_tick();
    }
    if (w.red_cross_frames > 0) --w.red_cross_frames;
    if (w.refuse_frames > 0) --w.refuse_frames;
    if (drag_ == kDragItem && !w.item(drag_item_)) cancel_drag();
    ++frames_;
    record_motion();
    bark_tick();
    // Fast-forward (milestone 6): a click-through button held during a scene runs the world
    // up to four times as fast. Only while the Avatar is in stasis and nothing waits for the
    // player (no answer menu, no window), so it is the same as waiting.
    for (int extra = 0; extra < 3 && fast_forward(); ++extra) {
        in.now_ms = now_ms();
        w.frame();
        ++frames_;
        record_motion();
        bark_tick();
    }
    for (int extra = 1; extra < world_frames; ++extra) {
        in.now_ms = now_ms();
        w.frame();
        ++frames_;
        record_motion();
        bark_tick();
    }
    debug_->tick();
    if (toast_frames_ > 0) --toast_frames_;
    if (w.game_over) {
        if (w.auto_ui) {
            // Headless runs: start again at once.
            PG_LOG("game over: restarting");
            cancel_drag();
            ++restarts_;
            w.new_game();
            debug_->world_reset();
        } else if (!w.gumps.find<MenuGump>()) {
            // The main menu after death (gumps.md 2.13): load, new game (entry 1) or quit.
            w.open_menu(MenuGump::kMenu);
        }
    }
}

void GameMode::draw_barks(Surface8& surf) {
    World& w = *world_;
    bark_rects_.clear();
    for (const Bark& b : w.barks) {
        const ShapeFont& f = w.font(b.font);
        if (b.page >= b.pages.size()) continue;
        const TextPage& pg = b.pages[b.page];
        int ax = 160, ay = 192;
        if (const Item* it = w.item(b.speaker)) {
            // Inside a container whose window is open: anchored to that window (gumps.md 1.4).
            const Gump* g = it->parent ? w.gumps.container_gump(it->parent) : nullptr;
            int sx, sy;
            if (g && g->item_anchor(it->id, sx, sy)) {
                ax = sx;
                ay = sy;
            } else {
                w.screen_point(*w.root_container(*it), ax, ay);
                ay -= int(data_.typeflags.get(it->shape).z()) * 8 + 16;
            }
        }
        int left = std::clamp(ax - pg.width / 2, 0, std::max(0, 320 - pg.width));
        int top = std::clamp(ay - pg.height, 0, std::max(0, 200 - pg.height));
        bark_rects_.push_back({b.speaker, left, top, pg.width, pg.height});
        for (size_t i = 0; i < pg.lines.size(); ++i) f.draw_top(surf, left, top + int(i) * f.line_advance(), pg.lines[i]);
    }
    // "Level up" rising above someone's head (game/leveling.h).
    if (w.leveling)
        for (const Leveling::FloatText& ft : w.leveling->floats) {
            const Actor* a = w.actor(ft.actor);
            if (!a || a->chunk < 0) continue;
            const ShapeFont& f = w.font(0);
            int ax, ay;
            w.screen_point(*a, ax, ay);
            ay -= int(data_.typeflags.get(a->shape).z()) * 8 + 16 + (75 - ft.frames) / 5;
            f.draw_top(surf, std::clamp(ax - f.text_width(ft.text) / 2, 0, std::max(0, 320 - f.text_width(ft.text))),
                       std::clamp(ay - f.height(), 0, 190), ft.text);
        }
    draw_virtue_popups(w, surf);  // "Humility +4" rising over the Avatar (game/virtues.h)
}

void GameMode::draw_ask(Surface8& surf) {
    World& w = *world_;
    const AskMenu* m = w.ask_menu.get();
    if (!m) return;
    int ax = 160, ay = 192;
    if (const Actor* av = w.avatar()) {
        w.screen_point(*av, ax, ay);
        ay -= int(data_.typeflags.get(av->shape).z()) * 8 + 16;
    }
    ask_x_ = std::clamp(ax - m->w / 2, 0, std::max(0, 320 - m->w));
    ask_y_ = std::clamp(ay - m->h, 0, std::max(0, 200 - m->h));
    const ShapeFont& f = w.font(6);
    for (size_t i = 0; i < m->buttons.size(); ++i)
        f.draw_top(surf, ask_x_ + m->buttons[i].x, ask_y_ + m->buttons[i].y, "@ " + w.vm().string(m->answers[i]));
    // The answer picked with the controller or the arrow keys: underlined.
    ask_sync();
    if (ask_sel_ >= 0 && size_t(ask_sel_) < m->buttons.size()) {
        const auto& b = m->buttons[size_t(ask_sel_)];
        surf.fill_rect(ask_x_ + b.x, ask_y_ + b.y + b.h - 1, b.w, 1, 0x0F);
    }
    // With a controller: a small hint of the answer B gives, under the menu's right end.
    if (pads_ > 0) {
        int e = ask_exit();
        if (e >= 0) {
            const ShapeFont& h = w.font(0);
            std::string hint = "B: " + w.vm().string(m->answers[size_t(e)]);
            while (!hint.empty() && hint.back() == ' ') hint.pop_back();
            int hw = h.text_width(hint);
            int hx = std::clamp(ask_x_ + m->w - hw, 0, std::max(0, 320 - hw));
            int hy = ask_y_ + m->h + 1;
            if (hy + h.height() > 200) hy = std::max(0, ask_y_ - h.height() - 1);
            h.draw_top(surf, hx, hy, hint);
        }
    }
}

namespace {
uint8_t cursor_note_colour(const Palette& pal, int r, int g, int b) {
    int best = 16, bd = 1 << 30;
    for (int i = 16; i < 256; ++i) {
        int dr = pal.rgb8[size_t(i)][0] - r, dg = pal.rgb8[size_t(i)][1] - g, db = pal.rgb8[size_t(i)][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}
}  // namespace

void GameMode::draw_cursor(Surface8& surf) {
    const Shape* m = data_.mouse.shape(0);
    if (!m || help_page_ >= 0) return;
    if (panel_w_ > 0 && mouse_x_ >= 320 && !cursor_on_panel_) return;  // drawn on the side panel
    World& w = *world_;
    const PlayerInput& in = w.input;
    int frame;
    int sdir = (in.cursor_dir + 1) & 7;  // world -> screen direction
    const Actor* av = w.avatar();
    bool combat = av && av->is(Actor::kInCombat) && !av->is(Actor::kCombatRun);
    const Gump* modal = w.gumps.top_modal();
    if (drag_ == kDragItem) {
        // The dragged item rides on the cursor where no window draws its ghost.
        if (const Item* it = w.item(drag_item_); it && !(drop_gump_ && gump_drop_ != Gump::kDropNo && drop_ != kDropMerge))
            if (const ShapeFrame* fr = data_.shapes.frame(it->shape, it->frame))
                surf.draw(*fr, mouse_x_ - grab_x_, mouse_y_ - grab_y_, it->flags & kItemFlipped);
        if (drop_gump_) frame = gump_drop_ == Gump::kDropNo ? kCursorRedCross : gump_drop_ == Gump::kDropInto ? kCursorTarget : in.cursor_len * 8 + sdir;
        else if (drop_ == kDropNone) frame = kCursorRedCross;
        else if (drop_ == kDropThrow) frame = kCursorTarget;
        else frame = in.cursor_len * 8 + sdir;
    } else if (drag_ == kDragInvalid) {
        frame = kCursorRedCross;
    } else if (modal && !w.target_pid) {
        frame = modal->cursor;
        if (frame == kCursorHidden) return;
        if (frame == kCursorArrow) frame = kCursorHand;
    } else if (w.target_pid) {
        frame = kCursorTarget;
    } else if (w.red_cross_frames > 0) {
        frame = kCursorRedCross;
    } else if (combat) {
        frame = 25 + sdir;
    } else {
        frame = in.cursor_len * 8 + sdir;
    }
    if (const ShapeFrame* fr = m->frame(unsigned(frame))) surf.draw(*fr, mouse_x_, mouse_y_);
    // Why the red cross: beside the pointer while dragging, and for a moment after a refusal.
    std::string why = frame == kCursorRedCross && (drag_ == kDragItem || drag_ == kDragInvalid) ? drop_reason() : "";
    if (why.empty() && w.refuse_frames > 0) why = w.refuse_text;
    // A gump's tooltip (the item names on a sheet), when nothing is dragged.
    bool tip = false;
    if (why.empty() && drag_ == kDragNone && frame != kCursorRedCross)
        if (Gump* hg = w.gumps.at(mouse_x_, mouse_y_); hg && !hg->modal) {
            why = hg->tip(mouse_x_ - hg->x, mouse_y_ - hg->y);
            tip = !why.empty();
        }
    if (!why.empty()) {
        const ShapeFont& f = w.font(1);
        const int tw = f.text_width(why), th = f.height();
        int tx = mouse_x_ + 12, ty = mouse_y_ + 12;
        if (tx + tw + 3 > 320) tx = std::max(3, mouse_x_ - 12 - tw);
        if (ty + th + 2 > 200) ty = std::max(2, mouse_y_ - 12 - th);
        static uint8_t paper = 0, edge = 0, tip_edge = 0;
        if (!paper) {
            paper = cursor_note_colour(data_.palette, 222, 206, 160);
            edge = cursor_note_colour(data_.palette, 120, 30, 20);
            tip_edge = cursor_note_colour(data_.palette, 70, 56, 40);
        }
        surf.fill_rect(tx - 3, ty - 2, tw + 6, th + 4, tip ? tip_edge : edge);
        surf.fill_rect(tx - 2, ty - 1, tw + 4, th + 2, paper);
        f.draw_top(surf, tx, ty, why);
    }
}

void GameMode::render(Surface8& surf) {
    World& w = *world_;
    vs_ = ViewState{};
    w.screen_mapper = nullptr;
    w.view_w = 320;
    w.view_h = 200;
    opt_.use_screen_camera = false;
    opt_.display_pos = nullptr;
    surf.reset_clip();
    surf.clear(0);
    int cam[3];
    w.camera_point(cam);
    vs_.cam_sx = (cam[0] - cam[1]) / 4;
    vs_.cam_sy = (cam[0] + cam[1]) / 8 - cam[2];
    render_world(surf, cam);
    render_ui(surf, false);
}

void GameMode::render_world(Surface8& surf, const int cam[3]) {
    World& w = *world_;
    // The weapon overlay (formats/weapons.md): drawn right after the Avatar.
    // Companions' recolours (game/party.h).
    // Rock Flesh's stone look first (game/rock_flesh.h), then the companions' recolours.
    if (!w.stone_skin.empty() || (w.party && w.party->enabled))
        opt_.remap = [&w](uint16_t id) -> const uint8_t* {
            if (const uint8_t* s = rock_flesh::remap(w, id)) return s;
            return w.party && w.party->enabled ? w.party->remap(id) : nullptr;
        };
    else opt_.remap = nullptr;
    // The new creatures are drawn from effect shapes (game/creatures.h).
    if (w.creatures && !w.creatures->live.empty())
        opt_.appearance = [&w](uint16_t id, WorldView::Options::Look& lk) { return w.creatures->appearance(id, lk); };
    else opt_.appearance = nullptr;
    opt_.after_paint = [this, &w](Surface8& s, const SortItem& si, int sx, int sy) {
        if (si.id != 1) return;
        const Actor* a = w.avatar();
        if (!a) return;
        unsigned act = a->last_anim;
        if (!a->is(Actor::kInCombat) && act != 6) return;
        const Item* wp = w.item(w.equipped(1, 6));
        const WeaponInfo* wi = wp ? weapon_info(wp->shape) : nullptr;
        if (!wi || wi->overlay > 3) return;
        ByteView e = data_.wpnovlay.entry(act);
        const AnimSequence* seq = data_.anim.get(1, act);
        if (e.empty() || !seq || !seq->length) return;
        unsigned L = seq->length, d = a->dir & 7, idx = 0;
        bool found = false;
        for (unsigned i = 0; i < L && !found; ++i)
            if (seq->at(d, i).frame == a->frame) { idx = i; found = true; }
        if (!found) return;
        size_t off = 4 * ((size_t(wi->overlay) * 8 + d) * L + idx);
        if (off + 4 > e.size) return;
        int ox = int8_t(e.data[off]), oy = int8_t(e.data[off + 1]);
        unsigned of = rd16(e.data + off + 2);
        if (ox == 0 && oy == 0 && of == 0) return;  // an all-zero record: no overlay
        static const unsigned kOverlayShape[4] = {241, 243, 244, 245};
        if (const ShapeFrame* f = data_.shapes.frame(kOverlayShape[wi->overlay], of)) s.draw(*f, sx + ox, sy + oy);
    };
    // The stitched world: the neighbouring maps' scenery across a seam (world/world_space.h).
    opt_.extra = w.neighbors && !w.neighbors->items().empty() ? &w.neighbors->items() : nullptr;
    view_.render(w.map(), surf, cam, cam, opt_);
}

// The UI over the world: windows, barks, the answer menu (part 1), the screen inversion of
// both (only of the UI when the world is drawn on its own surface), then the status line,
// the milestone 6 overlays and the cursor.
void GameMode::render_ui(Surface8& surf, bool layered) {
    World& w = *world_;
    debug_->draw_under(surf);  // critical-path line, boxes, routes: over the world, under the windows
    turn_ui_->draw_world(surf);  // turn-based combat: marker, reachable area, route, targets
    w.gumps.paint(surf, -100, kLayerAbove - 1);
    draw_barks(surf);
    draw_ask(surf);
    w.gumps.paint(surf, kLayerAbove, 100);
    if (!layered && w.invert_state >= 0x4000) {  // upside down (ui.md 0x0C8; the rolling mid-states are not drawn)
        for (int y = 0; y < surf.height() / 2; ++y) {
            uint8_t* a = surf.row(y);
            uint8_t* b = surf.row(surf.height() - 1 - y);
            std::swap_ranges(a, a + surf.width(), b);
        }
    }
    if (hud_) {
        const ShapeFont& f = w.font(0);
        char line[200];
        const Actor* av = w.avatar();
        std::snprintf(line, sizeof line, "map %u  %d,%d,%d  procs %zu  frame %u  %s%s", w.map().map_number(),
                      av ? av->x : 0, av ? av->y : 0, av ? av->z : 0, w.kernel().size(), w.kernel().frame_number(),
                      w.stasis ? "stasis " : "", av && av->is(Actor::kInCombat) ? "combat" : "");
        f.draw_top(surf, 2, 1, line);
        if (av) {
            std::snprintf(line, sizeof line, "HP %u/%d  STR %d DEX %d INT %d  mana %d  anim %u dir %u", av->hp,
                          2 * av->str, av->str, av->dex, av->intel, av->mana, av->last_anim, av->dir);
            f.draw_top(surf, 2, 11, line);
        }
        if (w.stitch && av) {
            int32_t wp[3];
            w.avatar_world_point(wp);
            std::snprintf(line, sizeof line, "world %d,%d,%d  layer %d  neighbour chunks %u", wp[0], wp[1], wp[2],
                          w.stitch->layer(w.map().map_number()), w.neighbors ? w.neighbors->chunks_live : 0u);
            f.draw_top(surf, 2, 31, line);
        }
        if (picked_) {
            const Item* it = w.item(uint16_t(picked_));
            const Actor* pa = it ? it->as_actor() : nullptr;
            std::snprintf(line, sizeof line, "object %u shape %u frame %u%s%s", picked_, it ? it->shape : 0,
                          it ? it->frame : 0, pa ? "  hp " : "", pa ? std::to_string(pa->hp).c_str() : "");
            f.draw_top(surf, 2, 21, line);
        }
    }
    debug_->draw_over(surf);
    draw_overlays(surf);
    turn_ui_->draw_hud(surf);  // turn-based combat: the turn strip and the action menu
    draw_cursor(surf);
    if (w.sky_flash) { w.sky_flash = 0; turn_ui_->flash_surface(surf); }  // the Lightning spell's flash, one frame
}

}  // namespace pg
