// Developer services for one running device, over its emulated USB cable.
//
// They use libimobiledevice, the same library real-device tools use (ideviceinfo, ifuse, ...),
// talking to lighttouch-usbmuxd.exe, which the guest's USB port is connected to:
//   - device information (lockdown);
//   - files: the whole filesystem as root over AFC2 on jailbroken devices, otherwise the Media
//     folder over AFC;
//   - apps: list them and install .ipa files (AppSync is preinstalled, so unsigned apps work);
//   - the live system log (syslog_relay);
//   - SSH: a local port forwarded to the device's port 22 (OpenSSH comes from Cydia).
// They live in the worker process, which passes the usbmuxd address in its environment.
#pragma once

#include "common.h"

#include <functional>
#include <mutex>
#include <thread>

struct idevice_private;
struct lockdownd_client_private;
struct afc_client_private;

class Services {
public:
    Services() = default;
    ~Services();
    Services(const Services &) = delete;
    Services &operator=(const Services &) = delete;

    // Each call throws std::runtime_error with a readable message on failure.
    Json Info();                                           // what lockdown knows about the device
    bool RootAccess();                                     // AFC2 is available (jailbroken device)
    Json ListFiles(const std::string &path);               // [{name, type: file|folder|link, size, modified, target}]
    void DownloadFile(const std::string &path, const fs::path &to);
    void UploadFile(const fs::path &from, const std::string &path,
                    const std::function<void(uint64_t sent, uint64_t total)> &progress = nullptr);
    void DeletePath(const std::string &path);              // folders with everything inside
    void MakeFolder(const std::string &path);
    void RenamePath(const std::string &from, const std::string &to);
    Json ListApps();                                       // [{bundle, name, version, type, path}]
    void InstallApp(const fs::path &ipa, const std::function<void(const std::string &)> &progress);

    // The system log, a line at a time, on a library thread.
    void StartSyslog(std::function<void(const std::string &)> onLine);
    void StopSyslog();

    // Forwards 127.0.0.1:<localPort> to the device's <devicePort> (22 for SSH).
    void StartForward(uint16_t localPort, uint16_t devicePort);
    void StopForward();
    bool Forwarding() const { return listener_ != ~uintptr_t(0); }

private:
    idevice_private *Device();
    afc_client_private *Afc();
    void ResetAfc();
    std::string Udid();
    void Upload(afc_client_private *afc, const fs::path &from, const std::string &path,
                const std::function<void(uint64_t, uint64_t)> &progress);
    void DeleteTree(afc_client_private *afc, const std::string &path);
    void ForwardLoop(uint16_t devicePort);

    std::recursive_mutex mutex_;           // one service call at a time
    idevice_private *device_ = nullptr;
    afc_client_private *afc_ = nullptr;
    bool afcIsRoot_ = false;

    struct syslog_relay_client_private *syslog_ = nullptr;
    std::function<void(const std::string &)> onSyslogLine_;
    std::string syslogLine_;

    uintptr_t listener_ = ~uintptr_t(0);   // the forward's listening socket (INVALID_SOCKET when off)
    std::thread forwardThread_;
};
