// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/editor_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>

#include "json/json.hpp"

namespace pg {

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void make_dirs(const std::string& path) {
    for (size_t p = 1; p <= path.size(); ++p)
        if (p == path.size() || path[p] == '/') mkdir(path.substr(0, p).c_str(), 0755);
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string name_of(const std::string& json) {
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false, true);
    return j.is_object() ? j.value("name", "") : "";
}

StorageResult fail(const std::string& err) {
    StorageResult r;
    r.err = err;
    return r;
}

}  // namespace

std::string stamp_revision(const std::string& json, int revision) {
    static const std::regex re("\"revision\"\\s*:\\s*-?[0-9]+");
    std::smatch m;
    std::string rep = "\"revision\": " + std::to_string(revision);
    if (std::regex_search(json, m, re)) return json.substr(0, size_t(m.position(0))) + rep + json.substr(size_t(m.position(0) + m.length(0)));
    size_t b = json.find('{');
    if (b == std::string::npos) return json;
    bool empty = json.find_first_not_of(" \t\r\n", b + 1) != std::string::npos && json[json.find_first_not_of(" \t\r\n", b + 1)] == '}';
    return json.substr(0, b + 1) + "\n  " + rep + (empty ? "" : ",") + json.substr(b + 1);
}

int read_revision(const std::string& json) {
    static const std::regex re("\"revision\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (std::regex_search(json, m, re)) return std::atoi(m[1].str().c_str());
    return 0;
}

bool valid_storage_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id[0] == '_' || id[0] == '.') return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

std::string utc_now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// --- MemoryStorage -----------------------------------------------------------------------

void MemoryStorage::list_areas(Done done) {
    StorageResult r;
    r.ok = true;
    for (auto& [id, d] : areas_)
        if (!d.deleted && !d.versions.empty()) r.list.push_back(d.versions.back().first);
    done(r);
}

void MemoryStorage::load_area(const std::string& id, Done done) {
    auto it = areas_.find(id);
    if (it == areas_.end() || it->second.deleted || it->second.versions.empty()) return done(fail("no area " + id));
    StorageResult r;
    r.ok = true;
    r.version = it->second.versions.back().first.version;
    r.text = it->second.versions.back().second;
    done(r);
}

void MemoryStorage::save(Doc& d, const std::string& id, const std::string& json, int base, Done& done) {
    int cur = d.versions.empty() || d.deleted ? 0 : d.versions.back().first.version;
    int top = d.versions.empty() ? 0 : d.versions.back().first.version;
    if (base >= 0 && base != cur) {
        StorageResult r;
        r.conflict = true;
        r.version = cur;
        r.err = "conflict: stored version is " + std::to_string(cur) + ", this edit is based on " + std::to_string(base);
        return done(r);
    }
    StoredInfo info;
    info.id = id;
    info.version = top + 1;
    info.saved_at = utc_now_iso();
    info.name = name_of(json);
    d.versions.push_back({info, stamp_revision(json, info.version)});
    d.deleted = false;
    ++writes;
    if (on_write) on_write();
    StorageResult r;
    r.ok = true;
    r.version = info.version;
    done(r);
}

void MemoryStorage::save_area(const std::string& id, const std::string& json, int base_version, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id \"" + id + "\""));
    save(areas_[id], id, json, base_version, done);
}

void MemoryStorage::delete_area(const std::string& id, int base_version, Done done) {
    auto it = areas_.find(id);
    if (it == areas_.end() || it->second.deleted) return done(fail("no area " + id));
    int cur = it->second.versions.back().first.version;
    if (base_version >= 0 && base_version != cur) {
        StorageResult r;
        r.conflict = true;
        r.version = cur;
        r.err = "conflict";
        return done(r);
    }
    it->second.deleted = true;  // the history stays
    ++writes;
    if (on_write) on_write();
    StorageResult r;
    r.ok = true;
    done(r);
}

void MemoryStorage::area_history(const std::string& id, Done done) {
    StorageResult r;
    r.ok = true;
    auto it = areas_.find(id);
    if (it != areas_.end())
        for (auto v = it->second.versions.rbegin(); v != it->second.versions.rend(); ++v) r.list.push_back(v->first);
    done(r);
}

void MemoryStorage::load_area_version(const std::string& id, int version, Done done) {
    auto it = areas_.find(id);
    if (it != areas_.end())
        for (auto& [info, text] : it->second.versions)
            if (info.version == version) {
                StorageResult r;
                r.ok = true;
                r.version = version;
                r.text = text;
                return done(r);
            }
    done(fail("no version " + std::to_string(version) + " of " + id));
}

void MemoryStorage::load_layout(Done done) {
    if (layout_.versions.empty()) return done(fail("no layout saved"));
    StorageResult r;
    r.ok = true;
    r.version = layout_.versions.back().first.version;
    r.text = layout_.versions.back().second;
    done(r);
}

void MemoryStorage::save_layout(const std::string& json, int base_version, Done done) { save(layout_, "layout", json, base_version, done); }

void MemoryStorage::layout_history(Done done) {
    StorageResult r;
    r.ok = true;
    for (auto v = layout_.versions.rbegin(); v != layout_.versions.rend(); ++v) r.list.push_back(v->first);
    done(r);
}

void MemoryStorage::load_layout_version(int version, Done done) {
    for (auto& [info, text] : layout_.versions)
        if (info.version == version) {
            StorageResult r;
            r.ok = true;
            r.version = version;
            r.text = text;
            return done(r);
        }
    done(fail("no layout version " + std::to_string(version)));
}

// --- FileStorage -------------------------------------------------------------------------

