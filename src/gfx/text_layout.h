// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <string>
#include <vector>

#include "gfx/font.h"

namespace pg {

// One laid-out page of text (specs/formats/fonts.md "Word wrapping", text.md 3).
struct TextPage {
    std::vector<std::string> lines;
    unsigned chars = 0;  // characters of the source text on this page (for bark timing)
    int width = 0, height = 0;
};

// Word-wraps `text` into pages of at most w x h pixels (h <= 0: one unlimited page).
// paged: '*' also starts a new page (books, scrolls).
std::vector<TextPage> layout_text(const ShapeFont& font, const std::string& text, int w, int h, bool paged = false);

}  // namespace pg
