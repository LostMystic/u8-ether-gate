// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/current_map.h"

#include <algorithm>
#include <cstdlib>

#include "core/bugfix.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "world/map_patches.h"

namespace pg {

namespace {
ItemRecord fixed_rec(uint16_t shape, uint8_t frame, uint16_t x, uint16_t y, uint8_t z) {
    ItemRecord r;
    r.shape = shape;
    r.frame = frame;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}
// "Shift to z": keep the screen position by adding 4 * dz to x and y.
void shift_z(ItemRecord& r, int newz) {
    int dz = newz - r.z;
    r.x = uint16_t(r.x + 4 * dz);
    r.y = uint16_t(r.y + 4 * dz);
    r.z = uint8_t(newz);
}
}  // namespace

void apply_map_fixes(unsigned map, std::vector<ItemRecord>& f) {
    static const auto fits = [](unsigned m) {
        if (compat::fixed_map_known(m)) return true;
        compat::off("the map fixes of map " + std::to_string(m), "its FIXED.DAT entry differs in this copy");
        return false;
    };
    switch (map == 7 || map == 50 || map == 62 || map == 49 || map == 21 || map == 5 ? (fits(map) ? map : 0u) : 0u) {
    case 7:
        // Original bug fixes (NOTES.md "Original bug fixes"): the spec's second tile, at y 21157, is
        // 38 units off the 128 grid, which leaves a 38-unit gap between it and the floor north of
        // it (y 20992..21029) that the Avatar's centre can drop into; at 21119 it fills the hole.
        for (auto [x, y] : {std::pair{2815, 25727}, {9983, original_bug_fixes() ? 21119 : 21157}, {13183, 16511}})
            f.push_back(fixed_rec(301, 1, uint16_t(x), uint16_t(y), 8));
        break;
    case 50:
        f.push_back(fixed_rec(34, 7, 16127, 6143, 0));
        break;
    case 62: {
        static const int xy[][2] = {
            {16255, 6143}, {16639, 6143}, {16511, 6143}, {15999, 6143}, {15871, 6143}, {15743, 6143},
            {15615, 6143}, {15999, 6015}, {15871, 6015}, {15743, 6015}, {15615, 6015}, {20095, 6911},
            {20223, 6911}, {20095, 6783}, {20223, 6783}, {19839, 6655}, {19967, 6655}, {19839, 6527},
            {19967, 6527}, {20095, 6527}, {19967, 6399}, {19839, 6399}, {19711, 6399}};
        for (auto& p : xy) f.push_back(fixed_rec(301, 1, uint16_t(p[0]), uint16_t(p[1]), 48));
        f.push_back(fixed_rec(497, 0, 15487, 6271, 48));
        f.push_back(fixed_rec(497, 0, 15359, 6271, 48));
        for (auto [x, y] : {std::pair{14975, 6399}, {14975, 6015}, {15103, 6015}})
            f.push_back(fixed_rec(409, 32, uint16_t(x), uint16_t(y), 0));
        if (original_bug_fixes()) {
            // Original bug fixes: the holes the spec's list leaves (specs/world/bugfixes.md 12: "more
            // missing tiles from about (16555, 6075, 48) west to the lava, and around (19999,
            // 6758, 48)"), found by the map scanner (tests/test_bugfixes.cpp "scan"): 128-unit
            // gaps at z 48 with floor on all four sides, nothing under them; one at the lava's edge.
            static const int g[][3] = {{16127, 6143, 1}, {16767, 6143, 1}, {16895, 6143, 1}, {19071, 6143, 3}, {20095, 7167, 3}};
            for (auto& p : g) f.push_back(fixed_rec(301, uint8_t(p[2]), uint16_t(p[0]), uint16_t(p[1]), 48));
            f.push_back(fixed_rec(409, 32, 14975, 6527, 0));
        }
        break;
    }
    case 49:
        for (auto& r : f)
            if (r.shape == 347 && r.z == 96 &&
                ((r.x == 23007 && r.y == 21343) || (r.x == 23135 && r.y == 21471) || (r.x == 23135 && r.y == 21343)))
                shift_z(r, 40);
        break;
    case 21:
        for (auto& r : f)
            if ((r.shape == 301 || r.shape == 31 || r.shape == 32) && r.z == 8 && (r.x == 6783 || r.x == 6655) &&
                (r.y == 15743 || r.y == 15615))
                shift_z(r, 16);
        break;
    case 5: {
        static const int xy[][2] = {{9151, 24127}, {9279, 23999}, {9535, 23615}, {9151, 23487},
                                    {10303, 23487}, {9919, 23487}, {10559, 23487}};
        for (auto& r : f)
            if (r.shape == 71 && r.frame == 8 && r.z == 0)
                for (auto& p : xy)
                    if (r.x == p[0] && r.y == p[1]) { shift_z(r, 48); break; }
        break;
    }
    default: break;
    }
    map_patches::apply(map, f);  // new content, after the fixes (world/map_patches.h)
}

std::vector<unsigned> CurrentMap::populated_maps(const GameData& d) {
    std::vector<unsigned> out;
    unsigned n = std::max(d.fixed.count(), d.nonfixed.count());
    for (unsigned m = 1; m < n; ++m)  // map 0 is the NPC list, not a place
        if (d.fixed.entry(m).size >= 16 * 16) out.push_back(m);
    return out;
}

CurrentMap::CurrentMap(const GameData& data)
    : data_(data), own_(std::make_unique<ObjectTable>()), objs_(own_.get()) {
    dead_.alive = false;
}

CurrentMap::CurrentMap(const GameData& data, ObjectTable& objects) : data_(data), objs_(&objects) {
    dead_.alive = false;
}

Item& CurrentMap::item(uint32_t id) {
    Item* it = id < 65536 ? objs_->get(uint16_t(id)) : nullptr;
    return it ? *it : dead_;
}

const Item& CurrentMap::item(uint32_t id) const {
    Item* it = id < 65536 ? objs_->get(uint16_t(id)) : nullptr;
    return it ? *it : dead_;
}

void CurrentMap::link(uint16_t id, bool at_front) {
    Item* it = objs_->get(id);
    if (!it || it->chunk >= 0) return;
    if (it->x < 0 || it->y < 0 || it->x >= kChunks * kChunkSize || it->y >= kChunks * kChunkSize) return;
    int c = (it->y / kChunkSize) * kChunks + it->x / kChunkSize;
    auto& list = chunks_[size_t(c)];
    if (at_front) list.insert(list.begin(), id);
    else list.push_back(id);
    it->chunk = c;
    it->ext_flags |= kExtInChunk;
}

void CurrentMap::unlink(uint16_t id) {
    Item* it = objs_->get(id);
    if (!it || it->chunk < 0) return;
    auto& list = chunks_[size_t(it->chunk)];
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == id) { list.erase(list.begin() + long(i)); break; }
    it->chunk = -1;
    it->ext_flags &= uint16_t(~kExtInChunk);
}

