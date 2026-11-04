#include "ui.h"

#include "common.h"
#include "library.h"
#include "engine.h"
#include "firmware.h"
#include "keymap.h"
#include "session.h"
#include "skin.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"
#include "../third_party/qemu-ios/qemu-ios-ui.h"

#include <GL/gl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cmath>
#include <functional>
#include <memory>
#include <set>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ---- Settings --------------------------------------------------------------------------------

// What the keyboard does while a device is on screen.
enum class KeyboardMode {
    Shortcuts,   // the hardware shortcuts
    Typing,      // what you type goes to iOS; the shortcuts take Ctrl
    Controls,    // keys hold fingers on the screen (keymap.h); the shortcuts take Ctrl
};
const char *const kKeyboardModes[] = {"Shortcuts", "Typing", "Controls"};   // by KeyboardMode

// The app's preferences, kept in <data>\settings.json.
struct Settings {
    bool internet = false;     // new sessions start with guest internet on
    bool webProxy = true;      // ...and HTTPS through the web proxy (proxy.h)
    bool headphones = false;   // route sound to the headphone jack
    bool muted = false;
    bool smooth = true;        // smooth scaling of the screen (off: sharp pixels)
    bool tilt = true;          // arrow keys tilt the device
    KeyboardMode keyboard = KeyboardMode::Shortcuts;
    KeyboardMode beforeControls = KeyboardMode::Shortcuts;   // the mode F2 goes back to
    bool showControls = true;  // faint labels show where the controls are, in Controls mode
    bool devTools = false;     // the developer tools show beside a running device
    std::string selected;      // the device selected last

    static fs::path File() { return DataDir() / "settings.json"; }

    static KeyboardMode ModeNamed(const std::string &name, KeyboardMode otherwise) {
        for (int mode = 0; mode < int(std::size(kKeyboardModes)); mode++)
            if (name == kKeyboardModes[mode])
                return KeyboardMode(mode);
        return otherwise;
    }

    void SetKeyboard(KeyboardMode mode) {
        if (keyboard != KeyboardMode::Controls)
            beforeControls = keyboard;
        keyboard = mode;
    }

    void Load() {
        try {
            Json j = Json::parse(LoadFile(File()));
            internet = j.value("internet", internet);
            webProxy = j.value("webProxy", webProxy);
            headphones = j.value("headphones", headphones);
            muted = j.value("muted", muted);
            smooth = j.value("smooth", smooth);
            tilt = j.value("tilt", tilt);
            const KeyboardMode old = j.value("sendKeys", false) ? KeyboardMode::Typing : keyboard;   // before the modes
            keyboard = ModeNamed(j.value("keyboard", ""), old);
            beforeControls = ModeNamed(j.value("beforeControls", ""), beforeControls);
            showControls = j.value("showControls", showControls);
            devTools = j.value("devTools", devTools);
            selected = j.value("selected", selected);
        } catch (...) {
            // first start, or an unreadable file: keep the defaults
        }
    }

    void Save() const {
        Json j = {{"internet", internet},
                  {"webProxy", webProxy},
                  {"headphones", headphones},
                  {"muted", muted},
                  {"smooth", smooth},
                  {"tilt", tilt},
                  {"keyboard", kKeyboardModes[int(keyboard)]},
                  {"beforeControls", kKeyboardModes[int(beforeControls)]},
                  {"showControls", showControls},
                  {"devTools", devTools},
                  {"selected", selected}};
        SaveFile(File(), j.dump(2));
    }

    bool ShortcutsTakeCtrl() const { return keyboard != KeyboardMode::Shortcuts; }

    // The keys of a hardware shortcut, as shown in tooltips: "H", or "Ctrl+H" when the keys do something else.
    std::string Shortcut(const char *keys) const { return (ShortcutsTakeCtrl() ? "Ctrl+" : "") + std::string(keys); }
};

// ---- Motion ----------------------------------------------------------------------------------

// What the accelerometer feels, in the guest's portrait axes (x right, y up, z toward you, m/s^2; at
// rest it points away from the ground). In portrait the device lies flat, face up. In landscape it
// stands on its side with Home on the right, so iOS turns the apps that support it. Turning back to
// portrait stands it upright for a second (iOS keeps its last orientation while a device lies flat).
// The arrow keys lean it, as LightTouch does: each lowers that edge, as shown, up to 35 degrees,
// easing there and back.
class Motion {
public:
    // Returns true when there's a new sample to send.
    bool Step(Orientation orientation, bool left, bool right, bool up, bool down, double acceleration[3]) {
        const uint64_t now = NowNs();
        const double dt = last_ ? std::min((now - last_) / 1e9, 0.1) : 0.0;
        last_ = now;

        // Which way is up for the device at rest.
        if (orientation == Orientation::Portrait && orientation_ == Orientation::Landscape)
            uprightUntil_ = now + 1'000'000'000;
        orientation_ = orientation;
        double base[3] = {0, 0, 1};                      // flat, face up
        if (orientation == Orientation::Landscape)
            base[0] = 1, base[2] = 0;                    // on its side, Home on the right
        else if (now < uprightUntil_)
            base[1] = 1, base[2] = 0;                    // upright

        // The arrows as shown, in the device's own directions: in landscape its top is on the left.
        if (orientation == Orientation::Landscape) {
            const bool shownLeft = left, shownRight = right, shownUp = up, shownDown = down;
            left = shownDown, right = shownUp, up = shownLeft, down = shownRight;
        }
        const double maxAngle = 35.0 * 3.14159265 / 180, reach = 150.0 * 3.14159265 / 180 * dt;
        const double rollTo = (int(right) - int(left)) * maxAngle, pitchTo = (int(down) - int(up)) * maxAngle;
        auto approach = [&](double from, double to) {
            return std::abs(to - from) <= reach ? to : from + std::copysign(reach, to - from);
        };
        const double roll = approach(roll_, rollTo), pitch = approach(pitch_, pitchTo);
        const bool moving = roll != roll_ || pitch != pitch_ || roll != 0 || pitch != 0;
        roll_ = roll;
        pitch_ = pitch;

        // Lean the base: rolling turns it about the device's y axis, pitching about its x axis.
        const double g = 9.80665;
        const double x1 = base[0] * std::cos(roll) - base[2] * std::sin(roll);
        const double z1 = base[0] * std::sin(roll) + base[2] * std::cos(roll);
        acceleration[0] = g * x1;
        acceleration[1] = g * (base[1] * std::cos(pitch) + z1 * std::sin(pitch));
        acceleration[2] = g * (-base[1] * std::sin(pitch) + z1 * std::cos(pitch));

        // Send while it moves (one last sample once it's back at rest), when its base changes, and
        // every second while it isn't lying flat (the worker holds a sample; this covers one sent
        // before iOS was up).
        const bool flat = base[2] == 1;
        const bool changed = base[0] != base_[0] || base[1] != base_[1] || base[2] != base_[2];
        const bool refresh = !flat && now - sent_ > 1'000'000'000;
        std::copy(base, base + 3, base_);
        const bool send = moving || wasMoving_ || changed || refresh;
        wasMoving_ = moving;
        if (send)
            sent_ = now;
        return send;
    }

private:
    Orientation orientation_ = Orientation::Portrait;
    double base_[3] = {0, 0, 1};
    double roll_ = 0, pitch_ = 0;
    uint64_t last_ = 0, sent_ = 0, uprightUntil_ = 0;
    bool wasMoving_ = false;
};

// ---- Screen textures -------------------------------------------------------------------------

struct Texture {
    GLuint id = 0;
    int width = 0, height = 0;

    void Upload(const std::vector<uint8_t> &bgra, int w, int h, bool smooth) {
        if (!id)
            glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (w != width || h != height) {
            // GL_RGB drops the guest's alpha channel, which isn't meaningful (iOS 3.x leaves it at 0):
            // kept, it would make the screen transparent, showing the bezel through it.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data());
            width = w;
            height = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data());
        }
    }

    void Release() {
        if (id)
            glDeleteTextures(1, &id);
        *this = {};
    }
};

// ---- Small UI helpers ------------------------------------------------------------------------

const ImVec4 kAccent(0.29f, 0.56f, 0.98f, 1.0f);
const ImVec4 kGood(0.35f, 0.80f, 0.45f, 1.0f);
const ImVec4 kWarn(0.95f, 0.70f, 0.25f, 1.0f);
const ImVec4 kBad(0.95f, 0.40f, 0.40f, 1.0f);
const ImVec4 kDim(0.60f, 0.62f, 0.66f, 1.0f);

void DimText(const std::string &text) { ImGui::TextColored(kDim, "%s", text.c_str()); }

const char *const kWebProxyTip =
    "HTTPS goes through a proxy on this computer, which speaks the old encryption iOS knows to iOS and modern "
    "encryption to websites. Most of today's websites need it on these iOS versions. Off, iOS connects to "
    "websites itself.";

// A button that's drawn disabled (and never fires) when `enabled` is false.
bool Button(const char *label, bool enabled = true, ImVec2 size = ImVec2(0, 0)) {
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    return clicked;
}

void Tooltip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

// Fonts besides the UI's own, loaded in RunUi (null when Windows lacks them): a large one for the
// version wheel, a semibold one for titles. g_icons tells whether the icons below were merged in.
ImFont *g_largeFont = nullptr, *g_titleFont = nullptr;
bool g_icons = false;

// Icons: glyphs of Windows' Segoe MDL2 Assets font, merged into the UI font.
namespace icon {
const char *const kRotate = "", *const kPause = "", *const kPlay = "", *const kCamera = "",
                  *const kFullscreen = "", *const kVolume = "", *const kMute = "",
                  *const kGlobe = "", *const kLock = "", *const kStop = "", *const kCode = "",
                  *const kLeft = "", *const kRight = "";
const char *const kRecord = "\xEE\x9F\x88";   // U+E7C8
const char *const kEdit = "\xEE\x9C\x8F";     // U+E70F
}

// A toolbar button: its icon (or `name`, without the icon font), drawn highlighted while `on`, and in
// `ink` when one is given.
bool IconButton(const char *icon, const char *name, const std::string &tip, bool on = false,
                const ImVec4 *ink = nullptr) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.33f, 0.56f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.40f, 0.66f, 1));
    }
    if (ink)
        ImGui::PushStyleColor(ImGuiCol_Text, *ink);
    ImGui::PushID(name);
    const bool clicked = g_icons ? ImGui::Button(icon, ImVec2(ImGui::GetFrameHeight() * 1.4f, 0)) : ImGui::Button(name);
    ImGui::PopID();
    ImGui::PopStyleColor((on ? 2 : 0) + (ink ? 1 : 0));
    Tooltip(tip.c_str());
    return clicked;
}

// The width IconButton takes.
float IconButtonWidth(const char *name) {
    return g_icons ? ImGui::GetFrameHeight() * 1.4f
                   : ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2;
}

float ButtonWidth(const char *label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; }

// Moves the cursor so that a row `width` pixels wide is centred in the space left on this line.
void CenterRow(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) / 2);
}

// Text in another font (or the UI's, when it's missing).
void FontText(ImFont *font, const std::string &text, ImVec4 color = ImVec4(1, 1, 1, 1)) {
    if (font)
        ImGui::PushFont(font);
    ImGui::TextColored(color, "%s", text.c_str());
    if (font)
        ImGui::PopFont();
}

// The iOS-Emulator folder in one of the user's folders (Pictures, Videos), or `fallback` in the
// app's data folder when Windows doesn't know it. Created if it's missing.
fs::path UserFolder(REFKNOWNFOLDERID folder, const char *fallback) {
    PWSTR path = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &path)))
        dir = fs::path(path) / "iOS-Emulator";
    CoTaskMemFree(path);
    if (dir.empty())
        dir = DataDir() / fallback;
    fs::create_directories(dir);
    return dir;
}

// A file name for something saved from a device now: "iPod 2024-05-01 14.03.22.png".
std::wstring StampedName(const Device &device, const char *extension) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64];
    snprintf(stamp, sizeof stamp, " %04d-%02d-%02d %02d.%02d.%02d.%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, extension);
    return Widen(device.name + stamp);
}

// ---- The drum wheel --------------------------------------------------------------------------

// A wheel's motion, kept between frames.
struct Drum {
    static constexpr float kBriskPull = 16, kFlingPull = 6;

    float position = 0;     // the row at its centre (between two while it turns)
    float target = 0;       // the row it's turning to
    float velocity = 0;     // rows per second
    float pull = kBriskPull;   // how hard the target draws it (1/s)
    bool dragging = false, dragged = false;
    float pressedAt = 0;    // where the mouse went down (y)
    int chosen = -1;        // the row it last reported

    bool Moving() const { return dragging || position != target || velocity != 0; }
};

struct WheelRow {
    std::string label, detail;   // "iOS 4.2.1", "8C148"
    bool tested = false;         // marked with a green dot
};

