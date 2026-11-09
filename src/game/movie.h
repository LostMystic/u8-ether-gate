// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio/sonarc.h"
#include "data/palette.h"
#include "game/gumps.h"
#include "gfx/surface.h"

namespace pg {

class Flex;

// An SKF movie (specs/formats/movies.md): the event list, palettes, delta picture frames on
// a persistent 320x200 buffer, raw voice samples and subtitles. Pure playback logic: the
// owner gets the sound / music / subtitle requests through the callbacks.
class SkfPlayer {
public:
    struct Event { uint16_t frame, code, param; };
    bool open(const Flex& skf);
    // One 30 Hz step. False once the movie has ended.
    bool tick();
    bool ended() const { return ended_; }

    // Current picture (palette indices) and its palette with the fade applied.
    const Surface8& picture() const { return buf_; }
    void palette_rgb(std::array<std::array<uint8_t, 3>, 256>& out) const;
    int frames_shown() const { return frame_; }
    int ticks() const { return ticks_; }
    int steps() const { return steps_; }
    const std::vector<Event>& events() const { return events_; }
    // Subtitle on screen ("" = none) and its y (20 or 100).
    const std::string& subtitle() const { return subtitle_; }
    int subtitle_y() const { return subtitle_y_; }

    // Decoders, exposed for tests.
    static bool decode_frame(ByteView obj, Surface8& onto);
    static bool decode_sound(ByteView obj, Sample& out);  // type 13: signed 8-bit PCM
    static std::string subtitle_text(ByteView obj, int* y = nullptr);

    std::function<void(int track)> on_music;
    std::function<void()> on_music_slow_stop;
    std::function<void(int sfx)> on_sfx;
    std::function<void(int sfx)> on_stop_sfx;
    std::function<void(std::shared_ptr<const Sample>)> on_voice;

private:
    void fire(const Event& e);
    bool advance_picture();

    const Flex* skf_ = nullptr;
    std::vector<Event> events_;
    size_t next_event_ = 0;
    unsigned next_obj_ = 1;
    int frame_ = 0;          // picture frames drawn so far
    int ticks_ = 0;
    int wait_ = 0;           // in 1/15 s steps
    int steps_ = 0;          // 1/15 s steps taken (the hold of a slow stop not counted)
    int speed_ = 0;          // the last Set speed value (unused, as in the reference)
    int hold_ = 0;           // slow music stop: hold playback while it fades
    // Fade: level 0 = picture, 16 = full black/white; direction +1 out, -1 in.
    int fade_level_ = 0, fade_dir_ = 0;
    bool fade_white_ = false;
    bool ended_ = false;
    Surface8 buf_{320, 200};
    std::array<std::array<uint8_t, 3>, 256> pal_{};  // 8-bit RGB
    std::string subtitle_;
    int subtitle_y_ = 20;
    int last_event_frame_ = 0;
};

// The skip confirmation shared by movies and the credits/quotes screens: a game-style box
// (the quit box picture, shape 17, with the Yes / No buttons 47 / 50) asking "Are you sure
// you'd like to skip this?". Mouse: click Yes / No. Keys: Y / N, arrows or Tab + Enter/Space,
// Esc = No.
class SkipPrompt {
public:
    enum Result { kNone, kYes, kNo };
    bool open = false;
    int choice = 1;  // 0 Yes, 1 No (the default, so a stray key does not skip)
    void show() { open = true; choice = 1; down_ = -1; age = 0; }
    // Frames the prompt has been up. Answers count only after kSettleFrames, so one physical
    // press delivered twice (or a pad button mapped to both "skip" and "yes") cannot open and
    // confirm it at once.
    int age = 0;
    static constexpr int kSettleFrames = 8;  // about 0.27 s
    bool settled() const { return age >= kSettleFrames; }
    void tick() { if (open) ++age; }
    void paint(Surface8& s, const World& w) const;
    Result press(const World& w, int button, int sx, int sy);
    Result release(const World& w, int button, int sx, int sy);
    void hover(const World& w, int sx, int sy);
    Result key(int key);
    // True for the keys and clicks that ask to skip (Esc, Space, Enter).
    static bool is_skip_key(int key);

private:
    void rect(const World& w, int& px, int& py, int& pw, int& ph) const;
    int button_at(const World& w, int sx, int sy) const;
    int down_ = -1;
};

// A movie shown as a modal 320x200 gump (presentation/movies.md 2): the world is paused,
// the cursor hidden. Skipping (Esc, Space, Enter or a click) first pauses the movie and its
// sound and asks "Are you sure you'd like to skip this?" (Yes / No; Y/N, Left/Right +
// Enter, Esc = No). Yes ends the movie, No resumes exactly where it paused.
class MovieGump : public Gump {
public:
    // keep_music: the new-game intro leaves its music for the game (movies.md 1).
    // fade_in_frames: the menu's fade transition (the game screen fades to black first).
    MovieGump(World& w, const Flex& skf, bool keep_music, int fade_in_frames = 0);
    const char* kind() const override { return "movie"; }
    void paint(Surface8& s) override;
    void tick() override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void on_close() override;
    // Writes the final picture: the movie through its own palette, then the subtitles and
    // the prompt through the game palette (the game surface was already converted to `rgba`).
    void compose(std::vector<uint32_t>& rgba, const Palette& game_pal) const;

    SkfPlayer player;
    bool prompt_open() const { return prompt_.open; }
    int prompt_choice() const { return prompt_.choice; }  // 0 Yes, 1 No
    void request_skip();
    void answer(bool yes);
    // Called when the movie ends (played out or skipped), before the gump closes.
    std::function<void(bool skipped)> on_end;

private:
    void paint_overlay(Surface8& s) const;
    void set_paused(bool p);
    void handle(SkipPrompt::Result r);
    bool keep_music_;
    int fade_in_ = 0, fade_in_total_ = 0;
    SkipPrompt prompt_;
    bool skipped_ = false;
    int voice_tag_ = 0;
};

}  // namespace pg
