// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 5 tests: Sonarc known answers (formats/sound.md), XMIDI parsing and sequencing
// (formats/music.md), the mixer and the music player, the sound system's channel rules,
// SKF movie decoding and timing (formats/movies.md), the movie skip prompt, the credits
// decoding and screen (formats/credits.md), settings persistence, and a headless intro run.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "app/game_mode.h"
#include "audio/mixer.h"
#include "audio/sonarc.h"
#include "audio/xmidi.h"
#include "core/config.h"
#include "data/gamedata.h"
#include "game/credits.h"
#include "game/movie.h"
#include "game/world.h"
#include "platform/input.h"

using namespace pg;

namespace {

uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

void test_sonarc(const GameData& d) {
    // Known answers from formats/sound.md: entry, samples, rate, first 4 bytes, CRC-32.
    struct KA { int e; uint32_t n; int rate; uint8_t first[4]; uint32_t crc; };
    const KA ka[] = {
        {1, 4526, 11111, {128, 129, 128, 127}, 0x96b2ff1d},   {6, 40064, 11111, {127, 128, 127, 128}, 0xd24eee26},
        {7, 14990, 11111, {127, 127, 127, 127}, 0x48a7f647},  {91, 33908, 11111, {128, 124, 123, 122}, 0x2ea2423a},
        {219, 4729, 11111, {128, 127, 128, 128}, 0x6d3ca4b5}, {16, 3982, 10000, {127, 127, 128, 127}, 0x402a01b6},
        {33, 24000, 9009, {128, 128, 128, 128}, 0x33a255f1},  {34, 55781, 5000, {128, 128, 128, 128}, 0xf4f24deb},
        {94, 53376, 9708, {128, 128, 128, 128}, 0x54e97764},  {121, 855, 8000, {128, 128, 127, 126}, 0x921e66a5},
        {125, 4055, 8849, {128, 128, 128, 128}, 0xae19e9cb},  {139, 55430, 11025, {128, 128, 128, 127}, 0xfdbed058},
        {48, 35245, 11111, {127, 127, 127, 127}, 0x9b3fe572}, {49, 50956, 11111, {128, 127, 127, 127}, 0x3f18ac0d},
        {87, 30877, 11111, {172, 168, 166, 162}, 0x0ff4430f}, {116, 32550, 11111, {128, 128, 128, 128}, 0x7cf63e70},
        {134, 44931, 11111, {128, 128, 128, 128}, 0x41926613}, {191, 50684, 11025, {128, 128, 128, 128}, 0xd95a1cd5},
        {202, 17476, 11025, {129, 125, 126, 127}, 0xf61ba444},
    };
    for (const KA& k : ka) {
        Sample s;
        int bad = -1;
        CHECK(decode_sonarc(d.sound.entry(unsigned(k.e)), s, &bad));
        CHECK_EQ(s.pcm.size(), k.n);
        CHECK_EQ(s.rate, k.rate);
        CHECK_EQ(bad, 0);
        CHECK(s.pcm.size() >= 4 && std::memcmp(s.pcm.data(), k.first, 4) == 0);
        CHECK_EQ(crc32(s.pcm.data(), s.pcm.size()), k.crc);
    }
    Sample s1;
    decode_sonarc(d.sound.entry(1), s1);
    CHECK(s1.pcm.size() > 1027 && s1.pcm[1024] == 41 && s1.pcm[1025] == 110 && s1.pcm[1026] == 120 && s1.pcm[1027] == 119);
    Sample q2;
    decode_sonarc(d.sound.entry(48), q2);
    CHECK_EQ(q2.loop_start, 3811);
    CHECK_EQ(q2.loop_len, 28324);
    int decoded = 0, mismatched = 0;
    for (unsigned e = 1; e < d.sound.count(); ++e) {
        if (!d.sound.has(e)) continue;
        Sample s;
        if (decode_sonarc(d.sound.entry(e), s)) ++decoded;
        if (s.pcm.size() != rd32(d.sound.entry(e).data)) ++mismatched;
    }
    CHECK_EQ(decoded, 132);
    CHECK_EQ(mismatched, 0);
}

std::vector<std::string> g_midi;
void sink_log(void*, uint8_t st, uint8_t a, uint8_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X %d %d", st, a, b);
    g_midi.push_back(buf);
}
void sink_null(void*, uint8_t, uint8_t, uint8_t) {}

void test_xmidi(const GameData& d) {
    MusicIndex idx;
    CHECK(idx.parse(d.music.entry(0)));
    CHECK_EQ(idx.songs.size(), 33);
    const MusicIndex::Song* intro = idx.song(49);
    CHECK(intro && intro->name == "intro.xmi" && intro->measures == 10 && intro->loop_measure == 5);
    CHECK(idx.song(119) && idx.song(44) && idx.song(48));
    CHECK_EQ(idx.transition(52, 53, 1), -1);  // docks -> execute: overlay transition 0
    CHECK_EQ(idx.transition(53, 98, 1), 8);   // execute -> combat: transition 7
    CHECK_EQ(idx.transition(52, 53, 0), 0);   // no measure yet
    int parsed = 0;
    for (unsigned t = 1; t < 128; ++t) {
        if (!d.music.has(t)) continue;
        std::vector<XmiSequence> s;
        if (parse_xmidi(d.music.entry(t), s) && s.size() == 1 && s[0].events.back().status == 0xFF) ++parsed;
    }
    CHECK_EQ(parsed, 33);  // music.md: 44, 48-57, 97-99, 101-103, 105-117, 119-121
    CHECK(!d.music.has(122));
    std::vector<XmiSequence> intro_seq, docks, trans;
    CHECK(parse_xmidi(d.music.entry(49), intro_seq));
    CHECK_EQ(intro_seq[0].length_ticks(), 4513);  // about 37.6 s at 120 ticks/s
    CHECK_EQ(intro_seq[0].timbres.size(), 15);
    CHECK(parse_xmidi(d.music.entry(52), docks));
    CHECK_EQ(docks[0].branches.size(), 3);
    for (auto& b : docks[0].branches) {
        const XmiEvent& e = docks[0].events[b.second];
        CHECK(e.status == 0xB0 && e.d1 == 0x78 && e.d2 == b.first);
    }
    CHECK(parse_xmidi(d.music.entry(258), trans));
    CHECK_EQ(trans.size(), 10);
    // The intro loops forever (FOR at measure 5): after 3x its length it still plays and the
    // measure callback has come back round to the loop.
    XmiPlayer p;
    p.loop_song = false;
    p.start(&intro_seq[0]);
    int max_measure = 0;
    bool wrapped = false;
    for (int t = 0; t < 4513 * 3; ++t) {
        int before = p.measure();
        p.advance(1, sink_null, nullptr);
        max_measure = std::max(max_measure, p.measure());
        if (p.measure() < before) wrapped = true;
    }
    CHECK(p.playing());
    CHECK_EQ(max_measure, 10);
    CHECK(wrapped);
    // Hand-built: FOR 2 around one note, a duration, and the AIL controllers swallowed.
    XmiSequence s;
    auto ev = [&](uint32_t delay, uint8_t st, uint8_t a, uint8_t b, uint32_t dur = 0) {
        XmiEvent e;
        e.delay = delay; e.status = st; e.d1 = a; e.d2 = b; e.duration = dur;
        s.events.push_back(e);
    };
    ev(0, 0xC0, 5, 0);
    ev(0, 0xB0, 0x74, 2);      // FOR 2
    ev(10, 0x90, 60, 100, 5);  // note, 5 ticks
    ev(10, 0xB0, 0x75, 127);   // NEXT
    ev(0, 0xB0, 0x77, 3);      // callback
    ev(0, 0xFF, 0, 0);
    XmiPlayer q;
    q.loop_song = false;
    q.start(&s);
    g_midi.clear();
    q.advance(100, sink_log, nullptr);
    CHECK(q.ended());
    CHECK_EQ(q.measure(), 3);
    std::string joined;
    for (auto& m : g_midi) joined += m + ";";
    CHECK(joined == "C0 5 0;90 60 100;80 60 64;90 60 100;80 60 64;");
}

void test_mixer(const GameData& d, const std::string& sf_path) {
    AudioMixer m;
    m.set_rate(22050);
    // A sample channel plays the decoded sound and frees itself at the end.
    auto s = std::make_shared<Sample>();
    decode_sonarc(d.sound.entry(1), *s);
    m.play(0, s, 0, 0x10000, 256, 256);
    std::vector<int16_t> buf(22050 * 2, 0);
    m.render(buf.data(), 2205);
    double e = 0;
    for (int i = 0; i < 2205 * 2; ++i) e += std::abs(buf[size_t(i)]);
    CHECK(e > 1000);
    CHECK(m.channel_busy(0));
    m.render(buf.data(), 22050);  // 4526 samples at 11111 Hz = 0.41 s: over
    CHECK(!m.channel_busy(0));
    // Music through the SoundFont.
    std::vector<uint8_t> sf;
    if (FILE* f = std::fopen(sf_path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        sf.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(sf.data(), 1, sf.size(), f) != sf.size()) sf.clear();
        std::fclose(f);
    }
    CHECK(!sf.empty() && m.load_soundfont(sf.data(), sf.size()));
    m.set_music_flex(&d.music);
    m.music_change(51);
    CHECK_EQ(m.music_playing(), 51);
    double energy = 0;
    for (int k = 0; k < 10; ++k) {
        std::fill(buf.begin(), buf.end(), 0);
        m.render(buf.data(), 22050 / 2);
        for (int i = 0; i < 22050; ++i) energy += double(buf[size_t(i)]) * buf[size_t(i)];
    }
    CHECK(m.notes_sent() > 20);
    CHECK(energy / (22050.0 * 5) > 1e4);  // audible music
    CHECK(m.music_measure() >= 1);
    // A missing track stops the music; 0 is silence.
    m.music_change(122);
    CHECK_EQ(m.music_playing(), 0);
    m.music_change(52);
    for (int k = 0; k < 4; ++k) m.render(buf.data(), 22050 / 2);
    CHECK(m.music_measure() >= 1);
    // docks -> execute is an overlay transition: the old song keeps playing under it, and the
    // new one follows once the transition ends.
    m.music_change(53);
    CHECK_EQ(m.music_playing(), 52);
    CHECK_EQ(m.music_wanted(), 53);
    for (int k = 0; k < 40 && m.music_playing() != 53; ++k) m.render(buf.data(), 22050 / 2);
    CHECK_EQ(m.music_playing(), 53);
    // Queue: the current song stops looping, the queued one follows.
    m.music_change(109);  // fanfare, one measure
    m.music_queue(98);
    for (int k = 0; k < 60 && m.music_playing() != 98; ++k) m.render(buf.data(), 22050 / 2);
    CHECK_EQ(m.music_playing(), 98);
    // Menu: save and stop, then restore.
    m.music_save_and_stop();
    CHECK_EQ(m.music_playing(), 0);
    m.music_restore_saved();
    CHECK_EQ(m.music_playing(), 98);
    // Slow stop: silent after 1.5 s.
    m.music_slow_stop();
    for (int k = 0; k < 4; ++k) m.render(buf.data(), 22050 / 2);
    CHECK_EQ(m.music_playing(), 0);
}

void test_sound_system(const GameData& d) {
    World w(d);
    w.new_game();
    SoundSystem& snd = w.sound;
    CHECK(snd.play_sfx(1));
    CHECK(snd.is_playing(1));
    CHECK(!snd.is_playing(6));
    for (int i = 0; i < 20; ++i) w.frame();  // 0.67 s > 0.41 s
    CHECK(!snd.is_playing(1));
    // 16 normal channels: a 17th sound of the same priority is dropped, a higher one steals.
    for (int i = 0; i < 16; ++i) CHECK(snd.play_sfx(6, 0x60));
    CHECK(!snd.play_sfx(7, 0x60));
    CHECK(snd.play_sfx(7, 0x70));
    CHECK(snd.is_playing(7));
    snd.stop_sfx(6);
    CHECK(!snd.is_playing(6));
    // Ambient loops: once per (sfx, item), forever, stopped per item.
    uint16_t av = 1;
    CHECK(snd.play_ambient(87, 50, av));
    CHECK(snd.play_ambient(87, 50, av));
    for (int i = 0; i < 300; ++i) w.frame();
    CHECK(snd.is_playing(87));
    snd.stop_sfx(87);          // no item: does not touch it
    CHECK(snd.is_playing(87));
    snd.stop_sfx_on(87, av);
    CHECK(!snd.is_playing(87));
    // Music request logic (audio.md 2.2) without a mixer.
    snd.music_play(52);
    CHECK_EQ(snd.music_wanted(), 52);
    snd.combat_music(98);
    snd.music_play(56);        // remembered, not played during combat
    CHECK_EQ(snd.music_wanted(), 98);
    snd.music_restore();
    CHECK_EQ(snd.music_wanted(), 56);
    snd.music_play(0x80 | 49); // masked with 0x7F
    CHECK_EQ(snd.music_last_request(), 49);
    // Sound effects off: nothing starts.
    w.options.sfx = false;
    CHECK(!snd.play_sfx(1));
    w.options.sfx = true;
}

void test_skf(const GameData& d) {
    CHECK_EQ(d.intro.count(), 354);
    CHECK_EQ(d.endgame.count(), 422);
    int frames = 0;
    for (unsigned i = 1; i < d.intro.count(); ++i)
        if (d.intro.entry(i).size >= 2 && rd16(d.intro.entry(i).data) == 2) ++frames;
    CHECK_EQ(frames, 294);
    Sample v;
    CHECK(SkfPlayer::decode_sound(d.intro.entry(337), v));
    CHECK_EQ(v.pcm.size(), 29772);
    CHECK_EQ(v.rate, 11111);
    int y = 0;
    CHECK(SkfPlayer::subtitle_text(d.intro.entry(336), &y) == "You have been a thorn in my side for far too long, Avatar...");
    CHECK_EQ(y, 20);
    CHECK(SkfPlayer::subtitle_text(d.intro.entry(338)).empty());
    CHECK(SkfPlayer::subtitle_text(d.intro.entry(350), &y) == "No one here knows of the AVATAR!");
    CHECK_EQ(y, 100);
    // Frame objects: 2 is the full 320x200 picture, 3 a 252x89 delta drawn at (37, 55).
    Surface8 buf(320, 200);
    buf.clear(0);
    CHECK(SkfPlayer::decode_frame(d.intro.entry(2), buf));
    Surface8 delta(320, 200);
    delta.clear(0);
    CHECK(SkfPlayer::decode_frame(d.intro.entry(3), delta));
    ByteView o3 = d.intro.entry(3);
    CHECK(rd16(o3.data + 4) == 252 && rd16(o3.data + 6) == 89 && rds16(o3.data + 8) == -37 && rds16(o3.data + 10) == -55);
    int outside = 0, inside = 0;
    for (int yy = 0; yy < 200; ++yy)
        for (int xx = 0; xx < 320; ++xx) {
            bool in = xx >= 37 && xx < 37 + 252 && yy >= 55 && yy < 55 + 89;
            if (delta.row(yy)[xx]) (in ? inside : outside)++;
        }
    CHECK_EQ(outside, 0);
    CHECK(inside > 100);
    // The reference step model: EINTRO 1118 steps (74.5 s), ENDGAME 944 (62.9 s).
    for (int which = 0; which < 2; ++which) {
        SkfPlayer p;
        CHECK(p.open(which ? d.endgame : d.intro));
        CHECK_EQ(p.events().size(), which ? 25u : 53u);
        std::vector<int> music, voices;
        int slow = 0, sfx = 0;
        p.on_music = [&](int t) { music.push_back(t); };
        p.on_music_slow_stop = [&]() { ++slow; };
        p.on_sfx = [&](int) { ++sfx; };
        p.on_voice = [&](std::shared_ptr<const Sample> s) { voices.push_back(int(s->pcm.size())); };
        int ticks = 0;
        while (p.tick() && ticks < 10000) ++ticks;
        CHECK(p.ended());
        CHECK_EQ(p.steps(), which ? 944 : 1118);
        CHECK_EQ(p.frames_shown(), which ? 410 : 294);
        if (which == 0) {
            CHECK(music == std::vector<int>({49, 50, 51}));
            CHECK_EQ(slow, 1);
            CHECK_EQ(sfx, 9);
            CHECK_EQ(voices.size(), 8);
            CHECK(!voices.empty() && voices[0] == 29772);
        } else {
            CHECK(music == std::vector<int>({111}));
            CHECK_EQ(sfx, 7);
            CHECK(voices == std::vector<int>({21215, 63818}));  // the frame-0 copy is skipped
        }
    }
}

void test_credits(const GameData& d) {
    std::vector<std::string> c = decode_credits(d.credits), q = decode_credits(d.quotes);
    CHECK_EQ(c.size(), 161);
    CHECK_EQ(q.size(), 105);
    CHECK(!c.empty() && c[0].rfind("+ULTIMA 8 CREDITS", 0) == 0);
    CHECK(!q.empty() && q[0].rfind("+ULTIMA 8 QUOTES", 0) == 0);
    CHECK(c.back() == "@" && q.back() == "@");
    int nonempty = 0;
    for (auto& l : c) nonempty += !l.empty();
    CHECK_EQ(nonempty, 123);
    bool found = false;
    for (auto& l : q) found |= l.find("mkdir U9*~-Unknown") != std::string::npos;
    CHECK(found);
    // The screen scrolls 30 px/s to the end, holds 2 s, sets the quotes flag and closes.
    Config::global().set_bool("quotes_seen", false);
    World w(d);
    w.new_game();
    auto* g = static_cast<CreditsGump*>(w.gumps.add(std::make_unique<CreditsGump>(w, CreditsGump::kCredits)));
    CHECK(g->title() == "ULTIMA 8 CREDITS");
    CHECK(g->strip_height() > 2000);
    CHECK_EQ(w.sound.music_wanted(), 51);
    int frames = 0;
    std::string second_title;
    while (w.gumps.is_open(g) && frames < 20000) {
        w.frame();
        ++frames;
        if (w.gumps.is_open(g) && second_title.empty() && g->title() != "ULTIMA 8 CREDITS") second_title = g->title();
    }
    CHECK(!second_title.empty());
    CHECK(frames > 1000 && frames < 20000);
    CHECK(Config::global().get_bool("quotes_seen", false));
    CHECK_EQ(w.sound.music_wanted(), 0);
    // Quotes: skip with the prompt (Esc asks, Y confirms).
    auto* qg = static_cast<CreditsGump*>(w.gumps.add(std::make_unique<CreditsGump>(w, CreditsGump::kQuotes)));
    for (int i = 0; i < 30; ++i) w.frame();
    int sc = qg->scroll();
    qg->key(kKeyEscape, 0);
    CHECK(qg->prompt_open());
    qg->key('y', 0);  // too soon
    CHECK(qg->prompt_open());
    for (int i = 0; i < 30; ++i) w.frame();
    CHECK_EQ(qg->scroll(), sc);  // held while the prompt is up
    qg->key('y', 0);
    w.gumps.reap();
    CHECK(!w.gumps.is_open(qg));
}

void test_config(const std::string& tmp) {
    Config c;
    c.parse("# x\nmusic=0\ntext_speed=7\nbad\nname=a=b\n");
    CHECK(!c.get_bool("music", true));
    CHECK_EQ(c.get_int("text_speed", 5), 7);
    CHECK_EQ(c.get_int("missing", 3), 3);
    CHECK(c.get("name") == "a=b");
    std::string path = tmp + "/pagan_test.cfg";
    c.load(path);  // missing file: keeps the values
    c.set_int("sfx_volume", 99);
    CHECK(c.save());
    Config r;
    r.load(path);
    CHECK_EQ(r.get_int("sfx_volume", 0), 99);
    CHECK_EQ(r.get_int("text_speed", 0), 7);
    std::remove(path.c_str());
}

InputEvent key_event(int k) {
    InputEvent e;
    e.type = InputEvent::kKeyDown;
    e.key = k;
    return e;
}

// The intro through the game mode: it starts the new game, the skip key pauses the movie and
// asks; No resumes where it was; Yes ends it and the name entry follows. Then a full,
// unskipped headless intro run.
void test_intro(const GameData& d) {
    {
        GameMode gm(d);
        World& w = gm.world();
        w.play_intro = true;
        w.ask_name = true;
        gm.start();
        for (int i = 0; i < 300; ++i) gm.tick();
        MovieGump* m = w.gumps.find<MovieGump>();
        CHECK(m != nullptr);
        if (m) {
            CHECK(w.kernel().paused());
            CHECK_EQ(w.startup_state, 0);  // FIRST waits for the movie
            int steps = m->player.steps();
            gm.handle(key_event(kKeyEscape));
            CHECK(m->prompt_open());
            CHECK_EQ(m->prompt_choice(), 1);
            for (int i = 0; i < 60; ++i) gm.tick();
            CHECK_EQ(m->player.steps(), steps);  // paused
            CHECK(w.sound.movie_paused);
            gm.handle(key_event('n'));
            CHECK(!m->prompt_open());
            CHECK(!w.sound.movie_paused);
            for (int i = 0; i < 30; ++i) gm.tick();
            CHECK(m->player.steps() > steps);  // resumed
            // Space asks again; Left selects Yes, Enter confirms.
            gm.handle(key_event(kKeySpace));
            CHECK(m->prompt_open());
            gm.handle(key_event('y'));  // too soon: one press cannot open and confirm it
            CHECK(m->prompt_open());
            for (int i = 0; i < SkipPrompt::kSettleFrames; ++i) gm.tick();
            gm.handle(key_event(kKeyLeft));
            CHECK_EQ(m->prompt_choice(), 0);
            gm.handle(key_event(kKeyReturn));
            for (int i = 0; i < 5; ++i) gm.tick();
            CHECK(w.gumps.find<MovieGump>() == nullptr);
            CHECK_EQ(w.startup_state, 1);
            MenuGump* name = w.gumps.find<MenuGump>();
            CHECK(name && name->mode() == MenuGump::kName);
        }
    }
    {
        // Mouse: a click asks, a click on No resumes, a click on Yes ends.
        GameMode gm(d);
        World& w = gm.world();
        w.play_intro = true;
        gm.start();
        for (int i = 0; i < 100; ++i) gm.tick();
        MovieGump* m = w.gumps.find<MovieGump>();
        CHECK(m != nullptr);
        if (m) {
            auto click = [&](int x, int y) {
                InputEvent e;
                e.type = InputEvent::kMouseMove; e.x = x; e.y = y; gm.handle(e);
                e.type = InputEvent::kMouseDown; e.button = 1; gm.handle(e);
                e.type = InputEvent::kMouseUp; gm.handle(e);
            };
            click(160, 100);
            CHECK(m->prompt_open());
            for (int i = 0; i < SkipPrompt::kSettleFrames; ++i) gm.tick();
            const ShapeFrame* box = d.gumps.frame(17, 0);
            int bw = box ? box->w : 125, bh = box ? box->h : 67;
            int px = (320 - bw) / 2, py = (200 - bh) / 2;
            click(px + bw - 16 - 20, py + 38 + 10);  // No
            CHECK(!m->prompt_open());
            CHECK(w.gumps.find<MovieGump>() == m);
            click(160, 100);
            for (int i = 0; i < SkipPrompt::kSettleFrames; ++i) gm.tick();
            click(px + 16 + 20, py + 38 + 10);  // Yes
            for (int i = 0; i < 3; ++i) gm.tick();
            CHECK(w.gumps.find<MovieGump>() == nullptr);
        }
    }
    {
        // A full headless intro: 74.5 s of steps plus the 1.5 s slow stop; then FIRST hatches
        // and the music egg asks for the docks (52) behind the name entry.
        World w(d);
        w.play_intro = true;
        w.ask_name = true;
        w.auto_ui = true;
        w.new_game();
        int frames = 0;
        std::vector<int> wanted;
        while (frames < 4000) {
            w.frame();
            ++frames;
            int m = w.sound.music_wanted();
            if (wanted.empty() || wanted.back() != m) wanted.push_back(m);
            if (!w.gumps.find<MovieGump>() && frames > 10) break;
        }
        CHECK(frames > 2200 && frames < 2400);  // 1118 steps * 2 + 45 frames of fade, about 76 s
        CHECK(std::find(wanted.begin(), wanted.end(), 49) != wanted.end());
        CHECK(std::find(wanted.begin(), wanted.end(), 51) != wanted.end());
        for (int i = 0; i < 30; ++i) w.frame();
        CHECK_EQ(w.startup_state, 1);
        CHECK_EQ(w.sound.music_last_request(), 52);
        std::printf("intro: %d frames (%.1f s), music %zu changes\n", frames, frames / 30.0, wanted.size());
    }
}

}  // namespace

void test_milestone5(const GameData& d, const std::string& tmp, const std::string& soundfont) {
    test_sonarc(d);
    test_xmidi(d);
    test_mixer(d, soundfont);
    test_sound_system(d);
    test_skf(d);
    test_credits(d);
    test_config(tmp);
    test_intro(d);
}
