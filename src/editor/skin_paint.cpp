// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/skin_paint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "data/gamedata.h"

namespace pg {

namespace {

constexpr uint8_t kT = ShapeFrame::kTransparent;

// Base materials, by palette colour (the Avatar's art).
enum Mat : uint8_t { mNone, mOut, mSteel, mRed, mGold, mLeather, mFx };

// Leather ramps: 160-175 (straps, trousers, pack highlights), 176-191 (gloves, belt), 200-223
// (the backpack's dark hide).
int leather_kind(uint8_t i) { return i >= 200 ? 2 : i >= 176 ? 1 : 0; }

Mat mat_of(uint8_t i) {
    if (i == kT) return mNone;
    if (i == 0) return mOut;
    if (i == 9 || (i >= 16 && i <= 31) || (i >= 240 && i <= 254) || (i >= 192 && i <= 199) || (i >= 96 && i <= 111))
        return mSteel;
    if (i >= 32 && i <= 47) return mRed;
    if ((i >= 48 && i <= 63) || (i >= 224 && i <= 238)) return mGold;
    if ((i >= 160 && i <= 191) || (i >= 200 && i <= 223)) return mLeather;
    return mFx;
}

// Body parts of the new figure.
enum Role : uint8_t {
    rNone, rOut, rFx, rHead, rNeck, rArm, rHand, rLeg, rFoot, rTorso, rSkirt, rSleeve, rSash, rPack, rHair, rFace, rEye,
    rHem, rCount
};

// Colour ramps, light to dark (U8PAL indices).
const std::vector<uint8_t> kSkin = {200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210};
const std::vector<uint8_t> kDress = {82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93};
const std::vector<uint8_t> kSleeve = {80, 81, 82, 83, 84, 85, 86, 87, 88};
const std::vector<uint8_t> kSash = {80, 80, 81, 82, 83, 84};
const std::vector<uint8_t> kTrim = {149, 150, 151, 152, 153, 154, 155};
const std::vector<uint8_t> kHair = {211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221};
const std::vector<uint8_t> kShoe = {151, 152, 153, 154, 155, 156};

// 4x4 ordered dither threshold in [0, 1).
float bayer(int x, int y) {
    static const int m[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return (m[y & 3][x & 3] + 0.5f) / 16.0f;
}

// Tone t (0 light .. 1 dark) to a ramp index, dithering between neighbouring steps.
uint8_t ramp(const std::vector<uint8_t>& r, float t, int x, int y, bool dither = true) {
    t = std::clamp(t, 0.0f, 1.0f) * float(r.size() - 1);
    int i = int(t);
    float frac = t - float(i);
    if (i + 1 < int(r.size()) && (dither ? frac > bayer(x, y) : frac >= 0.5f)) ++i;
    return r[size_t(i)];
}

struct Pt { int x, y; };

}  // namespace

SkinPainter::SkinPainter(const GameData& d, unsigned base_shape) : d_(d), base_(base_shape) {
    const Shape* sh = d.shapes.shape(base_shape);
    unsigned n = sh ? sh->frame_count() : 0;
    info_.resize(n);
    std::vector<bool> unflipped(n, false);
    for (unsigned a = 0; a < AnimDat::kActions; ++a) {
        const AnimSequence* s = d.anim.get(base_shape, a);
        if (!s) continue;
        for (unsigned dir = 0; dir < 8; ++dir)
            for (unsigned i = 0; i < s->length; ++i) {
                const AnimFrame& af = s->at(dir, i);
                if (af.frame >= n) continue;
                SkinFrameInfo& fi = info_[af.frame];
                // A mirrored frame is drawn for the mirror direction: E <-> S, N <-> W, NE <-> SW.
                int facing = af.flipped() ? (14 - int(dir)) % 8 : int(dir);
                if (!fi.used || (!af.flipped() && !unflipped[af.frame])) {
                    fi.facing = facing;
                    if (!af.flipped()) unflipped[af.frame] = true;
                }
                if (fi.prev < 0 && i > 0) fi.prev = int(s->at(dir, i - 1).frame);
                if (a == 57 && i >= 2) fi.engulfed = true;
                fi.used = true;
                if (std::find(fi.actions.begin(), fi.actions.end(), a) == fi.actions.end()) fi.actions.push_back(a);
            }
    }
    // Unused frames: the facing of the nearest used frame number.
    for (unsigned f = 0; f < n; ++f) {
        if (info_[f].used) continue;
        for (unsigned k = 1; k < n; ++k) {
            if (f >= k && info_[f - k].used) { info_[f].facing = info_[f - k].facing; break; }
            if (f + k < n && info_[f + k].used) { info_[f].facing = info_[f + k].facing; break; }
        }
    }
}

std::vector<ShapeFrame> SkinPainter::paint_all() {
    std::vector<ShapeFrame> out;
    for (unsigned f = 0; f < frame_count(); ++f) out.push_back(paint(f));
    return out;
}

ShapeFrame SkinPainter::paint(unsigned fno, SkinKeys* keys_out) {
    ShapeFrame out;
    const ShapeFrame* src = d_.shapes.frame(base_, fno);
    if (!src) {
        out.w = out.h = 1;
        out.pixels.assign(1, kT);
        return out;
    }
    out = *src;
    const int W = src->w, H = src->h;
    const SkinFrameInfo& fi = info_[fno];
    const bool only = [&](unsigned a) { return fi.actions.size() == 1 && fi.actions[0] == a; }(57);
    // Engulfed in flames (burning): the base frame is fire, smoke and ash; it stays as it is.
    if (only && fi.engulfed) return out;
    const bool burning = only, drowning = fi.actions.size() == 1 && fi.actions[0] == 56;
    auto at = [&](int x, int y) -> uint8_t { return (x < 0 || y < 0 || x >= W || y >= H) ? kT : src->pixels[size_t(y) * W + x]; };
    auto idx = [&](int x, int y) { return size_t(y) * W + x; };
    std::vector<Mat> mat(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) mat[idx(x, y)] = mat_of(at(x, y));
    auto mat_at = [&](int x, int y) { return (x < 0 || y < 0 || x >= W || y >= H) ? mNone : mat[idx(x, y)]; };

    // Base tone (0 light .. 1 dark) of a pixel within its own material's range.
    auto tone = [&](uint8_t i) {
        const auto& c = d_.palette.rgb8[i];
        float L = (0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2]) / 255.0f;
        float lo = 0.05f, hi = 0.6f;
        switch (mat_of(i)) {
        case mSteel: lo = 0.10f; hi = 0.62f; break;
        case mRed: lo = 0.04f; hi = 0.30f; break;
        case mGold: lo = 0.25f; hi = 0.92f; break;
        case mLeather: lo = 0.04f; hi = 0.45f; break;
        default: break;
        }
        return std::clamp((hi - L) / (hi - lo), 0.0f, 1.0f);
    };

