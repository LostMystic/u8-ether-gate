// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The Ether Gate credits (game/ether_credits.h, NOTES.md "Ether Gate credits").
#include "game/ether_credits.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "data/gamedata.h"
#include "data/picture.h"
#include "game/credits.h"
#include "game/world.h"
#include "gfx/text_layout.h"
#include "json/json.hpp"
#include "platform/input.h"

namespace pg {

using json = nlohmann::json;

namespace {
constexpr int kNameFont = 8, kTextFont = 6, kBigFont = 8;
constexpr int kPhotoW = 46, kPhotoH = 56, kMargin = 30, kGap = 12;
constexpr int kPersonGap = 14, kSectionGap = 34;

// UTF-8 to the fonts' code page 437 (the accented letters the names use).
std::string to_cp437(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += char(c); continue; }
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[++i]);
            static const struct { unsigned char u; unsigned char cp; } map[] = {
                {0xA9, 0x82}, {0xA8, 0x8A}, {0xAA, 0x88}, {0xA0, 0x85}, {0xA2, 0x83}, {0xA4, 0x84}, {0xB6, 0x94},
                {0xBC, 0x81}, {0xA7, 0x87}, {0x89, 0x90}, {0x9C, 0x9A}, {0x96, 0x99}, {0x84, 0x8E}, {0xAF, 0x8B}};
            char r = '?';
            for (const auto& m : map)
                if (m.u == d) r = char(m.cp);
            out += r;
            continue;
        }
        while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
        out += '?';
    }
    return out;
}

uint32_t hash2(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

std::vector<std::string> wrap(const ShapeFont& f, const std::string& text, int width) {
    std::vector<std::string> out;
    for (const TextPage& p : layout_text(f, text, width, 0))
        for (const std::string& l : p.lines) out.push_back(l);
    return out;
}
}  // namespace

EtherCreditsGump::Fx EtherCreditsGump::fx_named(const std::string& s) {
    static const char* names[] = {"fade", "glow", "type", "paint", "runes", "build", "wave", "stamp", "echo"};
    for (int i = 0; i < 9; ++i)
        if (s == names[i]) return Fx(i);
    return kFade;
}

EtherCreditsGump::EtherCreditsGump(World& w, const std::string& dir) : Gump(w), dir_(dir) {
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorArrow;
    x = y = 0;
    this->w = 320;
    h = 200;
    std::ifstream f(dir + "/ether_credits.json", std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        layout(ss.str());
        loaded_ = !entries_.empty();
    } catch (const std::exception& e) {
        PG_LOG("ether credits: %s", e.what());
        entries_.clear();
        return;
    }
    if (!loaded_) return;
    // The brightest plain colour: table(tint, level)[white_] is that tint's colour at that level.
    int best = 0;
    for (int i = 16; i < 255; ++i) {
        if (w.data().xform.loaded && w.data().xform.is_xform(uint8_t(i))) continue;
        const auto& c = w.data().palette.rgb8[size_t(i)];
        const auto& b = w.data().palette.rgb8[size_t(best)];
        if (c[0] + c[1] + c[2] > b[0] + b[1] + b[2]) best = i;
    }
    white_ = uint8_t(best);
    gold_ = table(3, 8)[white_];
    for (int i = 0; i < 48; ++i)
        motes_.push_back({int(hash2(i, 1) % 320), int(hash2(i, 2) % 200), 2 + int(hash2(i, 3) % 4), int(hash2(i, 4) % 3)});
    w.sound.stop_all(true);
    w.sound.music_direct(51);
}

EtherCreditsGump::~EtherCreditsGump() = default;

