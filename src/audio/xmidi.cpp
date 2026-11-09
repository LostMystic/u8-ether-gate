// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// XMIDI parsing and sequencing, written from specs/formats/music.md and
// specs/presentation/audio.md 2.3.
#include "audio/xmidi.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace pg {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool tag_is(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

bool read_vlq(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    v = 0;
    for (int n = 0; n < 5; ++n) {
        if (p >= end) return false;
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool parse_evnt(const uint8_t* d, size_t n, XmiSequence& seq, std::string* err) {
    const uint8_t* p = d;
    const uint8_t* end = d + n;
    while (p < end) {
        XmiEvent e;
        e.offset = uint32_t(p - d);
        while (p < end && *p < 0x80) e.delay += *p++;
        if (p >= end) break;
        uint8_t st = *p++;
        e.status = st;
        uint8_t hi = st & 0xF0;
        if (hi == 0x90) {
            if (end - p < 2) break;
            e.d1 = *p++;
            e.d2 = *p++;
            if (!read_vlq(p, end, e.duration)) break;
            if (e.duration == 0) e.duration = 1;
            if (e.d2 == 0) { e.status = uint8_t(0x80 | (st & 0x0F)); e.d2 = 0x40; e.duration = 0; }
        } else if (hi == 0x80 || hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
            if (end - p < 2) break;
            e.d1 = *p++;
            e.d2 = *p++;
        } else if (hi == 0xC0 || hi == 0xD0) {
            if (end - p < 1) break;
            e.d1 = *p++;
        } else if (st == 0xFF) {
            if (p >= end) break;
            uint8_t type = *p++;
            uint32_t len;
            if (!read_vlq(p, end, len) || uint32_t(end - p) < len) break;
            p += len;
            if (type == 0x2F) {
                seq.events.push_back(e);
                return true;
            }
            // Other meta events (tempo leftovers etc.) are dropped; keep their delay.
            if (e.delay) {
                XmiEvent nop = e;
                nop.status = 0;  // a pure delay
                seq.events.push_back(nop);
            }
            continue;
        } else if (st == 0xF0 || st == 0xF7) {
            uint32_t len;
            if (!read_vlq(p, end, len) || uint32_t(end - p) < len) break;
            p += len;
            if (e.delay) { e.status = 0; seq.events.push_back(e); }
            continue;
        } else {
            if (err) *err = "bad status byte";
            return false;
        }
        seq.events.push_back(e);
    }
    // No end of track: add one.
    XmiEvent eot;
    eot.status = 0xFF;
    eot.offset = uint32_t(n);
    seq.events.push_back(eot);
    return true;
}

bool parse_form_xmid(const uint8_t* d, size_t n, XmiSequence& seq, std::string* err) {
    // d points after "FORM" len "XMID": a run of chunks.
    std::vector<std::pair<int, uint32_t>> rbrn;
    size_t pos = 0;
    bool have_evnt = false;
    while (pos + 8 <= n) {
        const uint8_t* c = d + pos;
        uint32_t len = be32(c + 4);
        if (pos + 8 + len > n) len = uint32_t(n - pos - 8);
        const uint8_t* body = c + 8;
        if (tag_is(c, "TIMB") && len >= 2) {
            unsigned cnt = rd16(body);
            for (unsigned k = 0; k < cnt && 2 + 2 * k + 1 < len; ++k) seq.timbres.emplace_back(body[2 + 2 * k], body[3 + 2 * k]);
        } else if (tag_is(c, "RBRN") && len >= 2) {
            unsigned cnt = rd16(body);
            for (unsigned k = 0; k < cnt && 2 + 6 * k + 6 <= len; ++k)
                rbrn.emplace_back(int(rd16(body + 2 + 6 * k)), rd32(body + 4 + 6 * k));
        } else if (tag_is(c, "EVNT")) {
            if (!parse_evnt(body, len, seq, err)) return false;
            have_evnt = true;
        }
        pos += 8 + len + (len & 1);
    }
    if (!have_evnt) { if (err) *err = "no EVNT chunk"; return false; }
    for (auto& b : rbrn) {
        // The offset points into the branch's controller event (past its delay bytes).
        size_t idx = 0;
        while (idx + 1 < seq.events.size() && seq.events[idx + 1].offset <= b.second) ++idx;
        seq.branches.emplace_back(b.first, idx);
    }
    return true;
}

}  // namespace

uint32_t XmiSequence::length_ticks() const {
    uint32_t t = 0;
    for (const XmiEvent& e : events) t += e.delay;
    return t;
}

bool parse_xmidi(ByteView file, std::vector<XmiSequence>& out, std::string* err) {
    out.clear();
    const uint8_t* d = file.data;
    size_t n = file.size;
    size_t pos = 0;
    while (pos + 12 <= n) {
        const uint8_t* c = d + pos;
        uint32_t len = be32(c + 4);
        if (pos + 8 + len > n) len = uint32_t(n - pos - 8);
        if (tag_is(c, "FORM") && tag_is(c + 8, "XDIR")) {
            // INFO: the sequence count; nothing else needed.
        } else if (tag_is(c, "CAT ") && tag_is(c + 8, "XMID")) {
            size_t p2 = 12;
            while (p2 + 12 <= 8 + size_t(len)) {
                const uint8_t* f = c + p2;
                uint32_t flen = be32(f + 4);
                if (p2 + 8 + flen > 8 + size_t(len)) flen = uint32_t(8 + len - p2 - 8);
                if (tag_is(f, "FORM") && tag_is(f + 8, "XMID")) {
                    XmiSequence s;
                    if (!parse_form_xmid(f + 12, flen - 4, s, err)) return false;
                    out.push_back(std::move(s));
                }
                p2 += 8 + flen + (flen & 1);
            }
        } else if (tag_is(c, "FORM") && tag_is(c + 8, "XMID")) {
            XmiSequence s;
            if (!parse_form_xmid(c + 12, len - 4, s, err)) return false;
            out.push_back(std::move(s));
        }
        pos += 8 + len + (len & 1);
    }
    if (out.empty() && err && err->empty()) *err = "no XMID sequence";
    return !out.empty();
}

// --- The song index ---------------------------------------------------------------

bool MusicIndex::parse(ByteView text) {
    songs.clear();
    transitions.clear();
    std::string s(reinterpret_cast<const char*>(text.data), text.size);
    std::istringstream in(s);
    std::string line;
    int section = 0;
    std::map<std::string, int> by_name;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == 0)) line.pop_back();
        if (!line.empty() && line[0] == '#') { ++section; continue; }
        if (line.empty()) continue;
        if (section == 0) section = 1;
        if (section == 1) {
            // name.xmi C measures loopmeasure; C may itself be a space-like character, so
            // read it at the fixed spot after the first space.
            size_t sp = line.find(' ');
            if (sp == std::string::npos || sp + 1 >= line.size()) continue;
            Song song;
            song.name = line.substr(0, sp);
            song.track = uint8_t(line[sp + 1]);
            std::istringstream rest(line.substr(sp + 2));
            rest >> song.measures >> song.loop_measure;
            for (char& ch : song.name) ch = char(std::tolower(uint8_t(ch)));
            by_name[song.name] = song.track;
            songs.push_back(song);
        } else if (section >= 4 || (section == 3 && line.find(".xmi ") != std::string::npos &&
                                    std::count(line.begin(), line.end(), ' ') >= 2)) {
            std::istringstream ls(line);
            std::string from, to, tok;
            ls >> from >> to;
            for (char& ch : from) ch = char(std::tolower(uint8_t(ch)));
            for (char& ch : to) ch = char(std::tolower(uint8_t(ch)));
            auto a = by_name.find(from), b = by_name.find(to);
            if (a == by_name.end() || b == by_name.end()) continue;
            std::vector<int> vals;
            while (ls >> tok) {
                bool overlay = tok[0] == '!';
                int v = std::atoi(tok.c_str() + (overlay ? 1 : 0));
                vals.push_back(overlay ? -v : v);
            }
            transitions[{a->second, b->second}] = vals;
        }
    }
    return !songs.empty();
}

