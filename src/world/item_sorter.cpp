// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "world/item_sorter.h"

namespace pg {

void ItemSorter::begin() {
    items_.clear();
    list_.clear();
    next_order_ = 0;
    painted_ = 0;
}

bool ItemSorter::list_less(const SortItem& a, const SortItem& b) {
    if (a.z != b.z) return a.z < b.z;
    return a.flat && !b.flat;
}

bool ItemSorter::overlap(const SortItem& a, const SortItem& b) {
    if (a.frRight <= b.frLeft || b.frRight <= a.frLeft || a.frBottom <= b.frTop || b.frBottom <= a.frTop)
        return false;
    int dtx = a.sxTop - b.sxBot, dty = a.syTop - b.syBot;
    int dbx = a.sxBot - b.sxTop, dby = a.syBot - b.syTop;
    if (a.sxRight <= b.sxLeft) return false;
    if (a.sxLeft >= b.sxRight) return false;
    if (dtx + 2 * dty >= 0) return false;
    if (-dtx + 2 * dty >= 0) return false;
    if (dbx - 2 * dby >= 0) return false;
    if (-dbx - 2 * dby >= 0) return false;
    return true;
}

bool ItemSorter::occludes(const SortItem& a, const SortItem& b) {
    if (!(a.frLeft <= b.frLeft && a.frTop <= b.frTop && a.frRight >= b.frRight && a.frBottom >= b.frBottom))
        return false;
    int dtx = a.sxTop - b.sxTop, dty = a.syTop - b.syTop;
    int dbx = a.sxBot - b.sxBot, dby = a.syBot - b.syBot;
    return a.sxRight >= b.sxRight && a.sxLeft <= b.sxLeft && dtx + 2 * dty <= 0 && -dtx + 2 * dty <= 0 &&
           dbx - 2 * dby <= 0 && -dbx - 2 * dby <= 0;
}

bool ItemSorter::contains(const SortItem& a, int px, int py) {
    if (px < a.frLeft || px >= a.frRight || py < a.frTop || py >= a.frBottom) return false;
    int dtx = a.sxTop - px, dty = a.syTop - py;
    int dbx = a.sxBot - px, dby = a.syBot - py;
    if (a.sxRight < px || a.sxLeft > px) return false;
    if (dtx + 2 * dty > 0 || -dtx + 2 * dty > 0) return false;
    if (dbx - 2 * dby > 0 || -dbx - 2 * dby > 0) return false;
    return true;
}

// rendering.md 3.3: true if a must be painted before b. First deciding rule wins.
bool ItemSorter::below(const SortItem& a, const SortItem& b) {
    // 1. Separated in z.
    if (a.flat && b.flat) {
        if (a.z != b.z) return a.z < b.z;
    } else {
        if (a.zTop <= b.z) return true;
        if (a.z >= b.zTop) return false;
    }
    // 2. Separated in y.
    if (a.yflat && b.yflat) {
        if (a.y / 32 != b.y / 32) return a.y < b.y;
    } else {
        if (a.y <= b.yFar) return true;
        if (a.yFar >= b.y) return false;
    }
    // 3. Separated in x.
    if (a.xflat && b.xflat) {
        if (a.x / 32 != b.x / 32) return a.x < b.x;
    } else {
        if (a.x <= b.xLeft) return true;
        if (a.xLeft >= b.x) return false;
    }
    // 4. z with an 8-unit tolerance.
    if (a.zTop - 8 <= b.z && a.z < b.zTop - 8) return true;
    if (a.z >= b.zTop - 8 && a.zTop - 8 > b.z) return false;
    // 5. Exactly one is yflat: compare in tile units.
    if (a.yflat != b.yflat) {
        if (a.y / 32 <= b.yFar / 32) return true;
        if (a.yFar / 32 >= b.y / 32) return false;
        int ca = (a.yFar / 32 + a.y / 32) / 2, cb = (b.yFar / 32 + b.y / 32) / 2;
        if (ca != cb) return ca < cb;
    }
    // 6. Exactly one is xflat.
    if (a.xflat != b.xflat) {
        if (a.x / 32 <= b.xLeft / 32) return true;
        if (a.xLeft / 32 >= b.x / 32) return false;
        int ca = (a.xLeft / 32 + a.x / 32) / 2, cb = (b.xLeft / 32 + b.x / 32) / 2;
        if (ca != cb) return ca < cb;
    }
    // 7. At least one is flat.
    if (a.flat || b.flat) {
        if (a.z != b.z) return a.z < b.z;
        if (a.flat != b.flat) return a.flat;
        if (a.trans != b.trans) return b.trans;
        if (a.anim != b.anim) return b.anim;
        if (a.draw != b.draw) return a.draw;
        if (a.solid != b.solid) return a.solid;
        if (a.occl != b.occl) return a.occl;
        if (a.bigsq != b.bigsq) return a.bigsq;
    }
    // 8. Same position: the translucent one later.
    if (a.x == b.x && a.y == b.y && a.trans != b.trans) return b.trans;
    // 9. Land before roof.
    if (a.land && b.land && a.roof != b.roof) return !a.roof;
    // 10. Roof first.
    if (a.roof != b.roof) return a.roof;
    // 11.
    if (a.z != b.z) return a.z < b.z;
    // 12. Thin items.
    if (a.xflat || a.yflat || b.xflat || b.yflat) {
        if (a.sxLeft != b.sxLeft) return a.sxLeft > b.sxLeft;
        if (a.syBot != b.syBot) return a.syBot < b.syBot;
    }
    // 13-18.
    if (a.x + a.y != b.x + b.y) return a.x + a.y < b.x + b.y;
    if (a.xLeft + a.yFar != b.xLeft + b.yFar) return a.xLeft + a.yFar < b.xLeft + b.yFar;
    if (a.y != b.y) return a.y < b.y;
    if (a.x != b.x) return a.x < b.x;
    if (a.shape != b.shape) return a.shape < b.shape;
    return a.frame < b.frame;
}

void ItemSorter::insert_dep(SortItem& owner, uint32_t dep) {
    auto& d = owner.deps;
    const SortItem& n = items_[dep];
    size_t pos = d.size();
    for (size_t i = 0; i < d.size(); ++i)
        if (list_less(n, items_[d[i]])) { pos = i; break; }
    d.insert(d.begin() + long(pos), dep);
}

void ItemSorter::add(SortItem s) {
    s.xflat = s.x == s.xLeft;
    s.yflat = s.y == s.yFar;
    s.occluded = false;
    s.state = SortItem::kUnpainted;
    s.order = -1;
    s.deps.clear();
    uint32_t ni = uint32_t(items_.size());
    items_.push_back(std::move(s));

    size_t insert_at = list_.size();
    bool have_insert = false;
    for (size_t li = 0; li < list_.size(); ++li) {
        uint32_t ei = list_[li];
        SortItem& n = items_[ni];
        SortItem& e = items_[ei];
        if (!have_insert && list_less(n, e)) { insert_at = li; have_insert = true; }
        if (e.occluded) continue;
        if (!overlap(n, e)) continue;
        if (below(n, e)) {
            if (e.occl && occludes(e, n)) {
                n.occluded = true;
                break;
            }
            insert_dep(e, ni);
        } else {
            if (n.occl && occludes(n, e)) e.occluded = true;
            else insert_dep(n, ei);
        }
    }
    list_.insert(list_.begin() + long(insert_at), ni);
}

void ItemSorter::paint_one(uint32_t i, const std::function<void(const SortItem&)>& draw) {
    SortItem& s = items_[i];
    if (s.occluded) return;
    s.state = SortItem::kInProgress;
    for (size_t k = 0; k < items_[i].deps.size(); ++k) {
        uint32_t d = items_[i].deps[k];
        if (items_[d].state == SortItem::kInProgress) break;  // cycle: stop, paint this one
        if (items_[d].state == SortItem::kUnpainted) paint_one(d, draw);
    }
    SortItem& t = items_[i];
    if (draw) draw(t);
    t.state = SortItem::kPainted;
    t.order = next_order_++;
    ++painted_;
}

void ItemSorter::paint(const std::function<void(const SortItem&)>& draw) {
    for (uint32_t i : list_)
        if (items_[i].state == SortItem::kUnpainted) paint_one(i, draw);
}

int ItemSorter::pick(int px, int py, const std::function<bool(const SortItem&, int, int)>& opaque_at) {
    if (painted_ == 0 && !items_.empty()) paint(nullptr);
    int best = -1, best_order = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const SortItem& s = items_[i];
        if (s.id == 0 || s.order < 0) continue;
        if (s.trans && !s.solid) continue;
        if (!contains(s, px, py)) continue;
        if (!opaque_at(s, px, py)) continue;
        if (s.order > best_order) { best_order = s.order; best = int(i); }
    }
    return best;
}

}  // namespace pg