const uint8_t* EtherCreditsGump::table(int tint, int level) {
    level = std::clamp(level, 0, 8);
    const int key = tint * 16 + level;
    auto it = tables_.find(key);
    if (it != tables_.end()) return it->second.data();
    const GameData& d = w_.data();
    std::vector<uint8_t> t(256);
    for (int i = 0; i < 256; ++i) {
        const auto& c = d.palette.rgb8[size_t(i)];
        const double l = (0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]) / 255.0;
        double r = c[0], g = c[1], b = c[2];
        auto ramp = [&](double r0, double g0, double b0, double r1, double g1, double b1) {
            r = r0 + (r1 - r0) * l;
            g = g0 + (g1 - g0) * l;
            b = b0 + (b1 - b0) * l;
        };
        if (tint >= 4) {
            // Text tints: the font's own (red) glyph pixels brightened, so a whole line reads in
            // one light colour: 4 cream (roles, text), 5 warm gold (their lines).
            const double l2 = std::min(1.0, l * 2.6);
            if (tint == 4) { r = 70 + 170 * l2; g = 58 + 160 * l2; b = 40 + 138 * l2; }
            else { r = 80 + 175 * l2; g = 50 + 150 * l2; b = 10 + 80 * l2; }
        } else if (tint == 1) ramp(46, 26, 6, 255, 214, 110);
        else if (tint == 2) ramp(18, 26, 52, 160, 214, 255);
        else if (tint == 3) ramp(120, 80, 20, 255, 248, 214);
        const double k = level / 8.0;
        r *= k;
        g *= k;
        b *= k;
        int best = 0, bd = 1 << 30;
        for (int j = 16; j < 255; ++j) {
            if (d.xform.loaded && d.xform.is_xform(uint8_t(j))) continue;
            const auto& p = d.palette.rgb8[size_t(j)];
            int dr = p[0] - int(r), dg = p[1] - int(g), db = p[2] - int(b);
            int e = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (e < bd) { bd = e; best = j; }
        }
        t[size_t(i)] = uint8_t(best);
    }
    auto& slot = tables_[key];
    slot = std::move(t);
    return slot.data();
}

