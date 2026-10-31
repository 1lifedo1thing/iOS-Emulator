# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Instructions

This is a C++ copy of [LightTouch](https://github.com/blake-lucas/LightTouch) by blake-lucas.

- `resources/` contains all the qemu-ios prebuilt DLLs and LightTouch tools. They should be all you need.
- `EXTERNAL/` contains the source of both qemu-ios (prebuilt into `resources/`) and LightTouch. Use it as a guiding reference. It is not part of the build and is git-ignored.

Do not overcomplicate the project. The code should be modularized, yet simple: not too many files or folders, clean names, clean functions, good, documented (or commented) code. It should be instantly readable by external users even without context.

The scope is a simple, clean iOS emulator. It includes jailbreaking and Cydia, so users can customize their iOS installation or do developer work. It also exposes the filesystem and the device's intricacies to developers who want to build tweaks without owning an old device.

Don't know something? Want more information? Ask. Never leave a question unasked, and never make vague changes to the codebase. Always ask clear, concise questions.

## Build and run

Windows x64 only, Visual Studio 2026 (toolset v145), C++20. There is no CMake, linter or unit test suite.

```
msbuild iOS-Emulator.vcxproj -p:Configuration=Release -p:Platform=x64   (from a Developer Command Prompt)
build\Release\iOS-Emulator.exe
```

- A new `.cpp`/`.h` must be added to the `ClCompile`/`ClInclude` lists in `iOS-Emulator.vcxproj`. The `.filters` file uses wildcards and needs no change.
- The exe finds `resources/` next to itself or in a parent folder, so it runs straight from `build\<Config>`.
- `init.bat` puts the bundled Portable Git (`PortableGit-2.55.0.5-64-bit.7z/`, a folder despite the name) on PATH for one console.

### Testing

The headless command-line modes are the test harness (`src/main.cpp` documents them):

```
iOS-Emulator --list
iOS-Emulator --catalog [iPod3,1]
iOS-Emulator --prepare n18ap-8C148 --jailbreak
iOS-Emulator --run <device> --seconds 120 --screenshot shot.png --tap 0.5,0.9,60 --request 90 "{\"cmd\":\"shell\",\"command\":\"ls /\"}"
```

`--run` exits 0 only when iOS shut down cleanly. The iPod touch 2G on 3.1.3 boots fastest. Logs are in `%LOCALAPPDATA%\iOS-Emulator\logs\` (`app.log`, `worker-*.log`, `usbmuxd-*.log`, `firmwarekit.log`). `IOS_EMULATOR_ENGINE_ARGS` passes extra QEMU options to the engine.

## Architecture

One executable, several modes (`wWinMain` in `src/main.cpp`): no arguments opens the ImGui window (`ui.cpp`), `--list/--run/--catalog/--prepare/--import` are console modes, and `--worker` runs a single device.

**Why there is a worker process:** the engine (`resources/libqemu-arm.dll`, qemu-ios with a C API declared in `third_party/qemu-ios/*.h`) can start only once per process and can't be unloaded. So each running device is its own `iOS-Emulator.exe --worker` process. `Engine` in `engine.h` resolves the `qemu_ios_*` functions by name. To use a new engine function, add it there.

**A running device (`Session`, `session.cpp`)** launches two children, both in a job object so they die with the app:
1. `lighttouch-usbmuxd.exe`, the emulated USB cable. The guest's USB port connects to it.
2. The worker. It receives the usbmuxd address in its environment.

**UI ↔ worker channels:**
- Commands go to the worker's stdin and events come back on an inherited pipe, one JSON object per line. The full protocol is in the comment at the top of `src/worker.h`. Keep that comment in sync when you add a command or event.
- Screen frames go through a named file mapping (`SharedFrame` in `common.h`, BGRA pixels, with a seqlock `sequence`).
- Developer requests carry an `id`. `Session::Request` calls the reply callback from `Update()` on the caller's thread.

**Developer services (`services.cpp`)** run inside the worker, not the UI. They use libimobiledevice through usbmuxd: lockdown info, AFC2 (root filesystem on jailbroken devices) or AFC (Media folder only), app install via AppSync, syslog_relay, and SSH port forwarding. The libimobiledevice DLLs are delay-loaded from `resources/`, and only worker mode calls `SetDllDirectoryW(ResourcesDir())`. Calling services from the UI process would fail to load them.

**Adding a developer feature** usually touches: the request handler in `worker.cpp`, the implementation in `services.cpp`, the protocol comment in `worker.h`, and the tab in `ui.cpp`. It is then reachable from `--run --request` automatically.

**Device preparation (`firmware.cpp`):** the catalog comes from `resources/firmware-catalog.json`. `Preparation` downloads the IPSW (and a boot ROM for the oldest boards) on a background thread, then runs `resources/firmwarekit.exe`. That tool decrypts the IPSW, builds the NAND, installs `resources/guest-tools/` and, with `--jailbreak`, Cydia and AFC2. `library.cpp` then moves the output into the library.

**Device library (`library.cpp`):** `%LOCALAPPDATA%\iOS-Emulator\library\records\<id>\` holds `record.json`, `base/`, `state/` and `assets/`. `base/` is never written after preparation. Everything the guest writes goes to `state/` (NAND overlay, NOR copy), so **Erase** just clears `state/`. The layout deliberately matches LightTouch's, so LightTouch device folders can be imported. Keep that compatibility.

Each board's boot command line is built in `BuildBootArgs` (`engine.cpp`).
