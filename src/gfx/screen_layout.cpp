// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "gfx/screen_layout.h"

#include <algorithm>
#include <cmath>

namespace pg {

ScreenLayout compute_layout(int ww, int wh, int gw, int gh, bool aspect, LayoutScale mode, bool panel, int min_panel,
                            int max_panel) {
    ScreenLayout l;
    ww = std::max(1, ww);
    wh = std::max(1, wh);
    const double hf = aspect ? 1.2 : 1.0;
    const double GW = gw, GH = gh * hf;
    if (!panel) {
        // Classic: centred (the platform's old output rectangle, unchanged).
        if (mode == LayoutScale::kStretch) {
            l.game_x = l.game_y = 0;
            l.game_w = ww;
            l.game_h = wh;
            return l;
        }
        int ow, oh;
        if (mode == LayoutScale::kInteger && ww >= GW && wh >= GH) {
            int n = std::max(1, int(std::min(ww / GW, wh / GH)));
            ow = int(GW * n);
            oh = int(GH * n);
        } else {
            double s = std::min(ww / GW, wh / GH);
            ow = int(GW * s);
            oh = int(GH * s);
        }
        l.game_x = (ww - ow) / 2;
        l.game_y = (wh - oh) / 2;
        l.game_w = ow;
        l.game_h = oh;
        return l;
    }
    const double LW = GW + std::max(1, min_panel);  // the picture plus the smallest panel
    int ow, oh;
    if (mode == LayoutScale::kStretch) {
        ow = int(ww * GW / LW);
        oh = wh;
    } else if (mode == LayoutScale::kInteger && ww >= LW && wh >= GH &&
               int(std::min(ww / LW, wh / GH)) == int(std::min(ww / GW, wh / GH))) {
        // Whole scales while the panel fits at the scale the classic screen would use; when it
        // would cost a whole step (a 4:3 window: 3x -> 2x) the picture is scaled freely instead,
        // just enough to leave the minimum panel.
        int n = std::max(1, int(std::min(ww / LW, wh / GH)));
        ow = int(GW * n);
        oh = int(GH * n);
    } else {
        double s = std::min(ww / LW, wh / GH);
        ow = std::max(1, int(GW * s));
        oh = std::max(1, int(GH * s));
    }
    const double sx = double(ow) / GW;  // window pixels per game pixel, across
    l.game_x = 0;
    l.game_y = (wh - oh) / 2;
    l.game_w = ow;
    l.game_h = oh;
    int px = int((ww - ow) / sx + 1e-6);
    px = std::min(px, std::max(1, max_panel));
    if (px < 1) return l;
    l.panel_px = px;
    l.panel_x = ow;
    l.panel_y = l.game_y;
    l.panel_w = std::min(ww - ow, int(std::lround(px * sx)));
    l.panel_h = oh;
    return l;
}

void layout_window_to_game(const ScreenLayout& l, int gw, int gh, double wx, double wy, double& fx, double& fy) {
    fx = (wx - l.game_x) * gw / std::max(1, l.game_w);
    fy = (wy - l.game_y) * gh / std::max(1, l.game_h);
}

void compose_window(const ScreenLayout& l, int ww, int wh, const uint32_t* game, int fw, int fh, const uint32_t* panel,
                    int pw, int ph, std::vector<uint32_t>& out) {
    out.assign(size_t(std::max(0, ww)) * size_t(std::max(0, wh)), 0xFF000000u);
    auto blit = [&](const uint32_t* src, int sw, int sh, int dx, int dy, int dw, int dh) {
        if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        for (int y = std::max(0, dy); y < std::min(wh, dy + dh); ++y) {
            int sy = std::min(sh - 1, int((long long)(y - dy) * sh / dh));
            uint32_t* d = &out[size_t(y) * ww];
            const uint32_t* s = src + size_t(sy) * sw;
            for (int x = std::max(0, dx); x < std::min(ww, dx + dw); ++x) d[x] = s[std::min(sw - 1, int((long long)(x - dx) * sw / dw))];
        }
    };
    blit(game, fw, fh, l.game_x, l.game_y, l.game_w, l.game_h);
    if (l.has_panel()) blit(panel, pw, ph, l.panel_x, l.panel_y, l.panel_w, l.panel_h);
}

}  // namespace pg