void CurrentMap::reset(unsigned map) {
    map_ = map;
    for (auto& c : chunks_) c.clear();
    std::fill(fast_.begin(), fast_.end(), false);
}

uint16_t CurrentMap::create_item(const ItemRecord& r, Item::Origin origin) {
    auto it = std::make_unique<Item>();
    it->shape = r.shape;
    it->frame = r.frame;
    it->x = r.x;
    it->y = r.y;
    it->z = r.z;
    it->flags = uint16_t(r.flags & ~kItemFastArea);  // stored FASTAREA must be cleared
    it->quality = r.quality;
    it->npcnum = r.npcnum;
    it->mapnum = r.mapnum;
    it->origin = origin;
    if (origin == Item::kFixed) it->ext_flags |= kExtFromFixed;
    return objs_->add_item(std::move(it));
}

std::vector<uint16_t> CurrentMap::create_tree(const std::vector<MapEntry>& tree, Item::Origin origin) {
    std::vector<uint16_t> ids(tree.size(), 0), top;
    for (size_t i = 0; i < tree.size(); ++i) {
        const MapEntry& e = tree[i];
        uint16_t id = create_item(e.rec, origin);
        ids[i] = id;
        if (!id) continue;
        if (e.parent < 0) {
            top.push_back(id);
            link(id);
        } else if (uint16_t pid = ids[size_t(e.parent)]) {
            Item* it = objs_->get(id);
            it->parent = pid;
            it->x = 0;  // the record's x is the nesting depth; y keeps the gump position
            it->z = 0;
            objs_->get(pid)->contents.push_back(id);
        }
    }
    return top;
}