FileStorage::FileStorage(std::string areas_dir, std::string layout_path, std::string history_dir)
    : areas_dir_(std::move(areas_dir)), layout_path_(std::move(layout_path)), history_dir_(std::move(history_dir)) {}

void FileStorage::list_areas(Done done) {
    StorageResult r;
    r.ok = true;
    for (const std::string& f : list_dir(areas_dir_)) {
        if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0) continue;
        std::string id = f.substr(0, f.size() - 5);
        if (!valid_storage_id(id)) continue;
        std::string text;
        if (!read_file(areas_dir_ + "/" + f, text)) continue;
        StoredInfo info;
        info.id = id;
        info.version = read_revision(text);
        info.name = name_of(text);
        struct stat st;
        if (stat((areas_dir_ + "/" + f).c_str(), &st) == 0) {
            std::tm tm{};
            std::time_t t = st.st_mtime;
            gmtime_r(&t, &tm);
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
            info.saved_at = buf;
        }
        r.list.push_back(info);
    }
    done(r);
}

void FileStorage::load_area(const std::string& id, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id"));
    StorageResult r;
    if (!read_file(areas_dir_ + "/" + id + ".json", r.text)) return done(fail("no area " + id));
    r.ok = true;
    r.version = read_revision(r.text);
    done(r);
}

void FileStorage::save_doc(const std::string& path, const std::string& key, const std::string& json, int base, Done& done) {
    std::string old;
    bool had = read_file(path, old);
    int cur = had ? read_revision(old) : 0;
    // The newest version ever written (a deleted area keeps counting up from its history).
    int top = cur;
    for (const std::string& f : list_dir(history_dir_ + "/" + key)) top = std::max(top, std::atoi(f.c_str()));
    if (base >= 0 && base != cur) {
        StorageResult r;
        r.conflict = true;
        r.version = cur;
        r.err = "conflict: stored version is " + std::to_string(cur) + ", this edit is based on " + std::to_string(base);
        return done(r);
    }
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) make_dirs(path.substr(0, slash));
    std::string text = stamp_revision(json, top + 1);
    if (!write_file(path, text)) return done(fail("cannot write " + path));
    // History: this version, then trim to the newest kMaxHistory.
    if (!history_dir_.empty()) {
        std::string hd = history_dir_ + "/" + key;
        make_dirs(hd);
        write_file(hd + "/" + std::to_string(top + 1) + ".json", text);
        std::vector<int> vs;
        for (const std::string& f : list_dir(hd))
            if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) vs.push_back(std::atoi(f.c_str()));
        std::sort(vs.begin(), vs.end());
        for (size_t i = 0; i + kMaxHistory < vs.size(); ++i) std::remove((hd + "/" + std::to_string(vs[i]) + ".json").c_str());
    }
    if (on_write) on_write();
    StorageResult r;
    r.ok = true;
    r.version = top + 1;
    done(r);
}

void FileStorage::save_area(const std::string& id, const std::string& json, int base_version, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id \"" + id + "\""));
    save_doc(areas_dir_ + "/" + id + ".json", id, json, base_version, done);
}

void FileStorage::delete_area(const std::string& id, int base_version, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id"));
    std::string path = areas_dir_ + "/" + id + ".json", old;
    if (!read_file(path, old)) return done(fail("no area " + id));
    int cur = read_revision(old);
    if (base_version >= 0 && base_version != cur) {
        StorageResult r;
        r.conflict = true;
        r.version = cur;
        r.err = "conflict";
        return done(r);
    }
    if (std::remove(path.c_str()) != 0) return done(fail("cannot delete " + path));
    if (on_write) on_write();
    StorageResult r;
    r.ok = true;
    done(r);
}

void FileStorage::history_of(const std::string& key, Done& done) {
    StorageResult r;
    r.ok = true;
    std::vector<int> vs;
    for (const std::string& f : list_dir(history_dir_ + "/" + key))
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) vs.push_back(std::atoi(f.c_str()));
    std::sort(vs.rbegin(), vs.rend());
    for (int v : vs) {
        StoredInfo info;
        info.id = key;
        info.version = v;
        std::string text;
        if (read_file(history_dir_ + "/" + key + "/" + std::to_string(v) + ".json", text)) info.name = name_of(text);
        r.list.push_back(info);
    }
    done(r);
}

void FileStorage::area_history(const std::string& id, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id"));
    history_of(id, done);
}

void FileStorage::load_area_version(const std::string& id, int version, Done done) {
    if (!valid_storage_id(id)) return done(fail("bad area id"));
    StorageResult r;
    if (!read_file(history_dir_ + "/" + id + "/" + std::to_string(version) + ".json", r.text))
        return done(fail("no version " + std::to_string(version) + " of " + id));
    r.ok = true;
    r.version = version;
    done(r);
}

void FileStorage::load_layout(Done done) {
    StorageResult r;
    if (!read_file(layout_path_, r.text)) return done(fail("no layout at " + layout_path_));
    r.ok = true;
    r.version = read_revision(r.text);
    done(r);
}

void FileStorage::save_layout(const std::string& json, int base_version, Done done) {
    save_doc(layout_path_, "_layout", json, base_version, done);
}

void FileStorage::layout_history(Done done) { history_of("_layout", done); }

void FileStorage::load_layout_version(int version, Done done) {
    StorageResult r;
    if (!read_file(history_dir_ + "/_layout/" + std::to_string(version) + ".json", r.text))
        return done(fail("no layout version " + std::to_string(version)));
    r.ok = true;
    r.version = version;
    done(r);
}

}  // namespace pg
