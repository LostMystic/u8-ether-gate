// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The game world: new game, map changes, item moves, containers, the ethereal void,
// searches, placement tests, eggs and item animation (specs/world/*.md).
#include "data/compat.h"
#include "game/world.h"

#include "core/config.h"
#include "core/bugfix.h"

#include <algorithm>
#include <cstdlib>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/avatar_mover.h"
#include "game/combat.h"
#include "game/npc_dialog.h"
#include "game/creatures.h"
#include "game/virtues.h"
#include "game/spellbook.h"
#include "game/rock_flesh.h"
#include "game/world_stages.h"
#include "game/leveling.h"
#include "game/stat_growth.h"
#include "game/party.h"
#include "game/turn_combat.h"
#include "game/variants.h"
#include "world/map_patches.h"
#include "game/processes.h"
#include "game/item_powers.h"
#include "world/world_space.h"

namespace pg {

namespace {
bool xy_overlap(int ax, int ay, int axd, int ayd, int bx, int by, int bxd, int byd) {
    return ax - axd < bx && ax > bx - bxd && ay - ayd < by && ay > by - byd;
}
}  // namespace

World::World(const GameData& data) : data_(data), vm_(data.usecode, kernel_), map_(data, objs_) {
    map_.set_hooks(this);
    vm_.set_host(this);
    for (unsigned i = 0; i < data.fonts.count(); ++i) fonts_.emplace_back(data.fonts.shape(i), int(i));
    register_intrinsics(*this);
    party = std::make_unique<Party>(*this);
    leveling = std::make_unique<Leveling>(*this);
    creatures = std::make_unique<Creatures>(*this);
    virtues = std::make_unique<Virtues>(*this);
    spellbook = std::make_unique<Spellbook>(*this);
    stages = std::make_unique<WorldStages>(*this);
    dialog = std::make_unique<NpcDialog>(*this);
    turn = std::make_unique<TurnCombat>(*this);
}

World::~World() {
    kernel_.reset();  // processes refer to the world
}

bool World::same_party(uint16_t a, uint16_t b) const { return party && party->enabled && party->same_side(a, b); }
bool World::party_ghost(uint16_t mover, uint16_t other) const {
    return party && party->enabled && party->passes_through(mover, other);
}

const ShapeFont& World::font(int n) const {
    static ShapeFont none;
    return n >= 0 && size_t(n) < fonts_.size() ? fonts_[size_t(n)] : none;
}

int World::rand_below(int n) {
    if (n <= 1) return 0;
    return int(rng() % unsigned(n));
}

// ---------------------------------------------------------------------------------------------
// New game

std::unique_ptr<Actor> World::make_npc(const NpcRecord& n) const {
    auto a = std::make_unique<Actor>();
    a->shape = n.shape;
    a->frame = n.frame;
    a->x = n.x;
    a->y = n.y;
    a->z = n.z;
    a->flags = uint16_t((n.flags & ~kItemFastArea) | kItemInNpcList);
    a->quality = n.quality;
    a->npcnum = n.npcnum;
    a->mapnum = n.mapnum;
    a->origin = Item::kNpc;
    a->ext_flags |= kExtPermanentNpc;
    a->str = n.str;
    a->dex = n.dex;
    a->intel = n.intel;
    a->hp = n.hp;
    a->mana = n.mana;
    a->dir = uint8_t(n.dir & 7);
    a->last_anim = n.last_anim;
    a->last_anim_dir = a->dir;
    a->alignment = n.alignment & 0x0F;
    a->enemy_alignment = n.alignment >> 4;
    a->unk0c = n.unk0c;
    a->actor_flags = n.actor_flags;
    a->name = data_.usecode.class_name(1024 + n.number);
    return a;
}

void World::create_npcs() {
    for (const NpcRecord& n : data_.npcs) objs_.add_at(uint16_t(n.number), make_npc(n));
}

void World::new_game() {
    if (turn) turn->world_replaced();
    kernel_.reset();
    vm_.reset();
    objs_.clear();
    ether_.clear();
    barks.clear();
    sky_bolts.clear();
    sky_flash = 0;
    ask_menu.reset();
    if (dialog) dialog->reset();
    talk_pid = talk_item = stasis_pid = stasis_cls = 0;
    gumps.list.clear();
    request = Request{};
    red_cross_frames = 0;
    target_pid = 0;
    camera_pid_ = 0;
    time_offset_ = 0;
    stasis = false;
    quake = 0;
    item_powers::new_game(*this);
    fast_init_ = false;
    palette = palette_target = PaletteTransform{};
    startup_state = 0;
    stat_accumulator = {0, 0, 0};
    last_attack_frame = 0;
    game_over = false;
    death_pid = 0;
    input = PlayerInput{};

    // Per-map nonfixed items (map 0 holds NPC inventories; startup.md uses an empty backpack).
    nonfixed_.assign(256, {});
    for (unsigned m = 1; m < 256 && m < data_.nonfixed.count(); ++m) {
        std::vector<ItemRecord> records = parse_item_records(data_.nonfixed.entry(m));
        map_patches::apply_moves(m, records);
        nonfixed_[m] = build_item_tree(records, data_.typeflags);
    }

    create_npcs();
    if (Actor* av = avatar()) {
        av->name = avatar_name;
        uint16_t pack = create_item(529, 0);
        if (pack) {
            Item* p = item(pack);
            p->parent = 1;
            p->flags |= kItemEquipped | kItemOwned;
            p->z = 7;
            av->contents.push_back(pack);
        }
    }
    unsigned map = avatar() ? avatar()->mapnum : 3;
    visited_maps = {map};
    load_map_items(map, false);
    stasis = true;

    camera_follow(1);
    kernel_.add(std::make_unique<HealProcess>(*this));
    kernel_.add(std::make_unique<SchedulerProcess>(*this));
    kernel_.add(std::make_unique<StartupProcess>(*this));
    mover_pid = kernel_.add(std::make_unique<AvatarMoverProcess>(*this));
    if (party) party->reset();  // companions spawn once the opening releases the Avatar
    if (creatures) creatures->reset();
    if (virtues) virtues->reset();  // every virtue at 0
    if (spellbook) spellbook->reset();  // an empty spellbook
    stone_skin.clear();
    if (stages) stages->reset();
    if (leveling) leveling->new_game();  // the Avatar's starting stats, when Leveling is on
}

void World::load_map_items(unsigned map, bool cachein) {
    map_.reset(map);
    std::vector<ItemRecord> fixed = parse_item_records(data_.fixed.entry(map));
    apply_map_fixes(map, fixed);
    std::vector<uint16_t> fixed_ids = map_.create_tree(build_item_tree(fixed, data_.typeflags), Item::kFixed);
    std::vector<uint16_t> nonfixed_ids;
    if (map < nonfixed_.size()) nonfixed_ids = map_.create_tree(nonfixed_[map], Item::kNonFixed);
    if (map < nonfixed_.size()) nonfixed_[map].clear();
    underworld::sync_entrance(*this);  // (the items saved for this map may be from before a change of the option)
    kernel_.add(std::make_unique<EggHatcherProcess>(*this));
    if (cachein) {
        for (uint16_t id : fixed_ids) call_event(id, kEvCachein);
        for (uint16_t id : nonfixed_ids) call_event(id, kEvCachein);
    }
    for (unsigned n = 1; n < 256; ++n) {
        Actor* a = actor(uint16_t(n));
        if (!a) continue;
        if (cachein && !a->is(Actor::kDead)) {
            uint32_t minutes = game_seconds() / 60;
            call_event(uint16_t(n), kEvSchedule,
                       {uint8_t(minutes), uint8_t(minutes >> 8), uint8_t(minutes >> 16), uint8_t(minutes >> 24)});
        }
        if (a->mapnum == map && a->parent == 0 && !(a->flags & kItemEthereal)) map_.link(uint16_t(n));
    }
    fast_init_ = false;
}

void World::save_map_items() {
    unsigned old = map_.map_number();
    std::vector<MapEntry> store;
    // Walk the chunk lists (copy: we delete as we go).
    std::vector<uint16_t> tops;
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx)
            for (uint16_t id : map_.chunk(cx, cy)) tops.push_back(id);
    std::function<void(uint16_t, int, int)> save = [&](uint16_t id, int depth, int parent_index) {
        Item* it = item(id);
        if (!it) return;
        MapEntry e;
        e.rec.x = uint16_t(depth ? depth : it->x);
        e.rec.y = uint16_t(it->y);
        e.rec.z = uint8_t(depth ? 0 : it->z);
        e.rec.shape = it->shape;
        e.rec.frame = uint8_t(it->frame);
        e.rec.flags = uint16_t(it->flags & ~kItemFastArea);
        e.rec.quality = it->quality;
        e.rec.npcnum = it->npcnum;
        e.rec.mapnum = it->mapnum;
        e.parent = parent_index;
        int me = int(store.size());
        store.push_back(e);
        for (uint16_t c : it->contents) save(c, depth + 1, me);
    };
    for (uint16_t id : tops) {
        Item* it = item(id);
        if (!it) continue;
        if (id < 256 || (it->flags & kItemInNpcList)) { map_.unlink(id); continue; }
        if (!(it->flags & (kItemDisposable | kItemFastOnly)) && !(it->ext_flags & kExtFromFixed)) save(id, 0, -1);
        map_.unlink(id);
        destroy_contents(id);
        kernel_.kill(id, kProcAny);
        objs_.erase(id);
    }
    if (old < nonfixed_.size()) nonfixed_[old] = std::move(store);
}

