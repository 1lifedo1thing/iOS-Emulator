#include "keymap.h"

#include <algorithm>
#include <cmath>

namespace {

const float kNewFingerGap = 0.12f;   // far enough apart for the editor's circles to be grabbed one by one
const int kEaseFrames = 3;           // a joystick's finger takes this long from the centre to the ring

const char *TypeName(ControlType type) {
    switch (type) {
    case ControlType::MultiTap: return "multitap";
    case ControlType::Joystick: return "joystick";
    default: return "tap";
    }
}

bool KeyboardKey(int key) { return key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_GamepadStart; }

// Keys are stored by name: ImGuiKey's numbers change between versions of Dear ImGui.
ImGuiKey KeyNamed(const std::string &name) {
    for (int key = ImGuiKey_NamedKey_BEGIN; KeyboardKey(key); key++)
        if (name == ImGui::GetKeyName(ImGuiKey(key)))
            return ImGuiKey(key);
    return ImGuiKey_None;
}

std::string KeyText(ImGuiKey key) { return key == ImGuiKey_None ? "" : ImGui::GetKeyName(key); }

ImVec2 OnScreen(ImVec2 spot) { return ImVec2(std::clamp(spot.x, 0.0f, 1.0f), std::clamp(spot.y, 0.0f, 1.0f)); }

Json LayoutJson(const Layout &layout) {
    Json controls = Json::array();
    for (const Control &control : layout.controls) {
        Json spots = Json::array();
        for (const ImVec2 &spot : control.spots)
            spots.push_back(Json::array({spot.x, spot.y}));
        Json item = {{"key", KeyText(control.key)}, {"type", TypeName(control.type)}, {"spots", spots}};
        if (control.type == ControlType::Joystick) {
            item["radius"] = control.radius;
            item["keys"] = Json::array();   // in the order of kDirections
            for (ImGuiKey key : control.directions)
                item["keys"].push_back(KeyText(key));
        }
        controls.push_back(item);
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
        else if (type == TypeName(ControlType::Joystick))
            control.type = ControlType::Joystick;
        else if (type != TypeName(ControlType::Tap))
            continue;   // a kind this version doesn't know
        control.key = KeyNamed(item.value("key", ""));
        if (control.type == ControlType::Joystick) {
            control.radius = std::clamp(item.value("radius", kJoystickRadius), kMinJoystickRadius, kMaxJoystickRadius);
            const Json keys = item.value("keys", Json::array());
            for (int i = 0; i < kJoystickKeys; i++) {
                const ImGuiKey key = i < int(keys.size()) ? KeyNamed(keys[i].get<std::string>()) : ImGuiKey_None;
                control.directions[i] = BindableKey(key) ? key : ImGuiKey_None;
            }
        }
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

ImVec2 JoystickStep(const Control &control, unsigned held, ImVec2 at, float aspect) {
    const auto down = [&](int direction) { return (held >> direction & 1) ? 1.0f : 0.0f; };
    float x = down(3) - down(1), y = down(2) - down(0);   // right less left, down less up
    if (const float length = std::hypot(x, y); length > 0) {
        x /= length;
        y /= length;
    }
    const ImVec2 centre = control.spots.front();
    const ImVec2 target = OnScreen(ImVec2(centre.x + x * control.radius, centre.y + y * control.radius * aspect));

    // Measured in screen widths, so the finger moves as fast along both sides.
    const float toX = target.x - at.x, toY = (target.y - at.y) / aspect;
    const float distance = std::hypot(toX, toY), step = control.radius / kEaseFrames;
    if (distance <= step)
        return target;
    return ImVec2(at.x + toX / distance * step, at.y + toY / distance * step * aspect);
}

bool BindableKey(ImGuiKey key) {
    return KeyboardKey(key) && key != ImGuiKey_F2 && key != ImGuiKey_F11 && key != ImGuiKey_Escape;
}

std::string KeyOwner(const std::vector<Control> &controls, int control, int slot, ImGuiKey key) {
    for (int i = 0; i < int(controls.size()); i++) {
        const Control &other = controls[i];
        for (int k = 0; k < other.KeyCount(); k++) {
            if ((i == control && k == slot) || other.Key(k) != key)
                continue;
            if (other.type == ControlType::Joystick)
                return std::string(i == control ? "this" : "a") + " joystick's " + kDirections[k];
            return other.type == ControlType::MultiTap ? "a multi-finger tap" : "a tap";
        }
    }
    return "";
}

void ClearKey(std::vector<Control> &controls, ImGuiKey key) {
    for (Control &control : controls)
        for (int k = 0; k < control.KeyCount(); k++)
            if (control.Key(k) == key)
                control.Key(k) = ImGuiKey_None;
}
