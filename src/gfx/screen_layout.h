// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Where the 320 x 200 game picture and the HUD side panel go in a window (PLAN-HUD.md; the
// engine's own design). Pure maths, no SDL: the platform, the screenshot code and the tests
// share it.
//
// Classic (no panel): the picture is centred, as before (integer / fit / stretch, 4:3 aspect).
// Side panel: the picture is placed against the left edge and the panel takes the spare width
// on its right, drawn at the same scale (one panel pixel = one game pixel, so it looks like part
// of the game and the mouse maps straight through: game x 320.. is the panel). When the window
// has less spare width than kMinPanel game pixels (4:3, portrait windows) the picture shrinks
// to leave that much (integer mode drops to the next whole scale; when even 1x does not fit
// it scales freely). The panel is at most kMaxPanel game pixels wide; anything past that stays
// black on the right.
#include <cstdint>
#include <vector>

namespace pg {

struct ScreenLayout {
    // Window (drawable) pixels.
    int game_x = 0, game_y = 0, game_w = 320, game_h = 200;
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    // The panel's width in game pixels (0 = no panel); its height is always the game's (200).
    int panel_px = 0;
    bool has_panel() const { return panel_px > 0; }
};

enum class LayoutScale { kInteger = 0, kFit = 1, kStretch = 2 };

constexpr int kMinPanel = 96;   // game pixels
constexpr int kMaxPanel = 320;

// ww x wh: the window's drawable size. gw x gh: the game surface (320 x 200). aspect: 4:3 (game
// pixels 1.2x taller). panel: the side panel is wanted.
ScreenLayout compute_layout(int ww, int wh, int gw, int gh, bool aspect, LayoutScale mode, bool panel,
                            int min_panel = kMinPanel, int max_panel = kMaxPanel);

// Window pixel -> game-surface pixel (x may be >= gw: the panel). Sub-pixel results in fx / fy.
void layout_window_to_game(const ScreenLayout& l, int gw, int gh, double wx, double wy, double& fx, double& fy);

// Builds the whole window picture (black, the game frame scaled into its rectangle, the panel
// frame into its): headless screenshots of the side-panel layout. Nearest sampling, as the GPU
// path does with SDL's "0" scale quality.
void compose_window(const ScreenLayout& l, int ww, int wh, const uint32_t* game, int fw, int fh, const uint32_t* panel,
                    int pw, int ph, std::vector<uint32_t>& out);

}  // namespace pg
