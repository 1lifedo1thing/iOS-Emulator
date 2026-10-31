// iOS-Emulator: runs classic iOS devices (iPhone OS 1 to iOS 7) on Windows, using the qemu-ios
// engine. A C++ take on LightTouch (https://github.com/blake-lucas/LightTouch).
//
// Usage:
//   iOS-Emulator.exe                       the app window
//   iOS-Emulator.exe --list                lists the devices in the library
//   iOS-Emulator.exe --run <device> [--seconds N] [--screenshot file.png] [--internet]
//                    [--tap x,y,t ...] [--swipe x,y,toX,toY,t[,seconds] ...] [--request t json ...]
//                                          boots a device without a window (for testing and scripts);
//                                          --tap/--swipe touch the screen (coordinates 0..1) t seconds after start;
//                                          --request sends a developer command (worker.h) and prints the reply
//   iOS-Emulator.exe --catalog [product]   lists the firmwares that can be prepared
//   iOS-Emulator.exe --prepare <firmware id> [--name N] [--jailbreak] [--ipsw file]
//                                          prepares a new device (e.g. --prepare n18ap-8C148 --jailbreak)
//   iOS-Emulator.exe --import <folder>     copies a LightTouch device folder into the library
//   iOS-Emulator.exe --worker ...          internal: runs one device (started by the app)
#include "common.h"
#include "firmware.h"
#include "library.h"
#include "session.h"
#include "ui.h"
#include "worker.h"

#include <shellapi.h>

#include <thread>

using namespace std::chrono_literals;

// Console output for the command-line modes (this is a windowed program, so attach to the
// console that started it, if any).
static void Print(const std::string &text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteFile(out, text.data(), DWORD(text.size()), &written, nullptr);
    WriteFile(out, "\n", 1, &written, nullptr);
}

static void UseParentConsole() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN)
        return;   // output is already redirected to a file or pipe
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    HANDLE console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    SetStdHandle(STD_OUTPUT_HANDLE, console);
    SetStdHandle(STD_ERROR_HANDLE, console);
}

static int ListCommand() {
    for (const auto &device : ListDevices())
        Print(device.id + "  " + device.name + "  (" + device.Product() + ", iOS " + device.Version() +
              (device.Jailbroken() ? ", jailbroken" : "") + ")");
    return 0;
}

// Boots a device without a window, optionally saves the last frame, then shuts it down cleanly.
static int RunCommand(const std::vector<std::string> &args) {
    // A tap, or a swipe from (x, y) to (toX, toY) lasting `seconds`, `at` seconds after the start.
    struct Gesture {
        double x, y, toX, toY, at, seconds;
    };
    std::string name, screenshot;
    std::vector<Gesture> gestures;
    std::vector<std::pair<double, Json>> requests;   // developer commands to send, by time
    int seconds = 60;
    SessionOptions options;
    for (size_t i = 0; i < args.size(); i++) {
        Gesture g{};
        if (args[i] == "--tap" && i + 1 < args.size() && sscanf(args[++i].c_str(), "%lf,%lf,%lf", &g.x, &g.y, &g.at) == 3)
            gestures.push_back({g.x, g.y, g.x, g.y, g.at, 0});
        else if (args[i] == "--swipe" && i + 1 < args.size() &&
                 sscanf(args[++i].c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &g.x, &g.y, &g.toX, &g.toY, &g.at, &g.seconds) >= 5)
            gestures.push_back({g.x, g.y, g.toX, g.toY, g.at, g.seconds > 0 ? g.seconds : 1.0});
        else if (args[i] == "--request" && i + 2 < args.size()) {
            const double at = atof(args[++i].c_str());
            requests.push_back({at, Json::parse(args[++i])});
        } else if (args[i] == "--seconds" && i + 1 < args.size())
            seconds = std::stoi(args[++i]);
        else if (args[i] == "--screenshot" && i + 1 < args.size())
            screenshot = args[++i];
        else if (args[i] == "--internet")
            options.internet = true;
        else
            name = args[i];
    }
    Device device = FindDevice(name);
    Print("Starting " + device.name + " (" + device.Product() + ", iOS " + device.Version() + ")");
    Session session(device, options);
    std::string lastState;
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    while (!session.Exited()) {
        session.Update();
        session.UpdateFrame();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (auto it = gestures.begin(); it != gestures.end();) {
            if (elapsed < it->at) {
                ++it;
                continue;
            }
            // A sleeping display ignores touches (iOS turns the panel off): wake it with Home first.
            if (session.sleeping) {
                for (bool down : {true, false}) {
                    session.Send({{"cmd", "button"}, {"button", 0}, {"down", down}});
                    std::this_thread::sleep_for(150ms);
                }
                std::this_thread::sleep_for(1500ms);
                Print("Woke the display");
            }
            // Finger down, a move every 40 ms along the way (none for a tap), finger up.
            const int steps = it->seconds > 0 ? std::max(2, int(it->seconds / 0.04)) : 0;
            for (int step = 0; step <= steps + 1; step++) {
                const double t = steps ? std::min(1.0, double(step) / steps) : 0;
                const int phase = step == 0 ? 0 : step == steps + 1 ? 2 : 1;
                session.Send({{"cmd", "touch"},
                              {"contacts", {{{"id", 1}, {"phase", phase}, {"x", it->x + (it->toX - it->x) * t},
                                             {"y", it->y + (it->toY - it->y) * t}}}}});
                // Like a finger: rest a moment after touching down and before lifting off.
                std::this_thread::sleep_for(steps && (step == 0 || step == steps) ? 200ms : 40ms);
            }
            Print(steps ? "Swiped" : "Tapped");
            it = gestures.erase(it);
        }
        for (auto it = requests.begin(); it != requests.end();) {
            if (elapsed < it->first) {
                ++it;
                continue;
            }
            session.Request(it->second, [](const Json &reply) { Print(reply.dump(2)); });
            it = requests.erase(it);
        }
        if (session.state != lastState) {
            lastState = session.state;
            Print("State: " + lastState);
        }
        if (std::chrono::steady_clock::now() > end && session.state != "stopping" && session.state != "stopped") {
            if (!screenshot.empty() && session.FrameWidth() > 0) {
                WritePng(fs::path(Widen(screenshot)), session.FramePixels().data(), session.FrameWidth(),
                         session.FrameHeight());
                Print("Saved " + screenshot);
            }
            Print("Stopping");
            session.Stop();
        }
        std::this_thread::sleep_for(50ms);
    }
    session.Update();
    if (!session.syslog.empty()) {   // the end of the device log, if it was captured
        Print("Last lines of the system log:");
        for (size_t i = session.syslog.size() > 10 ? session.syslog.size() - 10 : 0; i < session.syslog.size(); i++)
            Print("  " + session.syslog[i]);
    }
    Print(session.ExitMessage());
    return session.ExitedCleanly() ? 0 : 1;
}

