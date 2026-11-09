// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>

namespace pg {

// Engine key codes, independent of SDL. Printable keys use their lower-case ASCII code.
enum Key : int {
    kKeyUnknown = 0, kKeyBackspace = 8, kKeyTab = 9, kKeyReturn = 13, kKeyEscape = 27, kKeySpace = 32,
    kKeyUp = 0x100, kKeyDown, kKeyLeft, kKeyRight, kKeyPageUp, kKeyPageDown, kKeyHome, kKeyEnd,
    kKeyInsert, kKeyDelete,
    kKeyF1 = 0x120, kKeyF2, kKeyF3, kKeyF4, kKeyF5, kKeyF6, kKeyF7, kKeyF8, kKeyF9, kKeyF10, kKeyF11, kKeyF12,
};

enum KeyMod : unsigned { kModShift = 1, kModCtrl = 2, kModAlt = 4 };

// Controller buttons and axes (milestone 6), numbered like SDL's GameController API.
enum PadButton : int {
    kPadA, kPadB, kPadX, kPadY, kPadBack, kPadGuide, kPadStart, kPadLStick, kPadRStick, kPadLB, kPadRB,
    kPadUp, kPadDown, kPadLeft, kPadRight, kPadButtons = 15
};
enum PadAxis : int { kAxisLX, kAxisLY, kAxisRX, kAxisRY, kAxisLT, kAxisRT, kPadAxes = 6 };

struct InputEvent {
    enum Type {
        kKeyDown, kKeyUp, kMouseMove, kMouseDown, kMouseUp, kWheel, kText, kQuit,
        kPadDown, kPadUp, kPadAxis, kPadAdded, kPadRemoved,  // controller (button / axis+value)
    } type = kQuit;
    int key = 0;        // Key, for key events
    unsigned mods = 0;  // KeyMod bits
    bool repeat = false;
    int x = 0, y = 0;   // mouse position in game-surface pixels (may be outside it)
    float fx = -1, fy = -1;  // the same with sub-pixel precision (-1: not known, use x / y)
    int button = 0;     // 1 left, 2 middle, 3 right
    int wheel = 0;
    char text[8] = {};  // UTF-8, for kText
    int axis = 0, value = 0;  // kPadAxis: PadAxis, -32768..32767 (triggers 0..32767)
    int pad = 0;              // controller instance id
    char name[48] = {};       // kPadAdded: the controller's name
};

}  // namespace pg
