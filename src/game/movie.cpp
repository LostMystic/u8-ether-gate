// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// SKF movie playback (specs/formats/movies.md) and the movie gump
// (specs/presentation/movies.md 2), with the skip confirmation prompt.
#include "game/movie.h"

#include <algorithm>

#include "audio/mixer.h"
#include "core/log.h"
#include "data/flex.h"
#include "data/gamedata.h"
#include "data/shape.h"
#include "game/world.h"
#include "gfx/text_layout.h"
#include "platform/input.h"

namespace pg {

namespace {
constexpr int kFadeSteps = 16;
constexpr int kTypePalette = 1, kTypeFrame = 2, kTypeSound = 13, kTypeSubtitle = 17;
constexpr int kSlowStopHold = 45;           // 1.5 s
}  // namespace

// --- Decoders ------------------------------------------------------------------------------

bool SkfPlayer::decode_frame(ByteView obj, Surface8& onto) {
    if (obj.size < 12 || rd16(obj.data) != kTypeFrame) return false;
    const uint8_t* d = obj.data;
    const uint8_t* end = d + obj.size;
    int comp = rd16(d + 2), w = rd16(d + 4), h = rd16(d + 6);
    int x0 = -rds16(d + 8), y0 = -rds16(d + 10);
    if (comp > 1 || w > 4096 || h > 4096 || 12 + 2 * size_t(h) > obj.size) return false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = d + 12 + 2 * y + rd16(d + 12 + 2 * y);
        int sy = y0 + y;
        uint8_t* row = (sy >= 0 && sy < onto.height()) ? onto.row(sy) : nullptr;
        int x = 0;
        while (x < w) {
            if (p >= end) return false;
            x += *p++;
            if (x >= w) break;
            if (p >= end) return false;
            int n = *p++;
            bool fill = false;
            if (comp == 1) { fill = n & 1; n >>= 1; }
            if (x + n > w) return false;
            for (int i = 0; i < n; ++i) {
                uint8_t c;
                if (fill) c = p < end ? *p : 0;
                else { if (p + i >= end) return false; c = p[i]; }
                int sx = x0 + x + i;
                if (row && sx >= 0 && sx < onto.width()) row[sx] = c;
            }
            p += fill ? 1 : n;
            x += n;
        }
    }
    return true;
}

bool SkfPlayer::decode_sound(ByteView obj, Sample& out) {
    out = Sample{};
    if (obj.size < 34 || rd16(obj.data) != kTypeSound) return false;
    uint32_t n = rd32(obj.data + 2);
    out.rate = rd16(obj.data + 6);
    int channels = rd16(obj.data + 8);
    if (out.rate <= 0) out.rate = 11111;
    n = std::min<uint32_t>(n, uint32_t(obj.size - 34));
    const uint8_t* p = obj.data + 34;
    if (channels == 2) {
        // Stereo: mix to mono.
        for (uint32_t i = 0; i + 1 < n; i += 2) out.pcm.push_back(uint8_t((int(int8_t(p[i])) + int(int8_t(p[i + 1]))) / 2 + 128));
    } else {
        out.pcm.resize(n);
        for (uint32_t i = 0; i < n; ++i) out.pcm[i] = uint8_t(int8_t(p[i]) + 128);
    }
    return true;
}

std::string SkfPlayer::subtitle_text(ByteView obj, int* y) {
    if (obj.size < 6 || rd16(obj.data) != kTypeSubtitle) return "";
    if (y) *y = rd16(obj.data + 4);
    std::string s;
    for (size_t i = 6; i < obj.size && obj.data[i]; ++i) s += char(obj.data[i]);
    return s;
}

// --- Player ----------------------------------------------------------------------------------

bool SkfPlayer::open(const Flex& skf) {
    skf_ = &skf;
    events_.clear();
    ByteView ev = skf.entry(0);
    for (size_t o = 0; o + 6 <= ev.size; o += 6) {
        Event e{rd16(ev.data + o), rd16(ev.data + o + 2), rd16(ev.data + o + 4)};
        if (e.frame == 0xFFFF) break;
        events_.push_back(e);
    }
    next_event_ = 0;
    next_obj_ = 1;
    frame_ = ticks_ = steps_ = wait_ = hold_ = speed_ = 0;
    fade_level_ = fade_dir_ = 0;
    ended_ = skf.count() < 2;
    buf_.clear(0);
    for (auto& c : pal_) c = {0, 0, 0};
    subtitle_.clear();
    last_event_frame_ = 0;
    return !ended_;
}

