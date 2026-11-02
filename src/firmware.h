// Preparing devices: the firmware catalog, downloading Apple's IPSW (and a boot ROM for the
// oldest boards), and running firmwarekit.exe, which turns an IPSW into a bootable device
// (decrypting it, building the NAND image, installing the guest tools and, if asked, Cydia).
#pragma once

#include "common.h"
#include "library.h"

#include <functional>
#include <mutex>
#include <thread>

// One iOS version for one device, from resources\firmware-catalog.json.
struct Firmware {
    std::string id;            // "<board>-<build>", e.g. "n18ap-8C148"
    std::string board;         // "n18ap"
    std::string productType;   // "iPod3,1"
    std::string version;       // "4.2.1"
    std::string label;         // the version as shown: "4.2.1", or "4.2 beta 2" for a developer build
    std::string build;         // "8C148"
    std::string released;      // "2010-11-22"
    std::string status;        // LightTouch's testing status: available, experimental, untested
    std::vector<std::string> urls;   // where the IPSW can be downloaded, best first
    std::string sha1;
    uint64_t bytes = 0;              // IPSW size
    uint64_t preparedBytes = 0;      // disk space the prepared device takes
    uint64_t peakBytes = 0;          // disk space needed while preparing
    std::string sibling;             // another firmware whose IPSW is also needed (its keybag ramdisk)
    int storageGb = 0;               // the emulated flash storage (fixed per board), 0 if unknown

    std::string FileName() const { return productType + "_" + version + "_" + build + "_Restore.ipsw"; }
    std::string Product() const { return ProductName(board); }
    bool Tested() const;                       // in LightTouch's list of firmwares that run well
    std::string JailbreakUnavailable() const;  // why Cydia can't be installed, or "" if it can
};

// Every firmware that can be prepared, ordered by device, then release date.
const std::vector<Firmware> &Catalog();
const Firmware *FindFirmware(const std::string &id);

struct PrepareRequest {
    Firmware firmware;
    std::string name;     // the new device's name ("" picks the product name)
    bool jailbreak = false;
    fs::path ipsw;        // an IPSW the user already has; empty downloads it
};

// Prepares one device on a background thread. Poll Status() from the UI.
class Preparation {
public:
    struct Status {
        std::string step;        // what's happening, for people
        double fraction = 0;     // progress of the whole preparation, 0..1
        bool finished = false;
        bool failed = false;
        std::string error;
        std::string deviceId;    // the new device, once finished
    };

    explicit Preparation(PrepareRequest request);
    ~Preparation();   // cancels and waits
    Preparation(const Preparation &) = delete;
    Preparation &operator=(const Preparation &) = delete;

    Status GetStatus();
    void Cancel();
    const PrepareRequest &Request() const { return request_; }

private:
    void Run();
    void Report(const std::string &step, double fraction);
    fs::path GetIpsw(const Firmware &firmware, double from, double to, bool &downloaded);
    fs::path GetBootrom();
    void RunFirmwarekit(const fs::path &ipsw, const fs::path &sibling, const fs::path &bootrom, const fs::path &out);

    PrepareRequest request_;
    std::mutex mutex_;
    Status status_;
    std::atomic<bool> cancelled_{false};
    ChildProcess firmwarekit_;
    std::thread thread_;
};

// Downloads `url` into `file`, resuming a partial file. `progress(done, total)` returns false to cancel.
// Throws with a readable message on failure.
void Download(const std::string &url, const fs::path &file, const std::function<bool(uint64_t, uint64_t)> &progress);