void CurrentMap::load(unsigned map) {
    objs_->clear();
    reset(map);
    std::vector<ItemRecord> fixed = parse_item_records(data_.fixed.entry(map));
    apply_map_fixes(map, fixed);
    create_tree(build_item_tree(fixed, data_.typeflags), Item::kFixed);
    std::vector<ItemRecord> nonfixed = parse_item_records(data_.nonfixed.entry(map));
    map_patches::apply_moves(map, nonfixed);
    nonfixed_tree_ = build_item_tree(nonfixed, data_.typeflags);
    create_tree(nonfixed_tree_, Item::kNonFixed);

    for (const NpcRecord& n : data_.npcs) {
        if (n.mapnum != map) continue;
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
        a->dir = n.dir;
        if (objs_->add_at(uint16_t(n.number), std::move(a))) link(uint16_t(n.number));
    }
}

size_t CurrentMap::live_items() const { return objs_->live_count(); }

void CurrentMap::expand_glob(uint16_t egg) {
    const Item* e = objs_->get(egg);
    if (!e) return;
    const auto& glob = data_.globs.get(e->quality);
    int bx = e->x & ~0x1FF, by = e->y & ~0x1FF, ez = e->z;
    for (const GlobItem& g : glob) {
        auto it = std::make_unique<Item>();
        it->shape = g.shape;
        it->frame = g.frame;
        it->x = bx + 2 * g.x + 1;
        it->y = by + 2 * g.y + 1;
        it->z = ez + g.z;
        it->flags = kItemDisposable | kItemFastOnly;
        it->origin = Item::kGlob;
        if (uint16_t id = objs_->add_item(std::move(it))) link(id);
    }
}

void CurrentMap::enter_chunk(int cx, int cy) {
    auto& list = chunks_[size_t(cy) * kChunks + cx];
    // Items appended while walking (glob contents) are visited too, as in the chunk list.
    for (size_t i = 0; i < list.size(); ++i) {
        uint16_t id = list[i];
        Item* it = objs_->get(id);
        if (!it || (it->flags & kItemFastArea)) continue;
        if (data_.typeflags.get(it->shape).family() == kFamGlobEgg) expand_glob(id);
        it = objs_->get(id);
        if (hooks_ && it) hooks_->item_enter_fast(*it);
        if ((it = objs_->get(id))) it->flags |= kItemFastArea;
    }
}

void CurrentMap::leave_chunk(int cx, int cy) {
    std::vector<uint16_t> list = chunks_[size_t(cy) * kChunks + cx];
    for (uint16_t id : list) {
        Item* it = objs_->get(id);
        if (!it) continue;
        if (hooks_ && (it->flags & kItemFastArea)) hooks_->item_leave_fast(*it);
        if (!(it = objs_->get(id))) continue;
        it->flags &= uint16_t(~kItemFastArea);
        it->hatched = false;  // eggs re-arm
        if (it->flags & kItemFastOnly) {  // glob contents, spawned monsters: discarded
            unlink(id);
            if (hooks_) hooks_->destroy_item(id);
            else objs_->erase(id);
        }
    }
}

void CurrentMap::set_fast(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= kChunks || cy >= kChunks) return;
    size_t i = size_t(cy) * kChunks + cx;
    if (fast_[i]) return;
    enter_chunk(cx, cy);
    fast_[i] = true;
}

bool CurrentMap::point_is_fast(int x, int y) const {
    if (x < 0 || y < 0 || x >= kChunks * kChunkSize || y >= kChunks * kChunkSize) return false;
    return is_fast(x / kChunkSize, y / kChunkSize);
}

