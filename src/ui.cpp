#include "ui.h"

#include "common.h"
#include "library.h"
#include "engine.h"
#include "firmware.h"
#include "session.h"

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

// The app's preferences, kept in <data>\settings.json.
struct Settings {
    bool internet = false;     // new sessions start with guest internet on
    bool webProxy = true;      // ...and HTTPS through the web proxy (proxy.h)
    bool headphones = false;   // route sound to the headphone jack
    bool muted = false;
    bool smooth = true;        // smooth scaling of the screen (off: sharp pixels)
    bool tilt = true;          // arrow keys tilt the device
    std::string selected;      // the device selected last

    static fs::path File() { return DataDir() / "settings.json"; }

    void Load() {
        try {
            Json j = Json::parse(LoadFile(File()));
            internet = j.value("internet", internet);
            webProxy = j.value("webProxy", webProxy);
            headphones = j.value("headphones", headphones);
            muted = j.value("muted", muted);
            smooth = j.value("smooth", smooth);
            tilt = j.value("tilt", tilt);
            selected = j.value("selected", selected);
        } catch (...) {
            // first start, or an unreadable file: keep the defaults
        }
    }

    void Save() const {
        Json j = {{"internet", internet}, {"webProxy", webProxy}, {"headphones", headphones}, {"muted", muted},
                  {"smooth", smooth},     {"tilt", tilt},         {"selected", selected}};
        SaveFile(File(), j.dump(2));
    }
};

// ---- Tilt ------------------------------------------------------------------------------------

// Leans the device with the arrow keys, as LightTouch does: each arrow lowers that edge up to 35
// degrees, easing there and back. Produces accelerometer samples in the guest's portrait axes
// (x right, y up, z toward you, m/s^2). At rest the device lies flat, face up.
class Tilt {
public:
    // Returns true when there's a new sample to send.
    bool Step(bool left, bool right, bool up, bool down, double acceleration[3]) {
        const uint64_t now = NowNs();
        const double dt = last_ ? std::min((now - last_) / 1e9, 0.1) : 0.0;
        last_ = now;
        const double maxAngle = 35.0 * 3.14159265 / 180, reach = 150.0 * 3.14159265 / 180 * dt;
        const double rollTo = (int(right) - int(left)) * maxAngle, pitchTo = (int(down) - int(up)) * maxAngle;
        auto approach = [&](double from, double to) {
            return std::abs(to - from) <= reach ? to : from + std::copysign(reach, to - from);
        };
        const double roll = approach(roll_, rollTo), pitch = approach(pitch_, pitchTo);
        const bool moving = roll != roll_ || pitch != pitch_ || roll != 0 || pitch != 0;
        roll_ = roll;
        pitch_ = pitch;
        const double g = 9.80665;
        acceleration[0] = -g * std::sin(roll);
        acceleration[1] = g * std::cos(roll) * std::sin(pitch);
        acceleration[2] = g * std::cos(roll) * std::cos(pitch);
        const bool send = moving || wasMoving_;   // one last sample once it's back at rest
        wasMoving_ = moving;
        return send;
    }

private:
    double roll_ = 0, pitch_ = 0;
    uint64_t last_ = 0;
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
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data());
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

fs::path PicturesDir() {
    PWSTR path = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
        dir = fs::path(path) / "iOS-Emulator";
    CoTaskMemFree(path);
    if (dir.empty())
        dir = DataDir() / "screenshots";
    fs::create_directories(dir);
    return dir;
}

// ---- The app ---------------------------------------------------------------------------------

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
    void StartSelected() {
        if (Device *device = Selected())
            StartDevice(*device);
    }
    bool Finished() const { return finished_; }
    bool Busy() const { return !sessions_.empty(); }   // something on screen keeps changing
    void ShowAddDevice() { page_ = Page::AddDevice; }

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
    void DeviceScreen(Session &session, ImVec2 area);
    void ScreenTouch(Session &session, ImVec2 origin, ImVec2 size, bool turned);
    void Controls(Session &session);
    void HardwareKeys(Session &session);
    void SaveScreenshot(Session &session);
    void SettingsPage();
    void AddDevicePage();
    void DeveloperPanel(Device &device, Session &session);
    void DevRequest(Session &session, Json command, std::function<void(const Json &)> onSuccess);
    void DevListFiles(const std::string &id, const std::string &path);
    void DevListApps(const std::string &id);
    void ClosingOverlay();

    Settings settings_;
    std::vector<Device> devices_;
    Page page_ = Page::Device;
    std::map<std::string, std::unique_ptr<Session>> sessions_;   // running devices by id
    std::map<std::string, Texture> textures_;
    std::string error_, notice_;
    bool closing_ = false, finished_ = false;

    // Input state for the device on screen.
    bool touching_ = false, twoFingers_ = false;
    ImVec2 lastTouch_{-1, -1};
    std::set<int> heldButtons_;
    Tilt tilt_;
    std::string typeText_, renameText_;

    // The Add Device page.
    std::unique_ptr<Preparation> preparation_;
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
        error_.clear();
    } catch (const std::exception &e) {
        error_ = std::string("Couldn't start ") + device.name + ": " + e.what();
    }
}