// A drum wheel like iOS's picker: the rows sit on a turning cylinder, and the one in the middle band
// is chosen. It turns with the mouse wheel, by dragging (it coasts a little when let go, then settles
// on a row), by clicking a row, and with Up and Down while the mouse is over it. `selected` is the
// chosen row, in and out.
void DrumPicker(const char *id, const std::vector<WheelRow> &rows, int &selected, Drum &drum, float width,
                int visibleRows = 7) {
    const int count = int(rows.size());
    if (count == 0)
        return;
    ImFont *font = g_largeFont ? g_largeFont : ImGui::GetFont();
    const float rowHeight = std::floor(font->FontSize * 1.55f), height = rowHeight * visibleRows;
    const float radius = height / 2;   // of the cylinder, so its visible half fills the wheel
    const ImGuiIO &io = ImGui::GetIO();
    const float dt = std::min(io.DeltaTime, 0.05f);
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + width, min.y + height);
    const float centerY = (min.y + max.y) / 2;

    const auto turnTo = [&](float row) {
        drum.target = row;
        drum.pull = Drum::kBriskPull;
    };
    if (selected != drum.chosen)   // chosen from outside (a new device): turn to it
        turnTo(float(selected));

    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
        drum.dragging = true;
        drum.dragged = false;
        drum.velocity = 0;
        drum.pressedAt = io.MousePos.y;
    }
    if (drum.dragging && ImGui::IsItemActive()) {
        // Dragging turns it under the mouse; the speed of the last 50 ms carries on when let go.
        const float rows = io.MouseDelta.y / rowHeight;
        drum.position = std::clamp(drum.position - rows, -0.4f, count - 0.6f);
        if (dt > 0)
            drum.velocity += (-rows / dt - drum.velocity) * (1 - std::exp(-dt / 0.05f));
        drum.dragged |= std::abs(io.MousePos.y - drum.pressedAt) > 4;
    }
    if (ImGui::IsItemDeactivated()) {
        drum.dragging = false;
        if (drum.dragged) {   // let go: aim for the row it would coast to
            drum.pull = std::abs(drum.velocity) > 2 ? Drum::kFlingPull : Drum::kBriskPull;
            drum.target = std::round(drum.position + drum.velocity / drum.pull);
        } else {               // a click: turn to the row under the mouse
            const float s = std::clamp((io.MousePos.y - centerY) / radius, -0.99f, 0.99f);
            turnTo(std::round(drum.position + std::asin(s) * radius / rowHeight));
            drum.velocity = 0;
        }
    }
    if (hovered && io.MouseWheel != 0)
        turnTo(std::round(drum.target) - io.MouseWheel);
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        turnTo(std::round(drum.target) - 1);
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        turnTo(std::round(drum.target) + 1);

    // Everything but a drag is one motion: a critically damped spring toward the target, solved
    // exactly so that it's the same at any frame rate. Its speed never jumps, and a fling, aimed at
    // where it would coast to, slows down evenly from the moment it's let go until it rests on a row.
    if (!drum.dragging) {
        drum.target = std::clamp(drum.target, 0.0f, float(count - 1));
        const float offset = drum.position - drum.target;
        const float carried = (drum.velocity + drum.pull * offset) * dt, decay = std::exp(-drum.pull * dt);
        drum.velocity = (drum.velocity - drum.pull * carried) * decay;
        drum.position = drum.target + (offset + carried) * decay;
        if (std::abs(drum.position - drum.target) < 0.002f && std::abs(drum.velocity) < 0.02f) {
            drum.position = drum.target;
            drum.velocity = 0;
        }
    }
    selected = std::clamp(int(std::lround(drum.position)), 0, count - 1);
    drum.chosen = selected;

    // The wheel: a dark well, the selection band, then the rows on the cylinder.
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetFontSize() * 0.6f;
    draw->AddRectFilled(min, max, IM_COL32(22, 24, 28, 255), rounding);
    const ImVec2 bandMin(min.x + 6, centerY - rowHeight / 2), bandMax(max.x - 6, centerY + rowHeight / 2);
    draw->AddRectFilled(bandMin, bandMax, IM_COL32(74, 143, 250, 38), rounding * 0.6f);
    draw->AddLine(ImVec2(bandMin.x, bandMin.y), ImVec2(bandMax.x, bandMin.y), IM_COL32(255, 255, 255, 40));
    draw->AddLine(ImVec2(bandMin.x, bandMax.y), ImVec2(bandMax.x, bandMax.y), IM_COL32(255, 255, 255, 40));
    draw->PushClipRect(min, max, true);
    const float pad = ImGui::GetFontSize() * 1.1f;
    // Rows glide between pixels while it turns (snapped, they'd crawl in visible steps as it slows
    // down), and sit sharply on a pixel at rest.
    const bool moving = drum.Moving();
    const auto pixel = [&](float y) { return moving ? y : std::floor(y); };
    for (int i = 0; i < count; i++) {
        const float angle = (i - drum.position) * rowHeight / radius;   // around the cylinder
        if (std::abs(angle) > 1.5f)
            continue;
        const float facing = std::cos(angle);   // 1 facing you, toward 0 at the top and bottom
        const float y = centerY + radius * std::sin(angle);
        const bool center = i == selected;
        const int alpha = int(255 * std::pow(facing, 2.0f));
        const float size = font->FontSize * (0.72f + 0.28f * facing);
        const ImVec2 labelSize = font->CalcTextSizeA(size, FLT_MAX, 0, rows[i].label.c_str());
        const ImU32 ink = center ? IM_COL32(255, 255, 255, alpha) : IM_COL32(170, 174, 182, alpha);
        draw->AddText(font, size, ImVec2(min.x + pad * 1.6f, pixel(y - labelSize.y / 2)), ink, rows[i].label.c_str());
        const ImVec2 detailSize = ImGui::CalcTextSize(rows[i].detail.c_str());
        draw->AddText(ImVec2(max.x - pad - detailSize.x, pixel(y - detailSize.y / 2)),
                      IM_COL32(140, 144, 152, int(alpha * 0.85f)), rows[i].detail.c_str());
        if (rows[i].tested)
            draw->AddCircleFilled(ImVec2(min.x + pad * 0.8f, y), size * 0.14f,
                                  IM_COL32(89, 204, 115, alpha));
    }
    draw->PopClipRect();
    // The cylinder's edges fade into the well.
    const float fade = height * 0.28f;
    const ImU32 well = IM_COL32(22, 24, 28, 255), clear = IM_COL32(22, 24, 28, 0);
    draw->AddRectFilledMultiColor(ImVec2(min.x, min.y + rounding), ImVec2(max.x, min.y + fade), well, well, clear, clear);
    draw->AddRectFilledMultiColor(ImVec2(min.x, max.y - fade), ImVec2(max.x, max.y - rounding), clear, clear, well, well);
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (count > 1 && hovered)
        ImGui::SetItemTooltip("Scroll, drag or click to choose. A green dot marks a version tested to run well.");
}

// ---- The app ---------------------------------------------------------------------------------

// A finger on the screen, as shown. Each has a fixed id, because the engine follows fingers by id:
// the mouse, its Alt mirror, the rest of its Shift row, then every finger of every control.
struct Finger {
    int id;
    ImVec2 at;
};
constexpr int kSwipeRow = 4;         // fingers in a Shift + drag: the mouse's and three more
constexpr float kSwipeGap = 0.08f;   // between them, as a share of the screen's width
constexpr int kMouseFinger = 1, kAltFinger = 2, kSwipeFinger = 3, kFirstKeyFinger = kSwipeFinger + kSwipeRow - 1;
int KeyFinger(int control, int finger) { return kFirstKeyFinger + control * kMaxFingers + finger; }

// A control with keys held. `keys` has bit i set for its key i, from the moment that key is pressed
// in Controls mode until it comes up, so a key that was already down doesn't count, even on a joystick
// that another key is holding.
struct HeldControl {
    int control;
    unsigned keys;
    ImVec2 at;             // where its first finger is, as shown; only a joystick's moves
    bool landed = false;   // that finger has had its first frame down
};

class App {
public:
    App() {
        settings_.Load();
        Reload();
    }

    ~App() {
        for (auto &[id, texture] : textures_)
            texture.Release();
    }

    void Frame();
    void RequestClose();
    void DropFiles(const std::vector<fs::path> &files);
    void StartSelected() {
        if (Device *device = Selected())
            StartDevice(*device);
    }
    bool Finished() const { return finished_; }
    bool Busy() const { return !sessions_.empty() || animating_; }   // something on screen keeps changing
    void ShowAddDevice(const std::string &productType = "") {   // the grid, or one device's versions
        page_ = Page::AddDevice;
        if (!productType.empty())
            ChooseProduct(productType);
    }
    bool Fullscreen() const { return fullscreen_; }    // the window should cover its monitor
    void EnterFullscreen() {                           // shows the selected device fullscreen, if it runs
        fullscreen_ = true;
        controlsShownAt_ = ImGui::GetTime();
    }
    void SetLandscape() {                              // turns the selected device (for tests)
        if (Device *device = Selected())
            orientations_[device->id] = Orientation::Landscape;
    }
    void ShowDevTools(bool show) { settings_.devTools = show; }   // for tests: not saved
    void ShowEditor() {                                // for tests: as open on the selected device
        if (Device *device = Selected()) {
            editing_ = true;
            heldDevice_ = device->id;
        }
    }

private:
    enum class Page { Device, AddDevice, Settings };

    void Reload();
    Device *Selected();
    Session *SessionFor(const Device &device);
    void StartDevice(const Device &device);
    void UpdateSessions();

    void Sidebar();
    void DevicePage(Device &device);
    void StoppedDeviceView(Device &device);
    void RunningDeviceView(Device &device, Session &session);
    void DeviceView(Device &device, Session *session, ImVec2 area);
    void DevToolsTab(ImVec2 at, float height);
    ImVec2 ScreenSize(Session &session, bool &turned);
    void DrawScreen(Session &session, ImVec2 origin, ImVec2 size, Orientation orientation);
    void ScreenTouch(Session &session, ImVec2 origin, ImVec2 size, Orientation orientation, bool turned);
    void KeyFingers(Session &session, const Layout &layout, float aspect, std::vector<Finger> &fingers);
    void SendFingers(Session &session, std::vector<Finger> fingers, Orientation orientation, bool turned);
    void WatchHeldInput(Session *shown);
    void ReleaseHeldInput();
    void FullscreenView(Session &session);
    void FullscreenControls(Session &session);
    void Controls(Session &session);
    void KeyboardBadge();
    void ToggleControlsMode();
    void ControlEditor(const Device &device, Layout &layout, ImVec2 origin, ImVec2 size);
    void ControlPopup(const Device &device, std::vector<Control> &controls);
    void AddControl(const Device &device, ControlType type);
    void SaveKeymap(const Device &device);
    void HardwareKeys(Session &session);
    void SendKeys(Session &session);
    void SaveScreenshot(Session &session);
    void ToggleRecording(Session &session);
    void Rotate(const Device &device);
    Orientation OrientationOf(const Device &device) {
        auto found = orientations_.find(device.id);
        return found != orientations_.end() ? found->second : Orientation::Portrait;
    }
    void SettingsPage();
    void AddDevicePage();
    void PreparationProgress();
    void DeviceGrid();
    void LightTouchImports();
    void VersionPicker();
    void ChooseProduct(const std::string &productType);
    void DeveloperPanel(Device &device, Session &session);
    void DevRequest(Session &session, Json command, std::function<void(const Json &)> onSuccess);
    void DevListFiles(const std::string &id, const std::string &path);
    void DevListApps(const std::string &id);
    void InstallDropped(Session &session, const fs::path &file, bool package);
    void ClosingOverlay();
    void ForceOffOverlay();

    Settings settings_;
    std::vector<Device> devices_;
    Page page_ = Page::Device;
    std::map<std::string, std::unique_ptr<Session>> sessions_;   // running devices by id
    std::map<std::string, Texture> textures_;
    std::string error_, notice_;
    bool closing_ = false, finished_ = false;
    std::string forceOffId_;   // the device whose "Force power off?" alert is showing, if any
    bool animating_ = false;                             // this frame shows motion, so the next must follow at once

    // Input state for the device on screen.
    bool touching_ = false, twoFingers_ = false, swipeRow_ = false;
    std::map<int, ImVec2> fingers_;                      // what iOS has down: finger id -> panel position
    std::vector<HeldControl> heldControls_;              // oldest press first
    std::set<int> heldButtons_;                          // held from the keyboard
    std::set<int> ignoredButtons_;                       // released for the user: their keys must come up first
    Motion motion_;
    std::map<std::string, Orientation> orientations_;    // turned devices, by id (each start is portrait)
    std::string typeText_, renameText_;

    // What the input above was held for (WatchHeldInput).
    std::string heldDevice_;
    KeyboardMode heldMode_ = KeyboardMode::Shortcuts;
    Orientation heldOrientation_ = Orientation::Portrait;
    bool heldEditing_ = false;

    // Key-to-touch mapping: each running device's keymap, and the editor's state.
    std::map<std::string, Keymap> keymaps_;              // by id, loaded when the device starts
    bool editing_ = false;                               // the editor is showing
    bool editorPopup_ = false;                           // its popup is open, so keys don't reach iOS
    bool openPopup_ = false;                             // open it on the next frame
    bool askingKey_ = false;                             // it's waiting for a key
    int keySlot_ = 0;                                    // ...for this one of the control's keys (Control::Key)
    std::string keyRefusal_;                             // why it turned down the last key pressed
    bool newControl_ = false;                            // just added, and goes again if it gets no key
    int chosenControl_ = -1;                             // the control it's for
    int resizingControl_ = -1;                           // the joystick whose rim is being dragged

    // Fullscreen: only the selected device's screen, with controls that show while the mouse moves.
    bool fullscreen_ = false;
    double controlsShownAt_ = 0;   // ImGui::GetTime() of the last mouse activity

    // The Add Device page: a grid of devices, then the chosen one's versions on a wheel.
    std::unique_ptr<Preparation> preparation_;
    bool choosingVersion_ = false;                       // past the grid
    Drum drum_;                                          // the version wheel
    std::string chosenProduct_ = "iPod3,1", chosenFirmware_ = "n18ap-8C148";
    std::string newName_;
    bool jailbreak_ = true;
    fs::path localIpsw_;

    // The developer panel's state, per device (kept while the app runs).
    struct DevPanel {
        int busy = 0;                    // requests waiting for an answer
        std::string message;             // the last error
        Json info;                       // lockdown's values
        std::string path = "/";          // the Files tab's folder
        Json files = Json::array();
        std::string target, newName;     // the item being renamed or deleted
        Json apps = Json::array();
        // Each tab loads itself when first shown, retrying every few seconds while iOS is still
        // connecting over USB. A tab is loaded once a request for it has succeeded.
        bool infoLoaded = false, filesLoaded = false, appsLoaded = false;
        double nextTry = 0;
        bool TimeToLoad(bool loaded) {
            if (loaded || busy > 0 || ImGui::GetTime() < nextTry)
                return false;
            nextTry = ImGui::GetTime() + 4;
            return true;
        }
        std::string installStatus;
        int drops = 0;                   // files dropped on the window, still installing
        std::string terminal, command;
        bool scrollTerminal = false;
        bool syslogOn = false;
        std::string filter;
        bool forwardOn = false;
        int port = 2222;
    };
    std::map<std::string, DevPanel> devPanels_;
};

void App::Reload() {
    devices_ = ListDevices();
    if (settings_.selected.empty() && !devices_.empty())
        settings_.selected = devices_[0].id;
}

Device *App::Selected() {
    for (auto &device : devices_)
        if (device.id == settings_.selected)
            return &device;
    return nullptr;
}

Session *App::SessionFor(const Device &device) {
    auto found = sessions_.find(device.id);
    return found != sessions_.end() ? found->second.get() : nullptr;
}

void App::StartDevice(const Device &device) {
    try {
        SessionOptions options;
        options.internet = settings_.internet;
        options.webProxy = settings_.webProxy;
        options.headphones = settings_.headphones;
        options.muted = settings_.muted;
        sessions_[device.id] = std::make_unique<Session>(device, options);
        keymaps_[device.id] = Keymap::Load(device.KeymapFile());
        error_.clear();
    } catch (const std::exception &e) {
        error_ = std::string("Couldn't start ") + device.name + ": " + e.what();
    }
}