void SkfPlayer::palette_rgb(std::array<std::array<uint8_t, 3>, 256>& out) const {
    int target = fade_white_ ? 255 : 0;
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 3; ++k) {
            int v = pal_[size_t(i)][size_t(k)];
            out[size_t(i)][size_t(k)] = uint8_t(v + (target - v) * fade_level_ / kFadeSteps);
        }
}

void SkfPlayer::fire(const Event& e) {
    switch (e.code) {
    case 3: if (on_music) on_music(e.param); break;
    case 4:
        if (on_music_slow_stop) on_music_slow_stop();
        hold_ = kSlowStopHold;
        break;
    case 5: if (on_sfx) on_sfx(e.param); break;
    case 6: if (on_stop_sfx) on_stop_sfx(e.param); break;
    case 7: speed_ = e.param; break;  // the reference ignores Set speed (movies.md Timing)
    case 8: fade_dir_ = 1; fade_white_ = false; break;
    case 15: fade_dir_ = 1; fade_white_ = true; break;
    case 9: fade_dir_ = -1; break;
    case 12: wait_ = e.param; break;  // in 1/15 s steps
    case 14: {
        if (e.param >= 1 && skf_) {
            int y = 20;
            std::string text = subtitle_text(skf_->entry(e.param - 1u), &y);
            if (!text.empty()) { subtitle_ = text; subtitle_y_ = y; }
        }
        auto s = std::make_shared<Sample>();
        if (skf_ && decode_sound(skf_->entry(e.param), *s) && on_voice) on_voice(s);
        break;
    }
    case 18: subtitle_.clear(); break;
    default: PG_LOG("movie: event code %u (param %u) ignored", e.code, e.param); break;
    }
}

bool SkfPlayer::advance_picture() {
    while (skf_ && next_obj_ < skf_->count()) {
        ByteView o = skf_->entry(next_obj_++);
        if (o.size < 2) continue;
        unsigned type = rd16(o.data);
        if (type == kTypePalette && o.size >= 770) {
            for (int i = 0; i < 256; ++i)
                for (int k = 0; k < 3; ++k) pal_[size_t(i)][size_t(k)] = uint8_t(o.data[2 + 3 * i + k] * 255 / 63);
        } else if (type == kTypeFrame) {
            if (!decode_frame(o, buf_)) PG_LOG("movie: bad frame object %u", next_obj_ - 1);
            ++frame_;
            return true;
        }
        // Empty objects take no time; sounds and subtitles are pulled in by events.
    }
    return false;
}

bool SkfPlayer::tick() {
    if (ended_) return false;
    ++ticks_;
    // Fades: 16 levels at one per 1/60 s tick, i.e. two per 30 Hz frame.
    fade_level_ = std::clamp(fade_level_ + 2 * fade_dir_, 0, kFadeSteps);
    if (hold_ > 0) { --hold_; return true; }
    // The reference step model (movies.md): a step every 1/15 s = every second frame.
    if ((ticks_ & 1) == 0) return true;
    ++steps_;
    if (wait_ > 0) { --wait_; return true; }
    while (next_event_ < events_.size() && events_[next_event_].frame <= frame_) {
        const Event& e = events_[next_event_++];
        // The lists end with a copy of their last record at frame 0: skip records that go
        // back in time (movies.md, uncertain).
        if (e.frame < last_event_frame_) continue;
        last_event_frame_ = e.frame;
        fire(e);
        if (wait_ > 0) return true;  // a Wait ends the step
    }
    if (!advance_picture()) {
        ended_ = true;
        --steps_;  // the step that finds no more pictures shows nothing
        return false;
    }
    wait_ = 1;  // every frame is held for 2 steps
    return true;
}

// --- The gump --------------------------------------------------------------------------------

MovieGump::MovieGump(World& w, const Flex& skf, bool keep_music, int fade_in_frames)
    : Gump(w), keep_music_(keep_music), fade_in_(fade_in_frames), fade_in_total_(fade_in_frames) {
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHidden;
    x = y = 0;
    this->w = 320;
    h = 200;
    static int next_tag = 200000;
    voice_tag_ = next_tag++;
    player.open(skf);
    World* wp = &w;
    int tag = voice_tag_;
    player.on_music = [wp](int t) { wp->sound.music_direct(t); };
    player.on_music_slow_stop = [wp]() { wp->sound.music_slow_stop(); };
    player.on_sfx = [wp](int s) { wp->sound.play_sfx(s, SoundSystem::kDefaultPriority, 0, 0, 255); };
    player.on_stop_sfx = [wp](int s) { wp->sound.stop_sfx(s); };
    player.on_voice = [wp, tag](std::shared_ptr<const Sample> s) {
        wp->sound.play_sample(std::move(s), SoundSystem::kDefaultPriority, 255, tag);
    };
    w.sound.stop_all(false);
    w.sound.music_direct(0);
    w.sound.movie_playing = true;
}