bool World::change_map(unsigned m) {
    if (m == map_.map_number() || m >= 256) return true;
    if (data_.fixed.entry(m).empty() && (m >= nonfixed_.size() || nonfixed_[m].empty())) return false;
    PG_LOG("map change %u -> %u", map_.map_number(), m);
    visited_maps.insert(m);
    sound.stop_all(true);  // items.md 9 step 1: everything but speech
    barks.clear();
    gumps.close_item_dependent();  // container windows close on a map change (gumps.md 1.4)
    // Destroy everything in the ethereal void.
    while (!ether_.empty()) {
        uint16_t id = ether_.back();
        ether_.pop_back();
        if (Item* it = item(id)) { it->flags &= uint16_t(~kItemEthereal); if (id >= 256) destroy_now(id); }
    }
    if (party) party->before_map_change(m);  // followers travel along (placed beside him next frame)
    save_map_items();
    kernel_.kill_for_map_change();
    // Death Speak's "speaking with the dead" flag G[40:1] is cleared by the spell's own process
    // when the Necromancer's answer ends; a map change kills that process first, which left the
    // flag set for good (the later Ancient Necromancers talked without the spell; NOTES.md
    // "Original bug fixes"). Nothing else sets it, so with that process gone it must be clear.
    if (original_bug_fixes() && compat::class_known(1414) && compat::class_known(623)) vm_.set_global(40, 1, 0);
    for (uint16_t pid = 1; pid < 32767; ++pid)  // replace the egg hatcher
        if (auto* p = dynamic_cast<EggHatcherProcess*>(kernel_.get(pid))) p->fail();
    load_map_items(m, true);
    if (camera_followed() != 1) camera_follow(1);
    quake = 0;
    return true;
}

