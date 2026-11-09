// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Savegames: the engine's own binary format (specs/formats/savegame.md says a new engine may
// use one, as long as it starts new games from U8SAVE.000). A save holds the whole world
// state, in the order startup.md 4 lists for a load: usecode globals / strings / lists, the
// kernel with every process, engine state (stasis, time, palette, camera), all objects, the
// current map's chunk lists and fast area, every other map's stored nonfixed items, and the
// open item windows, barks and answer menu. Nothing is re-hatched or re-cached on load.
#include "game/creatures.h"
#include "game/virtues.h"
#include "game/spellbook.h"
#include "game/rock_flesh.h"
#include "game/savegame.h"
#include "game/banter.h"
#include "game/variants.h"
#include "world/world_space.h"

#include <filesystem>
#include <system_error>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

#include "core/config.h"
#include "core/log.h"
#include "core/bugfix.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/avatar_mover.h"
#include "game/combat.h"
#include "game/pathfinder.h"
#include "game/npc_dialog.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/turn_combat.h"
#include "game/processes.h"
#include "game/world.h"
#include "game/item_powers.h"

namespace pg {

// ---------------------------------------------------------------------------------------------
// Slots on disk

std::string SaveStore::path(int slot) const {
    char name[32];
    std::snprintf(name, sizeof name, "pagan_%03d.sav", slot);
    return (dir_.empty() ? std::string(".") : dir_) + "/" + name;
}

namespace {
bool read_file(const std::string& p, std::vector<uint8_t>& out, size_t limit = 0) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
        if (limit && out.size() >= limit) break;
    }
    std::fclose(f);
    return true;
}
}  // namespace

void SaveStore::refresh() {
    for (int s = 0; s <= kMaxSlot; ++s) {
        desc_[size_t(s)].clear();
        if (s < 2) continue;
        std::vector<uint8_t> head;
        SaveHeader h;
        if (read_file(path(s), head, 4096) && SaveHeader::read(head, h)) desc_[size_t(s)] = h.description.empty() ? "(saved)" : h.description;
    }
}

bool SaveStore::write(int slot, const std::vector<uint8_t>& data) {
    if (slot < 2 || slot > kMaxSlot) return false;
    if (!dir_.empty()) { std::error_code ec; std::filesystem::create_directories(dir_, ec); }
    std::string p = path(slot), tmp = p + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { PG_LOG("save: cannot write %s", tmp.c_str()); return false; }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), p.c_str()) != 0) { PG_LOG("save: writing %s failed", p.c_str()); return false; }
    SaveHeader h;
    if (SaveHeader::read(data, h)) desc_[size_t(slot)] = h.description;
    last_slot = slot;
    if (slot != kSnapshotSlot) {
        Config::global().set_int("save_last", slot);
        Config::global().save();
    }
    if (on_write) on_write();
    return true;
}

bool SaveStore::read(int slot, std::vector<uint8_t>& data) const {
    return slot > 1 && slot <= kMaxSlot && read_file(path(slot), data);
}

int SaveStore::latest_autosave() const {
    int last = Config::global().get_int("autosave_last", 0);
    if (last >= kAutoFirst && last < kAutoFirst + kAutoSlots && exists(last)) return last;
    int best = 0;
    long long best_t = -1;
    for (int s = kAutoFirst; s < kAutoFirst + kAutoSlots; ++s) {
        struct stat st;
        if (!exists(s) || stat(path(s).c_str(), &st) != 0) continue;
        if ((long long)st.st_mtime > best_t) { best_t = (long long)st.st_mtime; best = s; }
    }
    return best;
}

int SaveStore::latest() const {
    int last = Config::global().get_int("save_last", 0);
    if (last != kSnapshotSlot && exists(last)) return last;
    int best = 0;
    long long best_t = -1;
    for (int s = 2; s <= kMaxSlot; ++s) {
        struct stat st;
        if (s == kSnapshotSlot || !exists(s) || stat(path(s).c_str(), &st) != 0) continue;
        if ((long long)st.st_mtime > best_t) { best_t = (long long)st.st_mtime; best = s; }
    }
    return best;
}

int SaveStore::latest_diary() const {
    int last = Config::global().get_int("save_last", 0);
    if (last >= 2 && last <= kSlots && exists(last)) return last;
    int best = 0;
    long long best_t = -1;
    for (int s = 2; s <= kSlots; ++s) {
        struct stat st;
        if (!exists(s) || stat(path(s).c_str(), &st) != 0) continue;
        if ((long long)st.st_mtime > best_t) { best_t = (long long)st.st_mtime; best = s; }
    }
    return best;
}

