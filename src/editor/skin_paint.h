// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// The custom actor painter (NOTES.md "Custom shapes"): repaints every frame of a base actor
// shape (the Avatar, shape 1) as another character, keeping each frame's size and anchor so
// ANIM.DAT, sorting and collision behave exactly as for the base.
//
// Per frame: the base pixels are sorted into materials by palette colour (outline, steel / mail,
// tunic, gold, leather, magic light), then into body parts by connected regions and geometry
// (helmet = head, mail = skin of arms / neck / legs, the big leather blob on the back = backpack,
// leather below the tunic = thighs, small leather ends of the arms = hands). Key points are
// tracked (head box, neck, waist, hem, hips, feet) and a new figure is painted over them: a face
// and long hair from the head box and the facing direction, the tunic turned into a bodice and a
// skirt that runs from the waist to mid-calf between and over the legs, bare arms and legs in
// the skin ramp, slippers, and the backpack removed. Shading follows the base art's own light
// (light from the upper left) mapped onto the new colour ramps, with ordered dithering where
// painted areas need a blend, and a dark outline on the new silhouette.
#include <cstdint>
#include <string>
#include <vector>

#include "data/shape.h"

namespace pg {

struct GameData;

// What is known about one frame from the base shape's animations.
struct SkinFrameInfo {
    int facing = 4;        // 0..7 (anim.md directions), mirrored frames already resolved
    bool used = false;     // referenced by some action
    bool posed = false;    // not upright (falling, lying, getting up): head found by tracking
    int prev = -1;         // the frame before it in a sequence (for tracking), -1 none
    bool engulfed = false; // burning (action 57) past its first two frames: only fire and smoke show
    std::vector<unsigned> actions;
};

// Debug key points of a painted frame (frame coordinates).
struct SkinKeys {
    int head_x0 = 0, head_y0 = 0, head_x1 = -1, head_y1 = -1;  // head box (inclusive)
    int waist_y = -1, hem_y = -1, foot_y = -1, hip_x = 0;
    bool upright = true;
    std::vector<uint8_t> roles;  // per pixel body part (debug views)
};

class SkinPainter {
public:
    explicit SkinPainter(const GameData& d, unsigned base_shape = 1);
    unsigned frame_count() const { return unsigned(info_.size()); }
    const SkinFrameInfo& info(unsigned f) const { return info_[f]; }
    // Paints frame f; the result has the base frame's size and anchor.
    ShapeFrame paint(unsigned f, SkinKeys* keys = nullptr);
    // All frames, in order (frames that cannot be decoded become 1 x 1 transparent).
    std::vector<ShapeFrame> paint_all();

private:
    const GameData& d_;
    unsigned base_;
    std::vector<SkinFrameInfo> info_;
    std::vector<SkinKeys> keys_cache_;
    std::vector<bool> keys_done_;
};

}  // namespace pg