bool World::teleport_to_egg(unsigned m, unsigned egg_id) {
    Actor* av = avatar();
    if (!av) return false;
    unsigned old_map = map_.map_number();
    int ox = av->x, oy = av->y, oz = av->z;
    av->mapnum = uint8_t(m);
    if (!change_map(m)) { av->mapnum = uint8_t(old_map); return false; }
    const Item* dest = nullptr;
    for (size_t i = 512; i < objs_.slots() && !dest; ++i) {
        const Item* it = objs_.get(uint16_t(i));
        if (it && it->chunk >= 0 && data_.typeflags.get(it->shape).family() == kFamTeleportEgg && it->frame == 1 &&
            (it->quality & 0xFF) == egg_id)
            dest = it;
    }
    if (!dest) {
        PG_LOG("teleport: no destination egg %u on map %u", egg_id, m);
        av->mapnum = uint8_t(old_map);
        change_map(old_map);
        move_to(1, ox, oy, oz);
        return false;
    }
    push_ether(1);
    ether_pop();
    move_to(1, dest->x, dest->y, dest->z);
    just_teleported = true;
    return true;
}

void World::teleport_actor(uint16_t id, int x, int y, int z, unsigned m) {
    Actor* a = actor(id);
    if (!a) return;
    if (id == 1 && m != map_.map_number()) {
        a->mapnum = uint8_t(m);
        if (!change_map(m)) return;
    }
    a->mapnum = uint8_t(m);
    if (m == map_.map_number()) {
        move_to(id, x, y, z);
    } else {
        detach(id);
        a->x = x;
        a->y = y;
        a->z = z;
    }
}

// ---------------------------------------------------------------------------------------------
// Frames

void World::frame() {
    if (party) party->pre_frame();
    if (turn) turn->pre_frame();  // turn-based combat: starting a fight, the freeze
    kernel_.frame();
    if (stitch) stitch_frame();
    void_rescue_frame();  // Original bug fixes: out of the world and back (game/void_rescue.cpp)
    invasion::frame(*this);  // Restored content: the undead invasion of Tenebrae (game/variants.h)
    item_powers::frame(*this);  // Item powers: the Skull of Quakes (game/item_powers.h)
    if (party) party->frame();
    if (leveling) leveling->frame();
    if (creatures) creatures->frame();  // the new creatures (game/creatures.h)
    rock_flesh::frame(*this);  // Rock Flesh effects running out (game/rock_flesh.h)
    if (spellbook) spellbook->frame();  // spells entered in the book, clean-up after its casts (game/spellbook.h)
    if (virtues) virtues->frame();  // the virtue deeds the story flags tell (game/virtues.h)
    if (stages) stages->frame();  // townsfolk greetings by deed (game/world_stages.h)
    if (dialog) dialog->frame();
    if (turn) turn->frame();
    int cam[3];
    camera_point(cam);
    if (!fast_init_) {
        std::copy(cam, cam + 3, prev_cam_);
        fast_init_ = true;
    }
    map_.update_fast_area(prev_cam_, cam, view_w, view_h);
    if (controls.responsive && controls.assist) free_actors_from_doors();
    std::copy(cam, cam + 3, prev_cam_);
    if (!kernel_.paused()) animate_items();
    update_ui();
    gumps.tick();
    sound.frame();
}

bool World::is_door_shape(unsigned shape) const {
    if (door_shapes_.empty()) door_shapes_.assign(2048, -1);
    if (shape >= door_shapes_.size()) return false;
    int8_t& v = door_shapes_[shape];
    if (v < 0) v = data_.usecode.class_name(shape).find("DOOR") != std::string::npos ? 1 : 0;
    return v == 1;
}

// Milestone 6: an actor standing in a door that closed on it is put on the nearest free,
// supported spot (rings of 8 units out to 128), so nothing is ever trapped in a door.
void World::free_actors_from_doors() {
    for (unsigned n = 1; n < 256; ++n) {
        Actor* a = actor(uint16_t(n));
        if (!a || a->chunk < 0 || !(a->flags & kItemFastArea) || a->is(Actor::kDead)) continue;
        Box b = box_of(*a);
        Placement p = can_exist(b.x, b.y, b.z, b.xd, b.yd, b.zd, shape_flags(*a), a->id);
        const Item* d = p.valid ? nullptr : item(p.blocker);
        if (!d || !is_door_shape(d->shape) || door_is_open(*d)) continue;
        bool done = false;
        for (int r = 8; r <= 128 && !done; r += 8)
            for (int k = 0; k < 16 && !done; ++k) {
                static const int ux[16] = {0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2, -2, -2, -2, -1};
                static const int uy[16] = {-2, -2, -2, -1, 0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2};
                int x = b.x + ux[k] * r / 2, y = b.y + uy[k] * r / 2;
                Placement q = can_exist(x, y, b.z, b.xd, b.yd, b.zd, shape_flags(*a), a->id);
                if (q.valid && q.supported) {
                    PG_LOG("door %u closed on actor %u: moved from %d,%d to %d,%d", d->id, n, b.x, b.y, x, y);
                    move_to(a->id, x, y, b.z);
                    done = true;
                }
            }
    }
}

