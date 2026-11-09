// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Answer-menu navigation and the exit answer (game/ask_nav.h).
#include "game/ask_nav.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pg {

int ask_move(const std::vector<AskMenu::Button>& b, int sel, AskDir d) {
    int n = int(b.size());
    if (n == 0) return -1;
    if (sel < 0 || sel >= n) return 0;
    if (d == kAskLeft) return (sel + n - 1) % n;
    if (d == kAskRight) return (sel + 1) % n;
    std::vector<int> rows;
    for (const auto& k : b) rows.push_back(k.y);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.size() < 2) return sel;
    int r = int(std::find(rows.begin(), rows.end(), b[size_t(sel)].y) - rows.begin());
    int nr = int(rows.size());
    int t = rows[size_t((r + (d == kAskDown ? 1 : nr - 1)) % nr)];
    const auto& c = b[size_t(sel)];
    int cx2 = 2 * c.x + c.w;
    int best = -1, best_dist = 0, best_ov = 0;
    for (int i = 0; i < n; ++i) {
        const auto& k = b[size_t(i)];
        if (k.y != t) continue;
        int dist = std::abs(2 * k.x + k.w - cx2);
        int ov = std::min(k.x + k.w, c.x + c.w) - std::max(k.x, c.x);
        if (best < 0 || dist < best_dist || (dist == best_dist && ov > best_ov)) { best = i; best_dist = dist; best_ov = ov; }
    }
    return best < 0 ? sel : best;
}

int ask_exit_kind(const std::string& answer) {
    std::string s;
    for (char ch : answer) {
        if (ch == ' ' && (s.empty() || s.back() == ' ')) continue;
        s += char(std::tolower(static_cast<unsigned char>(ch)));
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.' || s.back() == '!' || s.back() == '?')) s.pop_back();
    // Strings seen in the game's answer menus (EUSECODE.FLX) and engine/data/companions.json.
    static const char* const kBack[] = {
        "back", "go back", "something else", "talk about something else", "let's talk about something else",
        "let us talk about something else", "other topics", "another topic", "never mind", "nevermind",
        "never mind, thank you", "nevermind, thank you", "nothing", "nothing, thank you", "nothing, nevermind",
        "nothing. nevermind", "nothing, never mind", "nothing right now", "umm... nothing right now", "done",
        "none", "no more", "that is all", "that's all", "enough", "cancel",
    };
    static const char* const kBye[] = {
        "bye", "goodbye", "good bye", "farewell", "good day", "good day to you", "leave", "exit", "i must go",
        "i must be going", "no, sorry. goodbye", "uhm, ... i've got to leave now",
    };
    for (const char* k : kBack)
        if (s == k) return 2;
    if (s.rfind("enough about ", 0) == 0) return 2;
    for (const char* k : kBye)
        if (s == k) return 1;
    for (const char* p : {"goodbye, ", "farewell, ", "good day to you, "})
        if (s.rfind(p, 0) == 0) return 1;
    return 0;
}

int ask_exit_index(const std::vector<std::string>& answers) {
    int back = -1, bye = -1;
    for (size_t i = 0; i < answers.size(); ++i) {
        int k = ask_exit_kind(answers[i]);
        if (k == 2) back = int(i);
        if (k == 1) bye = int(i);
    }
    return back >= 0 ? back : bye;
}

}  // namespace pg
