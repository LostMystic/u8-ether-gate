// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Any copy of the game, patched or not (data/compat.*): scratch copies of the game folder with a
// changed usecode byte, a changed map record, a changed typo, each optional file removed, each
// required file removed, lower-case names, another language's usecode name. Each must load or
// refuse as it should, switch off only what no longer fits, and still play.
//
//   pagan_compat_tests GAME_DIR DATA_DIR TMP_DIR
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "core/bugfix.h"
#include "core/gamefs.h"
#include "core/variants.h"
#include "data/compat.h"
#include "data/gamedata.h"
#include "game/item_powers.h"
#include "game/npc_dialog.h"
#include "game/party.h"
#include "game/world.h"
#include "usecode/uc_patches.h"
#include "world/current_map.h"
#include "world/map_patches.h"

int g_fail = 0, g_pass = 0;
using namespace pg;
namespace fs = std::filesystem;

namespace {

std::string g_game, g_data, g_tmp;

// A fresh copy of the game folder's data (any case in, the original names out).
std::string make_copy(const std::string& name) {
    fs::path dst = fs::path(g_tmp) / name;
    fs::remove_all(dst);
    fs::create_directories(dst);
    for (const char* d : {"STATIC", "GAMEDAT", "USECODE", "SOUND"}) {
        GameFS g;
        g.open(g_game);
        std::string src = g.find(d);
        if (!src.empty()) fs::copy(src, dst / d, fs::copy_options::recursive | fs::copy_options::update_existing);
    }
    for (auto& e : fs::recursive_directory_iterator(dst))
        if (e.is_regular_file()) fs::permissions(e.path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add);
    return dst.string();
}

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void write_file(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// Where Flex entry `index` starts in the file.
size_t flex_offset(const Bytes& b, unsigned index) { return rd32(&b[0x80 + 8 * index]); }

// Plays a new game for a while and a walk across a few maps: no crash is the test.
void play_some(const GameData& d, const char* what) {
    World w(d);
    w.auto_ui = true;
    w.new_game();
    for (int i = 0; i < 900; ++i) {
        w.input.now_ms = uint32_t(i * 1000 / 30);
        w.frame();
    }
    w.teleport_actor(1, 13038, 15042, 48, 40);
    for (int i = 0; i < 120; ++i) w.frame();
    CHECK(w.avatar() != nullptr);
    std::printf("  played: %s\n", what);
}

bool load_from(const std::string& dir, GameData& d) {
    GameFS g;
    CHECK(g.open(dir));
    return d.load(g);
}

bool switched_off(const std::string& part) {
    for (const std::string& s : compat::switched_off())
        if (s.find(part) != std::string::npos) return true;
    return false;
}

void test_known() {
    std::printf("the copy our fixes were made for\n");
    GameData d;
    CHECK(load_from(g_game, d));
    CHECK(compat::copy_known());
    CHECK(compat::copy_name() == "English v2.10");
    CHECK(compat::about_line() == "English v2.10");
    CHECK(compat::usecode_known());
    CHECK(compat::world_known());
    CHECK(compat::differing_files().empty());
    for (unsigned c = 0; c < UsecodeFile::kClasses; ++c) CHECK(compat::class_known(c));
    for (unsigned m = 0; m < 256; ++m) CHECK(compat::fixed_map_known(m) && compat::nonfixed_map_known(m));
    std::string err;
    CHECK(load_story_patches(d.usecode, g_data + "/patches", err));
    CHECK(err.empty());
}

void test_changed_usecode() {
    std::printf("a changed usecode byte (the place a patch expects)\n");
    std::string dir = make_copy("usecode_byte");
    fs::path f = fs::path(dir) / "USECODE/EUSECODE.FLX";
    Bytes b = read_file(f);
    size_t at = flex_offset(b, 288 + 2) + 0x0C + 0x16B2;  // GRIMOIRE: patch resurrection_book_grimoire
    b[at] ^= 0xFF;
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    CHECK(!compat::copy_known());
    CHECK(compat::about_line() == "unknown version: running anyway");
    CHECK(!compat::usecode_known());
    CHECK(!compat::class_known(288));
    CHECK(compat::class_known(613));
    CHECK(compat::world_known());
    CHECK_EQ(compat::differing_files().size(), 1u);
    std::string err;
    CHECK(!load_story_patches(d.usecode, g_data + "/patches", err));  // says something was refused
    CHECK(!err.empty());
    // The whole fix stays out: both books (one bug, two classes), the rest go in.
    CHECK(d.usecode.patched_code(288).empty());
    CHECK(d.usecode.patched_code(613).empty());
    CHECK(!d.usecode.patched_code(1038).empty());  // Gwillim's topics
    play_some(d, "with the changed byte");
}

void test_dialog_and_rules() {
    std::printf("a changed NPC class and classes engine rules hang on\n");
    std::string dir = make_copy("usecode_classes");
    fs::path f = fs::path(dir) / "USECODE/EUSECODE.FLX";
    Bytes b = read_file(f);
    for (unsigned c : {1038u, 814u, 1413u, 1163u}) b[flex_offset(b, c + 2) + 0x0C + 0x90] ^= 0x55;  // Gwillim, Skull, ETHRSPEL, ANCTONES
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    CHECK(!compat::class_known(1038));
    CHECK(!compat::class_known(814));
    std::string err;
    load_story_patches(d.usecode, g_data + "/patches", err);
    World w(d);
    NpcDialog nd(w);
    CHECK(nd.load_dir(g_data + "/dialog", err));
    bool has_gwillim = false, has_beren = false;
    for (const NpcDialogDef& df : nd.defs) {
        has_gwillim = has_gwillim || df.id == "gwillim";
        has_beren = has_beren || df.id == "beren";
    }
    CHECK(!has_gwillim);
    CHECK(has_beren);
    CHECK(switched_off("new dialogue for gwillim"));
    // The Skull's power is off.
    item_powers::enabled() = true;
    CHECK(!item_powers::is_power_shape(814));
    CHECK(switched_off("skull"));
    // The invasion's flag is left alone.
    restored_invasion() = true;
    w.auto_ui = true;
    w.new_game();
    for (int i = 0; i < 60; ++i) w.frame();
    CHECK(switched_off("Undead invasion"));
    CHECK_EQ(w.vm().get_global(58, 1), 0);
    // The stitched world still works here: the maps are the known ones.
    CHECK(compat::world_known());
}

void test_changed_map() {
    std::printf("a changed map record (map 57, the rubble; map 62, the floor fixes)\n");
    std::string dir = make_copy("map_record");
    fs::path f = fs::path(dir) / "STATIC/FIXED.DAT";
    Bytes b = read_file(f);
    b[flex_offset(b, 57) + 3] ^= 1;
    b[flex_offset(b, 62) + 3] ^= 1;
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    CHECK(!compat::copy_known());
    CHECK(!compat::fixed_map_known(57));
    CHECK(compat::fixed_map_known(40));
    CHECK(!compat::world_known());
    CHECK(compat::usecode_known());  // the story patches still apply
    std::string err;
    CHECK(load_story_patches(d.usecode, g_data + "/patches", err));
    CHECK(map_patches::load_file(g_data + "/patches/map_content.json", err));
    underworld_entrance() = true;
    std::vector<ItemRecord> before = parse_item_records(d.fixed.entry(57)), after = before;
    apply_map_fixes(57, after);
    CHECK_EQ(after.size(), before.size());  // no rubble
    CHECK(switched_off("map patch underworld_entrance"));
    before = parse_item_records(d.fixed.entry(62));
    after = before;
    apply_map_fixes(62, after);
    CHECK_EQ(after.size(), before.size());  // no floor fixes
    CHECK(switched_off("the map fixes of map 62"));
    // A map we didn't touch keeps its fixes.
    before = parse_item_records(d.fixed.entry(50));
    after = before;
    apply_map_fixes(50, after);
    CHECK_EQ(after.size(), before.size() + 1);
    {
        World w(d);
        CHECK(!w.set_world_mode(true, g_data + "/world_layout.json"));
        CHECK(!w.stitched());
        CHECK(switched_off("the stitched world"));
    }
    play_some(d, "with the changed records");
}

void test_nonfixed_map() {
    std::printf("a changed NONFIXED record on map 57: the moves and the rubble stay out\n");
    std::string dir = make_copy("nonfixed_record");
    fs::path f = fs::path(dir) / "GAMEDAT/NONFIXED.DAT";
    Bytes b = read_file(f);
    b[flex_offset(b, 57) + 3] ^= 1;
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    CHECK(compat::fixed_map_known(57));
    CHECK(!compat::nonfixed_map_known(57));
    std::string err;
    CHECK(map_patches::load_file(g_data + "/patches/map_content.json", err));
    CHECK(!map_patches::moves_on(*map_patches::find("underworld_entrance")));
    CHECK(!map_patches::group_on(*map_patches::find("underworld_entrance")));
}

void test_text() {
    std::printf("a text correction whose typo this copy doesn't have is dropped\n");
    std::string dir = make_copy("text");
    fs::path f = fs::path(dir) / "USECODE/EUSECODE.FLX";
    Bytes b = read_file(f);
    const std::string typo = "Restortation", right = "Restoratiooo";
    int n = 0;
    for (auto it = std::search(b.begin(), b.end(), typo.begin(), typo.end()); it != b.end(); it = std::search(it, b.end(), typo.begin(), typo.end())) {
        std::copy(right.begin(), right.end(), it);
        ++n;
    }
    CHECK(n > 0);
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    std::string err;
    load_story_patches(d.usecode, g_data + "/patches", err);
    bool kept_typo = false, kept_other = false;
    for (const StoryPatches::Text& t : story_patches().texts) {
        kept_typo = kept_typo || t.find == typo;
        kept_other = kept_other || t.find == "Etherial";
    }
    CHECK(!kept_typo);
    CHECK(kept_other);
    // Another language's words: a sample of the corrections isn't in a copy with no English.
    original_bug_fixes() = true;
    CHECK(story_patches().fix_text("the Restortation spell") == "the Restortation spell");
}

void test_optional() {
    std::printf("each optional file removed\n");
    const char* files[] = {"SOUND/SOUND.FLX", "SOUND/MUSIC.FLX", "STATIC/EINTRO.SKF", "STATIC/ENDGAME.SKF",
                           "STATIC/ECREDITS.DAT", "STATIC/QUOTES.DAT", "STATIC/GUMPAGE.DAT", "STATIC/WPNOVLAY.DAT"};
    for (const char* name : files) {
        std::string dir = make_copy("optional");
        CHECK(fs::remove(fs::path(dir) / name));
        GameData d;
        CHECK(load_from(dir, d));
        bool named = false;
        for (const std::string& s : d.missing_optional()) named = named || s.find(name) != std::string::npos;
        CHECK(named);
        CHECK(compat::copy_known());
        play_some(d, name);
    }
    std::string dir = make_copy("optional");
    fs::remove_all(fs::path(dir) / "SOUND");
    GameData d;
    CHECK(load_from(dir, d));
    play_some(d, "no SOUND folder at all");
    // A damaged optional file counts as missing.
    dir = make_copy("optional");
    Bytes junk(1000, 0x41);
    write_file(fs::path(dir) / "SOUND/MUSIC.FLX", junk);
    write_file(fs::path(dir) / "STATIC/EINTRO.SKF", junk);
    GameData e;
    CHECK(load_from(dir, e));
    CHECK_EQ(e.music.count(), 0);
    play_some(e, "garbage in MUSIC.FLX and EINTRO.SKF");
}

void test_required() {
    std::printf("each required file removed: refused, and the message names it\n");
    for (const std::string& name : GameData::required_files()) {
        std::string dir = make_copy("required");
        CHECK(fs::remove(fs::path(dir) / name));
        GameData d;
        CHECK(!load_from(dir, d));
        CHECK_EQ(d.missing_required().size(), 1u);
        CHECK(d.error().find(name) != std::string::npos);
    }
    std::string dir = make_copy("required");
    fs::remove_all(fs::path(dir) / "GAMEDAT");
    fs::remove_all(fs::path(dir) / "USECODE");
    GameData d;
    CHECK(!load_from(dir, d));
    CHECK_EQ(d.missing_required().size(), 4u);
}

void test_case() {
    std::printf("lower-case names\n");
    std::string dir = make_copy("lower");
    std::vector<fs::path> all;
    for (auto& e : fs::recursive_directory_iterator(dir)) all.push_back(e.path());
    std::sort(all.begin(), all.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; };
    for (const fs::path& p : all) fs::rename(p, p.parent_path() / lower(p.filename().string()));
    CHECK(fs::exists(fs::path(dir) / "static/u8pal.pal"));
    GameData d;
    CHECK(load_from(dir, d));
    CHECK(compat::copy_known());
    CHECK(d.missing_optional().empty());
    CHECK(GameFS::is_game_folder(dir));
    play_some(d, "lower-case names");
}

void test_language() {
    std::printf("another language's usecode name\n");
    std::string dir = make_copy("language");
    fs::rename(fs::path(dir) / "USECODE/EUSECODE.FLX", fs::path(dir) / "USECODE/GUSECODE.FLX");
    fs::rename(fs::path(dir) / "STATIC/EINTRO.SKF", fs::path(dir) / "STATIC/GINTRO.SKF");
    GameData d;
    CHECK(load_from(dir, d));
    CHECK_EQ(d.language, 'G');
    CHECK(d.usecode_path == "USECODE/GUSECODE.FLX");
    CHECK(!compat::copy_known());
    CHECK(!compat::usecode_known());
    CHECK(d.intro.count() > 1);
    play_some(d, "GUSECODE.FLX");
}

void test_damage() {
    std::printf("random changed bytes in the usecode and maps: unknown copy, still no crash\n");
    std::string dir = make_copy("damage");
    fs::path f = fs::path(dir) / "USECODE/EUSECODE.FLX";
    Bytes b = read_file(f);
    uint32_t rng = 12345;
    for (int i = 0; i < 40; ++i) {
        rng = rng * 1664525u + 1013904223u;
        // In a class's code, past the header table, not in the Flex table.
        unsigned c = 1 + rng % 1400;
        size_t base = flex_offset(b, c + 2), size = rd32(&b[0x80 + 8 * (c + 2) + 4]);
        if (size < 0x100) continue;
        b[base + 0x0C + 0x80 + (rng >> 8) % (size - 0x100)] ^= uint8_t(1 + (rng >> 24) % 255);
    }
    write_file(f, b);
    GameData d;
    CHECK(load_from(dir, d));
    std::string err;
    load_story_patches(d.usecode, g_data + "/patches", err);
    play_some(d, "40 random usecode bytes changed");
}


void test_new_game_save() {
    std::printf("new games from SAVEGAME/U8SAVE.000: a played GAMEDAT, no GAMEDAT, a damaged save\n");
    auto with_save = [](const std::string& name) {
        std::string dir = make_copy(name);
        GameFS g;
        g.open(g_game);
        fs::create_directories(fs::path(dir) / "SAVEGAME");
        fs::copy_file(g.find("SAVEGAME/U8SAVE.000"), fs::path(dir) / "SAVEGAME/U8SAVE.000");
        return dir;
    };
    {   // GAMEDAT holds a game the DOS original played: the copy is still the known one
        std::string dir = with_save("played");
        fs::path f = fs::path(dir) / "GAMEDAT/NONFIXED.DAT";
        Bytes b = read_file(f);
        for (size_t i = 5000; i < 5004; ++i) b[i] ^= 0xFF;
        write_file(f, b);
        GameFS g;
        CHECK(g.open(dir));
        CHECK(g.new_game_from_save());
        CHECK(g.played_gamedat());
        CHECK(g.where("gamedat/nonfixed.dat") == "SAVEGAME/U8SAVE.000");
        Bytes got;
        CHECK(g.read("GAMEDAT/NONFIXED.DAT", got));
        CHECK(got != b);
        GameData d;
        CHECK(d.load(g));
        CHECK(compat::copy_known());
        play_some(d, "a played GAMEDAT");
    }
    {   // no GAMEDAT folder at all
        std::string dir = with_save("nogamedat");
        fs::remove_all(fs::path(dir) / "GAMEDAT");
        GameFS g;
        CHECK(g.open(dir));
        CHECK(g.exists("GAMEDAT/NPCDATA.DAT"));
        CHECK(!g.played_gamedat());
        GameData d;
        CHECK(d.load(g));
        CHECK(compat::copy_known());
        play_some(d, "no GAMEDAT, only U8SAVE.000");
    }
    {   // a cut-short save is not used: GAMEDAT is
        std::string dir = with_save("badsave");
        fs::path f = fs::path(dir) / "SAVEGAME/U8SAVE.000";
        Bytes b = read_file(f);
        b.resize(b.size() / 2);
        write_file(f, b);
        GameFS g;
        CHECK(g.open(dir));
        CHECK(!g.new_game_from_save());
        GameData d;
        CHECK(d.load(g));
        CHECK(compat::copy_known());
    }
    {   // the game a few folders down (a store's or a DOSBox package's folder around it)
        fs::path top = fs::path(g_tmp) / "store";
        fs::remove_all(top);
        fs::create_directories(top / "Games/Ultima 8");
        fs::create_directories(top / "Games/Other");
        std::string dir = make_copy("store/Games/Ultima 8/ultima8");
        CHECK(GameFS::game_folder_in(top.string()) == dir);
        CHECK(GameFS::game_folder_in(dir) == dir);
        CHECK(GameFS::game_folder_in(top.string(), 1).empty());
        CHECK(GameFS::game_folder_in((top / "Games/Other").string()).empty());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: pagan_compat_tests GAME_DIR DATA_DIR TMP_DIR\n"); return 2; }
    g_game = argv[1];
    g_data = argv[2];
    g_tmp = argv[3];
    fs::create_directories(g_tmp);
    test_known();
    test_changed_usecode();
    test_dialog_and_rules();
    test_changed_map();
    test_nonfixed_map();
    test_text();
    test_optional();
    test_required();
    test_case();
    test_language();
    test_damage();
    test_new_game_save();
    // Back to the known copy for anything that follows.
    GameData d;
    load_from(g_game, d);
    fs::remove_all(g_tmp);
    std::printf("compat tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