void World::load_options() {
    const Config& c = Config::global();
    options.music = c.get_bool("music", options.music);
    options.sfx = c.get_bool("sfx", options.sfx);
    options.steps = c.get_bool("steps", options.steps);
    options.animations = c.get_bool("animations", options.animations);
    options.text_speed = std::clamp(c.get_int("text_speed", options.text_speed), 0, 9);
    options.frame_skip = c.get_bool("frame_skip", options.frame_skip);
    options.speed_limit = c.get_bool("speed_limit", options.speed_limit);
    options.music_volume = std::clamp(c.get_int("music_volume", options.music_volume), 0, 255);
    options.sfx_volume = std::clamp(c.get_int("sfx_volume", options.sfx_volume), 0, 255);
    // Milestone 6 control settings: the player's defaults are Responsive and smooth.
    controls.responsive = c.get("controls", "responsive") != "classic";
    controls.smooth_scroll = c.get_bool("smooth_scroll", true);
    controls.click_text = c.get_bool("click_text", true);
    controls.zoom_step = std::clamp(c.get_int("zoom_step", 0), 0, 6);
    controls.assist = c.get_bool("movement_assist", true);
    controls.easy_ledges = c.get_bool("easy_ledges", true);
    original_bug_fixes() = c.get_bool("bug_fixes", true);
    apply_variant_options(*this);  // v2.12 changes, the undead invasion (game/variants.h)
    item_powers::apply_options();
    sound.apply_options();
}

void World::register_bugfix_option() {
    static bool done = false;
    if (done) return;
    done = true;
    World::extra_option_rows().push_back([](World&) {
        MenuGump::OptionRow r{"Original bug fixes:", [] { return std::string(original_bug_fixes() ? "On" : "Off"); },
                              [](int) {
                                  original_bug_fixes() = !original_bug_fixes();
                                  Config& c = Config::global();
                                  c.set_bool("bug_fixes", original_bug_fixes());
                                  if (!c.path().empty()) c.save();
                              }};
        r.group = "Game";
        return r;
    });
}

void World::store_options() {
    Config& c = Config::global();
    c.set_bool("music", options.music);
    c.set_bool("sfx", options.sfx);
    c.set_bool("steps", options.steps);
    c.set_bool("animations", options.animations);
    c.set_int("text_speed", options.text_speed);
    c.set_bool("frame_skip", options.frame_skip);
    c.set_bool("speed_limit", options.speed_limit);
    c.set_int("music_volume", options.music_volume);
    c.set_int("sfx_volume", options.sfx_volume);
    c.set("controls", controls.responsive ? "responsive" : "classic");
    c.set_bool("smooth_scroll", controls.smooth_scroll);
    c.set_bool("click_text", controls.click_text);
    c.set_int("zoom_step", controls.zoom_step);
    c.set_bool("movement_assist", controls.assist);
    c.set_bool("easy_ledges", controls.easy_ledges);
    if (!c.path().empty()) c.save();
    sound.apply_options();
}

void World::animate_items() {
    uint32_t fn = kernel_.frame_number();
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            if (!map_.is_fast(cx, cy)) continue;
            std::vector<uint16_t> ids = map_.chunk(cx, cy);
            for (uint16_t id : ids) {
                Item* it = item(id);
                if (!it || id < 256) continue;
                const ShapeInfo& si = data_.typeflags.get(it->shape);
                unsigned type = si.anim_type();
                if (!type) continue;
                unsigned speed = si.anim_speed();
                if (speed == 0) speed = 1;
                if (fn % speed != id % speed) continue;  // the original rule (items.md 4)
                const Shape* sh = data_.shapes.shape(it->shape);
                unsigned count = sh ? sh->frame_count() : 0;
                if (!count) continue;
                unsigned data = si.anim_data();
                auto advance = [&](bool resting_zero) {
                    unsigned f = it->frame;
                    if (data >= 2) {
                        unsigned base = f - f % data, n = f + 1;
                        if (resting_zero && f % data == 0) return;
                        if (n >= base + data || n >= count) n = resting_zero ? base + 1 : base;
                        it->frame = uint16_t(n);
                    } else {
                        if (resting_zero && f == 0) return;
                        unsigned n = f + 1;
                        if (n >= count) n = resting_zero ? 1 : 0;
                        it->frame = uint16_t(n);
                    }
                };
                switch (type) {
                case 1: case 3:
                    if (data == 1 && rand_below(2)) break;
                    advance(false);
                    break;
                case 2:
                    if (rand_below(2)) it->frame = uint16_t(rand_below(int(count)));
                    break;
                case 4:
                    if (it->frame != 0 || rand_below(int(data) + 2) == 0) it->frame = uint16_t((it->frame + 1) % count);
                    break;
                case 5:
                    call_event(id, kEvAnim);
                    break;
                case 6:
                    if (data == 1 && rand_below(2)) break;
                    advance(true);
                    break;
                default: break;
                }
            }
        }
}

// ---------------------------------------------------------------------------------------------
// Usecode events

int World::usecode_class(const Item& it) const {
    if (it.id < 256) {
        if (it.ext_flags & kExtPermanentNpc) return 1024 + it.id;
        if (!(it.flags & kItemFastOnly)) return -1;  // events.md rule 2
        return it.shape;
    }
    if (data_.typeflags.get(it.shape).family() == kFamUsecodeEgg) return it.quality + 0x47F;
    return it.shape;
}

