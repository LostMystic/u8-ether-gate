// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The mixer: sample channels and the XMIDI music player (specs/presentation/audio.md 1.3-2.4,
// specs/formats/music.md). TinySoundFont (MIT, third_party/tsf) is the synthesiser.
#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "data/flex.h"

#define TSF_IMPLEMENTATION
#define TSF_STATIC
#include "tsf/tsf.h"

namespace pg {

namespace {
constexpr int kMusicBlock = 64;       // frames between sequencer steps
constexpr double kTicksPerSecond = 120.0;  // XMIDI time base (music.md "Timing")
}  // namespace

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer() {
    if (synth_) tsf_close(synth_);
}

void AudioMixer::set_rate(int rate) {
    std::lock_guard<std::mutex> l(m_);
    rate_ = rate > 0 ? rate : 44100;
    if (synth_) tsf_set_output(synth_, TSF_STEREO_INTERLEAVED, rate_, -9.0f);
    for (Channel& c : ch_)
        if (c.s) c.step = (uint64_t(c.s->rate) << 32) / uint64_t(rate_);
}

// --- Sample channels -------------------------------------------------------------------

void AudioMixer::play(int ch, std::shared_ptr<const Sample> s, int loops, uint32_t pitch, int vol_l, int vol_r) {
    if (ch < 0 || ch >= kChannels || !s || s->pcm.empty()) return;
    std::lock_guard<std::mutex> l(m_);
    Channel& c = ch_[ch];
    c.s = std::move(s);
    c.pos = 0;
    c.step = uint64_t((double(c.s->rate) * (pitch ? pitch : 0x10000) / 65536.0) / rate_ * 4294967296.0);
    c.loops = loops;
    c.vol_l = vol_l;
    c.vol_r = vol_r;
}

void AudioMixer::stop(int ch) {
    if (ch < 0 || ch >= kChannels) return;
    std::lock_guard<std::mutex> l(m_);
    ch_[ch].s.reset();
}

void AudioMixer::set_volume(int ch, int vol_l, int vol_r) {
    if (ch < 0 || ch >= kChannels) return;
    std::lock_guard<std::mutex> l(m_);
    ch_[ch].vol_l = vol_l;
    ch_[ch].vol_r = vol_r;
}

void AudioMixer::set_paused(bool p) {
    std::lock_guard<std::mutex> l(m_);
    paused_ = p;
}

void AudioMixer::set_music_paused(bool p) {
    std::lock_guard<std::mutex> l(m_);
    music_paused_ = p;
}

bool AudioMixer::channel_busy(int ch) const {
    std::lock_guard<std::mutex> l(m_);
    return ch >= 0 && ch < kChannels && ch_[ch].s != nullptr;
}

void AudioMixer::stop_all_samples() {
    std::lock_guard<std::mutex> l(m_);
    for (Channel& c : ch_) c.s.reset();
}

void AudioMixer::set_music_volume(int v) {
    std::lock_guard<std::mutex> l(m_);
    music_vol_ = std::clamp(v, 0, 255);
}

void AudioMixer::set_sfx_volume(int v) {
    std::lock_guard<std::mutex> l(m_);
    sfx_vol_ = std::clamp(v, 0, 255);
}

uint64_t AudioMixer::notes_sent() const {
    std::lock_guard<std::mutex> l(m_);
    return notes_;
}

// --- Rendering ---------------------------------------------------------------------------

void AudioMixer::render(int16_t* out, int frames) {
    std::lock_guard<std::mutex> l(m_);
    std::vector<int32_t> acc(size_t(frames) * 2, 0);
    if (!paused_) {
        for (Channel& c : ch_) {
            if (!c.s) continue;
            const uint8_t* pcm = c.s->pcm.data();
            uint64_t len = c.s->pcm.size();
            int gl = c.vol_l * sfx_vol_ / 255, gr = c.vol_r * sfx_vol_ / 255;
            for (int i = 0; i < frames; ++i) {
                uint64_t ip = c.pos >> 32;
                if (ip >= len) {
                    if (c.loops < 0 || c.loops > 1) {
                        if (c.loops > 1) --c.loops;
                        c.pos -= len << 32;
                        ip = c.pos >> 32;
                        if (ip >= len) { c.s.reset(); break; }
                    } else {
                        c.s.reset();
                        break;
                    }
                }
                // Linear interpolation between neighbouring samples.
                int a = int(pcm[ip]) - 128;
                int b = ip + 1 < len ? int(pcm[ip + 1]) - 128 : (c.loops < 0 || c.loops > 1 ? int(pcm[0]) - 128 : 0);
                int frac = int((c.pos >> 16) & 0xFFFF);
                int v = (a * 65536 + (b - a) * frac) >> 8;  // 16-bit range
                acc[size_t(i) * 2] += v * gl >> 8;
                acc[size_t(i) * 2 + 1] += v * gr >> 8;
                c.pos += c.step;
            }
        }
    }
    render_music(acc.data(), frames);
    // A soft knee above 3/4 of full scale instead of hard clipping.
    for (int i = 0; i < frames * 2; ++i) {
        int v = acc[size_t(i)] + out[i];
        int a = v < 0 ? -v : v;
        if (a > 24576) {
            double over = (a - 24576) / 8192.0;
            a = 24576 + int(8191 * over / (1 + over));
            v = v < 0 ? -a : a;
        }
        out[i] = int16_t(std::clamp(v, -32768, 32767));
    }
}

void AudioMixer::render_music(int32_t* acc, int frames) {
    if (!synth_ || music_paused_) return;
    tmp_.resize(size_t(kMusicBlock) * 2);
    int done = 0;
    while (done < frames) {
        int n = std::min(kMusicBlock, frames - done);
        double ticks = n * kTicksPerSecond / rate_;
        if (mode_ != kIdle) {
            if (mode_ == kSong || mode_ == kOverlay) main_.advance(ticks, &AudioMixer::midi_thunk, this);
            if (mode_ == kTransThen || mode_ == kOverlay) overlay_.advance(ticks, &AudioMixer::midi_thunk, this);
            update_music_state();
        }
        tsf_render_short(synth_, tmp_.data(), n, 0);
        int gain = music_vol_;
        if (fade_total_ > 0) {
            gain = int(int64_t(gain) * fade_left_ / fade_total_);
            fade_left_ -= n;
            if (fade_left_ <= 0) {
                fade_total_ = fade_left_ = 0;
                wanted_ = 0;
                queued_ = 0;
                hard_start(0);
            }
        }
        for (int i = 0; i < n * 2; ++i) acc[size_t(done) * 2 + size_t(i)] += tmp_[size_t(i)] * gain / 255;
        done += n;
    }
}

// --- Music -----------------------------------------------------------------------------

bool AudioMixer::load_soundfont(const uint8_t* data, size_t size) {
    tsf* f = tsf_load_memory(data, int(size));
    if (!f) return false;
    std::lock_guard<std::mutex> l(m_);
    if (synth_) tsf_close(synth_);
    synth_ = f;
    tsf_set_output(synth_, TSF_STEREO_INTERLEAVED, rate_, -9.0f);
    tsf_set_max_voices(synth_, 96);  // allocate now, not on the audio thread
    reset_synth_channels();
    return true;
}

void AudioMixer::set_music_flex(const Flex* music) {
    std::lock_guard<std::mutex> l(m_);
    music_ = music;
    songs_.clear();
    trans_.clear();
    if (!music) return;
    if (!index_.parse(music->entry(0))) PG_LOG("music: no song index in MUSIC.FLX");
    std::string err;
    if (music->has(258) && !parse_xmidi(music->entry(258), trans_, &err)) PG_LOG("music: trans.xmi: %s", err.c_str());
}

const std::vector<XmiSequence>* AudioMixer::song(int track) {
    auto it = songs_.find(track);
    if (it != songs_.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<XmiSequence>& v = songs_[track];
    if (music_ && track > 0 && track < 128 && music_->has(unsigned(track))) {
        std::string err;
        if (!parse_xmidi(music_->entry(unsigned(track)), v, &err)) {
            PG_LOG("music: track %d: %s", track, err.c_str());
            v.clear();
        }
    }
    return v.empty() ? nullptr : &v;
}

void AudioMixer::midi_thunk(void* self, uint8_t status, uint8_t d1, uint8_t d2) {
    static_cast<AudioMixer*>(self)->midi(status, d1, d2);
}

void AudioMixer::midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!synth_) return;
    int ch = status & 0x0F;
    switch (status & 0xF0) {
    case 0x80: tsf_channel_note_off(synth_, ch, d1); break;
    case 0x90:
        tsf_channel_note_on(synth_, ch, d1, d2 / 127.0f);
        ++notes_;
        break;
    case 0xB0: tsf_channel_midi_control(synth_, ch, d1, d2); break;
    case 0xC0: tsf_channel_set_presetnumber(synth_, ch, d1, ch == 9); break;
    case 0xE0: tsf_channel_set_pitchwheel(synth_, ch, d1 | (d2 << 7)); break;
    default: break;  // aftertouch: not used by TinySoundFont
    }
}