bool SaveHeader::read(const std::vector<uint8_t>& data, SaveHeader& out) {
    if (data.size() < 12 || std::memcmp(data.data(), kMagic, 8) != 0) return false;
    SaveReader in(data.data(), data.size());
    char magic[8];
    in.raw(magic, 8);
    uint32_t ver = in.u32();
    if (ver < 1 || ver > kVersion) return false;
    out.description = in.str();
    out.map = in.u32();
    out.game_frames = in.u32();
    out.avatar_name = in.str();
    return !in.fail();
}

// ---------------------------------------------------------------------------------------------
// Process state (one pair per class; the base fields are written by Kernel::save)

namespace {
void put3(SaveWriter& o, const int v[3]) { o.i32(v[0]); o.i32(v[1]); o.i32(v[2]); }
void get3(SaveReader& i, int v[3]) { v[0] = i.i32(); v[1] = i.i32(); v[2] = i.i32(); }
void put_pal(SaveWriter& o, const PaletteTransform& t) { for (int32_t v : t.m) o.i32(v); }
void get_pal(SaveReader& i, PaletteTransform& t) { for (int32_t& v : t.m) v = i.i32(); }
void put_state(SaveWriter& o, const ActorState& s) {
    o.i32(s.x); o.i32(s.y); o.i32(s.z); o.u8(s.dir); o.u16(s.last_anim); o.u16(s.last_anim_dir);
    o.b(s.first_step); o.b(s.flipped); o.b(s.in_combat);
}
void get_state(SaveReader& i, ActorState& s) {
    s.x = i.i32(); s.y = i.i32(); s.z = i.i32(); s.dir = uint8_t(i.u8()); s.last_anim = uint16_t(i.u16());
    s.last_anim_dir = uint16_t(i.u16()); s.first_step = i.b(); s.flipped = i.b(); s.in_combat = i.b();
}
}  // namespace

void CameraProcess::save(SaveWriter& o) const {
    o.u16(follow_); put3(o, pos_); put3(o, from_); put3(o, to_); o.i32(scroll_left_); o.i32(scroll_total_);
}
void CameraProcess::load(SaveReader& i) {
    follow_ = uint16_t(i.u16()); get3(i, pos_); get3(i, from_); get3(i, to_); scroll_left_ = i.i32(); scroll_total_ = i.i32();
}
void EggHatcherProcess::save(SaveWriter&) const {}
void EggHatcherProcess::load(SaveReader&) {}
void SchedulerProcess::save(SaveWriter& o) const { o.u32(last_hour); o.i32(next_actor_); }
void SchedulerProcess::load(SaveReader& i) { last_hour = i.u32(); next_actor_ = i.i32(); }
void HealProcess::save(SaveWriter& o) const { o.u32(hunger); o.u32(counter_); }
void HealProcess::load(SaveReader& i) { hunger = i.u32(); counter_ = i.u32(); }
void StartupProcess::save(SaveWriter&) const {}
void StartupProcess::load(SaveReader&) {}

void AnimTracker::save(SaveWriter& o) const {
    o.u16(actor_); o.u32(action_); o.u32(shape_); o.i32(dir_);
    o.u32(uint32_t(frames_.size()));
    for (unsigned f : frames_) o.u16(f);
    o.i32(idx_);
    o.i32(x_); o.i32(y_); o.i32(z_); o.i32(px_); o.i32(py_); o.i32(pz_);
    o.i32(sx_); o.i32(sy_); o.i32(sz_); o.i32(sxd_); o.i32(syd_); o.i32(szd_);
    o.u16(mflags_);
    put_state(o, start_);
    o.b(flipped_); o.b(blocked_); o.b(unsupported_); o.u16(hit_); o.b(targeted_);
    put3(o, gap_);
    o.i32(air_left_);
    o.b(seq_ != nullptr);
}
void AnimTracker::load(SaveReader& i) {
    actor_ = uint16_t(i.u16()); action_ = i.u32(); shape_ = i.u32(); dir_ = i.i32();
    frames_.assign(i.u32(), 0);
    for (unsigned& f : frames_) f = i.u16();
    idx_ = i.i32();
    x_ = i.i32(); y_ = i.i32(); z_ = i.i32(); px_ = i.i32(); py_ = i.i32(); pz_ = i.i32();
    sx_ = i.i32(); sy_ = i.i32(); sz_ = i.i32(); sxd_ = i.i32(); syd_ = i.i32(); szd_ = i.i32();
    mflags_ = uint16_t(i.u16());
    get_state(i, start_);
    flipped_ = i.b(); blocked_ = i.b(); unsupported_ = i.b(); hit_ = uint16_t(i.u16()); targeted_ = i.b();
    get3(i, gap_);
    air_left_ = i.i32();
    bool had = i.b();
    seq_ = had ? w_.data().anim.get(shape_, action_) : nullptr;
    frame_ = nullptr;
    if (seq_ && idx_ > 0 && size_t(idx_) <= frames_.size() && frames_[size_t(idx_ - 1)] < seq_->length)
        frame_ = &seq_->at(unsigned(dir_), frames_[size_t(idx_ - 1)]);
    if (seq_ && size_t(idx_) > frames_.size()) i.set_fail();
}