void App::UpdateSessions() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Session &session = *it->second;
        session.Update();
        if (!session.recordingSaved.empty())
            notice_ = "Saved " + session.recordingSaved;
        if (!session.recordingError.empty())
            error_ = session.GetDevice().name + ": the recording failed. " + session.recordingError;
        session.recordingSaved.clear();
        session.recordingError.clear();
        if (devPanels_[it->first].drops > 0 && !session.progress.empty())
            notice_ = session.GetDevice().name + ": " + session.progress;
        if (session.Exited()) {
            if (!session.ExitedCleanly())
                error_ = session.GetDevice().name + ": " + session.ExitMessage();
            textures_[it->first].Release();
            textures_.erase(it->first);
            orientations_.erase(it->first);   // it starts upright next time
            keymaps_.erase(it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    if (closing_ && sessions_.empty())
        finished_ = true;
}

void App::RequestClose() {
    if (sessions_.empty()) {
        finished_ = true;
        return;
    }
    closing_ = true;   // shut every device down cleanly first
    for (auto &[id, session] : sessions_)
        session->Stop();
}

void App::Frame() {
    UpdateSessions();
    animating_ = false;

    // Fullscreen shows the device on screen: F11 toggles it, Esc leaves it, and it ends with the device.
    Device *shownDevice = page_ == Page::Device ? Selected() : nullptr;
    Session *shown = shownDevice ? SessionFor(*shownDevice) : nullptr;
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && !fullscreen_) {
        if (!editorPopup_)
            EnterFullscreen();
    } else if (ImGui::IsKeyPressed(ImGuiKey_F11, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        fullscreen_ = false;
    }
    if (!shown || closing_)
        fullscreen_ = false;
    if (fullscreen_)
        editing_ = false;   // its buttons are under the device

    // F2 turns Controls mode on and off while a device is on screen.
    if (shown && ImGui::IsKeyPressed(ImGuiKey_F2, false) && !ImGui::GetIO().WantTextInput && !editorPopup_)
        ToggleControlsMode();
    WatchHeldInput(shown);

    if (fullscreen_) {
        FullscreenView(*shown);
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    Sidebar();
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss"))
            error_.clear();
        ImGui::Separator();
    }
    if (!notice_.empty()) {
        ImGui::TextColored(kGood, "%s", notice_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("OK"))
            notice_.clear();
        ImGui::Separator();
    }
    if (page_ == Page::Settings) {
        SettingsPage();
    } else if (page_ == Page::AddDevice) {
        AddDevicePage();
    } else if (Device *device = Selected()) {
        DevicePage(*device);
    } else {
        ImGui::Dummy(ImVec2(0, 40));
        ImGui::TextUnformatted("No devices yet.");
        DimText("Add one: pick a device and an iOS version, and it's prepared from Apple's firmware for you.");
        if (ImGui::Button("Add a device"))
            page_ = Page::AddDevice;
    }
    ImGui::EndChild();
    if (closing_)
        ClosingOverlay();
    else if (!forceOffId_.empty())
        ForceOffOverlay();
    ImGui::End();
}

void App::Sidebar() {
    const float width = ImGui::GetFontSize() * 16;   // about 270 pixels at 100% scaling
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.10f, 0.12f, 1));
    ImGui::BeginChild("sidebar", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(kAccent, "iOS Emulator");
    ImGui::Spacing();
    DimText("DEVICES");
    for (const auto &device : devices_) {
        ImGui::PushID(device.id.c_str());
        const bool selected = page_ == Page::Device && device.id == settings_.selected;
        Session *session = SessionFor(device);
        const ImVec2 rowSize(0, ImGui::GetTextLineHeight() * 2 + 8);
        if (ImGui::Selectable("##row", selected, 0, rowSize)) {
            settings_.selected = device.id;
            page_ = Page::Device;
            settings_.Save();
        }
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddText(ImVec2(rowMin.x + 6, rowMin.y + 3), ImGui::GetColorU32(ImGuiCol_Text), device.name.c_str());
        std::string detail = device.Product() + "  iOS " + device.Version();
        draw->AddText(ImVec2(rowMin.x + 6, rowMin.y + 5 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(kDim),
                      detail.c_str());
        if (session) {   // a dot for running devices
            const float r = 4;
            draw->AddCircleFilled(ImVec2(ImGui::GetItemRectMax().x - 12, rowMin.y + 10 + r),
                                  r, ImGui::GetColorU32(session->state == "running" ? kGood : kWarn));
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("+  Add a device", ImVec2(-1, 0)))
        page_ = Page::AddDevice;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 8);
    if (ImGui::Button("Settings", ImVec2(-1, 0)))
        page_ = Page::Settings;
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::DevicePage(Device &device) {
    Session *session = SessionFor(device);
    ImGui::Text("%s", device.name.c_str());
    ImGui::SameLine();
    DimText(device.Product() + ", iOS " + device.Version());
    if (device.Jailbroken()) {
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "[jailbroken]");
        Tooltip("Prepared with Cydia, apt, bash and the AFC2 root file service.");
    }
    if (session) {
        ImGui::SameLine();
        const bool running = session->state == "running";
        ImGui::TextColored(running ? kGood : kWarn, "%s", session->state.c_str());
        if (session->recording) {
            const int seconds = int((NowNs() - session->recordingSince) / 1'000'000'000);
            ImGui::SameLine();
            ImGui::TextColored(kBad, "recording %d:%02d", seconds / 60, seconds % 60);
        }
    }
    ImGui::Separator();
    if (session)
        RunningDeviceView(device, *session);
    else
        StoppedDeviceView(device);
}

// A stopped device: its picture with the screen off (its Sleep/Wake button starts it), Start, and
// the Manage buttons.
void App::StoppedDeviceView(Device &device) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float below = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 2 +
                        style.ItemSpacing.y * 2 + (device.Dirty() ? ImGui::GetTextLineHeightWithSpacing() * 2 : 0);
    ImGui::Spacing();
    DeviceView(device, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - below));

    if (device.Dirty()) {
        const char *warning = "Last time this device stopped without shutting iOS down. iOS will check its storage "
                              "as it starts.";
        CenterRow(std::min(ImGui::CalcTextSize(warning).x, ImGui::GetContentRegionAvail().x));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kWarn, "%s", warning);
        ImGui::PopTextWrapPos();
    }
    const char *internet = settings_.internet ? "with internet" : "without internet (change it in Settings)";
    const float startWidth = ImGui::GetFontSize() * 9;
    CenterRow(startWidth);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.78f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.50f, 0.88f, 1));
    if (ImGui::Button("Start", ImVec2(startWidth, 0)))
        StartDevice(device);
    ImGui::PopStyleColor(2);
    Tooltip("Or press the device's Sleep/Wake button.");
    CenterRow(ImGui::CalcTextSize(internet).x);
    DimText(internet);

    ImGui::Spacing();
    const float manageWidth = ButtonWidth("Rename") + ButtonWidth("Erase...") + ButtonWidth("Delete...") +
                              ButtonWidth("Open folder") + style.ItemSpacing.x * 3;
    CenterRow(manageWidth);
    if (ImGui::Button("Rename")) {
        renameText_ = device.name;
        ImGui::OpenPopup("Rename");
    }
    ImGui::SameLine();
    if (ImGui::Button("Erase..."))
        ImGui::OpenPopup("Erase");
    Tooltip("Back to the freshly prepared system: deletes apps, settings and files added since.");
    ImGui::SameLine();
    if (ImGui::Button("Delete..."))
        ImGui::OpenPopup("Delete");
    ImGui::SameLine();
    if (ImGui::Button("Open folder"))
        ShellExecuteW(nullptr, L"open", device.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetKeyboardFocusHere();
        bool done = ImGui::InputText("Name", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || done) {
            RenameDevice(device, renameText_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Erase", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Erase %s? Everything added since it was prepared is removed.", device.name.c_str());
        if (ImGui::Button("Erase")) {
            EraseDevice(device);
            notice_ = device.name + " was erased.";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s and all of its data?", device.name.c_str());
        if (ImGui::Button("Delete")) {
            DeleteDevice(device);
            settings_.selected.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            Reload();   // `device` no longer exists after this
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// A running device: its picture with the live screen and the toolbar under it, and the developer
// tools beside it when they're shown (the tab at the device's right edge shows and hides them).
void App::RunningDeviceView(Device &device, Session &session) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float column = settings_.devTools ? std::max(avail * 0.46f, 380.0f) : avail;
    ImGui::BeginChild("device", ImVec2(column, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float tab = ImGui::GetFontSize() * 1.6f;
    const float controls = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y + 4;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(ImGui::GetContentRegionAvail().x - tab - 6, ImGui::GetContentRegionAvail().y - controls);
    DeviceView(device, &session, area);
    DevToolsTab(ImVec2(origin.x + area.x + 6, origin.y), area.y);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area.y + ImGui::GetStyle().ItemSpacing.y));
    Controls(session);
    ImGui::EndChild();
    if (settings_.devTools) {
        ImGui::SameLine();
        ImGui::BeginChild("developer", ImVec2(0, 0), ImGuiChildFlags_Borders);
        DeveloperPanel(device, session);
        ImGui::EndChild();
    }
    HardwareKeys(session);
}

// The developer tools' tab: a slim, tall button at `at`, centred in `height`.
void App::DevToolsTab(ImVec2 at, float height) {
    const float width = ImGui::GetFontSize() * 1.6f, tall = std::min(height, ImGui::GetFontSize() * 7);
    const ImVec2 min(at.x, at.y + (height - tall) / 2), max(min.x + width, min.y + tall);
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("devtools", ImVec2(width, tall))) {
        settings_.devTools = !settings_.devTools;
        settings_.Save();
    }
    Tooltip(settings_.devTools ? "Hide the developer tools" : "Show the developer tools: device info, files, apps, "
                                                               "terminal, logs and SSH");
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered
                                                                                               : ImGuiCol_Button),
                        width / 2);
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const float cx = (min.x + max.x) / 2, cy = (min.y + max.y) / 2;
    if (g_icons) {   // the code icon, with an arrow pointing where the panel goes
        const char *arrow = settings_.devTools ? icon::kRight : icon::kLeft;
        const ImVec2 a = ImGui::CalcTextSize(icon::kCode), b = ImGui::CalcTextSize(arrow);
        draw->AddText(ImVec2(cx - a.x / 2, cy - a.y - 4), ink, icon::kCode);
        draw->AddText(ImVec2(cx - b.x / 2, cy + 4), ink, arrow);
    } else {         // a drawn chevron
        const float s = width * 0.18f, d = settings_.devTools ? s : -s;
        const ImVec2 chevron[] = {ImVec2(cx - d / 2, cy - s), ImVec2(cx + d / 2, cy), ImVec2(cx - d / 2, cy + s)};
        draw->AddPolyline(chevron, 3, ink, 0, 2);
    }
}

// Fits a rectangle of `aspect` (width over height) into the area at `origin`, centred.
void FitRect(ImVec2 origin, ImVec2 area, float aspect, ImVec2 &min, ImVec2 &max) {
    const float width = std::min(area.x, area.y * aspect), height = width / aspect;
    min = ImVec2(std::floor(origin.x + (area.x - width) / 2), std::floor(origin.y + (area.y - height) / 2));
    max = ImVec2(min.x + width, min.y + height);
}

// The device as it looks, fitted into `area` at the cursor: its picture (skin.h), the screen (the
// running iOS, or the glass of a stopped device), and its buttons, which work like the real ones:
// held while the mouse button is. A stopped device's Sleep/Wake button starts it.
void App::DeviceView(Device &device, Session *session, ImVec2 area) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    area = ImVec2(std::max(area.x, 40.0f), std::max(area.y, 40.0f));
    const Orientation orientation = session ? OrientationOf(device) : Orientation::Portrait;
    bool turned = false;
    const ImVec2 upright = session ? ScreenSize(*session, turned) : ImVec2(2, 3);
    Skin *skin = Skin::For(device.board);
    SkinLayout layout;
    if (skin) {
        layout = skin->Fit(origin, area, orientation);
    } else {   // no art: just the screen
        const float aspect = orientation == Orientation::Landscape ? upright.y / upright.x : upright.x / upright.y;
        FitRect(origin, area, aspect, layout.screenMin, layout.screenMax);
    }

    // The buttons first (they draw nothing themselves), so the picture can show them pressed.
    int hovered = -1;
    std::set<int> down = heldButtons_;
    const bool canPress = !session || session->state != "stopping";
    for (const SkinLayout::Button &button : layout.buttons) {
        if (!canPress || (!session && button.button != QEMU_IOS_BUTTON_POWER))
            continue;
        ImGui::PushID(button.button);
        ImGui::SetCursorScreenPos(button.hitMin);
        ImGui::InvisibleButton("button", ImVec2(button.hitMax.x - button.hitMin.x, button.hitMax.y - button.hitMin.y));
        if (ImGui::IsItemHovered()) {
            hovered = button.button;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemActive())
            down.insert(button.button);
        const char *name = button.button == QEMU_IOS_BUTTON_HOME     ? "Home"
                         : button.button == QEMU_IOS_BUTTON_POWER    ? "Sleep/Wake"
                         : button.button == QEMU_IOS_BUTTON_VOLUME_UP ? "Volume up"
                                                                       : "Volume down";
        const char *key = button.button == QEMU_IOS_BUTTON_HOME      ? "H"
                        : button.button == QEMU_IOS_BUTTON_POWER     ? "P"
                        : button.button == QEMU_IOS_BUTTON_VOLUME_UP ? "+"
                                                                       : "-";
        if (session) {
            if (ImGui::IsItemActivated())
                session->Send({{"cmd", "button"}, {"button", button.button}, {"down", true}});
            if (ImGui::IsItemDeactivated())
                session->Send({{"cmd", "button"}, {"button", button.button}, {"down", false}});
            std::string tip = std::string(name) + " (" + settings_.Shortcut(key) + ")";
            if (button.button == QEMU_IOS_BUTTON_POWER)
                tip += ". Hold it to power off.";
            Tooltip(tip.c_str());
        } else {
            if (ImGui::IsItemClicked())
                StartDevice(device);
            Tooltip("Sleep/Wake: starts the device");
        }
        ImGui::PopID();
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (skin)
        skin->Draw(draw, layout, [&](int button) { return down.count(button) > 0; }, hovered);
    const ImVec2 screenSize(layout.screenMax.x - layout.screenMin.x, layout.screenMax.y - layout.screenMin.y);
    if (session)
        DrawScreen(*session, layout.screenMin, screenSize, orientation);
    else if (!skin)
        draw->AddRectFilled(layout.screenMin, layout.screenMax, IM_COL32(6, 6, 8, 255));
    if (skin)
        skin->DrawGlare(draw, layout);

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(area);
}

// Uploads the device's newest frame, and returns the screen's size upright, in pixels. A panel wider
// than tall (the iPad's) scans a quarter-turn from upright, so it's shown `turned`.
ImVec2 App::ScreenSize(Session &session, bool &turned) {
    Texture &texture = textures_[session.GetDevice().id];
    if (session.UpdateFrame())
        texture.Upload(session.FramePixels(), session.FrameWidth(), session.FrameHeight(), settings_.smooth);
    const float width = float(texture.width ? texture.width : session.screenWidth);
    const float height = float(texture.height ? texture.height : session.screenHeight);
    turned = width > height;
    return turned ? ImVec2(height, width) : ImVec2(width, height);
}

// Draws the screen into the rectangle at `screen` (as shown, turned to `orientation`) and makes it
// touchable.
void App::DrawScreen(Session &session, ImVec2 screen, ImVec2 screenSize, Orientation orientation) {
    bool turned;
    ScreenSize(session, turned);
    const Texture &texture = textures_[session.GetDevice().id];
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 screenEnd(screen.x + screenSize.x, screen.y + screenSize.y);
    if (texture.id && !session.sleeping) {   // each corner shows the pixel the turns put there
        const ImVec2 corner[4] = {screen, ImVec2(screenEnd.x, screen.y), screenEnd, ImVec2(screen.x, screenEnd.y)};
        const ImVec2 shown[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
        ImVec2 uv[4];
        for (int i = 0; i < 4; i++)
            uv[i] = ToPanel(shown[i], orientation, turned);
        draw->AddImageQuad(ImTextureID(intptr_t(texture.id)), corner[0], corner[1], corner[2], corner[3], uv[0], uv[1],
                           uv[2], uv[3]);
    } else {
        draw->AddRectFilled(screen, screenEnd, IM_COL32(0, 0, 0, 255));
        const char *text = !texture.id ? "Starting iOS..." : "Display off: click to wake";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(screen.x + (screenSize.x - size.x) / 2, screen.y + (screenSize.y - size.y) / 2),
                      IM_COL32(200, 200, 200, 255), text);
    }
    ScreenTouch(session, screen, screenSize, orientation, turned);
}

float ControlRadius() { return ImGui::GetFontSize() * 1.1f; }

// A control's fingers in window pixels, for the screen at `origin`, `size` big.
std::vector<ImVec2> ControlSpots(const Control &control, ImVec2 origin, ImVec2 size) {
    std::vector<ImVec2> spots = control.spots;
    for (ImVec2 &spot : spots)
        spot = ImVec2(origin.x + spot.x * size.x, origin.y + spot.y * size.y);
    return spots;
}

std::string KeyLabel(ImGuiKey key) { return key == ImGuiKey_None ? "?" : ImGui::GetKeyName(key); }

// A control's label: its key's name, with the finger count on a multi-finger tap.
std::string ControlLabel(const Control &control) {
    std::string label = KeyLabel(control.key);
    if (control.type == ControlType::MultiTap)
        label += " x" + std::to_string(control.spots.size());
    return label;
}

// The centre of a control's label, in window pixels. A tap's is in its circle. A multi-finger tap's
// sits above its fingers, clear of them (below them at the top of the screen), because the editor
// drags them all by it. `top` is the screen's top edge.
ImVec2 LabelCentre(const Control &control, const std::vector<ImVec2> &spots, float top) {
    if (control.type == ControlType::Tap)
        return spots.front();
    ImVec2 low = spots.front(), high = spots.front();
    for (const ImVec2 &spot : spots) {
        low = ImVec2(std::min(low.x, spot.x), std::min(low.y, spot.y));
        high = ImVec2(std::max(high.x, spot.x), std::max(high.y, spot.y));
    }
    const float clear = ControlRadius() + ImGui::GetFontSize() / 2 + 4;
    const float above = low.y - clear;
    return ImVec2((low.x + high.x) / 2, above - ImGui::GetFontSize() / 2 >= top ? above : high.y + clear);
}

// A label centred on `centre`, over a dark patch that keeps it readable on any screen.
void DrawLabel(ImDrawList *draw, const std::string &label, ImVec2 centre, ImU32 shade, float opacity) {
    const ImVec2 text = ImGui::CalcTextSize(label.c_str());
    const ImVec2 at(centre.x - text.x / 2, centre.y - text.y / 2);
    draw->AddRectFilled(ImVec2(at.x - 3, at.y), ImVec2(at.x + text.x + 3, at.y + text.y), shade, 3);
    draw->AddText(at, ImGui::GetColorU32(ImVec4(1, 1, 1, opacity)), label.c_str());
}

