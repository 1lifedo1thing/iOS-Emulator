#include "keymap.h"

#include <algorithm>

namespace {

const float kNewFingerGap = 0.12f;   // far enough apart for the editor's circles to be grabbed one by one

const char *TypeName(ControlType type) { return type == ControlType::MultiTap ? "multitap" : "tap"; }

bool KeyboardKey(int key) { return key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_GamepadStart; }

// Keys are stored by name: ImGuiKey's numbers change between versions of Dear ImGui.
ImGuiKey KeyNamed(const std::string &name) {
    for (int key = ImGuiKey_NamedKey_BEGIN; KeyboardKey(key); key++)
        if (name == ImGui::GetKeyName(ImGuiKey(key)))
            return ImGuiKey(key);
    return ImGuiKey_None;
}

ImVec2 OnScreen(ImVec2 spot) { return ImVec2(std::clamp(spot.x, 0.0f, 1.0f), std::clamp(spot.y, 0.0f, 1.0f)); }

Json LayoutJson(const Layout &layout) {
    Json controls = Json::array();
    for (const Control &control : layout.controls) {
        Json spots = Json::array();
        for (const ImVec2 &spot : control.spots)
            spots.push_back(Json::array({spot.x, spot.y}));
        controls.push_back(Json{{"key", control.key == ImGuiKey_None ? "" : ImGui::GetKeyName(control.key)},
                                {"type", TypeName(control.type)},
                                {"spots", spots}});
    }
    return {{"name", layout.name}, {"controls", controls}};
}

// A control's fingers. Files from before fingers were placed one by one have a centre, a finger
// count and a gap measured on the device held upright, which becomes that row. Turned, a screen is
// about 3:2, close enough for a row the editor can straighten.
std::vector<ImVec2> SpotsFrom(const Json &item, bool multi, bool landscape) {
    std::vector<ImVec2> spots;
    if (item.contains("spots")) {
        for (const Json &spot : item["spots"])
            spots.push_back(OnScreen(ImVec2(spot.at(0).get<float>(), spot.at(1).get<float>())));
        return spots;
    }
    const int count = multi ? std::clamp(item.value("fingers", kMinFingers), kMinFingers, kMaxFingers) : 1;
    const float gap = std::clamp(item.value("spread", 0.06f), 0.02f, 0.2f) * (landscape ? 2.0f / 3 : 1.0f);
    const float row = gap * (count - 1);
    const float left = std::clamp(item.value("x", 0.5f) - row / 2, 0.0f, 1.0f - row);
    for (int i = 0; i < count; i++)
        spots.push_back(OnScreen(ImVec2(left + gap * i, item.value("y", 0.5f))));
    return spots;
}

Layout LayoutFrom(const Json &j, bool landscape) {
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
        if (std::vector<ImVec2> spots = SpotsFrom(item, control.type == ControlType::MultiTap, landscape); !spots.empty())
            control.spots = spots;
        const int count = int(control.spots.size());
        SetFingerCount(control, control.type == ControlType::MultiTap ? std::clamp(count, kMinFingers, kMaxFingers) : 1);
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
        keymap.portrait = LayoutFrom(j.at("portrait"), false);
        keymap.landscape = LayoutFrom(j.at("landscape"), true);
    } catch (const std::exception &e) {
        Log("Couldn't read %s: %s", PathText(file).c_str(), e.what());
    }
    return keymap;
}

void Keymap::Save(const fs::path &file) const {
    const Json j = {{"portrait", LayoutJson(portrait)}, {"landscape", LayoutJson(landscape)}};
    SaveFile(file, j.dump(2));
}

void SetFingerCount(Control &control, int count) {
    if (int(control.spots.size()) > count)
        control.spots.resize(count);
    while (int(control.spots.size()) < count) {
        const ImVec2 last = control.spots.back();
        const float x = last.x + kNewFingerGap;
        control.spots.push_back(ImVec2(x > 1 ? x - 1 : x, last.y));   // past the edge, it starts again from the left
    }
}

void MoveControl(Control &control, ImVec2 by, int finger) {
    if (finger >= 0) {
        ImVec2 &spot = control.spots[finger];
        spot = OnScreen(ImVec2(spot.x + by.x, spot.y + by.y));
        return;
    }
    ImVec2 low(1, 1), high(0, 0);
    for (const ImVec2 &spot : control.spots) {
        low = ImVec2(std::min(low.x, spot.x), std::min(low.y, spot.y));
        high = ImVec2(std::max(high.x, spot.x), std::max(high.y, spot.y));
    }
    by = ImVec2(std::clamp(by.x, -low.x, 1 - high.x), std::clamp(by.y, -low.y, 1 - high.y));
    for (ImVec2 &spot : control.spots)
        spot = ImVec2(spot.x + by.x, spot.y + by.y);
}

bool BindableKey(ImGuiKey key) {
    return KeyboardKey(key) && key != ImGuiKey_F2 && key != ImGuiKey_F11 && key != ImGuiKey_Escape;
}