    // Connected regions (8-connected) of each material.
    std::vector<int> comp(size_t(W) * H, -1);
    struct Comp { Mat m; int n = 0, x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1; long sx = 0, sy = 0; };
    std::vector<Comp> comps;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            Mat m = mat[idx(x, y)];
            if (m == mNone || m == mOut || comp[idx(x, y)] >= 0) continue;
            int id = int(comps.size());
            comps.push_back(Comp{m});
            std::vector<Pt> st{{x, y}};
            comp[idx(x, y)] = id;
            while (!st.empty()) {
                Pt p = st.back();
                st.pop_back();
                Comp& c = comps[size_t(id)];
                ++c.n; c.sx += p.x; c.sy += p.y;
                c.x0 = std::min(c.x0, p.x); c.x1 = std::max(c.x1, p.x);
                c.y0 = std::min(c.y0, p.y); c.y1 = std::max(c.y1, p.y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = p.x + dx, ny = p.y + dy;
                        if (mat_at(nx, ny) == m && comp[idx(nx, ny)] < 0) {
                            comp[idx(nx, ny)] = id;
                            st.push_back({nx, ny});
                        }
                    }
            }
        }

    // --- The head: the densest 7 x 7 window of steel (the helmet); among near-equal windows the
    // highest one (so the neck mail under it is not taken).
    int best = -1, bx = 0, by = 0;
    std::vector<int> integral(size_t(W + 1) * (H + 1), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            integral[size_t(y + 1) * (W + 1) + x + 1] = (mat[idx(x, y)] == mSteel) + integral[size_t(y) * (W + 1) + x + 1] +
                integral[size_t(y + 1) * (W + 1) + x] - integral[size_t(y) * (W + 1) + x];
    auto box = [&](int x0, int y0, int x1, int y1) {  // steel count in [x0, x1) x [y0, y1)
        x0 = std::clamp(x0, 0, W); x1 = std::clamp(x1, 0, W); y0 = std::clamp(y0, 0, H); y1 = std::clamp(y1, 0, H);
        return integral[size_t(y1) * (W + 1) + x1] - integral[size_t(y0) * (W + 1) + x1] - integral[size_t(y1) * (W + 1) + x0] +
               integral[size_t(y0) * (W + 1) + x0];
    };
    for (int y = -2; y < H - 3; ++y)
        for (int x = -2; x < W - 3; ++x) {
            int n = box(x, y, x + 7, y + 7);
            if (n > best) { best = n; bx = x; by = y; }
        }
    int hx0 = 0, hy0 = 0, hx1 = -1, hy1 = -1, head_comp = -1;
    bool upright = true;
    if (best >= 12) {
        int top_y = by;
        for (int y = -2; y < H - 3; ++y) {
            bool found = false;
            for (int x = -2; x < W - 3 && !found; ++x)
                if (box(x, y, x + 7, y + 7) >= best - 4 && std::abs(x - bx) <= 3) { top_y = y; bx = x; found = true; }
            if (found) break;
        }
        by = top_y;
        // The steel region holding most of the window is the helmet's.
        std::map<int, int> votes;
        for (int y = by; y < by + 7; ++y)
            for (int x = bx; x < bx + 7; ++x)
                if (mat_at(x, y) == mSteel) ++votes[comp[idx(x, y)]];
        int bestv = 0;
        for (auto& [c, v] : votes) if (v > bestv) { bestv = v; head_comp = c; }
        // The head box: that region's pixels inside the window widened by one.
        hx0 = W; hy0 = H;
        for (int y = by - 1; y <= by + 7; ++y)
            for (int x = bx - 1; x <= bx + 7; ++x)
                if (mat_at(x, y) == mSteel && comp[idx(x, y)] == head_comp) {
                    hx0 = std::min(hx0, x); hx1 = std::max(hx1, x);
                    hy0 = std::min(hy0, y); hy1 = std::max(hy1, y);
                }
    }
    const bool have_head = hx1 >= hx0;
    float hcx = have_head ? (hx0 + hx1) / 2.0f : W / 2.0f, hcy = have_head ? (hy0 + hy1) / 2.0f : 4.0f;

    // --- The body axis: from the head to the tunic's middle (straight down when upright).
    // The tunic: every red region of 4+ pixels (the belt splits it in two).
    std::vector<uint8_t> tunic(comps.size(), 0);
    long tsx = 0, tsy = 0, tn_px = 0;
    for (size_t c = 0; c < comps.size(); ++c)
        if (comps[c].m == mRed && comps[c].n >= 4) {
            tunic[c] = 1;
            tsx += comps[c].sx; tsy += comps[c].sy; tn_px += comps[c].n;
        }
    auto is_tunic = [&](int x, int y) { int c = comp[idx(x, y)]; return mat[idx(x, y)] == mRed && c >= 0 && tunic[size_t(c)]; };
    float ux = 0, uy = 1;
    if (tn_px > 0 && have_head) {
        float rx = float(tsx) / tn_px, ry = float(tsy) / tn_px;
        float vx = rx - hcx, vy = ry - hcy, len = std::sqrt(vx * vx + vy * vy);
        if (len > 2) {
            float ang = std::atan2(vx, vy) * 180.0f / 3.14159265f;  // 0 = straight down
            if (std::fabs(ang) > 35) { upright = false; ux = vx / len; uy = vy / len; }
        }
    }
    // Body coordinates: u along the axis (down the body), v across it (to the screen right when upright).
    auto U = [&](float x, float y) { return (x - hcx) * ux + (y - hcy) * uy; };
    auto V = [&](float x, float y) { return (x - hcx) * uy - (y - hcy) * ux; };

    // Tunic extent along the axis.
    float red_u0 = 1e9f, red_u1 = -1e9f, foot_u = -1e9f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            Mat m = mat[idx(x, y)];
            if (is_tunic(x, y)) {
                red_u0 = std::min(red_u0, U(float(x), float(y)));
                red_u1 = std::max(red_u1, U(float(x), float(y)));
            }
            if (m != mNone && m != mFx) foot_u = std::max(foot_u, U(float(x), float(y)));
        }
    const bool have_body = tn_px > 0 && red_u1 > red_u0;
    float waist_u = have_body ? red_u0 + 0.48f * (red_u1 - red_u0) : 0;
    float hem_u = have_body ? red_u1 + 0.40f * std::max(0.0f, foot_u - red_u1) : 0;
    // Hips: the tunic's width across the axis in its lowest rows.
    float hip_v = 0, hip_hw = 3;
    if (have_body) {
        float v0 = 1e9f, v1 = -1e9f;
        int nv = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (is_tunic(x, y) && U(float(x), float(y)) >= red_u1 - 3) {
                    float v = V(float(x), float(y));
                    v0 = std::min(v0, v); v1 = std::max(v1, v); ++nv;
                }
        if (nv) { hip_v = (v0 + v1) / 2; hip_hw = std::max(2.0f, (v1 - v0) / 2); }
    }

