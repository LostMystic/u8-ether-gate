// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "core/gamefs.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace pg {

namespace {
bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = char(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = char(cb - 32);
        if (ca != cb) return false;
    }
    return true;
}
}  // namespace

bool GameFS::open(const std::string& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;
    root_ = root;
    virtual_.clear();
    played_ = false;
    read_new_game_save();
    return true;
}

namespace {
std::string upper(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c = char(c - 32);
    }
    return s;
}
uint32_t le32(const Bytes& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
}  // namespace

// SAVEGAME/U8SAVE.000 (formats/savegame.md 1): signature, u16 count, then members of u32 name
// length (with its NUL), the name, u32 data length, the data. Only the three files the engine
// starts a game from are taken, and only when all three are there and the archive is whole.
void GameFS::read_new_game_save() {
    Bytes b;
    const std::string path = find("SAVEGAME/U8SAVE.000");
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    static const char kSig[] = "Ultima 8 SaveGame File.";
    if (b.size() < 26 || std::memcmp(b.data(), kSig, 24) != 0) return;
    const unsigned n = unsigned(b[24]) | unsigned(b[25]) << 8;
    std::vector<std::pair<std::string, Bytes>> got;
    size_t at = 26;
    for (unsigned i = 0; i < n; ++i) {
        if (at + 4 > b.size()) return;
        const uint32_t l = le32(b, at);
        at += 4;
        if (l == 0 || l > 64 || at + l + 4 > b.size()) return;
        std::string name(reinterpret_cast<const char*>(&b[at]), strnlen(reinterpret_cast<const char*>(&b[at]), l));
        at += l;
        const uint32_t d = le32(b, at);
        at += 4;
        if (d > b.size() - at) return;
        name = upper(name);
        if (name == "NONFIXED.DAT" || name == "ITEMCACH.DAT" || name == "NPCDATA.DAT")
            got.emplace_back("GAMEDAT/" + name, Bytes(b.begin() + long(at), b.begin() + long(at + d)));
        at += d;
    }
    if (got.size() != 3) return;
    for (const auto& [rel, data] : got) {
        Bytes own;
        const std::string p = find(rel);
        std::ifstream g(p, std::ios::binary);
        if (!p.empty() && g) own.assign(std::istreambuf_iterator<char>(g), std::istreambuf_iterator<char>());
        if (!p.empty() && own != data) played_ = true;
    }
    virtual_ = std::move(got);
}

const Bytes* GameFS::virtual_of(const std::string& rel) const {
    if (virtual_.empty()) return nullptr;
    const std::string u = upper(rel);
    for (const auto& [k, v] : virtual_)
        if (k == u) return &v;
    return nullptr;
}

std::string GameFS::where(const std::string& rel) const {
    if (virtual_of(rel)) return "SAVEGAME/U8SAVE.000";
    const std::string p = find(rel);
    return p.empty() ? rel : p;
}

std::string GameFS::find(const std::string& rel) const {
    fs::path cur = root_;
    std::error_code ec;
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t slash = rel.find_first_of("/\\", pos);
        std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? rel.size() + 1 : slash + 1;
        if (part.empty() || part == ".") continue;
        fs::path exact = cur / part;
        if (fs::exists(exact, ec)) { cur = exact; continue; }
        bool found = false;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            if (iequal(it->path().filename().string(), part)) {
                cur = it->path();
                found = true;
                break;
            }
        }
        if (!found) return "";
    }
    return cur.string();
}

bool GameFS::read(const std::string& rel, Bytes& out) const {
    out.clear();
    if (const Bytes* v = virtual_of(rel)) { out = *v; return true; }
    std::string path = find(rel);
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize(size_t(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f) || f.eof();
}

std::vector<std::string> GameFS::list(const std::string& rel_dir) const {
    std::vector<std::string> out;
    std::string dir = find(rel_dir);
    if (dir.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(it->path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool GameFS::is_game_folder(const std::string& dir) {
    GameFS g;
    return g.open(dir) && g.exists("STATIC/U8PAL.PAL");
}

std::string GameFS::game_folder_in(const std::string& dir, int depth) {
    if (is_game_folder(dir)) return dir;
    std::vector<fs::path> level{fs::path(dir)};
    std::error_code ec;
    for (int d = 0; d < depth && !level.empty(); ++d) {
        std::vector<fs::path> next;
        for (const fs::path& p : level) {
            std::vector<fs::path> subs;
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec)) subs.push_back(it->path());
            std::sort(subs.begin(), subs.end());
            for (const fs::path& s : subs) {
                if (is_game_folder(s.string())) return s.string();
                next.push_back(s);
            }
            ec.clear();
        }
        if (next.size() > 2000) break;  // not an install folder
        level = std::move(next);
    }
    return "";
}

std::string GameFS::locate(const std::string& exe_dir, std::vector<std::string>* tried) {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("PAGAN_GAME"); env && *env) candidates.emplace_back(env);
#ifdef __EMSCRIPTEN__
    candidates.emplace_back("/game");  // the web build's folder
#endif
    if (!exe_dir.empty())
        for (const char* rel : {"game", "ultima8", "GAME", "ULTIMA8"}) candidates.push_back(fs::path(exe_dir) / rel);
    for (const char* rel : {"game", "ultima8", "GAME", "ULTIMA8", "."}) candidates.emplace_back(rel);
    for (const char* rel : {"../game", "../../game"}) candidates.emplace_back(rel);  // a source tree
    if (!exe_dir.empty())
        for (const char* rel : {"../game", "../../game", "../../../game"}) candidates.push_back(fs::path(exe_dir) / rel);
    for (const fs::path& c : candidates) {
        if (std::string in = game_folder_in(c.string(), c == fs::path(".") ? 0 : 2); !in.empty()) return in;
        if (tried) tried->push_back(c.string());
    }
    return "";
}

}  // namespace pg
