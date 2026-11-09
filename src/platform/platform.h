// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gfx/screen_layout.h"
#include "platform/input.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace pg {

enum class ScaleMode { kInteger, kFit, kStretch };

struct PlatformConfig {
    std::string title = "U8 Ether Gate";
    int game_w = 320, game_h = 200;  // the game surface
    int window_scale = 3;
#ifdef __EMSCRIPTEN__
    ScaleMode scale_mode = ScaleMode::kFit;  // a browser window is rarely a whole multiple
#else
    ScaleMode scale_mode = ScaleMode::kInteger;
#endif
    bool aspect_4_3 = true;  // show 320x200 at 4:3 (pixels 1.2x taller), as on a VGA monitor
    bool fullscreen = false;
    bool headless = false;   // no window needed (screenshot runs); a failing video driver is not fatal
    bool audio = true;
    bool no_video = false;   // don't touch SDL video at all (pure software runs, e.g. under node)
    // --window WxH: the window's size (0 = from window_scale). Headless runs have no drawable,
    // so this is also the size the layout (and a side-panel screenshot) is computed for.
    int window_w = 0, window_h = 0;
};

// Audio seam: the device pulls interleaved stereo S16 samples from this callback on the
// audio thread. The mixer milestone (SOUND.FLX, MUSIC.FLX) installs a real one.
using AudioCallback = std::function<void(int16_t* out, int frames)>;

// SDL2 window, scaling of the game surface, input events and the audio device.
class Platform {
public:
    bool init(const PlatformConfig& cfg);
    void shutdown();

    // Pending input, translated to game-surface coordinates. Display hotkeys are handled
    // here and not reported: F9 cycles scale mode, F10 toggles 4:3, F11 / Alt+Enter fullscreen.
    void poll(std::vector<InputEvent>& out);
    // Shows a w x h 0xAARRGGBB frame scaled into the window; with the HUD side panel on, the
    // panel frame (pw x ph, one pixel per game pixel) goes into its rectangle on the right.
    void present(const uint32_t* rgba, int w, int h, const uint32_t* panel = nullptr, int pw = 0, int ph = 0);
    // The HUD side panel (PLAN-HUD.md): wanted or not (Classic, the editor, the atlas, movies).
    void set_side_panel(bool on) { side_panel_ = on; }
    // Where the picture and the panel go now (gfx/screen_layout.h); panel_px = 0: no panel.
    ScreenLayout layout() const;
    // The window's drawable size (headless: --window, else the game surface).
    void drawable_size(int& w, int& h) const;
    uint64_t ticks_ms() const;
    bool has_window() const { return window_ != nullptr; }

    bool open_audio(int rate, AudioCallback cb);
    int audio_rate() const { return audio_rate_; }

    // Output pixels per game pixel of the current output rectangle, rounded down (1..maxs):
    // the game mode composes its frame at this scale so the world can scroll in output pixels.
    int frame_scale(int maxs = 4) const;

    // Controllers (SDL GameController; in the browser Emscripten's SDL maps the Gamepad API).
    // Hot-plugging is reported as kPadAdded / kPadRemoved events.
    int controllers() const { return int(pads_.size()); }
    // Tests: a virtual controller (SDL_JoystickAttachVirtualEx) driven by these calls; its
    // events come through poll() like a real one. Returns false if SDL cannot make one.
    bool attach_virtual_pad();
    void virtual_pad_button(int button, bool down);
    void virtual_pad_axis(int axis, int value);
    void detach_virtual_pad();

    void set_scale_mode(ScaleMode m) { cfg_.scale_mode = m; }
    void toggle_aspect() { cfg_.aspect_4_3 = !cfg_.aspect_4_3; }
    // Debug mode in the game: F10 goes to the application (the debug menu); Shift+F10 is 4:3.
    void set_f10_to_app(bool on) { f10_to_app_ = on; }
    void toggle_fullscreen();
    const PlatformConfig& config() const { return cfg_; }

private:
    struct Rect { int x, y, w, h; };
    Rect output_rect() const;
    bool side_panel_ = false;
    SDL_Texture* panel_tex_ = nullptr;
    int panel_tw_ = 0, panel_th_ = 0;
    void window_to_game(int wx, int wy, int& gx, int& gy, float* fx = nullptr, float* fy = nullptr) const;
    void open_pad(int device_index);

    PlatformConfig cfg_;
    bool f10_to_app_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    unsigned audio_dev_ = 0;
    int audio_rate_ = 0;
    AudioCallback audio_cb_;
    std::vector<std::pair<int, void*>> pads_;  // instance id, SDL_GameController*
    void* vpad_ = nullptr;                      // SDL_Joystick* of the virtual controller
    int vpad_index_ = -1;
    static void audio_thunk(void* self, uint8_t* stream, int len);
};

// Writes a w x h 0xAARRGGBB image as PNG, each pixel enlarged `scale` times.
bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale = 1);
// A zlib stream of the bytes (stb's deflate, kept with the PNG writer; the story checkpoints pack).
std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& in, int quality = 12);

}  // namespace pg
