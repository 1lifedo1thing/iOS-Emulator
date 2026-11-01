#include "worker.h"

#include "engine.h"
#include "keyboard.h"
#include "library.h"
#include "proxy.h"
#include "services.h"

#include <mmsystem.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace {

using namespace std::chrono_literals;

// ---- Sound -----------------------------------------------------------------------------------

// Plays the guest's mixer output (44.1 kHz, stereo, 16-bit) through the Windows waveOut API.
// A small ring of buffers keeps latency near 100 ms; if they are all still playing, the newest
// packet is dropped rather than letting sound drift behind the picture.
class AudioPlayer {
public:
    ~AudioPlayer() {
        if (!out_)
            return;
        waveOutReset(out_);
        for (auto &header : headers_)
            if (header.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(out_, &header, sizeof header);
        waveOutClose(out_);
    }

    bool Open() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = 44100 * 4;
        if (waveOutOpen(&out_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            out_ = nullptr;
            return false;
        }
        for (int i = 0; i < kBuffers; i++) {
            memory_[i].resize(kBufferBytes);
            headers_[i].lpData = memory_[i].data();
            headers_[i].dwFlags = WHDR_DONE;   // free to use
        }
        return true;
    }

    void Play(const void *data, size_t bytes) {
        if (!out_ || bytes == 0 || bytes > kBufferBytes)
            return;
        for (auto &header : headers_) {
            if (!(header.dwFlags & WHDR_DONE))
                continue;
            if (header.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(out_, &header, sizeof header);
            memcpy(header.lpData, data, bytes);
            header.dwBufferLength = DWORD(bytes);
            header.dwFlags = 0;
            waveOutPrepareHeader(out_, &header, sizeof header);
            waveOutWrite(out_, &header, sizeof header);
            return;
        }
    }

private:
    static constexpr int kBuffers = 6;
    static constexpr size_t kBufferBytes = 16384;   // the engine's packet size: 4096 stereo frames
    HWAVEOUT out_ = nullptr;
    WAVEHDR headers_[kBuffers]{};
    std::vector<char> memory_[kBuffers];
};

// ---- The worker ------------------------------------------------------------------------------

class Worker {
public:
    Worker(Device device, const std::string &framesName, HANDLE events, bool internet, bool webProxy, bool headphones,
           bool muted)
        : device_(std::move(device)), events_(events), internet_(internet), webProxy_(webProxy), headphones_(headphones),
          muted_(muted) {
        frames_.Open(framesName);
    }

    int Run();

private:
    // Guest-agent replies: status 0 means success; `body` is what the guest sent back.
    struct AgentReply {
        int status = -1;
        std::string body;
    };

    void Send(const Json &event);
    void Reply(const Json &command, bool ok, const Json &result, const std::string &error = {});
    void Background(std::function<void()> task);

    void GuestThread(std::vector<std::string> args);
    void PublishThread();
    void AudioThread();
    void CommandThread();
    void BackgroundThread();

    void HandleCommand(const Json &command);
    void HandleDeveloperCommand(const Json &command);
    uint64_t Touch(const std::vector<QemuIosPortableContact> &contacts);
    void PublishFrame();
    void SendStatusIfChanged();
    void Shutdown();
    void StartWebProxy(BootOptions &options);
    void UpdateWebProxy();
    void TrustWebProxy();
    AgentReply Agent(const std::string &op, const std::string &argument, const std::string &body,
                     std::chrono::milliseconds timeout = 30s);

    Device device_;
    std::unique_ptr<Engine> engine_;
    FrameMapping frames_;
    HANDLE events_;
    std::atomic<bool> internet_, webProxy_;
    bool headphones_;
    std::atomic<bool> muted_;

    std::mutex eventsMutex_;                 // one writer at a time on the events pipe
    std::atomic<bool> done_{false};          // qemu_ios_main returned
    std::atomic<bool> stopping_{false};      // a clean shutdown is under way
    std::atomic<int> exitCode_{0};

    std::mutex frameMutex_;                  // the engine's frame callback wakes the publisher
    std::condition_variable frameReady_;
    uint64_t frameSignals_ = 0;
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(SharedFrame::kPixelBytes);
    uint64_t frameSerial_ = 0;
    std::string lastStatus_;

    std::mutex touchMutex_;                  // the mouse and the on-screen keyboard both touch
    uint64_t touchId_ = 0;                   // every touch batch gets a new, increasing id
    uint64_t motionTime_ = 0;

    std::mutex agentMutex_;                  // the guest agent answers one request at a time
    uint64_t agentSequence_ = 0;

    std::mutex tasksMutex_;                  // slow requests (agent, USB services) run in order here
    std::condition_variable tasksReady_;
    std::deque<std::function<void()>> tasks_;

    Services services_;                      // USB developer services (files, apps, syslog, SSH)

    std::unique_ptr<WebProxy> proxy_;        // the guest's web proxy (proxy.h); null if it couldn't start
    std::atomic<bool> trusting_{false};      // its certificate is installed in iOS, or being installed

    // Types by tapping iOS's on-screen keyboard (keyboard.h). Declared last, so it's destroyed
    // first: its thread touches through this worker.
    std::unique_ptr<OnScreenKeyboard> keyboard_;
};

void Worker::Send(const Json &event) {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    WriteAll(events_, event.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n");
}

void Worker::Reply(const Json &command, bool ok, const Json &result, const std::string &error) {
    Json reply = {{"event", "reply"}, {"id", command.value("id", Json())}, {"cmd", command.value("cmd", "")}, {"ok", ok}};
    if (ok)
        reply["result"] = result;
    else
        reply["error"] = error;
    Send(reply);
}

void Worker::Background(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    tasks_.push_back(std::move(task));
    tasksReady_.notify_one();
}

int Worker::Run() {
    engine_ = std::make_unique<Engine>(ResourcesDir() / "libqemu-arm.dll");
    QemuIosPortableBoard board = engine_->FindBoard(device_.board);
    keyboard_ = std::make_unique<OnScreenKeyboard>(
        *engine_,
        [this](double x, double y, int phase) {
            QemuIosPortableContact contact{};
            contact.size = sizeof contact;
            contact.version = 1;
            contact.contact_id = 9;   // a finger of its own, apart from the mouse's (1 and 2)
            contact.phase = uint32_t(phase);
            contact.x = x;
            contact.y = y;
            return Touch({contact});
        },
        [this](const std::string &message) {
            Log("Typing: %s", message.c_str());
            Send({{"event", "typing"}, {"message", message}});
        });
    Send({{"event", "board"},
          {"machine", board.machine},
          {"width", board.width},
          {"height", board.height},
          {"capabilities", board.capabilities}});

    BootOptions options;
    options.machine = board.machine;
    options.internet = internet_;
    options.headphones = headphones_;
    StartWebProxy(options);
    std::vector<std::string> args = BuildBootArgs(device_, options);
    // For engine debugging: IOS_EMULATOR_ENGINE_ARGS adds QEMU options, separated by spaces
    // (e.g. "-trace enable=ipod_touch_multitouch_log").
    if (const char *extra = getenv("IOS_EMULATOR_ENGINE_ARGS")) {
        std::istringstream words(extra);
        for (std::string word; words >> word;)
            args.push_back(word);
    }
    for (size_t i = 0; i < args.size(); i++)
        Log("argv[%zu] %s", i, args[i].c_str());

    // The marker stays until iOS has shut down cleanly, so a crash is noticed next time.
    SaveFile(device_.State() / ".dirty", "The last session of this device didn't shut down cleanly.\n");

    engine_->attach(
        [](void *self) {
            auto *worker = static_cast<Worker *>(self);
            std::lock_guard<std::mutex> lock(worker->frameMutex_);
            worker->frameSignals_++;
            worker->frameReady_.notify_one();
        },
        this);
    std::thread guest(&Worker::GuestThread, this, args);
    std::thread publisher(&Worker::PublishThread, this);
    std::thread audio(&Worker::AudioThread, this);
    std::thread(&Worker::CommandThread, this).detach();      // blocked reading stdin; ends with the process
    std::thread(&Worker::BackgroundThread, this).detach();

    guest.join();
    publisher.join();
    audio.join();

    const bool storageFailed = engine_->storageFailed();
    const bool clean = engine_->shutdownConfirmed() && exitCode_ == 0 && !storageFailed;
    if (clean)
        fs::remove(device_.State() / ".dirty");
    std::string message = clean            ? "iOS shut down cleanly."
                          : storageFailed  ? "The device's storage failed; see the worker log."
                                           : "The device stopped without shutting iOS down.";
    Log("%s (engine exit code %d)", message.c_str(), exitCode_.load());
    Send({{"event", "exit"}, {"clean", clean}, {"message", message}});
    return clean ? 0 : 2;
}

void Worker::GuestThread(std::vector<std::string> args) {
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    // Runs the whole emulator; returns once the guest has stopped and qemu_ios_ui_quit was called.
    exitCode_ = engine_->main(int(args.size()), argv.data());
    done_ = true;
    frameReady_.notify_all();
}

void Worker::PublishThread() {
    while (!done_) {
        {
            std::unique_lock<std::mutex> lock(frameMutex_);
            const uint64_t seen = frameSignals_;
            frameReady_.wait_for(lock, 10ms, [&] { return frameSignals_ != seen || done_; });
        }
        if (done_ || !engine_->ready())
            continue;
        PublishFrame();
        SendStatusIfChanged();
    }
}

void Worker::PublishFrame() {
    QemuIosPortableFrame meta{};
    meta.size = sizeof meta;
    meta.version = 1;
    if (engine_->frameCopy(scratch_.data(), scratch_.size(), frameSerial_, &meta) != QEMU_IOS_OK)
        return;   // no newer frame yet
    frameSerial_ = meta.serial;
    SharedFrame *frame = frames_.Frame();
    frame->sequence.fetch_add(1, std::memory_order_acq_rel);   // odd: readers wait
    frame->width = meta.width;
    frame->height = meta.height;
    frame->stride = meta.stride;
    memcpy(frame->Pixels(), scratch_.data(), size_t(meta.stride) * meta.height);
    frame->sequence.fetch_add(1, std::memory_order_release);   // even: a complete frame
}

void Worker::SendStatusIfChanged() {
    const char *state = stopping_ ? "stopping" : engine_->paused() ? "paused" : "running";
    const bool sleeping = engine_->displaySleeping();
    const int agent = engine_->agentStatus();
    std::string key = std::string(state) + (sleeping ? "/sleeping" : "") + "/" + std::to_string(agent);
    if (key == lastStatus_)
        return;
    lastStatus_ = key;
    Send({{"event", "status"}, {"state", state}, {"sleeping", sleeping}, {"agent", agent}});
    if (agent == 1 && webProxy_)
        TrustWebProxy();   // the guest tools are up: iOS can be told to trust the proxy
}

void Worker::AudioThread() {
    AudioPlayer player;
    bool open = false;
    uint64_t generation = 0;
    std::vector<uint8_t> packet(16384);
    while (!done_) {
        if (!engine_->ready() || engine_->paused() || stopping_) {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        if (!open)
            open = player.Open() || (Log("No sound output device; sound is off."), false);
        if (generation == 0)
            generation = engine_->audioStart();
        double seconds = 0;
        int bytes = engine_->audioRead(generation, packet.data(), int(packet.size()), &seconds);
        if (bytes < 0) {   // the capture overflowed or expired: start a fresh one
            engine_->audioStop(generation);
            generation = 0;
            continue;
        }
        if (bytes == 0) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (!muted_)
            player.Play(packet.data(), size_t(bytes));
    }
    if (generation)
        engine_->audioStop(generation);
}

void Worker::CommandThread() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    std::string pending, line;
    while (ReadLineFrom(input, pending, line)) {
        if (line.empty())
            continue;
        try {
            HandleCommand(Json::parse(line));
        } catch (const std::exception &e) {
            Log("Bad command %s: %s", line.c_str(), e.what());
        }
    }
    // The UI closed our stdin (or went away): shut iOS down cleanly.
    HandleCommand({{"cmd", "stop"}});
}

void Worker::BackgroundThread() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(tasksMutex_);
            tasksReady_.wait(lock, [&] { return !tasks_.empty(); });
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception &e) {
            Log("Background task failed: %s", e.what());
        }
    }
}