void App::UpdateSessions() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Session &session = *it->second;
        session.Update();
        if (session.Exited()) {
            if (!session.ExitedCleanly())
                error_ = session.GetDevice().name + ": " + session.ExitMessage();
            textures_[it->first].Release();
            textures_.erase(it->first);
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
    }
    ImGui::Separator();
    if (session)
        RunningDeviceView(device, *session);
    else
        StoppedDeviceView(device);
}

void App::StoppedDeviceView(Device &device) {
    ImGui::Spacing();
    if (device.Dirty())
        ImGui::TextColored(kWarn, "Last time this device stopped without shutting iOS down. iOS will check its storage "
                                  "as it starts.");
    if (ImGui::Button("Start", ImVec2(160, 0)))
        StartDevice(device);
    ImGui::SameLine();
    DimText(settings_.internet ? "with internet" : "without internet (change it in Settings)");

    ImGui::Dummy(ImVec2(0, 20));
    DimText("MANAGE");
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

void App::RunningDeviceView(Device &device, Session &session) {
    // Left: the device and its controls. Right: developer tools.
    const float screenColumn = std::max(ImGui::GetContentRegionAvail().x * 0.48f, 360.0f);
    ImGui::BeginChild("device", ImVec2(screenColumn, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float controlsHeight = ImGui::GetFrameHeightWithSpacing() * 3 + 12;
    DeviceScreen(session, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - controlsHeight));
    Controls(session);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("developer", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DeveloperPanel(device, session);
    ImGui::EndChild();
    HardwareKeys(session);
}

void App::DeviceScreen(Session &session, ImVec2 area) {
    Texture &texture = textures_[session.GetDevice().id];
    if (session.UpdateFrame())
        texture.Upload(session.FramePixels(), session.FrameWidth(), session.FrameHeight(), settings_.smooth);

    // A panel wider than tall (the iPad's) scans a quarter-turn from upright, so it's shown turned.
    float sw = float(texture.width ? texture.width : session.screenWidth);
    float sh = float(texture.height ? texture.height : session.screenHeight);
    const bool turned = sw > sh;
    if (turned)
        std::swap(sw, sh);
    // Fit a device-shaped bezel (screen plus room for the Home button) into the area.
    const float side = 0.06f * sw, top = 0.16f * sh * sw / sh, bottom = 0.20f * sw;
    const float scale = std::max(0.1f, std::min(area.x / (sw + 2 * side), area.y / (sh + top + bottom)));
    const ImVec2 bezelSize((sw + 2 * side) * scale, (sh + top + bottom) * scale);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 bezel(cursor.x + (area.x - bezelSize.x) / 2, cursor.y);
    const ImVec2 screen(bezel.x + side * scale, bezel.y + top * scale);
    const ImVec2 screenSize(sw * scale, sh * scale);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(bezel, ImVec2(bezel.x + bezelSize.x, bezel.y + bezelSize.y), IM_COL32(24, 24, 27, 255),
                        28 * scale);
    draw->AddRect(bezel, ImVec2(bezel.x + bezelSize.x, bezel.y + bezelSize.y), IM_COL32(70, 70, 76, 255),
                  28 * scale, 0, 2);
    const ImVec2 screenEnd(screen.x + screenSize.x, screen.y + screenSize.y);
    if (texture.id && !session.sleeping && turned)   // each corner shows the pixel the turn puts there
        draw->AddImageQuad(ImTextureID(intptr_t(texture.id)), screen, ImVec2(screenEnd.x, screen.y), screenEnd,
                           ImVec2(screen.x, screenEnd.y), ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1));
    else if (texture.id && !session.sleeping)
        draw->AddImage(ImTextureID(intptr_t(texture.id)), screen, screenEnd);
    else
        draw->AddRectFilled(screen, screenEnd, IM_COL32(0, 0, 0, 255));
    if (!texture.id || session.sleeping) {
        const char *text = !texture.id ? "Starting iOS..." : "Display off: click to wake";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(screen.x + (screenSize.x - size.x) / 2, screen.y + (screenSize.y - size.y) / 2),
                      IM_COL32(200, 200, 200, 255), text);
    }
    ScreenTouch(session, screen, screenSize, turned);

    // The Home button: a circle under the screen you can click (H on the keyboard does the same).
    const float radius = bottom * scale * 0.3f;
    const ImVec2 home(screen.x + screenSize.x / 2, screenEnd.y + bottom * scale / 2);
    ImGui::SetCursorScreenPos(ImVec2(home.x - radius, home.y - radius));
    ImGui::InvisibleButton("home", ImVec2(radius * 2, radius * 2));
    if (ImGui::IsItemActivated())
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", true}});
    if (ImGui::IsItemDeactivated())
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", false}});
    Tooltip("Home (H)");
    draw->AddCircle(home, radius, ImGui::IsItemHovered() ? IM_COL32(150, 150, 160, 255) : IM_COL32(90, 90, 96, 255),
                    0, 2);
    draw->AddRect(ImVec2(home.x - radius * 0.38f, home.y - radius * 0.38f),
                  ImVec2(home.x + radius * 0.38f, home.y + radius * 0.38f), IM_COL32(90, 90, 96, 255), 3 * scale);
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + area.y));
}

