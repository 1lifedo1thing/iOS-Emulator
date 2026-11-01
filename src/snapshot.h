// Snapshots: a running machine saved to disk, so a test run can start from it in seconds instead
// of booting (iOS-Emulator --run --save-snapshot / --snapshot, see main.cpp).
//
// A snapshot is a folder, <device>\snapshots\<name>\, holding:
//   machine.state   qemu's migration stream: the guest's memory and devices
//   state\          a copy of the device's state\ (NAND overlay, NOR copy) from the same instant;
//                   the disk must match the memory, or iOS's filesystem breaks
//
// Why not the engine's qemu_ios_snapshot_save2 and "-incoming file:<path>": qemu's file channel
// can't set blocking mode on Windows (io/channel-file.c), so such a save fails and such a restore
// aborts. Both go through a local TCP stream instead: saving drives qemu's monitor (QMP) to stop
// the guest and send the machine to us; restoring starts the engine with "-incoming tcp:" and
// feeds it the file.
#pragma once

#include "common.h"

// Engine options for a run that may save snapshots: qemu's monitor on a free local port (returned
// in `port`), and no run state in the stream, so a restored machine starts running by itself.
// Anyone who can reach the monitor controls the machine, so only runs that save get one.
std::vector<std::string> MonitorArgs(int &port);

// Saves the machine (through the monitor on `monitorPort`) and the disk in `stateDir` into
// `folder`, replacing it only once complete. The guest is stopped meanwhile, and resumed
// afterwards whether the save worked or not. Throws on failure.
void SaveSnapshot(int monitorPort, const fs::path &stateDir, const fs::path &folder);

// Copies the snapshot's disk to `stateDir` (the run's throwaway state) and returns the engine
// options that restore its machine instead of booting. A background thread feeds the engine the
// machine once it listens.
std::vector<std::string> RestoreSnapshot(const fs::path &folder, const fs::path &stateDir);