uint16_t World::call_event(uint16_t id, unsigned event, const std::vector<uint8_t>& args) {
    const Item* it = item(id);
    if (!it) return 0;
    if (event == kEvLook) game_log.on_look(*this, id);  // the HUD's message log: "You see ..."
    if (event == kEvUse && item_powers::on_use(*this, id)) return 0;  // Item powers (game/item_powers.h)
    if (event == kEvUse) if (uint16_t pid = rock_flesh::on_use(*this, id)) return pid;  // Rock Flesh, remade
    if ((event == kEvUse || event == kEvLook) && leveling && leveling->growth->on_event(id, event)) return 0;  // tomes, elixirs
    if (event == kEvLook && id < 256 && creatures && creatures->look_event(id)) return 0;  // new creatures, "young troll"
    if (event == kEvGotHit && creatures && args.size() >= 2) creatures->on_touch(id, uint16_t(args[0] | (args[1] << 8)));  // bait in water
    if (id < 256 && (it->ext_flags & kExtCompanion) && party) {  // companions: no usecode, engine-side
        party->handle_event(id, event);
        return 0;
    }
    int cls = usecode_class(*it);
    if (cls < 0 || !data_.usecode.event_entry(unsigned(cls), event)) return 0;
    ++events_started;
    uint16_t pid = vm_.start_event(uint16_t(cls), event, id, args);
    if (event == kEvUse && spellbook && pid) spellbook->on_use(id, pid);  // spells cast or made (game/spellbook.h)
    if (trace_events)
        PG_LOG("event %02X -> %s (class %d) item %u shape %u, pid %u, frame %u", event,
               data_.usecode.class_name(unsigned(cls)).c_str(), cls, id, it->shape, pid, kernel_.frame_number());
    return pid;
}

uint16_t World::call_event_w(uint16_t id, unsigned event, uint16_t a) {
    return call_event(id, event, {uint8_t(a), uint8_t(a >> 8)});
}

uint16_t World::call_event_ww(uint16_t id, unsigned event, uint16_t a, uint16_t b) {
    return call_event(id, event, {uint8_t(a), uint8_t(a >> 8), uint8_t(b), uint8_t(b >> 8)});
}

void World::item_enter_fast(Item& it) {
    // Shape 712 (class SFX: wind on the Tenebrae dock, drips on the Plateau and in the Hall of the
    // Mountain King) starts a persistent (type 1) ambient-sound loop script here; its
    // leaveFastArea kills it. specs/world/items.md 7 never fires the event,
    // because in the original the loop played on forever after a map change (no leaveFastArea
    // is sent then). This engine ends every process of a map item when the map changes
    // (save_map_items), so the loop ends there and the event is fired as for any item: the
    // ambience plays and stops (NOTES.md "Original bug fixes"; ctest bugfixes "ambient").
    if (Actor* a = it.as_actor()) {
        if (a->is(Actor::kDead)) return;
        if (a->id != 1) {
            a->set(Actor::kInCombat, false);
            if (const AnimSequence* s = data_.anim.get(a->shape, 2)) a->frame = s->at(a->dir, 0).frame;
        }
    }
    // events.md: the reference fires enterFastArea for every item (the egg classes need it).
    call_event(it.id, kEvEnterFast);
}