// Draws a control over the screen. `opacity` makes it faint while playing and solid in the editor.
// A tap is a ring for each finger and its label. A joystick is a ring of its radius (`width` is the
// screen's, in pixels), a dot for its finger at `dot`, and each direction's key outside the ring on
// that side.
void DrawControl(ImDrawList *draw, const Control &control, const std::vector<ImVec2> &spots, float top, float width,
                 ImVec2 dot, ImVec4 ink, float opacity) {
    const ImU32 ring = ImGui::GetColorU32(ImVec4(ink.x, ink.y, ink.z, opacity));
    const ImU32 shade = IM_COL32(0, 0, 0, int(120 * opacity));
    if (control.type == ControlType::Joystick) {
        const ImVec2 centre = spots.front();
        const float radius = control.radius * width;
        draw->AddCircleFilled(centre, radius, shade);
        draw->AddCircle(centre, radius, ring, 0, 2);
        draw->AddCircleFilled(dot, ControlRadius() / 2, ring);
        const ImVec2 sides[kJoystickKeys] = {ImVec2(0, -1), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(1, 0)};   // as kDirections
        for (int i = 0; i < kJoystickKeys; i++) {
            const std::string label = KeyLabel(control.directions[i]);
            const ImVec2 text = ImGui::CalcTextSize(label.c_str());
            const float clear = radius + (sides[i].x != 0 ? text.x : text.y) / 2 + 5;
            DrawLabel(draw, label, ImVec2(centre.x + sides[i].x * clear, centre.y + sides[i].y * clear), shade, opacity);
        }
        return;
    }
    for (const ImVec2 &spot : spots) {
        draw->AddCircleFilled(spot, ControlRadius(), shade);
        draw->AddCircle(spot, ControlRadius(), ring, 0, 2);
    }
    DrawLabel(draw, ControlLabel(control), LabelCentre(control, spots, top), shade, opacity);
}

// Everything that touches the screen, gathered into one list each frame. The mouse is a finger:
// press, drag and release on the screen. On boards with multi-touch, a key held as the drag starts
// sets its gesture until it ends: Alt adds a second, mirrored finger (pinch and rotate), and Shift
// turns the mouse into a row of fingers that move with it (multi-finger swipes). Alt wins if both
// are held. In Controls mode, the keys bound to controls are fingers too. The editor's circles come
// first, which gives them the mouse before the screen.
void App::ScreenTouch(Session &session, ImVec2 origin, ImVec2 size, Orientation orientation, bool turned) {
    const Device &device = session.GetDevice();
    Layout &layout = keymaps_[device.id].For(orientation);
    if (editing_) {
        ControlEditor(device, layout, origin, size);
    } else if (settings_.keyboard == KeyboardMode::Controls && settings_.showControls && !session.sleeping) {
        for (int index = 0; index < int(layout.controls.size()); index++) {
            const Control &control = layout.controls[index];
            const std::vector<ImVec2> spots = ControlSpots(control, origin, size);
            ImVec2 dot = spots.front();
            for (const HeldControl &held : heldControls_)
                if (held.control == index)
                    dot = ImVec2(origin.x + held.at.x * size.x, origin.y + held.at.y * size.y);
            DrawControl(ImGui::GetWindowDrawList(), control, spots, origin.y, size.x, dot, ImVec4(1, 1, 1, 1), 0.4f);
        }
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("screen", size);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float x = std::clamp((mouse.x - origin.x) / size.x, 0.0f, 1.0f);
    const float y = std::clamp((mouse.y - origin.y) / size.y, 0.0f, 1.0f);
    // While iOS has the display off, its touch panel is off too: a click wakes it with Home instead.
    if (ImGui::IsItemActivated() && session.sleeping) {
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", true}});
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", false}});
    } else if (ImGui::IsItemActivated() && !session.paused) {
        touching_ = true;
        const bool multiTouch = session.capabilities & QEMU_IOS_CAP_MULTITOUCH;
        twoFingers_ = ImGui::GetIO().KeyAlt && multiTouch;
        swipeRow_ = ImGui::GetIO().KeyShift && multiTouch && !twoFingers_;
    }
    if (!ImGui::IsItemActive() || session.paused)
        touching_ = false;

    std::vector<Finger> fingers;
    auto awayFromCursor = [&](int id, ImVec2 at) {   // a finger the cursor doesn't show, so it gets a circle
        fingers.push_back({id, at});
        ImGui::GetWindowDrawList()->AddCircle(ImVec2(origin.x + at.x * size.x, origin.y + at.y * size.y), 10,
                                              IM_COL32(255, 255, 255, 160), 0, 2);
    };
    if (touching_ && swipeRow_) {
        // The whole row shifts in from the edges, so it keeps its spacing. It's centred on the cursor,
        // which leaves every finger, the mouse's too, beside it.
        const float half = (kSwipeRow - 1) * kSwipeGap / 2;
        const float left = std::clamp(x, half, 1 - half) - half;
        awayFromCursor(kMouseFinger, ImVec2(left, y));
        for (int i = 1; i < kSwipeRow; i++)
            awayFromCursor(kSwipeFinger + i - 1, ImVec2(left + i * kSwipeGap, y));
    } else if (touching_) {
        fingers.push_back({kMouseFinger, ImVec2(x, y)});
        if (twoFingers_)
            awayFromCursor(kAltFinger, ImVec2(1 - x, 1 - y));
    }
    KeyFingers(session, layout, size.x / size.y, fingers);
    SendFingers(session, fingers, orientation, turned);
}

// Adds the fingers of the keys held down to `fingers`, oldest press first. A key counts from the
// moment it's pressed in Controls mode: one held through a rotation, say, must be pressed again. A
// joystick's finger lands at the centre and steers from the next frame on. `aspect` is the screen's
// width over its height.
void App::KeyFingers(Session &session, const Layout &layout, float aspect, std::vector<Finger> &fingers) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool live = settings_.keyboard == KeyboardMode::Controls && !editing_ && !io.WantTextInput &&
                      session.state == "running" && !session.paused && !session.sleeping && !session.Stopping();
    if (!live) {
        heldControls_.clear();
        return;
    }
    const std::vector<Control> &controls = layout.controls;
    for (HeldControl &held : heldControls_) {
        const Control &control = controls[held.control];
        for (int k = 0; k < control.KeyCount(); k++)
            if ((held.keys >> k & 1) && !ImGui::IsKeyDown(control.Key(k)))
                held.keys &= ~(1u << k);
    }
    heldControls_.erase(std::remove_if(heldControls_.begin(), heldControls_.end(),
                                       [](const HeldControl &held) { return held.keys == 0; }),
                        heldControls_.end());
    for (int index = 0; index < int(controls.size()); index++) {
        const Control &control = controls[index];
        for (int k = 0; k < control.KeyCount(); k++) {
            const ImGuiKey key = control.Key(k);
            const bool ctrl = key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl;
            if (key == ImGuiKey_None || !ImGui::IsKeyPressed(key, false) || (io.KeyCtrl && !ctrl))   // Ctrl+key is a shortcut
                continue;
            auto held = std::find_if(heldControls_.begin(), heldControls_.end(),
                                     [&](const HeldControl &h) { return h.control == index; });
            if (held == heldControls_.end())
                held = heldControls_.insert(held, {index, 0, control.spots.front()});
            held->keys |= 1u << k;
        }
    }
    for (HeldControl &held : heldControls_) {
        if (held.landed && controls[held.control].type == ControlType::Joystick)
            held.at = JoystickStep(controls[held.control], held.keys, held.at, aspect);
        held.landed = true;
    }

    if (session.capabilities & QEMU_IOS_CAP_MULTITOUCH) {
        for (const HeldControl &held : heldControls_) {
            const std::vector<ImVec2> &spots = controls[held.control].spots;
            fingers.push_back({KeyFinger(held.control, 0), held.at});
            for (int finger = 1; finger < int(spots.size()); finger++)
                fingers.push_back({KeyFinger(held.control, finger), spots[finger]});
        }
    } else if (fingers.empty() && !heldControls_.empty()) {   // one finger only: the first of the key pressed last
        const HeldControl &held = heldControls_.back();
        fingers.push_back({KeyFinger(held.control, 0), held.at});
    }
}

// Makes iOS's fingers match `fingers`: one touch command with what changed, or none. The engine
// takes a command whole or not at all, so what it would refuse mustn't get in (worker.h).
void App::SendFingers(Session &session, std::vector<Finger> fingers, Orientation orientation, bool turned) {
    // Fingers already down keep their place, and new ones get what's left of the panel's limit.
    std::stable_partition(fingers.begin(), fingers.end(), [&](const Finger &f) { return fingers_.count(f.id) > 0; });
    if (int(fingers.size()) > session.maxContacts)
        fingers.resize(session.maxContacts);

    Json contacts = Json::array();
    auto add = [&](int id, int phase, ImVec2 panel) {
        contacts.push_back(Json{{"id", id}, {"phase", phase}, {"x", panel.x}, {"y", panel.y}});
    };
    for (auto down = fingers_.begin(); down != fingers_.end();) {   // lifted first: it frees slots for new fingers
        const int id = down->first;
        if (std::any_of(fingers.begin(), fingers.end(), [&](const Finger &f) { return f.id == id; })) {
            ++down;
        } else {
            add(id, QEMU_IOS_TOUCH_END, down->second);
            down = fingers_.erase(down);
        }
    }
    for (const Finger &finger : fingers) {
        if (contacts.size() == QEMU_IOS_PORTABLE_MAX_CONTACTS)
            break;   // the rest follow next frame
        // The engine wants the panel's own coordinates, before the device's turn and the panel's.
        const ImVec2 panel = ToPanel(finger.at, orientation, turned);
        const auto down = fingers_.find(finger.id);
        if (down == fingers_.end())
            add(finger.id, QEMU_IOS_TOUCH_BEGIN, panel);
        else if (down->second.x != panel.x || down->second.y != panel.y)
            add(finger.id, QEMU_IOS_TOUCH_UPDATE, panel);
        fingers_[finger.id] = panel;
    }
    if (!contacts.empty())
        session.Send({{"cmd", "touch"}, {"contacts", contacts}});
}

// Runs at the start of each frame. What's held down makes sense only for one device, keyboard mode
// and orientation, and only while the window has the keyboard and the device runs. When any of that
// changes, everything lifts, so nothing stays stuck down.
void App::WatchHeldInput(Session *shown) {
    const std::string device = shown ? shown->GetDevice().id : "";
    const Orientation orientation = shown ? OrientationOf(shown->GetDevice()) : Orientation::Portrait;
    if (device != heldDevice_)
        editing_ = false;
    if (!editing_) {
        editorPopup_ = false;
        resizingControl_ = -1;
    }
    if (device != heldDevice_ || settings_.keyboard != heldMode_ || orientation != heldOrientation_ ||
        editing_ != heldEditing_ || ImGui::GetIO().AppFocusLost || (shown && shown->Stopping()))
        ReleaseHeldInput();
    heldDevice_ = device;
    heldMode_ = settings_.keyboard;
    heldOrientation_ = orientation;
    heldEditing_ = editing_;
}

// Lifts every finger and releases every hardware button held from the keyboard. Their keys may still
// be down, so a button is ignored until its key comes up (a Home that pressed itself again would be
// a double click).
void App::ReleaseHeldInput() {
    const auto found = sessions_.find(heldDevice_);
    if (found != sessions_.end()) {
        SendFingers(*found->second, {}, Orientation::Portrait, false);
        for (int button : heldButtons_)
            found->second->Send({{"cmd", "button"}, {"button", button}, {"down", false}});
    }
    fingers_.clear();
    heldControls_.clear();
    touching_ = false;
    ignoredButtons_.insert(heldButtons_.begin(), heldButtons_.end());
    heldButtons_.clear();
}

// ---- Fullscreen ------------------------------------------------------------------------------

// The icons of the fullscreen controls, drawn in `ink` inside a round button of radius r at c.

// Volume: a speaker with a plus or a minus beside it.
void DrawVolumeIcon(ImDrawList *draw, ImVec2 c, float r, ImU32 ink, bool up) {
    const float width = r * 0.11f;
    draw->AddRectFilled(ImVec2(c.x - r * 0.52f, c.y - r * 0.15f), ImVec2(c.x - r * 0.32f, c.y + r * 0.15f), ink);
    draw->AddQuadFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.15f), ImVec2(c.x - r * 0.06f, c.y - r * 0.40f),
                        ImVec2(c.x - r * 0.06f, c.y + r * 0.40f), ImVec2(c.x - r * 0.32f, c.y + r * 0.15f), ink);
    const ImVec2 sign(c.x + r * 0.32f, c.y);
    draw->AddLine(ImVec2(sign.x - r * 0.17f, sign.y), ImVec2(sign.x + r * 0.17f, sign.y), ink, width);
    if (up)
        draw->AddLine(ImVec2(sign.x, sign.y - r * 0.17f), ImVec2(sign.x, sign.y + r * 0.17f), ink, width);
}

// Home, as on the device: a ring with a rounded square inside.
void DrawHomeIcon(ImDrawList *draw, ImVec2 c, float r, ImU32 ink) {
    const float width = r * 0.08f, half = r * 0.24f;
    draw->AddCircle(c, r * 0.62f, ink, 0, width);
    draw->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), ink, half * 0.4f, 0, width);
}

// Sleep/Wake: the power symbol, a circle open at the top with a line through the gap.
void DrawPowerIcon(ImDrawList *draw, ImVec2 c, float r, ImU32 ink) {
    const float width = r * 0.11f, radius = r * 0.40f, pi = 3.14159265f, gap = 0.75f;
    draw->PathArcTo(c, radius, -pi / 2 + gap, 3 * pi / 2 - gap, 32);
    draw->PathStroke(ink, 0, width);
    draw->AddLine(ImVec2(c.x, c.y - radius * 1.25f), ImVec2(c.x, c.y - radius * 0.2f), ink, width);
}

// Rotate: an arrow going three quarters of the way round.
void DrawRotateIcon(ImDrawList *draw, ImVec2 c, float r, ImU32 ink) {
    const float width = r * 0.10f, radius = r * 0.38f, pi = 3.14159265f;
    const float from = -pi * 0.35f, to = pi * 1.15f;
    draw->PathArcTo(c, radius, from, to, 32);
    draw->PathStroke(ink, 0, width);
    // the arrowhead at the arc's end, pointing along it
    const ImVec2 tip(c.x + std::cos(to) * radius, c.y + std::sin(to) * radius);
    const ImVec2 along(-std::sin(to), std::cos(to)), out(std::cos(to), std::sin(to));
    const float head = r * 0.2f;
    draw->AddTriangleFilled(ImVec2(tip.x + along.x * head, tip.y + along.y * head),
                            ImVec2(tip.x + out.x * head * 0.8f, tip.y + out.y * head * 0.8f),
                            ImVec2(tip.x - out.x * head * 0.8f, tip.y - out.y * head * 0.8f), ink);
}

// Leave fullscreen: four corners pointing inward.
void DrawExitFullscreenIcon(ImDrawList *draw, ImVec2 c, float r, ImU32 ink) {
    const float width = r * 0.09f, outer = r * 0.42f, inner = r * 0.14f;
    for (float sx : {-1.0f, 1.0f})
        for (float sy : {-1.0f, 1.0f}) {
            const ImVec2 corner[] = {ImVec2(c.x + sx * outer, c.y + sy * inner), ImVec2(c.x + sx * inner, c.y + sy * inner),
                                     ImVec2(c.x + sx * inner, c.y + sy * outer)};
            draw->AddPolyline(corner, 3, ink, 0, width);
        }
}

