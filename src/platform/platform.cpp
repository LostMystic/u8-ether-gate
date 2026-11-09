// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "platform/platform.h"
#include "platform/icon_data.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

namespace pg {

namespace {
int translate_key(SDL_Keycode k) {
    if (k >= 32 && k < 127) return (k >= 'A' && k <= 'Z') ? k + 32 : k;
    switch (k) {
    case SDLK_BACKSPACE: return kKeyBackspace;
    case SDLK_TAB: return kKeyTab;
    case SDLK_RETURN: case SDLK_KP_ENTER: return kKeyReturn;
    case SDLK_ESCAPE: return kKeyEscape;
    case SDLK_UP: return kKeyUp;
    case SDLK_DOWN: return kKeyDown;
    case SDLK_LEFT: return kKeyLeft;
    case SDLK_RIGHT: return kKeyRight;
    case SDLK_PAGEUP: return kKeyPageUp;
    case SDLK_PAGEDOWN: return kKeyPageDown;
    case SDLK_HOME: return kKeyHome;
    case SDLK_END: return kKeyEnd;
    case SDLK_INSERT: return kKeyInsert;
    case SDLK_DELETE: return kKeyDelete;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return kKeyF1 + (k - SDLK_F1);
    return kKeyUnknown;
}
unsigned translate_mods(Uint16 m) {
    unsigned r = 0;
    if (m & KMOD_SHIFT) r |= kModShift;
    if (m & KMOD_CTRL) r |= kModCtrl;
    if (m & KMOD_ALT) r |= kModAlt;
    return r;
}
}  // namespace

bool Platform::init(const PlatformConfig& cfg) {
    cfg_ = cfg;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (cfg.no_video) return SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS) == 0 || cfg.headless;
    Uint32 sub = SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS;
    if (SDL_Init(sub) != 0) {
        PG_LOG("SDL_Init: %s", SDL_GetError());
        if (!cfg.headless) return false;
        SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS);
        return true;
    }
    if (cfg.audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) PG_LOG("audio unavailable: %s", SDL_GetError());
    // Controllers: a focused window only (headless runs have no focus, so they take background
    // events); the devices themselves arrive as CONTROLLERDEVICEADDED events.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, cfg.headless ? "1" : "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) PG_LOG("controllers unavailable: %s", SDL_GetError());
    else if (const char* db = SDL_getenv("PAGAN_CONTROLLER_DB")) SDL_GameControllerAddMappingsFromFile(db);

    int w = cfg.game_w * cfg.window_scale;
    int h = int(cfg.game_h * cfg.window_scale * (cfg.aspect_4_3 ? 1.2f : 1.0f));
    if (cfg.window_w > 0 && cfg.window_h > 0) { w = cfg.window_w; h = cfg.window_h; }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (cfg.headless) flags = SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!window_) {
        PG_LOG("SDL_CreateWindow: %s", SDL_GetError());
        return cfg.headless;
    }
#ifndef __EMSCRIPTEN__
    // The window icon: our "U8" roundel (tools/make_icon.py, embedded in icon_data.h). The web
    // page has the same picture as its favicon.
    if (SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(kIconRGBA), kIconSize, kIconSize, 32,
                                                               kIconSize * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(window_, icon);
        SDL_FreeSurface(icon);
    }
#endif
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        PG_LOG("SDL_CreateRenderer: %s", SDL_GetError());
        if (!cfg.headless) return false;
    }
    if (!cfg.headless) SDL_ShowCursor(SDL_DISABLE);  // the game draws its own cursor
    return true;
}

void Platform::shutdown() {
    for (auto& p : pads_) SDL_GameControllerClose(static_cast<SDL_GameController*>(p.second));
    pads_.clear();
    if (vpad_) SDL_JoystickClose(static_cast<SDL_Joystick*>(vpad_));
    vpad_ = nullptr;
    if (audio_dev_) SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
    if (texture_) SDL_DestroyTexture(texture_);
    if (panel_tex_) SDL_DestroyTexture(panel_tex_);
    panel_tex_ = nullptr;
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    texture_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
    SDL_Quit();
}

uint64_t Platform::ticks_ms() const { return SDL_GetTicks64(); }

void Platform::drawable_size(int& ww, int& wh) const {
    ww = cfg_.game_w;
    wh = cfg_.game_h;
    if (renderer_) SDL_GetRendererOutputSize(renderer_, &ww, &wh);
    else if (cfg_.window_w > 0 && cfg_.window_h > 0) { ww = cfg_.window_w; wh = cfg_.window_h; }
}

ScreenLayout Platform::layout() const {
    int ww, wh;
    drawable_size(ww, wh);
    return compute_layout(ww, wh, cfg_.game_w, cfg_.game_h, cfg_.aspect_4_3, LayoutScale(int(cfg_.scale_mode)), side_panel_);
}