void World::item_leave_fast(Item& it) {
    const ShapeInfo& si = data_.typeflags.get(it.shape);
    if (!(it.flags & kItemFastOnly) || si.is(ShapeInfo::kNoisy)) call_event(it.id, kEvLeaveFast);
    close_bark(it.id);
    sound.item_left_fast(it.id);
    if (it.gravity_pid) {
        if (Process* p = kernel_.get(it.gravity_pid)) p->fail();
        it.gravity_pid = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Geometry

void World::footprint(const Item& it, int& xd, int& yd, int& zd) const {
    data_.typeflags.get(it.shape).footprint(it.flags & kItemFlipped, xd, yd, zd);
}

const Item* World::root_container(const Item& it) const {
    const Item* r = &it;
    for (int guard = 0; r->parent && guard < 64; ++guard) {
        const Item* p = item(r->parent);
        if (!p) break;
        r = p;
    }
    return r;
}

void World::location(const Item& it, int& x, int& y, int& z) const {
    const Item* r = root_container(it);
    x = r->x;
    y = r->y;
    z = r->z;
}

bool World::is_container(const Item& it) const {
    return it.as_actor() || data_.typeflags.get(it.shape).family() == kFamContainer;
}

// ---------------------------------------------------------------------------------------------
// Moving, containers, the ether

void World::detach(uint16_t id) {
    Item* it = item(id);
    if (!it) return;
    if (it->flags & kItemEthereal) {
        auto e = std::find(ether_.begin(), ether_.end(), id);
        if (e != ether_.end()) ether_.erase(e);
        return;
    }
    if (it->parent) {
        if (Item* p = item(it->parent)) {
            auto c = std::find(p->contents.begin(), p->contents.end(), id);
            if (c != p->contents.end()) p->contents.erase(c);
        }
        return;
    }
    map_.unlink(id);
}

void World::move_to(uint16_t id, int x, int y, int z, bool events) {
    Item* it = item(id);
    if (!it) return;
    bool was_fast = (it->flags & kItemFastArea) && !it->parent;
    if (it->flags & kItemEthereal) was_fast = false;
    detach(id);
    it->parent = 0;
    it->flags &= uint16_t(~(kItemContained | kItemEquipped | kItemEthereal));
    it->x = std::clamp(x, 0, 0xFFFF);
    it->y = std::clamp(y, 0, 0xFFFF);
    it->z = std::clamp(z, 0, 255);
    bool back = it->flags & (kItemDisposable | kItemFastOnly);
    map_.link(id, !back);
    if (events) call_event(id, kEvJustMoved);
    if (!(it = item(id))) return;
    bool dest_fast = map_.point_is_fast(it->x, it->y);
    if (dest_fast && !(it->flags & kItemFastArea)) {
        if (data_.typeflags.get(it->shape).family() == kFamGlobEgg) map_.expand_glob(id);
        item_enter_fast(*it);
        if ((it = item(id))) it->flags |= kItemFastArea;
    } else if (!dest_fast && was_fast && camera_followed() != id) {
        item_leave_fast(*it);
        if ((it = item(id))) {
            it->flags &= uint16_t(~kItemFastArea);
            if (it->flags & kItemFastOnly) destroy(id);
        }
    }
}

bool World::move_to_container(uint16_t id, uint16_t cont, bool check, bool at_end) {
    Item* it = item(id);
    Item* c = item(cont);
    if (!it || !c || !is_container(*c)) return false;
    if (it->parent == cont && !(it->flags & kItemEthereal)) return true;
    if (id < 256) return false;
    for (const Item* p = c; p; p = p->parent ? item(p->parent) : nullptr)
        if (p == it) return false;  // no cycles
    if (check && !can_put_in(id, cont)) return false;  // weight / volume (items.md 5 rule 4)
    bool returning = (it->flags & kItemEthereal) && it->parent == cont;
    detach(id);
    it->parent = cont;
    if (!returning) { it->x = 0; it->y = 0xFFFF; }  // 0xFFFF: placed at random when shown (items.md 5)
    it->z = 0;
    it->flags |= kItemContained;
    it->flags &= uint16_t(~(kItemEquipped | kItemEthereal));
    if (at_end) c->contents.push_back(id);
    else c->contents.insert(c->contents.begin(), id);
    if (root_container(*it)->id == 1) {
        std::function<void(Item&)> own = [&](Item& i) {
            i.flags |= kItemOwned;
            for (uint16_t k : i.contents)
                if (Item* ci = item(k)) own(*ci);
        };
        own(*it);
    }
    call_event(id, kEvJustMoved);
    if ((it = item(id)) && (c = item(cont))) {
        bool open = (c->flags & kItemGumpOpen) && cont != 1;
        if ((it->flags & kItemFastArea) && !open) {
            item_leave_fast(*it);
            if ((it = item(id))) it->flags &= uint16_t(~kItemFastArea);
        } else if (!(it->flags & kItemFastArea) && open) {
            item_enter_fast(*it);
            if ((it = item(id))) it->flags |= kItemFastArea;
        }
    }
    return true;
}

void World::push_ether(uint16_t id) {
    Item* it = item(id);
    if (!it || (it->flags & kItemEthereal)) return;
    uint16_t keep_parent = it->parent;
    detach(id);
    it->parent = keep_parent;
    it->flags |= kItemEthereal;
    ether_.push_back(id);
}

uint16_t World::ether_pop() {
    if (ether_.empty()) return 0;
    uint16_t id = ether_.back();
    ether_.pop_back();
    Item* it = item(id);
    if (!it) return 0;
    // Still flagged ETHEREAL (and keeps its parent) until it is placed.
    ether_.push_back(id);  // detach() inside the placement removes it
    return id;
}

uint16_t World::create_item(unsigned shape, unsigned frame) {
    auto it = std::make_unique<Item>();
    it->shape = uint16_t(shape);
    it->frame = uint16_t(frame);
    it->origin = Item::kCreated;
    it->ext_flags |= kExtNoInterp;
    uint16_t id = objs_.add_item(std::move(it));
    if (spellbook) spellbook->on_create(id);  // necromancy tokens made (game/spellbook.h)
    return id;
}

uint16_t World::create_monster(unsigned shape, unsigned frame) {
    auto a = std::make_unique<Actor>();
    a->shape = uint16_t(shape);
    a->frame = uint16_t(frame);
    a->origin = Item::kCreated;
    a->flags = kItemInNpcList;
    if (const MonsterInfo* m = monster_info(shape)) {
        a->hp = uint16_t(m->hp0 + rand_below(m->hp1 - m->hp0 + 1));
        a->dex = int16_t(m->dex0 + rand_below(m->dex1 - m->dex0 + 1));
        a->alignment = m->align & 0x0F;
        a->enemy_alignment = m->align >> 4;
    }
    uint16_t id = objs_.add_actor(std::move(a));
    if (Actor* na = actor(id)) {
        na->npcnum = uint8_t(id);
        na->mapnum = uint8_t(map_.map_number());
    }
    if (leveling) leveling->on_monster_created(id);  // Leveling: the map's level variant (stage 3)
    return id;
}

void World::destroy_contents(uint16_t id) {
    Item* it = item(id);
    if (!it) return;
    std::vector<uint16_t> c = it->contents;
    it->contents.clear();
    for (uint16_t k : c) {
        destroy_contents(k);
        kernel_.kill(k, kProcAny);
        objs_.erase(k);
    }
}

void World::destroy(uint16_t id, bool deferred) {
    if (id == 1) return;
    Item* it = item(id);
    if (!it) return;
    // A container spills its contents (items.md 5).
    if (!it->contents.empty() && !it->as_actor()) {
        std::vector<uint16_t> c = it->contents;
        int x, y, z;
        location(*it, x, y, z);
        for (uint16_t k : c) {
            if (it->parent) move_to_container(k, it->parent);
            else move_to(k, x, y, z, false);
        }
    }
    detach(id);
    if (camera_followed() == id) camera_follow(1);
    close_bark(id);
    kernel_.kill(id, kProcAny);
    if (deferred) kernel_.add(std::make_unique<DestroyItemProcess>(*this, id));
    else destroy_now(id);
}

void World::destroy_now(uint16_t id) {
    if (id == 1) return;
    Item* it = item(id);
    if (!it) return;
    detach(id);
    destroy_contents(id);
    if (camera_followed() == id) camera_follow(1);
    close_bark(id);
    kernel_.kill(id, kProcAny);
    objs_.erase(id);
}

// ---------------------------------------------------------------------------------------------
// Camera

void World::camera_follow(uint16_t id) {
    if (Process* p = kernel_.get(camera_pid_)) p->terminate();
    if (id == 0) {
        int pt[3];
        camera_point(pt);
        camera_pid_ = kernel_.add(std::make_unique<CameraProcess>(*this, pt[0], pt[1], pt[2]));
        return;
    }
    camera_pid_ = kernel_.add(std::make_unique<CameraProcess>(*this, id));
}

void World::camera_stay(int x, int y, int z) {
    if (Process* p = kernel_.get(camera_pid_)) p->terminate();
    camera_pid_ = kernel_.add(std::make_unique<CameraProcess>(*this, x, y, z));
}

uint16_t World::camera_scroll(int x, int y, int z, int frames) {
    int pt[3];
    camera_point(pt);
    if (Process* p = kernel_.get(camera_pid_)) p->terminate();
    auto cam = std::make_unique<CameraProcess>(*this, pt[0], pt[1], pt[2]);
    cam->start_scroll(x, y, z, frames);
    camera_pid_ = kernel_.add(std::move(cam));
    return camera_pid_;
}

void World::camera_point(int out[3]) const {
    if (auto* c = dynamic_cast<CameraProcess*>(kernel_.get(camera_pid_)); c && c->live()) {
        c->point(out);
    } else if (const Actor* av = avatar(); av && av->chunk >= 0) {
        out[0] = av->x; out[1] = av->y; out[2] = av->z + 20;
    } else {
        out[0] = 8192; out[1] = 8192; out[2] = 64;
    }
    out[0] += quake_offset[0];
    out[1] += quake_offset[1];
}

uint16_t World::camera_followed() const {
    auto* c = dynamic_cast<CameraProcess*>(kernel_.get(camera_pid_));
    return c && c->live() ? c->following() : 0;
}

void World::set_game_hours(unsigned h) {
    time_offset_ = uint32_t(h) * 27000u - kernel_.frame_number();
}

// ---------------------------------------------------------------------------------------------
// Eggs

void World::check_eggs() {
    const Actor* av = avatar();
    if (!av || av->chunk < 0) return;
    int axd, ayd, azd;
    footprint(*av, axd, ayd, azd);
    bool near_teleporter = false;
    std::vector<uint16_t> hatch;
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            if (!map_.is_fast(cx, cy)) continue;
            for (uint16_t id : map_.chunk(cx, cy)) {
                const Item* e = item(id);
                if (!e || id < 256) continue;
                unsigned fam = data_.typeflags.get(e->shape).family();
                if (fam != kFamUsecodeEgg && fam != kFamTeleportEgg) continue;
                int rx = (e->npcnum >> 4) * 32, ry = (e->npcnum & 15) * 32;
                bool in = e->x - rx <= av->x && av->x - axd < e->x + rx && e->y - ry <= av->y &&
                          av->y - ayd < e->y + ry && e->z - 48 < av->z && av->z <= e->z + 48;
                if (!in) continue;
                bool teleporter = fam == kFamTeleportEgg && e->frame != 1;
                if (teleporter) near_teleporter = true;
                if (e->hatched) continue;
                if (teleporter && just_teleported) continue;
                hatch.push_back(id);
            }
        }
    if (!near_teleporter) just_teleported = false;
    for (uint16_t id : hatch) {
        Item* e = item(id);
        if (!e) continue;
        e->hatched = true;
        if (data_.typeflags.get(e->shape).family() == kFamTeleportEgg) {
            if (e->frame != 1) {
                if (stitch && stitch_portal(*e)) {
                    // Stitched world: a teleporter between two seam maps is a crossing point.
                    pending_cross_ = e->mapnum;
                    pending_cross_egg_ = e->quality & 0xFF;
                } else {
                    kernel_.add(std::make_unique<TeleportProcess>(*this, e->mapnum, e->quality & 0xFF));
                }
            }
        } else {
            call_event(id, kEvHatch);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Searches

bool World::eval_criteria(const Item& it, const uint8_t* s, unsigned len) const {
    std::vector<uint16_t> st{1};
    auto pop = [&]() -> uint16_t { if (st.empty()) return 0; uint16_t v = st.back(); st.pop_back(); return v; };
    unsigned i = 0;
    auto word = [&]() -> uint16_t { uint16_t v = i + 1 < len ? uint16_t(s[i] | (s[i + 1] << 8)) : 0; i += 2; return v; };
    while (i < len) {
        uint8_t t = s[i++];
        switch (t) {
        case 0x00: st.push_back(0); break;
        case 0x01: st.push_back(1); break;
        case '$': return pop() != 0;
        case '%': st.push_back(word()); break;
        case '&': { uint16_t b = pop(), a = pop(); st.push_back(a && b); break; }
        case '+': { uint16_t b = pop(), a = pop(); st.push_back(a || b); break; }
        case '!': st.push_back(pop() == 0); break;
        case '=': { uint16_t b = pop(), a = pop(); st.push_back(a == b); break; }
        case '>': { uint16_t b = pop(), a = pop(); st.push_back(a > b); break; }
        case '<': { uint16_t b = pop(), a = pop(); st.push_back(a < b); break; }
        case ']': { uint16_t b = pop(), a = pop(); st.push_back(a >= b); break; }
        case '[': { uint16_t b = pop(), a = pop(); st.push_back(a <= b); break; }
        case '@': st.push_back(it.shape); break;
        case '`': st.push_back(it.frame); break;
        case '*': st.push_back(it.quality); break;
        case '#': st.push_back(it.npcnum); break;
        case '?': st.push_back(it.flags); break;
        case ':': st.push_back(uint16_t(data_.typeflags.get(it.shape).family())); break;
        default:
            if (t >= 'A' && t <= 'Z') {
                unsigned n = t - 0x40u;
                bool any = false;
                for (unsigned k = 0; k < n; ++k) any |= word() == it.shape;
                st.push_back(any);
            } else if (t >= 'a' && t <= 'z') {
                unsigned n = t - 0x60u;
                bool any = false;
                for (unsigned k = 0; k < n; ++k) any |= word() == it.frame;
                st.push_back(any);
            } else {
                return false;
            }
        }
    }
    return false;
}

void World::area_search(int cx, int cy, int range, bool recurse, std::vector<uint16_t>& out, int cxd, int cyd) const {
    int x0 = cx - cxd - range, x1 = cx + range, y0 = cy - cyd - range, y1 = cy + range;
    int c0 = std::max(0, x0 / 512 - 1), c1 = std::min(CurrentMap::kChunks - 1, x1 / 512 + 1);
    int r0 = std::max(0, y0 / 512 - 1), r1 = std::min(CurrentMap::kChunks - 1, y1 / 512 + 1);
    if (x1 < 0 || y1 < 0) return;
    std::function<void(uint16_t)> contents = [&](uint16_t id) {
        const Item* it = item(id);
        if (!it) return;
        for (uint16_t k : it->contents) {
            out.push_back(k);
            contents(k);
        }
    };
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            for (uint16_t id : map_.chunk(c, r)) {
                const Item* it = item(id);
                if (!it || (it->ext_flags & kExtSprite)) continue;
                if (it->x < x0 || it->x > x1 || it->y < y0 || it->y > y1) continue;
                out.push_back(id);
                if (recurse) contents(id);
            }
}

void World::surface_search(uint16_t ref, bool above, std::vector<uint16_t>& out) const {
    const Item* r = item(ref);
    if (!r || r->chunk < 0) return;
    int xd, yd, zd;
    footprint(*r, xd, yd, zd);
    int c0 = std::max(0, (r->x - xd) / 512 - 1), c1 = std::min(CurrentMap::kChunks - 1, r->x / 512 + 1);
    int r0 = std::max(0, (r->y - yd) / 512 - 1), r1 = std::min(CurrentMap::kChunks - 1, r->y / 512 + 1);
    for (int cy = r0; cy <= r1; ++cy)
        for (int cx = c0; cx <= c1; ++cx)
            for (uint16_t id : map_.chunk(cx, cy)) {
                if (id == ref) continue;
                const Item* it = item(id);
                if (!it || (it->ext_flags & kExtSprite)) continue;
                int oxd, oyd, ozd;
                footprint(*it, oxd, oyd, ozd);
                if (!xy_overlap(r->x, r->y, xd, yd, it->x, it->y, oxd, oyd)) continue;
                if (above ? it->z == r->z + zd : it->z + ozd == r->z) out.push_back(id);
            }
}

void World::loop_search(int type, uint16_t a, uint16_t b, const uint8_t* script, unsigned len,
                        std::vector<uint16_t>& out) {
    std::vector<uint16_t> cand;
    switch (type) {
    case 2: case 3: {
        const Item* c = item(a);
        if (!c) return;
        int x, y, z;
        location(*c, x, y, z);
        // The centre item's own footprint widens the box on the low side (loops.md 1.1); for an
        // item in a container it is the outermost container's.
        int xd = 0, yd = 0, zd = 0;
        footprint(*root_container(*c), xd, yd, zd);
        area_search(x, y, b, type == 3, cand, xd, yd);
        break;
    }
    case 4: case 5: {
        const Item* c = item(b);
        if (!c) return;
        std::function<void(const Item&)> walk = [&](const Item& con) {
            for (uint16_t k : con.contents) {
                cand.push_back(k);
                if (type == 5)
                    if (const Item* ci = item(k)) walk(*ci);
            }
        };
        walk(*c);
        break;
    }
    case 6: {
        bool below = b != 0xFFFF;
        surface_search(below ? b : a, !below, cand);
        break;
    }
    default: return;
    }
    for (uint16_t id : cand)
        if (const Item* it = item(id); it && eval_criteria(*it, script, len)) out.push_back(id);
}

}  // namespace pg
