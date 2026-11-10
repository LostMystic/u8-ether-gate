// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The usecode intrinsics (specs/intrinsics/*.md). Argument offsets follow the argument
// blocks of index.md: the first parameter is at offset 0, item "this" arguments are 4-byte
// pointers, other object arguments plain 16-bit ids.
#include <algorithm>
#include <cstdlib>
#include <functional>

#include "data/compat.h"
#include "core/bugfix.h"
#include "core/config.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "game/banish.h"
#include "game/grant_peace.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/world.h"
#include "game/world_stages.h"
#include "game/item_powers.h"
#include "game/servant.h"
#include "game/leveling.h"

namespace pg {

namespace {

int family(const World& w, const Item& it) { return int(w.data().typeflags.get(it.shape).family()); }

uint32_t weight(const World& w, const Item& it) {
    const ShapeInfo& si = w.data().typeflags.get(it.shape);
    switch (si.family()) {
    case kFamQuantity: return (uint32_t(it.quality) * si.weight() + 9) / 10;
    case kFamReagent: return uint32_t(it.quality) * si.weight();
    default: return (it.shape == 115 ? 30u : si.weight()) * 10;  // barrel: items.md weight note
    }
}

uint32_t total_weight(const World& w, const Item& it) {
    uint32_t t = weight(w, it);
    if (it.shape == 79) return t;  // the keyring counts only itself
    for (uint16_t k : it.contents)
        if (const Item* c = w.item(k)) t += total_weight(w, *c);
    return t;
}

uint32_t surface_weight(const World& w, const Item& it, int depth = 0) {
    if (depth > 16) return 0;
    std::vector<uint16_t> on;
    w.surface_search(it.id, true, on);
    uint32_t t = 0;
    for (uint16_t k : on)
        if (const Item* c = w.item(k)) {
            t += total_weight(w, *c);
            t += surface_weight(w, *c, depth + 1);
        }
    return t;
}

bool overlaps3(const World& w, const Item& a, const Item& b, bool xy_only) {
    int axd, ayd, azd, bxd, byd, bzd;
    w.footprint(a, axd, ayd, azd);
    w.footprint(b, bxd, byd, bzd);
    bool xy = a.x - axd < b.x && a.x > b.x - bxd && a.y - ayd < b.y && a.y > b.y - byd;
    if (xy_only) return xy;
    return xy && a.z < b.z + bzd && a.z + azd > b.z;
}

uint16_t equipped(const World& w, const Actor& a, unsigned slot) {
    for (uint16_t k : a.contents)
        if (const Item* it = w.item(k)) {
            if (!(it->flags & kItemEquipped)) continue;
            if (slot == 6 && it->shape == 529) return k;
            if (slot < 6 && w.data().typeflags.get(it->shape).equip_type() == slot + 1) return k;
        }
    return 0;
}

uint16_t hurl(World& w, uint16_t id, int vx, int vy, int vz, int g) { return w.hurl(id, vx, vy, vz, g); }

PaletteTransform solid(int r, int g, int b) {
    PaletteTransform t;
    t.m = {0, 0, 0, r * 2048 / 255, 0, 0, 0, g * 2048 / 255, 0, 0, 0, b * 2048 / 255};
    return t;
}

}  // namespace

// Intrinsic 0x0C8 (intrinsics/ui.md). Engine change (NOTES.md "Catacomb lever fireworks", with Original bug
// fixes on): the upside-down screen made clicks miss what is shown, worse when zoomed, so the lever sets
// off a burst of coloured sparkles over the Avatar instead and the screen stays the right way up.
uint16_t screen_invert_effect(World& w, bool invert) {
    if (!original_bug_fixes()) return w.kernel().add(std::make_unique<InvertProcess>(w, invert ? 0x8000 : 0));
    w.invert_state = 0;
    const Actor* av = w.avatar();
    if (!av) return 0;
    uint16_t last = 0;
    for (int k = 0; k < 24; ++k) {
        const int colour = w.rand_below(3) * 10;  // 587: 0-9 blue, 10-19 red, 20-29 yellow
        const int x = av->x - 64 + w.rand_below(129), y = av->y - 64 + w.rand_below(129);
        const int z = av->z + 40 + w.rand_below(60);
        last = w.kernel().add(std::make_unique<SpriteProcess>(w, 587, colour, colour + 9, 1 + w.rand_below(3),
                                                              1 + w.rand_below(2), x, y, z));
    }
    w.sound.play_sfx(54);
    w.sound.play_sfx(57);
    return last;
}

void register_intrinsics(World& w) {
    UCMachine& vm = w.vm();
    auto def = [&](unsigned n, const char* name, IntrinsicFn f) { vm.set_intrinsic(n, std::move(f), name); };
    auto I = [&w](IntrinsicCall& c, unsigned off = 0) -> Item* { return w.item(c.obj(off)); };
    auto A = [&w](IntrinsicCall& c, unsigned off = 0) -> Actor* { return w.actor(c.obj(off)); };
    auto loc = [&w](const Item& it, int& x, int& y, int& z) { w.location(it, x, y, z); };

    // --- targeting, barks, menus, readers (ui.md)
    def(0x000, "target_pick_item", [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t pid = w.target_pick();
        item_powers::on_target_pick(w, c.proc, pid);  // Item powers (game/item_powers.h)
        return pid;
    });
    def(0x049, "item_bark", [&w](IntrinsicCall& c) -> uint32_t {
        std::string text = c.str(4);
        item_powers::decorate_look(w, c.proc, text);  // Item powers: the cooldown when looked at
        if (uint16_t pid = w.skip_banter(c.proc, c.obj(0), text)) return pid;  // idle talk, 1 in 5 spoken
        return w.bark(c.obj(0), text);
    });
    def(0x04A, "item_ask", [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t pid = w.ask(c.u16(4), c.proc.item, c.proc.pid());
        if (pid) { w.talk_pid = c.proc.pid(); w.talk_item = c.proc.item; }  // the conversation (World::in_conversation)
        return pid;
    });
    def(0x04B, "item_get_slider", [&w](IntrinsicCall& c) -> uint32_t {
        // A modal slider; the calling process is suspended and woken with the value (ui.md).
        int mn = c.s16(4), mx = c.s16(6), step = c.s16(8);
        if (w.auto_ui) return uint16_t(mx);
        auto g = std::make_unique<SliderGump>(w, mn, mx, step, mn);
        g->wake_pid = c.proc.pid();
        w.gumps.add(std::move(g));
        c.proc.suspend();
        return uint16_t(mn);
    });
    def(0x04C, "item_open_gump", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) w.open_gump(it->id, c.u16(4));
        return 0;
    });
    def(0x04D, "item_close_gump", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) w.close_gump(it->id);
        return 0;
    });
    def(0x0B4, "close_item_gumps", [&w](IntrinsicCall&) -> uint32_t {
        w.gumps.close_item_dependent();
        for (size_t i = 1; i < w.objects().slots(); ++i)
            if (Item* it = w.item(uint16_t(i))) it->flags &= uint16_t(~kItemGumpOpen);
        return 0;
    });
    def(0x06E, "read_book", [&w](IntrinsicCall& c) -> uint32_t { return w.open_reader(ReaderGump::kBook, c.obj(0), 6, c.str(4)); });
    def(0x06F, "read_scroll", [&w](IntrinsicCall& c) -> uint32_t { return w.open_reader(ReaderGump::kScroll, c.obj(0), 19, c.str(4)); });
    def(0x070, "read_grave", [&w](IntrinsicCall& c) -> uint32_t { return w.open_reader(ReaderGump::kGrave, c.obj(0), c.u16(4), c.str(6)); });
    def(0x071, "read_plaque", [&w](IntrinsicCall& c) -> uint32_t { return w.open_reader(ReaderGump::kPlaque, c.obj(0), c.u16(4), c.str(6)); });

    // --- position and geometry (item.md)
    def(0x002, "item_touch", [](IntrinsicCall&) -> uint32_t { return 0; });
    def(0x003, "item_get_x", [I, loc](IntrinsicCall& c) -> uint32_t { Item* it = I(c); if (!it) return 0; int x, y, z; loc(*it, x, y, z); return uint16_t(x); });
    def(0x004, "item_get_y", [I, loc](IntrinsicCall& c) -> uint32_t { Item* it = I(c); if (!it) return 0; int x, y, z; loc(*it, x, y, z); return uint16_t(y); });
    def(0x005, "item_get_z", [I, loc](IntrinsicCall& c) -> uint32_t { Item* it = I(c); if (!it) return 0; int x, y, z; loc(*it, x, y, z); return uint16_t(z); });
    def(0x006, "item_get_center_x", [&w, I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int x, y, z, xd, yd, zd; loc(*it, x, y, z); w.footprint(*it, xd, yd, zd); return uint16_t(x - xd / 2);
    });
    def(0x007, "item_get_center_y", [&w, I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int x, y, z, xd, yd, zd; loc(*it, x, y, z); w.footprint(*it, xd, yd, zd); return uint16_t(y - yd / 2);
    });
    def(0x008, "item_get_center_z", [&w, I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int x, y, z, xd, yd, zd; loc(*it, x, y, z); w.footprint(*it, xd, yd, zd); return uint16_t(z + zd / 2);
    });
    def(0x00C, "item_get_point", [I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        int x, y, z; loc(*it, x, y, z);
        uint8_t b[5] = {uint8_t(x), uint8_t(x >> 8), uint8_t(y), uint8_t(y >> 8), uint8_t(z)};
        c.write_bytes(4, b, 5);
        return 0;
    });
    def(0x023, "item_get_footpad", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        const ShapeInfo& si = w.data().typeflags.get(it->shape);
        bool fl = it->flags & kItemFlipped;
        c.write16(4, uint16_t(fl ? si.y() : si.x()));
        c.write16(8, uint16_t(fl ? si.x() : si.y()));
        c.write16(12, uint16_t(si.z()));
        return 0;
    });
    def(0x025, "item_overlaps", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4)); return a && b && overlaps3(w, *a, *b, false);
    });
    def(0x026, "item_overlaps_xy", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4)); return a && b && overlaps3(w, *a, *b, true);
    });
    def(0x027, "item_is_on", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4));
        if (!a || !b) return 0;
        int bxd, byd, bzd; w.footprint(*b, bxd, byd, bzd);
        return overlaps3(w, *a, *b, true) && a->z == b->z + bzd;
    });
    def(0x055, "item_can_reach", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4));
        if (!a || !b) return 0;
        return w.can_reach(a->id, b->id, c.s16(6));  // distance + line of sight (physics.md 5)
    });
    def(0x056, "item_get_range", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4));
        if (!a || !b) return 0;
        int ax, ay, az, bx, by, bz, axd, ayd, azd, bxd, byd, bzd;
        w.location(*a, ax, ay, az); w.location(*b, bx, by, bz);
        w.footprint(*a, axd, ayd, azd); w.footprint(*b, bxd, byd, bzd);
        int r = std::max({(ax - axd) - bx, (bx - bxd) - ax, (ay - ayd) - by, (by - byd) - ay, 0});
        return uint16_t(r);
    });
    def(0x058, "item_dir_to_xy", [I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int x, y, z; loc(*it, x, y, z);
        return uint32_t(direction_to(c.u16(4) - x, c.u16(6) - y));
    });
    def(0x059, "item_dir_from_xy", [I, loc](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int x, y, z; loc(*it, x, y, z);
        return uint32_t(direction_to(x - c.u16(4), y - c.u16(6)));
    });
    def(0x05A, "item_dir_to_item", [&w, I, loc](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4)); if (!a || !b) return 0;
        int ax, ay, az, bx, by, bz; loc(*a, ax, ay, az); loc(*b, bx, by, bz);
        return uint32_t(direction_to(bx - ax, by - ay));
    });
    def(0x05B, "item_dir_from_item", [&w, I, loc](IntrinsicCall& c) -> uint32_t {
        Item* a = I(c); Item* b = w.item(c.u16(4)); if (!a || !b) return 0;
        int ax, ay, az, bx, by, bz; loc(*a, ax, ay, az); loc(*b, bx, by, bz);
        return uint32_t((direction_to(bx - ax, by - ay) + 4) & 7);
    });

    // --- shape, frame, quality, flags
    def(0x00D, "item_get_shape", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->shape : 0; });
    def(0x00E, "item_set_shape", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->shape = c.u16(4); return 0; });
    def(0x00F, "item_get_frame", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->frame : 0; });
    def(0x010, "item_set_frame", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->frame = c.u16(4); return 0; });
    def(0x011, "item_get_quality", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it && family(w, *it) == kFamQuality ? it->quality : 0; });
    def(0x012, "item_get_unkegg_type", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it && family(w, *it) == kFamUsecodeEgg ? it->quality : 0; });
    def(0x013, "item_get_quantity", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; int f = family(w, *it); return f == kFamQuantity || f == kFamReagent ? it->quality : 0;
    });
    def(0x015, "item_get_container", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->parent : 0; });
    def(0x016, "item_get_root_container", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it || !it->parent) return 0; return w.root_container(*it)->id;
    });
    def(0x019, "item_get_q", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->quality : 0; });
    def(0x01A, "item_set_q", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->quality = c.u16(4); return 0; });
    def(0x01B, "item_set_quality", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) if (family(w, *it) != kFamGeneric) it->quality = c.u16(4);
        return 0;
    });
    def(0x01D, "item_set_quantity", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) { int f = family(w, *it); if (f == kFamQuantity || f == kFamReagent) it->quality = c.u16(4); }
        return 0;
    });
    def(0x01E, "item_get_family", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? uint32_t(family(w, *it)) : 0; });
    def(0x01F, "item_get_type_flag", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; return w.data().typeflags.get(it->shape).bit(c.u16(4));
    });
    def(0x0D7, "shape_get_family", [&w](IntrinsicCall& c) -> uint32_t { return w.data().typeflags.get(c.u16(0)).family(); });
    def(0x03D, "item_is_explosive", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); return it && w.data().typeflags.get(it->shape).is(ShapeInfo::kExplode);
    });
    def(0x02C, "item_get_weight", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c);
        if (it && servant::on_weight_check(w, c.proc, it->id)) return 0;  // the fighting Aerial Servant (game/servant.h)
        return it ? uint16_t(weight(w, *it)) : 0;
    });
    def(0x02D, "item_get_total_weight", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? uint16_t(total_weight(w, *it)) : 0; });
    def(0x02E, "item_get_surface_weight", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? uint16_t(surface_weight(w, *it)) : 0; });
    def(0x020, "item_get_status", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->flags : 0; });
    def(0x021, "item_or_status", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->flags |= c.u16(4); return 0; });
    def(0x022, "item_and_status", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->flags &= c.u16(4); return 0; });
    def(0x050, "item_get_map_array", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->mapnum : 0; });
    def(0x052, "item_set_map_array", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->mapnum = uint8_t(c.u16(4)); return 0; });

    // --- containers and the ether
    def(0x03B, "cont_remove_contents", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        std::vector<uint16_t> k = it->contents;
        int x, y, z; w.location(*it, x, y, z);
        for (uint16_t id : k) {
            if (it->parent) w.move_to_container(id, it->parent);
            else w.move_to(id, x, y, z);
        }
        return 0;
    });
    def(0x03C, "cont_destroy_contents", [&w, I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) w.destroy_contents(it->id); return 0; });
    def(0x041, "item_legal_move_to_container", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; return w.move_to_container(it->id, c.obj(4), true);
    });
    def(0x032, "item_create_ethereal", [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t id = w.create_item(c.u16(4), c.u16(6));
        if (!id) return 0;
        w.push_ether(id);
        c.write16(0, id);
        return 1;
    });
    def(0x035, "item_push_ethereal", [&w, I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) w.push_ether(it->id); return 0; });
    def(0x0D1, "ether_peek", [&w](IntrinsicCall&) -> uint32_t { return w.ether_top(); });
    def(0x036, "ether_pop_to_xyz", [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t id = w.ether_pop();
        if (id) w.move_to(id, c.u16(4), c.u16(6), c.u16(8) & 0xFF);
        return id;
    });
    auto pop_to_container = [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t id = w.ether_pop();
        if (!id) return 0;
        Item* dest = w.item(c.u16(4));
        if (!dest) { w.destroy_now(id); return 0; }
        if (w.is_container(*dest) && w.move_to_container(id, dest->id)) return id;
        int x, y, z; w.location(*dest, x, y, z);
        w.move_to(id, x, y, z);
        return id;
    };
    def(0x037, "ether_pop_to_container", pop_to_container);
    def(0x039, "ether_pop_to_end", pop_to_container);
    def(0x038, "ether_pop_home", [&w](IntrinsicCall&) -> uint32_t {
        uint16_t id = w.ether_pop();
        Item* it = w.item(id);
        if (!it) return 0;
        if ((it->flags & (kItemContained | kItemEquipped)) && it->parent && w.item(it->parent)) {
            uint16_t parent = it->parent;
            bool eq = it->flags & kItemEquipped;
            int z = it->z;
            w.move_to_container(id, parent);
            if (eq) { it->flags = uint16_t((it->flags & ~kItemContained) | kItemEquipped); it->z = z; }
        } else {
            w.move_to(id, it->x, it->y, it->z);
        }
        return id;
    });

    // --- creation and destruction
    auto create_at = [&w](IntrinsicCall& c, int x, int y, int z) -> uint32_t {
        unsigned shape = c.u16(4), frame = c.u16(6);
        w.map().set_fast(x / 512, y / 512);
        if (!w.can_exist_shape(shape, x, y, z, 0).valid) return 0;
        uint16_t id = w.create_item(shape, frame);
        if (!id) return 0;
        w.move_to(id, x, y, z);
        c.write16(0, id);
        return 1;
    };
    def(0x031, "item_create_at_xyz", [create_at](IntrinsicCall& c) -> uint32_t {
        return create_at(c, c.u16(8), c.u16(10), c.u16(12) & 0xFF);
    });
    def(0x033, "item_create_at_point", [create_at, &vm](IntrinsicCall& c) -> uint32_t {
        uint8_t b[5] = {};
        if (!vm.read_ptr(c.u32(8), b, 5)) return 0;
        return create_at(c, b[0] | (b[1] << 8), b[2] | (b[3] << 8), b[4]);
    });
    def(0x034, "item_create_in_container", [&w](IntrinsicCall& c) -> uint32_t {
        c.write16(0, 0);
        uint16_t id = w.create_item(c.u16(4), c.u16(6));
        if (!id) return 0;
        if (!w.move_to_container(id, c.u16(8))) { w.destroy_now(id); return 0; }
        c.write16(0, id);
        return 1;
    });
    def(0x03A, "item_destroy", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) {
            on_item_destroyed(w, it->id, c.proc.cls, c.proc.ip, c.proc.item);  // Banish Daemon (game/banish.h)
            w.destroy(it->id);
        }
        return 0;
    });

    // --- movement and physics
    def(0x03E, "item_move_to_xyz", [&w, I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) w.move_to(it->id, c.u16(4), c.u16(6), c.u16(8) & 0xFF);
        return 0;
    });
    def(0x040, "item_legal_move_to_point", [&w, I, &vm](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c);
        uint8_t b[5] = {};
        if (!it || !vm.read_ptr(c.u32(4), b, 5)) return 0;
        int x = b[0] | (b[1] << 8), y = b[2] | (b[3] << 8), z = b[4];
        bool stop_if_blocked = c.u16(8) != 0;
        // physics.md 4: a blocking-only sweep first, then a (partial) collide move.
        Box start = w.box_of(*it);
        std::vector<SweepHit> hits;
        if (it->chunk >= 0) w.sweep(start, x, y, z, w.shape_flags(*it), it->id, true, hits);
        bool blocked = false;
        for (const SweepHit& h : hits)
            if (h.blocking && !h.touching && h.end_time > 0) { blocked = true; break; }
        if (blocked && stop_if_blocked) return 0;
        w.collide_move(it->id, x, y, z, false, false);
        return blocked ? 0 : 1;
    });
    def(0x02B, "item_ascend", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); return it ? w.ascend(it->id, c.s16(4)) : 0;
    });
    def(0x044, "item_hurl", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0; return hurl(w, it->id, c.s16(4), c.s16(6), c.s16(8), c.s16(10));
    });
    def(0x045, "item_shoot", [&w, I, &vm](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c);
        uint8_t b[5] = {};
        if (!it || !vm.read_ptr(c.u32(4), b, 5)) return 0;
        int tx = b[0] | (b[1] << 8), ty = b[2] | (b[3] << 8), tz = b[4];
        int speed = std::max(1, int(c.u16(8))), g = c.u16(10);
        int x, y, z; w.location(*it, x, y, z);
        int vx, vy, vz, n;
        w.missile_velocity(x, y, z, tx, ty, tz, speed, g, vx, vy, vz, n);  // physics.md 10
        w.hurl(it->id, vx, vy, vz, g);
        return 0;
    });
    def(0x046, "item_fall", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (it) w.fall(it->id); return 0;
    });
    def(0x047, "item_grab", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (it) w.grab(it->id); return 0;
    });
    def(0x053, "item_receive_hit", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        int damage = c.s16(8);
        // Leveling: Flame Bolt, Explosion, Conflagration and NPC casters (1411:233D) by the caster's
        // INT; traps run the same code with themselves as the caster and are not scaled.
        if (damage > 0 && w.leveling && w.leveling->active())
            if (uint16_t caster = Leveling::usecode_spell_caster(w, c.proc.start_cls, c.proc.start_addr, c.proc.stack.data(),
                                                                 UCProcess::kStackSize))
                if (const Actor* ca = w.actor(caster)) damage = Leveling::int_scaled(damage, ca->intel);
        w.receive_hit(it->id, c.u16(4), c.s16(6), damage, c.u16(10));  // combat.md 2
        return 0;
    });
    def(0x054, "item_explode", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (it) w.explode(it->id, true, true); return 0;
    });

    // --- item events from usecode
    def(0x05C, "event_look", [&w, I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? w.call_event(it->id, kEvLook) : 0; });
    def(0x05D, "event_use", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        if (Actor* a = it->as_actor(); a && a->is(Actor::kDead)) {
            if (w.gumps.container_gump(a->id)) w.close_gump(a->id); else w.open_gump(a->id, 12);
            return 0;
        }
        return w.call_event(it->id, kEvUse);
    });
    def(0x061, "event_got_hit", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); return it ? w.call_event_ww(it->id, kEvGotHit, c.u16(4), c.u16(6)) : 0;
    });
    def(0x067, "event_enter_fast_area", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); if (!it) return 0;
        if (it->flags & kItemFastArea) return 0;
        uint16_t before = uint16_t(w.events_started);
        uint16_t id = it->id;
        w.item_enter_fast(*it);
        if ((it = w.item(id))) it->flags |= kItemFastArea;
        (void)before;
        return 0;
    });
    def(0x06D, "event_guardian_bark", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); return it ? w.call_event_w(it->id, kEvGuardianBark, c.u16(4)) : 0;
    });

    // --- eggs (world.md)
    def(0x072, "egg_get_x_range", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->npcnum >> 4 : 0; });
    def(0x073, "egg_get_y_range", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->npcnum & 15 : 0; });
    def(0x074, "egg_set_x_range", [I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) it->npcnum = uint8_t((it->npcnum & 0x0F) | ((c.u16(4) & 15) << 4));
        return 0;
    });
    def(0x075, "egg_set_y_range", [I](IntrinsicCall& c) -> uint32_t {
        if (Item* it = I(c)) it->npcnum = uint8_t((it->npcnum & 0xF0) | (c.u16(4) & 15));
        return 0;
    });
    def(0x076, "egg_get_id", [I](IntrinsicCall& c) -> uint32_t { Item* it = I(c); return it ? it->mapnum : 0; });
    def(0x077, "egg_set_id", [I](IntrinsicCall& c) -> uint32_t { if (Item* it = I(c)) it->mapnum = uint8_t(c.u16(4)); return 0; });
    def(0x079, "monster_egg_hatch", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* egg = I(c);
        if (!egg || family(w, *egg) != kFamMonsterEgg) return 0;
        unsigned shape = egg->quality & 0x7FF;
        if (!shape) return 0;
        if (w.stages && w.stages->skip_hatch(*egg)) return 0;  // a safer heartland (game/world_stages.h)
        int ex = egg->x, ey = egg->y, ez = egg->z;
        unsigned activity = egg->mapnum & 7;
        uint16_t id = w.create_monster(shape, 0);
        Actor* a = w.actor(id);
        if (!a) return 0;
        a->flags |= kItemFastOnly | kItemDisposable | kItemInNpcList;
        if (!w.can_exist_shape(shape, ex, ey, ez, 0).valid) {
            PG_LOG("monster egg %u: shape %u cannot exist at %d,%d,%d", egg->id, shape, ex, ey, ez);
            w.destroy_now(id);
            return 0;
        }
        w.move_to(id, ex, ey, ez);
        PG_LOG("monster egg %u hatched %u (shape %u, activity %u) at %d,%d,%d", egg->id, id, shape, activity, ex, ey, ez);
        w.set_activity(id, activity);
        return id;
    });
    def(0x07A, "monster_egg_get_mon_id", [&w, I](IntrinsicCall& c) -> uint32_t {
        Item* it = I(c); return it && family(w, *it) == kFamMonsterEgg ? it->mapnum >> 3 : 0;
    });

    // --- actors (actor.md)
    def(0x042, "item_is_npc", [A](IntrinsicCall& c) -> uint32_t { return A(c) != nullptr; });
    def(0x080, "npc_is_busy", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); return a && w.kernel().count_exact(a->id, kProcActorAnim) > 0;
    });
    def(0x081, "npc_enemies_near", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a) return 0;
        std::vector<uint16_t> near;
        int x, y, z; w.location(*a, x, y, z);
        w.area_search(x, y, 0x800, false, near);
        for (uint16_t k : near)
            if (const Actor* o = w.actor(k); o && k != a->id && !o->is(Actor::kDead) && !o->is(Actor::kFeignDeath) &&
                                             o->is(Actor::kInCombat))
                return 1;
        return 0;
    });
    def(0x082, "npc_is_in_combat", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a && a->is(Actor::kInCombat); });
    def(0x083, "npc_set_in_combat", [&w, A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) w.set_in_combat(a->id); return 0; });
    def(0x084, "npc_clear_in_combat", [&w, A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) w.clear_in_combat(a->id); return 0; });
    def(0x085, "npc_set_target", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a) return 0;
        uint16_t id = a->id;
        w.set_in_combat(id);
        if ((a = w.actor(id))) a->target = c.u16(4);
        return 0;
    });
    def(0x086, "npc_get_target", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a && a->is(Actor::kInCombat) ? a->target : 0; });
    def(0x087, "npc_set_alignment", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->alignment = uint8_t(c.u16(4)); return 0; });
    def(0x088, "npc_get_alignment", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->alignment : 0; });
    def(0x089, "npc_set_enemy_alignment", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->enemy_alignment = uint8_t(c.u16(4)); return 0; });
    def(0x08A, "npc_get_enemy_alignment", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->enemy_alignment : 0; });
    def(0x08B, "npc_is_enemy", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); Actor* o = w.actor(c.u16(4)); return a && o && (a->enemy_alignment & o->alignment);
    });
    def(0x08C, "npc_is_dead", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return !a || a->is(Actor::kDead); });
    struct FlagDef { unsigned get, set, clr; uint32_t flag; const char* name; };
    const FlagDef flagdefs[] = {
        {0, 0x08D, 0x08E, Actor::kDead, "dead"},
        {0x08F, 0x090, 0x091, Actor::kImmortal, "immortal"},
        {0x092, 0x093, 0x094, Actor::kWithstandDeath, "withstand_death"},
        {0x095, 0, 0x097, Actor::kFeignDeath, "feign_death"},
    };
    for (const FlagDef& fd : flagdefs) {
        uint32_t f = fd.flag;
        if (fd.get) def(fd.get, "npc_is_flag", [A, f](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a && a->is(f); });
        if (fd.set) def(fd.set, "npc_set_flag", [A, f](IntrinsicCall& c) -> uint32_t {
            if (Actor* a = A(c)) { a->set(f, true); if (f == Actor::kImmortal) a->set(Actor::kInvincible, false); }
            return 0;
        });
        if (fd.clr) def(fd.clr, "npc_clear_flag", [A, f](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->set(f, false); return 0; });
    }
    def(0x096, "npc_set_feign_death", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a || a->is(Actor::kFeignDeath)) return 0;
        a->set(Actor::kFeignDeath, true);
        w.kernel().add(std::make_unique<ClearFeignProcess>(w, a->id));
        return 0;
    });
    def(0x09C, "npc_get_dir", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->dir : 0; });
    def(0x09D, "npc_get_map", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->mapnum : 0; });
    def(0x09E, "npc_teleport", [&w, A](IntrinsicCall& c) -> uint32_t {
        if (Actor* a = A(c)) w.teleport_actor(a->id, c.u16(4), c.u16(6), c.u16(8) & 0xFF, c.u16(10));
        return 0;
    });
    def(0x09F, "npc_do_anim", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a) return 0;
        int dir = c.u16(6);
        return w.do_anim(a->id, c.u16(4), dir >= 8 ? -1 : dir);
    });
    def(0x0A0, "npc_get_last_anim", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->last_anim : 0; });
    def(0x0A1, "npc_pathfind_to_xyz", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); return a ? w.pathfind(a->id, c.u16(4), c.u16(6), c.u16(8) & 0xFF, 0, 0) : 0;
    });
    def(0x0A2, "npc_pathfind_to_item", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a) return 0;
        uint16_t t = c.u16(4);
        if (!w.item(t)) return 0;
        return w.pathfind(a->id, 0, 0, 0, t, c.u16(6));
    });
    def(0x0A3, "npc_get_str", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? uint16_t(a->str) : 0; });
    def(0x0A4, "npc_get_int", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? uint16_t(a->intel) : 0; });
    def(0x0A5, "npc_get_dex", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? uint16_t(a->dex) : 0; });
    def(0x0A6, "npc_get_hp", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? a->hp : 0; });
    def(0x0A7, "npc_get_mana", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? uint16_t(a->mana) : 0; });
    def(0x0A8, "npc_set_str", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->str = c.s16(4); return 0; });
    def(0x0A9, "npc_set_int", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->intel = c.s16(4); return 0; });
    def(0x0AA, "npc_set_dex", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->dex = c.s16(4); return 0; });
    def(0x0AB, "npc_set_hp", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->hp = c.u16(4); return 0; });
    def(0x0AC, "npc_set_mana", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->mana = c.s16(4); return 0; });
    def(0x0AD, "npc_create", [&w](IntrinsicCall& c) -> uint32_t {
        uint16_t id = w.create_monster(c.u16(4), c.u16(6));
        if (!id) return 0;
        if (Actor* a = w.actor(id)) {
            a->mapnum = w.avatar() ? w.avatar()->mapnum : a->mapnum;
            // Original bug fix: the Ether spell Summon Creature (ETHRSPEL, class 1413, "Kal Xen")
            // is the one summoning script that never marks its creature fast-only (status 0x0080,
            // which every other summoning sets with 0x021). Such an actor gets no usecode events
            // at all (usecode/events.md: the original would run the wrong class) and is never
            // cleaned up when it leaves the fast area or the map.
            if (original_bug_fixes() && c.proc.cls == 1413 && compat::class_known(1413)) a->flags |= kItemFastOnly;
        }
        on_npc_created(w, id, c.proc.start_cls, c.proc.start_addr, c.proc.item);  // the Avatar's own daemon (game/banish.h)
        w.push_ether(id);
        c.write16(0, id);
        return id;
    });
    def(0x0AE, "npc_set_activity", [&w, A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? w.set_activity(a->id, c.u16(4)) : 0; });
    def(0x0AF, "npc_set_airwalk", [A](IntrinsicCall& c) -> uint32_t { if (Actor* a = A(c)) a->set(Actor::kAirWalk, c.u16(4) != 0); return 0; });
    def(0x0B0, "npc_get_airwalk", [A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a && a->is(Actor::kAirWalk); });
    def(0x0B1, "npc_schedule", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); if (!a || a->is(Actor::kDead)) return 0;
        uint32_t t = c.u32(4);
        return w.call_event(a->id, kEvSchedule, {uint8_t(t), uint8_t(t >> 8), uint8_t(t >> 16), uint8_t(t >> 24)});
    });
    def(0x0B2, "npc_get_equip", [&w, A](IntrinsicCall& c) -> uint32_t { Actor* a = A(c); return a ? equipped(w, *a, c.u16(4)) : 0; });
    def(0x0B3, "npc_set_equip", [&w, A](IntrinsicCall& c) -> uint32_t {
        Actor* a = A(c); Item* it = w.item(c.u16(6));
        if (!a || !it) return 0;
        unsigned et = w.data().typeflags.get(it->shape).equip_type();
        unsigned slot = it->shape == 529 ? 6 : et ? et - 1 : 99;
        if (slot == 99 || equipped(w, *a, slot)) return 0;
        if (w.party && (a->ext_flags & kExtCompanion) && !w.party->equip_refusal(a->id, it->id).empty()) return 0;  // gear class
        if (!w.move_to_container(it->id, a->id)) return 0;
        it->flags = uint16_t((it->flags & ~kItemContained) | kItemEquipped);
        it->z = int(slot + 1);
        return 1;
    });
    def(0x0CF, "avatar_get_stasis", [&w](IntrinsicCall&) -> uint32_t { return w.stasis; });
    def(0x0D0, "avatar_set_stasis", [&w, &vm](IntrinsicCall& c) -> uint32_t {
        bool on = c.s16(0) != 0;
        if (w.trace_events && on != w.stasis)
            PG_LOG("stasis %s by class %04X (%s) at %04X", on ? "on" : "off", c.proc.cls,
                   w.data().usecode.class_name(c.proc.cls).c_str(), c.proc.ip);
        if (on && !w.stasis) { w.stasis_pid = c.proc.pid(); w.stasis_cls = c.proc.cls; }
        w.stasis = on;
        // The Ancient Ones mutex (misc.md; NOTES.md "Original bug fixes": the scene run twice).
        if (original_bug_fixes() && c.proc.cls == 0x48B && compat::class_known(0x48B)) vm.set_global(0, 1, 1);
        return 0;
    });
    def(0x0E5, "avatar_feed", [&w](IntrinsicCall& c) -> uint32_t {
        auto* hp = dynamic_cast<HealProcess*>(w.kernel().find(0, kProcHeal));
        Actor* av = w.avatar();
        if (!hp || !av) return 0;
        unsigned food = c.u16(0), old = hp->hunger, eaten = std::min(food, old);
        if (!eaten) return 0;
        hp->hunger = old - eaten;
        av->hp = uint16_t(std::min<int>(2 * av->str, int(av->hp) + int(old / 4) - int(hp->hunger / 4)));
        return 0;
    });
    def(0x0E6, "avatar_train_str", [&w](IntrinsicCall& c) -> uint32_t { w.train_stat(0, c.s16(0)); return 0; });
    def(0x0E7, "avatar_train_int", [&w](IntrinsicCall& c) -> uint32_t { w.train_stat(1, c.s16(0)); return 0; });
    def(0x0E8, "avatar_train_dex", [&w](IntrinsicCall& c) -> uint32_t { w.train_stat(2, c.s16(0)); return 0; });
    def(0x0E9, "avatar_clear_combat", [&w](IntrinsicCall&) -> uint32_t { w.clear_in_combat(1); return 0; });
    def(0x0EA, "avatar_set_combat", [&w](IntrinsicCall&) -> uint32_t { w.set_in_combat(1); return 0; });
    def(0x0EB, "avatar_is_in_combat", [&w](IntrinsicCall&) -> uint32_t { return w.avatar() && w.avatar()->is(Actor::kInCombat); });
    def(0x0B8, "cast_grant_peace", [&w](IntrinsicCall&) -> uint32_t { return start_grant_peace(w); });  // magic.md 7.2

    // --- camera, quakes, teleporting (world.md)
    def(0x0BE, "camera_follow_item", [&w](IntrinsicCall& c) -> uint32_t { w.camera_follow(c.u16(0)); return 0; });
    def(0x0BF, "camera_move_to", [&w](IntrinsicCall& c) -> uint32_t { w.camera_stay(c.u16(0), c.u16(2), c.u16(4) & 0xFF); return 0; });
    def(0x0B5, "camera_scroll_to", [&w](IntrinsicCall& c) -> uint32_t { return w.camera_scroll(c.u16(0), c.u16(2), c.u16(4) & 0xFF, 25); });
    def(0x0C6, "quake_start", [&w](IntrinsicCall& c) -> uint32_t { w.quake = c.u16(0); return 0; });
    def(0x0C7, "quake_stop", [&w](IntrinsicCall&) -> uint32_t { w.quake = 0; return 0; });
    def(0x0CC, "teleport_to_egg", [&w](IntrinsicCall& c) -> uint32_t {
        return w.kernel().add(std::make_unique<TeleportProcess>(w, c.u16(0), c.u16(2)));
    });
    def(0x0D4, "can_exist_at", [&w](IntrinsicCall& c) -> uint32_t {
        // Milestone 6, Responsive mode: a door's own script asking whether the Avatar can stand
        // somewhere does not count the door itself (it is about to swing open, and open doors
        // have no collision then), so a door opens even when he stands in its swing.
        uint16_t ignore = 0;
        if (w.passable_door_rules() && c.u16(0) == 1 && w.is_door_shape(c.proc.cls)) ignore = c.proc.item;
        return w.can_exist_shape(c.u16(0), c.u16(2), c.u16(4), c.u16(6) & 0xFF, ignore).valid;
    });
    def(0x0BD, "ignite_chaos", [&w](IntrinsicCall& c) -> uint32_t {
        std::vector<uint16_t> near;
        w.area_search(c.u16(0), c.u16(2), 160, false, near);
        for (uint16_t k : near)
            if (const Item* it = w.item(k); it && it->shape == 592) w.call_event(k, kEvUse);
        return 0;
    });

    // --- time and randomness
    def(0x0D2, "timer_get_tick", [&w](IntrinsicCall&) -> uint32_t { return w.kernel().tick(); });
    def(0x0D8, "time_get_hours", [&w](IntrinsicCall&) -> uint32_t { return uint16_t(w.game_seconds() / 900); });
    def(0x0D9, "time_get_minutes", [&w](IntrinsicCall&) -> uint32_t { return w.game_seconds() / 60; });
    def(0x0DA, "time_get_seconds", [&w](IntrinsicCall&) -> uint32_t { return w.game_seconds(); });
    def(0x0DB, "time_set_hours", [&w](IntrinsicCall& c) -> uint32_t { w.set_game_hours(c.u16(0)); return 0; });
    def(0x0B6, "rand_below", [&w](IntrinsicCall& c) -> uint32_t { return uint16_t(w.rand_below(c.u16(0))); });
    def(0x0B7, "rand_range", [&w](IntrinsicCall& c) -> uint32_t {
        int lo = c.s16(0), hi = c.s16(2);
        if (hi <= lo) return uint16_t(lo);
        return uint16_t(lo + w.rand_below(hi - lo + 1));
    });

    // --- sprites
    def(0x0D6, "sprite_create", [&w](IntrinsicCall& c) -> uint32_t {
        return w.kernel().add(std::make_unique<SpriteProcess>(w, c.u16(0), c.s16(2), c.s16(4), 1, c.s16(6), c.u16(8),
                                                               c.u16(10), c.u16(12) & 0xFF));
    });
    def(0x0D5, "sprite_create_ex", [&w](IntrinsicCall& c) -> uint32_t {
        return w.kernel().add(std::make_unique<SpriteProcess>(w, c.u16(0), c.s16(2), c.s16(4), c.s16(8), c.s16(10),
                                                               c.u16(12), c.u16(14), c.u16(16) & 0xFF));
    });
    def(0x0FB, "tonys_balls", [](IntrinsicCall&) -> uint32_t { return 0; });  // fireball trap: milestone 3

    // --- palette and screen effects (ui.md)
    def(0x0DE, "fade_to_black", [&w](IntrinsicCall&) -> uint32_t { return w.start_fade(solid(0, 0, 0), 30, 0x7FFF); });
    def(0x0DF, "fade_from_black", [&w](IntrinsicCall&) -> uint32_t {
        PaletteTransform from = solid(0, 0, 0);
        return w.start_fade(w.palette_target, 30, 0x7FFF, &from);
    });
    def(0x0E2, "fade_to_white", [&w](IntrinsicCall&) -> uint32_t { return w.start_fade(solid(255, 255, 255), 30, 0x7FFF); });
    def(0x0E3, "fade_from_white", [&w](IntrinsicCall&) -> uint32_t {
        PaletteTransform from = solid(255, 255, 255);
        return w.start_fade(w.palette_target, 30, 0x7FFF, &from);
    });
    def(0x0E0, "fade_to_transform", [&w](IntrinsicCall& c) -> uint32_t {
        unsigned t = c.u16(0);
        w.palette_xform = t;
        w.palette_target = PaletteTransform::preset(int(t));
        if (t == 3 && w.stages) w.palette_target = w.stages->storm_transform();  // milder near the towns (world_stages.h)
        return w.start_fade(w.palette_target, 45, int16_t(c.u16(2)));
    });
    def(0x0E1, "lightning_flash", [&w](IntrinsicCall& c) -> uint32_t {
        if (w.stages && w.stages->skip_storm_flash(c.proc.cls)) return 0;  // fewer storm flashes (world_stages.h)
        PaletteTransform from;
        int d = 0x3F * 2048 / 255, off = 0xCF * 2048 / 255;
        from.m = {d, 0, 0, off, 0, d, 0, off, 0, 0, d, off};
        return w.start_fade(w.palette_target, 10, -1, &from);
    });
    def(0x0C8, "screen_invert", [&w](IntrinsicCall& c) -> uint32_t { return screen_invert_effect(w, c.u16(0) != 0); });
    def(0x0E4, "play_endgame", [&w](IntrinsicCall&) -> uint32_t {
        // presentation/movies.md 3: unlock End Game and Quotes, reset the palette, play.
        Config& cfg = Config::global();
        cfg.set_bool("endgame_seen", true);
        cfg.set_bool("quotes_seen", true);
        if (!cfg.path().empty()) cfg.save();
        w.palette = w.palette_target = PaletteTransform{};
        w.play_movie(World::kMovieEndgame, false);
        return 0;
    });

    // --- audio (intrinsics/audio.md)
    def(0x0BB, "music_play", [&w](IntrinsicCall& c) -> uint32_t { w.sound.music_play(int(c.u16(0))); return 0; });
    def(0x0F8, "music_stop", [&w](IntrinsicCall&) -> uint32_t { w.sound.music_stop(); return 0; });
    def(0x0EC, "sfx_play", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_sfx(c.s16(0)); return 0; });
    def(0x0ED, "sfx_play_pri", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_sfx(c.s16(0), c.u16(2) & 0xFF); return 0; });
    def(0x0EE, "sfx_play_at", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_sfx(c.s16(0), c.u16(2), c.u16(4)); return 0; });
    def(0x0EF, "sfx_loop", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_ambient(c.s16(0), SoundSystem::kDefaultPriority, 0); return 0; });
    def(0x0F0, "sfx_loop_pri", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_ambient(c.s16(0), c.u16(2) & 0xFF, 0); return 0; });
    def(0x0F1, "sfx_loop_at", [&w](IntrinsicCall& c) -> uint32_t { w.sound.play_ambient(c.s16(0), c.u16(2), c.u16(4)); return 0; });
    def(0x0F2, "sfx_is_playing", [&w](IntrinsicCall& c) -> uint32_t { return w.sound.is_playing(c.s16(0)) ? 1 : 0; });
    def(0x0F3, "sfx_set_volume", [&w](IntrinsicCall& c) -> uint32_t { w.sound.set_sfx_volume(c.s16(0), c.u16(2) & 0xFF); return 0; });
    def(0x0F4, "sfx_stop", [&w](IntrinsicCall& c) -> uint32_t { w.sound.stop_sfx(c.s16(0)); return 0; });
    def(0x0F5, "sfx_stop_at", [&w](IntrinsicCall& c) -> uint32_t { w.sound.stop_sfx_on(c.s16(0), c.u16(2)); return 0; });
    def(0x0F6, "sound_init", [](IntrinsicCall&) -> uint32_t { return 0; });
    def(0x0F7, "sound_deinit", [](IntrinsicCall&) -> uint32_t { return 0; });
    def(0x0F9, "music_stop_slow", [&w](IntrinsicCall&) -> uint32_t { w.sound.music_slow_stop(); return 0; });
    def(0x0FA, "music_play_direct", [&w](IntrinsicCall& c) -> uint32_t { w.sound.music_direct(int(c.u16(0)) & 0x7F); return 0; });

    // --- process control, strings, misc (misc.md)
    def(0x0CA, "proc_count", [&w](IntrinsicCall& c) -> uint32_t { return w.kernel().count(c.u16(0), c.u16(2)); });
    def(0x0CB, "proc_kill", [&w](IntrinsicCall& c) -> uint32_t { w.kernel().kill(c.u16(0), c.u16(2)); return 0; });
    def(0x0CD, "proc_kill_2", [&w](IntrinsicCall& c) -> uint32_t { w.kernel().kill(c.u16(0), c.u16(2)); return 0; });
    def(0x0B9, "num_to_string", [&vm](IntrinsicCall& c) -> uint32_t { return vm.new_string(std::to_string(c.s16(0))); });
    def(0x0BC, "get_avatar_name", [&w, &vm](IntrinsicCall&) -> uint32_t { return vm.new_string(w.avatar_name); });
    def(0x0FC, "cheats_enabled", [&w](IntrinsicCall&) -> uint32_t { return w.cheats; });
    def(0x0FD, "mark_cheater", [&w](IntrinsicCall&) -> uint32_t { w.cheated = true; return 0; });
    def(0x0FE, "is_game_running", [](IntrinsicCall&) -> uint32_t { return 1; });
}

}  // namespace pg