Platform::Rect Platform::output_rect() const {
    ScreenLayout l = layout();
    return {l.game_x, l.game_y, l.game_w, l.game_h};
}

int Platform::frame_scale(int maxs) const {
    if (!renderer_ && !(cfg_.window_w > 0 && cfg_.window_h > 0)) return 1;
    Rect r = output_rect();
    return std::clamp(r.w / std::max(1, cfg_.game_w), 1, std::max(1, maxs));
}

void Platform::open_pad(int index) {
    if (!SDL_IsGameController(index)) {
        PG_LOG("controller: joystick %d (%s) has no controller mapping", index, SDL_JoystickNameForIndex(index));
        return;
    }
    SDL_GameController* c = SDL_GameControllerOpen(index);
    if (!c) return;
    int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    for (auto& p : pads_)
        if (p.first == id) { SDL_GameControllerClose(c); return; }  // already open (the open adds a reference)
    pads_.push_back({id, c});
    PG_LOG("controller connected: %s (instance %d)", SDL_GameControllerName(c), id);
}

bool Platform::attach_virtual_pad() {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER) && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return false;
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "Pagan virtual pad";
    vpad_index_ = SDL_JoystickAttachVirtualEx(&d);
    if (vpad_index_ < 0) { PG_LOG("virtual pad: %s", SDL_GetError()); return false; }
    vpad_ = SDL_JoystickOpen(vpad_index_);
    return vpad_ != nullptr;
}

void Platform::virtual_pad_button(int b, bool down) {
    if (vpad_) SDL_JoystickSetVirtualButton(static_cast<SDL_Joystick*>(vpad_), b, down ? 1 : 0);
}

void Platform::detach_virtual_pad() {
    if (!vpad_) return;
    SDL_JoystickClose(static_cast<SDL_Joystick*>(vpad_));
    vpad_ = nullptr;
    SDL_JoystickDetachVirtual(vpad_index_);
    vpad_index_ = -1;
}

void Platform::virtual_pad_axis(int a, int v) {
    if (vpad_) SDL_JoystickSetVirtualAxis(static_cast<SDL_Joystick*>(vpad_), a, Sint16(std::clamp(v, -32768, 32767)));
}

void Platform::window_to_game(int wx, int wy, int& gx, int& gy, float* fx, float* fy) const {
    // Mouse events come in window coordinates; the output rect is in drawable pixels.
    int ww = 1, wh = 1, dw = 1, dh = 1;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (renderer_) SDL_GetRendererOutputSize(renderer_, &dw, &dh);
    double px = wx * double(dw) / std::max(1, ww), py = wy * double(dh) / std::max(1, wh);
    Rect r = output_rect();
    double dx = (px - r.x) * cfg_.game_w / std::max(1, r.w), dy = (py - r.y) * cfg_.game_h / std::max(1, r.h);
    gx = int(dx);
    gy = int(dy);
    if (fx) *fx = float(dx);
    if (fy) *fy = float(dy);
}

