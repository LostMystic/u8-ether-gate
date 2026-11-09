// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "usecode/uc_patches.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "core/bugfix.h"
#include "core/log.h"
#include "data/compat.h"
#include "json/json.hpp"
#include "usecode/usecode_file.h"

namespace pg {

namespace {

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool parse_hex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        int v = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        if (hi < 0) hi = v;
        else { out.push_back(uint8_t(hi * 16 + v)); hi = -1; }
    }
    return hi < 0 && !out.empty();
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '\''; }

}  // namespace

StoryPatches& story_patches() {
    static StoryPatches p;
    return p;
}

bool StoryPatches::load_usecode_json(const std::string& text, std::string& err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("patches") || !j["patches"].is_array()) {
        err = "usecode.json: expected {\"patches\": [...]}";
        return false;
    }
    for (const auto& e : j["patches"]) {
        Bytes b;
        b.id = e.value("id", std::string());
        b.bug = e.value("bug", std::string());
        b.why = e.value("why", std::string());
        b.cls = e.value("class", 0u);
        b.option = e.value("option", std::string());
        if (!b.option.empty() && b.option != "v212") {
            err = "usecode.json: patch '" + b.id + "': unknown option '" + b.option + "'";
            return false;
        }
        if (e.contains("at") && e["at"].is_string()) b.at = unsigned(std::stoul(e["at"].get<std::string>(), nullptr, 16));
        else b.at = e.value("at", 0u);
        if (e.contains("append")) {  // new code at the end of the class (`at` = where it must start)
            b.append = true;
            if (!parse_hex(e.value("append", std::string()), b.replace)) {
                err = "usecode.json: patch '" + b.id + "': append must be hex";
                return false;
            }
        } else if (!parse_hex(e.value("expect", std::string()), b.expect) || !parse_hex(e.value("replace", std::string()), b.replace) ||
            b.expect.size() != b.replace.size()) {
            err = "usecode.json: patch '" + b.id + "': expect / replace must be hex of the same length";
            return false;
        }
        bytes.push_back(b);
    }
    return true;
}

bool StoryPatches::load_text_json(const std::string& text, std::string& err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("replace") || !j["replace"].is_array()) {
        err = "text.json: expected {\"replace\": [...]}";
        return false;
    }
    for (const auto& e : j["replace"]) {
        Text t;
        t.find = e.value("find", std::string());
        t.replace = e.value("replace", std::string());
        t.bug = e.value("bug", std::string());
        t.word = e.value("word", true);
        if (t.find.empty()) { err = "text.json: an entry without \"find\""; return false; }
        texts.push_back(t);
    }
    return true;
}

bool StoryPatches::load_dir(const std::string& dir, std::string& err) {
    std::string text;
    if (read_text(dir + "/usecode.json", text) && !load_usecode_json(text, err)) return false;
    if (read_text(dir + "/text.json", text) && !load_text_json(text, err)) return false;
    return true;
}