void EtherCreditsGump::layout(const std::string& text) {
    json j = json::parse(text);
    const ShapeFont& nf = w_.font(kNameFont);
    const ShapeFont& tf = w_.font(kTextFont);
    const ShapeFont& bf = w_.font(kBigFont);
    int y = kWinH;  // starts empty: everything rises in from the bottom
    auto add_text = [&](const std::string& raw, int fx) {
        Entry e;
        e.type = Entry::kText;
        std::string s = to_cp437(raw);
        e.big = !s.empty() && s[0] == '^';
        if (e.big) s.erase(0, 1);
        const ShapeFont& f = e.big ? bf : tf;
        e.text_lines = wrap(f, s, 256);
        e.fx = fx;
        e.y = y;
        e.h = int(e.text_lines.size()) * f.line_advance() + 3;
        y += e.h;
        entries_.push_back(e);
    };
    auto add_heading = [&](const std::string& title) {
        Entry e;
        e.type = Entry::kHeading;
        e.name = to_cp437(title);
        e.fx = kGlow;
        e.y = y;
        e.h = bf.line_advance() + 8;
        sections_.emplace_back(y + e.h, e.name);  // the header takes it once the heading has scrolled past
        y += e.h + 6;
        entries_.push_back(e);
    };
    std::vector<std::string> opening = j.value("opening", std::vector<std::string>{});
    sections_.emplace_back(0, "");
    for (const std::string& l : opening) add_text(l, kFade);
    y += kSectionGap + 20;
    bool left = true;
    std::map<std::string, std::string> cache_ids;
    for (const json& sec : j.value("sections", json::array())) {
        add_heading(sec.value("title", ""));
        const int sfx = fx_named(sec.value("fx", "fade"));
        for (const json& p : sec.value("people", json::array())) {
            Entry e;
            e.type = Entry::kPerson;
            e.name = to_cp437(p.value("name", ""));
            e.role = to_cp437(p.value("role", ""));
            e.note = to_cp437(p.value("note", ""));
            e.photo_id = p.value("group", p.value("photo", ""));
            e.group = p.contains("group");
            e.link = p.value("link", "");
            if (e.link.rfind("https://", 0) != 0) e.link.clear();
            e.fx = p.contains("fx") ? fx_named(p.value("fx", "")) : sfx;
            if (!e.photo_id.empty()) {
                auto it = photos_.find(e.photo_id);
                if (it == photos_.end()) {
                    auto fr = std::make_unique<ShapeFrame>();
                    std::string err;
                    if (!load_picture(dir_ + "/photos/" + e.photo_id + ".png", w_.data().palette, w_.data().xform, *fr, &err)) {
                        PG_LOG("ether credits: photo %s: %s", e.photo_id.c_str(), err.c_str());
                        fr.reset();
                    }
                    it = photos_.emplace(e.photo_id, std::move(fr)).first;
                }
                e.photo = it->second.get();
            }
            if (e.photo && e.group) {
                // A group photo: centred above the name, the caption under the text.
                e.role_lines = wrap(tf, e.role, 256);
                e.caption_lines = wrap(tf, to_cp437(p.value("caption", "")), 256);
                e.h = e.photo->h + 8 + nf.line_advance() + 2 + int(e.role_lines.size()) * tf.line_advance() +
                      (e.note.empty() ? 0 : 3 + tf.line_advance()) + 3 + int(e.caption_lines.size()) * tf.line_advance();
            } else if (e.photo) {
                e.photo_left = left;
                e.caption_lines = wrap(tf, to_cp437(p.value("caption", "")), 320 - 2 * kMargin - kPhotoW - kGap);
                left = !left;
                const int tw = 320 - 2 * kMargin - kPhotoW - kGap;
                e.role_lines = wrap(tf, e.role, tw);
                int th = nf.line_advance() + 2 + int(e.role_lines.size()) * tf.line_advance();
                if (!e.note.empty()) th += 3 + tf.line_advance();
                if (!e.caption_lines.empty()) th += 3 + int(e.caption_lines.size()) * tf.line_advance();
                e.h = std::max(kPhotoH + 4, th + 4);
            } else {
                e.role_lines = wrap(tf, e.role, 256);
                int th = nf.line_advance() + 2 + int(e.role_lines.size()) * tf.line_advance();
                if (!e.note.empty()) th += 3 + tf.line_advance();
                e.h = th + 2;
            }
            e.y = y;
            y += e.h + kPersonGap;
            entries_.push_back(e);
        }
        y += kSectionGap;
    }
    for (const json& c : j.value("closing", json::array())) {
        std::string hd = c.value("heading", "");
        if (!hd.empty()) add_heading(hd);
        for (const std::string& l : c.value("lines", std::vector<std::string>{})) {
            add_text(l, kFade);
            y += 4;
        }
        y += kSectionGap;
    }
    end_y_ = y;
}

int EtherCreditsGump::photos_loaded() const {
    int n = 0;
    for (const auto& p : photos_) n += p.second != nullptr;
    return n;
}

void EtherCreditsGump::tick() {
    ++frame_;
    for (Mote& m : motes_)
        if (frame_ % m.speed == 0 && --m.y < 0) { m.y = 199; m.x = int(hash2(uint32_t(frame_), uint32_t(m.x)) % 320); }
    for (Entry& e : entries_) {
        if (e.age >= 0) { ++e.age; continue; }
        if (kWinY + e.y - scroll_ < kWinY + kWinH - 46) e.age = 0;  // a little way up: the effect plays in view
    }
    for (const auto& s : sections_)
        if (scroll_ >= s.first) section_ = s.second;
    if (hold_ >= 0) {
        if (--hold_ <= 0) close();
        return;
    }
    if (manual_ > 0) { --manual_; return; }
    if (hover_link_ >= 0) return;  // the pointer on a linked name holds the roll
    // About 25 pixels a second (5 every 6 frames of 30 a second).
    if ((sub_ = (sub_ + 1) % 6) != 0) ++scroll_;
    if (end_y_ - scroll_ <= kWinH) hold_ = 150;
}

void EtherCreditsGump::wheel(int dir) {
    scroll_ = std::clamp(scroll_ - dir * 16, 0, std::max(0, end_y_ - kWinH));
    manual_ = 45;
    hold_ = -1;
}

void EtherCreditsGump::button_rect(int b, int& bx, int& by, int& bw, int& bh) {
    by = 183;
    bh = 14;
    if (b == 0) { bx = 8; bw = 96; }
    else { bw = 56; bx = 320 - 8 - bw; }
}