void MovieGump::set_paused(bool p) {
    w_.sound.movie_paused = p;
    if (AudioMixer* m = w_.sound.mixer()) m->set_music_paused(p);
}

void MovieGump::tick() {
    if (fade_in_ > 0) { --fade_in_; return; }
    if (prompt_.open) { prompt_.tick(); return; }
    if (!player.tick()) {
        if (on_end) on_end(false);
        close();
    }
}

void MovieGump::request_skip() {
    if (prompt_.open) return;
    prompt_.show();
    set_paused(true);
}

void MovieGump::answer(bool yes) {
    if (!prompt_.open) return;
    prompt_.open = false;
    if (yes) {
        skipped_ = true;
        if (on_end) on_end(true);
        close();
    } else {
        set_paused(false);
    }
}

void MovieGump::handle(SkipPrompt::Result r) {
    if (r == SkipPrompt::kYes) answer(true);
    else if (r == SkipPrompt::kNo) answer(false);
}

void MovieGump::on_close() {
    set_paused(false);
    w_.sound.stop_tagged(voice_tag_);
    w_.sound.stop_all(true);
    w_.sound.movie_playing = false;
    if (!keep_music_ || skipped_) w_.sound.music_direct(0);
}

bool MovieGump::press(int button, int lx, int ly) {
    if (fade_in_ > 0) return true;
    if (!prompt_.open) { PG_LOG("movie: skip asked (mouse button %d at %d,%d)", button, x + lx, y + ly); request_skip(); return true; }
    handle(prompt_.press(w_, button, x + lx, y + ly));
    return true;
}

void MovieGump::release(int button, int lx, int ly) {
    if (prompt_.open) handle(prompt_.release(w_, button, x + lx, y + ly));
}

void MovieGump::hover(int lx, int ly) {
    if (prompt_.open) prompt_.hover(w_, x + lx, y + ly);
}

bool MovieGump::key(int key, unsigned mods) {
    if (fade_in_ > 0) return true;
    if (!prompt_.open) {
        if (SkipPrompt::is_skip_key(key)) { PG_LOG("movie: skip asked (key %d)", key); request_skip(); }
        return true;
    }
    SkipPrompt::Result r = prompt_.key(key);
    if (r != SkipPrompt::kNone) PG_LOG("movie: prompt answered %s (key %d)", r == SkipPrompt::kYes ? "yes" : "no", key);
    handle(r);
    return true;
}

// --- The skip prompt -----------------------------------------------------------------------

bool SkipPrompt::is_skip_key(int key) { return key == kKeyEscape || key == kKeySpace || key == kKeyReturn; }

void SkipPrompt::rect(const World& w, int& px, int& py, int& pw, int& ph) const {
    const ShapeFrame* f = w.data().gumps.frame(17, 0);
    pw = f ? f->w : 125;
    ph = f ? f->h : 67;
    px = (320 - pw) / 2;
    py = (200 - ph) / 2;
}

int SkipPrompt::button_at(const World& w, int sx, int sy) const {
    int px, py, pw, ph;
    rect(w, px, py, pw, ph);
    const ShapeFrame* yb = w.data().gumps.frame(47, 0);
    const ShapeFrame* nb = w.data().gumps.frame(50, 0);
    int yx = px + 16, by = py + 38;
    if (yb && sx >= yx && sy >= by && sx < yx + yb->w && sy < by + yb->h) return 0;
    if (nb) {
        int nx = px + pw - 16 - nb->w;
        if (sx >= nx && sy >= by && sx < nx + nb->w && sy < by + nb->h) return 1;
    }
    return -1;
}

SkipPrompt::Result SkipPrompt::press(const World& w, int button, int sx, int sy) {
    down_ = button == 1 ? button_at(w, sx, sy) : -1;
    if (down_ >= 0) choice = down_;
    return kNone;
}

SkipPrompt::Result SkipPrompt::release(const World& w, int button, int sx, int sy) {
    int d = down_;
    down_ = -1;
    if (!settled()) return kNone;
    if (d >= 0 && button_at(w, sx, sy) == d) return d == 0 ? kYes : kNo;
    return kNone;
}