static int CatalogCommand(const std::vector<std::string> &args) {
    for (const auto &firmware : Catalog())
        if (args.empty() || firmware.productType == args[0] || firmware.board == args[0])
            Print(firmware.id + "  " + firmware.Product() + " iOS " + firmware.label +
                  (firmware.Tested() ? "  (tested)" : ""));
    return 0;
}

static int PrepareCommand(const std::vector<std::string> &args) {
    PrepareRequest request;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--name" && i + 1 < args.size())
            request.name = args[++i];
        else if (args[i] == "--ipsw" && i + 1 < args.size())
            request.ipsw = fs::path(Widen(args[++i]));
        else if (args[i] == "--jailbreak")
            request.jailbreak = true;
        else if (const Firmware *firmware = FindFirmware(args[i]))
            request.firmware = *firmware;
    }
    if (request.firmware.id.empty())
        throw std::runtime_error("Name a firmware from --catalog, e.g. n18ap-8C148");
    if (request.jailbreak && !request.firmware.JailbreakUnavailable().empty())
        throw std::runtime_error(request.firmware.JailbreakUnavailable());
    Preparation preparation(request);
    std::string lastStep;
    int lastPercent = -1;
    for (;;) {
        Preparation::Status status = preparation.GetStatus();
        const int percent = int(status.fraction * 100);
        if (status.step != lastStep && (percent != lastPercent || status.step.find("Downloading") == std::string::npos)) {
            Print("[" + std::to_string(percent) + "%] " + status.step);
            lastStep = status.step;
            lastPercent = percent;
        }
        if (status.finished) {
            Print(status.failed ? "Failed: " + status.error : "Prepared device " + status.deviceId);
            return status.failed ? 1 : 0;
        }
        std::this_thread::sleep_for(500ms);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t **argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
        args.push_back(Narrow(argvW[i]));
    LocalFree(argvW);

    const std::string mode = args.empty() ? "" : args[0];
    const std::vector<std::string> rest = args.empty() ? args : std::vector<std::string>(args.begin() + 1, args.end());
    if (mode == "--worker") {
        // The USB libraries (delay-loaded) and their dependencies live in the resources folder.
        SetDllDirectoryW(ResourcesDir().c_str());
        return RunWorker(rest);
    }   // stdout and stderr already go to the worker's log file

    if (mode.empty())
        return RunUi();
    UseParentConsole();
    try {
        if (mode == "--list")
            return ListCommand();
        if (mode == "--run" && !rest.empty())
            return RunCommand(rest);
        if (mode == "--catalog")
            return CatalogCommand(rest);
        if (mode == "--prepare")
            return PrepareCommand(rest);
        if (mode == "--import" && rest.size() == 1) {
            Device device = ImportDevice(fs::path(Widen(rest[0])));
            Print("Imported " + device.name + " as " + device.id);
            return 0;
        }
        Print("usage: iOS-Emulator [--list | --run <device> [--seconds N] [--screenshot F] [--internet] | "
              "--catalog | --prepare <firmware> [--jailbreak] | --import <folder>]");
        return 64;
    } catch (const std::exception &e) {
        Print(std::string("Error: ") + e.what());
        return 1;
    }
}