const MusicIndex::Song* MusicIndex::song(int track) const {
    for (const Song& s : songs)
        if (s.track == track) return &s;
    return nullptr;
}

int MusicIndex::transition(int from, int to, int measure) const {
    auto it = transitions.find({from, to});
    if (it == transitions.end() || measure < 1 || measure > int(it->second.size())) return 0;
    return it->second[size_t(measure - 1)];
}

// --- Playback ------------------------------------------------------------------------

void XmiPlayer::start(const XmiSequence* seq, size_t first_event) {
    seq_ = seq;
    offs_.clear();
    loops_.clear();
    measure_ = 0;
    ended_ = false;
    now_ = 0;
    if (!seq || seq->events.empty()) { seq_ = nullptr; return; }
    next_ = std::min(first_event, seq->events.size() - 1);
    next_at_ = first_event == 0 ? seq->events[0].delay : 0;
}

void XmiPlayer::chase(Sink sink, void* user) const {
    if (!seq_) return;
    for (size_t i = 0; i < next_ && i < seq_->events.size(); ++i) {
        const XmiEvent& e = seq_->events[i];
        uint8_t hi = e.status & 0xF0;
        if (hi == 0xC0 || hi == 0xE0 || (hi == 0xB0 && !(e.d1 >= 0x6E && e.d1 <= 0x78) && e.d1 != 123))
            sink(user, e.status, e.d1, e.d2);
    }
}

