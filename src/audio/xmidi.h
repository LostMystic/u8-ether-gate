// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/bytes.h"

namespace pg {

// One XMIDI event (specs/formats/music.md "EVNT stream encoding"). Note-ons carry their
// duration (XMIDI has no note-offs). Meta events other than end of track are dropped.
struct XmiEvent {
    uint32_t delay = 0;     // ticks (1/120 s) since the previous event
    uint8_t status = 0;     // 0x80..0xEF channel message, 0xFF = end of track
    uint8_t d1 = 0, d2 = 0;
    uint32_t duration = 0;  // note-on only, ticks (at least 1)
    uint32_t offset = 0;    // byte offset of the event's first delay byte in the EVNT body
};

struct XmiSequence {
    std::vector<XmiEvent> events;  // ends with an end-of-track event
    // RBRN branch points: (branch index, event index), in RBRN order.
    std::vector<std::pair<int, size_t>> branches;
    std::vector<std::pair<uint8_t, uint8_t>> timbres;  // TIMB (patch, bank)
    uint32_t length_ticks() const;  // sum of all delays (ignoring loops)
};

// Parses an XMIDI file (FORM XDIR + CAT XMID, or a bare FORM XMID). False if malformed.
bool parse_xmidi(ByteView file, std::vector<XmiSequence>& out, std::string* error = nullptr);

// MUSIC.FLX entry 0: the song list and the transition table (music.md "Entry 0").
struct MusicIndex {
    struct Song { std::string name; int track = 0, measures = 0, loop_measure = 0; };
    std::vector<Song> songs;
    // (from track, to track) -> per-measure values: 0 = cut, k > 0 = play transition k-1
    // between them, -k = overlay transition k-1 on top of the old song.
    std::map<std::pair<int, int>, std::vector<int>> transitions;
    bool parse(ByteView text);
    const Song* song(int track) const;
    // The transition value for a change at `measure` (1-based callback value); 0 if none.
    int transition(int from, int to, int measure) const;
};

// Plays one sequence: loops (controllers 0x74 / 0x75), durations turned into note-offs,
// the measure callback (0x77). Messages go to `sink(status, d1, d2)`.
class XmiPlayer {
public:
    using Sink = void (*)(void* user, uint8_t status, uint8_t d1, uint8_t d2);
    void start(const XmiSequence* seq, size_t first_event = 0);
    void stop() { seq_ = nullptr; offs_.clear(); }
    // Advances by `ticks` (may be fractional), sending every event that falls due.
    void advance(double ticks, Sink sink, void* user);
    // After starting part-way in: sends the program, controller and pitch-bend events
    // before the start point (no notes), so the instruments are right.
    void chase(Sink sink, void* user) const;
    // Sends note-offs for all sounding notes now.
    void release_notes(Sink sink, void* user);
    bool playing() const { return seq_ != nullptr && !ended_; }
    bool ended() const { return ended_; }
    int measure() const { return measure_; }
    double tempo = 1.0;      // 2.0 = double speed (overlay transitions)
    bool loop_song = true;   // at end of track: restart from the top

private:
    struct NoteOff { double at; uint8_t ch, key; };
    struct Loop { size_t start; int count; };
    const XmiSequence* seq_ = nullptr;
    size_t next_ = 0;
    double now_ = 0, next_at_ = 0;
    std::vector<NoteOff> offs_;
    std::vector<Loop> loops_;
    int measure_ = 0;
    bool ended_ = false;
};

}  // namespace pg