// Fullscreen: the screen as large as the monitor allows, on black, with the controls over it.
void App::FullscreenView(Session &session) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::Begin("fullscreen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    bool turned;
    const Orientation orientation = OrientationOf(session.GetDevice());
    ImVec2 shown = ScreenSize(session, turned);
    if (orientation == Orientation::Landscape)
        shown = ImVec2(shown.y, shown.x);
    const float scale = std::min(viewport->Size.x / shown.x, viewport->Size.y / shown.y);
    const ImVec2 size(shown.x * scale, shown.y * scale);
    DrawScreen(session,
               ImVec2(viewport->Pos.x + (viewport->Size.x - size.x) / 2, viewport->Pos.y + (viewport->Size.y - size.y) / 2),
               size, orientation);
    ImGui::End();
    FullscreenControls(session);   // a window of its own, so it sits over the screen
    HardwareKeys(session);
}

// The fullscreen controls along the bottom: Volume down and up, Home, Sleep/Wake, Rotate and Leave
// fullscreen. They show when the mouse moves and fade out a few seconds after it stops, unless
// it's over them.
void App::FullscreenControls(Session &session) {
    const ImGuiIO &io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsAnyMouseDown())
        controlsShownAt_ = now;
    const float alpha = std::clamp(float(controlsShownAt_ + 2.5 - now) / 0.3f, 0.0f, 1.0f);   // fades in its last 0.3 s
    if (alpha <= 0)
        return;

    const float button = ImGui::GetFontSize() * 2.8f, gap = button * 0.3f, pad = button * 0.2f;
    const ImVec2 size(6 * button + 6 * gap + 2 * pad, button + 2 * pad);   // 6 buttons, the last set apart
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 at(viewport->Pos.x + (viewport->Size.x - size.x) / 2,
                    viewport->Pos.y + viewport->Size.y - size.y - button * 0.4f);
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
    ImGui::Begin("fullscreen controls", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    auto gray = [&](int value, int opacity) { return IM_COL32(value, value, value, int(opacity * alpha)); };
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), gray(18, 215), size.y / 2);

    // A round button with an icon; returns whether it was clicked.
    auto roundButton = [&](const char *id, const std::string &tip, const std::function<void(ImVec2, float, ImU32)> &icon) {
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(button, button));
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 c(min.x + button / 2, min.y + button / 2);
        draw->AddCircleFilled(c, button / 2, gray(255, ImGui::IsItemActive() ? 90 : ImGui::IsItemHovered() ? 55 : 28));
        icon(c, button / 2, gray(240, 255));
        Tooltip(tip.c_str());
        return clicked;
    };
    // A device button: held down while the mouse button is, like the real one.
    auto deviceButton = [&](const char *id, int which, const std::string &tip, const std::function<void(ImVec2, float, ImU32)> &icon) {
        roundButton(id, tip, icon);
        if (ImGui::IsItemActivated())
            session.Send({{"cmd", "button"}, {"button", which}, {"down", true}});
        if (ImGui::IsItemDeactivated())
            session.Send({{"cmd", "button"}, {"button", which}, {"down", false}});
        ImGui::SameLine();
    };
    deviceButton("voldown", QEMU_IOS_BUTTON_VOLUME_DOWN, "Volume down (" + settings_.Shortcut("-") + ")",
                 [&](ImVec2 c, float r, ImU32 ink) { DrawVolumeIcon(draw, c, r, ink, false); });
    deviceButton("volup", QEMU_IOS_BUTTON_VOLUME_UP, "Volume up (" + settings_.Shortcut("+") + ")",
                 [&](ImVec2 c, float r, ImU32 ink) { DrawVolumeIcon(draw, c, r, ink, true); });
    deviceButton("home", QEMU_IOS_BUTTON_HOME, "Home (" + settings_.Shortcut("H") + ")",
                 [&](ImVec2 c, float r, ImU32 ink) { DrawHomeIcon(draw, c, r, ink); });
    deviceButton("power", QEMU_IOS_BUTTON_POWER, "Sleep/Wake (" + settings_.Shortcut("P") + "). Hold it to power off.",
                 [&](ImVec2 c, float r, ImU32 ink) { DrawPowerIcon(draw, c, r, ink); });
    if (roundButton("rotate", "Rotate (" + settings_.Shortcut("R") + ")",
                    [&](ImVec2 c, float r, ImU32 ink) { DrawRotateIcon(draw, c, r, ink); }))
        Rotate(session.GetDevice());
    ImGui::SameLine();
    // A thin line, then Leave fullscreen.
    const float lineX = ImGui::GetCursorScreenPos().x;   // halfway between Sleep/Wake and Leave
    draw->AddLine(ImVec2(lineX, at.y + pad + button * 0.2f), ImVec2(lineX, at.y + pad + button * 0.8f), gray(255, 60),
                  1.5f);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + gap, ImGui::GetCursorScreenPos().y));
    if (roundButton("leave", "Leave fullscreen (F11 or Esc)",
                    [&](ImVec2 c, float r, ImU32 ink) { DrawExitFullscreenIcon(draw, c, r, ink); }))
        fullscreen_ = false;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        controlsShownAt_ = now;   // stays while the mouse rests on it
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---- Keyboard --------------------------------------------------------------------------------

// The hardware shortcuts: H Home, P Sleep/Wake, + and - volume (held while the key is), R rotates,
// arrows tilt. In Typing and Controls modes the keys do something else, so these take Ctrl.
void App::HardwareKeys(Session &session) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool typing = io.WantTextInput || editorPopup_ || session.Stopping();
    const bool modifiers = io.KeyCtrl == settings_.ShortcutsTakeCtrl() && !io.KeyAlt && !io.KeyShift;
    auto key = [&](ImGuiKey k) { return !typing && modifiers && ImGui::IsKeyDown(k); };
    const std::pair<bool, int> buttons[] = {
        {key(ImGuiKey_H), QEMU_IOS_BUTTON_HOME},
        {key(ImGuiKey_P), QEMU_IOS_BUTTON_POWER},
        {key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd), QEMU_IOS_BUTTON_VOLUME_UP},
        {key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract), QEMU_IOS_BUTTON_VOLUME_DOWN},
    };
    for (const auto &[down, button] : buttons) {
        if (!down)
            ignoredButtons_.erase(button);
        if (down == bool(heldButtons_.count(button)) || ignoredButtons_.count(button))
            continue;
        if (down)
            heldButtons_.insert(button);
        else
            heldButtons_.erase(button);
        session.Send({{"cmd", "button"}, {"button", button}, {"down", down}});
    }
    if (!typing && modifiers && ImGui::IsKeyPressed(ImGuiKey_R, false))
        Rotate(session.GetDevice());
    const bool tilt = settings_.tilt;
    double a[3];
    if (motion_.Step(OrientationOf(session.GetDevice()), tilt && key(ImGuiKey_LeftArrow), tilt && key(ImGuiKey_RightArrow),
                     tilt && key(ImGuiKey_UpArrow), tilt && key(ImGuiKey_DownArrow), a))
        session.Send({{"cmd", "motion"}, {"x", a[0]}, {"y", a[1]}, {"z", a[2]}});
    SendKeys(session);
}

// Turns the device between portrait and landscape: the picture turns, and the accelerometer tells
// iOS (Motion), which turns the apps that support it.
void App::Rotate(const Device &device) {
    orientations_[device.id] =
        OrientationOf(device) == Orientation::Portrait ? Orientation::Landscape : Orientation::Portrait;
}

// In Typing mode, what you type goes to iOS while no text box here has focus: the characters as
// typed (Shift and AltGr included), Backspace and Enter. The worker types them into the text field
// that has focus in iOS (worker.h, "type").
void App::SendKeys(Session &session) {
    const ImGuiIO &io = ImGui::GetIO();
    if (settings_.keyboard != KeyboardMode::Typing || io.WantTextInput || editorPopup_)
        return;
    std::wstring text;
    for (ImWchar c : io.InputQueueCharacters)
        if (c >= 0x20 && c != 0x7f)   // not control characters (Ctrl+H and the like come as these)
            text += wchar_t(c);
    if (!io.KeyCtrl && !io.KeyAlt) {   // Ctrl+arrows tilt; Alt is for the second finger
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace))   // repeats while held, as typing does
            text += L'\b';
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            text += L'\n';
    }
    if (!text.empty()) {
        session.typingProblem.clear();
        session.Send({{"cmd", "type"}, {"text", Narrow(text)}});
    }
}

// The toolbar under the device: Rotate, Pause, Screenshot, Record, Fullscreen | Sound, Internet, Web
// proxy | Stop. Under it is the keyboard's row: its mode, Edit controls, and the box for typing into
// iOS (or, in the editor, the buttons that add controls). The hardware buttons are on the device itself.
void App::Controls(Session &session) {
    const bool ready = session.state == "running" || session.state == "paused";
    const ImGuiStyle &style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x * 2;   // between the groups
    const char *mark = "(!)";
    const char *names[] = {"Rotate", session.paused ? "Resume" : "Pause", "Screenshot", "Record", "Fullscreen", "Sound",
                           "Internet", "Web proxy", "Stop"};
    float width = gap * 2 + style.ItemSpacing.x * 6;
    for (const char *name : names)
        width += IconButtonWidth(name);
    if (session.proxyError)
        width += ImGui::CalcTextSize(mark).x + style.ItemSpacing.x;
    CenterRow(width);

    const bool accelerometer = session.capabilities & QEMU_IOS_CAP_ACCEL;
    const bool landscape = OrientationOf(session.GetDevice()) == Orientation::Landscape;
    if (IconButton(icon::kRotate, "Rotate",
                   std::string(landscape ? "Back to portrait" : "Turn to landscape") + " (" + settings_.Shortcut("R") + ")" +
                       (accelerometer ? "" : ". This device has no accelerometer, so iOS itself doesn't turn."),
                   landscape))
        Rotate(session.GetDevice());
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    if (IconButton(session.paused ? icon::kPlay : icon::kPause, names[1], session.paused ? "Resume" : "Pause",
                   session.paused)) {
        session.paused = !session.paused;   // at once: no finger may start once the engine has stopped
        session.Send({{"cmd", "pause"}, {"paused", session.paused}});
    }
    ImGui::SameLine();
    if (IconButton(icon::kCamera, "Screenshot", "Save a screenshot to Pictures\\iOS-Emulator"))
        SaveScreenshot(session);
    ImGui::SameLine();
    if (IconButton(icon::kRecord, "Record",
                   session.recording ? "Stop recording and save the video"
                                     : "Record the screen and sound to a video in Videos\\iOS-Emulator",
                   false, session.recording ? &kBad : nullptr))
        ToggleRecording(session);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (IconButton(icon::kFullscreen, "Fullscreen", "Only the screen, on the whole monitor (F11). F11 or Esc leaves it."))
        EnterFullscreen();

    ImGui::SameLine(0, gap);
    if (IconButton(session.muted ? icon::kMute : icon::kVolume, "Sound", session.muted ? "Sound is off" : "Sound is on",
                   !session.muted)) {
        session.muted = !session.muted;
        session.Send({{"cmd", "mute"}, {"muted", session.muted}});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    if (IconButton(icon::kGlobe, "Internet",
                   std::string("Internet is ") + (session.internet ? "on" : "off") +
                       ": lets iOS reach the internet through your connection (Cydia needs it).",
                   session.internet)) {
        session.internet = !session.internet;
        session.Send({{"cmd", "internet"}, {"on", session.internet}});
    }
    ImGui::SameLine();
    const std::string proxyTip = std::string("Web proxy is ") + (session.webProxy ? "on" : "off") + ". " +
                                 kWebProxyTip + (session.proxyStatus.empty() ? "" : "\n\n" + session.proxyStatus);
    if (IconButton(icon::kLock, "Web proxy", proxyTip, session.webProxy)) {
        session.webProxy = !session.webProxy;
        session.Send({{"cmd", "webproxy"}, {"on", session.webProxy}});
    }
    ImGui::EndDisabled();
    if (session.proxyError) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kWarn, "%s", mark);
        Tooltip(session.proxyStatus.c_str());
    }

    ImGui::SameLine(0, gap);
    if (IconButton(icon::kStop, "Stop",
                   session.Stopping() ? "Shutting down. If it's stuck, click to force the device off."
                                      : "Shut down: iOS stops cleanly, like sliding to power off.")) {
        if (session.Stopping())
            forceOffId_ = session.GetDevice().id;
        else
            session.Stop();
    }

    const char *problemMark = "(!)";
    const float rowWidth = std::min(ImGui::GetContentRegionAvail().x, std::max(width, ImGui::GetFontSize() * 24));
    CenterRow(rowWidth);
    const float rowStart = ImGui::GetCursorScreenPos().x;
    KeyboardBadge();
    ImGui::SameLine();
    if (editing_) {
        const Device &device = session.GetDevice();
        if (ImGui::Button("Add Tap"))
            AddControl(device, ControlType::Tap);
        Tooltip("A key that holds one finger on the screen.");
        ImGui::SameLine();
        if (ImGui::Button("Add Multi-finger tap"))
            AddControl(device, ControlType::MultiTap);
        Tooltip("A key that holds several fingers on the screen. Drag each one where you want it.");
        ImGui::SameLine();
        if (ImGui::Button("Add Joystick"))
            AddControl(device, ControlType::Joystick);
        Tooltip("Four keys that steer one finger around a ring. It starts on W, A, S and D, taking them from any "
                "other control here.");
        return;
    }

    // Typing: the worker types the text into the text field that has focus in iOS (worker.h,
    // "type"). A (!) after the box tells what couldn't be typed.
    ImGui::BeginDisabled(!ready);
    float reserve = ImGui::GetCursorScreenPos().x - rowStart + ButtonWidth("Type") + 8;
    if (!session.typingProblem.empty())
        reserve += ImGui::CalcTextSize(problemMark).x + 8;
    ImGui::SetNextItemWidth(rowWidth - reserve);
    bool enter = ImGui::InputTextWithHint("##type", "Type into iOS (Enter sends)", &typeText_,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    Tooltip("Typed into the text field that has focus in iOS, so tap one first.");
    ImGui::SameLine();
    if ((ImGui::Button("Type") || enter) && !typeText_.empty()) {
        session.typingProblem.clear();
        session.Send({{"cmd", "type"}, {"text", typeText_}});
        typeText_.clear();
    }
    ImGui::EndDisabled();
    if (!session.typingProblem.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s", problemMark);
        Tooltip(session.typingProblem.c_str());
    }
}

// The keyboard's mode, and Edit controls. The badge turns Controls mode on and off, as F2 does.
void App::KeyboardBadge() {
    const bool controls = settings_.keyboard == KeyboardMode::Controls;
    if (controls) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.33f, 0.56f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.40f, 0.66f, 1));
    }
    const bool clicked = ImGui::Button(kKeyboardModes[int(settings_.keyboard)]);
    if (controls)
        ImGui::PopStyleColor(2);
    const std::string tip =
        controls ? std::string("Keys hold fingers on the screen, and shortcuts take Ctrl. Click or press F2 to go back to ") +
                       kKeyboardModes[int(settings_.beforeControls)] + "."
                 : std::string("Click or press F2 for Controls, where keys hold fingers on the screen. Shortcuts and "
                               "Typing are chosen in Settings.");
    Tooltip(tip.c_str());
    if (clicked)
        ToggleControlsMode();
    ImGui::SameLine();
    if (IconButton(icon::kEdit, "Edit controls",
                   "Edit controls: keys that hold fingers on the screen. Drag a finger to place it, drag a "
                   "multi-finger tap's label to move all its fingers, drag a joystick's rim to resize it, and "
                   "right-click to change a control.",
                   editing_))
        editing_ = !editing_;
}

// Into Controls mode, or back to the mode it came from.
void App::ToggleControlsMode() {
    const bool on = settings_.keyboard == KeyboardMode::Controls;
    settings_.SetKeyboard(on ? settings_.beforeControls : KeyboardMode::Controls);
    settings_.Save();
}