int StoryPatches::install(UsecodeFile& uc, std::vector<std::string>& problems) const {
    // The patches of one class hang together (a jump into code an append adds, addresses that
    // count on the appends before them), and so do the patches of one bug across classes: a
    // group goes in whole or not at all, so a copy with other data never gets half a fix.
    const size_t count = bytes.size();
    std::vector<size_t> parent(count);
    for (size_t i = 0; i < count; ++i) parent[i] = i;
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    {
        std::map<unsigned, size_t> by_class;
        std::map<std::string, size_t> by_bug;
        for (size_t i = 0; i < count; ++i) {
            auto c = by_class.emplace(bytes[i].cls, i);
            parent[root(i)] = root(c.first->second);
            if (!bytes[i].bug.empty()) {
                auto b = by_bug.emplace(bytes[i].bug, i);
                parent[root(i)] = root(b.first->second);
            }
        }
    }
    // Tries `b` on `work` (the class's code so far). "" = fine, else why not.
    std::map<unsigned, UsecodeFile::Gate> gates;  // the option of each class's patches, this install
    auto attempt = [&](const Bytes& b, std::map<unsigned, std::vector<uint8_t>>& work) -> std::string {
        const uint8_t* code = uc.code(b.cls);
        uint32_t size = uc.code_size(b.cls);
        char where[64];
        std::snprintf(where, sizeof where, "class %u at %04X", b.cls, b.at);
        if (!code) return std::string(where) + ": no such class";
        auto it = work.find(b.cls);
        if (it == work.end()) {
            std::vector<uint8_t> cur = uc.patched_code(b.cls);
            if (cur.empty()) cur.assign(code, code + size);  // (no overlay yet: code() is the original)
            it = work.emplace(b.cls, std::move(cur)).first;
        }
        std::vector<uint8_t>& cur = it->second;
        const UsecodeFile::Gate gate = b.option == "v212" ? UsecodeFile::kGateV212 : UsecodeFile::kGateBugFixes;
        if ((!uc.patched_code(b.cls).empty() && uc.patch_gate(b.cls) != gate) || gates.emplace(b.cls, gate).first->second != gate)
            return std::string(where) + ": the class already has patches of another option";
        if (b.append) {
            if (b.at != cur.size()) return std::string(where) + " is not the end of the class code";
            cur.insert(cur.end(), b.replace.begin(), b.replace.end());
            return "";
        }
        if (b.at + b.expect.size() > cur.size()) return std::string(where) + " is outside the class";
        for (size_t i = 0; i < b.expect.size(); ++i)
            if (cur[b.at + i] != b.expect[i]) return std::string(where) + " does not hold the expected bytes";
        for (size_t i = 0; i < b.replace.size(); ++i) cur[b.at + i] = b.replace[i];
        return "";
    };
    // First a dry run: which groups have a patch that doesn't fit?
    std::vector<std::string> why(count);
    std::set<size_t> bad_groups;
    {
        std::map<unsigned, std::vector<uint8_t>> work;
        for (size_t i = 0; i < count; ++i) {
            why[i] = attempt(bytes[i], work);
            if (!why[i].empty()) bad_groups.insert(root(i));
        }
    }
    int n = 0;
    std::map<unsigned, std::vector<uint8_t>> work;
    gates.clear();
    for (size_t i = 0; i < count; ++i) {
        if (bad_groups.count(root(i))) {
            if (!why[i].empty()) problems.push_back(bytes[i].id + ": " + why[i]);
            else problems.push_back(bytes[i].id + ": switched off with the rest of its fix");
            continue;
        }
        attempt(bytes[i], work);
        ++n;
    }
    for (auto& [cls, code] : work) uc.set_patched_code(cls, std::move(code), gates[cls]);
    return n;
}

int StoryPatches::keep_texts_found_in(const UsecodeFile& uc) {
    int dropped = 0;
    std::vector<Text> kept;
    for (const Text& t : texts) {
        if (uc.contains_text(t.find)) kept.push_back(t);
        else ++dropped;
    }
    texts = std::move(kept);
    return dropped;
}

std::string StoryPatches::fix_text(const std::string& s) const {
    if (texts.empty() || !original_bug_fixes()) return s;
    std::string out = s;
    for (const Text& t : texts) {
        size_t pos = 0;
        while ((pos = out.find(t.find, pos)) != std::string::npos) {
            size_t end = pos + t.find.size();
            if (t.word && ((pos > 0 && word_char(out[pos - 1])) || (end < out.size() && word_char(out[end])))) {
                pos = end;
                continue;
            }
            out.replace(pos, t.find.size(), t.replace);
            pos += t.replace.size();
        }
    }
    return out;
}

bool load_story_patches(UsecodeFile& uc, const std::string& dir, std::string& err) {
    StoryPatches& p = story_patches();
    p = StoryPatches();
    uc.clear_patches();
    if (!p.load_dir(dir, err)) return false;
    std::vector<std::string> problems;
    int n = p.install(uc, problems);
    if (!compat::usecode_known()) {
        // Another copy: a correction only for a typo this copy has (its words may be another language's).
        int dropped = p.keep_texts_found_in(uc);
        if (dropped) PG_LOG("story patches: switched off %d text corrections: their words are not in this copy's usecode", dropped);
    }
    for (const std::string& s : problems) PG_LOG("story patches: switched off %s", s.c_str());
    PG_LOG("story patches: %d usecode patches, %zu text corrections (from %s)", n, p.texts.size(), dir.c_str());
    if (!problems.empty()) err = problems.front();
    return problems.empty();
}

}  // namespace pg
