#include "worker.h"

#include "engine.h"
#include "ipa.h"
#include "library.h"
#include "proxy.h"
#include "recorder.h"
#include "services.h"
#include "snapshot.h"

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

// ---- Typing ----------------------------------------------------------------------------------

// The most text one "type" request carries (the guest agent takes up to 64 KiB).
constexpr size_t kTypeChunk = 16384;

// What a failed input request to the guest agent ("type", "backspace", "uidump") means. The guest
// reports Darwin errno values; the engine reports its own timeout with Windows' ETIMEDOUT (138).
std::string InputError(int status, const std::string &body) {
    if (status == -1 && body.empty())
        return "The guest tools aren't running yet.";
    switch (-status) {
    case 13:   // EACCES
        return "iOS is locked. Unlock it first.";
    case 19:   // ENODEV
        return "No text field has focus. Tap one in iOS first.";
    case 35:   // EAGAIN
        return "The app in front isn't ready for input yet. Try again.";
    case 3:    // ESRCH
        return "No app is in front.";
    case 78:   // ENOSYS
        return "The text field that has focus doesn't take typed text.";
    case 60:   // ETIMEDOUT (guest)
    case 138:  // ETIMEDOUT (engine)
        return "it_typein didn't answer. Devices prepared before typing used it don't have it: prepare the "
               "device again.";
    default:
        return "The guest tools failed (status " + std::to_string(status) + ").";
    }
}

// ---- Packages --------------------------------------------------------------------------------

// The guest script that installs <job>.deb, saved as <job>.sh and run as a launchd job called <job>.
// It logs to <job>.log and writes dpkg's status to <job>.status when it's done. A package whose
// dependencies are missing stays half-installed, so that's undone. A new app needs uicache (as
// mobile, whose caches SpringBoard reads), a new tweak a new SpringBoard. launchd would run the
// script again and again, so it removes its own job last.
constexpr char kInstallDeb[] = R"(f=${0%.sh}
exec </dev/null >$f.log 2>&1
package=$(dpkg-deb -f $f.deb Package 2>/dev/null)
contents=$(dpkg-deb -c $f.deb 2>/dev/null)
dpkg -i $f.deb
status=$?
rm -f $f.deb
if [ $status != 0 ]; then
    (dpkg -s "$package" | grep -q 'ok installed' || dpkg --remove "$package") >/dev/null 2>&1
else
    case "$contents" in *' ./Applications/'*) su -c uicache mobile ;; esac
    case "$contents" in *' ./Library/MobileSubstrate/DynamicLibraries/'*) launchctl stop com.apple.SpringBoard ;; esac
fi
echo $status > $f.status
launchctl remove ${f##*/}
)";

// What a failed dpkg log means. dpkg says " foo depends on bar (>= 2.0); however:" for each missing
// dependency; anything else is shown as it is, without the summary dpkg ends with.
std::string PackageError(const std::string &log) {
    std::string missing, line;
    for (std::istringstream lines(log); std::getline(lines, line);) {
        const size_t start = line.find(" depends on ");
        if (start != std::string::npos)
            missing += (missing.empty() ? "" : ", ") + line.substr(start + 12, line.find(';') - start - 12);
    }
    if (!missing.empty())
        return "It needs " + missing + ". Install that first.";
    std::string text = log.substr(0, log.find("Errors were encountered"));
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    return text;
}

// ---- The worker ------------------------------------------------------------------------------

