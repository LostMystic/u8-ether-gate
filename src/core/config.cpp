// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "core/config.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pg {

Config& Config::global() {
    static Config c;
    return c;
}

std::string Config::default_path() {
#ifdef __EMSCRIPTEN__
    return "localStorage:pagan.cfg";
#else
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/.pagan/pagan.cfg";
    return "pagan.cfg";
#endif
}

std::string Config::get(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

int Config::get_int(const std::string& key, int def) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    return (end && *end == 0) ? int(v) : def;
}

bool Config::get_bool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return def;
}

std::string Config::text() const {
    std::string s = "# Pagan engine settings\n";
    for (const auto& kv : values_) s += kv.first + "=" + kv.second + "\n";
    return s;
}

void Config::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

#ifdef __EMSCRIPTEN__
EM_JS(char*, pg_config_read, (), {
    var s = null;
    try { s = window.localStorage.getItem('pagan.cfg'); } catch (e) { s = null; }
    if (s === null) return 0;
    return stringToNewUTF8(s);
});
EM_JS(int, pg_config_write, (const char* text), {
    try { window.localStorage.setItem('pagan.cfg', UTF8ToString(text)); return 1; } catch (e) { return 0; }
});
#endif

void Config::load(const std::string& path) {
    path_ = path;
#ifdef __EMSCRIPTEN__
    if (char* s = pg_config_read()) {
        parse(s);
        std::free(s);
    }
#else
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::string t;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
        std::fclose(f);
        parse(t);
    }
#endif
}

bool Config::save() const {
#ifdef __EMSCRIPTEN__
    return pg_config_write(text().c_str()) != 0;
#else
    if (path_.empty()) return false;
    size_t slash = path_.find_last_of('/');
    if (slash != std::string::npos) mkdir(path_.substr(0, slash).c_str(), 0755);
    std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::string t = text();
    bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
    ok = std::fclose(f) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path_.c_str()) == 0;
#endif
}

}  // namespace pg
