// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Sound effects, speech and the music request logic (specs/presentation/audio.md,
// specs/intrinsics/audio.md, specs/formats/sound.md "Playback notes").
#include "game/sound.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "audio/mixer.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/world.h"
#include "game/world_stages.h"

namespace pg {

namespace {
constexpr int kAllChannels = SoundSystem::kNormalChannels + SoundSystem::kAmbientChannels;
std::string trim_spaces(std::string s) {
    for (char& c : s)
        if (c == '\t') c = ' ';
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}
}  // namespace

void SoundSystem::set_mixer(AudioMixer* m) {
    mixer_ = m;
    if (mixer_) {
        mixer_->set_music_flex(w_.data().music.count() ? &w_.data().music : nullptr);
        apply_options();
    }
}

void SoundSystem::apply_options() {
    if (!mixer_) return;
    const World::Options& o = w_.options;
    mixer_->set_music_volume(o.music ? o.music_volume : 0);
    mixer_->set_sfx_volume(o.sfx ? o.sfx_volume : 0);
}

std::shared_ptr<const Sample> SoundSystem::sample(int sfx) {
    const Flex& f = w_.data().sound;
    if (sfx <= 0 || unsigned(sfx) >= f.count() || !f.has(unsigned(sfx))) return nullptr;
    if (cache_.size() < f.count()) cache_.resize(f.count());
    if (!cache_[size_t(sfx)]) {
        auto s = std::make_shared<Sample>();
        if (!decode_sonarc(f.entry(unsigned(sfx)), *s)) {
            PG_LOG("sound %d: bad Sonarc data", sfx);
            return nullptr;
        }
        cache_[size_t(sfx)] = std::move(s);
    }
    return cache_[size_t(sfx)];
}

uint32_t SoundSystem::random_pitch() {
    // xorshift: independent of the world's random numbers (sound must not change the game).
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    int d = int(rng_ % 8193) - 4096;  // +-1/16 of 0x10000
    return uint32_t(0x10000 + d);
}

int SoundSystem::pick_channel(bool ambient, int priority) {
    int lo = ambient ? kNormalChannels : 0, hi = ambient ? kAllChannels : kNormalChannels;
    for (int c = lo; c < hi; ++c)
        if (ch_[c].channel < 0) return c;
    // Steal the lowest-priority sound if it is strictly lower (audio.md 1.3).
    int best = -1;
    for (int c = lo; c < hi; ++c)
        if (ch_[c].priority < priority && (best < 0 || ch_[c].priority < ch_[best].priority)) best = c;
    return best;
}

void SoundSystem::kill_channel(int c) {
    if (c < 0 || c >= kAllChannels || ch_[c].channel < 0) return;
    if (mixer_) mixer_->stop(c);
    ch_[c] = Voice{};
}

void SoundSystem::position(const Voice& v, int& l, int& r) const {
    int vol = 255, bal = 0;
    if (v.item) {
        if (const Item* it = w_.item(v.item)) {
            int x, y, z, cam[3];
            w_.location(*it, x, y, z);
            w_.camera_point(cam);
            int dx = x - cam[0], dy = y - cam[1], dz = z - cam[2];
            int sx = (dx - dy) / 4, sy = (dx + dy) / 8 - dz;
            long long d2 = 1LL * sx * sx + 1LL * sy * sy;
            long long vv = 256LL * (350LL * 350 - d2) / (350LL * 350);
            vol = int(std::clamp<long long>(vv, 0, 255));
            bal = std::clamp(sx * 127 / 160, -127, 127);
        }
    }
    vol = vol * v.volume / 256;
    // Linear balance: the far side fades out, the near side stays at full.
    l = vol * std::min(127, 127 - bal) / 127;
    r = vol * std::min(127, 127 + bal) / 127;
    l = l * 256 / 255;
    r = r * 256 / 255;
}

bool SoundSystem::start_voice(Voice v) {
    int c = pick_channel(v.ambient, v.priority);
    if (c < 0) return false;
    kill_channel(c);
    v.channel = c;
    double secs = v.s->seconds() * 65536.0 / double(v.pitch ? v.pitch : 0x10000);
    v.left = v.loops < 0 ? -1 : secs * std::max(1, v.loops);
    ch_[c] = v;
    if (mixer_) {
        int l, r;
        position(ch_[c], l, r);
        ch_[c].last_l = l;
        ch_[c].last_r = r;
        mixer_->play(c, v.s, v.loops, v.pitch, l, r);
    }
    return true;
}

bool SoundSystem::play_sfx(int sfx, int priority, uint16_t item, int loops, int volume, uint32_t pitch, bool ambient) {
    if (w_.stages && w_.stages->hush_sfx(sfx)) return false;  // virtue stages: fewer moans, skipped thunder
    if (!w_.options.sfx) return false;
    std::shared_ptr<const Sample> s = sample(sfx);
    if (!s) return false;
    Voice v;
    v.sfx = sfx;
    v.priority = priority;
    v.item = item;
    v.loops = loops;
    v.volume = std::clamp(volume, 0, 255);
    v.pitch = pitch;
    v.ambient = ambient;
    v.s = std::move(s);
    return start_voice(std::move(v));
}

bool SoundSystem::play_sample(std::shared_ptr<const Sample> s, int priority, int volume, int tag) {
    if (!s || s->pcm.empty() || !w_.options.sfx) return false;
    Voice v;
    v.sfx = -1;
    v.priority = priority;
    v.volume = volume;
    v.tag = tag;
    v.speech = true;
    v.s = std::move(s);
    return start_voice(std::move(v));
}

bool SoundSystem::play_ambient(int sfx, int priority, uint16_t item) {
    for (const Voice& v : ch_)
        if (v.channel >= 0 && v.sfx == sfx && v.item == item && v.loops < 0) return true;
    return play_sfx(sfx, priority, item, -1, 255, 0x10000, true);
}

bool SoundSystem::is_playing(int sfx) const {
    for (const Voice& v : ch_)
        if (v.channel >= 0 && v.sfx == sfx) return true;
    return false;
}

void SoundSystem::set_sfx_volume(int sfx, int volume) {
    for (Voice& v : ch_)
        if (v.channel >= 0 && v.sfx == sfx) v.volume = std::clamp(volume, 0, 255);
}

void SoundSystem::stop_sfx(int sfx) {
    for (int c = 0; c < kAllChannels; ++c)
        if (ch_[c].channel >= 0 && ch_[c].sfx == sfx && ch_[c].item == 0 && !ch_[c].speech) kill_channel(c);
}

void SoundSystem::stop_sfx_on(int sfx, uint16_t item) {
    for (int c = 0; c < kAllChannels; ++c)
        if (ch_[c].channel >= 0 && ch_[c].item == item && (sfx < 0 || ch_[c].sfx == sfx)) kill_channel(c);
}

void SoundSystem::stop_tagged(int tag) {
    for (int c = 0; c < kAllChannels; ++c)
        if (ch_[c].channel >= 0 && ch_[c].tag == tag) kill_channel(c);
}

bool SoundSystem::tag_playing(int tag) const {
    for (const Voice& v : ch_)
        if (v.channel >= 0 && v.tag == tag) return true;
    return false;
}

void SoundSystem::stop_all(bool keep_speech) {
    for (int c = 0; c < kAllChannels; ++c)
        if (ch_[c].channel >= 0 && !(keep_speech && ch_[c].speech)) kill_channel(c);
    if (!keep_speech) speaking_.clear();
}

void SoundSystem::item_left_fast(uint16_t item) {
    for (int c = 0; c < kAllChannels; ++c)
        if (ch_[c].channel >= 0 && ch_[c].item == item && ch_[c].loops < 0) kill_channel(c);
}

void SoundSystem::frame() {
    bool paused = (w_.kernel().paused() && !movie_playing) || movie_paused;
    if (paused != paused_) {
        paused_ = paused;
        if (mixer_) mixer_->set_paused(paused);
    }
    if (paused) return;
    for (int c = 0; c < kAllChannels; ++c) {
        Voice& v = ch_[c];
        if (v.channel < 0) continue;
        if (v.left >= 0) {
            v.left -= 1.0 / 30;
            if (v.left <= 0) {
                if (mixer_) mixer_->stop(c);
                v = Voice{};
                continue;
            }
        }
        if (v.item && !w_.item(v.item)) {
            // The item is gone: a loop stops; a one-shot plays out where it was.
            if (v.loops < 0) { kill_channel(c); continue; }
            v.item = 0;
            v.fixed_l = v.last_l;
            v.fixed_r = v.last_r;
        }
        if (mixer_) {
            int l, r;
            if (v.fixed_l >= 0) { l = v.fixed_l; r = v.fixed_r; }
            else position(v, l, r);
            v.last_l = l;
            v.last_r = r;
            mixer_->set_volume(c, l, r);
        }
    }
    // Speech: start the next matched phrase when the previous one finished.
    for (size_t i = 0; i < speaking_.size();) {
        if (!tag_playing(speaking_[i].channel_tag)) {
            uint16_t sp = speaking_[i].speaker;
            next_speech(sp);
            bool still = false;
            for (const Speaking& s : speaking_) still |= s.speaker == sp;
            if (!still) continue;  // erased
        }
        ++i;
    }
}

// --- Speech ------------------------------------------------------------------------------

SoundSystem::SpeechPack* SoundSystem::speech_pack(unsigned shape) {
    auto it = speech_.find(shape);
    if (it != speech_.end()) return it->second.get();
    std::unique_ptr<SpeechPack> p;
    Bytes b;
    if (w_.data().fs.read("SOUND/" + std::string(1, w_.data().language) + std::to_string(shape) + ".FLX", b)) {
        p = std::make_unique<SpeechPack>();
        if (!p->flex.load(std::move(b))) p.reset();
        else {
            ByteView t = p->flex.entry(0);
            std::string cur;
            for (size_t k = 0; k < t.size; ++k) {
                if (t.data[k] == 0) { p->phrases.push_back(trim_spaces(cur)); cur.clear(); }
                else cur += char(t.data[k]);
            }
            p->samples.resize(p->phrases.size() + 1);
            PG_LOG("speech: SOUND/%c%u.FLX, %zu phrases", w_.data().language, shape, p->phrases.size());
        }
    }
    SpeechPack* raw = p.get();
    speech_[shape] = std::move(p);
    return raw;
}

int SoundSystem::match_phrase(const SpeechPack& p, const std::string& rest, size_t& used) const {
    for (size_t k = 0; k < p.phrases.size(); ++k) {
        const std::string& ph = p.phrases[k];
        if (ph.empty() || ph.size() > rest.size()) continue;
        bool ok = true;
        for (size_t j = 0; j < ph.size() && ok; ++j) ok = std::tolower(uint8_t(ph[j])) == std::tolower(uint8_t(rest[j]));
        if (ok) { used = ph.size(); return int(k + 1); }
    }
    return 0;
}

unsigned SoundSystem::speak(uint16_t speaker, unsigned shape, const std::string& text) {
    stop_speech(speaker);
    if (!w_.options.sfx) return 0;
    SpeechPack* p = speech_pack(shape);
    if (!p) return 0;
    // Total length: walk the whole text with the same matching.
    unsigned ms = 0;
    std::string rest = trim_spaces(text);
    while (!rest.empty()) {
        size_t used = 0;
        int n = match_phrase(*p, rest, used);
        if (!n) break;
        if (!p->samples[size_t(n)]) {
            auto s = std::make_shared<Sample>();
            if (decode_sonarc(p->flex.entry(unsigned(n)), *s)) p->samples[size_t(n)] = s;
        }
        if (p->samples[size_t(n)]) ms += unsigned(p->samples[size_t(n)]->seconds() * 1000);
        rest = trim_spaces(rest.substr(used));
    }
    if (!ms) return 0;
    speaking_.push_back({speaker, shape, trim_spaces(text), 100000 + speaker});
    next_speech(speaker);
    return ms;
}

void SoundSystem::next_speech(uint16_t speaker) {
    for (size_t i = 0; i < speaking_.size(); ++i) {
        Speaking& s = speaking_[i];
        if (s.speaker != speaker) continue;
        SpeechPack* p = speech_pack(s.shape);
        size_t used = 0;
        int n = p && !s.rest.empty() ? match_phrase(*p, s.rest, used) : 0;
        if (!n || !p->samples[size_t(n)]) { speaking_.erase(speaking_.begin() + long(i)); return; }
        s.rest = trim_spaces(s.rest.substr(used));
        play_sample(p->samples[size_t(n)], kSpeechPriority, 255, s.channel_tag);
        return;
    }
}

void SoundSystem::stop_speech(uint16_t speaker) {
    for (size_t i = 0; i < speaking_.size(); ++i)
        if (speaking_[i].speaker == speaker) {
            stop_tagged(speaking_[i].channel_tag);
            speaking_.erase(speaking_.begin() + long(i));
            return;
        }
}

bool SoundSystem::speaking(uint16_t speaker) const {
    for (const Speaking& s : speaking_)
        if (s.speaker == speaker) return true;
    return false;
}

// --- Music -------------------------------------------------------------------------------

void SoundSystem::change(int track) {
    wanted_ = track;
    if (mixer_) mixer_->music_change(track);
}

int SoundSystem::music_wanted() const { return mixer_ ? mixer_->music_wanted() : wanted_; }

void SoundSystem::music_play(int track) {
    track &= 0x7F;
    if (w_.stages) track = w_.stages->music(track) & 0x7F;  // a lighter track at later virtue stages
    last_request_ = track;
    if (combat_) return;
    int q = mixer_ ? mixer_->music_queued() : queued_;
    if (q) {
        queued_ = track;
        if (mixer_) mixer_->music_queue(track);
    } else {
        change(track);
    }
}

void SoundSystem::music_stop() { change(0); }

void SoundSystem::combat_music(int track) {
    combat_ = track != 0;
    change(track);
}

void SoundSystem::combat_fanfare() {
    combat_music(109);
    queued_ = 98;
    if (mixer_) mixer_->music_queue(98);
}

void SoundSystem::music_restore() {
    combat_ = false;
    int q = mixer_ ? mixer_->music_queued() : queued_;
    if (q) {
        queued_ = last_request_;
        if (mixer_) mixer_->music_queue(last_request_);
    } else {
        change(last_request_);
    }
}

void SoundSystem::music_death() {
    queued_ = 0;
    if (mixer_) mixer_->music_clear_queue();
    combat_music(44);
}

void SoundSystem::music_direct(int track) {
    queued_ = 0;
    if (mixer_) mixer_->music_clear_queue();
    change(track);
}

void SoundSystem::music_slow_stop() {
    wanted_ = 0;
    if (mixer_) mixer_->music_slow_stop();
}

void SoundSystem::menu_opened() {
    if (menu_depth_++ == 0 && mixer_) mixer_->music_save_and_stop();
}

void SoundSystem::menu_closed() {
    if (menu_depth_ == 0) return;
    if (--menu_depth_ == 0 && mixer_) mixer_->music_restore_saved();
}

// --- Saves ----------------------------------------------------------------------------------

void SoundSystem::save(SaveWriter& o) const {
    o.tag("SND1");
    o.u32(uint32_t(last_request_));
    o.b(combat_);
    o.u32(uint32_t(music_wanted()));
    o.u32(uint32_t(mixer_ ? mixer_->music_queued() : queued_));
    uint32_t n = 0;
    for (const Voice& v : ch_) n += v.channel >= 0 && v.sfx > 0;
    o.u32(n);
    for (const Voice& v : ch_)
        if (v.channel >= 0 && v.sfx > 0) {
            o.u32(uint32_t(v.sfx)); o.u32(uint32_t(v.priority)); o.u16(v.item); o.i32(v.loops);
            o.u32(v.pitch); o.u32(uint32_t(v.volume)); o.b(v.ambient);
        }
}

void SoundSystem::load(SaveReader& i) {
    reset();
    if (!i.tag("SND1")) { i.set_fail(); return; }
    last_request_ = int(i.u32());
    combat_ = i.b();
    int wanted = int(i.u32());
    int queued = int(i.u32());
    uint32_t n = i.u32();
    std::vector<Voice> vs;
    for (uint32_t k = 0; k < n && !i.fail() && k < 64; ++k) {
        Voice v;
        v.sfx = int(i.u32()); v.priority = int(i.u32()); v.item = uint16_t(i.u16()); v.loops = i.i32();
        v.pitch = i.u32(); v.volume = int(i.u32()); v.ambient = i.b();
        vs.push_back(v);
    }
    change(wanted);
    queued_ = queued;
    if (mixer_ && queued) mixer_->music_queue(queued);
    for (const Voice& v : vs) play_sfx(v.sfx, v.priority, v.item, v.loops, v.volume, v.pitch, v.ambient);
}

std::string SoundSystem::describe() const {
    std::string out;
    for (const Voice& v : ch_)
        if (v.channel >= 0)
            out += std::to_string(v.sfx) + "@" + std::to_string(v.item) + (v.loops < 0 ? "(loop)" : "") + " ";
    return out;
}

void SoundSystem::reset() {
    stop_all(false);
    last_request_ = 0;
    combat_ = false;
    queued_ = 0;
    menu_depth_ = 0;
    change(0);
    if (mixer_) { mixer_->music_clear_queue(); mixer_->music_restore_saved(); mixer_->music_change(0); }
}

}  // namespace pg