class Worker {
public:
    Worker(Device device, const std::string &framesName, HANDLE events, bool internet, bool webProxy, bool headphones,
           bool muted, fs::path snapshot, bool saveSnapshots)
        : device_(std::move(device)), events_(events), internet_(internet), webProxy_(webProxy), headphones_(headphones),
          muted_(muted), snapshot_(std::move(snapshot)), saveSnapshots_(saveSnapshots) {
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
    void TypingThread();

    void HandleCommand(const Json &command);
    void HandleDeveloperCommand(const Json &command);
    void Touch(const std::vector<QemuIosPortableContact> &contacts);
    void TypeText(const std::string &text);
    void PublishFrame();
    void SendStatusIfChanged();
    void SendMotion();   // motion_ to the engine, with motionMutex_ held
    void Shutdown();
    void StartWebProxy(BootOptions &options);
    void UpdateWebProxy();
    void TrustWebProxy();
    void StartRecording(const fs::path &file, bool landscape);
    void StopRecording();
    Json SaveSnapshot(const std::string &name);
    void InstallPackage(const fs::path &deb, const std::function<void(const std::string &)> &progress);
    AgentReply Shell(const std::string &script);
    AgentReply Agent(const std::string &op, const std::string &argument, const std::string &body,
                     std::chrono::milliseconds timeout = 30s);

    Device device_;
    std::unique_ptr<Engine> engine_;
    FrameMapping frames_;
    HANDLE events_;
    std::atomic<bool> internet_, webProxy_;
    bool headphones_;
    std::atomic<bool> muted_;
    fs::path snapshot_;                      // restore this snapshot folder (a throwaway run); empty: boot
    bool saveSnapshots_;                     // "snapshot.save" may save the machine (through qemu's monitor)
    int monitorPort_ = 0;                    // qemu's monitor, when saveSnapshots_

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

    uint64_t touchId_ = 0;                   // every touch batch gets a new, increasing id

    // The engine forgets an accelerometer sample after 250 ms, so the last one is sent again every
    // 100 ms: a device turned to landscape stays turned while nothing new comes from the UI.
    std::mutex motionMutex_;
    bool motionHeld_ = false;                // a sample was sent, so keep repeating it
    double motion_[3] = {0, 0, 9.80665};     // that sample, m/s^2 in the guest's portrait axes
    uint64_t motionTime_ = 0;                // its timestamp (must strictly increase)

    std::mutex agentMutex_;                  // the guest agent answers one request at a time
    uint64_t agentSequence_ = 0;

    std::mutex tasksMutex_;                  // slow requests (agent, USB services) run in order here
    std::condition_variable tasksReady_;
    std::deque<std::function<void()>> tasks_;

    Services services_;                      // USB developer services (files, apps, syslog, SSH)

    std::unique_ptr<WebProxy> proxy_;        // the guest's web proxy (proxy.h); null if it couldn't start
    std::atomic<bool> trusting_{false};      // its certificate is installed in iOS, or being installed

    std::mutex recorderMutex_;               // guards recorder_, which the frame and sound threads feed
    std::unique_ptr<Recorder> recorder_;     // the screen recording under way (recorder.h), if any

    std::mutex typingMutex_;                 // text the "type" commands queued, not yet typed
    std::condition_variable typingReady_;
    std::string typing_;
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
    Send({{"event", "board"},
          {"machine", board.machine},
          {"width", board.width},
          {"height", board.height},
          {"capabilities", board.capabilities},
          {"maxContacts", board.max_contacts}});

    BootOptions options;
    options.machine = board.machine;
    options.internet = internet_;
    options.headphones = headphones_;
    StartWebProxy(options);
    // Snapshots (snapshot.h): a run from one works on a throwaway copy of its disk and restores
    // the machine instead of booting it; a run that saves them gets qemu's monitor.
    std::vector<std::string> snapshotArgs;
    if (!snapshot_.empty()) {
        if (device_.stateDir.empty())   // never overwrite the device's own state\ with the snapshot's
            throw std::runtime_error("A run from a snapshot needs a throwaway state folder (--state)");
        snapshotArgs = RestoreSnapshot(snapshot_, device_.State());
    }
    if (saveSnapshots_) {
        std::vector<std::string> monitor = MonitorArgs(monitorPort_);
        snapshotArgs.insert(snapshotArgs.end(), monitor.begin(), monitor.end());
    }
    std::vector<std::string> args = BuildBootArgs(device_, options);
    args.insert(args.end(), snapshotArgs.begin(), snapshotArgs.end());
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
    std::thread(&Worker::TypingThread, this).detach();

    guest.join();
    publisher.join();
    audio.join();
    StopRecording();   // iOS shut itself down: what was recorded is still saved

    const bool storageFailed = engine_->storageFailed();
    const bool throwaway = !snapshot_.empty();   // its state is discarded: no iOS shutdown needed
    const bool clean = (throwaway || engine_->shutdownConfirmed()) && exitCode_ == 0 && !storageFailed;
    if (clean)
        fs::remove(device_.State() / ".dirty");
    std::string message = storageFailed ? "The device's storage failed; see the worker log."
                          : !clean      ? "The device stopped without shutting iOS down."
                          : throwaway   ? "The snapshot run ended; its changes were discarded."
                                        : "iOS shut down cleanly.";
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
    auto motionSent = std::chrono::steady_clock::now();
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
        {
            std::lock_guard<std::mutex> lock(recorderMutex_);
            if (recorder_)
                recorder_->Blank(engine_->displaySleeping());
        }
        if (std::chrono::steady_clock::now() - motionSent >= 100ms) {   // keep the last motion sample alive
            motionSent = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(motionMutex_);
            if (motionHeld_)
                SendMotion();
        }
    }
}

