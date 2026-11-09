#include "session.h"
#include "engine.h"

#include <condition_variable>

using namespace std::chrono_literals;

static fs::path LogsDir() {
    fs::path dir = DataDir() / "logs";
    fs::create_directories(dir);
    return dir;
}

Session::Session(const Device &device, const SessionOptions &options) : device_(device) {
    muted = options.muted;
    internet = options.internet;
    webProxy = options.webProxy;
    if (!options.loadState.empty())
        state = "loading";
    StartUsbmuxd();
    try {
        StartWorker(options);
    } catch (...) {
        StopUsbmuxd();
        throw;
    }
}

Session::~Session() {
    Kill();
    if (eventReader_.joinable())
        eventReader_.join();   // ends when the worker's end of the pipe closes
    if (eventsRead_)
        CloseHandle(eventsRead_);
    StopUsbmuxd();
    if (!throwawayState_.empty()) {   // the engine keeps its files open until the worker has exited
        std::error_code ignored;
        fs::remove_all(throwawayState_, ignored);
    }
}

fs::path Session::WorkerLog() const { return LogsDir() / ("worker-" + device_.id.substr(0, 8) + ".log"); }

// lighttouch-usbmuxd.exe <pairing folder>: once it listens, it prints
//   DAEMON {"format":"lighttouch-windows-usbmux","pid":..,"created":"..","client_port":..,"guest_port":..,"token":..}
// The guest's USB port connects to guest_port; libusbmuxd clients connect to client_port.
// It exits when we write "stop" to its stdin (or the pipe closes).
void Session::StartUsbmuxd() {
    const fs::path pairing = DataDir() / "pairing" / device_.id;
    fs::create_directories(pairing);
    const std::string token = RandomHex(16);
    LaunchOptions options;
    options.args = {pairing.wstring()};
    options.env = {{"LTW_SESSION_PROFILE", device_.id}, {"LTW_SESSION_TOKEN", token}, {"LTW_SESSION_GENERATION", "1"}};
    options.removeEnv = {"IT_USB_TCP", "USBMUXD_SOCKET_ADDRESS", "USBMUXD_QEMU_ADDR"};
    options.pipeStdin = true;
    options.pipeStdout = true;
    options.log = LogsDir() / ("usbmuxd-" + device_.id.substr(0, 8) + ".log");
    fs::remove(options.log);
    usbmuxd_ = ChildProcess::Launch(ResourcesDir() / "lighttouch-usbmuxd.exe", options);

    // Wait up to 8 s for its DAEMON line. A helper thread reads its output (reading a pipe blocks)
    // and keeps draining it afterwards, so the daemon never stalls on a full pipe.
    struct Announcement {
        std::mutex mutex;
        std::condition_variable ready;
        std::string line;
        bool ended = false;
    };
    auto announcement = std::make_shared<Announcement>();
    usbmuxdDrain_ = std::thread([announcement, this] {
        std::string line;
        while (usbmuxd_.ReadLine(line)) {
            std::lock_guard<std::mutex> lock(announcement->mutex);
            if (announcement->line.empty() && line.rfind("DAEMON ", 0) == 0) {
                announcement->line = line.substr(7);
                announcement->ready.notify_one();
            }
        }
        std::lock_guard<std::mutex> lock(announcement->mutex);
        announcement->ended = true;
        announcement->ready.notify_one();
    });
    std::string daemonLine;
    {
        std::unique_lock<std::mutex> lock(announcement->mutex);
        announcement->ready.wait_for(lock, 8s, [&] { return !announcement->line.empty() || announcement->ended; });
        daemonLine = announcement->line;
    }
    if (daemonLine.empty()) {
        StopUsbmuxd();
        throw std::runtime_error("The USB helper (lighttouch-usbmuxd.exe) didn't start; see " +
                                 PathText(options.log));
    }
    Json daemon = Json::parse(daemonLine);
    if (daemon.value("format", "") != "lighttouch-windows-usbmux" || daemon.value("token", "") != token)
        throw std::runtime_error("The USB helper answered with something unexpected");
    usbmuxClientPort_ = std::to_string(daemon.at("client_port").get<int>());
    usbmuxGuestPort_ = std::to_string(daemon.at("guest_port").get<int>());
    usbmuxPid_ = std::to_string(daemon.at("pid").get<unsigned long>());
    usbmuxCreated_ = daemon.at("created").get<std::string>();
}

