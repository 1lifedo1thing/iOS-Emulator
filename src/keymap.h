// Key-to-touch mapping: keyboard keys that hold fingers on the iOS screen (the Controls keyboard mode).
//
// Each device has a Keymap, kept as keymap.json in its folder, so Erase leaves it alone. It holds one
// Layout for each orientation, because an app's buttons sit elsewhere when the device is turned.
// A control's place is measured on the screen as shown; ToPanel (skin.h) turns its fingers into touches.
#pragma once

#include "common.h"
#include "skin.h"

// What a control does while its key is held. A new kind adds a value here, its name in keymap.cpp
// and its fingers in FingerSpots.
enum class ControlType {
    Tap,        // one finger
    MultiTap,   // several fingers in a row
};

struct Control {
    ImGuiKey key = ImGuiKey_None;           // the key on the keyboard; None until one is chosen
    ControlType type = ControlType::Tap;
    float x = 0.5f, y = 0.5f;               // the centre, 0 to 1 across the screen as shown
    int fingers = 2;                        // MultiTap: how many, 2 to kMaxRowFingers
    float spread = 0.06f;                   // MultiTap: the gap between fingers, as a fraction of the
                                            // width of the device held upright
};

constexpr int kMinRowFingers = 2, kMaxRowFingers = 5;
constexpr float kMinSpread = 0.02f, kMaxSpread = 0.2f;   // 5 fingers at the widest still fit on screen

struct Layout {
    std::string name = "Default";
    std::vector<Control> controls;
};

struct Keymap {
    Layout portrait, landscape;

    Layout &For(Orientation orientation) { return orientation == Orientation::Landscape ? landscape : portrait; }

    static Keymap Load(const fs::path &file);   // empty if there's no file or it can't be read
    void Save(const fs::path &file) const;      // throws if it can't be written
};

// Where a control's fingers land, 0 to 1 across the screen as shown. `screen` is its size as shown,
// in pixels: a row keeps the same gap in pixels in both orientations, and near an edge it shifts
// inward as a whole.
std::vector<ImVec2> FingerSpots(const Control &control, ImVec2 screen);

// A key that can be bound to a control. F2 and F11 switch the app's modes and Esc cancels, so those can't.
bool BindableKey(ImGuiKey key);