// The mouse is a finger: press, drag and release on the screen. Hold Alt while dragging for a
// second, mirrored finger (pinch and rotate), on boards with multi-touch.
void App::ScreenTouch(Session &session, ImVec2 origin, ImVec2 size, bool turned) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("screen", size);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float x = std::clamp((mouse.x - origin.x) / size.x, 0.0f, 1.0f);
    const float y = std::clamp((mouse.y - origin.y) / size.y, 0.0f, 1.0f);
    // The engine wants panel coordinates: on a turned panel, (x, y) on screen is (y, 1 - x) there.
    auto contact = [&](int id, int phase, float u, float v) {
        return Json{{"id", id}, {"phase", phase}, {"x", turned ? v : u}, {"y", turned ? 1 - u : v}};
    };
    auto send = [&](int phase) {
        Json contacts = Json::array({contact(1, phase, x, y)});
        if (twoFingers_)
            contacts.push_back(contact(2, phase, 1 - x, 1 - y));
        session.Send({{"cmd", "touch"}, {"contacts", contacts}});
        lastTouch_ = ImVec2(x, y);
    };
    // While iOS has the display off, its touch panel is off too: a click wakes it with Home instead.
    if (ImGui::IsItemActivated() && session.sleeping) {
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", true}});
        session.Send({{"cmd", "button"}, {"button", QEMU_IOS_BUTTON_HOME}, {"down", false}});
    } else if (ImGui::IsItemActivated()) {
        touching_ = true;
        twoFingers_ = ImGui::GetIO().KeyAlt && (session.capabilities & QEMU_IOS_CAP_MULTITOUCH);
        send(QEMU_IOS_TOUCH_BEGIN);
    } else if (touching_ && ImGui::IsItemActive()) {
        if (x != lastTouch_.x || y != lastTouch_.y)
            send(QEMU_IOS_TOUCH_UPDATE);
    } else if (touching_) {
        send(QEMU_IOS_TOUCH_END);
        touching_ = false;
    }
    if (twoFingers_ && touching_) {   // show where the second finger is
        ImGui::GetWindowDrawList()->AddCircle(ImVec2(origin.x + (1 - x) * size.x, origin.y + (1 - y) * size.y), 10,
                                              IM_COL32(255, 255, 255, 160), 0, 2);
    }
}

// H: Home, P: Sleep/Wake, + and -: volume (held while the key is), arrows: tilt.
void App::HardwareKeys(Session &session) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool typing = io.WantTextInput;
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeyShift;
    auto key = [&](ImGuiKey k) { return !typing && plain && ImGui::IsKeyDown(k); };
    const std::pair<bool, int> buttons[] = {
        {key(ImGuiKey_H), QEMU_IOS_BUTTON_HOME},
        {key(ImGuiKey_P), QEMU_IOS_BUTTON_POWER},
        {key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd), QEMU_IOS_BUTTON_VOLUME_UP},
        {key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract), QEMU_IOS_BUTTON_VOLUME_DOWN},
    };
    for (const auto &[down, button] : buttons) {
        if (down == bool(heldButtons_.count(button)))
            continue;
        if (down)
            heldButtons_.insert(button);
        else
            heldButtons_.erase(button);
        session.Send({{"cmd", "button"}, {"button", button}, {"down", down}});
    }
    if (settings_.tilt) {
        double a[3];
        if (tilt_.Step(key(ImGuiKey_LeftArrow), key(ImGuiKey_RightArrow), key(ImGuiKey_UpArrow),
                       key(ImGuiKey_DownArrow), a))
            session.Send({{"cmd", "motion"}, {"x", a[0]}, {"y", a[1]}, {"z", a[2]}});
    }
}