void Platform::toggle_fullscreen() {
    if (!window_) return;
    bool fs = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(window_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void Platform::poll(std::vector<InputEvent>& out) {
    if (cfg_.no_video) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        InputEvent ev;
        switch (e.type) {
        case SDL_QUIT:
            ev.type = InputEvent::kQuit;
            out.push_back(ev);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            ev.type = e.type == SDL_KEYDOWN ? InputEvent::kKeyDown : InputEvent::kKeyUp;
            ev.key = translate_key(e.key.keysym.sym);
            ev.mods = translate_mods(e.key.keysym.mod);
            ev.repeat = e.key.repeat != 0;
            if (ev.type == InputEvent::kKeyDown) {
                if (ev.key == kKeyF9) {
                    cfg_.scale_mode = ScaleMode((int(cfg_.scale_mode) + 1) % 3);
                    break;
                }
                if (ev.key == kKeyF10 && (!f10_to_app_ || (ev.mods & kModShift))) { toggle_aspect(); break; }
                if (ev.key == kKeyF11 || (ev.key == kKeyReturn && (ev.mods & kModAlt))) {
                    if (!ev.repeat) toggle_fullscreen();
                    break;
                }
            }
            out.push_back(ev);
            break;
        }
        case SDL_TEXTINPUT:
            ev.type = InputEvent::kText;
            std::strncpy(ev.text, e.text.text, sizeof(ev.text) - 1);
            out.push_back(ev);
            break;
        case SDL_MOUSEMOTION:
            ev.type = InputEvent::kMouseMove;
            window_to_game(e.motion.x, e.motion.y, ev.x, ev.y, &ev.fx, &ev.fy);
            out.push_back(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            ev.type = e.type == SDL_MOUSEBUTTONDOWN ? InputEvent::kMouseDown : InputEvent::kMouseUp;
            window_to_game(e.button.x, e.button.y, ev.x, ev.y, &ev.fx, &ev.fy);
            ev.button = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            ev.mods = translate_mods(Uint16(SDL_GetModState()));
            out.push_back(ev);
            break;
        case SDL_CONTROLLERDEVICEADDED: {
            size_t had = pads_.size();
            open_pad(e.cdevice.which);
            if (pads_.size() > had) {
                ev.type = InputEvent::kPadAdded;
                ev.pad = pads_.back().first;
                std::strncpy(ev.name, SDL_GameControllerName(static_cast<SDL_GameController*>(pads_.back().second)) ?: "controller",
                             sizeof(ev.name) - 1);
                out.push_back(ev);
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            for (size_t i = 0; i < pads_.size(); ++i)
                if (pads_[i].first == e.cdevice.which) {
                    SDL_GameControllerClose(static_cast<SDL_GameController*>(pads_[i].second));
                    pads_.erase(pads_.begin() + long(i));
                    PG_LOG("controller disconnected (instance %d)", e.cdevice.which);
                    ev.type = InputEvent::kPadRemoved;
                    ev.pad = e.cdevice.which;
                    out.push_back(ev);
                    break;
                }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (e.cbutton.button >= kPadButtons) break;
            ev.type = e.type == SDL_CONTROLLERBUTTONDOWN ? InputEvent::kPadDown : InputEvent::kPadUp;
            ev.button = e.cbutton.button;
            ev.pad = e.cbutton.which;
            out.push_back(ev);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (e.caxis.axis >= kPadAxes) break;
            ev.type = InputEvent::kPadAxis;
            ev.axis = e.caxis.axis;
            ev.value = e.caxis.value;
            ev.pad = e.caxis.which;
            out.push_back(ev);
            break;
        case SDL_MOUSEWHEEL:
            ev.type = InputEvent::kWheel;
            ev.wheel = e.wheel.y;
            {
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                window_to_game(mx, my, ev.x, ev.y, &ev.fx, &ev.fy);
            }
            out.push_back(ev);
            break;
        default: break;
        }
    }
}

void Platform::present(const uint32_t* rgba, int w, int h, const uint32_t* panel, int pw, int ph) {
    if (!renderer_) return;
#ifdef __EMSCRIPTEN__
    // The page stretches the canvas over the window; keep the drawable in step with it.
    double cw = 0, ch = 0;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    int ww, wh;
    SDL_GetWindowSize(window_, &ww, &wh);
    if (cw >= 1 && ch >= 1 && (int(cw) != ww || int(ch) != wh)) {
        SDL_SetWindowSize(window_, int(cw), int(ch));
    }
#endif
    if (!texture_ || tex_w_ != w || tex_h_ != h) {
        if (texture_) SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w_ = w;
        tex_h_ = h;
        if (!texture_) { PG_LOG("SDL_CreateTexture: %s", SDL_GetError()); return; }
    }
    SDL_UpdateTexture(texture_, nullptr, rgba, w * 4);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    Rect r = output_rect();
    SDL_Rect dst{r.x, r.y, r.w, r.h};
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
    // The HUD side panel (PLAN-HUD.md), one panel pixel per game pixel, in its own rectangle.
    ScreenLayout l = layout();
    if (panel && pw > 0 && ph > 0 && l.has_panel()) {
        if (!panel_tex_ || panel_tw_ != pw || panel_th_ != ph) {
            if (panel_tex_) SDL_DestroyTexture(panel_tex_);
            panel_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
            panel_tw_ = pw;
            panel_th_ = ph;
        }
        if (panel_tex_) {
            SDL_UpdateTexture(panel_tex_, nullptr, panel, pw * 4);
            SDL_Rect pd{l.panel_x, l.panel_y, l.panel_w, l.panel_h};
            SDL_RenderCopy(renderer_, panel_tex_, nullptr, &pd);
        }
    }
    SDL_RenderPresent(renderer_);
}

void Platform::audio_thunk(void* self, uint8_t* stream, int len) {
    auto* p = static_cast<Platform*>(self);
    std::memset(stream, 0, size_t(len));
    if (p->audio_cb_) p->audio_cb_(reinterpret_cast<int16_t*>(stream), len / 4);
}

bool Platform::open_audio(int rate, AudioCallback cb) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) return false;
    audio_cb_ = std::move(cb);
    SDL_AudioSpec want{}, have{};
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Platform::audio_thunk;
    want.userdata = this;
    audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!audio_dev_) {
        PG_LOG("SDL_OpenAudioDevice: %s", SDL_GetError());
        return false;
    }
    audio_rate_ = have.freq;
    SDL_PauseAudioDevice(audio_dev_, 0);
    return true;
}

}  // namespace pg
