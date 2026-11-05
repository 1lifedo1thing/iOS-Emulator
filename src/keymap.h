// Key-to-touch mapping: keys and controller buttons that hold fingers on the iOS screen (the Controls
// keyboard mode).
//
// Each device has a Keymap, kept as keymap.json in its folder, so Erase leaves it alone. It holds one
// Layout for each orientation, because an app's buttons sit elsewhere when the device is turned.
// A control's fingers are placed on the screen as shown; ToPanel (skin.h) turns them into touches.
// A Tap or MultiTap has one key and holds its fingers still. A Joystick has four keys that steer one
// finger around a ring, like a thumb on a game's on-screen stick. A key belongs to one control.
//
// A controller is more keys: Dear ImGui's Win32 backend reads XInput controller 0 into the
// ImGuiKey_Gamepad* keys, with each stick as four analog keys, so a half-pushed stick steers a
// joystick's finger halfway to its ring. keymap.json stores them by name like any other key.
#pragma once

#include "common.h"
#include "skin.h"

#include <array>

// What a control does while its key is held. A new kind adds a value here and its name in keymap.cpp.
enum class ControlType {
    Tap,        // one finger
    MultiTap,   // several fingers, each placed on its own
    Joystick,   // one finger, steered by four keys around a centre
};

constexpr int kMinFingers = 2, kMaxFingers = 5;   // a MultiTap's; the touch panel holds 5

constexpr int kJoystickKeys = 4;
const char *const kDirections[kJoystickKeys] = {"Up", "Left", "Down", "Right"};   // the order of Control::directions
constexpr float kJoystickRadius = 0.08f, kMinJoystickRadius = 0.04f, kMaxJoystickRadius = 0.25f;

// A controller's four keys for a whole joystick, in the order of kDirections.
constexpr std::array<ImGuiKey, kJoystickKeys> kDpadKeys = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadLeft,
                                                           ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadRight};
constexpr std::array<ImGuiKey, kJoystickKeys> kLeftStickKeys = {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickLeft,
                                                                ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickRight};

struct Control {
    ImGuiKey key = ImGuiKey_None;                  // the key or controller button; None until one is
                                                   // chosen. A Joystick uses `directions` instead
    ControlType type = ControlType::Tap;
    std::vector<ImVec2> spots = {ImVec2(0.5f, 0.5f)};   // where each finger lands, 0 to 1 across the
                                                        // screen as shown; never empty. A Joystick has
                                                        // one: its centre
    std::array<ImGuiKey, kJoystickKeys> directions = {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D};   // None if unbound
    float radius = kJoystickRadius;                // a Joystick's, as a share of the screen's width

    // Its keys by number, for code that handles every kind: a Joystick's four, or the one `key`.
    int KeyCount() const { return type == ControlType::Joystick ? kJoystickKeys : 1; }
    ImGuiKey Key(int index) const { return type == ControlType::Joystick ? directions[index] : key; }
    ImGuiKey &Key(int index) { return type == ControlType::Joystick ? directions[index] : key; }
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

// One frame of a joystick's finger: from `at`, towards the point the keys push it to, reaching the ring
// from the centre in about 3 frames. `strengths` are how far each of directions[] is pushed, 0 to 1: a
// keyboard key held is 1, a key not held 0. They add up to a direction with length up to 1, so a
// half-pushed stick stops halfway to the ring, and opposite keys held fully return it to the centre.
// `aspect` is the screen's width over its height: spots are fractions of each side, so a round ring
// needs it. A ring past the screen's edge is cut off by the edge.
ImVec2 JoystickStep(const Control &control, const std::array<float, kJoystickKeys> &strengths, ImVec2 at, float aspect);

// The key keymap.json calls `name`, or None.
ImGuiKey KeyNamed(const std::string &name);

// A key that can be bound to a control: the keyboard's or the controller's. F2 and F11 switch the app's
// modes and Esc cancels, so those can't.
bool BindableKey(ImGuiKey key);

// What else holds `key`, in words ("a tap", "this joystick's Up"), or "" if nothing does. Key number
// `slot` of `controls[control]` is the one being bound, so it doesn't count.
std::string KeyOwner(const std::vector<Control> &controls, int control, int slot, ImGuiKey key);

// Takes `key` away from every control that has it. Those stay, unbound, until they get another.
void ClearKey(std::vector<Control> &controls, ImGuiKey key);