    // --- Leg regions: steel / leather / gold regions that reach past the tunic's hem.
    std::vector<uint8_t> is_leg(comps.size(), 0);
    for (size_t c = 0; c < comps.size(); ++c) {
        if (!have_body || int(c) == head_comp || comps[c].m == mRed || comps[c].m == mFx) continue;
        // farthest point along the axis
        bool reach = false;
        for (int y = comps[c].y0; y <= comps[c].y1 && !reach; ++y)
            for (int x = comps[c].x0; x <= comps[c].x1 && !reach; ++x)
                if (comp[idx(x, y)] == int(c) && U(float(x), float(y)) > red_u1 + 2) reach = true;
        is_leg[c] = reach;
    }
    // The backpack: leather above the waist joined into regions whatever its ramp (the pack mixes
    // two browns), cut at the waist so it cannot run into the thighs; the largest region of 20+
    // pixels with the pack's dark hide in it. Glove-ramp pixels touching the arms' mail stay hands.
    std::vector<uint8_t> pack_mask(size_t(W) * H, 0);
    {
        auto cand = [&](int x, int y) {
            return mat_at(x, y) == mLeather && (!have_body || U(float(x), float(y)) <= waist_u + 1);
        };
        std::vector<int> lab(size_t(W) * H, -1);
        std::vector<std::vector<Pt>> regions;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!cand(x, y) || lab[idx(x, y)] >= 0) continue;
                int id = int(regions.size());
                regions.emplace_back();
                std::vector<Pt> st{{x, y}};
                lab[idx(x, y)] = id;
                while (!st.empty()) {
                    Pt p = st.back();
                    st.pop_back();
                    regions[size_t(id)].push_back(p);
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (cand(p.x + dx, p.y + dy) && lab[idx(p.x + dx, p.y + dy)] < 0) {
                                lab[idx(p.x + dx, p.y + dy)] = id;
                                st.push_back({p.x + dx, p.y + dy});
                            }
                }
            }
        // Score: size, less for regions lower down the body (the pack sits on the upper back;
        // the hands and the belt are lower).
        int bestr = -1;
        float best_score = -1e9f;
        const float shoulder_u = have_head ? hy1 - hcy : 0;
        for (size_t r = 0; r < regions.size(); ++r) {
            const auto& R = regions[r];
            if (R.size() < 20) continue;
            int dark = 0;
            float su = 0;
            for (Pt p : R) { dark += leather_kind(at(p.x, p.y)) == 2; su += U(float(p.x), float(p.y)); }
            if (std::getenv("SKIN_DEBUG"))
                std::fprintf(stderr, "pack candidate %zu: %zu px, %d dark, mean u %.1f, waist %.1f\n", r, R.size(), dark,
                             su / R.size(), waist_u);
            if (dark < 5 || (have_body && su / R.size() > waist_u - 1)) continue;
            float score = float(R.size()) - 3.0f * std::max(0.0f, su / R.size() - shoulder_u);
            if (score > best_score) { best_score = score; bestr = int(r); }
        }
        if (bestr >= 0)
            for (Pt p : regions[size_t(bestr)]) {
                bool glove = false;
                // Glove leather by the arms' mail; any leather there below the shoulders.
                bool low = have_head && U(float(p.x), float(p.y)) > (hy1 - hcy) + 6;
                if (leather_kind(at(p.x, p.y)) == 1 || low)
                    for (int dy = -2; dy <= 2 && !glove; ++dy)
                        for (int dx = -2; dx <= 2 && !glove; ++dx) {
                            int qx = p.x + dx, qy = p.y + dy;
                            if (mat_at(qx, qy) == mSteel && comp[idx(qx, qy)] != head_comp) glove = true;
                        }
                if (!glove) pack_mask[idx(p.x, p.y)] = 1;
            }
    }
    const bool have_pack = std::find(pack_mask.begin(), pack_mask.end(), 1) != pack_mask.end();

    // --- Roles.
    std::vector<Role> role(size_t(W) * H, rNone);
    std::vector<float> tn(size_t(W) * H, 0.5f);
    // Tunic span across the axis per u (to tell a belt from a hand).
    auto in_torso_span = [&](int x, int y) {
        for (int dx = 1; dx <= 6; ++dx) {
            bool l = false, r = false;
            for (int k = 1; k <= dx; ++k) {
                if (mat_at(x - k, y) == mRed) l = true;
                if (mat_at(x + k, y) == mRed) r = true;
            }
            if (l && r) return true;
        }
        bool up = false, dn = false;
        for (int k = 1; k <= 4; ++k) {
            if (mat_at(x, y - k) == mRed) up = true;
            if (mat_at(x, y + k) == mRed) dn = true;
        }
        return up && dn;
    };
    // Steel regions that are mostly outside the tunic are arms; leather touching one is a hand.
    std::vector<int> outside(comps.size(), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (mat[idx(x, y)] == mSteel && !in_torso_span(x, y)) ++outside[size_t(comp[idx(x, y)])];
    std::vector<uint8_t> arm_comp(comps.size(), 0), hand_comp(comps.size(), 0);
    for (size_t c = 0; c < comps.size(); ++c)
        arm_comp[c] = comps[c].m == mSteel && int(c) != head_comp && !is_leg[c] && outside[c] * 2 > comps[c].n;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (mat[idx(x, y)] != mLeather) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (mat_at(x + dx, y + dy) == mSteel && arm_comp[size_t(comp[idx(x + dx, y + dy)])])
                        hand_comp[size_t(comp[idx(x, y)])] = 1;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = idx(x, y);
            Mat m = mat[i];
            if (m == mNone) continue;
            tn[i] = tone(at(x, y));
            int c = comp[i];
            float u = U(float(x), float(y));
            Role r = rNone;
            switch (m) {
            case mOut: r = rOut; break;
            case mFx: r = rFx; break;
            case mSteel:
                if (c == head_comp && have_head && x >= hx0 && x <= hx1 && y >= hy0 && y <= hy1) r = rHead;
                else if (c == head_comp) r = (u - (hy1 - hcy) <= 2.5f) ? rNeck : rTorso;
                else if (is_leg[size_t(c)]) r = rLeg;
                else if (in_torso_span(x, y)) r = rTorso;
                else r = rArm;
                break;
            case mRed: r = rTorso; break;
            case mGold:
                if (is_leg[size_t(c)] || (have_body && u > red_u1 - 1)) r = rLeg;
                else if (have_body && std::fabs(u - waist_u) <= 2.5f && in_torso_span(x, y)) r = rSash;
                else if (in_torso_span(x, y) && have_body && u > red_u0 + 2) r = rTorso;
                else if (!have_head || u - (hy1 - hcy) <= 7.5f) r = rSleeve;  // the shoulder: a cap sleeve
                else r = rArm;
                break;
            case mLeather:
                if (pack_mask[i]) r = rPack;
                else if (is_leg[size_t(c)] || (have_body && u > red_u1 - 1)) r = rLeg;
                else if (hand_comp[size_t(c)] && (leather_kind(at(x, y)) == 1 || !in_torso_span(x, y))) r = rHand;
                else if (have_body && std::fabs(u - waist_u) <= 2.5f && in_torso_span(x, y)) r = rSash;
                else if (in_torso_span(x, y)) r = rTorso;
                else r = rHand;
                break;
            default: break;
            }
            role[i] = r;
        }
    auto role_at = [&](int x, int y) { return (x < 0 || y < 0 || x >= W || y >= H) ? rNone : role[idx(x, y)]; };
    // Burning (its first frames): the flames rising round the legs are red and gold too; below
    // the waist they stay fire.
    if (burning && have_body)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Mat m = mat[idx(x, y)];
                if ((m == mRed || m == mGold) && U(float(x), float(y)) > waist_u) role[idx(x, y)] = rFx;
            }

    // The pack's straps and highlights (other leather and gold above the waist touching it).
    if (have_pack) {
        const std::vector<Role> before = role;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = idx(x, y);
                if (mat[i] != mGold || before[i] == rPack) continue;
                if (have_body && U(float(x), float(y)) > waist_u + 2) continue;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        n += (x + dx >= 0 && y + dy >= 0 && x + dx < W && y + dy < H) && before[idx(x + dx, y + dy)] == rPack;
                if (n >= 6) role[i] = rPack;  // a buckle inside the pack, not a shoulder in front of it
            }
    }
    // --- The backpack goes: seen from behind it becomes the dress's back; otherwise only a thin
    // layer next to the body stays (the back of the bodice), the rest is cleared.
    std::vector<uint8_t> pack_edge(size_t(W) * H, 0);  // outline that bordered the pack
    if (have_pack) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (role[idx(x, y)] != rOut) continue;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (role_at(x + dx, y + dy) == rPack) pack_edge[idx(x, y)] = 1;
            }
        bool from_behind = false;
        int reach = (fi.facing == 3 || fi.facing == 2 || fi.facing == 4) ? 1 : 2;
        const std::vector<Role> before = role;
        auto before_at = [&](int x, int y) { return (x < 0 || y < 0 || x >= W || y >= H) ? rNone : before[idx(x, y)]; };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = idx(x, y);
                if (role[i] != rPack) continue;
                bool near = from_behind;
                // Three-quarter back views: the pack covers the back, so what lies over the
                // body's width (the hips' band up the body) is the back of the dress.
                if ((fi.facing == 0 || fi.facing == 6 || fi.facing == 7) && std::fabs(V(float(x), float(y)) - hip_v) <= hip_hw + (fi.facing == 7 ? 2 : 1))
                    near = true;
                for (int dy = -reach; dy <= reach && !near; ++dy)
                    for (int dx = -reach; dx <= reach && !near; ++dx) {
                        Role q = before_at(x + dx, y + dy);
                        if (q == rTorso || q == rSash) near = true;
                    }
                if (!near) {  // between dress pixels (across or above and below): fill the gap
                    auto dressy = [&](int qx, int qy) { Role q = before_at(qx, qy); return q == rTorso || q == rSash; };
                    bool l = false, r = false, u = false, dn = false;
                    for (int k = 1; k <= 8; ++k) { l |= dressy(x - k, y); r |= dressy(x + k, y); }
                    for (int k = 1; k <= 6; ++k) { u |= dressy(x, y - k); dn |= dressy(x, y + k); }
                    near = (l && r) || (u && dn);
                }
                role[i] = near ? rTorso : rNone;
            }
        // Outline pixels that only bordered the pack are cleared later (stray outline pass).
    }

    // --- Leg ends: slippers on the feet (the leg pixels near the farthest point from the hips).
    if (have_body) {
        float hipx = hcx + hip_v * uy + red_u1 * ux, hipy = hcy - hip_v * ux + red_u1 * uy;
        for (size_t c = 0; c < comps.size(); ++c) {
            if (!is_leg[c]) continue;
            float far = -1;
            int fx = 0, fy = 0;
            for (int y = comps[c].y0; y <= comps[c].y1; ++y)
                for (int x = comps[c].x0; x <= comps[c].x1; ++x)
                    if (comp[idx(x, y)] == int(c)) {
                        float dd = (x - hipx) * (x - hipx) + (y - hipy) * (y - hipy);
                        if (dd > far) { far = dd; fx = x; fy = y; }
                    }
            if (U(float(fx), float(fy)) < hem_u + 1) continue;  // a thigh piece under the skirt
            for (int y = comps[c].y0; y <= comps[c].y1; ++y)
                for (int x = comps[c].x0; x <= comps[c].x1; ++x)
                    if (comp[idx(x, y)] == int(c) && (x - fx) * (x - fx) + (y - fy) * (y - fy) <= 7 &&
                        U(float(x), float(y)) > hem_u + 1)
                        role[idx(x, y)] = rFoot;
        }
    }

    // --- The skirt: from the tunic's lower part to mid-calf, over and between the legs.
    std::vector<uint8_t> skirt(size_t(W) * H, 0);
    if (have_body) {
        int ub0 = int(std::floor(red_u1 - 2)), ub1 = int(std::floor(hem_u));
        int nb = std::max(0, ub1 - ub0 + 1);
        std::vector<float> lv(size_t(nb), 1e9f), rv(size_t(nb), -1e9f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Role r = role[idx(x, y)];
                if (r != rLeg && r != rFoot && !(r == rOut && have_body)) continue;
                if (r == rOut) {  // an outline between leg pixels counts with the legs
                    bool legn = false;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            Role q = role_at(x + dx, y + dy);
                            if (q == rLeg || q == rFoot) legn = true;
                        }
                    if (!legn) continue;
                }
                int b = int(std::floor(U(float(x), float(y)))) - ub0;
                if (b < 0 || b >= nb) continue;
                float v = V(float(x), float(y));
                lv[size_t(b)] = std::min(lv[size_t(b)], v);
                rv[size_t(b)] = std::max(rv[size_t(b)], v);
            }
        // A-line: the skirt never narrows going down, and stays within a flare limit.
        float cl = hip_v - hip_hw, cr = hip_v + hip_hw;
        for (int b = 0; b < nb; ++b) {
            float t = float(b) / std::max(1, nb - 1);
            float lim = hip_hw + 1.5f + 6.0f * t;
            float l = std::min(lv[size_t(b)] - 0.5f, hip_v - hip_hw - 1.2f * t);
            float r = std::max(rv[size_t(b)] + 0.5f, hip_v + hip_hw + 1.2f * t);
            l = std::max(l, hip_v - lim);
            r = std::min(r, hip_v + lim);
            cl = std::min(cl, l);
            cr = std::max(cr, r);
            lv[size_t(b)] = cl;
            rv[size_t(b)] = cr;
        }
        // Where the legs are at hem height: between widely planted legs the cloth hangs in a curve
        // that rises away from each leg, so the stance shows instead of a tent.
        std::vector<float> leg_v;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Role r = role[idx(x, y)];
                if ((r == rLeg || r == rFoot) && std::fabs(U(float(x), float(y)) - hem_u) <= 2.0f) leg_v.push_back(V(float(x), float(y)));
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float u = U(float(x), float(y)), v = V(float(x), float(y));
                int b = int(std::floor(u)) - ub0;
                if (b < 0 || b >= nb) continue;
                if (v < lv[size_t(b)] - 0.01f || v > rv[size_t(b)] + 0.01f) continue;
                if (!leg_v.empty()) {
                    float dmin = 1e9f;
                    for (float lvv : leg_v) dmin = std::min(dmin, std::fabs(v - lvv));
                    float lift = 0.6f * std::max(0.0f, dmin - 2.5f);
                    if (u > hem_u - std::min(lift, 0.6f * (hem_u - red_u1))) continue;
                }
                size_t i = idx(x, y);
                Role r = role[i];
                if (r == rArm || r == rHand || r == rFx || r == rSleeve) continue;
                if (u < red_u1 - 1.5f && r != rTorso && r != rLeg) continue;  // above the tunic's hem: only fill the tunic
                float span = std::max(1.0f, rv[size_t(b)] - lv[size_t(b)]);
                float across = (v - lv[size_t(b)]) / span;
                bool over_leg = r == rLeg || r == rFoot;
                float t = 0.30f + 0.45f * across + (over_leg ? -0.10f : 0.12f);
                if (r == rTorso) t = 0.5f * t + 0.5f * (0.2f + 0.8f * tn[i]);
                tn[i] = t;
                role[i] = rSkirt;
                skirt[i] = 1;
            }
    }

    // The hem: the skirt's lowest pixel in each column gets the deep-blue trim.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = idx(x, y);
            if (!skirt[i]) continue;
            int bx2 = x + int(std::lround(ux * 1.0f)), by2 = y + int(std::lround(uy * 1.0f));
            bool below_skirt = bx2 >= 0 && by2 >= 0 && bx2 < W && by2 < H && skirt[idx(bx2, by2)];
            if (!below_skirt) role[i] = rHem;
        }

    // --- Head: hair on top and at the back, a face on the side it faces.
    const int f = fi.facing;
    // Screen facing: x sign (+1 right) and front-ness (+1 towards the viewer).
    static const int kFx[8] = {+1, +1, +1, 0, -1, -1, -1, 0};
    static const int kFz[8] = {-1, 0, +1, +1, +1, 0, -1, -1};
    const int sx = kFx[f], sz = kFz[f];
    if (have_head) {
        int w = hx1 - hx0 + 1, hh = hy1 - hy0 + 1;
        int face_top = hy0 + std::max(2, int(std::lround(0.38 * hh)));
        int fx0 = hx0, fx1 = hx1;  // face columns
        bool face = true;
        if (sz > 0 && sx == 0) { fx0 = hx0 + 1; fx1 = hx1 - 1; }
        else if (sz > 0) { if (sx > 0) fx0 = hx0 + 2; else fx1 = hx1 - 2; }
        else if (sz == 0) { if (sx > 0) fx0 = hx0 + w / 2; else fx1 = hx1 - w / 2; }
        else if (sx != 0) { if (sx > 0) fx0 = hx1 - 1; else fx1 = hx0 + 1; face_top = hy0 + int(std::lround(0.6 * hh)); }
        else face = false;
        for (int y = hy0; y <= hy1; ++y)
            for (int x = hx0; x <= hx1; ++x) {
                size_t i = idx(x, y);
                if (role[i] != rHead) continue;
                bool is_face = face && y >= face_top && x >= fx0 && x <= fx1;
                float across = float(x - hx0) / std::max(1, w - 1), down = float(y - hy0) / std::max(1, hh - 1);
                if (is_face) {
                    role[i] = rFace;
                    tn[i] = 0.10f + 0.35f * across + 0.25f * down;
                } else {
                    role[i] = rHair;
                    tn[i] = 0.15f + 0.45f * across + 0.25f * down;
                }
            }
        // Round the head: clear the helmet's top corners.
        for (int k = 0; k < 2; ++k) {
            int x = k ? hx1 : hx0;
            if (role_at(x, hy0) == rHair) role[idx(x, hy0)] = rNone;
        }
        // Eyes on the face's upper row.
        if (face && fi.facing != 7) {
            int ey = face_top + (hh >= 7 ? 1 : 0);
            std::vector<int> ex;
            if (sz > 0 && sx == 0) { ex = {fx0 + 1, fx1 - 1}; }
            else if (sz > 0) { ex = sx > 0 ? std::vector<int>{fx0 + 1, fx1 - 1} : std::vector<int>{fx0 + 1, fx1 - 1}; }
            else if (sz == 0) { ex = {sx > 0 ? fx1 - 1 : fx0 + 1}; }
            for (int x : ex)
                if (role_at(x, ey) == rFace) role[idx(x, ey)] = rEye;
        }
        // Long hair below the head, by facing (head-relative bands per row).
        bool run = std::find(fi.actions.begin(), fi.actions.end(), 1u) != fi.actions.end();
        bool walk = std::find(fi.actions.begin(), fi.actions.end(), 0u) != fi.actions.end();
        int len = sz < 0 ? 10 : sz == 0 ? 9 : 7;
        for (int k = -3; k <= len; ++k) {
            int y = hy1 + k;
            float t = float(std::max(0, k)) / len;
            int trail = run ? -sx * int(std::lround(2 * t)) : walk ? -sx * int(std::lround(1 * t)) : 0;
            std::vector<std::pair<int, int>> bands;
            if (sz < 0 && sx == 0) {  // from behind: one mass over the back
                int grow = k < len - 2 ? 1 : (k == len - 1 ? -1 : -2);
                if (k < 0) grow = 0;
                bands.push_back({hx0 - grow + trail, hx1 + grow + trail});
            } else if (sz < 0) {  // three-quarter back: the mass leans away from the face
                int c = sx > 0 ? -1 : 1;
                int grow = k < len - 2 ? 0 : (k == len - 1 ? -1 : -2);
                if (k < 0) bands.push_back({sx > 0 ? hx0 : hx1 - 2, sx > 0 ? hx0 + 2 : hx1});
                else bands.push_back({hx0 + c - grow + trail, hx1 + c + grow + trail - 1});
            } else if (sz == 0) {  // profile: hangs behind the head
                int flow = k > 3 ? 1 : 0;
                int tip = k >= len - 1 ? 1 : 0;
                if (sx > 0) bands.push_back({hx0 - 1 - flow + trail, hx0 + 2 - tip + trail});
                else bands.push_back({hx1 - 2 + tip + trail, hx1 + 1 + flow + trail});
            } else if (sx != 0) {  // three-quarter front: behind the far shoulder, a lock by the face
                int tip = k >= len - 1 ? 1 : 0;
                if (sx > 0) {
                    bands.push_back({hx0 - 1 + trail, hx0 + 1 - tip + trail});
                    if (k <= len - 3) bands.push_back({hx1, hx1 + (k >= 1 ? 1 : 0)});
                } else {
                    bands.push_back({hx1 - 1 + tip + trail, hx1 + 1 + trail});
                    if (k <= len - 3) bands.push_back({hx0 - (k >= 1 ? 1 : 0), hx0});
                }
            } else {  // from the front: two locks over the shoulders
                int tip = k >= len - 1 ? 1 : 0;
                bands.push_back({hx0 - 1 + tip, hx0 + (k >= 1 ? 0 : 0)});
                bands.push_back({hx1, hx1 + 1 - tip});
            }
            if (y < 0 || y >= H) continue;
            for (auto [x0, x1] : bands)
                for (int x = x0; x <= x1; ++x) {
                    if (x < 0 || x >= W) continue;
                    size_t i = idx(x, y);
                    Role r = role[i];
                    if (r == rArm || r == rHand || r == rFx || r == rFace || r == rEye) continue;
                    if (k < 0 && r != rHead && r != rHair && r != rNone && r != rOut && r != rNeck) continue;
                    float across = (x - x0) / float(std::max(1, x1 - x0));
                    role[i] = rHair;
                    // strands: every other column a little darker; tips darker
                    tn[i] = 0.25f + 0.45f * across + 0.3f * t + (((x + (k + 8) / 3) & 1) ? 0.08f : 0.0f);
                }
        }
    }

    // --- A slighter build: arms lose a pixel on their outer side (the outline moves in).
    if (upright) {
        const std::vector<Role> before = role;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Role r = before[idx(x, y)];
                if (r != rArm && r != rSleeve && r != rHand) continue;
                int out_dir = x < hcx ? -1 : 1;
                int ox = x + out_dir;
                if (ox < 0 || ox >= W || before[idx(ox, y)] != rOut) continue;
                int ox2 = ox + out_dir;
                if (ox2 >= 0 && ox2 < W && before[idx(ox2, y)] != rNone) continue;
                role[idx(ox, y)] = rNone;
            }
    }

    // --- Smooth the dress's shading: the tunic's metal-and-cloth texture would read as noise
    // on plain fabric. Each dress pixel takes half of its neighbourhood's average tone.
    {
        auto dressy = [](Role r) { return r == rTorso || r == rSkirt || r == rHem; };
        std::vector<float> t2 = tn;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!dressy(role[idx(x, y)])) continue;
                float sum = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dressy(role_at(x + dx, y + dy))) { sum += tn[idx(x + dx, y + dy)]; ++n; }
                t2[idx(x, y)] = 0.4f * tn[idx(x, y)] + 0.6f * sum / float(n);
            }
        tn = t2;
    }

    // --- Colour the roles.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = idx(x, y);
            Role r = role[i];
            float t = tn[i];
            uint8_t& o = out.pixels[i];
            switch (r) {
            case rNone: o = kT; break;
            case rOut: o = 0; break;
            case rFx: break;
            case rHead: case rHair: o = ramp(kHair, t, x, y); break;
            case rFace: o = ramp(kSkin, 0.05f + 0.6f * t, x, y); break;
            case rEye: o = 154; break;
            case rNeck: o = ramp(kSkin, 0.25f + 0.6f * t, x, y); break;
            case rArm: o = ramp(kSleeve, 0.15f + 0.7f * t, x, y, false); break;  // long sleeves
            case rHand:  // the glove ramp is the hand; other leather on the arm is the sleeve's cuff
                if (leather_kind(at(x, y)) == 1 || mat[i] != mLeather) o = ramp(kSkin, 0.15f + 0.7f * t, x, y);
                else o = ramp(kSleeve, 0.35f + 0.6f * t, x, y, false);
                break;
            case rLeg: o = ramp(kSkin, 0.1f + 0.7f * t, x, y, false); break;
            case rFoot: o = ramp(kShoe, t, x, y); break;
            case rTorso: o = ramp(kDress, 0.15f + 0.7f * t, x, y, false); break;
            case rSkirt: o = ramp(kDress, t, x, y); break;
            case rHem: o = ramp(kTrim, 0.2f + 0.6f * t, x, y); break;
            case rSleeve: o = ramp(kSleeve, 0.05f + 0.6f * t, x, y, false); break;
            case rSash: o = ramp(kSash, t, x, y); break;
            case rPack: o = ramp(kDress, t, x, y); break;
            default: break;
            }
        }
    // Drowning: the base darkens as he sinks; she darkens by the same amount (each pixel scaled
    // by the base pixel's brightness against a normal lit value, matched to the palette).
    if (drowning) {
        const auto& pal = d_.palette.rgb8;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = idx(x, y);
                uint8_t o = out.pixels[i], b = at(x, y);
                if (o == kT || o == 0 || b == kT || role[i] == rFx) continue;
                const auto& c = pal[b];
                float L = (0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2]) / 255.0f;
                float ref = mat[i] == mRed ? 0.22f : mat[i] == mGold ? 0.6f : 0.38f;
                float k = std::clamp(L / ref, 0.12f, 1.0f);
                if (k > 0.95f) continue;
                int r = int(pal[o][0] * k), g = int(pal[o][1] * k), bl = int(pal[o][2] * k), best = o, bd = 1 << 30;
                for (int q = 16; q < 256; ++q) {
                    if (d_.xform.loaded && d_.xform.is_xform(uint8_t(q))) continue;
                    int dr = r - pal[q][0], dg = g - pal[q][1], db = bl - pal[q][2];
                    int dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                    if (dd < bd) { bd = dd; best = q; }
                }
                out.pixels[i] = uint8_t(best);
            }
    }

    // --- Clean up: outline pixels with no body next to them go; then every body pixel on the
    // silhouette becomes outline (the game's dark edge). Hair edges against the body darken.
    auto opaque = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && out.pixels[idx(x, y)] != kT; };
    // The pack's old outline: wherever it no longer lies against the figure (no figure pixel
    // above, below or beside it), it goes; the silhouette is outlined again below.
    for (int pass = 0; pass < 3; ++pass)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = idx(x, y);
                if (!pack_edge[i] || out.pixels[i] != 0) continue;
                static const int nb4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                int body = 0;
                for (auto& q : nb4)
                    if (opaque(x + q[0], y + q[1]) && out.pixels[idx(x + q[0], y + q[1])] != 0) ++body;
                if (body == 0) out.pixels[i] = kT;
            }
    for (int pass = 0; pass < 2; ++pass)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = idx(x, y);
                if (out.pixels[i] != 0) continue;
                bool body = false;
                for (int dy = -1; dy <= 1 && !body; ++dy)
                    for (int dx = -1; dx <= 1 && !body; ++dx)
                        if ((dx || dy) && opaque(x + dx, y + dy) && out.pixels[idx(x + dx, y + dy)] != 0) body = true;
                if (!body) out.pixels[i] = kT;
            }
    std::vector<uint8_t> edge(size_t(W) * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = idx(x, y);
            if (out.pixels[i] == kT || role[i] == rFx) continue;
            if (!opaque(x - 1, y) || !opaque(x + 1, y) || !opaque(x, y - 1) || !opaque(x, y + 1)) edge[i] = 1;
        }
    for (size_t i = 0; i < edge.size(); ++i)
        if (edge[i]) out.pixels[i] = 0;
    // Hair against the rest of the figure: its darkest shade, so the mass reads as one shape.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = idx(x, y);
            if (role[i] != rHair || edge[i]) continue;
            static const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& n : nb) {
                Role q = role_at(x + n[0], y + n[1]);
                if (q != rHair && q != rHead && q != rNone && q != rOut && !edge[idx(x + n[0], y + n[1])]) {
                    out.pixels[i] = kHair[kHair.size() - 2];
                    break;
                }
            }
        }
    // Loose pieces (a strap end left from the backpack): only the figure and magic light stay.
    {
        std::vector<int> lab(size_t(W) * H, -1);
        std::vector<int> size;
        std::vector<uint8_t> has_fx;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (out.pixels[idx(x, y)] == kT || lab[idx(x, y)] >= 0) continue;
                int id = int(size.size());
                size.push_back(0);
                has_fx.push_back(0);
                std::vector<Pt> st{{x, y}};
                lab[idx(x, y)] = id;
                while (!st.empty()) {
                    Pt p = st.back();
                    st.pop_back();
                    ++size[size_t(id)];
                    if (role[idx(p.x, p.y)] == rFx) has_fx[size_t(id)] = 1;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (opaque(p.x + dx, p.y + dy) && lab[idx(p.x + dx, p.y + dy)] < 0) {
                                lab[idx(p.x + dx, p.y + dy)] = id;
                                st.push_back({p.x + dx, p.y + dy});
                            }
                }
            }
        int big = 0;
        for (size_t k = 0; k < size.size(); ++k) if (size[k] > size[size_t(big)]) big = int(k);
        for (size_t i = 0; i < lab.size(); ++i)
            if (lab[i] >= 0 && lab[i] != big && !has_fx[size_t(lab[i])] && size[size_t(lab[i])] < 60) out.pixels[i] = kT;
    }
    if (keys_out) {
        keys_out->head_x0 = hx0; keys_out->head_y0 = hy0; keys_out->head_x1 = hx1; keys_out->head_y1 = hy1;
        keys_out->upright = upright;
        keys_out->roles.assign(role.begin(), role.end());
        keys_out->waist_y = int(hcy + waist_u);
        keys_out->hem_y = int(hcy + hem_u);
        keys_out->foot_y = int(hcy + foot_u);
        keys_out->hip_x = int(hcx + hip_v);
    }
    return out;
}

}  // namespace pg