void Worker::HandleCommand(const Json &command) {
    const std::string cmd = command.value("cmd", "");
    const bool ready = engine_ && engine_->ready() && !done_;

    if (cmd == "stop") {
        if (!stopping_.exchange(true))
            std::thread(&Worker::Shutdown, this).detach();
    } else if (cmd == "touch" && ready) {
        std::vector<QemuIosPortableContact> contacts;
        for (const auto &c : command.at("contacts")) {
            if (contacts.size() == QEMU_IOS_PORTABLE_MAX_CONTACTS)
                break;
            QemuIosPortableContact contact{};
            contact.size = sizeof contact;
            contact.version = 1;
            contact.contact_id = c.at("id").get<uint32_t>();
            contact.phase = c.at("phase").get<uint32_t>();
            contact.x = std::clamp(c.at("x").get<double>(), 0.0, 1.0);
            contact.y = std::clamp(c.at("y").get<double>(), 0.0, 1.0);
            contacts.push_back(contact);
        }
        Touch(contacts);
    } else if (cmd == "button" && ready) {
        engine_->button(command.at("button").get<int>(), command.at("down").get<bool>());
    } else if (cmd == "motion" && ready) {
        QemuIosPortableMotion motion{};
        motion.size = sizeof motion;
        motion.version = 1;
        motionTime_ = std::max(motionTime_ + 1, NowNs());   // must strictly increase
        motion.timestamp_ns = motionTime_;
        motion.fields = QEMU_IOS_MOTION_ACCEL;
        motion.acceleration[0] = command.value("x", 0.0);
        motion.acceleration[1] = command.value("y", 0.0);
        motion.acceleration[2] = command.value("z", 9.80665);
        engine_->motion(&motion);
    } else if (cmd == "pause" && ready) {
        command.value("paused", false) ? engine_->pause() : engine_->resume();
    } else if (cmd == "mute") {
        muted_ = command.value("muted", false);
    } else if (cmd == "internet" && ready) {
        internet_ = command.value("on", false);
        engine_->netRestrict("wifi0", !internet_);
        UpdateWebProxy();
    } else if (cmd == "webproxy") {
        webProxy_ = command.value("on", false);
        UpdateWebProxy();
        if (webProxy_ && ready && engine_->agentStatus() == 1)
            TrustWebProxy();
    } else if (cmd == "type") {
        keyboard_->Type(command.value("text", ""));
    } else if (ready && command.contains("id")) {
        HandleDeveloperCommand(command);
    } else {
        Reply(command, false, nullptr, ready ? "Unknown command " + cmd : "The device isn't running yet");
    }
}

