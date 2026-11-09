// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <map>
#include <string>

namespace pg {

// Persistent player settings (the options page, the endgame/quotes unlock flags): plain
// "key=value" lines. Native: a file (default ~/.pagan/pagan.cfg). Web: the browser's
// localStorage key "pagan.cfg" (every access in try/catch; a blocked storage just means the
// settings are not kept). Values are strings; the typed getters return `def` when a key is
// missing or unparsable. set() only changes memory; call save() to write.
class Config {
public:
    // Reads the store (missing = empty). `path` is ignored on the web.
    void load(const std::string& path);
    bool save() const;
    const std::string& path() const { return path_; }

    bool has(const std::string& key) const { return values_.count(key) != 0; }
    std::string get(const std::string& key, const std::string& def = "") const;
    int get_int(const std::string& key, int def) const;
    bool get_bool(const std::string& key, bool def) const;
    void set(const std::string& key, const std::string& value) { values_[key] = value; }
    void set_int(const std::string& key, int v) { values_[key] = std::to_string(v); }
    void set_bool(const std::string& key, bool v) { values_[key] = v ? "1" : "0"; }

    // Serialised form (also used for tests).
    std::string text() const;
    void parse(const std::string& text);

    // The process-wide settings used by the engine (loaded once by main).
    static Config& global();
    // The default file: $HOME/.pagan/pagan.cfg (native).
    static std::string default_path();

private:
    std::string path_;
    std::map<std::string, std::string> values_;
};

}  // namespace pg
