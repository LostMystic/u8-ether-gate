// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// "Everyone knows": the phrase rewrites of data/avatar_title.json (game/avatar_title.h).
#include "game/avatar_title.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "json/json.hpp"

namespace pg {

using nlohmann::json;

namespace {
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '\''; }
}  // namespace

bool AvatarTitle::load_text(const std::string& text, std::string& err) {
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    if (!j.is_object()) { err = "not an object"; return false; }
    mark = j.value("mark", mark);
    title = j.value("title", title);
    name_in_speech = j.value("name_in_speech", name_in_speech);
    classes.clear();
    if (j.contains("classes") && j["classes"].is_object())
        for (auto it = j["classes"].begin(); it != j["classes"].end(); ++it) {
            if (!it.value().is_array()) { err = "class " + it.key() + ": not a list"; return false; }
            std::vector<Rule>& rules = classes[it.key()];
            for (const json& r : it.value()) {
                Rule x;
                x.find = r.value("find", std::string());
                x.replace = r.value("replace", std::string());
                x.word = r.value("word", true);
                x.exact = r.value("exact", false);
                if (x.find.empty()) { err = "class " + it.key() + ": a rule without \"find\""; return false; }
                rules.push_back(std::move(x));
            }
        }
    return true;
}

bool AvatarTitle::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!load_text(ss.str(), err)) { err = path + ": " + err; return false; }
    return true;
}

size_t AvatarTitle::find_phrase(const std::string& s, const std::string& phrase, bool word, size_t from, bool& capital) {
    if (phrase.empty()) return std::string::npos;
    std::string cap = phrase;
    bool can_cap = std::islower(static_cast<unsigned char>(cap[0])) != 0;
    if (can_cap) cap[0] = char(std::toupper(static_cast<unsigned char>(cap[0])));
    for (size_t i = from; i + phrase.size() <= s.size(); ++i) {
        bool plain = s.compare(i, phrase.size(), phrase) == 0;
        bool capd = !plain && can_cap && s.compare(i, cap.size(), cap) == 0;
        if (!plain && !capd) continue;
        if (word) {
            if (i > 0 && word_char(s[i - 1]) && word_char(phrase.front())) continue;
            size_t e = i + phrase.size();
            if (e < s.size() && word_char(s[e]) && word_char(phrase.back())) continue;
        }
        capital = capd;
        return i;
    }
    return std::string::npos;
}

int AvatarTitle::rewrite(const std::string& cls, std::string& s) const {
    auto it = classes.find(cls);
    if (it == classes.end()) return 0;
    int n = 0;
    auto trimmed = [](std::string t) {
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    };
    for (const Rule& r : it->second) {
        if (r.exact) {
            std::string a = trimmed(s), b = trimmed(r.find);
            if (a != b) continue;
            s = r.replace + s.substr(a.size());  // keeps the trailing spaces the script joins with
            ++n;
            continue;
        }
        size_t at = 0;
        bool capital = false;
        while ((at = find_phrase(s, r.find, r.word, at, capital)) != std::string::npos) {
            std::string rep = r.replace;
            if (capital && !rep.empty()) rep[0] = char(std::toupper(static_cast<unsigned char>(rep[0])));
            s.replace(at, r.find.size(), rep);
            at += rep.size();
            ++n;
        }
    }
    return n;
}

std::string AvatarTitle::with_title(const std::string& text, const std::string& name) const {
    if (name.empty() || name == title) return text;
    std::string s = text;
    size_t at = 0;
    bool capital = false;
    while ((at = find_phrase(s, name, true, at, capital)) != std::string::npos) {
        s.replace(at, name.size(), title);
        at += title.size();
    }
    return s;
}

}  // namespace pg