void Worker::SendMotion() {
    QemuIosPortableMotion motion{};
    motion.size = sizeof motion;
    motion.version = 1;
    motionTime_ = std::max(motionTime_ + 1, NowNs());   // must strictly increase
    motion.timestamp_ns = motionTime_;
    motion.fields = QEMU_IOS_MOTION_ACCEL;
    for (int i = 0; i < 3; i++)
        motion.acceleration[i] = motion_[i];
    engine_->motion(&motion);
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
    std::lock_guard<std::mutex> lock(recorderMutex_);
    if (recorder_)
        recorder_->Frame(scratch_.data(), int(meta.width), int(meta.height), int(meta.stride));
}

// Runs on the command thread: starting takes a moment, and it mustn't wait behind a slow background
// request like an app install. The "recording" event tells the UI how it went.
void Worker::StartRecording(const fs::path &file, bool landscape) {
    if (std::lock_guard<std::mutex> lock(recorderMutex_); recorder_)
        return;
    try {
        // The recording starts with the screen as it is now, however long it then stands still.
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        uint64_t seen = ~uint64_t(0);
        for (int tries = 0; tries < 50 && !frames_.Read(seen, pixels, width, height); tries++)
            std::this_thread::sleep_for(2ms);
        if (width == 0)
            throw std::runtime_error("There's no screen to record yet.");
        auto recorder = std::make_unique<Recorder>(file, width, height, landscape);
        recorder->Frame(pixels.data(), width, height, width * 4);
        recorder->Blank(engine_->displaySleeping());
        std::lock_guard<std::mutex> lock(recorderMutex_);
        recorder_ = std::move(recorder);
        Send({{"event", "recording"}, {"on", true}});
    } catch (const std::exception &e) {
        Send({{"event", "recording"}, {"on", false}, {"error", e.what()}});
    }
}

void Worker::StopRecording() {
    std::unique_ptr<Recorder> recorder;
    {
        std::lock_guard<std::mutex> lock(recorderMutex_);
        recorder.swap(recorder_);
    }
    if (!recorder)
        return;
    const std::string error = recorder->Finish();
    if (error.empty())
        Send({{"event", "recording"}, {"on", false}, {"file", PathText(recorder->File())}});
    else
        Send({{"event", "recording"}, {"on", false}, {"error", error}});
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
        std::lock_guard<std::mutex> lock(recorderMutex_);
        if (recorder_)
            recorder_->Sound(packet.data(), size_t(bytes));
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

// Types the text the "type" commands queued. Whatever is queued while a request is under way goes
// out together in the next one, so fast typing (a keystroke per command) doesn't fall behind.
void Worker::TypingThread() {
    for (;;) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(typingMutex_);
            typingReady_.wait(lock, [&] { return !typing_.empty(); });
            text.swap(typing_);
        }
        TypeText(text);
    }
}

// Types through it_typein, a library the guest tools load into every app (and SpringBoard): the
// guest agent's "type" op hands it text, which it inserts into the text field that has focus, and
// "backspace" deletes one character. '\b' in the text is a backspace; '\n' is typed as a line
// break, which a one-line field takes as Return. On a failure the rest of the text is dropped and
// a "typing" event tells why.
void Worker::TypeText(const std::string &text) {
    size_t start = 0;
    while (start < text.size()) {
        AgentReply reply;
        if (text[start] == '\b') {
            reply = Agent("backspace", "", "", 10s);
            start++;
        } else {
            size_t end = std::min({text.find('\b', start), text.size(), start + kTypeChunk});
            while (end < text.size() && end > start + 1 && (uint8_t(text[end]) & 0xC0) == 0x80)
                end--;   // don't split a UTF-8 character
            reply = Agent("type", "", text.substr(start, end - start), 10s);
            start = end;
        }
        if (reply.status != 0) {
            const std::string message = InputError(reply.status, reply.body);
            Log("Typing: %s (status %d)", message.c_str(), reply.status);
            Send({{"event", "typing"}, {"message", message}});
            return;
        }
    }
}

