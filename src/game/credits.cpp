// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Credits and quotes (specs/formats/credits.md, presentation/gumps.md 2.17).
#include "game/credits.h"

#include <algorithm>

#include "core/config.h"
#include "data/gamedata.h"
#include "game/world.h"
#include "gfx/text_layout.h"
#include "platform/input.h"

namespace pg {

namespace {
constexpr int kAreaX = 32, kAreaY = 44, kAreaW = 256, kAreaH = 156;

uint8_t nearest_index(const Palette& p, int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 1; i < 256; ++i) {
        int dr = p.rgb8[size_t(i)][0] - r, dg = p.rgb8[size_t(i)][1] - g, db = p.rgb8[size_t(i)][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return uint8_t(best);
}
}  // namespace

std::vector<std::string> decode_credits(const Bytes& f) {
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < f.size(); ++i) {
        uint8_t k;
        if (i < 2) k = 0;
        else if (i == 2) k = 0xE1;
        else {
            size_t q = (i / 64) % 4;
            k = uint8_t((32 * (i + 1) + i / 2 + (i % 64) * q * 64) & 0xFF);
        }
        uint8_t c = f[i] ^ k;
        if (c == 0) lines.emplace_back();
        else lines.back() += char(c);
    }
    return lines;
}

CreditsGump::CreditsGump(World& w, Kind kind, int fade_in_frames)
    : Gump(w), kind_(kind), fade_in_(fade_in_frames), fade_in_total_(fade_in_frames) {
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHidden;
    x = y = 0;
    this->w = 320;
    h = 200;
    rule_index_ = nearest_index(w.data().palette, 0xD4, 0x30, 0x30);
    layout(decode_credits(kind == kCredits ? w.data().credits : w.data().quotes));
    w.sound.stop_all(true);
    w.sound.music_direct(kind == kCredits ? 51 : 113);
}

CreditsGump::CreditsGump(World& w, const std::vector<std::string>& paragraphs, int music)
    : Gump(w), kind_(kCredits) {
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHidden;
    x = y = 0;
    this->w = 320;
    h = 200;
    rule_index_ = nearest_index(w.data().palette, 0xD4, 0x30, 0x30);
    layout(paragraphs);
    w.sound.stop_all(true);
    w.sound.music_direct(music);
}

void CreditsGump::layout(const std::vector<std::string>& lines) {
    int gap = kind_ == kCredits ? 24 : 80;
    int y = kAreaH;  // the strip starts empty: the text rises in from the bottom
    bool first_title = true, ended = false;
    for (const std::string& para : lines) {
        if (ended) break;
        if (para.empty()) continue;
        if (para[0] == '+') {
            std::string t = para.substr(1, para.find('*') == std::string::npos ? std::string::npos : para.find('*') - 1);
            if (first_title) { title_ = t; first_title = false; }
            else { titles_.emplace_back(y, t); y += 160; }
            continue;
        }
        int font = 6, indent = 0;
        bool centre = false;
        size_t pos = 0;
        while (pos <= para.size() && !ended) {
            size_t star = para.find('*', pos);
            std::string seg = para.substr(pos, star == std::string::npos ? std::string::npos : star - pos);
            pos = star == std::string::npos ? para.size() + 1 : star + 1;
            bool modifier = false;
            if (!seg.empty()) {
                char m = seg[0];
                if (m == '&') { font = 8; centre = true; modifier = true; }
                else if (m == '}') { font = 6; centre = true; modifier = true; }
                else if (m == '~') { font = 8; centre = false; indent = 32; modifier = true; }
                else if (m == '@') { ended = true; break; }
                if (modifier) seg.erase(0, 1);
            }
            std::string text;
            for (size_t k = 0; k < seg.size(); ++k) {
                text += seg[k];
                if (seg[k] == '%' && k + 1 < seg.size() && seg[k + 1] == '%') ++k;
            }
            if (!modifier && text.empty()) { y += 48; continue; }
            if (!text.empty() && text[0] == '&') {
                Item r;
                r.type = Item::kRule;
                r.w = std::min(192, 8 * int(text.size()));
                r.x = 128 - r.w / 2;
                r.y = y + 3;
                items_.push_back(r);
                y += 7;
                continue;
            }
            const ShapeFont& f = w_.font(font);
            int width = kAreaW - indent;
            std::vector<std::string> out;
            if (text.empty()) out.emplace_back();
            else {
                std::vector<TextPage> pages = layout_text(f, text, width, 0);
                for (const TextPage& p : pages)
                    for (const std::string& l : p.lines) out.push_back(l);
                if (out.empty()) out.emplace_back();
            }
            for (const std::string& l : out) {
                Item it;
                it.y = y;
                it.font = font;
                it.text = l;
                it.x = centre ? indent + (width - f.text_width(l)) / 2 : indent;
                items_.push_back(it);
                y += f.line_advance();
            }
        }
        if (!ended) y += gap;
    }
    end_y_ = y;
}

void CreditsGump::tick() {
    if (fade_in_ > 0) { --fade_in_; return; }
    (void)fade_in_total_;
    if (prompt_.open) { prompt_.tick(); return; }
    if (finished_) return;
    if (hold_ >= 0) {
        if (--hold_ <= 0) {
            finished_ = true;
            if (kind_ == kCredits) {
                Config::global().set_bool("quotes_seen", true);
                if (!Config::global().path().empty()) Config::global().save();
            }
            if (on_end) on_end(true);
            close();
        }
        return;
    }
    // 1 pixel every 2 engine ticks = 1 per 30 Hz frame.
    if (end_y_ - scroll_ <= kAreaH) { hold_ = 60; return; }
    ++scroll_;
    while (next_title_ < titles_.size() && scroll_ >= titles_[next_title_].first) title_ = titles_[next_title_++].second;
}

void CreditsGump::paint(Surface8& s) {
    s.fill_rect(0, 0, 320, 200, 0);
    if (fade_in_ > 0) return;
    const ShapeFont& tf = w_.font(6);
    if (!title_.empty()) tf.draw(s, 64 + (192 - tf.text_width(title_)) / 2, 34, title_);
    s.fill_rect(64, 41, 192, 1, rule_index_);
    s.set_clip(kAreaX, kAreaY, kAreaW, kAreaH);
    for (const Item& it : items_) {
        int sy = kAreaY + it.y - scroll_;
        if (sy > kAreaY + kAreaH || sy + 40 < kAreaY) continue;
        if (it.type == Item::kRule) s.fill_rect(kAreaX + it.x, sy, it.w, 1, rule_index_);
        else if (!it.text.empty()) w_.font(it.font).draw_top(s, kAreaX + it.x, sy, it.text);
    }
    s.reset_clip();
    prompt_.paint(s, w_);
}

void CreditsGump::handle(SkipPrompt::Result r) {
    if (r == SkipPrompt::kYes) {
        prompt_.open = false;
        if (on_end) on_end(false);
        close();
    } else if (r == SkipPrompt::kNo) {
        prompt_.open = false;
    }
}

bool CreditsGump::press(int button, int lx, int ly) {
    if (!prompt_.open) { prompt_.show(); return true; }
    handle(prompt_.press(w_, button, lx, ly));
    return true;
}

void CreditsGump::release(int button, int lx, int ly) {
    if (prompt_.open) handle(prompt_.release(w_, button, lx, ly));
}

void CreditsGump::hover(int lx, int ly) {
    if (prompt_.open) prompt_.hover(w_, lx, ly);
}

bool CreditsGump::key(int key, unsigned mods) {
    if (!prompt_.open) {
        if (SkipPrompt::is_skip_key(key)) prompt_.show();
        return true;
    }
    handle(prompt_.key(key));
    return true;
}

void CreditsGump::on_close() { w_.sound.music_direct(0); }

}  // namespace pg