void EtherCreditsGump::choose(int b) {
    if (b == 0) {
        close();
        w_.gumps.add(std::make_unique<CreditsGump>(w_, CreditsGump::kCredits, 0));
    } else if (b == 1) {
        close();
    }
}

int EtherCreditsGump::link_at(int sx, int sy) const {
    if (sy < kWinY || sy >= kWinY + kWinH) return -1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.link.empty() || e.age <= 20 || e.nw == 0) continue;
        const int top = kWinY + e.y - scroll_;
        if (top > kWinY + kWinH || top + e.h < kWinY) continue;
        if (sx >= e.nx - 2 && sx < e.nx + e.nw + 2 && sy >= e.ny - 2 && sy < e.ny + e.nh + 3) return int(i);
    }
    return -1;
}

bool EtherCreditsGump::press(int button, int lx, int ly) {
    if (button == 3) { close(); return true; }
    down_link_ = button == 1 ? link_at(lx, ly) : -1;
    down_ = -1;
    for (int b = 0; b < 2; ++b) {
        int bx, by, bw, bh;
        button_rect(b, bx, by, bw, bh);
        if (lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) down_ = b;
    }
    return true;
}

void EtherCreditsGump::release(int button, int lx, int ly) {
    if (down_link_ >= 0) {
        const int l = down_link_;
        down_link_ = -1;
        if (link_at(lx, ly) == l) {
            last_opened_ = entries_[size_t(l)].link;
            if (w_.open_url) w_.open_url(last_opened_);
        }
        return;
    }
    if (down_ < 0) return;
    int bx, by, bw, bh;
    button_rect(down_, bx, by, bw, bh);
    const int b = down_;
    down_ = -1;
    if (lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) choose(b);
}

void EtherCreditsGump::hover(int lx, int ly) {
    hover_link_ = link_at(lx, ly);
    cursor = hover_link_ >= 0 ? kCursorHand : kCursorArrow;
    hover_ = -1;
    for (int b = 0; b < 2; ++b) {
        int bx, by, bw, bh;
        button_rect(b, bx, by, bw, bh);
        if (lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) hover_ = b;
    }
}

bool EtherCreditsGump::key(int k, unsigned) {
    switch (k) {
    case kKeyEscape:
    case kKeyBackspace: close(); return true;
    case 'o':
    case 'O': choose(0); return true;
    case kKeyUp: wheel(1); return true;
    case kKeyDown: wheel(-1); return true;
    case kKeyPageUp: wheel(6); return true;
    case kKeyPageDown: wheel(-6); return true;
    case kKeyHome: scroll_ = 0; manual_ = 45; return true;
    case kKeyReturn:
    case kKeySpace:
        if (hover_ >= 0) choose(hover_);
        return true;
    default: return true;
    }
}

void EtherCreditsGump::on_close() { w_.sound.music_direct(0); }