void XmiPlayer::release_notes(Sink sink, void* user) {
    for (const NoteOff& o : offs_) sink(user, uint8_t(0x80 | o.ch), o.key, 0x40);
    offs_.clear();
}

void XmiPlayer::advance(double ticks, Sink sink, void* user) {
    if (!seq_) return;
    double target = now_ + ticks * tempo;
    const std::vector<XmiEvent>& ev = seq_->events;
    for (int guard = 0; guard < 100000; ++guard) {
        double t_ev = ended_ ? std::numeric_limits<double>::infinity() : next_at_;
        double limit = std::min(t_ev, target);
        // Note-offs due before the next event, earliest first.
        while (!offs_.empty()) {
            auto it = std::min_element(offs_.begin(), offs_.end(), [](const NoteOff& a, const NoteOff& b) { return a.at < b.at; });
            if (it->at > limit) break;
            sink(user, uint8_t(0x80 | it->ch), it->key, 0x40);
            offs_.erase(it);
        }
        if (t_ev > target) break;
        now_ = t_ev;
        const XmiEvent& e = ev[next_];
        size_t idx = next_;
        size_t jump = idx + 1;
        uint8_t hi = e.status & 0xF0, ch = e.status & 0x0F;
        if (e.status == 0xFF) {
            if (loop_song) {
                release_notes(sink, user);
                loops_.clear();
                jump = 0;
            } else {
                release_notes(sink, user);
                ended_ = true;
                break;
            }
        } else if (e.status == 0) {
            // a pure delay
        } else if (hi == 0x90) {
            // Re-struck key (music.md): keep an earlier-ending old note-off, else end the old
            // note now and use the new duration.
            double off_at = now_ + e.duration;
            bool keep_old = false;
            for (size_t k = 0; k < offs_.size(); ++k)
                if (offs_[k].ch == ch && offs_[k].key == e.d1) {
                    if (offs_[k].at < off_at) keep_old = true;
                    else { sink(user, uint8_t(0x80 | ch), e.d1, 0x40); offs_.erase(offs_.begin() + long(k)); }
                    break;
                }
            sink(user, e.status, e.d1, e.d2);
            if (!keep_old) offs_.push_back({off_at, ch, e.d1});
        } else if (hi == 0xB0 && e.d1 >= 0x6E && e.d1 <= 0x78) {
            // AIL controllers 110..120 never reach the synthesiser.
            if (e.d1 == 0x74) {
                if (loops_.size() < 4) loops_.push_back({idx + 1, e.d2});
                else loops_.back() = {idx + 1, e.d2};  // a fifth FOR replaces the innermost
            } else if (e.d1 == 0x75) {
                if (!loops_.empty()) {
                    Loop& l = loops_.back();
                    if (e.d2 >= 64 && (l.count == 0 || --l.count > 0)) jump = l.start;
                    else loops_.pop_back();
                }
            } else if (e.d1 == 0x77) {
                measure_ = e.d2;
            }
        } else {
            sink(user, e.status, e.d1, e.d2);
        }
        if (jump >= ev.size()) jump = ev.size() - 1;
        next_ = jump;
        next_at_ = now_ + ev[next_].delay;
    }
    now_ = target;
}

}  // namespace pg
