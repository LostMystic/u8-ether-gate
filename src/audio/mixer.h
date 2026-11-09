// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/sonarc.h"
#include "audio/xmidi.h"

struct tsf;

namespace pg {

class Flex;

// The software mixer behind the audio device (specs/presentation/audio.md): 20 sample
// channels (16 normal + 4 ambient, chosen by the game side) resampled from their own rates,
// plus the music: XMIDI sequences (MUSIC.FLX) played through TinySoundFont with the
// transition rules of audio.md 2.2. Every public call takes the lock; render() runs on the
// audio thread (or is called directly for offline recording and tests).
class AudioMixer {
public:
    static constexpr int kChannels = 20;
    AudioMixer();
    ~AudioMixer();
    void set_rate(int rate);
    int rate() const { return rate_; }
    // Adds `frames` stereo S16 frames into `out` (which the caller has cleared).
    void render(int16_t* out, int frames);

    // --- Sample channels
    // pitch: 16.16 rate factor (0x10000 = normal). loops: 0/1 once, n times, -1 forever.
    // Volumes 0..256 per side.
    void play(int ch, std::shared_ptr<const Sample> s, int loops, uint32_t pitch, int vol_l, int vol_r);
    void stop(int ch);
    void set_volume(int ch, int vol_l, int vol_r);
    void set_paused(bool paused);  // sample channels only (pausing modal gumps)
    void set_music_paused(bool paused);  // holds the sequencer and the synth (movie skip prompt)
    bool channel_busy(int ch) const;
    void stop_all_samples();

    // --- Music
    bool load_soundfont(const uint8_t* data, size_t size);
    bool has_synth() const { return synth_ != nullptr; }
    // MUSIC.FLX (kept by the caller): the song index, transition table and songs.
    void set_music_flex(const Flex* music);
    // Changes to `track` (0 = stop) using the transition table (audio.md 2.2 "Changing").
    void music_change(int track);
    // Plays `track` after the current one ends; the current one then stops looping.
    void music_queue(int track);
    void music_clear_queue();
    // Fades out over 1.5 s, then stops (movie event 4).
    void music_slow_stop();
    // Main menu: remember the wanted track and go silent / restore it.
    void music_save_and_stop();
    void music_restore_saved();
    int music_wanted() const;
    int music_playing() const;
    int music_queued() const;
    int music_measure() const;
    bool music_fading() const;

    // --- Volumes 0..255 (the options page); music on/off.
    void set_music_volume(int v);
    void set_sfx_volume(int v);

    // For tests: the number of MIDI note-ons sent so far.
    uint64_t notes_sent() const;

private:
    struct Channel {
        std::shared_ptr<const Sample> s;
        uint64_t pos = 0;   // 32.32 fixed point sample position
        uint64_t step = 0;
        int loops = 0;
        int vol_l = 0, vol_r = 0;
    };
    enum MusicMode { kIdle, kSong, kTransThen, kOverlay };
    static void midi_thunk(void* self, uint8_t status, uint8_t d1, uint8_t d2);
    void midi(uint8_t status, uint8_t d1, uint8_t d2);
    void reset_synth_channels();
    void hard_start(int track);
    void change_locked(int track);
    const std::vector<XmiSequence>* song(int track);
    void update_music_state();
    void render_music(int32_t* acc, int frames);

    mutable std::mutex m_;
    int rate_ = 44100;
    Channel ch_[kChannels];
    bool paused_ = false, music_paused_ = false;
    int music_vol_ = 255, sfx_vol_ = 255;

    tsf* synth_ = nullptr;
    const Flex* music_ = nullptr;
    MusicIndex index_;
    std::map<int, std::vector<XmiSequence>> songs_;
    std::vector<XmiSequence> trans_;
    std::map<int, int> branch_next_;
    XmiPlayer main_, overlay_;
    MusicMode mode_ = kIdle;
    int current_ = 0, wanted_ = 0, queued_ = 0, after_trans_ = 0;
    int saved_wanted_ = -1, saved_queued_ = 0;
    int fade_total_ = 0, fade_left_ = 0;
    uint64_t notes_ = 0;
    std::vector<int16_t> tmp_;
};

}  // namespace pg
