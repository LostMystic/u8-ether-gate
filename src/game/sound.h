// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/sonarc.h"
#include "data/flex.h"

namespace pg {

class World;
class AudioMixer;
class SaveWriter;
class SaveReader;

// The game side of sound and music (specs/presentation/audio.md, intrinsics/audio.md).
// It decides channels, priorities, positions, lifetimes and the music state machine
// deterministically from game frames (so `is_playing` and saves do not depend on the audio
// device), and drives an AudioMixer if one is attached (none in headless tests).
class SoundSystem {
public:
    explicit SoundSystem(World& w) : w_(w) {}
    void set_mixer(AudioMixer* m);
    AudioMixer* mixer() const { return mixer_; }

    static constexpr int kNormalChannels = 16, kAmbientChannels = 4;
    static constexpr int kDefaultPriority = 0x60, kSpeechPriority = 200;

    // --- Sound effects (audio.md 1). loops: 0/1 once, n times, -1 forever. volume 0..255
    // is the base volume factor; pitch is 16.16 (0x10000 normal). Returns false if dropped.
    bool play_sfx(int sfx, int priority = kDefaultPriority, uint16_t item = 0, int loops = 0, int volume = 0x80,
                  uint32_t pitch = 0x10000, bool ambient = false);
    // A ready sample (movie voice, speech): unpositioned.
    bool play_sample(std::shared_ptr<const Sample> s, int priority, int volume = 255, int tag = -1);
    // 0x0EF..0x0F1: loop forever, not started twice on the same item.
    bool play_ambient(int sfx, int priority, uint16_t item);
    bool is_playing(int sfx) const;
    void set_sfx_volume(int sfx, int volume);
    // 0x0F4: instances with no item; 0x0F5: on that item (sfx -1 = everything on it).
    void stop_sfx(int sfx);
    void stop_sfx_on(int sfx, uint16_t item);
    void stop_tagged(int tag);   // samples started with play_sample(tag)
    bool tag_playing(int tag) const;
    // Map change: everything but speech stops (items.md 9 step 1).
    void stop_all(bool keep_speech);
    // An item left the fast area: its forever loops stop.
    void item_left_fast(uint16_t item);
    // Footsteps etc. (actors.md 7.7): a pitch varied by up to +-1/16.
    uint32_t random_pitch();
    // The decoded SOUND.FLX entry (cached), or null.
    std::shared_ptr<const Sample> sample(int sfx);

    // --- Speech (audio.md 3): plays the speaker's speech for a bark; returns its total
    // length in milliseconds (0: no speech pack line for it).
    unsigned speak(uint16_t speaker, unsigned shape, const std::string& text);
    void stop_speech(uint16_t speaker);
    bool speaking(uint16_t speaker) const;

    // --- Music (audio.md 2.2)
    void music_play(int track);       // intrinsic 0x0BB (normal request)
    void music_stop();                // intrinsic 0x0F8
    void combat_music(int track);     // engine: combat 98, running 110, death 44 (0 clears)
    void combat_fanfare();            // an enemy died: 109, then 98 queued
    void music_restore();             // combat ended
    void music_death();
    void music_direct(int track);     // movies, credits: a plain change, no request
    void music_slow_stop();
    void menu_opened();
    void menu_closed();
    int music_last_request() const { return last_request_; }
    bool music_combat() const { return combat_; }
    int music_wanted() const;

    // A movie is showing: its sounds play although the world is paused.
    bool movie_playing = false;
    // The movie's skip prompt is up: its sounds wait.
    bool movie_paused = false;
    // --- Per frame: positions, lifetimes, pause state.
    void frame();
    // Options: on/off and volumes (applied to the mixer).
    void apply_options();

    // Saves: the playing sounds (restarted from the beginning on load) and the music state.
    void save(SaveWriter& o) const;
    void load(SaveReader& i);
    void reset();
    // For tests / the log: "sfx@item(loop) ..." for every playing voice.
    std::string describe() const;

private:
    struct Voice {
        int sfx = 0, priority = 0, loops = 0, volume = 0, channel = -1, tag = -1;
        uint16_t item = 0, speaker = 0;
        uint32_t pitch = 0x10000;
        double left = 0;  // seconds of playback left (< 0: forever)
        bool ambient = false, speech = false;
        int last_l = 0, last_r = 0, fixed_l = -1, fixed_r = -1;
        std::shared_ptr<const Sample> s;
    };
    int pick_channel(bool ambient, int priority);
    bool start_voice(Voice v);
    void kill_channel(int ch);
    void position(const Voice& v, int& l, int& r) const;
    void next_speech(uint16_t speaker);
    void change(int track);

    World& w_;
    AudioMixer* mixer_ = nullptr;
    std::vector<std::shared_ptr<const Sample>> cache_;
    Voice ch_[kNormalChannels + kAmbientChannels];
    bool paused_ = false;
    uint32_t rng_ = 0x2545F491;
    // Music state (the mixer plays and handles transitions; this is the request logic).
    int last_request_ = 0;
    bool combat_ = false;
    int wanted_ = 0, queued_ = 0;   // mirrors for headless runs without a mixer
    int menu_depth_ = 0;
    // Speech packs: SOUND/E<shape>.FLX, loaded on demand (null = none).
    struct SpeechPack {
        Flex flex;
        std::vector<std::string> phrases;
        std::vector<std::shared_ptr<const Sample>> samples;
    };
    std::map<unsigned, std::unique_ptr<SpeechPack>> speech_;
    struct Speaking { uint16_t speaker; unsigned shape; std::string rest; int channel_tag; };
    std::vector<Speaking> speaking_;
    SpeechPack* speech_pack(unsigned shape);
    // Finds the first phrase that starts `rest` (case-insensitive); returns its number or 0.
    int match_phrase(const SpeechPack& p, const std::string& rest, size_t& used) const;
};

}  // namespace pg