void App::Controls(Session &session) {
    const bool ready = session.state == "running" || session.state == "paused";
    auto pressButton = [&](const char *label, int button, const char *tip) {
        ImGui::Button(label);
        if (ImGui::IsItemActivated())
            session.Send({{"cmd", "button"}, {"button", button}, {"down", true}});
        if (ImGui::IsItemDeactivated())
            session.Send({{"cmd", "button"}, {"button", button}, {"down", false}});
        Tooltip(tip);
        ImGui::SameLine();
    };
    ImGui::BeginDisabled(!ready);
    pressButton("Home", QEMU_IOS_BUTTON_HOME, "Home button (H)");
    pressButton("Sleep/Wake", QEMU_IOS_BUTTON_POWER, "Sleep/Wake button (P). Hold it to power off.");
    pressButton("Vol -", QEMU_IOS_BUTTON_VOLUME_DOWN, "Volume down (-)");
    pressButton("Vol +", QEMU_IOS_BUTTON_VOLUME_UP, "Volume up (+)");
    if (ImGui::Button(session.paused ? "Resume" : "Pause"))
        session.Send({{"cmd", "pause"}, {"paused", !session.paused}});
    ImGui::EndDisabled();

    bool sound = !session.muted;
    if (ImGui::Checkbox("Sound", &sound)) {
        session.muted = !sound;
        session.Send({{"cmd", "mute"}, {"muted", session.muted}});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    if (ImGui::Checkbox("Internet", &session.internet))
        session.Send({{"cmd", "internet"}, {"on", session.internet}});
    Tooltip("Lets iOS reach the internet through your connection (Cydia needs it).");
    ImGui::SameLine();
    if (ImGui::Checkbox("Web proxy", &session.webProxy))
        session.Send({{"cmd", "webproxy"}, {"on", session.webProxy}});
    const std::string proxyTip =
        std::string(kWebProxyTip) + (session.proxyStatus.empty() ? "" : "\n\n" + session.proxyStatus);
    Tooltip(proxyTip.c_str());
    if (session.proxyError) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "(!)");
        Tooltip(session.proxyStatus.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot"))
        SaveScreenshot(session);
    ImGui::SameLine();
    ImGui::EndDisabled();
    if (Button("Stop", session.state != "stopping"))
        session.Stop();
    Tooltip("Shuts iOS down cleanly, like sliding to power off.");

    // Typing: the text goes to the field that has focus in iOS.
    ImGui::BeginDisabled(!ready || session.agent != 1);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Type").x - ImGui::GetStyle().FramePadding.x * 2 - 8);
    bool enter = ImGui::InputTextWithHint("##type", "Type into iOS (Enter sends)", &typeText_,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Type") || enter) && !typeText_.empty()) {
        session.Send({{"cmd", "type"}, {"text", typeText_}});
        typeText_.clear();
    }
    ImGui::EndDisabled();
}

void App::SaveScreenshot(Session &session) {
    if (session.FrameWidth() == 0)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d.%02d.%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    const fs::path path = PicturesDir() / Widen(session.GetDevice().name + " " + stamp + ".png");
    try {
        WritePng(path, session.FramePixels().data(), session.FrameWidth(), session.FrameHeight());
        notice_ = "Saved " + PathText(path);
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
    if (changed)
        settings_.Save();

    ImGui::Spacing();
    DimText("CONTROLS");
    ImGui::BulletText("Click and drag on the screen to touch. Hold Alt while dragging to pinch or rotate.");
    ImGui::BulletText("H: Home, P: Sleep/Wake, + and -: volume, arrow keys: tilt.");
    ImGui::BulletText("Type text into iOS with the box under the device.");
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

void App::AddDevicePage() {
    ImGui::Text("Add a device");
    ImGui::Separator();

    // A preparation in progress (or just finished) takes over the page.
    if (preparation_) {
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
            Reload();
            settings_.selected = status.deviceId;
            settings_.Save();
            page_ = Page::Device;
            notice_ = "The new device is ready. Press Start.";
        }
        return;
    }

    const std::vector<Firmware> &catalog = Catalog();
    const float listWidth = ImGui::GetFontSize() * 11;

    // Column 1: the device.
    ImGui::BeginChild("products", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    DimText("DEVICE");
    std::string previous;
    for (const auto &firmware : catalog) {
        if (firmware.productType == previous)
            continue;
        previous = firmware.productType;
        if (ImGui::Selectable((firmware.Product() + "##" + firmware.productType).c_str(),
                              chosenProduct_ == firmware.productType)) {
            chosenProduct_ = firmware.productType;
            // Preselect a tested version if there is one, else the newest.
            chosenFirmware_.clear();
            for (const auto &f : catalog)
                if (f.productType == chosenProduct_ && (chosenFirmware_.empty() || !FindFirmware(chosenFirmware_)->Tested()))
                    chosenFirmware_ = f.id;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Column 2: its iOS versions.
    const float versionsWidth = listWidth * 1.3f;
    ImGui::BeginChild("versions", ImVec2(versionsWidth, 0), ImGuiChildFlags_Borders);
    DimText("iOS VERSION");
    for (const auto &firmware : catalog) {
        if (firmware.productType != chosenProduct_)
            continue;
        const std::string label = firmware.label + "  (" + firmware.build + ")##" + firmware.id;
        if (ImGui::Selectable(label.c_str(), chosenFirmware_ == firmware.id))
            chosenFirmware_ = firmware.id;
        if (firmware.Tested()) {
            ImGui::SameLine(versionsWidth - ImGui::CalcTextSize("tested").x - 24);
            ImGui::TextColored(kGood, "tested");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Column 3: details and options.
    ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const Firmware *firmware = FindFirmware(chosenFirmware_); !firmware || firmware->productType != chosenProduct_) {
        DimText("Pick a device and an iOS version.");
    } else {
        ImGui::Text("%s, iOS %s", firmware->Product().c_str(), firmware->label.c_str());
        DimText("Build " + firmware->build + ", released " + firmware->released);
        if (firmware->Tested())
            ImGui::TextColored(kGood, "Runs well (tested by LightTouch).");
        else if (firmware->board == "n45ap" || firmware->board == "m68ap")
            ImGui::TextColored(kWarn, "Experimental: this device currently stops at a white screen after setup.");
        else
            ImGui::TextColored(kWarn, "Not tested yet: it may or may not boot.");
        ImGui::Spacing();
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
        if (ImGui::Button("Prepare", ImVec2(ImGui::GetFontSize() * 9, 0))) {
            PrepareRequest request;
            request.firmware = *firmware;
            request.name = newName_;
            request.jailbreak = jailbreak;
            request.ipsw = localIpsw_;
            preparation_ = std::make_unique<Preparation>(request);
            newName_.clear();
        }
    }

    // Devices LightTouch already made can be brought over as they are.
    const std::vector<fs::path> lightTouch = LightTouchDevices(devices_);
    if (!lightTouch.empty()) {
        ImGui::Dummy(ImVec2(0, 16));
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
        Tooltip("Unsigned apps work too: AppSync is part of every prepared device.");
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

// ---- Window and OpenGL -----------------------------------------------------------------------

// For testing the UI where nobody watches the screen: IOS_EMULATOR_UI_START=1 starts the selected
// device, IOS_EMULATOR_UI_PAGE=add opens the Add Device page, and IOS_EMULATOR_UI_SHOT=<file.png> saves the window after IOS_EMULATOR_UI_SHOT_AFTER
// seconds (default 5) and closes the app.
std::string EnvText(const char *name) {
    const char *value = getenv(name);
    return value ? value : "";
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
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * scale);
    SetupStyle(scale);
    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init();

    int result = 0;
    {
        App app;
        g_app = &app;
        const std::string shot = EnvText("IOS_EMULATOR_UI_SHOT");
        const double shotAfter = EnvText("IOS_EMULATOR_UI_SHOT_AFTER").empty() ? 5.0 : atof(EnvText("IOS_EMULATOR_UI_SHOT_AFTER").c_str());
        const uint64_t started = NowNs();
        if (EnvText("IOS_EMULATOR_UI_START") == "1")
            app.StartSelected();
        if (EnvText("IOS_EMULATOR_UI_PAGE") == "add")
            app.ShowAddDevice();
        bool shotTaken = false;
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
            if (IsIconic(hwnd) && !app.Busy()) {
                Sleep(50);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Frame();
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