void SkipPrompt::hover(const World& w, int sx, int sy) {
    int b = button_at(w, sx, sy);
    if (b >= 0) choice = b;
}

SkipPrompt::Result SkipPrompt::key(int key) {
    if (!settled()) return kNone;
    switch (key) {
    case 'y': return kYes;
    case 'n': case kKeyEscape: return kNo;
    case kKeyLeft: case kKeyUp: case kKeyRight: case kKeyDown: case kKeyTab: choice = 1 - choice; return kNone;
    case kKeyReturn: case kKeySpace: return choice == 0 ? kYes : kNo;
    default: return kNone;
    }
}

void SkipPrompt::paint(Surface8& s, const World& w) const {
    if (!open) return;
    int px, py, pw, ph;
    rect(w, px, py, pw, ph);
    const ShapeArchive& g = w.data().gumps;
    if (const ShapeFrame* bg = g.frame(17, 0)) s.draw(*bg, px + bg->xoff, py + bg->yoff);
    else s.fill_rect(px, py, pw, ph, 0);
    const ShapeFont& f = w.font(0);
    std::vector<TextPage> pages = layout_text(f, "Are you sure you'd like to skip this?", pw - 16, 0);
    int top = py + 7;
    if (!pages.empty())
        for (const std::string& line : pages[0].lines) {
            f.draw_top(s, px + (pw - f.text_width(line)) / 2, top, line);
            top += f.line_advance();
        }
    const ShapeFrame* yb = g.frame(47, choice == 0 ? 1 : 0);
    const ShapeFrame* nb = g.frame(50, choice == 1 ? 1 : 0);
    if (yb) s.draw(*yb, px + 16 + yb->xoff, py + 38 + yb->yoff);
    if (nb) s.draw(*nb, px + pw - 16 - nb->w + nb->xoff, py + 38 + nb->yoff);
}

void MovieGump::paint(Surface8& s) {
    s.fill_rect(x, y, w, h, 0);  // the real picture is composed in RGBA (compose)
}

void MovieGump::paint_overlay(Surface8& s) const {
    if (!player.subtitle().empty() && fade_in_ == 0) {
        const ShapeFont& f = w_.font(6);
        std::vector<TextPage> pages = layout_text(f, player.subtitle(), 200, 0);
        int base = player.subtitle_y();
        if (!pages.empty())
            for (const std::string& line : pages[0].lines) {
                f.draw(s, 60 + (200 - f.text_width(line)) / 2, base, line);
                base += f.line_advance();
            }
    }
    prompt_.paint(s, w_);
}

void MovieGump::compose(std::vector<uint32_t>& rgba, const Palette& game_pal) const {
    if (rgba.size() < 320u * 200u) return;
    if (fade_in_ > 0) {
        // The menu's fade transition: the game screen goes to black first.
        int k = fade_in_total_ > 0 ? fade_in_ * 256 / fade_in_total_ : 0;
        for (uint32_t& p : rgba) {
            uint32_t r = (p >> 16 & 0xFF) * uint32_t(k) >> 8, g = (p >> 8 & 0xFF) * uint32_t(k) >> 8, b = (p & 0xFF) * uint32_t(k) >> 8;
            p = 0xFF000000u | r << 16 | g << 8 | b;
        }
        return;
    }
    std::array<std::array<uint8_t, 3>, 256> pal;
    player.palette_rgb(pal);
    const Surface8& pic = player.picture();
    for (int yy = 0; yy < 200; ++yy) {
        const uint8_t* src = const_cast<Surface8&>(pic).row(yy);
        uint32_t* dst = &rgba[size_t(yy) * 320];
        for (int xx = 0; xx < 320; ++xx) {
            const auto& c = pal[src[xx]];
            dst[xx] = 0xFF000000u | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
        }
    }
    if (player.subtitle().empty() && !prompt_.open) return;
    // Overlay pixels: painted twice over different backgrounds; where both agree, it is drawn.
    Surface8 a(320, 200), b(320, 200);
    a.clear(0);
    b.clear(255);
    paint_overlay(a);
    paint_overlay(b);
    for (size_t i = 0; i < 320u * 200u; ++i) {
        uint8_t pa = a.data()[i];
        if (pa != b.data()[i]) continue;
        const auto& c = game_pal.rgb8[pa];
        rgba[i] = 0xFF000000u | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
    }
}

}  // namespace pg
