// Snapshots and save states: a running machine saved to disk.
//
// A snapshot is a folder, <device>\snapshots\<name>\, holding:
//   machine.state   qemu's migration stream: the guest's memory and devices
//   state\          a copy of the device's state\ (NAND overlay, NOR copy) from the same instant;
//                   the disk must match the memory, or iOS's filesystem breaks
//   slot.json       when it was saved, how the device was turned, its size, the engine that saved it
//   thumbnail.png   the screen at that moment
//
// One folder has two uses:
//   - a save state: the window's slots, and --run --load-state, replace the device's real storage
//     with the folder's and carry on from there (LoadSaveState);
//   - a test run: --run --snapshot works on a throwaway copy, so the device is never touched.
// Each device has a quick slot (F5 and F9, the folder kQuickSlot) and any number of named ones.
//
// Why not the engine's qemu_ios_snapshot_save2 and "-incoming file:<path>": qemu's file channel
// can't set blocking mode on Windows (io/channel-file.c), so such a save fails and such a restore
// aborts. Both go through a local TCP stream instead: saving drives qemu's monitor (QMP) to stop
// the guest and send the machine to us; restoring starts the engine with "-incoming tcp:" and
// feeds it the file.
#pragma once

#include "common.h"
#include "library.h"

inline const char *const kQuickSlot = "quick";

// What a snapshot's slot.json says about it.
struct SaveState {
    std::string name;         // its folder's name
    fs::path folder;
    int64_t savedAt = 0;      // Unix seconds
    bool landscape = false;   // the device was turned to landscape
    uint64_t bytes = 0;       // size on disk
    std::string engine;       // EngineVersion() of the engine that saved it

    fs::path Thumbnail() const { return folder / "thumbnail.png"; }
};

// The screen at the moment of a save. Empty if iOS hadn't drawn anything yet.
struct ScreenPicture {
    std::vector<uint8_t> bgra;
    int width = 0, height = 0;
};

// The sha256 of libqemu-arm.dll. qemu's migration format can change between builds, so a machine
// only goes back into the engine that saved it.
std::string EngineVersion();

std::vector<SaveState> ListSaveStates(const Device &device);    // the quick slot first, then newest first
SaveState ReadSaveState(const fs::path &folder);                // throws if it's no save state
void CheckSaveState(const SaveState &slot);                     // throws if this engine can't load it
bool ValidSlotName(const std::string &name);                    // it can name a slot's folder
void RenameSaveState(const SaveState &slot, const std::string &name);
void DeleteSaveState(const SaveState &slot);

// qemu's monitor on 127.0.0.1. Anyone who reaches it controls the machine, and qemu serves one
// client at a time, so the worker connects as soon as the engine starts and stays connected:
// nothing else on this computer can get in.
class Monitor {
public:
    explicit Monitor(int port);   // waits for the engine to listen; throws if it doesn't
    ~Monitor();
    Monitor(const Monitor &) = delete;
    Monitor &operator=(const Monitor &) = delete;

    Json Execute(const std::string &command, const Json &arguments = nullptr);

private:
    std::string ReadLine();

    uintptr_t socket_;      // a SOCKET
    std::string pending_;   // bytes read past the last full line
};

// Engine options for a run that may save: the monitor on a free local port (returned in `port`),
// and no run state in the stream, so a restored machine starts running by itself.
std::vector<std::string> MonitorArgs(int &port);

// Saves the machine (through `monitor`), the disk in `stateDir`, the picture and slot.json into
// `folder`, replacing it only once complete. The guest is stopped meanwhile, and resumed afterwards
// whether the save worked or not. Throws on failure.
void SaveSnapshot(Monitor &monitor, const fs::path &stateDir, const fs::path &folder, const ScreenPicture &screen,
                  bool landscape);

// Copies the snapshot's disk to `stateDir` (emptying it first) and returns the engine options that
// restore its machine instead of booting. A background thread feeds the engine the machine once it
// listens.
std::vector<std::string> RestoreSnapshot(const fs::path &folder, const fs::path &stateDir);

// RestoreSnapshot over the device's real state. A marker lives in the device's folder while the copy
// runs, so if it's cut short the half-copied state\ is noticed instead of booted (InterruptedLoad).
std::vector<std::string> LoadSaveState(const Device &device, const fs::path &folder);

// The slot whose load was cut short on this device, or "" if there's none. The device's storage is
// incomplete until it's loaded again or erased.
std::string InterruptedLoad(const Device &device);