void Worker::HandleCommand(const Json &command) {
    const std::string cmd = command.value("cmd", "");
    const bool ready = engine_ && engine_->ready() && !done_;

    if (cmd == "stop") {
        StopRecording();
        if (!stopping_.exchange(true))
            std::thread(&Worker::Shutdown, this).detach();
    } else if (cmd == "record") {
        if (!command.contains("to"))
            StopRecording();
        else if (ready)
            StartRecording(fs::path(Widen(command.value("to", ""))), command.value("landscape", false));
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
    } else if (cmd == "motion") {   // kept even before iOS is up: PublishThread sends it once the engine runs
        std::lock_guard<std::mutex> lock(motionMutex_);
        motion_[0] = command.value("x", 0.0);
        motion_[1] = command.value("y", 0.0);
        motion_[2] = command.value("z", 9.80665);
        motionHeld_ = true;
        if (ready)
            SendMotion();
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
        std::lock_guard<std::mutex> lock(typingMutex_);
        typing_ += command.value("text", "");
        typingReady_.notify_one();
    } else if (ready && command.contains("id")) {
        HandleDeveloperCommand(command);
    } else if (command.contains("id")) {   // a request gets an answer; input before iOS is up is dropped
        Reply(command, false, nullptr, ready ? "Unknown command " + cmd : "The device isn't running yet");
    } else if (ready) {
        Log("Unknown command %s", cmd.c_str());
    }
}

