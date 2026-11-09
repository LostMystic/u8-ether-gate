// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Weight, volume and carrying (specs/world/items.md 4-5, specs/world/avatar.md 8.5-8.6).
#include <algorithm>
#include <functional>

#include "core/log.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "game/virtues.h"
#include "game/world.h"

namespace pg {

uint32_t World::item_weight(const Item& it) const {
    const ShapeInfo& si = data_.typeflags.get(it.shape);
    switch (si.family()) {
    case kFamQuantity: return (uint32_t(it.quality) * si.weight() + 9) / 10;
    case kFamReagent: return uint32_t(it.quality) * si.weight();
    default: return si.weight() * 10u;
    }
}

uint32_t World::total_weight(const Item& it) const {
    uint32_t t = item_weight(it);
    if (it.shape == 79) return t;  // the keyring counts only itself
    for (uint16_t k : it.contents)
        if (const Item* c = item(k)) t += total_weight(*c);
    return t;
}

unsigned World::item_volume(const Item& it) const {
    const ShapeInfo& si = data_.typeflags.get(it.shape);
    if (it.flags & kItemInvisible) return 0;
    switch (si.family()) {
    case kFamQuantity: return (it.quality * si.volume() + 99) / 100;
    case kFamReagent: return (it.quality * si.volume() + 9) / 10;
    case kFamContainer: return std::max(1u, si.volume());
    default: return si.volume();
    }
}

bool World::give_to_avatar(uint16_t id) {
    Actor* av = avatar();
    Item* it = item(id);
    if (!av || !it || id < 256) return false;
    uint16_t pack = equipped(1, 7);
    if (!pack) return false;
    if (root_container(*it)->id != 1 && total_weight(*av) + total_weight(*it) > uint32_t(40 * std::max<int>(0, av->str)))
        return false;
    return move_to_container(id, pack, true);
}

void World::theft_check(uint16_t id) {
    const Item* it = item(id);
    const Actor* av = avatar();
    if (!it || !av || (it->flags & kItemOwned)) return;
    if (party && party->enabled) party->on_avatar_theft();  // companions react (game/party.h)
    if (virtues && virtues->enabled) {
        // Infamy (game/virtues.h): only in company, like the companions' reaction: a townsperson nearby.
        std::vector<uint16_t> near;
        area_search(av->x, av->y, 512, false, near);
        for (uint16_t k : near)
            if (const Actor* o = actor(k); o && k != 1 && (o->ext_flags & kExtPermanentNpc) && !o->is(Actor::kDead) &&
                                           !(party && party->party_member(k))) {
                virtues->on_theft(id);
                break;
            }
    }
    Box b = box_of(*av);
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx) {
            if (cx * 512 > b.x + 640 + 512 || (cx + 1) * 512 < b.x - b.xd - 640) continue;
            if (cy * 512 > b.y + 640 + 512 || (cy + 1) * 512 < b.y - b.yd - 640) continue;
            std::vector<uint16_t> ids = map_.chunk(cx, cy);
            for (uint16_t k : ids) {
                const Actor* a = actor(k);
                if (!a || k == 1 || a->is(Actor::kDead)) continue;
                if (a->x <= b.x - b.xd - 640 || a->x > b.x + 640 || a->y <= b.y - b.yd - 640 || a->y > b.y + 640) continue;
                call_event_w(k, kEvStole, id);
            }
        }
}

}  // namespace pg
