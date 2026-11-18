// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "data/shape.h"
#include "game/gumps.h"

namespace pg {

// The Ether Gate credits (an engine extra; NOTES.md "Ether Gate credits"): the main menu's Credits
// entry opens this first. The people of the game's own credits in their order, each with their
// credited role, a small sepia photo where one was found (data/credits/photos/) and a line in the
// spirit of their work, scrolling up through a 320 x 142 window; each name appears with its
// section's effect (fade, glow, type, paint, runes, build, wave, stamp, echo) as it rises in.
// Data: data/credits/ether_credits.json. Buttons: "Original Credits" (closes this and opens the
// game's own credits, CreditsGump) and "Back" (Esc or a right click too): back to the menu.
// Up / Down (and the wheel) scroll by hand; the roll ends back at the menu.
class EtherCreditsGump : public Gump {
public:
    // dir: where ether_credits.json and photos/ are (data/credits). False from load(): no file.
    EtherCreditsGump(World& w, const std::string& dir);
    ~EtherCreditsGump() override;
    bool loaded() const { return loaded_; }
    const char* kind() const override { return "ethercredits"; }
    void paint(Surface8& s) override;
    void tick() override;
    bool press(int button, int lx, int ly) override;
    void release(int button, int lx, int ly) override;
    void hover(int lx, int ly) override;
    bool key(int key, unsigned mods) override;
    void wheel(int dir);
    void on_close() override;

    enum Fx { kFade, kGlow, kType, kPaint, kRunes, kBuild, kWave, kStamp, kEcho };
    struct Entry {
        enum Type { kText, kHeading, kPerson } type = kText;
        int y = 0, h = 0;         // strip position and height
        std::string name, role, note, photo_id, link;
        std::vector<std::string> role_lines, text_lines, caption_lines;
        bool group = false;       // a group or pair photo: wide, above the name, with a caption
        bool big = false;         // kText: the large gold font
        int fx = kFade;
        bool photo_left = true;
        const ShapeFrame* photo = nullptr;
        int age = -1;             // frames since it rose into view (-1: not yet)
        mutable int nx = 0, ny = 0, nw = 0, nh = 0;  // the name on screen at the last paint (links)
    };
    const std::vector<Entry>& entries() const { return entries_; }
    int strip_height() const { return end_y_; }
    int scroll() const { return scroll_; }
    const std::string& section() const { return section_; }
    int photos_loaded() const;
    // Buttons (screen coordinates): 0 = Original Credits, 1 = Back.
    static void button_rect(int b, int& x, int& y, int& w, int& h);
    void choose(int b);  // what a click on button b does
    // The linked name under a screen point (-1: none), and what a click on it opens.
    int link_at(int sx, int sy) const;
    const std::string& last_opened() const { return last_opened_; }
    // The window the strip scrolls through.
    static constexpr int kWinY = 36, kWinH = 142;
    static Fx fx_named(const std::string& s);

private:
    void layout(const std::string& json_text);
    void paint_entry(Surface8& s, const Entry& e, int sy);
    void draw_fx_text(Surface8& s, const Entry& e, int x, int y, const std::string& text);
    const uint8_t* table(int tint, int level);  // tint 0 plain, 1 gold, 2 ether blue, 3 bright gold, 4 cream text, 5 gold text
    bool loaded_ = false;
    std::string dir_;
    std::vector<Entry> entries_;
    std::vector<std::pair<int, std::string>> sections_;  // (strip y, title)
    std::map<std::string, std::unique_ptr<ShapeFrame>> photos_;
    std::string section_;
    int end_y_ = 0, scroll_ = 0, sub_ = 0, hold_ = -1, frame_ = 0;
    int hover_ = -1, down_ = -1, hover_link_ = -1, down_link_ = -1;
    std::string last_opened_;
    int manual_ = 0;  // frames of hand scrolling left (the auto roll waits)
    struct Mote { int x, y, speed, tint; };
    std::vector<Mote> motes_;
    std::map<int, std::vector<uint8_t>> tables_;
    uint8_t white_ = 0, gold_ = 0;
};

}  // namespace pg