void AudioMixer::reset_synth_channels() {
    if (!synth_) return;
    for (int c = 0; c < 16; ++c) {
        tsf_channel_note_off_all(synth_, c);
        tsf_channel_midi_control(synth_, c, 64, 0);   // sustain off (music.md "Stopping")
        tsf_channel_midi_control(synth_, c, 121, 0);  // reset controllers
        if (c == 9) tsf_channel_set_bank_preset(synth_, c, 128, 0);
        else tsf_channel_set_bank_preset(synth_, c, 0, 0);
        // The AIL driver's open: channels 2..10 at volume 127 (music.md GM output).
        if (c >= 1 && c <= 9) tsf_channel_midi_control(synth_, c, 7, 127);
    }
}

void AudioMixer::hard_start(int track) {
    main_.release_notes(&AudioMixer::midi_thunk, this);
    overlay_.release_notes(&AudioMixer::midi_thunk, this);
    main_.stop();
    overlay_.stop();
    reset_synth_channels();
    current_ = 0;
    mode_ = kIdle;
    if (track <= 0) return;
    const std::vector<XmiSequence>* s = song(track);
    if (!s) return;  // a missing track counts as "stop music" (music.md)
    const XmiSequence& seq = (*s)[0];
    // Each later play of a track starts at its next branch point (audio.md 2.2 step 4).
    size_t first = 0;
    auto bn = branch_next_.find(track);
    if (bn == branch_next_.end()) {
        branch_next_[track] = 0;
    } else if (!seq.branches.empty()) {
        int want = bn->second;
        const std::pair<int, size_t>* hit = nullptr;
        for (const auto& b : seq.branches)
            if (b.first == want) hit = &b;
        if (!hit) {
            want = 0;
            for (const auto& b : seq.branches)
                if (b.first == 0) hit = &b;
        }
        if (hit) first = hit->second;
        bn->second = want + 1;
    }
    main_.tempo = 1.0;
    main_.loop_song = queued_ == 0;
    main_.start(&seq, first);
    if (first) main_.chase(&AudioMixer::midi_thunk, this);
    current_ = track;
    mode_ = kSong;
}

