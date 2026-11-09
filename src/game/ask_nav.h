// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Answer-menu helpers for the keyboard and the controller (the engine's own design): moving
// the highlight by where the answers are drawn, and finding the answer that leaves the menu
// (a sub-menu's "back" / "something else", else the conversation's "bye").
#pragma once

#include <string>
#include <vector>

#include "game/world.h"

namespace pg {

enum AskDir { kAskUp, kAskDown, kAskLeft, kAskRight };

// The highlight after one step in direction d from answer `sel` (buttons as laid out by
// World::ask: rows that wrap horizontally, in reading order). Left / Right step through the
// answers in order (wrapping round to the previous / next row); Up / Down go to the nearest
// answer (by centre, then overlap) in the row above / below, wrapping top <-> bottom.
// sel < 0 (nothing highlighted yet) gives 0; -1 for an empty menu.
int ask_move(const std::vector<AskMenu::Button>& buttons, int sel, AskDir d);

// How an answer leaves the menu: 2 = back to the topics before ("back", "never mind",
// "nothing, thank you", "enough about ..."), 1 = ends the conversation ("bye", "goodbye",
// "farewell", "good day"), 0 = neither. Case and trailing punctuation do not matter.
int ask_exit_kind(const std::string& answer);

// The answer B picks: the last kind-2 answer, else the last kind-1 one; -1 if there is none.
int ask_exit_index(const std::vector<std::string>& answers);

}  // namespace pg
