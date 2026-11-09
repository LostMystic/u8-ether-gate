// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/item.h"

#include <cstdlib>

namespace pg {

void ObjectTable::clear() {
    objs_.clear();
    objs_.resize(32768);
    item_ids_.reset(512, 32766);
    actor_ids_.reset(1, 255);
    item_ids_.reserve(666);  // the Guardian's speaker id
    live_ = 0;
}

uint16_t ObjectTable::add_item(std::unique_ptr<Item> it) {
    uint16_t id = item_ids_.allocate();
    if (!id) return 0;
    it->id = id;
    objs_[id] = std::move(it);
    ++live_;
    return id;
}

uint16_t ObjectTable::add_actor(std::unique_ptr<Actor> a) {
    uint16_t id = actor_ids_.allocate();
    if (!id) return 0;
    a->id = id;
    objs_[id] = std::move(a);
    ++live_;
    return id;
}

bool ObjectTable::add_at(uint16_t id, std::unique_ptr<Item> it) {
    if (id == 0 || id >= objs_.size() || objs_[id]) return false;
    if (id < 256) actor_ids_.reserve(id);
    else item_ids_.reserve(id);
    it->id = id;
    objs_[id] = std::move(it);
    ++live_;
    return true;
}

void ObjectTable::erase(uint16_t id) {
    auto it = take(id);
    (void)it;
}

std::unique_ptr<Item> ObjectTable::take(uint16_t id) {
    if (id == 0 || id >= objs_.size() || !objs_[id]) return nullptr;
    std::unique_ptr<Item> out = std::move(objs_[id]);
    if (id < 256) actor_ids_.release(id);
    else if (id != 666) item_ids_.release(id);
    --live_;
    return out;
}

int direction_to(int dx, int dy) {
    // intrinsics/index.md "Coordinates and directions".
    if (dx == 0) return dy > 0 ? 4 : dy < 0 ? 0 : 1;
    long q = (1024L * dy) / dx;
    long aq = std::labs(q);
    if (aq <= 424) return dx > 0 ? 2 : 6;
    if (aq <= 2472) {
        if (dx > 0) return dy > 0 ? 3 : 1;
        return dy > 0 ? 5 : 7;
    }
    return dy > 0 ? 4 : 0;
}

}  // namespace pg