// One line of a name with its effect (the entry's age: frames since it rose into view).
void EtherCreditsGump::draw_fx_text(Surface8& s, const Entry& e, int x0, int y0, const std::string& text) {
    const ShapeFont& f = w_.font(kNameFont);
    const int age = e.age;
    if (age < 0) return;
    const int n = int(text.size());
    const int level = std::min(8, age / 2);
    const int fh = f.height();
    int cx = x0;
    auto glyph = [&](int i, int gx, int gy, const uint8_t* t) { f.draw_top(s, gx, gy, std::string(1, text[size_t(i)]), t); };
    switch (e.fx) {
    case kGlow: {
        const int pos = (age / 2) % (n + 14) - 3;
        for (int i = 0; i < n; ++i) {
            const int dist = std::abs(i - pos);
            const uint8_t* t = dist == 0 ? table(3, level) : dist == 1 ? table(3, std::min(level, 6)) : table(1, level);
            glyph(i, cx, y0, t);
            cx += f.char_width(static_cast<unsigned char>(text[size_t(i)]));
        }
        break;
    }
    case kType: {
        const int shown = std::min(n, age / 2);
        f.draw_top(s, x0, y0, text.substr(0, size_t(shown)));
        const int pen = x0 + f.text_width(text.substr(0, size_t(shown)));
        if (shown < n || (age / 8) % 2 == 0) {
            if (age < n * 2 + 48) s.fill_rect(pen + 1, y0 + 1, 4, fh - 1, gold_);
        }
        break;
    }
    case kPaint: {
        const int tw = f.text_width(text);
        const int rw = age * 6;
        s.set_clip(std::max(0, x0 - 1), kWinY, std::min(rw, tw + 2) + 1, kWinH);
        f.draw_top(s, x0, y0, text, table(1, 8));
        s.set_clip(0, kWinY, 320, kWinH);
        if (rw < tw + 6) {
            // The brush: a bright stroke at the wipe's edge, a few drops behind it.
            const int bx = x0 + rw;
            s.fill_rect(bx, y0 - 1, 2, fh + 2, table(3, 8)[white_]);
            for (int k = 0; k < 3; ++k) {
                uint32_t hsh = hash2(uint32_t(age), uint32_t(k));
                s.fill_rect(bx - 2 - int(hsh % 10), y0 + int(hsh / 16 % uint32_t(fh + 2)), 1, 1, gold_);
            }
        }
        break;
    }
    case kRunes: {
        for (int i = 0; i < n; ++i) {
            const int settle = 6 + i * 3;
            const unsigned char ch = static_cast<unsigned char>(text[size_t(i)]);
            const int cw = f.char_width(ch);
            if (ch == ' ') { cx += cw; continue; }
            if (age < settle) {
                const char r = char('A' + hash2(uint32_t(i), uint32_t(age / 3)) % 26);
                f.draw_top(s, cx, y0, std::string(1, r), table(2, std::min(6, age)));
            } else {
                glyph(i, cx, y0, age - settle < 4 ? table(3, 8) : nullptr);
            }
            cx += cw;
        }
        break;
    }
    case kBuild: {
        for (int i = 0; i < n; ++i) {
            const int start = i * 2;
            const unsigned char ch = static_cast<unsigned char>(text[size_t(i)]);
            if (age >= start) {
                const int t = age - start;
                int dy = -std::max(0, 16 - t * 3);
                if (t >= 6 && t < 8) dy = 1;
                glyph(i, cx, y0 + dy, t < 6 ? table(0, 4 + t / 2) : nullptr);
            }
            cx += f.char_width(ch);
        }
        break;
    }
    case kWave: {
        const double fall = std::max(0.0, 1.0 - age / 50.0);
        for (int i = 0; i < n; ++i) {
            const int dy = int(std::lround(3.5 * std::sin(age * 0.45 - i * 0.8) * fall));
            glyph(i, cx, y0 + dy, table(fall > 0.05 ? 2 : 0, level));
            cx += f.char_width(static_cast<unsigned char>(text[size_t(i)]));
        }
        break;
    }
    case kStamp: {
        if (age < 4) break;
        const int t = age - 4;
        const int dy = -std::max(0, 8 - t * 2);
        f.draw_top(s, x0, y0 + dy, text, t >= 4 && t < 7 ? table(3, 8) : nullptr);
        if (t >= 8) {
            // A tick mark checked off beside the name.
            const int mx = x0 - 12, my = y0 + fh / 2;
            const int len = std::min(9, (t - 8) * 2);
            for (int k = 0; k < len; ++k) {
                const int px = k < 3 ? mx + k : mx + k;
                const int py = k < 3 ? my + k : my + 2 - (k - 3);
                s.fill_rect(px, py, 2, 2, gold_);
            }
        }
        break;
    }
    case kEcho: {
        const int d = std::max(0, 18 - age);
        if (d > 0) {
            f.draw_top(s, x0 - d, y0, text, table(2, 3));
            f.draw_top(s, x0 + d, y0, text, table(2, 3));
        }
        f.draw_top(s, x0, y0, text, table(0, level));
        break;
    }
    default:
        f.draw_top(s, x0, y0, text, table(0, level));
        break;
    }
}

