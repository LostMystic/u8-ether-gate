// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/gamedata.h"

#include "data/compat.h"
#include "core/log.h"
#include <fstream>
#include <sstream>

#include "json/json.hpp"

namespace pg {

const std::vector<std::string>& GameData::required_files() {
    static const std::vector<std::string> files = {
        "STATIC/U8PAL.PAL", "STATIC/XFORMPAL.DAT", "STATIC/U8SHAPES.FLX", "STATIC/U8GUMPS.FLX", "STATIC/U8FONTS.FLX",
        "STATIC/U8MOUSE.SHP", "STATIC/TYPEFLAG.DAT", "STATIC/ANIM.DAT", "STATIC/GLOB.FLX", "STATIC/FIXED.DAT",
        "GAMEDAT/NONFIXED.DAT", "GAMEDAT/ITEMCACH.DAT", "GAMEDAT/NPCDATA.DAT", "USECODE/EUSECODE.FLX"};
    return files;
}

namespace {
// USECODE/<L>USECODE.FLX: English first, then whatever language this copy has.
std::string find_usecode(const GameFS& fs) {
    if (fs.exists("USECODE/EUSECODE.FLX")) return "USECODE/EUSECODE.FLX";
    for (const std::string& n : fs.list("USECODE")) {
        std::string u;
        for (char c : n) u += char(c >= 'a' && c <= 'z' ? c - 32 : c);
        if (u.size() == 12 && u.compare(1, 11, "USECODE.FLX") == 0) return "USECODE/" + u;
    }
    return "";
}
}  // namespace

bool GameData::load(const GameFS& fs) {
    Bytes b;
    missing_.clear();
    missing_optional_.clear();
    usecode_path = find_usecode(fs);
    language = usecode_path.empty() ? 'E' : usecode_path[8];
    for (const std::string& f : required_files()) {
        if (f == "USECODE/EUSECODE.FLX" ? usecode_path.empty() : !fs.exists(f)) missing_.push_back(f);
    }
    if (!missing_.empty()) {
        error_ = "missing";
        for (const std::string& f : missing_) error_ += " " + f;
        return false;
    }
    auto need = [&](const char* path) {
        if (!fs.read(path, b)) { error_ = std::string("missing ") + path; return false; }
        return true;
    };
    auto bad = [&](const char* path, const std::string& why = "") {
        error_ = std::string("cannot parse ") + path + (why.empty() ? "" : ": " + why);
        return false;
    };
    std::string e;
    if (!need("STATIC/U8PAL.PAL") || !palette.load(b)) return error_.empty() ? bad("STATIC/U8PAL.PAL") : false;
    if (!need("STATIC/XFORMPAL.DAT") || !xform.load(b)) return error_.empty() ? bad("STATIC/XFORMPAL.DAT") : false;
    if (!need("STATIC/U8SHAPES.FLX")) return false;
    if (!shapes.load_flex(std::move(b), &e)) return bad("STATIC/U8SHAPES.FLX", e);
    if (!need("STATIC/U8GUMPS.FLX")) return false;
    if (!gumps.load_flex(std::move(b), &e)) return bad("STATIC/U8GUMPS.FLX", e);
    if (!need("STATIC/U8FONTS.FLX")) return false;
    if (!fonts.load_flex(std::move(b), &e)) return bad("STATIC/U8FONTS.FLX", e);
    if (!need("STATIC/U8MOUSE.SHP")) return false;
    if (!mouse.load_single(std::move(b), &e)) return bad("STATIC/U8MOUSE.SHP", e);
    if (!need("STATIC/TYPEFLAG.DAT")) return false;
    if (!typeflags.load(b)) return bad("STATIC/TYPEFLAG.DAT");
    if (!need("STATIC/ANIM.DAT")) return false;
    if (!anim.load(b)) return bad("STATIC/ANIM.DAT");
    if (!need("STATIC/GLOB.FLX")) return false;
    if (!globs.load(std::move(b))) return bad("STATIC/GLOB.FLX");
    if (!need("STATIC/FIXED.DAT")) return false;
    if (!fixed.load(std::move(b))) return bad("STATIC/FIXED.DAT", fixed.error());
    if (!need("GAMEDAT/NONFIXED.DAT")) return false;
    if (!nonfixed.load(std::move(b))) return bad("GAMEDAT/NONFIXED.DAT", nonfixed.error());
    Bytes ic, nd;
    if (!fs.read("GAMEDAT/ITEMCACH.DAT", ic) || !fs.read("GAMEDAT/NPCDATA.DAT", nd)) {
        error_ = "missing GAMEDAT/ITEMCACH.DAT or NPCDATA.DAT";
        return false;
    }
    if (!load_npcs(std::move(ic), std::move(nd), npcs)) return bad("GAMEDAT/ITEMCACH.DAT / NPCDATA.DAT");
    if (!need(usecode_path.c_str())) return false;
    if (!usecode.load(std::move(b))) return bad(usecode_path.c_str(), usecode.error());
    // Optional extras: the UI works without them (no item areas / no weapon overlays). A file that
    // is there but isn't what it should be counts as missing.
    auto lacks = [&](const std::string& path, const char* what) {
        missing_optional_.push_back(path + " (" + what + ")");
        PG_LOG("optional file not found or unusable: %s: %s", path.c_str(), what);
    };
    auto localised = [&](const std::string& en_name) {  // STATIC/EINTRO.SKF -> this language's, else the English one
        std::string p = en_name;
        p[p.find('/') + 1] = language;
        return fs.exists(p) ? p : en_name;
    };
    gump_areas.assign(1, GumpArea{});
    if (!fs.exists("STATIC/GUMPAGE.DAT")) lacks("STATIC/GUMPAGE.DAT", "containers use the whole window");
    if (fs.read("STATIC/GUMPAGE.DAT", b))
        for (size_t o = 0; o + 8 <= b.size(); o += 8)
            gump_areas.push_back({rds16(&b[o]), rds16(&b[o + 2]), rds16(&b[o + 4]), rds16(&b[o + 6])});
    if (!fs.read("STATIC/WPNOVLAY.DAT", b) || !wpnovlay.load(std::move(b))) lacks("STATIC/WPNOVLAY.DAT", "no weapons drawn in the hand");
    this->fs = fs;
    if (!fs.read("SOUND/SOUND.FLX", b) || !sound.load(std::move(b))) { sound = Flex(); lacks("SOUND/SOUND.FLX", "no sound effects"); }
    if (!fs.read("SOUND/MUSIC.FLX", b) || !music.load(std::move(b))) { music = Flex(); lacks("SOUND/MUSIC.FLX", "no music"); }
    const std::string in = localised("STATIC/EINTRO.SKF");
    if (!fs.read(in, b) || !intro.load(std::move(b))) { intro = Flex(); lacks(in, "no intro movie"); }
    if (!fs.read("STATIC/ENDGAME.SKF", b) || !endgame.load(std::move(b))) { endgame = Flex(); lacks("STATIC/ENDGAME.SKF", "no ending movie"); }
    const std::string cr = localised("STATIC/ECREDITS.DAT");
    if (!fs.read(cr, credits)) lacks(cr, "no credits");
    if (!fs.read("STATIC/QUOTES.DAT", quotes)) lacks("STATIC/QUOTES.DAT", "no quotes page");
    // Which copy is this? Everything written for particular data asks data/compat.h from now on.
    compat::check(fs, *this);
    if (!compat::file_known("STATIC/TYPEFLAG.DAT")) {
        typeflags.drop_fixes();
        compat::off("the TYPEFLAG data fixes", "STATIC/TYPEFLAG.DAT differs");
    }
    return true;
}

int GameData::load_custom_shapes(const std::string& dir, std::string* error) {
    auto slurp = [](const std::string& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };
    std::string text;
    if (!slurp(dir + "/shapes.json", text)) return 0;
    int added = 0;
    try {
        nlohmann::json j = nlohmann::json::parse(text, nullptr, true, true);
        for (const auto& e : j.at("shapes")) {
            unsigned number = e.at("number").get<unsigned>();
            if (number < kFirstCustomShape || number > 0xFFFF) {
                if (error) *error = "shape number out of range: " + std::to_string(number);
                continue;
            }
            std::string e2;
            if (e.contains("frames_from")) {
                // A copy of an original shape's frames (the new creatures' bodies).
                if (!shapes.copy_shape(e["frames_from"].get<unsigned>(), number, &e2)) {
                    if (error) *error = std::to_string(number) + ": " + e2;
                    continue;
                }
            } else {
                std::string file = e.at("file").get<std::string>(), bytes;
                if (!slurp(dir + "/" + file, bytes)) {
                    if (error) *error = "missing " + dir + "/" + file;
                    continue;
                }
                if (!shapes.add_shape(number, Bytes(bytes.begin(), bytes.end()), &e2)) {
                    if (error) *error = file + ": " + e2;
                    continue;
                }
            }
            if (e.contains("anim_from")) anim.copy_shape(e["anim_from"].get<unsigned>(), number);
            if (e.contains("typeflags_from")) typeflags.copy_shape(e["typeflags_from"].get<unsigned>(), number);
            custom_shapes.push_back(number);
            ++added;
        }
    } catch (const std::exception& ex) {
        if (error) *error = std::string("shapes.json: ") + ex.what();
    }
    return added;
}

}  // namespace pg