void ActorAnimProcess::save(SaveWriter& o) const {
    o.u32(action_); o.i32(dir_); o.i32(steps_limit_);
    o.b(started_); o.b(played_); o.b(locked_); o.b(fell_); o.i32(hold_); o.b(targeted_); put3(o, target_);
    o.b(tr_ != nullptr);
    if (tr_) tr_->save(o);
}
void ActorAnimProcess::load(SaveReader& i) {
    action_ = i.u32(); dir_ = i.i32(); steps_limit_ = i.i32();
    started_ = i.b(); played_ = i.b(); locked_ = i.b(); fell_ = i.b(); hold_ = i.i32(); targeted_ = i.b(); get3(i, target_);
    if (i.b()) {
        if (!tr_) tr_ = std::make_unique<AnimTracker>(w_);
        tr_->load(i);
    } else {
        tr_.reset();
    }
}

void LoiterProcess::save(SaveWriter& o) const { o.i32(count_); o.i32(stage_); }
void LoiterProcess::load(SaveReader& i) { count_ = i.i32(); stage_ = i.i32(); }
void SpriteProcess::save(SaveWriter& o) const {
    o.u16(sprite_); o.i32(first_); o.i32(last_); o.i32(repeats_); o.i32(delay_); o.i32(frame_); o.i32(wait_);
    put3(o, pos_); o.b(created_);
}
void SpriteProcess::load(SaveReader& i) {
    sprite_ = uint16_t(i.u16()); first_ = i.i32(); last_ = i.i32(); repeats_ = i.i32(); delay_ = i.i32(); frame_ = i.i32();
    wait_ = i.i32(); get3(i, pos_); created_ = i.b();
}
void NotifyProcess::save(SaveWriter&) const {}
void NotifyProcess::load(SaveReader&) {}
void FadeProcess::save(SaveWriter& o) const { put_pal(o, from_); put_pal(o, to_); o.i32(total_); o.i32(left_); }
void FadeProcess::load(SaveReader& i) { get_pal(i, from_); get_pal(i, to_); total_ = i.i32(); left_ = i.i32(); }
void InvertProcess::save(SaveWriter& o) const { o.i32(target_); o.i32(step_); }
void InvertProcess::load(SaveReader& i) { target_ = i.i32(); step_ = i.i32(); }
void TeleportProcess::save(SaveWriter& o) const { o.u32(map_); o.u32(egg_); }
void TeleportProcess::load(SaveReader& i) { map_ = i.u32(); egg_ = i.u32(); }
void DestroyItemProcess::save(SaveWriter&) const {}
void DestroyItemProcess::load(SaveReader&) {}
void GravityProcess::save(SaveWriter& o) const {
    o.i32(vx_); o.i32(vy_); o.i32(vz_); o.i32(g_); o.i32(frames_); o.i32(fall_start_);
}
void GravityProcess::load(SaveReader& i) {
    vx_ = i.i32(); vy_ = i.i32(); vz_ = i.i32(); g_ = i.i32(); frames_ = i.i32(); fall_start_ = i.i32();
}
void ClearFeignProcess::save(SaveWriter& o) const { o.i32(stage_); }
void ClearFeignProcess::load(SaveReader& i) { stage_ = i.i32(); }
void CombatProcess::save(SaveWriter& o) const { o.u8(uint32_t(mode_)); o.i32(delay_); }
void CombatProcess::load(SaveReader& i) { mode_ = Mode(i.u8()); delay_ = i.i32(); }
void AvatarDeathProcess::save(SaveWriter& o) const { o.i32(stage_); }
void AvatarDeathProcess::load(SaveReader& i) { stage_ = i.i32(); }
void ResurrectProcess::save(SaveWriter& o) const { o.i32(left_); }
void ResurrectProcess::load(SaveReader& i) { left_ = i.i32(); }
void AvatarMoverProcess::save(SaveWriter& o) const {
    o.i32(idle_); o.i32(last_look_); o.b(back_active_); o.i32(back_dir_); o.b(was_moving_);
}
void AvatarMoverProcess::load(SaveReader& i) {
    idle_ = i.i32(); last_look_ = i.i32(); back_active_ = i.b(); back_dir_ = i.i32(); was_moving_ = i.b();
}
void PathfindProcess::save(SaveWriter& o) const {
    o.i32(tx_); o.i32(ty_); o.i32(tz_); o.u16(target_); o.b(hit_); o.b(planned_);
    o.u32(uint32_t(path_.size()));
    for (const PathStep& s : path_) { o.u32(s.action); o.i32(s.dir); o.i32(s.steps); }
    o.u32(uint32_t(next_)); put3(o, target_pos_); o.i32(replans_);
}
void PathfindProcess::load(SaveReader& i) {
    tx_ = i.i32(); ty_ = i.i32(); tz_ = i.i32(); target_ = uint16_t(i.u16()); hit_ = i.b(); planned_ = i.b();
    path_.assign(i.u32(), PathStep{});
    for (PathStep& s : path_) { s.action = i.u32(); s.dir = i.i32(); s.steps = i.i32(); }
    next_ = i.u32(); get3(i, target_pos_); replans_ = i.i32();
}

