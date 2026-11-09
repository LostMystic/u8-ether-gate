// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Debug mode, the critical-path tracker and autosaves (see debug_tools.h).
#include "app/debug_tools.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>

#include "app/atlas_view.h"
#include "app/game_mode.h"
#include "core/config.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/container_sort.h"
#include "game/game_log.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/pathfinder.h"
#include "game/savegame.h"
#include "game/virtues.h"
#include "game/world_stages.h"
#include "game/spellbook.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "platform/input.h"
#include "world/atlas_layout.h"
#include "world/world_space.h"

namespace pg {

namespace {

uint8_t nearest(const Palette& p, int r, int g, int b) {
    int best = 1, bd = 1 << 30;
    for (int i = 1; i < 256; ++i) {
        int dr = p.rgb8[size_t(i)][0] - r, dg = p.rgb8[size_t(i)][1] - g, db = p.rgb8[size_t(i)][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}

void plot(Surface8& s, int x, int y, uint8_t c) {
    if (x >= 0 && y >= 0 && x < s.width() && y < s.height()) s.fill_rect(x, y, 1, 1, c);
}

// A line; dashed: (i + phase) % 8 < 5 in colour a, else b (b = 0 skips).
void line(Surface8& s, int x0, int y0, int x1, int y1, uint8_t a, uint8_t b = 0, int phase = -1, bool thick = false) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    bool steep = -dy > dx;
    for (int i = 0; i < 4000; ++i) {
        uint8_t c = a;
        if (phase >= 0 && ((i + phase) & 7) >= 5) c = b;
        if (c) {
            plot(s, x0, y0, c);
            if (thick) plot(s, x0 + (steep ? 1 : 0), y0 + (steep ? 0 : 1), c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Liang-Barsky: clips the segment to the rectangle; false if it misses it.
bool clip(double& x0, double& y0, double& x1, double& y1, double l, double t, double r, double b) {
    double dx = x1 - x0, dy = y1 - y0, u0 = 0, u1 = 1;
    const double p[4] = {-dx, dx, -dy, dy}, q[4] = {x0 - l, r - x0, y0 - t, b - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) { if (q[i] < 0) return false; continue; }
        double u = q[i] / p[i];
        if (p[i] < 0) u0 = std::max(u0, u);
        else u1 = std::min(u1, u);
        if (u0 > u1) return false;
    }
    double nx0 = x0 + u0 * dx, ny0 = y0 + u0 * dy;
    x1 = x0 + u1 * dx;
    y1 = y0 + u1 * dy;
    x0 = nx0;
    y0 = ny0;
    return true;
}

std::string onoff(bool b) { return b ? "On" : "Off"; }

// A row of the Give item page (shape, frame, count for stacks, label, quality).
struct GiveItem { unsigned shape, frame; int qty; std::string label; int quality = -1; };

}  // namespace

DebugTools::DebugTools(GameMode& gm) : ctx(gm.world()), route(std::make_unique<RouteGuide>(gm.world())), gm_(gm) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        // Options page rows (gumps.md 2.3 extras), persisted with the other settings.
        World::extra_option_rows().push_back([](World&) {
            return MenuGump::OptionRow{
                "Autosave:", [] { return onoff(Config::global().get_bool("autosave", true)); },
                [](int) {
                    Config& c = Config::global();
                    c.set_bool("autosave", !c.get_bool("autosave", true));
                    c.save();
                }};
        });
        World::extra_option_rows().push_back([](World&) {
            return MenuGump::OptionRow{
                "Debug Mode:", [] { return onoff(Config::global().get_bool("debug_mode", false)); },
                [](int) {
                    Config& c = Config::global();
                    c.set_bool("debug_mode", !c.get_bool("debug_mode", false));
                    c.save();
                }};
        });
    }
}

bool DebugTools::enabled() const { return forced_ || Config::global().get_bool("debug_mode", false); }

void DebugTools::set_enabled(bool on) {
    Config::global().set_bool("debug_mode", on);
    Config::global().save();
    if (!on) forced_ = false;
}

bool DebugTools::route_line() const { return Config::global().get_bool("crit_route", true); }

void DebugTools::set_route_line(bool on) {
    Config::global().set_bool("crit_route", on);
    Config::global().save();
}

bool DebugTools::autosave_enabled() const { return !no_autosave_run && Config::global().get_bool("autosave", true); }

void DebugTools::set_autosave(bool on) {
    Config::global().set_bool("autosave", on);
    Config::global().save();
}

void DebugTools::say(const std::string& m, int frames) {
    message = m;
    message_frames = frames;
    PG_LOG("debug: %s", m.c_str());
}

bool DebugTools::load_crit(const std::vector<std::string>& files) {
    crit.clear();
    crit.sources.clear();
    bool ok = true;
    for (const std::string& f : files) {
        std::string err;
        if (!crit.load_file(f, err)) {
            PG_LOG("critpath: %s", err.c_str());
            ok = false;
        }
    }
    PG_LOG("critpath: %zu steps, %zu map links", crit.steps.size(), crit.links.size());
    world_reset();
    return ok;
}

DebugMenuGump* DebugTools::menu() const { return gm_.world().gumps.find<DebugMenuGump>(); }

void DebugTools::open_menu() {
    World& w = gm_.world();
    if (menu() || !enabled()) return;
    w.gumps.add(std::make_unique<DebugMenuGump>(w, root_page()));
    PG_LOG("debug menu opened");
}

void DebugTools::close_menu() {
    // Only marked: the gump is removed by the game mode's next reap (actions run inside it).
    if (DebugMenuGump* m = menu()) m->close();
}

// ---------------------------------------------------------------------------------------------
// Input

bool DebugTools::handle(const InputEvent& ev) {
    World& w = gm_.world();
    auto set_mouse = [&]() {
        if (ev.x == -9999) return;
        gm_.set_mouse_f(ev.fx >= 0 ? ev.fx : ev.x + 0.5f, ev.fy >= 0 ? ev.fy : ev.y + 0.5f);
        gm_.set_mouse(ev.x, ev.y);
    };
    // The death menu: controller A loads the last autosave (also drawn there).
    if (MenuGump* mg = w.gumps.find<MenuGump>(); mg && mg->mode() == MenuGump::kMenu && w.avatar() &&
                                                  w.avatar()->is(Actor::kDead) && w.saves &&
                                                  w.saves->latest_autosave() && !menu()) {
        // (Enter is handled by the menu itself.)
        bool go = ev.type == InputEvent::kPadDown && ev.button == kPadA;
        if (go) {
            w.request = {World::Request::kLoad, w.saves->latest_autosave(), ""};
            PG_LOG("death menu: load last autosave (slot %d)", w.request.slot);
            return true;
        }
    }
    if (DebugMenuGump* m = menu()) {
        switch (ev.type) {
        case InputEvent::kKeyDown:
            m->key(ev.key, ev.mods);
            break;
        case InputEvent::kText: break;
        case InputEvent::kWheel: m->wheel(ev.wheel); break;
        case InputEvent::kMouseMove:
            set_mouse();
            m->hover(gm_.mouse_x() - m->x, gm_.mouse_y() - m->y);
            break;
        case InputEvent::kMouseDown:
            set_mouse();
            m->press(ev.button, gm_.mouse_x() - m->x, gm_.mouse_y() - m->y);
            break;
        case InputEvent::kMouseUp: set_mouse(); break;
        case InputEvent::kPadDown:
            switch (ev.button) {
            case kPadUp: m->key(kKeyUp, 0); break;
            case kPadDown: m->key(kKeyDown, 0); break;
            case kPadLeft: m->key(kKeyLeft, 0); break;
            case kPadRight: m->key(kKeyRight, 0); break;
            case kPadLB: m->key(kKeyPageUp, 0); break;
            case kPadRB: m->key(kKeyPageDown, 0); break;
            case kPadA: m->key(kKeyReturn, 0); break;
            case kPadB: m->key(kKeyEscape, 0); break;
            case kPadBack: case kPadStart: m->close(); break;
            default: break;
            }
            break;
        case InputEvent::kPadUp: case InputEvent::kPadAxis: break;
        default: return false;  // key releases, pad hot-plug: the game mode keeps its state right
        }
        w.gumps.reap();
        return true;
    }
    if (!enabled()) return false;
    // Pick modes: the next left click in the world.
    if (pick != kPickNone) {
        if (ev.type == InputEvent::kKeyDown && ev.key == kKeyEscape) { pick = kPickNone; say("Cancelled"); return true; }
        if (ev.type == InputEvent::kMouseMove) return false;
        if (ev.type == InputEvent::kMouseDown && ev.button == 1) {
            set_mouse();
            int mx = gm_.mouse_x(), my = gm_.mouse_y();
            Pick p = pick;
            pick = kPickNone;
            if (p == kPickWarp) {
                uint16_t id;
                int pt[3];
                if (gm_.world_point(mx, my, id, pt)) warp_to(w.map().map_number(), pt[0], pt[1], pt[2]);
                else say("Nothing to stand on there");
            } else {
                uint16_t id = gm_.pick(mx, my);
                if (!kill(id)) say("No actor there");
            }
            return true;
        }
        if (ev.type == InputEvent::kMouseUp && ev.button == 1) return true;
    }
    if (ev.type != InputEvent::kKeyDown || ev.repeat) return false;
    if (w.gumps.top_modal() || gm_.atlas_open() || gm_.help_open()) return false;
    if (ev.key == kKeyF10 && !(ev.mods & kModShift)) { open_menu(); return true; }
    if ((ev.mods & kModCtrl) && !(ev.mods & kModAlt)) {
        bool shift = ev.mods & kModShift;
        switch (ev.key) {
        case 'h': toggle("hud"); return true;
        case 'b': toggle("boxes"); return true;
        case 'e': toggle("editor"); return true;
        case 'p': toggle("routes"); return true;
        case 'j': toggle("crit"); return true;
        case 'c': toggle("noclip"); return true;
        case 'g': toggle("god"); return true;
        case 'm': toggle("mana"); return true;
        case 'l': toggle("heal"); return true;
        case 'k': toggle("kill_pointer"); return true;
        case 'v': toggle("warp_click"); return true;
        case 'a': toggle("warp_atlas"); return true;
        case 'z': toggle("pause"); return true;
        case 's': toggle("step"); return true;
        case 'd': toggle("speed"); return true;
        case 'o': toggle("hour"); return true;
        case 'u': toggle(shift ? "load_snapshot" : "snapshot"); return true;
        case 'f': open_menu(); if (DebugMenuGump* m = menu()) m->push(npcs_page()); return true;
        case 'i': open_menu(); if (DebugMenuGump* m = menu()) m->push(items_page()); return true;
        case 'n': open_menu(); if (DebugMenuGump* m = menu()) m->push(steps_page()); return true;
        default: break;
        }
    }
    return false;
}

void DebugTools::toggle(const std::string& what) {
    World& w = gm_.world();
    if (what == "hud") { gm_.hud_ = !gm_.hud_; say(std::string("Status line ") + (gm_.hud_ ? "on" : "off")); }
    else if (what == "boxes") { show_boxes = !show_boxes; say("Collision boxes " + onoff(show_boxes)); }
    else if (what == "editor") { gm_.opt_.show_editor = !gm_.opt_.show_editor; say("Eggs and editor items " + onoff(gm_.opt_.show_editor)); }
    else if (what == "routes") { show_routes = !show_routes; say("Pathfinder routes " + onoff(show_routes)); }
    else if (what == "roof") { gm_.opt_.roof_hiding = !gm_.opt_.roof_hiding; say("Roof hiding " + onoff(gm_.opt_.roof_hiding)); }
    else if (what == "crit") { show_crit = !show_crit; eval_timer_ = 0; say("Critical path " + onoff(show_crit)); }
    else if (what == "crit_route") {
        set_route_line(!route_line());
        say(std::string("Crit path: ") + (route_line() ? "route line" : "straight line"));
    }
    else if (what == "noclip") { w.debug_noclip = !w.debug_noclip; say("Noclip " + onoff(w.debug_noclip)); }
    else if (what == "god") { god = !god; say("God mode " + onoff(god)); }
    else if (what == "mana") { infinite_mana = !infinite_mana; say("Infinite mana " + onoff(infinite_mana)); }
    else if (what == "spells") {  // the spellbook lists and casts everything (game/spellbook.h)
        w.spellbook->debug_infinite = !w.spellbook->debug_infinite;
        say("Infinite spells " + onoff(w.spellbook->debug_infinite));
    }
    else if (what == "heal") heal();
    else if (what == "kill_pointer") {
        if (!kill(gm_.pick(gm_.mouse_x(), gm_.mouse_y()))) say("No actor under the pointer");
    } else if (what == "kill_pick") { pick = kPickKill; say("Click an actor to kill it (Esc cancels)", 300); }
    else if (what == "warp_click") { pick = kPickWarp; say("Click a spot to warp there (Esc cancels)", 300); }
    else if (what == "warp_atlas") {
        gm_.open_atlas();
        if (gm_.atlas_) gm_.atlas_->set_from_game(false);
        atlas_warp = true;
        say("Click a spot in the atlas to warp there", 300);
    } else if (what == "atlas") gm_.open_atlas();
    else if (what == "edit") editor_requested = true;  // the edit mode (main opens it)
    else if (what == "pause") { paused = !paused; say(paused ? "World paused (Ctrl-S steps)" : "World running"); }
    else if (what == "step") { if (!paused) paused = true; ++step_pending; }
    else if (what == "speed") { speed = speed >= 4 ? 1 : speed * 2; say("World speed " + std::to_string(speed) + "x"); }
    else if (what == "hour") advance_hours(1);
    else if (what == "snapshot") save_snapshot();
    else if (what == "load_snapshot") load_snapshot();
    else if (what.rfind("chapter:", 0) == 0) {  // a story chapter by number (1..) or id (scripts)
        const std::string c = what.substr(8);
        int idx = std::atoi(c.c_str()) - 1;
        for (size_t i = 0; i < chapters.size(); ++i)
            if (chapters[i].id == c) idx = int(i);
        if (menu()) { pending_jump_ = idx; close_menu(); }
        else jump_to_chapter(idx);
    } else if (what == "story_page" || what == "levels_page" || what == "virtues_page") {
        open_menu();
        if (DebugMenuGump* m = menu())
            m->push(what == "story_page" ? story_page() : what == "levels_page" ? levels_page() : virtues_page());
    }
}

int DebugTools::world_frames() {
    if (paused) {
        if (step_pending > 0) { --step_pending; return 1; }
        return 0;
    }
    return speed;
}

// ---------------------------------------------------------------------------------------------
// Actions

bool DebugTools::place_avatar_near(int x, int y, int z) {
    World& w = gm_.world();
    Actor* av = w.avatar();
    if (!av) return false;
    int xd, yd, zd;
    w.footprint(*av, xd, yd, zd);
    uint16_t fl = w.shape_flags(*av);
    static const int offs[][2] = {{0, 0}, {64, 0}, {0, 64}, {-64, 0}, {0, -64}, {64, 64}, {-64, -64}, {64, -64},
                                  {-64, 64}, {128, 0}, {0, 128}, {-128, 0}, {0, -128}};
    // The nearest height first, then the nearest spot: standing on something at about the
    // asked height beats the ground far below it.
    // Pass 0 wants a floor exactly under his feet; pass 1 takes the plain ground (z 0) too.
    for (int pass = 0; pass < 2; ++pass)
        for (int dz : {0, 8, -8, 16, -16, 24, -24, 32, -32, 40, 48, -48, 64, 80, 96, -64, -96, -128})
            for (const auto& o : offs) {
                int px = x + o[0], py = y + o[1], pz = std::clamp(z + dz, 0, 255);
                World::Placement p = w.can_exist(px, py, pz, xd, yd, zd, fl, 1);
                if (p.valid && p.supported && (pass == 1 || p.support_z == pz)) {
                    w.move_to(1, px, py, pz);
                    return true;
                }
            }
    return false;
}

bool DebugTools::warp_to(unsigned map, int x, int y, int z) {
    World& w = gm_.world();
    if (!w.avatar()) return false;
    w.teleport_actor(1, x, y, z, map);
    bool placed = place_avatar_near(x, y, z);
    w.camera_follow(1);
    char b[96];
    std::snprintf(b, sizeof b, "Warped to map %u at %d,%d,%d%s", map, w.avatar()->x, w.avatar()->y, w.avatar()->z,
                  placed ? "" : " (no free spot found)");
    say(b);
    return true;
}

bool DebugTools::teleport_to_npc(unsigned npc) {
    World& w = gm_.world();
    const Actor* a = w.actor(uint16_t(npc));
    if (!a || npc == 1) return false;
    unsigned m;
    int x, y, z;
    if (!ctx.npc_location(npc, m, x, y, z)) return false;
    if (m == 0) { say(a->name + " is not on any map"); return false; }
    std::string name = a->name;
    warp_to(m, x + 64, y, z);
    say("Teleported to " + name + " (map " + std::to_string(m) + ")");
    return true;
}

bool DebugTools::give(unsigned shape, unsigned frame, int qty, int quality, const std::string& name) {
    World& w = gm_.world();
    std::string what = name.empty() ? w.data().usecode.class_name(shape) : name;
    if (what.empty()) what = "shape " + std::to_string(shape);
    auto report = [&](const std::string& m) {
        say(m);
        if (DebugMenuGump* dm = menu()) dm->status = m;
        w.game_log.add(GameLog::kMessages, GameLog::kInfo, "Debug: " + m);
    };
    Actor* av = w.avatar();
    if (shape == 0 || shape >= w.data().shapes.count() || !av) { report("No such item: shape " + std::to_string(shape)); return false; }
    uint16_t id = w.create_item(shape, frame);
    Item* it = w.item(id);
    if (!it) { report("Cannot create shape " + std::to_string(shape)); return false; }
    const bool stack = sort_is_stack(w, *it);
    const unsigned count = stack ? unsigned(std::clamp(qty, 1, int(kMaxStack))) : 1u;
    if (stack) it->quality = uint16_t(count);
    else if (quality >= 0) it->quality = uint16_t(quality);
    // How many of these he carries (pack, keyring, bags...), for the confirmation: the same
    // shape and frame, or for a stack everything it would merge with.
    const Item proto = *it;
    auto carried = [&]() {
        unsigned n = 0;
        std::function<void(const Item&)> walk = [&](const Item& c) {
            for (uint16_t k : c.contents)
                if (const Item* ci = w.item(k)) {
                    if (ci->shape == shape) {
                        Item a = proto;
                        a.id = 0;
                        a.flags = (a.flags & uint16_t(~kItemOwned)) | (ci->flags & kItemOwned);
                        if (stack ? sort_can_merge(w, a, *ci) : ci->frame == frame)
                            n += stack ? std::max<unsigned>(1, ci->quality) : 1;
                    }
                    walk(*ci);
                }
        };
        if (const Actor* a = w.avatar()) walk(*a);
        return n;
    };
    std::string given = count > 1 ? std::to_string(count) + " x " + what : what;
    const uint16_t pack = w.equipped(1, 7);
    Item* bag = pack ? w.item(pack) : nullptr;
    // A stack first joins matching stacks in the pack (up to 666 each; the rest stays a stack of its own).
    if (stack && bag) {
        it->flags |= kItemOwned;  // as everything in his pack
        std::vector<uint16_t> grown;
        for (uint16_t k : bag->contents) {
            Item* t = w.item(k);
            if (!t || !sort_can_merge(w, *it, *t) || t->quality >= kMaxStack) continue;
            unsigned total = unsigned(t->quality) + it->quality;
            t->quality = uint16_t(std::min(kMaxStack, total));
            it->quality = uint16_t(total > kMaxStack ? total - kMaxStack : 0);
            grown.push_back(k);
            if (it->quality == 0) break;  // else the rest tries the next stack, then stands alone
        }
        it->flags &= uint16_t(~kItemOwned);
        for (uint16_t k : grown) w.call_event(k, kEvCombine);  // the pile's picture follows its amount
        if ((it = w.item(id)) && it->quality == 0) {
            w.destroy_now(id);
            report("Gave " + given + " (" + std::to_string(carried()) + " carried)");
            return true;
        }
        bag = w.item(pack);
    }
    // Into the pack if it has room (his carrying weight is not checked: this is a cheat).
    bool fits = false;
    if (bag && (it = w.item(id))) {
        unsigned used = 0;
        for (uint16_t k : bag->contents)
            if (const Item* ci = w.item(k)) used += w.item_volume(*ci);
        unsigned cap = w.data().typeflags.get(bag->shape).volume();
        if (cap == 0) cap = 32;
        fits = used + w.item_volume(*it) <= cap;
    }
    if (fits && w.move_to_container(id, pack, false)) {
        if (stack) w.call_event(id, kEvCombine);
        report("Gave " + given + " (" + std::to_string(carried()) + " carried)");
        return true;
    }
    // No room: put it at his feet.
    w.move_to(id, av->x + 32, av->y, av->z);
    if (stack) w.call_event(id, kEvCombine);
    report(std::string(bag ? "Backpack full" : "No backpack") + ": " + given + " dropped at his feet");
    return true;
}

void DebugTools::heal() {
    World& w = gm_.world();
    Actor* av = w.avatar();
    if (!av) return;
    av->hp = uint16_t(std::max(1, 2 * int(av->str)));
    av->mana = int16_t(std::max(0, 2 * int(av->intel)));
    say("Healed: HP " + std::to_string(av->hp) + ", mana " + std::to_string(av->mana));
}

bool DebugTools::kill(uint16_t id) {
    World& w = gm_.world();
    Actor* a = w.actor(id);
    if (!a || id == 1 || a->is(Actor::kDead)) return false;
    std::string name = a->name;
    w.kill_actor(id, 0);
    say("Killed " + (name.empty() ? "actor " + std::to_string(id) : name));
    return true;
}

unsigned DebugTools::hour() const { return gm_.world().game_seconds() / 900; }

void DebugTools::advance_hours(int h) {
    World& w = gm_.world();
    int nh = std::max(0, int(hour()) + h);
    w.set_game_hours(unsigned(nh));
    char b[64];
    std::snprintf(b, sizeof b, "Time: day %d, %02d:00", nh / 24 + 1, nh % 24);
    say(b);
}

bool DebugTools::save_snapshot() {
    int keep = gm_.saves().last_slot;
    bool ok = gm_.save_slot(SaveStore::kSnapshotSlot, "Debug snapshot");
    gm_.saves().last_slot = keep;
    say(ok ? "Snapshot saved (Ctrl-Shift-U loads it)" : "Snapshot failed");
    return ok;
}

bool DebugTools::load_snapshot() {
    bool ok = gm_.load_slot(SaveStore::kSnapshotSlot);
    say(ok ? "Snapshot loaded" : "No snapshot");
    return ok;
}

bool DebugTools::atlas_jump(unsigned map, const int at[3]) {
    if (!atlas_warp) return false;
    atlas_warp = false;
    if (gm_.atlas_) gm_.atlas_->set_from_game(true);
    warp_to(map, at[0], at[1], at[2]);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Story jump (NOTES.md "Story jump")

bool DebugTools::load_story(const std::string& json_path) {
    chapters.clear();
    std::ifstream f(json_path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.contains("chapters") || !j["chapters"].is_array()) {
        PG_LOG("story jump: %s is not valid", json_path.c_str());
        return false;
    }
    const size_t slash = json_path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : json_path.substr(0, slash);
    story_pack_path = dir + "/" + j.value("pack", std::string("story/checkpoints.pgs"));
    auto triples = [](const nlohmann::json& a, int def_count) {
        std::vector<std::array<int, 3>> out;
        if (!a.is_array()) return out;
        for (const nlohmann::json& e : a)
            if (e.is_array() && e.size() >= 2)
                out.push_back({e[0].get<int>(), e[1].get<int>(), e.size() > 2 ? e[2].get<int>() : def_count});
        return out;
    };
    for (const nlohmann::json& c : j["chapters"]) {
        StoryChapter ch;
        ch.id = c.value("id", std::string());
        ch.title = c.value("title", ch.id);
        ch.checkpoint = c.value("checkpoint", std::string());
        ch.kit = c.value("kit", std::string());
        ch.step = c.value("step", 0);
        ch.level = std::clamp(c.value("level", 1), 1, kMaxLevel);
        ch.xp = std::max(0, c.value("xp", 0));
        if (c.contains("stats_original") && c["stats_original"].is_array() && c["stats_original"].size() == 3)
            for (int k = 0; k < 3; ++k) ch.stats_original[size_t(k)] = c["stats_original"][size_t(k)].get<int>();
        if (c.contains("virtues") && c["virtues"].is_array())
            for (size_t k = 0; k < ch.virtues.size() && k < c["virtues"].size(); ++k) ch.virtues[k] = c["virtues"][k].get<int>();
        if (c.contains("equip")) ch.equip = triples(c["equip"], 1);
        if (c.contains("pack")) ch.pack = triples(c["pack"], 1);
        if (c.contains("remove")) ch.remove = triples(c["remove"], 0);
        chapters.push_back(std::move(ch));
    }
    PG_LOG("story jump: %zu chapters (%s)", chapters.size(), story_pack_path.c_str());
    return true;
}

bool DebugTools::have_checkpoints() const {
    if (story_ && !story_->entries.empty()) return true;
    std::ifstream f(story_pack_path, std::ios::binary);
    return !story_pack_path.empty() && bool(f);
}

int DebugTools::chapter_now() const {
    if (chapters.empty() || crit.steps.empty()) return -1;
    const int cur = crit.current(crit.done(ctx));
    const int step = cur < 0 ? int(crit.steps.size()) : cur;
    int best = 0;
    for (size_t i = 0; i < chapters.size(); ++i)
        if (chapters[i].step <= step) best = int(i);
    return best;
}

void DebugTools::typical_virtues(int i) {
    Virtues* v = gm_.world().virtues.get();
    if (!v || i < 0 || size_t(i) >= chapters.size()) return;
    for (int k = 0; k < kVirtueCount; ++k) v->set(k, chapters[size_t(i)].virtues[size_t(k)]);
}

bool DebugTools::jump_to_chapter(int i) {
    World& w = gm_.world();
    jump_error.clear();
    if (i < 0 || size_t(i) >= chapters.size()) { jump_error = "no such chapter"; return false; }
    const StoryChapter& c = chapters[size_t(i)];
    // What carries over: the virtues as they are now, the companions walking with him.
    std::array<int, 8> scores{};
    std::set<std::string> paid;
    if (w.virtues) {
        for (int k = 0; k < kVirtueCount; ++k) scores[size_t(k)] = w.virtues->get(k);
        paid = w.virtues->paid;
    }
    std::vector<std::string> companions;
    if (w.party && w.party->enabled && jump_companions)
        for (int m : w.party->members) {
            const CompanionState& st = w.party->state[size_t(m)];
            if (st.mode == CompanionState::kFollow || st.mode == CompanionState::kWait) companions.push_back(w.party->defs[size_t(m)].id);
        }
    if (c.checkpoint.empty()) {
        // The first chapter: a new game (without the intro movie).
        gm_.cancel_drag();
        for (Gump*& g : gm_.press_gump_) g = nullptr;
        gm_.hover_gump_ = nullptr;
        w.play_intro = false;
        w.new_game();
        world_reset();
    } else {
        if (!story_) {
            story_ = std::make_unique<StoryPack>();
            std::string err;
            if (!story_->load_file(story_pack_path, err)) {
                story_.reset();
                jump_error = "no story checkpoints (" + err + ")";
                say(jump_error, 150);
                return false;
            }
        }
        std::vector<uint8_t> bytes;
        std::string err;
        if (!story_->get(story_->find(c.checkpoint), bytes, err)) {
            jump_error = err;
            say("Story jump: " + err, 150);
            return false;
        }
        if (!gm_.load_bytes(bytes, "story checkpoint " + c.checkpoint)) {
            jump_error = "the checkpoint did not load";
            say("Story jump: " + jump_error, 150);
            return false;
        }
    }
    crit.forced_done.clear();
    crit.jump_to = -1;
    apply_chapter(c, scores, paid, companions);
    eval_timer_ = 0;
    char b[160];
    const Leveling* lv = w.leveling.get();
    if (lv && lv->enabled) std::snprintf(b, sizeof b, "Story: %s (level %d)", c.title.c_str(), lv->level_of(1));
    else std::snprintf(b, sizeof b, "Story: %s", c.title.c_str());
    say(b, 150);
    return true;
}

void DebugTools::apply_chapter(const StoryChapter& c, const std::array<int, 8>& scores, const std::set<std::string>& paid,
                               const std::vector<std::string>& companions) {
    World& w = gm_.world();
    Actor* av = w.avatar();
    if (!av) return;
    const bool checkpoint = !c.checkpoint.empty();
    if (checkpoint) {
        // The replay's own powers go: it made the Avatar invulnerable from the catacombs on.
        if (!(god && enabled())) av->set(Actor::kInvincible, false);
        set_invincible_ = false;
        av->set(Actor::kInCombat, false);
        // What a player would not have.
        auto carried = [&](unsigned shape, int frame) {
            std::vector<uint16_t> out;
            for (unsigned i = 1; i < w.objects().slots(); ++i) {
                const Item* it = w.item(uint16_t(i));
                if (!it || i == 1 || it->shape != shape || (frame >= 0 && int(it->frame) != frame) || !it->parent) continue;
                const Item* r = w.root_container(*it);
                if (r && r->id == 1) out.push_back(uint16_t(i));
            }
            return out;
        };
        for (const auto& r : c.remove)
            for (uint16_t id : carried(unsigned(r[0]), r[1])) w.destroy_now(id);
        // The weapons and armour a player has found or bought by now, worn.
        const uint16_t pack = w.equipped(1, 7);
        // (Something already worn in that slot goes into the backpack.)
        for (const auto& e : c.equip) {
            uint16_t id = w.create_item(unsigned(e[0]), unsigned(e[1]));
            if (!w.item(id)) continue;
            if (pack) w.move_to_container(id, pack);
            if (!w.equip(1, id)) PG_LOG("story jump: could not equip shape %d", e[0]);
        }
        for (const auto& p : c.pack) {
            if (!pack) break;
            const unsigned fam = w.data().typeflags.get(unsigned(p[0])).family();
            const int n = std::max(1, p[2]);
            const bool stack = fam == kFamQuantity || fam == kFamReagent;
            for (int k = 0; k < (stack ? 1 : n); ++k) {
                uint16_t id = w.create_item(unsigned(p[0]), unsigned(p[1]));
                Item* it = w.item(id);
                if (!it) break;
                if (stack) it->quality = uint16_t(n);
                w.move_to_container(id, pack);
                if (stack) w.call_event(id, kEvCombine);  // coins pick their pile frame
            }
        }
    }
    // Levels (Leveling on) or the usual stats by now (off).
    Leveling* lv = w.leveling.get();
    if (checkpoint) {
        if (lv && lv->enabled) {
            lv->start_at_story(c.level, c.xp);
        } else {
            av->str = int16_t(c.stats_original[0]);
            av->dex = int16_t(c.stats_original[1]);
            av->intel = int16_t(c.stats_original[2]);
        }
    }
    av->hp = uint16_t(w.party->max_hp(*av));
    av->mana = int16_t(w.party->max_mana(*av));
    // Virtues: as they were, typical for this point, or the replay's (the checkpoint's own).
    if (Virtues* v = w.virtues.get()) {
        if (jump_virtues == kKeepVirtues) {
            for (int k = 0; k < kVirtueCount; ++k) v->set(k, scores[size_t(k)]);
            v->paid = paid;
        } else if (jump_virtues == kTypicalVirtues) {
            for (int k = 0; k < kVirtueCount; ++k) v->set(k, c.virtues[size_t(k)]);
        }
        // What the world already shows done (the replay's deeds, the kit) pays nothing now.
        if (checkpoint && jump_virtues != kReplayVirtues) v->settle_deeds();
    }
    // The companions who were walking with him join him here, at his level.
    Party* party = w.party.get();
    if (checkpoint && party && party->enabled && !companions.empty()) {
        if (!party->spawned_once) party->spawn_all();
        for (const std::string& id : companions) {
            const int idx = party->find_def(id);
            if (idx < 0) continue;
            CompanionState& st = party->state[size_t(idx)];
            if (st.mode == CompanionState::kUnspawned) continue;
            if (st.mode == CompanionState::kDead) party->revive(idx, 100);
            if (!party->in_party(idx)) party->join(idx);
            else party->follow(idx);
            party->bring_to_avatar(idx);
            if (lv && lv->enabled) lv->set_level(st.id, c.level, true, c.xp);
            if (Actor* a = w.actor(st.id)) {
                a->hp = uint16_t(party->max_hp(*a));
                a->mana = int16_t(party->max_mana(*a));
            }
        }
    }
    PG_LOG("story jump: %s (%s), level %d, STR %d DEX %d INT %d", c.id.c_str(), checkpoint ? c.checkpoint.c_str() : "a new game",
           lv ? lv->level_of(1) : 1, av->str, av->dex, av->intel);
}

// ---------------------------------------------------------------------------------------------
// The tracker and autosaves

void DebugTools::world_reset() {
    baseline_ = true;
    autosave_pending_ = false;
    pending_title_.clear();
    ctx.reset();
    eval_timer_ = 0;
    route->clear();            // a new game or a load: nothing of the old route or grid holds
    route->invalidate_grid();
}

void DebugTools::evaluate(bool force_guide) {
    World& w = gm_.world();
    if (!w.avatar()) return;
    ++evaluations;
    ctx.observe();
    std::vector<bool> now(crit.steps.size(), false);
    for (size_t i = 0; i < crit.steps.size(); ++i) now[i] = crit.steps[i].done_when.eval(ctx);
    if (baseline_ || now.size() != natural_done.size()) {
        baseline_ = false;
    } else {
        // Autosaves follow the major steps: milestones (all steps if the data marks none).
        int completed = -1;
        bool only_ms = crit.has_milestones();
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i] && !natural_done[i]) {
                const CritStep& st = crit.steps[i];
                if (!only_ms || st.milestone) completed = int(i);
                PG_LOG("critpath: step %s \"%s\" completed%s", st.id.c_str(), st.title.c_str(), st.milestone ? " (milestone)" : "");
            }
        if (completed >= 0 && autosave_enabled()) {
            autosave_pending_ = true;
            pending_title_ = crit.steps[size_t(completed)].title;
            pending_frames_ = 0;
        }
    }
    natural_done = now;
    if (force_guide || show_crit) guide = crit.guide(ctx);
}

std::string DebugTools::unsafe_reason() const {
    World& w = gm_.world();
    const Actor* av = w.avatar();
    if (!av || av->is(Actor::kDead) || w.game_over) return "dead";
    if (w.stasis) return "stasis (a scene)";
    if (w.ask_menu) return "conversation";
    if (w.target_pid) return "target cursor";
    // Speech of the Avatar or of someone close by is a conversation; far-off barks
    // (guards calling out) are not.
    for (const Bark& b : w.barks) {
        const Item* sp = w.item(b.speaker);
        if (!sp || b.speaker == 1) return "speech";
        int x, y, z;
        w.location(*sp, x, y, z);
        if (std::abs(x - av->x) + std::abs(y - av->y) < 384) return "speech";
    }
    if (av->is(Actor::kInCombat) || av->is(Actor::kCombatRun)) return "combat";
    if (av->gravity_pid) return "falling or jumping";
    if (w.gumps.top_modal()) return "a window";
    if (paused) return "paused";
    if (gm_.dragging()) return "dragging";
    if (gm_.atlas_open() || gm_.help_open()) return "atlas / help";
    if (!w.can_save()) return "saving not allowed";
    return "";
}

bool DebugTools::safe_to_save() const { return unsafe_reason().empty(); }

bool DebugTools::autosave_now(const std::string& title) {
    World& w = gm_.world();
    SaveStore& st = gm_.saves();
    int last = Config::global().get_int("autosave_last", 0);
    int slot = (last >= SaveStore::kAutoFirst && last < SaveStore::kAutoFirst + SaveStore::kAutoSlots)
                   ? SaveStore::kAutoFirst + (last - SaveStore::kAutoFirst + 1) % SaveStore::kAutoSlots
                   : SaveStore::kAutoFirst;
    char when[32] = "";
    std::time_t t = std::time(nullptr);
    if (std::tm* lt = std::localtime(&t)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", lt);
    unsigned h = w.game_seconds() / 900;
    char desc[200];
    std::snprintf(desc, sizeof desc, "Autosave - %s (%s, day %u %02u:00)", title.c_str(), when, h / 24 + 1, h % 24);
    int keep = st.last_slot;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = gm_.save_slot(slot, desc);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    st.last_slot = keep;
    if (!ok) return false;
    Config::global().set_int("autosave_last", slot);
    Config::global().save();
    ++autosaves_written;
    last_autosave_label = desc;
    last_autosave_slot = slot;
    autosaved_note_ = 60;
    PG_LOG("autosave: slot %d \"%s\" (%.1f ms)", slot, desc, ms);
    return true;
}

bool DebugTools::load_last_autosave() {
    int s = gm_.saves().latest_autosave();
    return s && gm_.load_slot(s);
}

void DebugTools::tick() {
    World& w = gm_.world();
    ++frame_;
    bool en = enabled();
    if (en && !was_enabled_) gm_.toast("F10: debug menu", 120);
    was_enabled_ = en;
    if (w.party) w.party->set_debug_beach(en);  // companions not yet recruited: the beach / their homes
    if (!gm_.atlas_open() && atlas_warp) {
        atlas_warp = false;
        if (gm_.atlas_) gm_.atlas_->set_from_game(true);
    }
    if (message_frames > 0) --message_frames;
    if (!pending_.empty() && !menu()) {
        std::string a = pending_;
        pending_.clear();
        toggle(a);
    }
    if (pending_jump_ >= 0 && !menu()) {
        const int c = pending_jump_;
        pending_jump_ = -1;
        jump_to_chapter(c);
    }
    if (autosaved_note_ > 0) --autosaved_note_;
    Actor* av = w.avatar();
    if (av && !av->is(Actor::kDead)) {
        if (god && en) {
            av->hp = uint16_t(std::max<int>(av->hp, 2 * av->str));
            if (!av->is(Actor::kInvincible)) { av->set(Actor::kInvincible, true); set_invincible_ = true; }
        } else if (set_invincible_) {
            av->set(Actor::kInvincible, false);
            set_invincible_ = false;
        }
        if (infinite_mana && en) av->mana = int16_t(std::max<int>(av->mana, 2 * av->intel));
    }
    if (!en) { pick = kPickNone; }
    // The tracker: three times a second.
    if (!crit.steps.empty() && --eval_timer_ <= 0) {
        eval_timer_ = 10;
        evaluate(false);
    }
    // The walking route: planned a slice at a time while the line is up in route mode.
    if (en && show_crit && route_line() && guide.have_point && av) {
        // The teleporters inside this map it may lead through (their conditions now).
        if (eval_timer_ == 10 || !route->has_target()) route->set_links(w.map_number_now(), crit.walk_links(ctx, w.map_number_now()));
        route->set_target(w.map_number_now(), guide.x, guide.y, guide.z);
#ifdef __EMSCRIPTEN__
        const double budget = route_budget_ms > 0 ? route_budget_ms : 1.5;
#else
        const double budget = route_budget_ms > 0 ? route_budget_ms : 2.0;
#endif
        route->update(budget);
    } else if (route->has_target()) {
        route->clear();
    }
    // A pending autosave waits for a safe moment; after 60 s speech, combat or a fall no longer
    // hold it back (a scene in stasis, a window, death and pause always do).
    if (autosave_pending_) {
        ++pending_frames_;
        if (pending_frames_ % 300 == 0) PG_LOG("autosave: waiting (%s)", unsafe_reason().c_str());
        bool dead = !av || av->is(Actor::kDead) || w.game_over;
        bool ok = safe_to_save() ||
                  (pending_frames_ > 1800 && !dead && !w.stasis && !w.gumps.top_modal() && !paused && w.can_save());
        if (ok) {
            autosave_pending_ = false;
            bool strict = safe_to_save();
            if (autosave_now(pending_title_)) {
                last_autosave_safe = strict;
                last_autosave_frame = gm_.frames();
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing

void DebugTools::to_screen(int x, int y, int z, int& sx, int& sy) const {
    World& w = gm_.world();
    if (w.screen_mapper) { w.screen_mapper(x, y, z, sx, sy); return; }
    int cam[3];
    w.camera_point(cam);
    int camSx = (cam[0] - cam[1]) / 4, camSy = (cam[0] + cam[1]) / 8 - cam[2];
    sx = 160 + (x - y) / 4 - camSx;
    sy = 100 + (x + y) / 8 - z - camSy;
}

void DebugTools::draw_under(Surface8& s) {
    if (!enabled()) return;
    if (show_boxes) draw_boxes(s);
    if (show_routes) draw_routes(s);
    if (show_crit) draw_crit(s);
}

void DebugTools::draw_boxes(Surface8& s) {
    World& w = gm_.world();
    const Palette& p = w.data().palette;
    const uint8_t red = nearest(p, 255, 40, 40), yel = nearest(p, 255, 230, 0), cyan = nearest(p, 0, 230, 255);
    CurrentMap& m = w.map();
    int drawn = 0;
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            if (!m.is_fast(cx, cy)) continue;
            for (uint16_t id : m.chunk(cx, cy)) {
                const Item* it = w.item(id);
                if (!it || (it->ext_flags & kExtSprite)) continue;
                const ShapeInfo& si = w.data().typeflags.get(it->shape);
                bool actor = it->as_actor() != nullptr;
                // Solid things; floors (land) would cover the screen, so they are left out.
                if (!actor && (!(si.flags() & ShapeInfo::kSolid) || (si.flags() & ShapeInfo::kLand))) continue;
                Box b = w.box_of(*it);
                if (b.xd == 0 && b.yd == 0) continue;
                int ax, ay;
                to_screen(b.x, b.y, b.z, ax, ay);
                if (ax < -80 || ay < -80 || ax > 400 || ay > 300) continue;
                uint8_t c = id == 1 ? cyan : actor ? yel : red;
                int X[2] = {b.x - b.xd, b.x}, Y[2] = {b.y - b.yd, b.y}, Z[2] = {b.z, b.z + b.zd};
                int px[8], py[8];
                for (int k = 0; k < 8; ++k) to_screen(X[k & 1], Y[(k >> 1) & 1], Z[k >> 2], px[k], py[k]);
                static const int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4},
                                             {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& ed : e) line(s, px[ed[0]], py[ed[0]], px[ed[1]], py[ed[1]], c);
                if (++drawn > 1500) return;
            }
        }
}

void DebugTools::draw_routes(Surface8& s) {
    World& w = gm_.world();
    const Palette& p = w.data().palette;
    const uint8_t cyan = nearest(p, 0, 230, 255), dark = nearest(p, 0, 40, 60);
    w.kernel().for_each([&](Process* pr) {
        auto* pf = dynamic_cast<PathfindProcess*>(pr);
        if (!pf || !pf->live()) return;
        const Item* a = w.item(pf->item);
        if (!a || a->chunk < 0) return;
        Box b = w.box_of(*a);
        int x0, y0, x1, y1, gx, gy, gz;
        to_screen(b.x - b.xd / 2, b.y - b.yd / 2, b.z, x0, y0);
        pf->goal(gx, gy, gz);
        to_screen(gx, gy, gz, x1, y1);
        line(s, x0, y0, x1, y1, cyan, dark, frame_ / 2);
        line(s, x1 - 3, y1 - 3, x1 + 3, y1 + 3, cyan);
        line(s, x1 - 3, y1 + 3, x1 + 3, y1 - 3, cyan);
    });
}

// The walking route (game/route_guide.h): a dashed line along the route's points from where the
// Avatar is on it, rounded at the corners, with small chevrons where it goes up or down; off
// screen an arrow at the edge along the route's own direction and the walking distance left.
bool DebugTools::draw_route_line(Surface8& s) {
    World& w = gm_.world();
    const Actor* av = w.avatar();
    const Route& rt = route->route();
    if (!av || !route->current() || !rt.usable() || rt.pts.size() < 2) return false;
    const Palette& p = w.data().palette;
    const uint8_t yel = nearest(p, 255, 230, 0), blk = nearest(p, 0, 0, 0);
    Box b = w.box_of(*av);
    const int fx = b.x - b.xd / 2, fy = b.y - b.yd / 2, fz = b.z;
    RouteGuide::Where wh = route->where(fx, fy, fz);
    // World points from the Avatar on: him, his point on the route, the points after it.
    struct WP { int x, y, z; };
    std::vector<WP> wp{{fx, fy, fz}};
    if (std::abs(wh.px - fx) + std::abs(wh.py - fy) > 8) wp.push_back({wh.px, wh.py, wh.pz});
    // (To the end of this leg: the next teleporter on the way, or the target.)
    for (size_t i = wh.seg + 1; i <= wh.leg_end && i < rt.pts.size(); ++i) wp.push_back({rt.pts[i].x, rt.pts[i].y, rt.pts[i].z});
    // Screen points, then rounded corners (two passes of corner cutting, the ends kept).
    std::vector<std::pair<double, double>> sp;
    for (const WP& q : wp) {
        int sx, sy;
        to_screen(q.x, q.y, q.z, sx, sy);
        if (sp.empty() || std::abs(sp.back().first - sx) + std::abs(sp.back().second - sy) >= 1) sp.push_back({double(sx), double(sy)});
    }
    for (int pass = 0; pass < 2 && sp.size() > 2; ++pass) {
        std::vector<std::pair<double, double>> o{sp.front()};
        for (size_t i = 0; i + 1 < sp.size(); ++i) {
            const auto &a = sp[i], &c = sp[i + 1];
            double seg = std::abs(c.first - a.first) + std::abs(c.second - a.second);
            if (seg < 6) { if (i + 1 < sp.size() - 1) o.push_back(c); continue; }
            if (i > 0) o.push_back({0.75 * a.first + 0.25 * c.first, 0.75 * a.second + 0.25 * c.second});
            if (i + 2 < sp.size()) o.push_back({0.25 * a.first + 0.75 * c.first, 0.25 * a.second + 0.75 * c.second});
        }
        o.push_back(sp.back());
        sp.swap(o);
    }
    const int L = 6, T = 6, R = 313, B = 160;  // above the hint lines
    auto inside = [&](double x, double y) { return x >= L && x <= R && y >= T && y <= B; };
    const int phase = (frame_ / 2) & 7;
    int run = 0;  // pixels along the line so far (the dashes march along the whole route)
    bool left_screen = false;
    double ex = 0, ey = 0, edx = 0, edy = 0;
    for (size_t i = 0; i + 1 < sp.size(); ++i) {
        double x0 = sp[i].first, y0 = sp[i].second, x1 = sp[i + 1].first, y1 = sp[i + 1].second;
        bool out_after = !inside(x1, y1);
        double cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
        if (clip(cx0, cy0, cx1, cy1, L, T, R, B)) {
            line(s, int(cx0), int(cy0), int(cx1), int(cy1), yel, blk, (8 - phase + run) & 7, true);
            run += int(std::max(std::abs(cx1 - cx0), std::abs(cy1 - cy0)));
        }
        if (out_after && inside(x0, y0)) {
            // Leaves the screen here: the arrow follows this stretch of the route.
            left_screen = true;
            ex = cx1; ey = cy1;
            edx = x1 - x0; edy = y1 - y0;
            break;
        }
        if (out_after && !inside(x0, y0)) {
            // (Already outside: the Avatar's own end is off screen, e.g. zoomed in; stop.)
            left_screen = true;
            ex = std::clamp(x0, double(L), double(R)); ey = std::clamp(y0, double(T), double(B));
            edx = x1 - x0; edy = y1 - y0;
            break;
        }
    }
    // Up / down chevrons where the route climbs or descends (stairs, ledges, drops).
    {
        int base = fz, last_x = -1000, last_y = -1000;
        for (size_t i = 1; i < wp.size(); ++i) {
            int dz = wp[i].z - base;
            if (std::abs(dz) < 16) continue;
            base = wp[i].z;
            int sx, sy;
            to_screen(wp[i].x, wp[i].y, wp[i].z, sx, sy);
            if (!inside(sx, sy) || std::abs(sx - last_x) + std::abs(sy - last_y) < 14) continue;
            last_x = sx; last_y = sy;
            const bool up = dz > 0;
            for (int k = 0; k < 2; ++k) {
                uint8_t c = k ? yel : blk;
                int o = k ? 0 : 1;
                for (int j = 0; j < 2; ++j) {  // a double chevron
                    int ty = sy - 3 + (up ? -j * 3 : j * 3) + o;
                    line(s, sx - 3 + o, ty + (up ? 3 : -3), sx + o, ty, c);
                    line(s, sx + o, ty, sx + 3 + o, ty + (up ? 3 : -3), c);
                }
            }
        }
    }
    const int dz = (wh.jump ? wp.back().z : guide.z) - b.z;  // (a leg ends at its teleporter)
    auto height_marker = [&](int px, int py) {
        if (std::abs(dz) < 16) return;
        const bool up = dz > 0;
        const ShapeFont& hf = w.font(0);
        char hb[24];
        std::snprintf(hb, sizeof hb, "%s %d", up ? "Above" : "Below", std::abs(dz));
        int ax = std::clamp(px + 10, 6, 300 - hf.text_width(hb));
        int ay = std::clamp(py - 6, 10, 150);
        int bob = (frame_ / 8) % 2;
        int tip = up ? ay - 7 - bob : ay + 7 + bob;
        for (int k = 0; k < 2; ++k) {
            uint8_t c = k ? yel : blk;
            int o = k ? 0 : 1;
            line(s, ax + o, ay, ax + o, tip, c);
            line(s, ax + o, tip, ax - 3 + o, tip + (up ? 3 : -3), c);
            line(s, ax + o, tip, ax + 3 + o, tip + (up ? 3 : -3), c);
        }
        int lx = ax + 6, ly = ay - hf.height() / 2;
        s.fill_rect(lx - 1, ly - 1, hf.text_width(hb) + 2, hf.height() + 2, blk);
        hf.draw_top(s, lx, ly, hb);
    };
    if (!left_screen) {
        int sx1 = int(sp.back().first), sy1 = int(sp.back().second);
        height_marker(sx1, sy1);
        int r = 4 + (frame_ / 6) % 3;
        for (int k = 0; k < 2; ++k) {
            uint8_t c = k ? yel : blk;
            int rr = r - k;
            line(s, sx1 - rr, sy1, sx1, sy1 - rr, c);
            line(s, sx1, sy1 - rr, sx1 + rr, sy1, c);
            line(s, sx1 + rr, sy1, sx1, sy1 + rr, c);
            line(s, sx1, sy1 + rr, sx1 - rr, sy1, c);
        }
        return true;
    }
    double len = std::sqrt(edx * edx + edy * edy);
    if (len < 1e-6) return true;
    edx /= len;
    edy /= len;
    int tx = int(ex), ty = int(ey);
    for (int side : {-1, 1}) {
        double ax = -edx * 8 + side * -edy * 5, ay = -edy * 8 + side * edx * 5;
        line(s, tx, ty, tx + int(ax), ty + int(ay), yel, 0, -1, true);
        line(s, tx + 1, ty + 1, tx + int(ax) + 1, ty + int(ay) + 1, blk);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", wh.left);
    const ShapeFont& f = w.font(0);
    int lx = std::clamp(tx - int(edx * 18) - f.text_width(buf) / 2, 2, 318 - f.text_width(buf));
    int ly = std::clamp(ty - int(edy * 18) - f.height() / 2, 2, 196 - f.height());
    s.fill_rect(lx - 1, ly - 1, f.text_width(buf) + 2, f.height() + 2, blk);
    f.draw_top(s, lx, ly, buf);
    // The height marker beside the distance, toward the middle of the screen (never over it).
    {
        const int hw = 60;  // about the marker's width ("Above 104" with its arrow)
        int hx = lx + f.text_width(buf) / 2 - hw / 2 - 10;
        int hy = ly < 80 ? ly + f.height() + 12 : ly - 10;
        height_marker(std::clamp(hx, 0, 300 - hw), hy);
    }
    return true;
}

std::string DebugTools::route_hint() const {
    if (!route_line() || !guide.have_point || !route->has_target()) return "";
    World& w = gm_.world();
    const Route& rt = route->route();
    if (!route->current()) return route->busy() ? "Finding the walking route..." : "";
    if (rt.status == Route::kFailed) return "No walking route found - " + rt.reason;
    const Actor* av = w.avatar();
    if (!av) return "";
    Box b = w.box_of(*av);
    RouteGuide::Where wh = route->where(b.x - b.xd / 2, b.y - b.yd / 2, b.z);
    std::string via = route->via(wh.seg);
    // (A teleporter inside this map first: the hint is for this leg.)
    if (!wh.jump && guide.link >= 0 && size_t(guide.link) < crit.links.size()) {
        const MapLink& l = crit.links[size_t(guide.link)];
        std::string t;
        if (w.stitched() && w.stitch && w.stitch->seam(w.map_number_now(), l.to_map)) t = "walk on into " + map_name(l.to_map);
        else if (l.kind == "egg") t = "take the teleporter";
        else if (l.has_point) t = "use the way into " + map_name(l.to_map);
        if (!t.empty()) via = via.empty() ? t : via + ", then " + t;
    }
    std::string h = "Walk " + std::to_string(wh.left);
    if (!via.empty()) h += ": " + via;
    if (rt.status == Route::kNear && !rt.reason.empty()) h += " (" + rt.reason + ")";
    return h;
}

void DebugTools::draw_crit(Surface8& s) {
    World& w = gm_.world();
    const Actor* av = w.avatar();
    if (!av || !guide.have_point) return;
    if (route_line() && draw_route_line(s)) return;
    const Palette& p = w.data().palette;
    const uint8_t yel = nearest(p, 255, 230, 0), blk = nearest(p, 0, 0, 0);
    Box b = w.box_of(*av);
    int sx0, sy0, sx1, sy1;
    to_screen(b.x - b.xd / 2, b.y - b.yd / 2, b.z + b.zd / 2, sx0, sy0);
    to_screen(guide.x, guide.y, guide.z, sx1, sy1);
    const int L = 6, T = 6, R = 313, B = 160;  // above the hint lines
    bool inside = sx1 >= L && sx1 <= R && sy1 >= T && sy1 <= B;
    double x0 = sx0, y0 = sy0, x1 = sx1, y1 = sy1;
    if (!clip(x0, y0, x1, y1, L, T, R, B)) return;
    int phase = (frame_ / 2) & 7;
    line(s, int(x0), int(y0), int(x1), int(y1), yel, blk, 8 - phase, true);
    // The target above or below the Avatar: an up / down arrow beside the end of the line with
    // the height difference, so a spot on a ledge or the floor above isn't mistaken for one here.
    const int dz = guide.z - b.z;
    auto height_marker = [&](int px, int py) {
        if (std::abs(dz) < 16) return;
        const bool up = dz > 0;
        const ShapeFont& hf = w.font(0);
        char hb[24];
        std::snprintf(hb, sizeof hb, "%s %d", up ? "Above" : "Below", std::abs(dz));
        int ax = std::clamp(px + 10, 6, 300 - hf.text_width(hb));
        int ay = std::clamp(py - 6, 10, 150);
        int bob = (frame_ / 8) % 2;                // a gentle bob in the arrow's direction
        int tip = up ? ay - 7 - bob : ay + 7 + bob;
        for (int k = 0; k < 2; ++k) {              // black outline, then yellow
            uint8_t c = k ? yel : blk;
            int o = k ? 0 : 1;
            line(s, ax + o, ay, ax + o, tip, c);
            line(s, ax + o, tip, ax - 3 + o, tip + (up ? 3 : -3), c);
            line(s, ax + o, tip, ax + 3 + o, tip + (up ? 3 : -3), c);
        }
        int lx = ax + 6, ly = ay - hf.height() / 2;
        s.fill_rect(lx - 1, ly - 1, hf.text_width(hb) + 2, hf.height() + 2, blk);
        hf.draw_top(s, lx, ly, hb);
    };
    if (inside) {
        height_marker(sx1, sy1);
        // A pulsing diamond on the target.
        int r = 4 + (frame_ / 6) % 3;
        for (int k = 0; k < 2; ++k) {
            uint8_t c = k ? yel : blk;
            int rr = r - k;
            line(s, sx1 - rr, sy1, sx1, sy1 - rr, c);
            line(s, sx1, sy1 - rr, sx1 + rr, sy1, c);
            line(s, sx1 + rr, sy1, sx1, sy1 + rr, c);
            line(s, sx1, sy1 + rr, sx1 - rr, sy1, c);
        }
        return;
    }
    // Off screen: an arrow at the edge and the distance.
    double dx = sx1 - sx0, dy = sy1 - sy0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len;
    dy /= len;
    int tx = int(x1), ty = int(y1);
    height_marker(std::clamp(tx - int(dx * 30), 10, 290), std::clamp(ty - int(dy * 30), 16, 150));
    for (int side : {-1, 1}) {
        double ax = -dx * 8 + side * -dy * 5, ay = -dy * 8 + side * dx * 5;
        line(s, tx, ty, tx + int(ax), ty + int(ay), yel, 0, -1, true);
        line(s, tx + 1, ty + 1, tx + int(ax) + 1, ty + int(ay) + 1, blk);
    }
    int wx = guide.x - (b.x - b.xd / 2), wy = guide.y - (b.y - b.yd / 2);
    int dist = int(std::sqrt(double(wx) * wx + double(wy) * wy));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", dist);
    const ShapeFont& f = w.font(0);
    int lx = std::clamp(tx - int(dx * 18) - f.text_width(buf) / 2, 2, 318 - f.text_width(buf));
    int ly = std::clamp(ty - int(dy * 18) - f.height() / 2, 2, 196 - f.height());
    s.fill_rect(lx - 1, ly - 1, f.text_width(buf) + 2, f.height() + 2, blk);
    f.draw_top(s, lx, ly, buf);
}

void DebugTools::draw_over(Surface8& s) {
    World& w = gm_.world();
    const ShapeFont& f = w.font(0);
    auto boxed = [&](int x, int y, const std::string& t) {
        int tw = f.text_width(t);
        x = std::clamp(x, 1, std::max(1, 319 - tw));
        s.fill_rect(x - 2, y - 1, tw + 4, f.height() + 2, 0);
        f.draw_top(s, x, y, t);
    };
    if (autosaved_note_ > 0) boxed(318 - f.text_width("Autosaved"), 200 - f.height() - 2, "Autosaved");
    if (!enabled()) return;
    int y = 200 - 2;
    if (show_crit && !menu()) {
        std::vector<std::string> ls;
        ls.push_back(guide.line1.empty() ? (crit.steps.empty() ? "No critical path data" : "...") : guide.line1);
        if (!guide.line2.empty()) ls.push_back(guide.line2);
        if (std::string rh = route_hint(); !rh.empty()) ls.push_back(rh);
        // Long lines are word-wrapped to the screen width (never cut off).
        auto wrap = [&](const std::string& text) {
            std::vector<std::string> out;
            std::string cur, word;
            auto flush_word = [&]() {
                if (word.empty()) return;
                std::string cand = cur.empty() ? word : cur + " " + word;
                if (!cur.empty() && f.text_width(cand) > 300) { out.push_back(cur); cur = word; }
                else cur = cand;
                // a single word wider than the line: hard-split it
                while (f.text_width(cur) > 300 && cur.size() > 1) {
                    size_t n = cur.size() - 1;
                    while (n > 1 && f.text_width(cur.substr(0, n)) > 300) --n;
                    out.push_back(cur.substr(0, n));
                    cur = cur.substr(n);
                }
                word.clear();
            };
            for (char c : text) { if (c == ' ') flush_word(); else word += c; }
            flush_word();
            if (!cur.empty()) out.push_back(cur);
            return out;
        };
        {
            std::vector<std::string> wrapped;
            for (const std::string& l : ls) for (std::string& p : wrap(l)) wrapped.push_back(std::move(p));
            ls.swap(wrapped);
        }
        y -= int(ls.size()) * (f.line_advance() + 2);
        if (autosaved_note_ > 0) y -= f.line_advance() + 2;
        for (size_t i = 0; i < ls.size(); ++i) boxed(160 - f.text_width(ls[i]) / 2, y + int(i) * (f.line_advance() + 2), ls[i]);
    }
    std::string status;
    if (paused) status += "PAUSED  ";
    if (speed > 1) status += std::to_string(speed) + "x  ";
    if (w.debug_noclip) status += "NOCLIP  ";
    if (god) status += "GOD  ";
    if (!status.empty()) boxed(318 - f.text_width(status), 24, status);
    if (message_frames > 0 && !message.empty() && !menu()) boxed(160 - f.text_width(message) / 2, 34, message);
}

// ---------------------------------------------------------------------------------------------
// The menu pages

DebugPage DebugTools::root_page() {
    DebugPage pg;
    pg.title = "Debug menu";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        auto hdr = [&](const std::string& t) { DebugRow d; d.label = t; d.header = true; r.push_back(d); };
        auto tog = [&](const std::string& label, const std::string& key, const char* what, std::function<bool()> v) {
            DebugRow d;
            d.label = label;
            d.shortcut = key;
            d.value = [v] { return onoff(v()); };
            std::string wh = what;
            d.act = [this, wh](int) { toggle(wh); };
            r.push_back(d);
        };
        auto act = [&](const std::string& label, const std::string& key, std::function<void()> f, bool closes = false) {
            DebugRow d;
            d.label = label;
            d.shortcut = key;
            d.act = [f](int) { f(); };
            d.closes = closes;
            r.push_back(d);
        };
        hdr("Show");
        tog("Status line (HUD)", "Ctrl-H  H", "hud", [this] { return gm_.hud_; });
        tog("Collision boxes", "Ctrl-B", "boxes", [this] { return show_boxes; });
        tog("Eggs and editor items", "Ctrl-E  E", "editor", [this] { return gm_.opt_.show_editor; });
        tog("Pathfinder routes", "Ctrl-P", "routes", [this] { return show_routes; });
        tog("Roof hiding", "R", "roof", [this] { return gm_.opt_.roof_hiding; });
        tog("Show crit path", "Ctrl-J", "crit", [this] { return show_crit; });
        {
            DebugRow d;
            d.label = "Crit path";
            d.value = [this] { return std::string(route_line() ? "route line" : "straight line"); };
            d.act = [this](int) { toggle("crit_route"); };
            r.push_back(d);
        }
        hdr("Story");
        act("Crit path: list steps...", "Ctrl-N", [this] { if (DebugMenuGump* m = menu()) m->push(steps_page()); });
        act("Mark step done / jump to step...", "", [this] { if (DebugMenuGump* m = menu()) m->push(steps_page()); });
        act("Global flags...", "", [this] { if (DebugMenuGump* m = menu()) m->push(globals_page()); });
        act("Jump to story chapter...", "", [this] { if (DebugMenuGump* m = menu()) m->push(story_page()); });
        act("Levels (Avatar, companions)...", "", [this] { if (DebugMenuGump* m = menu()) m->push(levels_page()); });
        hdr("Cheats");
        tog("Noclip (walk through walls)", "Ctrl-C", "noclip", [&w] { return w.debug_noclip; });
        tog("God mode (no damage)", "Ctrl-G", "god", [this] { return god; });
        tog("Infinite mana", "Ctrl-M", "mana", [this] { return infinite_mana; });
        tog("Infinite spells (spellbook, G)", "", "spells", [&w] { return w.spellbook->debug_infinite; });
        act("Heal (HP and mana)", "Ctrl-L", [this] { heal(); });
        act("Kill: click an actor", "Ctrl-K pointer", [this] { toggle("kill_pick"); }, true);
        act("Give item...", "Ctrl-I", [this] { if (DebugMenuGump* m = menu()) m->push(items_page()); });
        hdr("Travel");
        act("Warp: click a spot here", "Ctrl-V", [this] { toggle("warp_click"); }, true);
        act("Warp: click in the atlas", "Ctrl-A", [this] { toggle("warp_atlas"); }, true);
        act("Warp to map...", "", [this] { if (DebugMenuGump* m = menu()) m->push(maps_page()); });
        act("Teleport to NPC...", "Ctrl-F", [this] { if (DebugMenuGump* m = menu()) m->push(npcs_page()); });
        act("Open the atlas", "M  Tab", [this] { toggle("atlas"); }, true);
        act("Editor (paint the world)", "", [this] { toggle("edit"); }, true);
        if (w.party && w.party->enabled) {  // companions (game/party.h)
            hdr("Companions");
            Party* p = w.party.get();
            act("Spawn companions here", "", [p] { p->spawn_here(); }, true);
            act("All join (up to six)", "", [p] {
                for (size_t i = 0; i < p->defs.size(); ++i) p->join(int(i));
            });
            act("Revive dead companions", "", [p] {
                for (size_t i = 0; i < p->state.size(); ++i) p->revive(int(i), 100);
            });
            act("Send all companions home", "", [this, p] {
                const int n = p->send_all_home();
                say(n ? std::to_string(n) + (n == 1 ? " companion" : " companions") + " sent home" : "Nobody to send home");
            });
            act("Where they are...", "", [this] { if (DebugMenuGump* m = menu()) m->push(companions_page()); });
        }
        if (w.virtues) {  // the Avatar's virtue scores (game/virtues.h): see the octagon fill and glow
            hdr("Virtues");
            act("Virtue scores...", "", [this] { if (DebugMenuGump* m = menu()) m->push(virtues_page()); });
        }
        hdr("Time");
        tog("Pause world", "Ctrl-Z", "pause", [this] { return paused; });
        act("Step one frame", "Ctrl-S", [this] { toggle("step"); }, true);
        {
            DebugRow d;
            d.label = "World speed";
            d.shortcut = "Ctrl-D";
            d.value = [this] { return std::to_string(speed) + "x"; };
            d.act = [this](int dir) {
                if (dir < 0) speed = speed <= 1 ? 4 : speed / 2;
                else speed = speed >= 4 ? 1 : speed * 2;
            };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Time of day (+1 hour)";
            d.shortcut = "Ctrl-O";
            d.value = [this] {
                char b[32];
                unsigned h = hour();
                std::snprintf(b, sizeof b, "day %u %02u:00", h / 24 + 1, h % 24);
                return std::string(b);
            };
            d.act = [this](int dir) { advance_hours(dir < 0 ? -1 : 1); };
            d.number = [this](int n) { advance_hours(((n % 24) + 24) % 24 - int(hour() % 24)); };
            r.push_back(d);
        }
        hdr("State");
        act("Save state snapshot", "Ctrl-U", [this] { pending_ = "snapshot"; }, true);
        act("Load state snapshot", "Ctrl-Shift+U", [this] { pending_ = "load_snapshot"; }, true);
        {
            DebugRow d;
            d.label = "Debug mode";
            d.shortcut = "options page";
            d.value = [this] { return onoff(enabled()); };
            d.act = [this](int) { set_enabled(false); };
            d.closes = true;
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::virtues_page() {
    DebugPage pg;
    pg.title = "Virtues (Left down, Right up 5)";
    pg.build = [this]() {
        std::vector<DebugRow> r;
        Virtues* v = gm_.world().virtues.get();
        if (!v) return r;
        for (int k = 0; k < kVirtueCount; ++k) {
            DebugRow d;
            d.label = Virtues::name(k);
            d.value = [v, k] { return std::to_string(v->get(k)) + " of " + std::to_string(v->max_score); };
            d.act = [v, k](int dir) { v->set(k, v->get(k) + (dir < 0 ? -5 : 5)); };
            d.number = [v, k](int n) { v->set(k, n); };
            r.push_back(d);
        }
        auto all = [&](const std::string& label, std::function<int()> to) {
            DebugRow d;
            d.label = label;
            d.act = [v, to](int) { for (int k = 0; k < kVirtueCount; ++k) v->set(k, to()); };
            r.push_back(d);
        };
        all("All to 0", [] { return 0; });
        all("All to half", [v] { return v->max_score / 2; });
        all("All near the top (glowing)", [v] { return v->max_score * 9 / 10; });
        all("All to the maximum", [v] { return v->max_score; });
        if (!chapters.empty()) {
            DebugRow d;
            d.label = "Typical for this point of the story";
            d.value = [this] { int c = chapter_now(); return c < 0 ? std::string("?") : chapters[size_t(c)].id; };
            d.act = [this](int dir) { if (dir == 0) typical_virtues(chapter_now()); };
            r.push_back(d);
        }
        {
            // Infamy (the tyrant ending's bad acts; not a virtue): kept as "Vice/<key>/<n>".
            DebugRow d;
            d.label = "Infamy (Left down, Right up)";
            d.value = [v] { return std::to_string(v->infamy()) + (v->broken_promises() ? " +" + std::to_string(v->broken_promises()) + " broken promises" : ""); };
            d.act = [v](int dir) { if (dir) v->set_infamy(v->infamy() + dir); };
            d.number = [v](int n) { v->set_infamy(n); };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Clear infamy";
            d.act = [v](int dir) { if (dir == 0) v->clear_infamy(); };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Reset all (scores, deeds paid, infamy, marks)";
            d.act = [v](int dir) { if (dir == 0) v->reset(); };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Total";
            d.value = [v] { return std::to_string(v->total()) + " of " + std::to_string(v->total_max()) +
                                   (v->enabled ? "" : " (Virtues off)"); };
            r.push_back(d);
        }
        if (WorldStages* st = gm_.world().stages.get()) {
            // The world's stage (game/world_stages.h, PLAN-VIRTUES.md 5): Left/Right or a digit forces
            // one for testing (not saved); -1 / "auto" follows the scores again.
            DebugRow d;
            d.label = "World stage (Left/Right: force)";
            d.value = [st] {
                return std::to_string(st->stage()) + (st->forced >= 0 ? " (forced)" : " (auto: " + std::to_string(st->natural_stage()) + ")");
            };
            d.act = [st](int dir) {
                int f = st->forced + (dir < 0 ? -1 : 1);
                if (dir == 0) f = -1;
                st->forced = std::clamp(f, -1, WorldStages::kStages - 1);
            };
            d.number = [st](int n) { st->forced = std::clamp(n, -1, WorldStages::kStages - 1); };
            r.push_back(d);
            DebugRow g;
            g.label = "Greet the Avatar now";
            g.act = [st](int) { st->try_greet(true); };
            g.closes = true;
            r.push_back(g);
        }
        {
            DebugRow h;
            h.label = "Marks (Enter: set / clear)";
            h.header = true;
            r.push_back(h);
        }
        for (const std::string& mk : v->known_marks()) {
            DebugRow d;
            d.label = "  " + mk;
            d.value = [v, mk] { return onoff(v->marked(mk)); };
            d.act = [v, mk](int) { if (v->marked(mk)) v->clear_mark(mk); else v->set_mark(mk); };
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::levels_page() {
    DebugPage pg;
    pg.title = "Levels (Left down, Right up, digits set)";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        Leveling* lv = w.leveling.get();
        auto hdr = [&](const std::string& t) { DebugRow d; d.label = t; d.header = true; r.push_back(d); };
        if (!lv || !lv->enabled) {
            hdr("Leveling is Off: levels do nothing (Options, Game)");
            if (lv) {
                DebugRow d;
                d.label = "Turn Leveling on";
                d.act = [lv](int) {
                    lv->set_enabled(true);
                    Config& c = Config::global();
                    c.set_bool("leveling", true);
                    if (!c.path().empty()) c.save();
                };
                r.push_back(d);
            }
            return r;
        }
        if (!lv->started) hdr("Levels start with the next frame (an older game)");
        auto stats = [&w](uint16_t id) {
            const Actor* a = w.actor(id);
            if (!a) return std::string();
            char b[96];
            std::snprintf(b, sizeof b, "STR %d DEX %d INT %d  HP %d  mana %d", a->str, a->dex, a->intel, a->hp, a->mana);
            return std::string(b);
        };
        auto who = [&](uint16_t id, const std::string& name) {
            DebugRow d;
            d.label = name;
            d.value = [lv, id] {
                char b[64];
                std::snprintf(b, sizeof b, "level %d  XP %d", lv->level_of(id), lv->xp_of(id));
                return std::string(b);
            };
            d.act = [this, lv, id](int dir) {
                if (!dir) return;
                lv->set_level(id, lv->level_of(id) + dir, false);
                say(GameLog::name_of(gm_.world(), id) + ": level " + std::to_string(lv->level_of(id)));
            };
            d.number = [lv, id](int n) { lv->set_level(id, n, false); };
            r.push_back(d);
            DebugRow s;
            s.label = "  ";
            s.value = [stats, id] { return stats(id); };
            r.push_back(s);
        };
        hdr("The Avatar");
        who(1, "Avatar");
        {
            DebugRow d;
            d.label = "Unspent chosen points (Enter spreads)";
            d.value = [lv] { return std::to_string(lv->pending_points); };
            d.act = [lv](int dir) {
                if (dir) return;
                for (int k = 0; lv->pending_points > 0 && k < 64; ++k) lv->spend_point(k % 3);
            };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Rebuild his stats from his level";
            d.act = [lv](int dir) { if (dir == 0) lv->set_level(1, lv->level_of(1), true, lv->xp_of(1)); };
            r.push_back(d);
        }
        Party* p = w.party.get();
        if (p && p->enabled) {
            hdr("Companions (those spawned)");
            for (size_t i = 0; i < p->defs.size(); ++i) {
                const CompanionState& st = p->state[i];
                if (st.mode == CompanionState::kUnspawned || !w.actor(st.id)) continue;
                std::string name = p->defs[i].name;
                if (p->in_party(int(i))) name += " (party)";
                else if (!st.joined_before) name += " (not recruited)";
                who(st.id, name);
            }
            DebugRow d;
            d.label = "Party to the Avatar's level";
            d.act = [lv, p](int dir) {
                if (dir) return;
                for (int m : p->members) lv->set_level(p->state[size_t(m)].id, lv->level_of(1), false, lv->xp_of(1));
            };
            r.push_back(d);
            DebugRow b;
            b.label = "Rebuild the party's stats from their levels";
            b.act = [lv, p](int dir) {
                if (dir) return;
                for (int m : p->members) {
                    const uint16_t id = p->state[size_t(m)].id;
                    lv->set_level(id, lv->level_of(id), true, lv->xp_of(id));
                }
            };
            r.push_back(b);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::story_page() {
    DebugPage pg;
    pg.title = "Jump to story chapter (Enter jumps)";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        auto hdr = [&](const std::string& t) { DebugRow d; d.label = t; d.header = true; r.push_back(d); };
        if (chapters.empty()) { hdr("No story data (data/story_jump.json)"); return r; }
        const bool have = have_checkpoints();
        if (!have) hdr("No story checkpoints in this build: only a new game");
        {
            DebugRow d;
            d.label = "Virtues after the jump";
            d.value = [this] {
                return std::string(jump_virtues == kKeepVirtues ? "keep mine" : jump_virtues == kTypicalVirtues ? "typical then" : "the replay's");
            };
            d.act = [this](int dir) { jump_virtues = JumpVirtues((int(jump_virtues) + (dir < 0 ? 2 : 1)) % 3); };
            r.push_back(d);
        }
        if (w.party && w.party->enabled) {
            DebugRow d;
            d.label = "My companions come along";
            d.value = [this] { return onoff(jump_companions); };
            d.act = [this](int) { jump_companions = !jump_companions; };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Leveling (the option)";
            Leveling* lv = w.leveling.get();
            d.value = [lv] { return onoff(lv && lv->enabled); };
            d.act = [lv](int) {
                if (!lv) return;
                lv->set_enabled(!lv->enabled);
                Config& c = Config::global();
                c.set_bool("leveling", lv->enabled);
                if (!c.path().empty()) c.save();
            };
            r.push_back(d);
        }
        hdr("Chapters: the whole world as it was then");
        const int now = chapter_now();
        for (size_t i = 0; i < chapters.size(); ++i) {
            const StoryChapter& c = chapters[i];
            const bool usable = c.checkpoint.empty() || have;
            DebugRow d;
            char b[96];
            std::snprintf(b, sizeof b, "%2zu %s", i + 1, c.title.c_str());
            d.label = b;
            const int lvl = c.level;
            const bool here = int(i) == now;  // (the font has no arrow: the value says it)
            d.value = [usable, lvl, here] {
                return !usable ? std::string("(missing)") : (here ? "now, level " : "level ") + std::to_string(lvl);
            };
            const int idx = int(i);
            if (usable) {
                d.act = [this, idx](int dir) { if (dir == 0) { pending_jump_ = idx; close_menu(); } };
                d.closes = true;
            }
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::steps_page() {
    DebugPage pg;
    pg.title = "Critical path (Enter: jump to, Right: mark done)";
    pg.build = [this]() {
        std::vector<DebugRow> r;
        if (crit.steps.empty()) {
            DebugRow d;
            d.label = "No critical path data loaded";
            d.header = true;
            r.push_back(d);
            return r;
        }
        std::vector<bool> d = crit.done(ctx);
        int cur = crit.current(d);
        {
            DebugRow c;
            c.label = "Clear debug overrides";
            c.value = [this] { return std::to_string(crit.forced_done.size() + (crit.jump_to >= 0 ? 1 : 0)); };
            c.act = [this](int dir) { if (dir == 0) { crit.forced_done.clear(); crit.jump_to = -1; eval_timer_ = 0; } };
            r.push_back(c);
        }
        for (size_t i = 0; i < crit.steps.size(); ++i) {
            const CritStep& st = crit.steps[i];
            DebugRow row;
            std::string mark = int(i) == cur ? "> " : d[i] ? "x " : "  ";
            row.label = mark + (st.act.empty() ? "" : st.act + " ") + st.title;
            if (row.label.size() > 44) row.label = row.label.substr(0, 42) + "..";
            bool forced = crit.forced_done.count(int(i)) || (crit.jump_to >= 0 && int(i) < crit.jump_to);
            bool nat = st.done_when.eval(ctx);
            row.value = [nat, forced, i, cur] {
                return int(i) == cur ? std::string("CURRENT") : nat ? std::string("done") : forced ? std::string("forced") : std::string("todo");
            };
            int idx = int(i);
            row.act = [this, idx](int dir) {
                if (dir == 0) {
                    crit.jump_to = idx;
                    for (auto it = crit.forced_done.begin(); it != crit.forced_done.end();)
                        it = *it >= idx ? crit.forced_done.erase(it) : std::next(it);
                    say("Jumped to step " + crit.steps[size_t(idx)].id);
                } else if (crit.forced_done.count(idx)) {
                    crit.forced_done.erase(idx);
                } else {
                    crit.forced_done.insert(idx);
                }
                eval_timer_ = 0;
                guide = crit.guide(ctx);
            };
            r.push_back(row);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::maps_page() {
    DebugPage pg;
    pg.title = "Warp to map (Enter: entry point, Right: pick in atlas)";
    pg.build = [this]() {
        std::vector<DebugRow> r;
        static const unsigned maps[] = {3, 4, 5, 6, 7, 8, 12, 14, 15, 16, 21, 24, 25, 26, 28, 31, 37, 39, 40,
                                        41, 43, 44, 45, 46, 47, 49, 50, 51, 54, 57, 62, 63};
        for (unsigned m : maps) {
            DebugRow d;
            char b[96];
            std::snprintf(b, sizeof b, "%3u  %s", m, map_name(m).c_str());
            d.label = b;
            d.value = [] { return std::string(""); };
            d.act = [this, m](int dir) {
                World& w = gm_.world();
                if (dir > 0) {
                    close_menu();
                    toggle("warp_atlas");
                    MapFootprint fp = compute_footprint(w.data(), m);
                    if (gm_.atlas_ && fp.chunks > 0)
                        gm_.atlas_->show(m, (fp.cx0 + fp.cx1 + 1) * 256, (fp.cy0 + fp.cy1 + 1) * 256, fp.median_z, 2);
                    return;
                }
                if (dir < 0) return;
                // An entry point: the destination of a teleporter egg into the map, else its middle.
                for (const TeleportLink& l : teleport_links(w.data()))
                    if (l.to_map == m) { warp_to(m, l.tx, l.ty, l.tz); close_menu(); return; }
                MapFootprint fp = compute_footprint(w.data(), m);
                warp_to(m, (fp.cx0 + fp.cx1 + 1) * 256, (fp.cy0 + fp.cy1 + 1) * 256, fp.median_z);
                close_menu();
            };
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::npcs_page() {
    DebugPage pg;
    pg.title = "Teleport to NPC";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        for (unsigned n = 2; n < 256; ++n) {
            const Actor* a = w.actor(uint16_t(n));
            if (!a) continue;
            unsigned m;
            int x, y, z;
            if (!ctx.npc_location(n, m, x, y, z)) continue;
            DebugRow d;
            char b[96];
            std::snprintf(b, sizeof b, "%3u  %s%s", n, a->name.c_str(), a->is(Actor::kDead) ? " (dead)" : "");
            d.label = b;
            d.shortcut = "map " + std::to_string(m);
            d.act = [this, n](int) { teleport_to_npc(n); };
            d.closes = true;
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

// Where each companion is (NOTES.md "Companions: homes"); map markers would spoil finding them.
DebugPage DebugTools::companions_page() {
    DebugPage pg;
    pg.title = "Where the companions are";
    pg.build = [this]() {
        std::vector<DebugRow> r;
        Party* p = gm_.world().party.get();
        if (!p) return r;
        for (size_t i = 0; i < p->defs.size(); ++i) {
            DebugRow d;
            d.label = p->defs[i].name;
            const std::string where = p->where_text(int(i));
            d.value = [where] { return where; };
            r.push_back(d);
            DebugRow h;
            h.label = "  home: " + (p->defs[i].home_place.empty() ? std::string("the beach") : p->defs[i].home_place);
            r.push_back(h);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::items_page() {
    DebugPage pg;
    pg.title = "Give item";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        auto num = [&](const std::string& label, int* v, int lo, int hi) {
            DebugRow d;
            d.label = label;
            d.value = [v] { return std::to_string(*v); };
            d.act = [v, lo, hi](int dir) { if (dir) *v = std::clamp(*v + dir, lo, hi); };
            d.number = [v, lo, hi](int n) { *v = std::clamp(n, lo, hi); };
            r.push_back(d);
        };
        auto header = [&](const std::string& label) {
            DebugRow h;
            h.label = label;
            h.header = true;
            r.push_back(h);
        };
        // One click (or Enter / controller A) on a row gives it at once: one item, or a stack of
        // kCoinsPerClick for money (the quantity family: obsidian coins, the only one in U8).
        auto item = [&](const GiveItem& gi) {
            DebugRow d;
            d.label = gi.label;
            d.shortcut = std::to_string(gi.shape) + ":" + std::to_string(gi.frame);
            if (gi.qty > 1) d.shortcut += "  x" + std::to_string(gi.qty);
            GiveItem g = gi;
            d.act = [this, g](int dir) { if (dir == 0) give(g.shape, g.frame, g.qty, g.quality, g.label); };
            r.push_back(d);
        };
        static const struct Section { const char* title; std::vector<GiveItem> items; } kSections[] = {
            // Money: specs/world/items.md (family 2: shape 143 only), combat.md 5 (obsidian).
            {"Money", {{143, 0, kCoinsPerClick, "Obsidian coins"}}},
            // Weapons: combat.md 4 (the equippable shapes, equipment type 6).
            {"Weapons", {{417, 0, 1, "Mace"}, {418, 0, 1, "Dagger"}, {419, 0, 1, "Axe"}, {420, 0, 1, "Sword"},
                         {540, 0, 1, "Hammer"}, {541, 0, 1, "Scimitar"}, {542, 0, 1, "Sabre"},
                         {815, 0, 1, "Bone Crusher (magic)"}, {816, 0, 1, "Slayer (magic)"},
                         {817, 0, 1, "Flame Sting (magic)"}, {818, 0, 1, "Protector (magic)"},
                         {819, 0, 1, "Striking (magic)"}, {820, 0, 1, "Korghin (magic)"},
                         {821, 0, 1, "Khumash-Gor (magic)"}, {822, 0, 1, "Deceiver (magic)"}}},
            // Armour: combat.md 4.3 (shape, frame, armour).
            {"Armour", {{531, 0, 1, "Helmet (4)"}, {531, 6, 1, "Helmet (3)"}, {523, 0, 1, "Body armour (4)"},
                        {523, 2, 1, "Body armour (3)"}, {530, 6, 1, "Arm guard (3)"}, {530, 0, 1, "Arm guard (2)"},
                        {532, 2, 1, "Leggings (3)"}, {532, 0, 1, "Leggings (2)"}, {539, 0, 1, "Shield (4)"},
                        {539, 2, 1, "Shield (3)"}, {841, 0, 1, "Magic armour"}, {842, 0, 1, "Magic shield"},
                        {843, 0, 1, "Magic helm"}, {844, 0, 1, "Magic arm guards"}, {845, 0, 1, "Magic leggings"}}},
            // Necromancy reagents (shape 395 frame groups, bugfixes.md / items.md "Stacks").
            {"Necromancy reagents", {{395, 5, 1, "Blood"}, {395, 6, 1, "Bone"}, {395, 8, 1, "Wood"},
                                     {395, 9, 1, "Dirt"}, {395, 10, 1, "Executioner's hood"},
                                     {395, 14, 1, "Blackmoor"}, {395, 20, 1, "Dead man's elbow"}}},
            // Sorcery reagents and foci (magic.md 3.1).
            {"Sorcery", {{398, 0, 1, "Volcanic ash"}, {398, 2, 1, "Pumice"}, {398, 6, 1, "Obsidian (reagent)"},
                         {398, 10, 1, "Iron"}, {398, 14, 1, "Brimstone"}, {398, 18, 1, "Daemon bone"},
                         {397, 15, 1, "Focus: wand (blank)"}, {397, 16, 1, "Focus: rod (blank)"},
                         {397, 17, 1, "Focus: staff (blank)"}, {397, 18, 1, "Focus: talisman (blank)"},
                         {397, 19, 1, "Focus: symbol (blank)"}}},
            // Other magic and story items (critical_path.md 7).
            {"Magic and story items", {{833, 0, 1, "Recall item"}, {288, 0, 1, "Ethereal Travel book", 0x56},
                                       {827, 0, 1, "Dispel Portals scroll", 1}, {746, 4, 1, "Open Ground spell"},
                                       {746, 5, 1, "Create Golem spell"}, {396, 0, 1, "Theurgy focus"}}},
            {"Keys", {{79, 0, 1, "Keyring"}, {232, 0, 1, "Key of the Caretaker"}, {232, 3, 1, "Key of the Scion"},
                      {232, 4, 1, "Key of the Necromancer"}, {82, 0, 1, "Key to Mordea's chest", 48}}},
            // Potions (shape 766 frames 0-7; magic.md 8 lists the colours), food, the rest.
            {"Potions and food", {{766, 0, 1, "Potion 0"}, {766, 1, 1, "Potion 1"}, {766, 2, 1, "Potion 2"},
                                  {766, 3, 1, "Potion 3"}, {766, 4, 1, "Potion 4"}, {766, 5, 1, "Potion 5"},
                                  {766, 6, 1, "Potion 6"}, {766, 7, 1, "Potion 7"}, {537, 2, 1, "Meat"},
                                  {537, 0, 1, "Food"}, {538, 0, 1, "More food"}}},
            {"Gear", {{534, 0, 1, "Bedroll"}, {568, 0, 1, "Torch"}, {533, 0, 1, "Rope"}, {637, 0, 1, "Bag"},
                      {592, 0, 1, "Firegem"}, {579, 0, 1, "Oil flask"}}},
        };
        for (const Section& sec : kSections) {
            header(sec.title);
            for (const GiveItem& gi : sec.items) item(gi);
        }
        // Any shape by number (the usecode class name is shown).
        header("Any shape");
        num("Shape (type digits)", &give_shape, 1, int(w.data().shapes.count()) - 1);
        r.back().shortcut = w.data().usecode.class_name(unsigned(give_shape));
        num("Frame", &give_frame, 0, 255);
        num("Count (stacks)", &give_qty, 1, int(kMaxStack));
        {
            DebugRow d;
            d.label = "Give it";
            d.act = [this](int dir) { if (dir == 0) give(unsigned(give_shape), unsigned(give_frame), give_qty); };
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

DebugPage DebugTools::globals_page() {
    DebugPage pg;
    pg.title = "Global flags (usecode G[offset:width])";
    pg.build = [this]() {
        World& w = gm_.world();
        std::vector<DebugRow> r;
        auto num = [&](const std::string& label, unsigned* v, unsigned lo, unsigned hi) {
            DebugRow d;
            d.label = label;
            d.value = [v] { return std::to_string(*v); };
            d.act = [v, lo, hi](int dir) { if (dir) *v = unsigned(std::clamp(int(*v) + dir, int(lo), int(hi))); };
            d.number = [v, lo, hi](int n) { *v = unsigned(std::clamp(n, int(lo), int(hi))); };
            r.push_back(d);
        };
        num("Offset (bit position)", &global_pos, 0, 4095);
        num("Width (bits)", &global_width, 1, 16);
        {
            DebugRow d;
            d.label = "Value (Left/Right or digits + Enter sets)";
            d.value = [this, &w] { return std::to_string(w.vm().get_global(global_pos, global_width)); };
            d.act = [this, &w](int dir) {
                if (!dir) return;
                int v = int(w.vm().get_global(global_pos, global_width)) + dir;
                w.vm().set_global(global_pos, global_width, uint16_t(std::max(0, v)));
            };
            d.number = [this, &w](int n) { w.vm().set_global(global_pos, global_width, uint16_t(std::max(0, n))); };
            r.push_back(d);
        }
        {
            DebugRow d;
            d.label = "Print to the log";
            d.act = [this, &w](int dir) {
                if (dir) return;
                char b[80];
                std::snprintf(b, sizeof b, "G[%u:%u] = %u", global_pos, global_width, w.vm().get_global(global_pos, global_width));
                say(b);
            };
            r.push_back(d);
        }
        DebugRow h;
        h.label = "Flags the critical path reads (Enter selects)";
        h.header = true;
        r.push_back(h);
        std::set<std::pair<int, int>> seen;
        std::function<void(const CritCond&)> walk = [&](const CritCond& c) {
            if (c.kind == CritCond::kGlobal) seen.insert({c.a, std::max(1, c.b)});
            for (const CritCond& s : c.sub) walk(s);
        };
        for (const CritStep& s : crit.steps) walk(s.done_when);
        for (const MapLink& l : crit.links) walk(l.condition);
        for (const auto& g : seen) {
            DebugRow d;
            char b[48];
            std::snprintf(b, sizeof b, "G[%d:%d]", g.first, g.second);
            d.label = b;
            unsigned pos = unsigned(g.first), wd = unsigned(g.second);
            d.value = [&w, pos, wd] { return std::to_string(w.vm().get_global(pos, wd)); };
            d.act = [this, pos, wd](int dir) {
                if (dir == 0) { global_pos = pos; global_width = wd; if (DebugMenuGump* m = menu()) m->select("Value"); }
            };
            r.push_back(d);
        }
        return r;
    };
    return pg;
}

}  // namespace pg
