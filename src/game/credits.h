// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "game/gumps.h"
#include "game/movie.h"

namespace pg {

// ECREDITS.DAT / QUOTES.DAT decoding (specs/formats/credits.md): a position-dependent XOR,
// then NUL = line break. Returns the lines.
std::vector<std::string> decode_credits(const Bytes& file);

// The credits / quotes screen (formats/credits.md "Screen and scrolling"): a black 320x200
// modal gump, the section title above a red rule, the text strip scrolling up 30 px/s through
// a 256x156 window. Esc, Space, Enter or a click asks to skip (SkipPrompt).
class CreditsGump : public Gump {
public:
    enum Kind { kCredits, kQuotes };
    CreditsGump(World& w, Kind kind, int fade_in_frames = 0);
    // The Virtues epilogue (game/virtues.h, PLAN-VIRTUES.md 6): the same screen with the engine's
    // own paragraphs (the credits' format, already decoded) and a music track.
    CreditsGump(World& w, const std::vector<std::string>& paragraphs, int music);
    const char* kind() const override { return "credits"; }
    void paint(Surface8& s) override;
    void tick() override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void on_close() override;

    struct Item {
        enum Type { kText, kRule } type = kText;
        int y = 0;          // top in strip pixels
        int x = 0;          // left in the 256-wide area (text) / rule left
        int font = 6;
        int w = 0;          // rule width
        std::string text;
    };
    const std::vector<Item>& items() const { return items_; }
    int strip_height() const { return end_y_; }
    int scroll() const { return scroll_; }
    const std::string& title() const { return title_; }
    bool finished() const { return finished_; }
    bool prompt_open() const { return prompt_.open; }
    std::function<void(bool completed)> on_end;

private:
    void layout(const std::vector<std::string>& lines);
    void handle(SkipPrompt::Result r);
    Kind kind_;
    std::vector<Item> items_;
    std::vector<std::pair<int, std::string>> titles_;  // (strip y where it takes over, text)
    std::string title_;
    size_t next_title_ = 0;
    int end_y_ = 0, scroll_ = 0, hold_ = -1;
    int fade_in_ = 0, fade_in_total_ = 0;
    bool finished_ = false;
    uint8_t rule_index_ = 0;
    SkipPrompt prompt_;
};

}  // namespace pg