// Queues a batch of touches with the engine. The UI's mouse and the on-screen keyboard both touch,
// from different threads, and every batch needs a new, increasing id. Returns the batch's id.
uint64_t Worker::Touch(const std::vector<QemuIosPortableContact> &contacts) {
    std::lock_guard<std::mutex> lock(touchMutex_);
    if (touchId_) {   // how the previous batch ended (the engine applies them asynchronously)
        const int previous = engine_->inputStatus(touchId_);
        if (previous != QEMU_IOS_OK && previous != QEMU_IOS_QUEUED)
            Log("Touch batch %llu ended with %d", (unsigned long long)touchId_, previous);
    }
    const int result = engine_->touch(++touchId_, NowNs(), contacts.data(), contacts.size());
    if (result != QEMU_IOS_QUEUED && result != QEMU_IOS_OK)
        Log("The engine refused a touch (phase %u): %d", contacts.empty() ? 0 : contacts[0].phase, result);
    return touchId_;
}

// Developer requests: each runs in order on the background thread and answers with a reply.
void Worker::HandleDeveloperCommand(const Json &command) {
    Background([this, command] {
        const std::string cmd = command.value("cmd", "");
        const std::string path = command.value("path", "");
        try {
            Json result = nullptr;
            if (cmd == "info") {
                result = services_.Info();
            } else if (cmd == "files.list") {
                result = services_.ListFiles(path);
            } else if (cmd == "files.download") {
                services_.DownloadFile(path, fs::path(Widen(command.at("to").get<std::string>())));
            } else if (cmd == "files.upload") {
                services_.UploadFile(fs::path(Widen(command.at("from").get<std::string>())), path);
            } else if (cmd == "files.delete") {
                services_.DeletePath(path);
            } else if (cmd == "files.mkdir") {
                services_.MakeFolder(path);
            } else if (cmd == "files.rename") {
                services_.RenamePath(path, command.at("to").get<std::string>());
            } else if (cmd == "apps.list") {
                result = services_.ListApps();
            } else if (cmd == "apps.install") {
                services_.InstallApp(fs::path(Widen(command.at("ipa").get<std::string>())), [&](const std::string &text) {
                    Send({{"event", "progress"}, {"id", command["id"]}, {"text", text}});
                });
            } else if (cmd == "apps.launch") {
                AgentReply reply = Agent("launch", command.at("bundle").get<std::string>(), "");
                if (reply.status != 0)
                    throw std::runtime_error("iOS didn't open the app (status " + std::to_string(reply.status) + ")");
            } else if (cmd == "shell") {
                // it_agent's "spawn" runs a program as root and returns its output. Through the
                // jailbreak's /bin/sh this is a root shell; without it, give a program's full path.
                const std::string line = command.value("command", "");
                std::vector<std::string> argv;
                if (command.value("sh", true)) {
                    argv = {"/bin/sh", "-c", line};
                } else {
                    std::istringstream words(line);
                    for (std::string word; words >> word;)
                        argv.push_back(word);
                }
                std::string body;
                for (const auto &arg : argv)
                    body += arg + '\0';
                AgentReply reply = Agent("spawn", "", body, std::chrono::seconds(70));
                if (reply.status == -1 && reply.body.empty())
                    throw std::runtime_error("The guest tools aren't answering yet");
                result = {{"status", reply.status}, {"output", reply.body}};
            } else if (cmd == "syslog") {
                if (command.value("on", false))
                    services_.StartSyslog([this](const std::string &line) { Send({{"event", "log"}, {"line", line}}); });
                else
                    services_.StopSyslog();
            } else if (cmd == "forward") {
                if (command.value("on", false))
                    services_.StartForward(command.value("port", 2222), command.value("devicePort", 22));
                else
                    services_.StopForward();
            } else {
                throw std::runtime_error("Unknown command " + cmd);
            }
            Reply(command, true, result);
        } catch (const std::exception &e) {
            Reply(command, false, nullptr, e.what());
        }
    });
}

