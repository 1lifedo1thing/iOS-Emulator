// The device library: every prepared device is a folder on disk, laid out like LightTouch's
// (so a LightTouch device folder can be copied in and used):
//
//   <data>\library\records\<id>\
//       record.json   name, board, iOS build
//       keymap.json   the keys that hold fingers on the screen (keymap.h); Erase keeps it
//       base\        the prepared system from firmwarekit; never written after preparation
//       state\        everything the device writes: its NAND overlay, NOR copy, logs
//       assets\       extra boot files (the boot ROM for the oldest boards)
//       snapshots\<name>\   saved machines for quick test runs: machine.state + a copy of state\
//                           (iOS-Emulator --run --save-snapshot / --snapshot, see main.cpp)
#pragma once

#include "common.h"

struct Device {
    std::string id;      // 32 lowercase hex characters
    std::string name;    // what the user called it
    std::string board;   // the board ID, e.g. "n18ap"
    std::string build;   // the iOS build, e.g. "8C148"
    fs::path dir;
    fs::path stateDir;   // set for a run from a snapshot: a throwaway copy used instead of state\

    fs::path Base() const { return dir / "base"; }
    fs::path State() const { return stateDir.empty() ? dir / "state" : stateDir; }
    fs::path Assets() const { return dir / "assets"; }
    fs::path Snapshot(const std::string &name) const { return dir / "snapshots" / name; }
    fs::path KeymapFile() const { return dir / "keymap.json"; }

    Json Lock() const;                // base\device.lock.json, the facts firmwarekit recorded
    std::string Product() const;      // "iPod touch 3G"
    std::string Version() const;      // "4.2.1"
    bool Jailbroken() const;          // prepared with Cydia and the AFC2 root file service
    bool Dirty() const;               // the last session didn't shut iOS down cleanly
};

fs::path LibraryDir();

// Reads the device in a record folder (one holding record.json).
Device OpenDevice(const fs::path &dir);

// Every device in the library, sorted by name.
std::vector<Device> ListDevices();

// Finds a device by its id (or a unique id prefix) or its exact name.
Device FindDevice(const std::string &idOrName);

// Turns firmwarekit's output folder into a new device: moves it into the library as "base",
// copies the boot ROM (if the board needs one) and writes record.json.
Device AddPreparedDevice(const fs::path &prepared, const std::string &name, const fs::path &bootrom);

// Copies a device folder from elsewhere (e.g. LightTouch's %LOCALAPPDATA%\LightTouch\App\library\
// records\<id>) into the library. The read-only base is hard-linked when possible, so it takes no space.
Device ImportDevice(const fs::path &recordDir);

void RenameDevice(Device &device, const std::string &name);
void EraseDevice(const Device &device);    // back to the freshly prepared system
void DeleteDevice(const Device &device);

// Readable product name for a board ID: "n18ap" -> "iPod touch 3G".
std::string ProductName(const std::string &board);