void EtherCreditsGump::paint_entry(Surface8& s, const Entry& e, int sy) {
    const ShapeFont& nf = w_.font(kNameFont);
    const ShapeFont& tf = w_.font(kTextFont);
    const ShapeFont& bf = w_.font(kBigFont);
    if (e.age < 0) return;
    const int level = std::min(8, e.age / 2);
    if (e.type == Entry::kHeading) {
        const int tw = bf.text_width(e.name);
        draw_fx_text(s, e, (320 - tw) / 2, sy, e.name);
        // A gold rule under it, growing out from the middle and fading at its ends.
        const int half = std::min(96, e.age * 4);
        const int ry = sy + bf.line_advance() + 3;
        for (int dx = -half; dx < half; ++dx) {
            const int fade = 8 - std::max(0, (std::abs(dx) - half + 24) / 3);
            s.fill_rect(160 + dx, ry, 1, 1, table(1, std::min(level, fade))[white_]);
        }
        return;
    }
    if (e.type == Entry::kText) {
        const ShapeFont& f = e.big ? bf : tf;
        int ly = sy;
        for (const std::string& l : e.text_lines) {
            f.draw_top(s, (320 - f.text_width(l)) / 2, ly, l, table(e.big ? 1 : 4, level));
            ly += f.line_advance();
        }
        return;
    }
    // A person: the photo in a gold frame (developing in), the name with its effect, the role,
    // then their line.
    int tx, tw;
    auto framed = [&](int px, int py) {
        const int pw = e.photo->w, ph = e.photo->h;
        const int fl = std::min(8, e.age / 3);
        s.fill_rect(px + 2, py + 2, pw + 2, ph + 2, table(0, 1)[white_]);  // shadow
        s.fill_rect(px - 2, py - 2, pw + 4, ph + 4, table(1, std::min(fl, 5))[white_]);
        s.fill_rect(px - 1, py - 1, pw + 2, ph + 2, table(1, std::min(fl, 8))[white_]);
        s.fill_rect(px, py, pw, ph, 0);
        s.draw(*e.photo, px + e.photo->xoff, py + e.photo->yoff, false, nullptr, nullptr, table(0, fl));
    };
    if (e.photo && e.group) {
        framed((320 - e.photo->w) / 2, sy + 2);
        sy += e.photo->h + 8;
        tw = 256;
        tx = 32;
    } else if (e.photo) {
        framed(e.photo_left ? kMargin : 320 - kMargin - kPhotoW, sy + 2);
        tw = 320 - 2 * kMargin - kPhotoW - kGap;
        tx = e.photo_left ? kMargin + kPhotoW + kGap : kMargin;
    } else {
        tw = 256;
        tx = 32;
    }
    // Text block: vertically centred beside a photo.
    const bool beside = e.photo && !e.group;
    int th = nf.line_advance() + 2 + int(e.role_lines.size()) * tf.line_advance() + (e.note.empty() ? 0 : 3 + tf.line_advance());
    if (beside && !e.caption_lines.empty()) th += 3 + int(e.caption_lines.size()) * tf.line_advance();
    int ty = beside ? sy + std::max(0, (kPhotoH + 4 - th) / 2) : sy;
    auto place = [&](const ShapeFont& f, const std::string& l) {
        if (!beside) return tx + (tw - f.text_width(l)) / 2;
        return e.photo_left ? tx : tx + tw - f.text_width(l);
    };
    const int nx = place(nf, e.name);
    draw_fx_text(s, e, nx, ty, e.name);
    e.nx = nx;
    e.ny = ty;
    e.nw = nf.text_width(e.name);
    e.nh = nf.height();
    if (!e.link.empty() && e.age > 20) {
        // A linked name: a dotted gold underline; solid while the pointer is on it.
        const bool lit = hover_link_ >= 0 && &entries_[size_t(hover_link_)] == &e;
        for (int dx = 0; dx < e.nw; ++dx)
            if (lit || dx % 3 != 2) s.fill_rect(nx + dx, ty + e.nh + 1, 1, 1, table(lit ? 3 : 1, lit ? 8 : 5)[white_]);
    }
    ty += nf.line_advance() + 2;
    const int rl = std::clamp((e.age - 10) / 2, 0, 8);
    for (const std::string& l : e.role_lines) {
        if (rl > 0) tf.draw_top(s, place(tf, l), ty, l, table(4, rl));
        ty += tf.line_advance();
    }
    if (!e.note.empty()) {
        ty += 3;
        const int nl = std::clamp((e.age - 24) / 2, 0, 8);
        if (nl > 0) tf.draw_top(s, place(tf, e.note), ty, e.note, table(5, nl));
        ty += tf.line_advance();
    }
    if (!e.caption_lines.empty()) {
        ty += 3;
        const int cl = std::clamp((e.age - 30) / 2, 0, 6);
        for (const std::string& l : e.caption_lines) {
            if (cl > 0) tf.draw_top(s, place(tf, l), ty, l, table(4, cl));
            ty += tf.line_advance();
        }
    }
}

