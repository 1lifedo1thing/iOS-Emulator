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

### Rebuilding `resources/firmwarekit.exe`

It is built from `EXTERNAL/LightTouch-main` with a portable Rust (`EXTERNAL/rust/toolchain`) and MinGW GCC (`EXTERNAL/rust/mingw`), installed only inside the project (no installer, PATH or registry changes). Its C sources need GCC, so the target is `x86_64-pc-windows-gnu`, not MSVC. From Git Bash, in `EXTERNAL/LightTouch-main/LightTouch-main`:

```
R=<project>/EXTERNAL/rust
export CARGO_HOME=$R/cargo-home CARGO_TARGET_DIR=$R/target PATH="$R/mingw/bin:$R/toolchain/bin:$PATH" \
       CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=gcc CC_x86_64_pc_windows_gnu=gcc AR_x86_64_pc_windows_gnu=ar
cargo build --release --target x86_64-pc-windows-gnu -p lighttouch-firmware --bin firmwarekit
```

Then copy `$R/target/x86_64-pc-windows-gnu/release/firmwarekit.exe` to `resources/`. Devices keep the kernel they were prepared with, so a firmwarekit fix reaches only newly prepared devices.

### Testing

The headless command-line modes are the test harness (`src/main.cpp` documents them):

```
iOS-Emulator --list
iOS-Emulator --catalog [iPod3,1]
iOS-Emulator --prepare n18ap-8C148 --jailbreak
iOS-Emulator --run <device> --seconds 120 --screenshot shot.png --tap 0.5,0.9,60 --request 90 "{\"cmd\":\"shell\",\"command\":\"ls /\"}"
```

`--run` takes `--internet` (off by default) and `--no-web-proxy` (the proxy is on by default). `--send <t> <json>` sends any worker command without waiting for a reply, e.g. `{"cmd":"type","text":"hi"}`, and `--shot <t> <file.png>` saves the screen at that time. In PowerShell 5.1, JSON with spaces gets mangled, so put such a command line in a `.cmd` file. `iOS-Emulator --keyboard shot.png` prints what typing reads off a screenshot (the keyboard's rows and keys, page, Shift). When typing finds no keyboard, the frame it looked at is saved as `keyboard-not-found.png` in the logs folder. `--run` exits 0 only when iOS shut down cleanly. The iPod touch 2G on 3.1.3 boots fastest. Logs are in `%LOCALAPPDATA%\iOS-Emulator\logs\` (`app.log`, `worker-*.log`, `usbmuxd-*.log`, `firmwarekit.log`). `IOS_EMULATOR_ENGINE_ARGS` passes extra QEMU options to the engine.

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

**Typing (`OnScreenKeyboard`, `keyboard.cpp`)** taps the keys of iOS's own on-screen keyboard, on every board and iOS version. No other route works everywhere: only the A4 boards and the iPad have a USB host for a keyboard, and only the iPod touch 2G's guest package has `it_typein` (the guest agent's `type` op times out elsewhere, -138). The worker reads the keyboard off the screen frame before it types and again whenever a tap may have changed it: the rows of keys along the interface's bottom (at any of the four turns of the panel), the design (iPhone or iPad, by the top row's key count), the page (letters, by row 2's key count; numbers and symbols look alike, so it tracks those itself) and Shift (lit, or a blue arrow on the iPad). Before each read it waits for the guest to take the taps sent (`qemu_ios_portable_input_status`) and for the screen to settle, since the guest can lag well behind. The character tables per page are `kPhone` and `kPad`. Tested on the iPod touch 2G (3.1.3), iPhone 3GS (6.1.3), iPhone 4 (7.1.2) and iPad (5.1.1).

**Developer services (`services.cpp`)** run inside the worker, not the UI. They use libimobiledevice through usbmuxd: lockdown info, AFC2 (root filesystem on jailbroken devices) or AFC (Media folder only), app install via AppSync, syslog_relay, and SSH port forwarding. The libimobiledevice DLLs are delay-loaded from `resources/`, and only worker mode calls `SetDllDirectoryW(ResourcesDir())`. Calling services from the UI process would fail to load them.

**Web proxy (`proxy.cpp`)** runs inside the worker too. firmwarekit gives iOS a proxy auto-config file that sends web traffic to `10.0.2.100:3128` first, and the engine forwards that address (a `guestfwd` in `BuildBootArgs`) to the proxy's Unix socket. The forward must always be there: with nothing answering, every new connection in iOS waits out a timeout. Modes: Off (internet off, refuses), Direct (passes bytes through), Bridge (the default: re-encrypts HTTPS, old TLS to iOS and modern TLS to the website, which Windows' trust store checks). For Bridge, the worker installs the proxy's CA in iOS through the guest agent (`put`, then the bundled `ittrust add`). The CA is kept in `%LOCALAPPDATA%\iOS-Emulator\network\<id>\proxy-ca.pem`. OpenSSL is MSYS2's `mingw-w64-x86_64-openssl` 3.6.5, the same DLLs as in `resources/` (delay-loaded like libimobiledevice). `third_party/openssl/` holds that package's headers and import libraries made from the DLLs (`dumpbin /exports` into a `.def`, then `lib /def`). Those DLLs use another C runtime, so never pass them a `FILE*`. Use memory BIOs instead.

**Adding a developer feature** usually touches: the request handler in `worker.cpp`, the implementation in `services.cpp`, the protocol comment in `worker.h`, and the tab in `ui.cpp`. It is then reachable from `--run --request` automatically.

**Device preparation (`firmware.cpp`):** the catalog comes from `resources/firmware-catalog.json`. `Preparation` downloads the IPSW (and a boot ROM for the oldest boards) on a background thread, then runs `resources/firmwarekit.exe`. That tool decrypts the IPSW, builds the NAND, installs `resources/guest-tools/` and, with `--jailbreak`, Cydia and AFC2. `library.cpp` then moves the output into the library.

**Device library (`library.cpp`):** `%LOCALAPPDATA%\iOS-Emulator\library\records\<id>\` holds `record.json`, `base/`, `state/` and `assets/`. `base/` is never written after preparation. Everything the guest writes goes to `state/` (NAND overlay, NOR copy), so **Erase** just clears `state/`. The layout deliberately matches LightTouch's, so LightTouch device folders can be imported. Keep that compatibility.

Each board's boot command line is built in `BuildBootArgs` (`engine.cpp`).
