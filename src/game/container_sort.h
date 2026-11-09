// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// "Sort" for container windows (the engine's own feature, not in the original): merge
// stackable items, group them by kind, and lay them out so every item stays visible and
// clickable. See NOTES.md "Container sort".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

class World;
class Item;
class ContainerGump;
struct GameData;
struct ShapeFrame;

// Item groups, in the order a sorted container shows them.
enum class SortCategory : uint8_t {
    kWeapon, kArmour, kReagent, kMagic, kPotion, kKey, kFood, kMoney, kBook, kContainer, kMisc, kCount
};
const char* sort_category_name(SortCategory c);
// The group of a shape, derived from TYPEFLAG.DAT (equip type, family) and the shape's usecode
// class name in EUSECODE.FLX (see container_sort.cpp for the rules).
SortCategory sort_category(const GameData& d, unsigned shape);

// The engine's stack-merge rule (world/items.md "Stacks": same shape, quantity family, or
// reagent family with equal frames / frame groups), plus: both or neither OWNED, so a sort
// never mixes stolen goods into the player's own stack.
bool sort_can_merge(const World& w, const Item& a, const Item& b);
// A stack of the quantity or reagent family.
bool sort_is_stack(const World& w, const Item& it);
constexpr unsigned kMaxStack = 666;

// One item to lay out: its drawn frame (flipped = mirrored) in item-area pixels.
struct SortBox {
    const ShapeFrame* frame = nullptr;
    bool flipped = false;
    int w = 0, h = 0;      // bounding box of the drawn frame
    int left_off = 0;      // bounding box left = anchor x + left_off
    int top_off = 0;       // bounding box top  = anchor y + top_off
    bool opaque(int bx, int by) const;  // pixel of the bounding box
};
SortBox sort_box(const ShapeFrame* f, bool flipped);

struct SortLayout {
    enum Mode { kShelf, kFirstFit, kDense, kOverlap };
    Mode mode = kShelf;
    int gap = 0;
    // Per input item: the bounding box's top-left inside the item area.
    std::vector<int> left, top;
    // Back-to-front drawing order (indices into the input).
    std::vector<size_t> order;
    // Overlap mode only: items partly covered, and the smallest visible part of any item
    // (fraction of its opaque pixels that clicks reach).
    int covered = 0;
    double min_visible = 1.0;
    bool all_clickable = true;  // every item keeps >= 50% and a clear 8x8 (or smaller) patch
};
const char* sort_mode_name(SortLayout::Mode m);

// Pure layout of boxes (in the wanted reading order) inside an area of area_w x area_h:
// shelf rows with a 2/1 pixel gap, else first-fit in reading order (gap 1/0), else first-fit
// largest first, else the minimal-overlap fallback (largest at the back).
SortLayout sort_layout(const std::vector<SortBox>& items, int area_w, int area_h);

// What a sort did.
struct SortResult {
    bool ok = false;
    std::string refused;        // why not (stasis, out of reach, no window)
    int items = 0;              // items laid out
    int merged_away = 0;        // stacks emptied into others
    SortLayout::Mode mode = SortLayout::kShelf;
    int covered = 0;
    bool all_clickable = true;
};

// Sorts the container shown in gump g: merges stacks (the kept stack gets the "combine" event
// the engine fires on every merge, so coins pick their pile frame), groups and lays out the
// contents, and puts them in drawing order. No other usecode events (the items do not move
// in the world). The layout runs again once those combine events have ended, in case a
// frame changed (ContainerGump::tick).
SortResult sort_container(World& w, ContainerGump& g);
// The same for a container id whose window is open (false if none).
SortResult sort_container(World& w, uint16_t container);
// The window a Sort key / button acts on: the sortable container window under screen point
// (sx, sy), else the topmost open one. nullptr if none.
ContainerGump* sort_target(World& w, int sx, int sy);
// Re-lays out g's contents in their current order without merging (after combine events).
void sort_relayout(World& w, ContainerGump& g);

}  // namespace pg