void EtherCreditsGump::paint(Surface8& s) {
    s.fill_rect(0, 0, 320, 200, 0);
    // Motes of ether drifting up behind everything.
    for (const Mote& m : motes_) {
        const int lv = 2 + int(hash2(uint32_t(m.x), uint32_t(m.y / 8)) % 3);
        s.fill_rect(m.x, m.y, 1, 1, table(m.tint == 0 ? 2 : 1, lv)[white_]);
    }
    // The current section above a gold rule.
    const ShapeFont& hf = w_.font(kTextFont);
    const std::string head = section_.empty() ? std::string("U8 ETHER GATE") : section_;
    hf.draw_top(s, (320 - hf.text_width(head)) / 2, 18, head, table(1, 8));
    for (int dx = -110; dx < 110; ++dx) {
        const int fade = 8 - std::max(0, (std::abs(dx) - 80) / 4);
        s.fill_rect(160 + dx, 31, 1, 1, table(1, fade)[white_]);
    }
    s.set_clip(0, kWinY, 320, kWinH);
    for (const Entry& e : entries_) {
        const int sy = kWinY + e.y - scroll_;
        if (sy > kWinY + kWinH || sy + e.h < kWinY - 8) continue;
        paint_entry(s, e, sy);
    }
    s.reset_clip();
    // Soft edges: the strip fades out at the top and bottom of the window.
    for (int r = 0; r < 10; ++r) {
        const uint8_t* t = table(0, r * 8 / 10);
        for (int yy : {kWinY + r, kWinY + kWinH - 1 - r}) {
            uint8_t* row = s.row(yy);
            for (int xx = 0; xx < 320; ++xx) row[xx] = row[xx] ? t[row[xx]] : 0;
        }
    }
    // Where a linked name under the pointer leads, between the buttons.
    if (hover_link_ >= 0) {
        std::string u = entries_[size_t(hover_link_)].link;
        u = u.substr(u.find("//") + 2);
        if (u.rfind("www.", 0) == 0) u.erase(0, 4);
        u = u.substr(0, u.find('/'));
        const ShapeFont& f = w_.font(kTextFont);
        const std::string t = "Opens " + u;
        f.draw_top(s, 160 - f.text_width(t) / 2 + 22, 186, t, table(4, 8));
    }
    // Buttons.
    static const char* labels[2] = {"Original Credits", "Back"};
    for (int b = 0; b < 2; ++b) {
        int bx, by, bw, bh;
        button_rect(b, bx, by, bw, bh);
        const bool lit = hover_ == b || down_ == b;
        s.fill_rect(bx, by, bw, bh, table(1, lit ? 5 : 3)[white_]);
        s.fill_rect(bx + 1, by + 1, bw - 2, bh - 2, table(0, 1)[white_]);
        const ShapeFont& f = w_.font(kTextFont);
        const int tx = bx + (bw - f.text_width(labels[b])) / 2 + (down_ == b ? 1 : 0);
        const int ty = by + (bh - f.height()) / 2 + (down_ == b ? 1 : 0);
        f.draw_top(s, tx, ty, labels[b], table(lit ? 3 : 1, 8));
    }
}

}  // namespace pg