// The editor, over the screen: drag a finger to place it, drag a multi-finger tap's label to move
// all its fingers, drag a joystick's centre to move it and its rim to resize it, and right-click any
// of them for the control's popup. The device gets no touches from the controls meanwhile; the mouse
// still works on the rest of the screen.
void App::ControlEditor(const Device &device, Layout &layout, ImVec2 origin, ImVec2 size) {
    const ImGuiIO &io = ImGui::GetIO();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float side = ControlRadius() * 2;
    constexpr int kRimGrip = -2;
    std::vector<Control> &controls = layout.controls;
    for (int index = 0; index < int(controls.size()); index++) {
        Control &control = controls[index];
        const std::vector<ImVec2> spots = ControlSpots(control, origin, size);
        const bool joystick = control.type == ControlType::Joystick;
        const float radius = control.radius * size.x;
        const float fromCentre = std::hypot(io.MousePos.x - spots.front().x, io.MousePos.y - spots.front().y);
        bool hot = false;
        // A part that can be grabbed, `extent` big around `centre`: one finger, -1 for all of them, or
        // a joystick's rim.
        auto grip = [&](int finger, ImVec2 centre, ImVec2 extent) {
            ImGui::PushID(finger);
            ImGui::SetCursorScreenPos(ImVec2(centre.x - extent.x / 2, centre.y - extent.y / 2));
            ImGui::InvisibleButton("grip", extent);
            hot |= ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (ImGui::IsItemActivated() && finger == kRimGrip)
                resizingControl_ = index;
            if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
                if (finger == kRimGrip)
                    control.radius = std::clamp(fromCentre / size.x, kMinJoystickRadius, kMaxJoystickRadius);
                else
                    MoveControl(control, ImVec2(io.MouseDelta.x / size.x, io.MouseDelta.y / size.y), finger);
            }
            if (ImGui::IsItemDeactivated()) {
                resizingControl_ = -1;
                SaveKeymap(device);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                chosenControl_ = index;
                askingKey_ = false;
                openPopup_ = true;
            }
            ImGui::PopID();
        };
        ImGui::PushID(index);
        // A joystick's centre stays smaller than the ring, which leaves the rim free to grab.
        const float fingerSide = joystick ? std::min(side, radius) : side;
        for (int finger = 0; finger < int(spots.size()); finger++)
            grip(finger, spots[finger], ImVec2(fingerSide, fingerSide));
        if (control.type == ControlType::MultiTap) {
            const ImVec2 text = ImGui::CalcTextSize(ControlLabel(control).c_str());
            grip(-1, LabelCentre(control, spots, origin.y), ImVec2(text.x + 12, text.y + 4));
        }
        // The rim's grip follows the mouse along the ring: a square around all of it would also take
        // the clicks meant for the screen, in its corners and inside the ring.
        const float reach = ControlRadius() / 2;
        if (joystick && (resizingControl_ == index || std::abs(fromCentre - radius) <= reach))
            grip(kRimGrip, io.MousePos, ImVec2(reach * 2, reach * 2));
        ImGui::PopID();
        DrawControl(draw, control, spots, origin.y, size.x, spots.front(), hot ? kAccent : ImVec4(1, 1, 1, 1), 1);
    }

    if (openPopup_)
        ImGui::OpenPopup("control");
    openPopup_ = false;
    if (ImGui::BeginPopup("control")) {
        ControlPopup(device, controls);
        ImGui::EndPopup();
    }
    editorPopup_ = ImGui::IsPopupOpen("control");
    if (!editorPopup_) {
        if (newControl_ && chosenControl_ >= 0 && chosenControl_ < int(controls.size()))
            controls.erase(controls.begin() + chosenControl_);   // no key was given
        chosenControl_ = -1;
        askingKey_ = newControl_ = false;
        keyRefusal_.clear();
    }
}

// The popup of the chosen control: its keys, its fingers and Delete. A key that another control holds
// is turned down, and it keeps waiting for one that's free.
void App::ControlPopup(const Device &device, std::vector<Control> &controls) {
    if (chosenControl_ < 0 || chosenControl_ >= int(controls.size())) {
        ImGui::CloseCurrentPopup();
        return;
    }
    Control &control = controls[chosenControl_];

    const bool joystick = control.type == ControlType::Joystick;
    if (askingKey_) {
        ImGui::Text("Press a key%s%s...", joystick ? " for " : "", joystick ? kDirections[keySlot_] : "");
        if (!keyRefusal_.empty())
            ImGui::TextColored(kWarn, "%s", keyRefusal_.c_str());
        DimText("Esc cancels");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            keyRefusal_.clear();
            if (newControl_)
                ImGui::CloseCurrentPopup();   // ControlEditor removes it
            else
                askingKey_ = false;
            return;
        }
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
            if (!BindableKey(ImGuiKey(key)) || !ImGui::IsKeyPressed(ImGuiKey(key), false))
                continue;
            const std::string owner = KeyOwner(controls, chosenControl_, keySlot_, ImGuiKey(key));
            if (owner.empty()) {
                control.Key(keySlot_) = ImGuiKey(key);
                askingKey_ = newControl_ = false;
                keyRefusal_.clear();
                SaveKeymap(device);
            } else {
                keyRefusal_ = std::string(ImGui::GetKeyName(ImGuiKey(key))) + " is taken by " + owner + ".";
            }
            break;
        }
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    // A row for each key: the one of a tap, or a joystick's four.
    for (int slot = 0; slot < control.KeyCount(); slot++) {
        const ImGuiKey key = control.Key(slot);
        ImGui::PushID(slot);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s: %s", joystick ? kDirections[slot] : "Key", key == ImGuiKey_None ? "none" : ImGui::GetKeyName(key));
        ImGui::SameLine(joystick ? ImGui::GetFontSize() * 7 : 0);
        if (ImGui::Button("Change key")) {
            askingKey_ = true;
            keySlot_ = slot;
        }
        ImGui::PopID();
    }
    if (control.type == ControlType::MultiTap) {
        int fingers = int(control.spots.size());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::SliderInt("Fingers", &fingers, kMinFingers, kMaxFingers))
            SetFingerCount(control, fingers);
        if (ImGui::IsItemDeactivatedAfterEdit())
            SaveKeymap(device);
    }
    if (ImGui::Button("Delete")) {
        controls.erase(controls.begin() + chosenControl_);
        chosenControl_ = -1;
        SaveKeymap(device);
        ImGui::CloseCurrentPopup();
    }
}

// Places a new control at the centre of the screen and asks for its key. It's saved once it has one.
// A joystick needs no asking: it starts on its default keys, which other controls here give up.
void App::AddControl(const Device &device, ControlType type) {
    std::vector<Control> &controls = keymaps_[device.id].For(OrientationOf(device)).controls;
    Control control;
    control.type = type;
    if (type == ControlType::Joystick) {
        for (ImGuiKey key : control.directions)
            ClearKey(controls, key);
        controls.push_back(control);
        SaveKeymap(device);
        return;
    }
    if (type == ControlType::MultiTap) {
        SetFingerCount(control, kMinFingers);
        MoveControl(control, ImVec2(-0.06f, 0), -1);   // centred on the screen
    }
    controls.push_back(control);
    chosenControl_ = int(controls.size()) - 1;
    keySlot_ = 0;
    askingKey_ = newControl_ = true;
    openPopup_ = true;
}

void App::SaveKeymap(const Device &device) {
    try {
        keymaps_[device.id].Save(device.KeymapFile());
    } catch (const std::exception &e) {
        error_ = std::string("Couldn't save the controls: ") + e.what();
    }
}

void App::SaveScreenshot(Session &session) {
    if (session.FrameWidth() == 0)
        return;
    const fs::path path = UserFolder(FOLDERID_Pictures, "screenshots") / StampedName(session.GetDevice(), "png");
    try {
        WritePng(path, session.FramePixels().data(), session.FrameWidth(), session.FrameHeight());
        notice_ = "Saved " + PathText(path);
    } catch (const std::exception &e) {
        error_ = e.what();
    }
}

// Starts or ends the device's recording. The worker does the recording (recorder.h), so it goes on
// when another device is on screen. The video keeps the orientation the device has now.
void App::ToggleRecording(Session &session) {
    if (session.recording) {
        session.Send({{"cmd", "record"}});
        return;
    }
    const Device &device = session.GetDevice();
    try {
        const fs::path path = UserFolder(FOLDERID_Videos, "recordings") / StampedName(device, "mp4");
        session.Send({{"cmd", "record"},
                      {"to", PathText(path)},
                      {"landscape", OrientationOf(device) == Orientation::Landscape}});
    } catch (const std::exception &e) {
        error_ = e.what();
    }
}

void App::SettingsPage() {
    ImGui::Text("Settings");
    ImGui::Separator();
    bool changed = false;
    DimText("NEW SESSIONS");
    changed |= ImGui::Checkbox("Internet", &settings_.internet);
    Tooltip("Devices start with internet access (you can switch it while one runs).");
    changed |= ImGui::Checkbox("Web proxy", &settings_.webProxy);
    Tooltip(kWebProxyTip);
    changed |= ImGui::Checkbox("Headphones plugged in", &settings_.headphones);
    Tooltip("iOS plays through the headphone jack: stereo, without the speaker's EQ (a little faster).");
    changed |= ImGui::Checkbox("Start muted", &settings_.muted);
    ImGui::Spacing();
    DimText("DISPLAY AND CONTROLS");
    changed |= ImGui::Checkbox("Smooth scaling", &settings_.smooth);
    changed |= ImGui::Checkbox("Arrow keys tilt the device", &settings_.tilt);
    const char *const modeTips[] = {
        "The keys are the hardware shortcuts listed below.",
        "What you type while a device is on screen goes into the text field that has focus in iOS (characters, "
        "Backspace and Enter). The shortcuts then take Ctrl (Ctrl+H for Home, and so on).",
        "Keys hold fingers on the screen, wherever you placed them with Edit controls under the device. The "
        "shortcuts then take Ctrl. F2 turns this mode on and off.",
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keyboard");
    for (int mode = 0; mode < int(std::size(kKeyboardModes)); mode++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kKeyboardModes[mode], settings_.keyboard == KeyboardMode(mode))) {
            settings_.SetKeyboard(KeyboardMode(mode));
            changed = true;
        }
        Tooltip(modeTips[mode]);
    }
    changed |= ImGui::Checkbox("Show controls", &settings_.showControls);
    Tooltip("In Controls mode, faint labels on the screen show which key is where.");
    if (changed)
        settings_.Save();

    ImGui::Spacing();
    DimText("CONTROLS");
    ImGui::BulletText("Click and drag on the screen to touch. Hold Alt while dragging to pinch or rotate, or Shift to swipe with four fingers.");
    ImGui::BulletText("The device's buttons work: click Home, Sleep/Wake or the volume keys (hold them as on the real one).");
    if (settings_.ShortcutsTakeCtrl()) {
        ImGui::BulletText(settings_.keyboard == KeyboardMode::Typing
                              ? "What you type goes to iOS while a device is on screen."
                              : "Keys hold fingers on the screen, as placed with Edit controls under the device.");
        ImGui::BulletText("Ctrl+H: Home, Ctrl+P: Sleep/Wake, Ctrl with + and -: volume, Ctrl+R: rotate, "
                          "Ctrl with the arrow keys: tilt.");
    } else {
        ImGui::BulletText("H: Home, P: Sleep/Wake, + and -: volume, R: rotate, arrow keys: tilt.");
        ImGui::BulletText("Type text into iOS with the box under the device.");
    }
    ImGui::BulletText("F2: Controls mode on and off. F11: fullscreen. F11 or Esc leaves it.");
    ImGui::Spacing();
    DimText("FOLDERS");
    ImGui::Text("Data: %s", PathText(DataDir()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open"))
        ShellExecuteW(nullptr, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Text("Resources: %s", PathText(ResourcesDir()).c_str());
}

// Asks for a file with the standard Open dialog. Returns an empty path if cancelled.
fs::path ChooseFile(const wchar_t *filter) {
    wchar_t path[MAX_PATH * 2] = L"";
    OPENFILENAMEW dialog{sizeof dialog};
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = DWORD(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? fs::path(path) : fs::path();
}

// Devices LightTouch prepared on this computer that aren't in our library yet.
std::vector<fs::path> LightTouchDevices(const std::vector<Device> &ours) {
    std::vector<fs::path> found;
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        return found;
    const fs::path records = fs::path(local) / "LightTouch" / "App" / "library" / "records";
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(records, error)) {
        const std::string id = PathText(entry.path().filename());
        const bool imported = std::any_of(ours.begin(), ours.end(), [&](const Device &d) { return d.id == id; });
        if (!imported && fs::exists(entry.path() / "record.json"))
            found.push_back(entry.path());
    }
    return found;
}

// Adding a device: a grid of the devices that can be emulated, then the chosen one's iOS versions
// on a wheel. A preparation in progress (or just finished) takes over the page.
void App::AddDevicePage() {
    if (preparation_)
        PreparationProgress();
    else if (choosingVersion_)
        VersionPicker();
    else
        DeviceGrid();
}

void App::PreparationProgress() {
    FontText(g_titleFont, "Add a device");
    ImGui::Separator();
    {
        const Preparation::Status status = preparation_->GetStatus();
        const Firmware &firmware = preparation_->Request().firmware;
        ImGui::Text("Preparing %s, iOS %s%s", firmware.Product().c_str(), firmware.label.c_str(),
                    preparation_->Request().jailbreak ? " (jailbroken)" : "");
        ImGui::ProgressBar(float(status.fraction), ImVec2(-1, 0));
        ImGui::TextWrapped("%s", status.step.c_str());
        if (!status.finished) {
            if (ImGui::Button("Cancel"))
                preparation_->Cancel();
        } else if (status.failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, kBad);
            ImGui::TextWrapped("%s", status.error.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Back"))
                preparation_.reset();
        } else {
            preparation_.reset();
            choosingVersion_ = false;
            Reload();
            settings_.selected = status.deviceId;
            settings_.Save();
            page_ = Page::Device;
            notice_ = "The new device is ready. Press Start.";
        }
    }
}

// Picks a device on the Add Device page and moves on to its versions, preselecting one that's
// tested if there is one, else the newest.
void App::ChooseProduct(const std::string &productType) {
    chosenProduct_ = productType;
    chosenFirmware_.clear();
    int index = 0, chosenIndex = 0;
    for (const auto &f : Catalog()) {
        if (f.productType != chosenProduct_)
            continue;
        if (chosenFirmware_.empty() || !FindFirmware(chosenFirmware_)->Tested()) {
            chosenFirmware_ = f.id;
            chosenIndex = index;
        }
        index++;
    }
    drum_ = Drum{float(chosenIndex), float(chosenIndex)};
    choosingVersion_ = !chosenFirmware_.empty();
    newName_.clear();
    localIpsw_.clear();
}

// The first page of Add Device: every device that can be emulated, as tiles grouped by family.
void App::DeviceGrid() {
    FontText(g_titleFont, "Add a device");
    DimText("Pick the device to emulate. Next, you choose its iOS version.");
    ImGui::Spacing();
    ImGui::BeginChild("grid", ImVec2(0, 0));

    // The catalog's devices in its order (oldest first), with the range of versions each runs.
    struct Product {
        std::string type, board, first, last;
        int versions = 0;
    };
    std::vector<Product> products;
    for (const auto &f : Catalog()) {
        if (products.empty() || products.back().type != f.productType)
            products.push_back({f.productType, f.board, f.label});
        products.back().last = f.label;
        products.back().versions++;
    }

    const float tile = std::floor(ImGui::GetFontSize() * 10.5f), tall = std::floor(tile * 1.32f);
    const float gap = std::floor(ImGui::GetFontSize() * 0.9f);
    const std::pair<const char *, const char *> families[] = {{"IPHONE", "iPhone"}, {"IPOD TOUCH", "iPod"}, {"IPAD", "iPad"}};
    for (const auto &[heading, prefix] : families) {
        DimText(heading);
        const int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
        int column = 0;
        for (const Product &product : products) {
            if (product.type.rfind(prefix, 0) != 0)
                continue;
            if (column++ % perRow)
                ImGui::SameLine(0, gap);
            ImGui::PushID(product.type.c_str());
            const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tile, tall));
            const bool hovered = ImGui::IsItemHovered();
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            if (hovered) {   // lifts a little under the mouse
                min.y -= 3, max.y -= 3;
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImDrawList *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, max, hovered ? IM_COL32(44, 48, 57, 255) : IM_COL32(34, 37, 43, 255), 12);
            if (hovered)
                draw->AddRect(min, max, ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.7f)), 12, 0, 1.5f);
            const float pad = tile * 0.08f;
            if (Skin *skin = Skin::For(product.board)) {
                const SkinLayout layout = skin->Fit(ImVec2(min.x + pad, min.y + pad), ImVec2(tile - 2 * pad, tall * 0.66f),
                                                    Orientation::Portrait);
                skin->Draw(draw, layout, [](int) { return false; }, -1, false);
                skin->DrawGlare(draw, layout);
            }
            auto centered = [&](const std::string &text, float y, ImU32 color) {
                const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                draw->AddText(ImVec2(std::floor((min.x + max.x - size.x) / 2), y), color, text.c_str());
            };
            const float line = ImGui::GetTextLineHeight();
            centered(ProductName(product.board), min.y + tall * 0.66f + pad * 1.6f, ImGui::GetColorU32(ImGuiCol_Text));
            centered(product.first == product.last ? "iOS " + product.first : "iOS " + product.first + " to " + product.last,
                     min.y + tall * 0.66f + pad * 1.6f + line + 2, ImGui::GetColorU32(kDim));
            centered(std::to_string(product.versions) + (product.versions == 1 ? " version" : " versions"),
                     min.y + tall * 0.66f + pad * 1.6f + 2 * line + 4, ImGui::GetColorU32(kDim));
            if (clicked)
                ChooseProduct(product.type);
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, gap / 2));
    }
    LightTouchImports();
    ImGui::EndChild();
}

