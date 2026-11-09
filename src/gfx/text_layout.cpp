// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "gfx/text_layout.h"

#include <algorithm>

namespace pg {

namespace {
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '%' || c == '^'; }
bool is_break(char c) { return c == '~' || c == '*' || c == '\n'; }
}  // namespace

std::vector<TextPage> layout_text(const ShapeFont& font, const std::string& text, int w, int h, bool paged) {
    std::vector<TextPage> pages(1);
    const int adv = font.line_advance(), fh = font.height();
    std::string line;
    unsigned page_start = 0;
    size_t line_start = 0;  // where the line being built begins in the text
    auto line_fits = [&](const TextPage& p) {
        if (h <= 0) return true;
        int top = int(p.lines.size()) * adv;
        return top + fh <= h;
    };
    auto finish_line = [&](size_t pos) {
        TextPage* p = &pages.back();
        if (!line_fits(*p)) {
            // The line moves to the new page, and its characters (its time) with it.
            unsigned from = unsigned(line.empty() ? pos : line_start);
            p->chars = from - page_start;
            page_start = from;
            pages.emplace_back();
            p = &pages.back();
        }
        p->lines.push_back(line);
        p->width = std::max(p->width, font.text_width(line));
        line.clear();
    };
    size_t i = 0, n = text.size();
    while (i < n) {
        char c = text[i];
        if (is_break(c)) {
            finish_line(i);
            ++i;
            if (c == '*' && paged) {
                pages.back().chars = unsigned(i) - page_start;
                page_start = unsigned(i);
                pages.emplace_back();
            }
            continue;
        }
        if (is_space(c)) {
            if (!line.empty()) line += (c == '%' || c == '\t') ? "    " : " ";
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && !is_space(text[j]) && !is_break(text[j])) ++j;
        std::string word = text.substr(i, j - i);
        std::string trial = line + word;
        if (line.empty()) line_start = i;
        if (font.text_width(trial) <= w || line.empty()) {
            if (line.empty() && font.text_width(word) > w) {
                // Break mid-word at the last character that fits.
                size_t k = 1;
                while (k < word.size() && font.text_width(word.substr(0, k + 1)) <= w) ++k;
                line = word.substr(0, k);
                finish_line(i);
                i += k;
                continue;
            }
            line = trial;
            i = j;
        } else {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            finish_line(i);
        }
    }
    if (!line.empty()) finish_line(n);
    pages.back().chars = unsigned(n) - page_start;
    for (TextPage& p : pages) p.height = p.lines.empty() ? 0 : int(p.lines.size()) * adv - adv + fh;
    while (pages.size() > 1 && pages.back().lines.empty()) pages.pop_back();
    return pages;
}

}  // namespace pg
