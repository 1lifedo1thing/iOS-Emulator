#include "keymap.h"

#include <algorithm>

namespace {

const char *TypeName(ControlType type) { return type == ControlType::MultiTap ? "multitap" : "tap"; }

bool KeyboardKey(int key) { return key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_GamepadStart; }

// Keys are stored by name: ImGuiKey's numbers change between versions of Dear ImGui.
ImGuiKey KeyNamed(const std::string &name) {
    for (int key = ImGuiKey_NamedKey_BEGIN; KeyboardKey(key); key++)
        if (name == ImGui::GetKeyName(ImGuiKey(key)))
            return ImGuiKey(key);
    return ImGuiKey_None;
}

Json LayoutJson(const Layout &layout) {
    Json controls = Json::array();
    for (const Control &control : layout.controls) {
        Json j = {{"key", control.key == ImGuiKey_None ? "" : ImGui::GetKeyName(control.key)},
                  {"type", TypeName(control.type)},
                  {"x", control.x},
                  {"y", control.y}};
        if (control.type == ControlType::MultiTap) {
            j["fingers"] = control.fingers;
            j["spread"] = control.spread;
        }
        controls.push_back(j);
    }
    return {{"name", layout.name}, {"controls", controls}};
}

Layout LayoutFrom(const Json &j) {
    Layout layout;
    layout.name = j.value("name", layout.name);
    for (const Json &item : j.value("controls", Json::array())) {
        Control control;
        const std::string type = item.value("type", "");
        if (type == TypeName(ControlType::MultiTap))
            control.type = ControlType::MultiTap;
        else if (type != TypeName(ControlType::Tap))
            continue;   // a kind this version doesn't know
        control.key = KeyNamed(item.value("key", ""));
        control.x = std::clamp(item.value("x", control.x), 0.0f, 1.0f);
        control.y = std::clamp(item.value("y", control.y), 0.0f, 1.0f);
        control.fingers = std::clamp(item.value("fingers", control.fingers), kMinRowFingers, kMaxRowFingers);
        control.spread = std::clamp(item.value("spread", control.spread), kMinSpread, kMaxSpread);
        layout.controls.push_back(control);
    }
    return layout;
}

}  // namespace

Keymap Keymap::Load(const fs::path &file) {
    Keymap keymap;
    if (!fs::exists(file))
        return keymap;
    try {
        const Json j = Json::parse(LoadFile(file));
        keymap.portrait = LayoutFrom(j.at("portrait"));
        keymap.landscape = LayoutFrom(j.at("landscape"));
    } catch (const std::exception &e) {
        Log("Couldn't read %s: %s", PathText(file).c_str(), e.what());
    }
    return keymap;
}

void Keymap::Save(const fs::path &file) const {
    const Json j = {{"portrait", LayoutJson(portrait)}, {"landscape", LayoutJson(landscape)}};
    SaveFile(file, j.dump(2));
}

std::vector<ImVec2> FingerSpots(const Control &control, ImVec2 screen) {
    const int count = control.type == ControlType::MultiTap ? control.fingers : 1;
    const float gap = control.spread * std::min(screen.x, screen.y) / screen.x;
    const float row = gap * (count - 1);
    const float left = std::clamp(control.x - row / 2, 0.0f, 1.0f - row);
    std::vector<ImVec2> spots;
    for (int i = 0; i < count; i++)
        spots.push_back(ImVec2(left + gap * i, control.y));
    return spots;
}

bool BindableKey(ImGuiKey key) {
    return KeyboardKey(key) && key != ImGuiKey_F2 && key != ImGuiKey_F11 && key != ImGuiKey_Escape;
}