// Devices LightTouch already made can be brought over as they are.
void App::LightTouchImports() {
    const std::vector<fs::path> lightTouch = LightTouchDevices(devices_);
    if (lightTouch.empty())
        return;
    ImGui::Separator();
    DimText("FROM LIGHTTOUCH");
    for (const auto &dir : lightTouch) {
        ImGui::PushID(PathText(dir).c_str());
        std::string label = PathText(dir.filename());
        try {
            Device device = OpenDevice(dir);
            label = device.name + " (" + device.Product() + ", iOS " + device.Version() + ")";
        } catch (...) {
        }
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Import")) {
            try {
                Device device = ImportDevice(dir);
                Reload();
                settings_.selected = device.id;
                page_ = Page::Device;
                notice_ = "Imported " + device.name + ".";
            } catch (const std::exception &e) {
                error_ = e.what();
            }
        }
        ImGui::PopID();
    }
}

// The second page of Add Device: the chosen device, a wheel of its iOS versions, and the options.
void App::VersionPicker() {
    std::vector<const Firmware *> versions;
    for (const auto &f : Catalog())
        if (f.productType == chosenProduct_)
            versions.push_back(&f);
    if (versions.empty()) {
        choosingVersion_ = false;
        return;
    }
    if (ImGui::Button(g_icons ? (std::string(icon::kLeft) + "  All devices").c_str() : "<  All devices")) {
        choosingVersion_ = false;
        return;
    }
    ImGui::SameLine();
    FontText(g_titleFont, ProductName(versions[0]->board));
    ImGui::Separator();

    int index = 0;
    for (size_t i = 0; i < versions.size(); i++)
        if (versions[i]->id == chosenFirmware_)
            index = int(i);

    // Left: the device, with the chosen version on its glass.
    const float artWidth = std::max(ImGui::GetContentRegionAvail().x * 0.36f, ImGui::GetFontSize() * 13);
    ImGui::BeginChild("art", ImVec2(artWidth, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (Skin *skin = Skin::For(versions[0]->board)) {
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos(), area = ImGui::GetContentRegionAvail();
        const SkinLayout layout = skin->Fit(ImVec2(origin.x + 8, origin.y + 8), ImVec2(area.x - 16, area.y - 16),
                                            Orientation::Portrait);
        skin->Draw(draw, layout, [](int) { return false; });
        const std::string text = "iOS " + versions[index]->label;
        ImFont *font = g_largeFont ? g_largeFont : ImGui::GetFont();
        const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str());
        draw->AddText(font, font->FontSize,
                      ImVec2(std::floor((layout.screenMin.x + layout.screenMax.x - size.x) / 2),
                             std::floor((layout.screenMin.y + layout.screenMax.y - size.y) / 2)),
                      IM_COL32(150, 154, 162, 255), text.c_str());
        skin->DrawGlare(draw, layout);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the wheel, then what the chosen version needs and the options.
    ImGui::BeginChild("choose", ImVec2(0, 0));
    DimText("iOS VERSION");
    std::vector<WheelRow> rows;
    for (const Firmware *f : versions)
        rows.push_back({"iOS " + f->label, f->build, f->Tested()});
    const float wheelWidth = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22);
    DrumPicker("versions", rows, index, drum_, wheelWidth);
    animating_ = drum_.Moving();
    const Firmware *firmware = versions[index];
    chosenFirmware_ = firmware->id;
    ImGui::Spacing();
    ImGui::Text("iOS %s", firmware->label.c_str());
    ImGui::SameLine();
    DimText("build " + firmware->build + ", released " + firmware->released);
    if (firmware->Tested())
        ImGui::TextColored(kGood, "Runs well (tested by LightTouch).");
    else if (firmware->board == "n45ap" || firmware->board == "m68ap")
        ImGui::TextColored(kWarn, "Experimental: this device currently stops at a white screen after setup.");
    else
        ImGui::TextColored(kWarn, "Not tested yet: it may or may not boot.");
    ImGui::Spacing();
    if (firmware->storageGb)
        ImGui::BulletText("Storage: %d GB (the emulated hardware's, the same for every version)", firmware->storageGb);
    ImGui::BulletText("Download: %s from Apple (or a mirror)", FormatBytes(firmware->bytes).c_str());
    ImGui::BulletText("Needs about %s free while preparing; the device then takes %s",
                      FormatBytes(firmware->peakBytes).c_str(), FormatBytes(firmware->preparedBytes).c_str());
    if (!firmware->sibling.empty())
        ImGui::BulletText("Also downloads the IPSW of %s (for its data-protection setup)", firmware->sibling.c_str());
    if (NeedsBootrom(firmware->board))
        ImGui::BulletText("Also downloads this device's boot ROM");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("Name", firmware->Product().c_str(), &newName_);

    const std::string noJailbreak = firmware->JailbreakUnavailable();
    bool jailbreak = jailbreak_ && noJailbreak.empty();
    ImGui::BeginDisabled(!noJailbreak.empty());
    if (ImGui::Checkbox("Jailbreak (Cydia, apt, bash, root file access)", &jailbreak))
        jailbreak_ = jailbreak;
    ImGui::EndDisabled();
    if (!noJailbreak.empty())
        DimText(noJailbreak);
    else if (jailbreak)
        DimText("Cydia's files are downloaded while preparing. Turn on Internet when you run it.");

    ImGui::Spacing();
    if (localIpsw_.empty()) {
        DimText("The IPSW is downloaded for you.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Use a file I have..."))
            localIpsw_ = ChooseFile(L"iOS firmware (*.ipsw)\0*.ipsw\0All files\0*.*\0");
    } else {
        ImGui::TextWrapped("IPSW: %s", PathText(localIpsw_).c_str());
        if (ImGui::SmallButton("Download it instead"))
            localIpsw_.clear();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.78f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.50f, 0.88f, 1));
    if (ImGui::Button("Prepare", ImVec2(ImGui::GetFontSize() * 9, 0))) {
        PrepareRequest request;
        request.firmware = *firmware;
        request.name = newName_;
        request.jailbreak = jailbreak;
        request.ipsw = localIpsw_;
        preparation_ = std::make_unique<Preparation>(request);
        newName_.clear();
    }
    ImGui::PopStyleColor(2);
    ImGui::EndChild();
}

// Asks where to save a file with the standard Save dialog. Returns an empty path if cancelled.
fs::path ChooseSaveFile(const std::string &suggestedName) {
    wchar_t path[MAX_PATH * 2] = L"";
    wcsncpy(path, Widen(suggestedName).c_str(), MAX_PATH);
    OPENFILENAMEW dialog{sizeof dialog};
    dialog.lpstrFilter = L"All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = DWORD(std::size(path));
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? fs::path(path) : fs::path();
}

std::string ParentPath(const std::string &path) {
    size_t slash = path.find_last_of('/', path.size() > 1 ? path.size() - 2 : 0);
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

std::string ChildPath(const std::string &folder, const std::string &name) {
    return folder.back() == '/' ? folder + name : folder + "/" + name;
}

void App::DevRequest(Session &session, Json command, std::function<void(const Json &)> onSuccess) {
    DevPanel &panel = devPanels_[session.GetDevice().id];
    panel.busy++;
    const std::string id = session.GetDevice().id;
    session.Request(std::move(command), [this, id, onSuccess](const Json &reply) {
        DevPanel &panel = devPanels_[id];
        panel.busy--;
        if (reply.value("ok", false)) {
            panel.message.clear();
            if (onSuccess)
                onSuccess(reply.value("result", Json()));
        } else {
            panel.message = reply.value("error", "Failed");
        }
    });
}

void App::DevListFiles(const std::string &id, const std::string &path) {
    auto found = sessions_.find(id);
    if (found == sessions_.end())
        return;
    devPanels_[id].path = path;
    DevRequest(*found->second, {{"cmd", "files.list"}, {"path", path}},
               [this, id](const Json &result) {
                   devPanels_[id].files = result;
                   devPanels_[id].filesLoaded = true;
               });
}

void App::DevListApps(const std::string &id) {
    auto found = sessions_.find(id);
    if (found != sessions_.end())
        DevRequest(*found->second, {{"cmd", "apps.list"}}, [this, id](const Json &result) {
            devPanels_[id].apps = result;
            devPanels_[id].appsLoaded = true;
        });
}

// Installs files dropped on the window on the selected device: .ipa apps on any device, .deb
// packages on a jailbroken one (the worker refuses them otherwise). The worker does them in order.
void App::DropFiles(const std::vector<fs::path> &files) {
    for (const fs::path &file : files) {
        std::string extension = PathText(file.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(tolower(c)); });
        const Device *device = Selected();
        Session *session = device ? SessionFor(*device) : nullptr;
        if (extension != ".ipa" && extension != ".deb") {
            error_ = "Only .ipa and .deb files can be installed: " + PathText(file.filename());
        } else if (!device) {
            error_ = "Add a device first, then drop the file again.";
        } else if (!session || session->Stopping()) {
            error_ = "Start " + device->name + " first, then drop the file again.";
        } else {
            InstallDropped(*session, file, extension == ".deb");
        }
    }
}

// Shows how an install goes above the page, since the developer tools may be hidden.
void App::InstallDropped(Session &session, const fs::path &file, bool package) {
    const std::string id = session.GetDevice().id, device = session.GetDevice().name, name = PathText(file.filename());
    error_.clear();
    notice_ = device + ": installing " + name;
    session.progress.clear();
    devPanels_[id].drops++;
    session.Request({{"cmd", package ? "packages.install" : "apps.install"}, {package ? "deb" : "ipa", PathText(file)}},
                    [this, id, device, name](const Json &reply) {
                        DevPanel &panel = devPanels_[id];
                        panel.drops--;
                        panel.appsLoaded = false;   // the Apps tab reloads when it's next shown
                        if (reply.value("ok", false)) {
                            notice_ = name + " is installed on " + device + ".";
                        } else {
                            notice_.clear();
                            error_ = "Couldn't install " + name + " on " + device + ": " + reply.value("error", "it failed.");
                        }
                    });
}