namespace {
std::unique_ptr<Process> make_process(World& w, const std::string& t) {
    if (t == "usecode") return std::make_unique<UCProcess>(w.vm(), 0, 0, false, std::vector<uint8_t>{}, std::vector<uint8_t>{});
    if (t == "delay") return std::make_unique<DelayProcess>(0);
    if (t == "camera") return std::make_unique<CameraProcess>(w, 0);
    if (t == "egghatch") return std::make_unique<EggHatcherProcess>(w);
    if (t == "scheduler") return std::make_unique<SchedulerProcess>(w);
    if (t == "heal") return std::make_unique<HealProcess>(w);
    if (t == "startup") return std::make_unique<StartupProcess>(w);
    if (t == "anim") return std::make_unique<ActorAnimProcess>(w, 0, 0, 0, 0);
    if (t == "loiter") return std::make_unique<LoiterProcess>(w, 0, 0);
    if (t == "sprite") return std::make_unique<SpriteProcess>(w);
    if (t == "notify") return std::make_unique<NotifyProcess>(0);
    if (t == "fade") return std::make_unique<FadeProcess>(w, PaletteTransform{}, PaletteTransform{}, 0);
    if (t == "invert") return std::make_unique<InvertProcess>(w, 0);
    if (t == "teleport") return std::make_unique<TeleportProcess>(w, 0, 0);
    if (t == "destroy") return std::make_unique<DestroyItemProcess>(w, 0);
    if (t == "gravity") return std::make_unique<GravityProcess>(w, 0, 0, 0, 0, 0);
    if (t == "clearfeign") return std::make_unique<ClearFeignProcess>(w, 0);
    if (t == "combat") return std::make_unique<CombatProcess>(w, 0);
    if (t == "death") return std::make_unique<AvatarDeathProcess>(w);
    if (t == "resurrect") return std::make_unique<ResurrectProcess>(w, 0, 0);
    if (t == "mover") return std::make_unique<AvatarMoverProcess>(w);
    if (t == "pathfind") return std::make_unique<PathfindProcess>(w, 0, 0, 0, 0, 0);
    return nullptr;
}

void put_record(SaveWriter& o, const ItemRecord& r) {
    o.u16(r.x); o.u16(r.y); o.u8(r.z); o.u16(r.shape); o.u8(r.frame); o.u16(r.flags); o.u16(r.quality);
    o.u8(r.npcnum); o.u8(r.mapnum);
}
void get_record(SaveReader& i, ItemRecord& r) {
    r.x = uint16_t(i.u16()); r.y = uint16_t(i.u16()); r.z = uint8_t(i.u8()); r.shape = uint16_t(i.u16());
    r.frame = uint8_t(i.u8()); r.flags = uint16_t(i.u16()); r.quality = uint16_t(i.u16()); r.npcnum = uint8_t(i.u8());
    r.mapnum = uint8_t(i.u8());
}
void put_pages(SaveWriter& o, const std::vector<TextPage>& pages) {
    o.u32(uint32_t(pages.size()));
    for (const TextPage& p : pages) {
        o.u32(uint32_t(p.lines.size()));
        for (const std::string& l : p.lines) o.str(l);
        o.u32(p.chars); o.i32(p.width); o.i32(p.height);
    }
}
void get_pages(SaveReader& i, std::vector<TextPage>& pages) {
    pages.assign(std::min<uint32_t>(i.u32(), 10000), TextPage{});
    for (TextPage& p : pages) {
        p.lines.assign(std::min<uint32_t>(i.u32(), 10000), "");
        for (std::string& l : p.lines) l = i.str();
        p.chars = i.u32(); p.width = i.i32(); p.height = i.i32();
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// The world

void World::save_game(SaveWriter& o, const std::string& description) const {
    o.raw(SaveHeader::kMagic, 8);
    o.u32(SaveHeader::kVersion);
    o.str(description);
    o.u32(map_.map_number());
    o.u32(game_frames());
    o.str(avatar_name);

    vm_.save(o);
    kernel_.save(o);

    o.tag("WRLD");
    o.b(stasis); o.str(avatar_name); o.b(cheats); o.b(cheated);
    o.u32(time_offset_); o.i32(engine_quake ? 0 : quake); o.i32(quake_offset[0]); o.i32(quake_offset[1]);
    for (int v : stat_accumulator) o.i32(v);
    o.u32(last_attack_frame); o.b(game_over); o.u16(death_pid); o.u16(mover_pid); o.u16(camera_pid_);
    put_pal(o, palette); put_pal(o, palette_target); o.u32(palette_xform); o.u16(fade_pid); o.i32(fade_priority);
    o.i32(invert_state); o.b(just_teleported); o.u32(startup_state); o.u64(events_started);
    put3(o, prev_cam_); o.b(fast_init_);
    std::ostringstream rs;
    rs << rng;
    o.str(rs.str());
    o.u8(uint32_t(options.text_speed)); o.b(options.music); o.b(options.sfx); o.b(options.steps);
    o.b(options.animations); o.b(options.frame_skip); o.b(options.speed_limit);
    sound.save(o);
    o.u32(uint32_t(visited_maps.size()));  // v3: maps the Avatar has been on (critical path)
    for (unsigned m : visited_maps) o.u16(m);

    o.tag("OBJS");
    objs_.save_ids(o);
    uint32_t count = 0;
    for (size_t i = 1; i < objs_.slots(); ++i) count += objs_.get(uint16_t(i)) != nullptr;
    o.u32(count);
    for (size_t i = 1; i < objs_.slots(); ++i) {
        const Item* it = objs_.get(uint16_t(i));
        if (!it) continue;
        const Actor* a = it->as_actor();
        o.u16(it->id); o.b(a != nullptr);
        o.u16(it->shape); o.u16(it->frame); o.i32(it->x); o.i32(it->y); o.i32(it->z);
        o.u16(it->flags); o.u16(it->ext_flags); o.u16(it->quality); o.u8(it->npcnum); o.u8(it->mapnum);
        o.u8(it->origin); o.b(it->alive); o.u16(it->parent); o.u16(it->gravity_pid); o.b(it->hatched);
        o.u32(uint32_t(it->contents.size()));
        for (uint16_t c : it->contents) o.u16(c);
        if (a) {
            o.u16(uint16_t(a->str)); o.u16(uint16_t(a->dex)); o.u16(uint16_t(a->intel)); o.u16(a->hp);
            o.u16(uint16_t(a->mana)); o.u8(a->dir); o.u16(a->last_anim); o.u16(a->last_anim_dir);
            o.u8(a->alignment); o.u8(a->enemy_alignment); o.u8(a->unk0c); o.u32(a->actor_flags);
            o.u16(a->target); o.u16(a->fixed_target); o.u16(a->activity); o.str(a->name);
        }
    }
    o.u32(uint32_t(ether_.size()));
    for (uint16_t e : ether_) o.u16(e);

    o.tag("MAPS");
    o.u32(map_.map_number());
    CurrentMap& m = const_cast<CurrentMap&>(map_);
    for (int c = 0; c < CurrentMap::kChunks * CurrentMap::kChunks; ++c) {
        const std::vector<uint16_t>& l = map_.chunk(c % CurrentMap::kChunks, c / CurrentMap::kChunks);
        o.u32(uint32_t(l.size()));
        for (uint16_t id : l) o.u16(id);
    }
    std::vector<uint8_t> bits(CurrentMap::kChunks * CurrentMap::kChunks / 8, 0);
    const std::vector<bool>& fast = m.fast_bits();
    for (size_t c = 0; c < fast.size(); ++c)
        if (fast[c]) bits[c / 8] |= uint8_t(1u << (c % 8));
    o.bytes(bits.data(), bits.size());
    o.u32(uint32_t(nonfixed_.size()));
    for (const auto& store : nonfixed_) {
        o.u32(uint32_t(store.size()));
        for (const MapEntry& e : store) { put_record(o, e.rec); o.i32(e.parent); }
    }

    o.tag("GUMP");
    o.u32(uint32_t(barks.size()));
    for (const Bark& b : barks) {
        o.u16(b.speaker); o.u16(b.pid); o.i32(b.font); put_pages(o, b.pages); o.u32(b.page); o.i32(b.ticks_left);
    }
    o.b(ask_menu != nullptr);
    if (ask_menu) {
        const AskMenu& a = *ask_menu;
        o.u16(a.pid); o.u16(a.list);
        o.u32(uint32_t(a.answers.size()));
        for (uint16_t s : a.answers) o.u16(s);
        o.u32(uint32_t(a.buttons.size()));
        for (const auto& b : a.buttons) { o.i32(b.x); o.i32(b.y); o.i32(b.w); o.i32(b.h); }
        o.i32(a.w); o.i32(a.h); o.i32(a.frames_open);
    }
    gumps.save(o);
    party->save(o);  // v4: companions (game/party.h)
    // v5: the World option the save was made in and its layout's hash (game/world_stitch.cpp).
    // The live state is the same in both modes (one map, its own coordinates), so a save loads
    // in either mode: Stitched <-> Original needs no conversion.
    o.tag("WRLD");
    o.u8(stitch ? 1 : 0);
    o.u32(stitch ? stitch->layout_hash() : 0);
    item_powers::save(*this, o);  // optional section, only while a power is on cooldown
    leveling->save(o);  // v6
    // Optional: travel banter heard and banter points recharging (game/banter.h).
    if (party->banter->has_state()) party->banter->save(o);
    // Optional: the new creatures out in the world (game/creatures.h).
    if (creatures->has_state()) creatures->save(o);
    // Optional: the Avatar's virtue scores and the deeds that have paid (game/virtues.h).
    if (virtues->has_state()) virtues->save(o);
    // Optional: the spells entered in the spellbook (game/spellbook.h).
    if (spellbook->has_state()) spellbook->save(o);
    rock_flesh::save(*this, o);  // optional: Rock Flesh effects running (RKFL)
    o.tag("END.");
}

void World::reset_state() {
    sky_bolts.clear();
    stone_skin.clear();
    sky_flash = 0;
    if (turn) turn->world_replaced();  // turn-based combat: a load ends any fight in memory
    kernel_.reset();
    vm_.reset();
    gumps.list.clear();
    objs_.clear();
    ether_.clear();
    barks.clear();
    ask_menu.reset();
    if (dialog) dialog->reset();
    talk_pid = talk_item = stasis_pid = stasis_cls = 0;
    target_pid = 0;
    camera_pid_ = 0;
    request = Request{};
    red_cross_frames = 0;
    input = PlayerInput{};
    map_.reset(0);
    sound.reset();
}

bool World::load_game(SaveReader& i) {
    char magic[8];
    i.raw(magic, 8);
    uint32_t version = 0;
    if (std::memcmp(magic, SaveHeader::kMagic, 8) != 0 || (version = i.u32()) < 1 || version > SaveHeader::kVersion) {
        PG_LOG("load: not a savegame of this engine version");
        return false;
    }
    i.str();
    i.u32();
    i.u32();
    i.str();
    reset_state();
    auto fail = [&](const char* where) {
        PG_LOG("load: bad data in section %s", where);
        reset_state();
        return false;
    };
    if (!vm_.load(i)) return fail("usecode");
    if (!kernel_.load(i, [this](const std::string& t) { return make_process(*this, t); })) return fail("kernel");

    if (!i.tag("WRLD")) return fail("world");
    stasis = i.b(); avatar_name = i.str(); cheats = i.b(); cheated = i.b();
    int v1_music = version == 1 ? int(i.u32()) : 0;
    if (version == 1) i.u32();  // v1: the combat-saved track
    time_offset_ = i.u32(); quake = i.i32(); quake_offset[0] = i.i32(); quake_offset[1] = i.i32();
    for (int& v : stat_accumulator) v = i.i32();
    last_attack_frame = i.u32(); game_over = i.b(); death_pid = uint16_t(i.u16()); mover_pid = uint16_t(i.u16());
    camera_pid_ = uint16_t(i.u16());
    get_pal(i, palette); get_pal(i, palette_target); palette_xform = i.u32(); fade_pid = uint16_t(i.u16());
    fade_priority = i.i32();
    invert_state = i.i32(); just_teleported = i.b(); startup_state = i.u32(); events_started = i.u64();
    if (original_bug_fixes()) invert_state = 0;  // the catacomb lever makes sparkles now (intrinsics.cpp 0x0C8)
    get3(i, prev_cam_); fast_init_ = i.b();
    {
        std::istringstream rs(i.str());
        rs >> rng;
    }
    options.text_speed = int(i.u8()); options.music = i.b(); options.sfx = i.b(); options.steps = i.b();
    options.animations = i.b(); options.frame_skip = i.b(); options.speed_limit = i.b();
    load_options();  // the player's persistent settings win over the ones in the save
    if (version >= 2) sound.load(i);
    else { sound.reset(); sound.music_play(v1_music); }
    visited_maps.clear();
    if (version >= 3) {
        uint32_t n = std::min<uint32_t>(i.u32(), 256);
        for (uint32_t k = 0; k < n; ++k) visited_maps.insert(unsigned(i.u16()));
    }

    if (!i.tag("OBJS")) return fail("objects");
    SaveReader ids_at = i;  // the pools are applied after the objects exist
    {
        ObjectTable scratch;
        scratch.load_ids(i);
    }
    uint32_t count = i.u32();
    for (uint32_t k = 0; k < count && !i.fail(); ++k) {
        uint16_t id = uint16_t(i.u16());
        bool is_actor = i.b();
        std::unique_ptr<Item> it;
        if (is_actor) it = std::make_unique<Actor>();
        else it = std::make_unique<Item>();
        it->shape = uint16_t(i.u16()); it->frame = uint16_t(i.u16()); it->x = i.i32(); it->y = i.i32(); it->z = i.i32();
        it->flags = uint16_t(i.u16()); it->ext_flags = uint16_t(i.u16() & ~kExtInChunk); it->quality = uint16_t(i.u16());
        it->npcnum = uint8_t(i.u8()); it->mapnum = uint8_t(i.u8());
        it->origin = Item::Origin(i.u8()); it->alive = i.b(); it->parent = uint16_t(i.u16());
        it->gravity_pid = uint16_t(i.u16()); it->hatched = i.b();
        it->contents.assign(std::min<uint32_t>(i.u32(), 32768), 0);
        for (uint16_t& c : it->contents) c = uint16_t(i.u16());
        it->chunk = -1;
        if (Actor* a = it->as_actor()) {
            a->str = int16_t(i.u16()); a->dex = int16_t(i.u16()); a->intel = int16_t(i.u16()); a->hp = uint16_t(i.u16());
            a->mana = int16_t(i.u16()); a->dir = uint8_t(i.u8()); a->last_anim = uint16_t(i.u16());
            a->last_anim_dir = uint16_t(i.u16()); a->alignment = uint8_t(i.u8()); a->enemy_alignment = uint8_t(i.u8());
            a->unk0c = uint8_t(i.u8()); a->actor_flags = i.u32(); a->target = uint16_t(i.u16());
            a->fixed_target = uint16_t(i.u16()); a->activity = uint16_t(i.u16()); a->name = i.str();
        }
        if (i.fail() || !objs_.add_at(id, std::move(it))) return fail("objects");
    }
    objs_.load_ids(ids_at);
    ether_.assign(std::min<uint32_t>(i.u32(), 32768), 0);
    for (uint16_t& e : ether_) e = uint16_t(i.u16());

    if (!i.tag("MAPS")) return fail("maps");
    unsigned mapnum = i.u32();
    map_.reset(mapnum);
    for (int c = 0; c < CurrentMap::kChunks * CurrentMap::kChunks && !i.fail(); ++c) {
        uint32_t n = i.u32();
        for (uint32_t k = 0; k < n && !i.fail(); ++k) map_.link(uint16_t(i.u16()));
    }
    std::vector<uint8_t> bits = i.bytes();
    std::vector<bool>& fast = map_.fast_bits();
    if (bits.size() * 8 != fast.size()) return fail("maps");
    for (size_t c = 0; c < fast.size(); ++c) fast[c] = (bits[c / 8] >> (c % 8)) & 1;
    nonfixed_.assign(std::min<uint32_t>(i.u32(), 256), {});
    for (auto& store : nonfixed_) {
        store.assign(std::min<uint32_t>(i.u32(), 65536), MapEntry{});
        for (MapEntry& e : store) { get_record(i, e.rec); e.parent = i.i32(); }
    }
    if (nonfixed_.size() < 256) nonfixed_.resize(256);

    if (!i.tag("GUMP")) return fail("gumps");
    barks.assign(std::min<uint32_t>(i.u32(), 1000), Bark{});
    for (Bark& b : barks) {
        b.speaker = uint16_t(i.u16()); b.pid = uint16_t(i.u16()); b.font = i.i32(); get_pages(i, b.pages);
        b.page = i.u32(); b.ticks_left = i.i32();
    }
    if (i.b()) {
        ask_menu = std::make_unique<AskMenu>();
        AskMenu& a = *ask_menu;
        a.pid = uint16_t(i.u16()); a.list = uint16_t(i.u16());
        a.answers.assign(std::min<uint32_t>(i.u32(), 1000), 0);
        for (uint16_t& s : a.answers) s = uint16_t(i.u16());
        a.buttons.assign(std::min<uint32_t>(i.u32(), 1000), AskMenu::Button{0, 0, 0, 0});
        for (auto& b : a.buttons) { b.x = i.i32(); b.y = i.i32(); b.w = i.i32(); b.h = i.i32(); }
        a.w = i.i32(); a.h = i.i32(); a.frames_open = i.i32();
    }
    if (!gumps.load(i)) return fail("gumps");
    if (version >= 4) {
        if (!party->load(i)) return fail("party");
    } else {
        party->reset();  // an older save: the companions appear at their homes as in a new game
    }
    if (version >= 5) {
        if (!i.tag("WRLD")) return fail("world");
        bool was_stitched = i.u8() != 0;
        uint32_t hash = i.u32();
        if (was_stitched != stitched())
            PG_LOG("load: saved with the %s world, loaded with the %s world (same state, no conversion needed)",
                   was_stitched ? "stitched" : "original", stitched() ? "stitched" : "original");
        else if (stitch && hash != stitch->layout_hash())
            PG_LOG("load: saved with another world layout (hash %08x, now %08x)", hash, stitch->layout_hash());
    }
    item_powers::load(*this, i);  // (no section: every power is ready)
    if (version >= 6) {
        if (!leveling->load(i)) return fail("levels");
    } else {
        leveling->reset();  // an older save: with the option on, the level comes from the story (Leveling::frame)
    }
    // (no section: nothing heard yet, every point ready; Party::load reset it)
    if (i.peek_tag("BNTR") && !party->banter->load(i)) return fail("banter");
    creatures->reset();  // (no section: nothing out yet; they rise as their maps are entered)
    if (i.peek_tag("CRTR") && !creatures->load(i)) return fail("creatures");
    virtues->reset();  // (no section: an older save, or nothing earned yet: every virtue at 0)
    if (i.peek_tag("VIRT") && !virtues->load(i)) return fail("virtues");
    spellbook->reset();  // (no section: nothing entered yet)
    if (i.peek_tag("SPBK") && !spellbook->load(i)) return fail("spellbook");
    rock_flesh::load(*this, i);  // (no section: none running)
    if (!i.tag("END.")) return fail("end");
    visited_maps.insert(map_.map_number());  // older saves: seeded with the current map
    underworld::sync_entrance(*this);  // the save holds the map as it was: the option may differ now
    PG_LOG("load: map %u, %zu objects, %zu processes, frame %u", mapnum, objs_.live_count(), kernel_.size(),
           kernel_.frame_number());
    return true;
}

}  // namespace pg