void CurrentMap::update_fast_area(const int prev[3], const int cur[3], int w, int h) {
    int xmin = std::min(prev[0], cur[0]), xmax = std::max(prev[0], cur[0]);
    int ymin = std::min(prev[1], cur[1]), ymax = std::max(prev[1], cur[1]);
    int zmin = std::min(prev[2], cur[2]), zmax = std::max(prev[2], cur[2]);
    // rendering.md 2.1: screen bounds with half a chunk of margin.
    int left = (xmin - ymin) / 4 - (w / 2 + 512 / 4);
    int top = (xmin + ymin) / 8 - zmax - (h / 2 + 512 / 8);
    int right = (xmax - ymax) / 4 + (w / 2 + 512 / 4);
    int bottom = (xmax + ymax) / 8 - zmin + (h / 2 + 512 / 8);
    int sxl = w / 256 + 3, syl = h / 128 + 7, xyl = (sxl + syl) / 2;
    int cx0 = std::max(0, xmin / 512 - xyl), cx1 = std::min(kChunks - 1, xmax / 512 + xyl);
    int cy0 = std::max(0, ymin / 512 - xyl), cy1 = std::min(kChunks - 1, ymax / 512 + xyl);

    std::vector<bool> now(kChunks * kChunks, false);
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            int scx = (cx * 512 - cy * 512) / 4, scy = (cx * 512 + cy * 512) / 8;
            int bl = scx - 128, br = scx + 128, bt = scy - 256, bb = scy + 128;
            bool outside = br <= left || bl >= right || bb <= top || bt >= bottom;
            now[size_t(cy) * kChunks + cx] = !outside;
        }
    for (int cy = 0; cy < kChunks; ++cy)
        for (int cx = 0; cx < kChunks; ++cx) {
            size_t i = size_t(cy) * kChunks + cx;
            if (fast_[i] && !now[i]) leave_chunk(cx, cy);
        }
    for (int cy = 0; cy < kChunks; ++cy)
        for (int cx = 0; cx < kChunks; ++cx) {
            size_t i = size_t(cy) * kChunks + cx;
            if (!fast_[i] && now[i]) enter_chunk(cx, cy);
        }
    fast_ = std::move(now);
}

int CurrentMap::roof_limit(const int p[3]) const {
    // Probe box: x P.x-64..P.x, y P.y-64..P.y, z P.z..P.z+16.
    int px0 = p[0] - 64, px1 = p[0], py0 = p[1] - 64, py1 = p[1];
    int best = 65536;
    int cx0 = std::max(0, px0 / 512 - 1), cx1 = std::min(kChunks - 1, px1 / 512 + 1);
    int cy0 = std::max(0, py0 / 512 - 1), cy1 = std::min(kChunks - 1, py1 / 512 + 1);
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
            for (uint16_t idx : chunk(cx, cy)) {
                const Item& it = item(idx);
                if (it.id == 1) continue;  // not the Avatar
                const ShapeInfo& si = data_.typeflags.get(it.shape);
                if (!si.is(ShapeInfo::kRoof)) continue;
                int xd, yd, zd;
                si.footprint(it.flags & kItemFlipped, xd, yd, zd);
                bool overlap = it.x - xd < px1 && it.x > px0 && it.y - yd < py1 && it.y > py0;
                if (!overlap || it.z < p[2] + 16) continue;
                best = std::min(best, it.z);
            }
    return best;
}

const Item* CurrentMap::find_item(unsigned shape, int quality, int x, int y, int range) const {
    for (size_t i = 0; i < objs_->slots(); ++i) {
        const Item* it = objs_->get(uint16_t(i));
        if (it && it->chunk >= 0 && it->shape == shape && (quality < 0 || it->quality == quality) &&
            std::abs(it->x - x) <= range && std::abs(it->y - y) <= range)
            return it;
    }
    return nullptr;
}

const Item* CurrentMap::npc(unsigned number) const {
    const Item* it = number < 256 ? objs_->get(uint16_t(number)) : nullptr;
    return it && it->chunk >= 0 ? it : nullptr;
}

}  // namespace pg