// Queues a batch of touches with the engine. Every batch needs a new, increasing id.
void Worker::Touch(const std::vector<QemuIosPortableContact> &contacts) {
    if (touchId_) {   // how the previous batch ended (the engine applies them asynchronously)
        const int previous = engine_->inputStatus(touchId_);
        if (previous != QEMU_IOS_OK && previous != QEMU_IOS_QUEUED)
            Log("Touch batch %llu ended with %d", (unsigned long long)touchId_, previous);
    }
    const int result = engine_->touch(++touchId_, NowNs(), contacts.data(), contacts.size());
    if (result != QEMU_IOS_QUEUED && result != QEMU_IOS_OK)
        Log("The engine refused a touch (phase %u): %d", contacts.empty() ? 0 : contacts[0].phase, result);
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
            } else if (cmd == "apps.install" || cmd == "packages.install") {
                auto progress = [&](const std::string &text) {
                    Send({{"event", "progress"}, {"id", command["id"]}, {"text", text}});
                };
                if (cmd == "apps.install") {
                    const fs::path ipa(Widen(command.at("ipa").get<std::string>()));
                    const IpaInfo app = ReadIpa(ipa);
                    if (IosOlderThan(device_.Version(), app.minimumOS))   // installd would say so minutes from now
                        throw std::runtime_error("This app needs iOS " + app.minimumOS + " or later, and this device runs iOS " +
                                                 device_.Version() + ".");
                    services_.InstallApp(ipa, app.unpackedBytes, progress);
                } else {
                    InstallPackage(fs::path(Widen(command.at("deb").get<std::string>())), progress);
                }
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
            } else if (cmd == "ui.dump") {
                // it_typein lists the views of the front app's key window (SpringBoard's when it's
                // locked): each view's description, and its text if it has any (not a password's).
                AgentReply reply = Agent("uidump", "", "", 15s);
                if (reply.status != 0)
                    throw std::runtime_error(InputError(reply.status, reply.body));
                result = {{"views", reply.body}};
            } else if (cmd == "syslog") {
                if (command.value("on", false))
                    services_.StartSyslog([this](const std::string &line) { Send({{"event", "log"}, {"line", line}}); });
                else
                    services_.StopSyslog();
            } else if (cmd == "snapshot.save") {
                result = SaveSnapshot(command.at("name").get<std::string>());
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
    if (!snapshot_.empty()) {   // a run from a snapshot is thrown away: no need to shut iOS down
        engine_->quit();
        return;
    }
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

// Saves the running machine and its disk to <device>\snapshots\<name>\ (snapshot.h).
Json Worker::SaveSnapshot(const std::string &name) {
    if (!monitorPort_)
        throw std::runtime_error("This run can't save snapshots (the worker needs --save-snapshots)");
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos)
        throw std::runtime_error("Not a valid snapshot name: " + name);
    engine_->cancelInput();   // a touch held across the save would stay held in every restore
    // Whether the display sleeps isn't in the snapshot, so a restored run couldn't tell that it
    // must wake it before touching: wake it now (with Home, never Sleep/Wake).
    if (engine_->displaySleeping()) {
        engine_->button(QEMU_IOS_BUTTON_HOME, true);
        std::this_thread::sleep_for(150ms);
        engine_->button(QEMU_IOS_BUTTON_HOME, false);
        std::this_thread::sleep_for(1500ms);
    }
    const fs::path folder = device_.Snapshot(name);
    const auto start = std::chrono::steady_clock::now();
    ::SaveSnapshot(monitorPort_, device_.State(), folder);
    Log("Saved the snapshot %s in %.1f s", PathText(folder).c_str(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return {{"path", PathText(folder)}, {"bytes", fs::file_size(folder / "machine.state")}};
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

// Installs a .deb with the jailbreak's dpkg. The guest agent ends a program after a minute or less,
// along with anything it left in the background, which would break dpkg's database in the middle
// of a big package. So launchd runs the install (kInstallDeb), and this asks every 2 seconds
// whether it has finished.
void Worker::InstallPackage(const fs::path &deb, const std::function<void(const std::string &)> &progress) {
    if (!device_.Jailbroken())
        throw std::runtime_error("Only a jailbroken device can install .deb packages.");
    const std::string name = "ios-emulator-" + RandomHex(8), job = "/private/var/tmp/" + name;
    services_.UploadFile(deb, job + ".deb", [&](uint64_t sent, uint64_t total) {
        progress("Copying to the device: " + FormatBytes(sent) + " of " + FormatBytes(total));
    });
    progress("Installing the package");
    if (Agent("put", job + ".sh 755", kInstallDeb).status != 0 ||
        Shell("launchctl submit -l " + name + " -- /bin/sh " + job + ".sh").status != 0)
        throw std::runtime_error("The guest tools aren't answering");

    const auto deadline = std::chrono::steady_clock::now() + 10min;
    for (;;) {
        std::this_thread::sleep_for(2s);
        // The first line is dpkg's status, the rest its log.
        AgentReply reply = Shell("[ -e " + job + ".status ] && cat " + job + ".status " + job + ".log && rm -f " + job + ".*");
        if (reply.status == 0) {
            if (std::atoi(reply.body.c_str()) == 0)
                return;
            throw std::runtime_error(PackageError(reply.body.substr(reply.body.find('\n') + 1)));
        }
        if (done_ || std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("It didn't finish within 10 minutes");
    }
}

// Runs a shell script in the guest as root and returns its output.
Worker::AgentReply Worker::Shell(const std::string &script) {
    using namespace std::string_literals;
    return Agent("spawn", "", "/bin/sh\0-c\0"s + script + '\0');
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
    std::string deviceDir, framesName, snapshot, stateDir;
    HANDLE events = nullptr;
    bool internet = false, webProxy = false, headphones = false, muted = false, saveSnapshots = false;
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
        else if (arg == "--snapshot" && hasValue)
            snapshot = args[++i];
        else if (arg == "--save-snapshots")
            saveSnapshots = true;
        else if (arg == "--state" && hasValue)
            stateDir = args[++i];
    }
    if (deviceDir.empty() || framesName.empty() || !events) {
        Log("usage: iOS-Emulator --worker --device <dir> --frames <name> --events <handle> [--internet] "
            "[--web-proxy] [--headphones] [--muted] [--snapshot <folder> --state <dir>] [--save-snapshots]");
        return 64;
    }
    try {
        Device device = OpenDevice(fs::path(Widen(deviceDir)));
        if (!stateDir.empty())
            device.stateDir = fs::path(Widen(stateDir));
        Worker worker(std::move(device), framesName, events, internet, webProxy, headphones, muted,
                      snapshot.empty() ? fs::path() : fs::path(Widen(snapshot)), saveSnapshots);
        return worker.Run();
    } catch (const std::exception &e) {
        Log("Worker failed: %s", e.what());
        WriteAll(events, Json({{"event", "exit"}, {"clean", false}, {"message", e.what()}}).dump() + "\n");
        return 2;
    }
}
