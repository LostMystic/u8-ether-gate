// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// "Everyone knows": once Devon has proclaimed the Avatar in Tenebrae (the Virtues mark
// "known_avatar", data/dialog/devon.json), the people of Pagan stop calling the Avatar "stranger".
// The engine's own design (PLAN-VIRTUES.md "Every virtue has deeds"; NOTES.md "Known as the Avatar").
//
// data/avatar_title.json:
//   {"mark": "known_avatar", "title": "Avatar", "name_in_speech": true,
//    "classes": {"SALKIND": [{"find": "Greetings stranger.", "replace": "Greetings, Avatar."},
//                            {"find": "stranger", "replace": "Avatar"}], ...}}
// Rules are per usecode class (the class whose code creates the string, by name) and are tried in
// order on each string a script creates (push.str), each replacing every occurrence of its phrase.
// So a phrase rule placed before a class's word rule gets the grammar right ("a stranger like you"),
// and a class without a word rule only changes the lines listed. Matching is on whole words
// ("word": false for a part of a word); a phrase starting with a small letter also matches it
// capitalised, and then the replacement is capitalised too ("Stranger, are you lost?"). "exact":
// true matches only a string that is the phrase alone (a word the script joins to other strings).
// name_in_speech: an NPC's spoken line (not the Avatar's, not an answer menu) says the title where
// the script put the Avatar's name ("Greetings once again, Avatar.").
// Nothing here is saved; the mark is (game/virtues.h).
#include <map>
#include <string>
#include <vector>

namespace pg {

struct AvatarTitle {
    struct Rule {
        std::string find, replace;
        bool word = true;
        bool exact = false;  // "exact": the whole string (trailing spaces aside) must be `find`
    };
    std::string mark = "known_avatar";
    std::string title = "Avatar";
    bool name_in_speech = true;
    std::map<std::string, std::vector<Rule>> classes;

    bool load_text(const std::string& text, std::string& err);
    bool load_file(const std::string& path, std::string& err);
    bool empty() const { return classes.empty(); }
    // Applies class `cls`'s rules to `s`; returns the number of replacements made.
    int rewrite(const std::string& cls, std::string& s) const;
    // `text` with every whole-word `name` replaced by the title (name_in_speech).
    std::string with_title(const std::string& text, const std::string& name) const;
    // A rule's phrase found in `s` (the same matching rewrite() uses), for tests.
    static size_t find_phrase(const std::string& s, const std::string& phrase, bool word, size_t from, bool& capital);
};

}  // namespace pg
