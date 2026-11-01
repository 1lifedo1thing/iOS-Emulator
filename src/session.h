// A running device, as the UI sees it.
//
// Starting a session launches two helper processes:
//   1. lighttouch-usbmuxd.exe, the "USB cable": the guest's USB port connects to it, and the
//      developer services (pairing, files, apps, syslog, SSH) talk to iOS through it;
//   2. the worker (iOS-Emulator.exe --worker, see worker.h), which runs the engine.
// The session then reads the worker's events and screen frames, and sends it commands.
#pragma once

#include "common.h"
#include "library.h"

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

struct SessionOptions {
    bool internet = false;
    bool webProxy = true;     // HTTPS through the web proxy's bridge (proxy.h)
    bool headphones = false;
    bool muted = false;
    bool keyboard = false;    // keep the USB keyboard plugged in (boards with a USB host, worker.h)
};

class Session {
public:
    Session(const Device &device, const SessionOptions &options);   // throws if a helper can't start
    ~Session();                                                     // kills the helpers if still running
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    const Device &GetDevice() const { return device_; }

    // Sends a command (worker.h lists them). Requests get their reply passed to `onReply` later,
    // from Update(), on the caller's thread.
    void Send(const Json &command);
    void Request(Json command, std::function<void(const Json &reply)> onReply);

    void Stop();      // asks iOS to shut down cleanly; Update() force-stops it after a minute
    void Kill();      // ends the worker at once (the device is marked dirty)
    void Update();    // call regularly: applies the worker's events and enforces the stop deadline

    // Copies the newest screen frame (BGRA) if there is one. Returns true when the frame changed.
    bool UpdateFrame();
    const std::vector<uint8_t> &FramePixels() const { return pixels_; }
    int FrameWidth() const { return frameWidth_; }
    int FrameHeight() const { return frameHeight_; }

    // What the worker last reported.
    std::string state = "starting";   // starting, running, paused, stopping, stopped
    bool sleeping = false;            // iOS turned the display off
    int agent = 0;                    // guest tools: 0 not running, 1 alive, 2 not answering
    int screenWidth = 320, screenHeight = 480;
    uint64_t capabilities = 0;        // QEMU_IOS_CAP_* bits
    bool usbKeyboard = false;         // typing goes through a USB keyboard plugged into the device
    std::string machine;
    bool paused = false, muted = false, internet = false, webProxy = true;
    std::string proxyStatus;          // how setting up the web proxy went (the latest "proxy" event)
    bool proxyError = false;

    bool Exited() const { return exited_; }
    bool ExitedCleanly() const { return exitClean_; }
    const std::string &ExitMessage() const { return exitMessage_; }
    std::deque<std::string> syslog;   // device log lines, newest last (capped)
    std::string progress;             // the latest progress line of a long request (an app install)

    fs::path WorkerLog() const;

private:
    void StartUsbmuxd();
    void StartWorker(const SessionOptions &options);
    void ReadEvents();
    void HandleEvent(const Json &event);
    void StopUsbmuxd();

    Device device_;
    ChildProcess usbmuxd_, worker_;
    std::thread usbmuxdDrain_, eventReader_;
    std::string usbmuxClientPort_, usbmuxGuestPort_, usbmuxPid_, usbmuxCreated_;

    FrameMapping frames_;
    uint64_t frameSeen_ = 0;
    std::vector<uint8_t> pixels_;
    int frameWidth_ = 0, frameHeight_ = 0;

    HANDLE eventsRead_ = nullptr;
    std::mutex inboxMutex_;
    std::deque<Json> inbox_;
    bool eventsEnded_ = false;

    uint64_t nextRequest_ = 0;
    std::map<uint64_t, std::function<void(const Json &)>> pending_;

    bool exited_ = false, exitClean_ = false;
    std::string exitMessage_;
    std::chrono::steady_clock::time_point stopDeadline_ = std::chrono::steady_clock::time_point::max();
};