void Session::StopUsbmuxd() {
    if (usbmuxd_.Running()) {
        usbmuxd_.WriteLine("stop");
        usbmuxd_.CloseStdin();
        // Once the guest has connected, the daemon can wait out a 30 s USB read; don't wait that long.
        if (usbmuxd_.Wait(1500) < 0)
            usbmuxd_.Kill();
    }
    if (usbmuxdDrain_.joinable())
        usbmuxdDrain_.join();
}

void Session::StartWorker(const SessionOptions &options) {
    const std::string framesName = "Local\\iOS-Emulator-" + device_.id + "-" + RandomHex(4);
    frames_.Create(framesName);

    // The worker writes events into this pipe. Only its write end is inheritable.
    SECURITY_ATTRIBUTES inheritable{sizeof inheritable, nullptr, TRUE};
    HANDLE eventsWrite = nullptr;
    if (!CreatePipe(&eventsRead_, &eventsWrite, &inheritable, 0))
        throw std::runtime_error("Couldn't create the worker's event pipe");
    SetHandleInformation(eventsRead_, HANDLE_FLAG_INHERIT, 0);

    LaunchOptions launch;
    launch.args = {L"--worker", L"--device", device_.dir.wstring(), L"--frames", Widen(framesName), L"--events",
                   std::to_wstring(reinterpret_cast<uintptr_t>(eventsWrite))};
    if (options.internet)
        launch.args.push_back(L"--internet");
    if (options.webProxy)
        launch.args.push_back(L"--web-proxy");
    if (options.headphones)
        launch.args.push_back(L"--headphones");
    if (options.muted)
        launch.args.push_back(L"--muted");
    if (!options.snapshot.empty()) {   // the worker runs on a throwaway copy, deleted once it has exited
        throwawayState_ = fs::temp_directory_path() / ("ios-emulator-" + device_.id.substr(0, 8) + "-" + RandomHex(4));
        launch.args.insert(launch.args.end(),
                           {L"--snapshot", options.snapshot.wstring(), L"--state", throwawayState_.wstring()});
    } else if (!options.loadState.empty()) {
        launch.args.insert(launch.args.end(), {L"--load-state", options.loadState.wstring()});
    }
    if (options.saveSnapshots && HasSaveStates(device_.board))   // the iPhone 4S can't save state yet
        launch.args.push_back(L"--save-snapshots");
    // The engine and the USB libraries read these with getenv(), once, so they go in the
    // environment the worker starts with.
    launch.env = {
        {"IT_TVOUT_READY", "1"},                                   // the engine presents frames to us
        {"IT_USB_TCP", "127.0.0.1:" + usbmuxGuestPort_},           // where the guest's USB port connects
        {"USBMUXD_SOCKET_ADDRESS", "127.0.0.1:" + usbmuxClientPort_},   // where libusbmuxd finds the daemon
        {"LTW_USBMUX_PID", usbmuxPid_},                            // ...and how it checks it's the right one
        {"LTW_USBMUX_CREATED", usbmuxCreated_},
    };
    launch.pipeStdin = true;
    launch.log = WorkerLog();
    launch.inherit = {eventsWrite};
    fs::remove(launch.log);
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    try {
        worker_ = ChildProcess::Launch(exe, launch);
    } catch (...) {
        CloseHandle(eventsWrite);
        throw;
    }
    CloseHandle(eventsWrite);   // now only the worker holds it, so the pipe ends when the worker does
    eventReader_ = std::thread(&Session::ReadEvents, this);
}

