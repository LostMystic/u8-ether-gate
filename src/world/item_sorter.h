// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace pg {

// The world draw-order sorter of specs/presentation/rendering.md section 3, kept in its
// own module so the rest of the renderer does not depend on how the order is found.
//
// Usage per frame: begin(); add() every drawable item in visiting order; paint(draw).
struct SortItem {
    uint32_t item = 0;   // caller's handle (index into the current map)
    uint16_t id = 0;     // object id, 0 = not a real item (for picking)
    uint16_t shape = 0, frame = 0;
    bool flipped = false;

    // World box: x/y/z = right, near, bottom corner; xLeft/yFar/zTop the opposite one.
    int32_t x = 0, y = 0, z = 0, xLeft = 0, yFar = 0, zTop = 0;
    // Screen points relative to the screen centre.
    int32_t sxLeft = 0, sxRight = 0, sxTop = 0, syTop = 0, sxBot = 0, syBot = 0;
    // Frame rectangle, half-open [frLeft, frRight) x [frTop, frBottom).
    int32_t frLeft = 0, frTop = 0, frRight = 0, frBottom = 0;

    bool flat = false, bigsq = false, xflat = false, yflat = false;
    bool occl = false, solid = false, draw = false, roof = false, land = false, fixed = false, noisy = false;
    bool trans = false, anim = false;

    // Sorter state.
    bool occluded = false;
    enum State : uint8_t { kUnpainted, kInProgress, kPainted } state = kUnpainted;
    int order = -1;
    std::vector<uint32_t> deps;  // indices into the sorter's item array
};

class ItemSorter {
public:
    void begin();
    // Adds an item whose box, screen points, frame rectangle and flags are filled in.
    void add(SortItem s);
    // Paints in dependency order, calling draw(item) for each visible item.
    void paint(const std::function<void(const SortItem&)>& draw);
    // Mouse picking (rendering.md 3.6): `opaque_at` checks the frame pixel. Returns the
    // sort item index, or -1.
    int pick(int px, int py, const std::function<bool(const SortItem&, int, int)>& opaque_at);

    const std::vector<SortItem>& items() const { return items_; }
    size_t painted_count() const { return painted_; }

    // The comparison rules, exposed for tests.
    static bool overlap(const SortItem& a, const SortItem& b);
    static bool occludes(const SortItem& a, const SortItem& b);
    static bool contains(const SortItem& a, int px, int py);
    static bool below(const SortItem& a, const SortItem& b);
    static bool list_less(const SortItem& a, const SortItem& b);

private:
    void paint_one(uint32_t i, const std::function<void(const SortItem&)>& draw);
    void insert_dep(SortItem& owner, uint32_t dep);

    std::vector<SortItem> items_;
    std::vector<uint32_t> list_;  // the main list
    int next_order_ = 0;
    size_t painted_ = 0;
};

}  // namespace pg