// Shuts iOS down the way pressing "slide to power off" would, then stops the engine
// (ported from LightTouch's Portable/src/Runtime.cpp publisher).
void Worker::Shutdown() {
    auto waitFor = [&](auto condition, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done_ && !condition() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(100ms);
        return condition();
    };
    Log("Stopping: waiting for the engine");
    if (!waitFor([&] { return engine_->ready(); }, 120s) || done_)
        return;
    engine_->cancelInput();
    if (engine_->paused())
        engine_->resume();
    // The guest agent performs the clean power-off; give a guest that is still booting time to start it.
    waitFor([&] { return engine_->agentStatus() == 1; }, 20s);
    if (engine_->displaySleeping()) {   // wake the screen with Home, never with Sleep/Wake
        engine_->button(QEMU_IOS_BUTTON_HOME, true);
        std::this_thread::sleep_for(150ms);
        engine_->button(QEMU_IOS_BUTTON_HOME, false);
    }
    const int route = engine_->shutdown();   // 1: the agent halts iOS, 2: power-off gesture, 0: couldn't start
    Log("Stopping: shutdown route %d", route);
    if (route > 0)
        waitFor([&] { return engine_->shutdownConfirmed(); }, 35s);
    Log(engine_->shutdownConfirmed() ? "Stopping: iOS has halted" : "Stopping: iOS didn't halt in time");
    engine_->cancelInput();
    engine_->quit();
}