void Session::ReadEvents() {
    std::string pending, line;
    while (ReadLineFrom(eventsRead_, pending, line)) {
        try {
            Json event = Json::parse(line);
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox_.push_back(std::move(event));
        } catch (const std::exception &) {
            Log("Unreadable worker event: %s", line.c_str());
        }
    }
    std::lock_guard<std::mutex> lock(inboxMutex_);
    eventsEnded_ = true;
}

void Session::Send(const Json &command) {
    if (worker_.Running())
        worker_.WriteLine(command.dump());
}

void Session::Request(Json command, std::function<void(const Json &)> onReply) {
    const uint64_t id = ++nextRequest_;
    command["id"] = id;
    pending_[id] = std::move(onReply);
    if (!worker_.Running()) {
        HandleEvent({{"event", "reply"}, {"id", id}, {"ok", false}, {"error", "The device isn't running"}});
        return;
    }
    Send(command);
}

void Session::Stop() {
    if (exited_ || Stopping())
        return;
    state = "stopping";
    Send({{"cmd", "stop"}});
    // The worker waits for a booting guest, then up to ~55 s for iOS to halt.
    stopDeadline_ = std::chrono::steady_clock::now() + 3min;
}

void Session::Kill() {
    if (worker_.Running()) {
        worker_.Kill();
        worker_.Wait(5000);
    }
}

void Session::ForceOff() {
    forcedOff_ = true;
    Kill();
}

void Session::Update() {
    std::deque<Json> events;
    bool ended;
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        events.swap(inbox_);
        ended = eventsEnded_;
    }
    for (const auto &event : events)
        HandleEvent(event);
    if (ended && !exited_) {   // the worker ended without saying goodbye: it crashed or was killed
        exited_ = true;
        exitClean_ = forcedOff_;   // what the user asked for is no failure, though iOS didn't shut down
        if (!forcedOff_)
            exitMessage_ = "The device stopped unexpectedly. Details are in " + PathText(WorkerLog());
    }
    if (exited_) {
        state = "stopped";
        auto unanswered = std::move(pending_);   // a callback may send a new request
        pending_.clear();
        for (auto &[id, callback] : unanswered)
            callback({{"event", "reply"}, {"id", id}, {"ok", false}, {"error", "The device stopped"}});
        StopUsbmuxd();
    } else if (std::chrono::steady_clock::now() > stopDeadline_) {
        Log("The device didn't stop in time; ending it");
        Kill();
    }
}

void Session::HandleEvent(const Json &event) {
    const std::string kind = event.value("event", "");
    if (kind == "board") {
        machine = event.value("machine", "");
        screenWidth = event.value("width", 320);
        screenHeight = event.value("height", 480);
        capabilities = event.value("capabilities", uint64_t(0));
        maxContacts = event.value("maxContacts", 1);
    } else if (kind == "status") {
        state = event.value("state", state);
        paused = state == "paused";
        sleeping = event.value("sleeping", false);
        agent = event.value("agent", 0);
    } else if (kind == "reply" && event.contains("id") && event["id"].is_number_unsigned()) {
        auto found = pending_.find(event["id"].get<uint64_t>());
        if (found != pending_.end()) {
            auto callback = std::move(found->second);
            pending_.erase(found);
            callback(event);
        }
    } else if (kind == "proxy") {
        proxyStatus = event.value("status", "");
        proxyError = event.value("error", false);
    } else if (kind == "typing") {
        typingProblem = event.value("message", "");
    } else if (kind == "recording") {
        recording = event.value("on", false);
        recordingSince = NowNs();
        recordingSaved = event.value("file", "");
        recordingError = event.value("error", "");
    } else if (kind == "progress") {
        progress = event.value("text", "");
    } else if (kind == "log") {
        syslog.push_back(event.value("line", ""));
        while (syslog.size() > 5000)
            syslog.pop_front();
    } else if (kind == "exit") {
        exited_ = true;
        exitClean_ = event.value("clean", false);
        exitMessage_ = event.value("message", "");
    }
}

bool Session::UpdateFrame() { return frames_.Read(frameSeen_, pixels_, frameWidth_, frameHeight_); }
