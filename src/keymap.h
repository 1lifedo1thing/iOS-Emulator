// Key-to-touch mapping: keyboard keys that hold fingers on the iOS screen (the Controls keyboard mode).
//
// Each device has a Keymap, kept as keymap.json in its folder, so Erase leaves it alone. It holds one
// Layout for each orientation, because an app's buttons sit elsewhere when the device is turned.
// A control's fingers are placed on the screen as shown; ToPanel (skin.h) turns them into touches.
#pragma once

#include "common.h"
#include "skin.h"

// What a control does while its key is held. A new kind adds a value here and its name in keymap.cpp.
enum class ControlType {
    Tap,        // one finger
    MultiTap,   // several fingers, each placed on its own
};

constexpr int kMinFingers = 2, kMaxFingers = 5;   // a MultiTap's; the touch panel holds 5

struct Control {
    ImGuiKey key = ImGuiKey_None;                  // the key on the keyboard; None until one is chosen
    ControlType type = ControlType::Tap;
    std::vector<ImVec2> spots = {ImVec2(0.5f, 0.5f)};   // where each finger lands, 0 to 1 across the
                                                        // screen as shown; never empty
};

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

// Gives a control `count` fingers: the last ones go, and new ones land beside the last.
void SetFingerCount(Control &control, int count);

// Moves one finger, or with `finger` -1 all of them. They stay on the screen, and all together they
// keep their shape.
void MoveControl(Control &control, ImVec2 by, int finger);

// A key that can be bound to a control. F2 and F11 switch the app's modes and Esc cancels, so those can't.
bool BindableKey(ImGuiKey key);