// The web proxy listens before iOS boots; the engine forwards the guest's 10.0.2.100:3128 to it.
// Its identity is kept per device in <data>\network\<id> (as LightTouch keeps it).
void Worker::StartWebProxy(BootOptions &options) {
    try {
        proxy_ = std::make_unique<WebProxy>(DataDir() / "network" / device_.id);
        const fs::path socket = fs::temp_directory_path() / ("ios-emulator-" + device_.id.substr(0, 8) + "-" +
                                                             std::to_string(GetCurrentProcessId()) + ".sock");
        proxy_->Listen(socket);
        options.proxySocket = socket;
        UpdateWebProxy();
    } catch (const std::exception &e) {
        // Without it, iOS waits out a timeout on every new connection before going direct.
        proxy_.reset();
        Log("The web proxy didn't start: %s", e.what());
        Send({{"event", "proxy"}, {"status", std::string("The web proxy didn't start: ") + e.what()}, {"error", true}});
    }
}

void Worker::UpdateWebProxy() {
    if (proxy_)
        proxy_->SetMode(!internet_ ? WebProxy::Mode::Off : webProxy_ ? WebProxy::Mode::Bridge : WebProxy::Mode::Direct);
}

// Installs the proxy's certificate in iOS's trust store, so iOS accepts the HTTPS connections the
// proxy re-encrypts. Uses the guest-tools helper ittrust (from LightTouch), copied in each time:
// the trust store keeps the certificate, but a newer helper may have been bundled since.
void Worker::TrustWebProxy() {
    if (!proxy_ || trusting_.exchange(true))
        return;
    Background([this] {
        const std::string helper = "/private/var/tmp/ios-emulator-ittrust";
        const std::string certificate = "/private/var/tmp/ios-emulator-proxy.der";
        Send({{"event", "proxy"}, {"status", "Installing the web proxy's certificate in iOS"}, {"error", false}});
        AgentReply reply = Agent("put", helper + " 755", LoadFile(ResourcesDir() / "guest-tools" / "ittrust"));
        if (reply.status == 0)
            reply = Agent("put", certificate + " 644", proxy_->Certificate());
        if (reply.status == 0)   // spawn takes argv as NUL-separated words
            reply = Agent("spawn", "", helper + '\0' + "add" + '\0' + certificate + '\0');
        const bool trusted = reply.status == 0 && reply.body.find("Guest trust add: 0") != std::string::npos;
        if (trusted) {
            Log("iOS trusts the web proxy's certificate");
        } else {
            trusting_ = false;   // try again the next time the proxy is switched on
            Log("Installing the web proxy's certificate failed (guest status %d): %s", reply.status, reply.body.c_str());
        }
        Send({{"event", "proxy"},
              {"status", trusted ? "Ready: iOS trusts the web proxy" : "iOS didn't accept the web proxy's certificate"},
              {"error", !trusted}});
    });
}

