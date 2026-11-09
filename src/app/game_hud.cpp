// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The game mode's side of the HUD side panel (app/hud_panel.h, PLAN-HUD.md): its width, input
// routing (mouse x >= 320 is the panel), drawing it with the pointer on it, and the setting.
#include <algorithm>

#include "app/game_mode.h"
#include "app/hud_panel.h"
#include "core/config.h"
#include "data/gamedata.h"
#include "game/party.h"
#include "gfx/surface.h"

namespace pg {

bool GameMode::side_panel_setting() { return Config::global().get("hud", "panel") != "classic"; }

void GameMode::set_panel_width(int pw) {
    pw = std::max(0, pw);
    if (pw == panel_w_) return;
    panel_w_ = pw;
    if (!pw && hud_panel_) {
        if (hud_panel_->focused()) hud_panel_->set_focus(false);
        hud_panel_->leave();
    }
    // The party bar overlay goes away while the panel shows the portraits.
    if (world_->party) {
        world_->party->hud_panel = pw > 0;
        world_->party->update_bar();
    }
}

bool GameMode::hud_route(const InputEvent& ev) {
    const bool mouse = ev.type == InputEvent::kMouseMove || ev.type == InputEvent::kMouseDown ||
                       ev.type == InputEvent::kMouseUp || ev.type == InputEvent::kWheel;
    if (!mouse) return false;
    int x = ev.x == -9999 ? mouse_x_ : ev.x, y = ev.x == -9999 ? mouse_y_ : ev.y;
    const bool inside = x >= 320 && x < 320 + panel_w_ && y >= 0 && y < 200;
    auto take_pointer = [&] {
        mouse_x_ = x;
        mouse_y_ = y;
        mouse_fx_ = ev.fx >= 0 ? ev.fx : x + 0.5f;
        mouse_fy_ = ev.fy >= 0 ? ev.fy : y + 0.5f;
    };
    const int b = ev.button == 1 ? 0 : ev.button == 2 ? 1 : 2;
    const PlayerInput& in = world_->input;
    switch (ev.type) {
    case InputEvent::kWheel:
        if (!inside) return false;
        hud_panel_->wheel(ev.wheel, x - 320, y);
        return true;
    case InputEvent::kMouseDown:
        if (!inside || drag_ != kDragNone) return false;
        take_pointer();
        hud_press_[b] = true;
        hud_panel_->mouse_down(ev.button, x - 320, y);
        return true;
    case InputEvent::kMouseUp:
        if (!hud_press_[b]) return false;
        hud_press_[b] = false;
        take_pointer();
        hud_panel_->mouse_up(ev.button, x - 320, y);
        return true;
    case InputEvent::kMouseMove:
        if (inside) hud_panel_->mouse_move(x - 320, y);
        else hud_panel_->leave();
        if (hud_press_[0]) {  // dragging the log's bar
            take_pointer();
            hud_panel_->mouse_move(x - 320, y);
            return true;
        }
        // Over the panel with nothing held: the pointer is the panel's (the world does not see it).
        if (inside && drag_ == kDragNone && !in.button[0].down && !in.button[2].down) {
            take_pointer();
            pointer_frames_ = 1000;
            return true;
        }
        return false;
    default: return false;
    }
}

void GameMode::render_panel(Surface8& s) {
    if (panel_w_ <= 0) return;
    if (s.width() != panel_w_ || s.height() != 200) s.resize(panel_w_, 200);
    hud_panel_->draw(s);
    if (mouse_x_ >= 320 && help_page_ < 0 && !atlas_on_) {
        // The pointer on the panel: the dragged item and its cursor, else the hand.
        int mx = mouse_x_;
        float mfx = mouse_fx_;
        mouse_x_ -= 320;
        mouse_fx_ -= 320;
        cursor_on_panel_ = true;
        if (drag_ != kDragNone) draw_cursor(s);
        else if (const Shape* m = data_.mouse.shape(0))
            if (const ShapeFrame* fr = m->frame(unsigned(kCursorHand))) s.draw(*fr, mouse_x_, mouse_y_);
        cursor_on_panel_ = false;
        mouse_x_ = mx;
        mouse_fx_ = mfx;
    }
}

void GameMode::render_panel_rgba(std::vector<uint32_t>& out, int& w, int& h) {
    w = h = 0;
    if (panel_w_ <= 0) { out.clear(); return; }
    if (!panel_surf_) panel_surf_ = std::make_unique<Surface8>(panel_w_, 200);
    render_panel(*panel_surf_);
    panel_surf_->to_rgba(data_.palette, world_->palette, out);
    w = panel_w_;
    h = 200;
}

}  // namespace pg