void App::DeveloperPanel(Device &device, Session &session) {
    DevPanel &panel = devPanels_[device.id];
    const bool ready = session.state == "running" || session.state == "paused";
    ImGui::TextUnformatted("Developer tools");
    ImGui::SameLine();
    DimText(session.agent == 1 ? "guest tools running" : session.agent == 2 ? "guest tools not answering"
                                                                             : "guest tools starting...");
    if (panel.busy > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "working...");
    }
    if (!panel.message.empty())
        ImGui::TextColored(kBad, "%s", panel.message.c_str());
    if (!ready) {
        DimText("Available once iOS has started.");
        return;
    }
    if (!ImGui::BeginTabBar("devtabs"))
        return;

    // Info: what lockdown reports, plus the emulator's own facts.
    if (ImGui::BeginTabItem("Info")) {
        if (ImGui::Button("Refresh") || panel.TimeToLoad(panel.infoLoaded))
            DevRequest(session, {{"cmd", "info"}}, [&panel](const Json &result) {
                panel.info = result;
                panel.infoLoaded = true;
            });
        ImGui::SameLine();
        if (ImGui::Button("Kernel log"))
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (device.State() / "serial.log").c_str(), nullptr, SW_SHOWNORMAL);
        Tooltip("The kernel's serial console output (state\\serial.log).");
        ImGui::SameLine();
        if (ImGui::Button("Device folder"))
            ShellExecuteW(nullptr, L"open", device.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::BeginTable("info", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            auto row = [](const std::string &key, const std::string &value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(kDim, "%s", key.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
            };
            row("Emulated machine", session.machine + ", " + std::to_string(session.screenWidth) + "x" +
                                        std::to_string(session.screenHeight));
            row("Root file access", device.Jailbroken() ? "yes (AFC2)" : "no (Media folder only)");
            if (panel.info.is_object()) {
                for (const auto &[key, value] : panel.info.items()) {
                    if (key == "ActivationPublicKey" || key == "DevicePublicKey" || key == "RootFileAccess")
                        continue;   // long and not useful to read
                    row(key, value.is_string() ? value.get<std::string>() : value.dump());
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // Files: browse, download, upload, delete, rename, new folder.
    if (ImGui::BeginTabItem("Files")) {
        auto open = [&](const std::string &path) { DevListFiles(device.id, path); };
        if (panel.TimeToLoad(panel.filesLoaded))
            open(panel.path);
        if (ImGui::Button("Up"))
            open(ParentPath(panel.path));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 13);
        if (ImGui::InputText("##path", &panel.path, ImGuiInputTextFlags_EnterReturnsTrue))
            open(panel.path);
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
            open(panel.path);
        ImGui::SameLine();
        if (ImGui::Button("Upload...")) {
            fs::path file = ChooseFile(L"All files\0*.*\0");
            if (!file.empty())
                DevRequest(session, {{"cmd", "files.upload"}, {"from", PathText(file)},
                                     {"path", ChildPath(panel.path, PathText(file.filename()))}},
                           [this, id = device.id, path = panel.path](const Json &) { DevListFiles(id, path); });
        }
        ImGui::SameLine();
        if (ImGui::Button("New folder")) {
            panel.newName = "New Folder";
            ImGui::OpenPopup("New folder");
        }
        if (!device.Jailbroken())
            DimText("Without the jailbreak only the Media folder is reachable.");

        if (ImGui::BeginTable("files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
            ImGui::TableHeadersRow();
            std::string openPath;
            for (const auto &entry : panel.files) {
                const std::string name = entry.value("name", ""), type = entry.value("type", "file");
                const std::string path = ChildPath(panel.path, name);
                ImGui::PushID(name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string label = (type == "folder" ? "[ ] " : type == "link" ? "->  " : "    ") + name +
                                          (type == "link" ? "  -> " + entry.value("target", "") : "");
                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(0) && type != "file")
                    openPath = path;   // links usually point at folders; listing one that doesn't just fails
                ImGui::TableNextColumn();
                if (type == "file")
                    DimText(FormatBytes(entry.value("size", uint64_t(0))));
                ImGui::TableNextColumn();
                if (type == "file" && ImGui::SmallButton("Save")) {
                    fs::path to = ChooseSaveFile(name);
                    if (!to.empty())
                        DevRequest(session, {{"cmd", "files.download"}, {"path", path}, {"to", PathText(to)}},
                                   [this, to](const Json &) { notice_ = "Saved " + PathText(to); });
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Rename")) {
                    panel.target = path;
                    panel.newName = name;
                    ImGui::OpenPopup("Rename item");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    panel.target = path;
                    ImGui::OpenPopup("Delete item");
                }
                if (ImGui::BeginPopupModal("Rename item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::InputText("New name", &panel.newName);
                    if (ImGui::Button("Rename")) {
                        DevRequest(session, {{"cmd", "files.rename"}, {"path", panel.target},
                                             {"to", ChildPath(panel.path, panel.newName)}},
                                   [this, id = device.id, folder = panel.path](const Json &) { DevListFiles(id, folder); });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (ImGui::BeginPopupModal("Delete item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Delete %s%s?", panel.target.c_str(), type == "folder" ? " and everything in it" : "");
                    if (ImGui::Button("Delete")) {
                        DevRequest(session, {{"cmd", "files.delete"}, {"path", panel.target}},
                                   [this, id = device.id, folder = panel.path](const Json &) { DevListFiles(id, folder); });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!openPath.empty())
                open(openPath);
        }
        if (ImGui::BeginPopupModal("New folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("Name", &panel.newName);
            if (ImGui::Button("Create")) {
                DevRequest(session, {{"cmd", "files.mkdir"}, {"path", ChildPath(panel.path, panel.newName)}},
                           [this, id = device.id, folder = panel.path](const Json &) { DevListFiles(id, folder); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndTabItem();
    }

    // Apps: what's installed, open one, install an .ipa.
    if (ImGui::BeginTabItem("Apps")) {
        auto refresh = [&] { DevListApps(device.id); };
        if (panel.TimeToLoad(panel.appsLoaded))
            refresh();
        if (ImGui::Button("Refresh"))
            refresh();
        ImGui::SameLine();
        if (ImGui::Button("Install .ipa...")) {
            fs::path ipa = ChooseFile(L"iOS apps (*.ipa)\0*.ipa\0All files\0*.*\0");
            if (!ipa.empty()) {
                panel.installStatus = "Starting the install of " + PathText(ipa.filename());
                DevRequest(session, {{"cmd", "apps.install"}, {"ipa", PathText(ipa)}}, [this, id = device.id, ipa](const Json &) {
                    devPanels_[id].installStatus = PathText(ipa.filename()) + " is installed.";
                    DevListApps(id);
                });
            }
        }
        Tooltip("Unsigned apps work too: AppSync is part of every prepared device.\n"
                "You can also drop an .ipa, or a .deb on a jailbroken device, on the window.");
        if (!session.progress.empty() && panel.busy > 0)
            panel.installStatus = session.progress;
        if (!panel.installStatus.empty())
            DimText(panel.installStatus);
        if (ImGui::BeginTable("apps", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Bundle ID", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
            ImGui::TableHeadersRow();
            for (const auto &app : panel.apps) {
                const std::string bundle = app.value("bundle", "");
                ImGui::PushID(bundle.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", app.value("name", "").c_str());
                ImGui::SameLine();
                DimText(app.value("version", "") + (app.value("type", "") == "User" ? "" : "  system"));
                ImGui::TableNextColumn();
                DimText(bundle);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Open"))
                    DevRequest(session, {{"cmd", "apps.launch"}, {"bundle", bundle}}, nullptr);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // Terminal: commands run as root through the guest agent (a shell on jailbroken devices).
    if (ImGui::BeginTabItem("Terminal")) {
        DimText(device.Jailbroken() ? "Runs as root through /bin/sh. More tools (Core Utilities, OpenSSH...) come from Cydia."
                                    : "Runs a program as root: give its full path (no shell without the jailbreak).");
        ImGui::BeginChild("output", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(panel.terminal.c_str());
        if (panel.scrollTerminal) {
            ImGui::SetScrollHereY(1.0f);
            panel.scrollTerminal = false;
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
        bool run = ImGui::InputTextWithHint("##command", "e.g. ls -la /var/mobile", &panel.command,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        run |= ImGui::Button("Run");
        if (run && !panel.command.empty()) {
            panel.terminal += "# " + panel.command + "\n";
            panel.scrollTerminal = true;
            DevRequest(session, {{"cmd", "shell"}, {"command", panel.command}, {"sh", device.Jailbroken()}},
                       [&panel](const Json &r) {
                           panel.terminal += r.value("output", "");
                           if (int status = r.value("status", 0); status != 0)
                               panel.terminal += "(exit status " + std::to_string(status) + ")\n";
                           panel.scrollTerminal = true;
                       });
            panel.command.clear();
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::EndTabItem();
    }

    // Logs: the live system log, filtered.
    if (ImGui::BeginTabItem("Logs")) {
        if (ImGui::Checkbox("Capture the system log", &panel.syslogOn))
            DevRequest(session, {{"cmd", "syslog"}, {"on", panel.syslogOn}}, nullptr);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##filter", "filter", &panel.filter);
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
            session.syslog.clear();
        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto &line : session.syslog)
            if (panel.filter.empty() || line.find(panel.filter) != std::string::npos)
                ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);   // follow new lines while at the bottom
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // SSH: a local port forwarded to the device over USB.
    if (ImGui::BeginTabItem("SSH")) {
        ImGui::TextWrapped("Forward a port on this computer to the device's SSH port (22) over the USB cable, "
                           "like iproxy. Only this computer can connect.");
        ImGui::BeginDisabled(panel.forwardOn);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputInt("Local port", &panel.port, 0);
        ImGui::EndDisabled();
        if (ImGui::Checkbox("Forward", &panel.forwardOn))
            DevRequest(session, {{"cmd", "forward"}, {"on", panel.forwardOn}, {"port", panel.port}, {"devicePort", 22}},
                       nullptr);
        if (panel.forwardOn) {
            const std::string ssh = "ssh -oHostKeyAlgorithms=+ssh-rsa root@localhost -p " + std::to_string(panel.port);
            ImGui::TextUnformatted("Connect with:");
            ImGui::TextColored(kAccent, "%s", ssh.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy"))
                ImGui::SetClipboardText(ssh.c_str());
            DimText("The default password is alpine (change it with passwd).");
        }
        ImGui::Spacing();
        DimText("Needs OpenSSH, from Cydia: turn Internet on, open Cydia, search for OpenSSH and install it.");
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void App::ClosingOverlay() {
    ImGui::OpenPopup("Closing");
    if (ImGui::BeginPopupModal("Closing", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Shutting down %zu running device(s)...", sessions_.size());
        if (ImGui::Button("Quit now (the devices will check their storage next time)")) {
            for (auto &[id, session] : sessions_)
                session->Kill();
        }
        ImGui::EndPopup();
    }
}

// Asks before forcing off a device that is shutting down. It lives here rather than in Controls()
// so that it goes away by itself if the device finishes shutting down meanwhile.
void App::ForceOffOverlay() {
    auto found = sessions_.find(forceOffId_);
    ImGui::OpenPopup("Force power off");
    if (ImGui::BeginPopupModal("Force power off", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        bool done = found == sessions_.end();
        if (!done) {
            ImGui::TextUnformatted("Force power off this device?");
            DimText("iOS will check its storage next time it starts.");
            if (ImGui::Button("Force power off")) {
                found->second->ForceOff();
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                done = true;
        }
        if (done) {
            forceOffId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---- Window and OpenGL -----------------------------------------------------------------------

// For testing the UI where nobody watches the screen: IOS_EMULATOR_UI_START=1 starts the selected
// device (IOS_EMULATOR_UI_FULLSCREEN=1 also shows it fullscreen, IOS_EMULATOR_UI_LANDSCAPE=1 turns
// it, IOS_EMULATOR_UI_DEVTOOLS=1 shows the developer tools without saving that,
// IOS_EMULATOR_UI_EDITOR=1 opens the controls editor), IOS_EMULATOR_UI_PAGE=add
// opens the Add Device page (IOS_EMULATOR_UI_PRODUCT=<product type>, e.g. iPhone3,1, goes straight to
// that device's versions), IOS_EMULATOR_UI_DROP=<file> drops that file on the window after
// IOS_EMULATOR_UI_DROP_AFTER seconds (default 1), and IOS_EMULATOR_UI_SHOT=<file.png> saves the
// window after IOS_EMULATOR_UI_SHOT_AFTER seconds (default 5) and closes the app.
std::string EnvText(const char *name) {
    const char *value = getenv(name);
    return value ? value : "";
}

// Posts what Windows sends the window when a file is dropped on it (for IOS_EMULATOR_UI_DROP).
void PostDrop(HWND hwnd, const std::string &file) {
    const std::wstring path = Widen(file);
    // A DROPFILES header, then the paths, the last one followed by an extra NUL.
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t));
    auto *drop = static_cast<DROPFILES *>(GlobalLock(memory));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(drop + 1, path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    PostMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
}

void SaveWindowPicture(const fs::path &path, int width, int height) {
    std::vector<uint8_t> rgba(size_t(width) * height * 4), bgra(rgba.size());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (int y = 0; y < height; y++)   // OpenGL's rows run bottom-up
        for (int x = 0; x < width; x++) {
            const uint8_t *from = &rgba[(size_t(height - 1 - y) * width + x) * 4];
            uint8_t *to = &bgra[(size_t(y) * width + x) * 4];
            to[0] = from[2], to[1] = from[1], to[2] = from[0], to[3] = 255;
        }
    WritePng(path, bgra.data(), width, height);
}

App *g_app = nullptr;
int g_width = 1280, g_height = 820;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return true;
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_width = LOWORD(lParam);
            g_height = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)   // Alt is for two-finger touches, not the window menu
            return 0;
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::vector<fs::path> files(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0));
        for (UINT i = 0; i < files.size(); i++) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, path.data(), UINT(path.size() + 1));
            files[i] = path;
        }
        DragFinish(drop);
        if (g_app)
            g_app->DropFiles(files);
        return 0;
    }
    case WM_CLOSE:
        if (g_app)
            g_app->RequestClose();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Fullscreen: the window loses its frame and covers its monitor. Leaving puts it back as it was
// (size, position, maximized or not).
void SetFullscreen(HWND hwnd, bool on) {
    static WINDOWPLACEMENT before = {sizeof before};
    if (on) {
        GetWindowPlacement(hwnd, &before);
        MONITORINFO monitor = {sizeof monitor};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT &area = monitor.rcMonitor;
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &before);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void SetupStyle(float scale) {
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 0;
    style.ChildRounding = 6;
    style.FrameRounding = 5;
    style.PopupRounding = 6;
    style.GrabRounding = 4;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.WindowPadding = ImVec2(14, 14);
    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.13f, 0.15f, 1);
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.24f, 0.28f, 1);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f, 0.32f, 0.38f, 1);
    colors[ImGuiCol_ButtonActive] = kAccent;
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.30f, 0.48f, 1);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.25f, 0.30f, 1);
    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.22f, 1);
    colors[ImGuiCol_CheckMark] = kAccent;
    style.ScaleAllSizes(scale);
}

}  // namespace

int RunUi() {
    // The app's own log (the workers keep theirs) goes to <data>\logs\app.log.
    fs::create_directories(DataDir() / "logs");
    HANDLE log = CreateFileW((DataDir() / "logs" / "app.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN)
        SetStdHandle(STD_ERROR_HANDLE, log);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW windowClass = {sizeof windowClass, CS_OWNDC, WindowProc, 0, 0, GetModuleHandleW(nullptr)};
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = L"iOSEmulator";
    RegisterClassExW(&windowClass);
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(windowClass.lpszClassName, L"iOS Emulator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, int(1280 * scale), int(860 * scale), nullptr, nullptr,
                              windowClass.hInstance, nullptr);
    DragAcceptFiles(hwnd, TRUE);   // .ipa and .deb files dropped on the window install (App::DropFiles)

    // A plain OpenGL context for the window (Dear ImGui's OpenGL 3 backend draws with it).
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR format = {sizeof format, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                    PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &format), &format);
    HGLRC gl = wglCreateContext(dc);
    wglMakeCurrent(dc, gl);
    using SwapInterval = BOOL(WINAPI *)(int);
    if (auto swapInterval = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT")))
        swapInterval(1);   // draw at the display's refresh rate

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing to remember about window layout
    // Segoe UI, with the icons of Segoe MDL2 Assets merged in; a large size for the version wheel,
    // and Segoe UI Semibold for titles.
    const fs::path fonts = "C:\\Windows\\Fonts";
    if (fs::exists(fonts / "segoeui.ttf")) {
        io.Fonts->AddFontFromFileTTF(PathText(fonts / "segoeui.ttf").c_str(), 17.0f * scale);
        if (fs::exists(fonts / "segmdl2.ttf")) {
            static const ImWchar icons[] = {0xE700, 0xE9FF, 0};
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.PixelSnapH = true;
            merge.GlyphOffset = ImVec2(0, 3.0f * scale);   // MDL2 sits high next to Segoe UI's letters
            g_icons = io.Fonts->AddFontFromFileTTF(PathText(fonts / "segmdl2.ttf").c_str(), 15.0f * scale, &merge, icons);
        }
        g_largeFont = io.Fonts->AddFontFromFileTTF(PathText(fonts / "segoeui.ttf").c_str(), 25.0f * scale);
        const fs::path semibold = fs::exists(fonts / "seguisb.ttf") ? fonts / "seguisb.ttf" : fonts / "segoeui.ttf";
        g_titleFont = io.Fonts->AddFontFromFileTTF(PathText(semibold).c_str(), 22.0f * scale);
    }
    SetupStyle(scale);
    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init();

    int result = 0;
    {
        App app;
        g_app = &app;
        const std::string shot = EnvText("IOS_EMULATOR_UI_SHOT");
        const double shotAfter = EnvText("IOS_EMULATOR_UI_SHOT_AFTER").empty() ? 5.0 : atof(EnvText("IOS_EMULATOR_UI_SHOT_AFTER").c_str());
        std::string drop = EnvText("IOS_EMULATOR_UI_DROP");
        const double dropAfter = EnvText("IOS_EMULATOR_UI_DROP_AFTER").empty() ? 1.0 : atof(EnvText("IOS_EMULATOR_UI_DROP_AFTER").c_str());
        const uint64_t started = NowNs();
        if (EnvText("IOS_EMULATOR_UI_START") == "1")
            app.StartSelected();
        if (EnvText("IOS_EMULATOR_UI_FULLSCREEN") == "1")
            app.EnterFullscreen();
        if (EnvText("IOS_EMULATOR_UI_PAGE") == "add")
            app.ShowAddDevice();
        if (!EnvText("IOS_EMULATOR_UI_PRODUCT").empty())
            app.ShowAddDevice(EnvText("IOS_EMULATOR_UI_PRODUCT"));
        if (EnvText("IOS_EMULATOR_UI_LANDSCAPE") == "1")
            app.SetLandscape();
        if (EnvText("IOS_EMULATOR_UI_DEVTOOLS") == "1")
            app.ShowDevTools(true);
        if (EnvText("IOS_EMULATOR_UI_EDITOR") == "1")
            app.ShowEditor();
        bool shotTaken = false, fullscreen = false;
        while (!app.Finished()) {
            // Idle politely: with no device running, wait for input (or a periodic refresh).
            if (!app.Busy())
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT)
                    app.RequestClose();
            }
            if (!drop.empty() && (NowNs() - started) / 1e9 > dropAfter) {
                PostDrop(hwnd, drop);
                drop.clear();
            }
            if (IsIconic(hwnd) && !app.Busy()) {
                Sleep(50);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Frame();
            if (app.Fullscreen() != fullscreen) {
                fullscreen = app.Fullscreen();
                SetFullscreen(hwnd, fullscreen);
            }
            ImGui::Render();
            glViewport(0, 0, g_width, g_height);
            glClearColor(0.12f, 0.13f, 0.15f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (!shot.empty() && !shotTaken && (NowNs() - started) / 1e9 > shotAfter) {
                SaveWindowPicture(fs::path(Widen(shot)), g_width, g_height);
                shotTaken = true;
                app.RequestClose();
            }
            SwapBuffers(dc);
        }
        g_app = nullptr;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gl);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return result;
}