Worker::AgentReply Worker::Agent(const std::string &op, const std::string &argument, const std::string &body,
                                 std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(agentMutex_);
    AgentReply reply;
    if (!engine_->ready() || engine_->agentStatus() != 1)
        return reply;   // the guest tools aren't running (yet)
    // Request: "<id> <op> [argument]\n<base64 body>". Reply: "<id> <status>\n<base64 body>".
    const std::string id = "ie-" + std::to_string(++agentSequence_);
    const std::string request = id + " " + op + (argument.empty() ? "" : " " + argument) + "\n" + Base64Encode(body);
    if (!engine_->agentRequest(request.c_str()))
        return reply;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline && !done_) {
        char *raw = engine_->agentResult();
        if (!raw) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        std::string text = raw;
        engine_->agentFreeResult(raw);
        const size_t space = text.find(' '), newline = text.find('\n');
        if (space == std::string::npos || newline == std::string::npos || text.substr(0, space) != id)
            continue;   // someone else's reply (e.g. the shutdown's)
        reply.status = std::atoi(text.substr(space + 1, newline - space - 1).c_str());
        reply.body = Base64Decode(text.substr(newline + 1));
        return reply;
    }
    return reply;
}

}  // namespace

int RunWorker(const std::vector<std::string> &args) {
    std::string deviceDir, framesName;
    HANDLE events = nullptr;
    bool internet = false, webProxy = false, headphones = false, muted = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "--device" && hasValue)
            deviceDir = args[++i];
        else if (arg == "--frames" && hasValue)
            framesName = args[++i];
        else if (arg == "--events" && hasValue)
            events = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(args[++i])));
        else if (arg == "--internet")
            internet = true;
        else if (arg == "--web-proxy")
            webProxy = true;
        else if (arg == "--headphones")
            headphones = true;
        else if (arg == "--muted")
            muted = true;
    }
    if (deviceDir.empty() || framesName.empty() || !events) {
        Log("usage: iOS-Emulator --worker --device <dir> --frames <name> --events <handle> [--internet] "
            "[--web-proxy] [--headphones] [--muted]");
        return 64;
    }
    try {
        Worker worker(OpenDevice(fs::path(Widen(deviceDir))), framesName, events, internet, webProxy, headphones,
                      muted);
        return worker.Run();
    } catch (const std::exception &e) {
        Log("Worker failed: %s", e.what());
        WriteAll(events, Json({{"event", "exit"}, {"clean", false}, {"message", e.what()}}).dump() + "\n");
        return 2;
    }
}