void AudioMixer::change_locked(int track) {
    wanted_ = track;
    fade_total_ = fade_left_ = 0;
    bool in_transition = mode_ == kTransThen || mode_ == kOverlay;
    if (track == current_ && !in_transition && (track == 0 || mode_ == kSong)) return;
    if (current_ == 0 || in_transition || mode_ != kSong) { hard_start(track); return; }
    int t = index_.transition(current_, track, main_.measure());
    int seq = t > 0 ? t - 1 : -t - 1;
    if (t == 0 || seq < 0 || size_t(seq) >= trans_.size()) { hard_start(track); return; }
    after_trans_ = track;
    overlay_.loop_song = false;
    if (t > 0) {
        // Stop the old song, play the transition, then the new song.
        main_.release_notes(&AudioMixer::midi_thunk, this);
        main_.stop();
        overlay_.tempo = 1.0;
        overlay_.start(&trans_[size_t(seq)]);
        mode_ = kTransThen;
    } else {
        // Overlay at double tempo on top of the old song.
        overlay_.tempo = 2.0;
        overlay_.start(&trans_[size_t(seq)]);
        mode_ = kOverlay;
    }
}

void AudioMixer::update_music_state() {
    if ((mode_ == kTransThen || mode_ == kOverlay) && !overlay_.playing()) {
        hard_start(after_trans_);
        return;
    }
    if (mode_ == kSong && main_.ended()) {
        int q = queued_;
        queued_ = 0;
        hard_start(q);
        if (q) wanted_ = q;
        else wanted_ = 0;
    }
}

void AudioMixer::music_change(int track) {
    std::lock_guard<std::mutex> l(m_);
    change_locked(track);
}

void AudioMixer::music_queue(int track) {
    std::lock_guard<std::mutex> l(m_);
    queued_ = track;
    if (mode_ == kSong) main_.loop_song = track == 0;
}

void AudioMixer::music_clear_queue() {
    std::lock_guard<std::mutex> l(m_);
    queued_ = 0;
    if (mode_ == kSong) main_.loop_song = true;
}

void AudioMixer::music_slow_stop() {
    std::lock_guard<std::mutex> l(m_);
    if (mode_ == kIdle) return;
    fade_total_ = fade_left_ = rate_ * 3 / 2;
}

void AudioMixer::music_save_and_stop() {
    std::lock_guard<std::mutex> l(m_);
    if (saved_wanted_ >= 0) return;  // already saved (nested menus)
    saved_wanted_ = wanted_;
    saved_queued_ = queued_;
    queued_ = 0;
    hard_start(0);
}

void AudioMixer::music_restore_saved() {
    std::lock_guard<std::mutex> l(m_);
    if (saved_wanted_ < 0) return;
    int w = saved_wanted_;
    queued_ = saved_queued_;
    saved_wanted_ = -1;
    wanted_ = w;
    hard_start(w);
}

int AudioMixer::music_wanted() const {
    std::lock_guard<std::mutex> l(m_);
    return saved_wanted_ >= 0 ? saved_wanted_ : wanted_;
}

int AudioMixer::music_playing() const {
    std::lock_guard<std::mutex> l(m_);
    return current_;
}

int AudioMixer::music_queued() const {
    std::lock_guard<std::mutex> l(m_);
    return queued_;
}

int AudioMixer::music_measure() const {
    std::lock_guard<std::mutex> l(m_);
    return main_.measure();
}

bool AudioMixer::music_fading() const {
    std::lock_guard<std::mutex> l(m_);
    return fade_total_ > 0;
}

}  // namespace pg
