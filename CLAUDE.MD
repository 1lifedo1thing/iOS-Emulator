# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Instructions

This is a C++ copy of [LightTouch](https://github.com/blake-lucas/LightTouch) by blake-lucas.

- `resources/` contains all the qemu-ios prebuilt DLLs and LightTouch tools. They should be all you need.
- `EXTERNAL/` contains the source of both qemu-ios (prebuilt into `resources/`) and LightTouch. Use it as a guiding reference. It is not part of the build. Both sources are committed, since they're the GPLv2 source of the binaries in `resources/` (LightTouch carries this project's firmwarekit changes). `EXTERNAL/rust/` (the toolchains) is git-ignored.

Do not overcomplicate the project. The code should be modularized, yet simple: not too many files or folders, clean names, clean functions, good, documented (or commented) code. It should be instantly readable by external users even without context.

The scope is a simple, clean iOS emulator. It includes jailbreaking and Cydia, so users can customize their iOS installation or do developer work. It also exposes the filesystem and the device's intricacies to developers who want to build tweaks without owning an old device.

Don't know something? Want more information? Ask. Never leave a question unasked, and never make vague changes to the codebase. Always ask clear, concise questions.

## Build and run

Windows x64 only, Visual Studio 2026 (toolset v145), C++20. There is no CMake, linter or unit test suite.

```
msbuild iOS-Emulator.vcxproj -p:Configuration=Release -p:Platform=x64   (from a Developer Command Prompt)
build\Release\iOS-Emulator.exe
```

From plain PowerShell, find MSBuild first: `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -find "MSBuild\**\Bin\MSBuild.exe"`.

- A new `.cpp`/`.h` must be added to the `ClCompile`/`ClInclude` lists in `iOS-Emulator.vcxproj`. The `.filters` file uses wildcards and needs no change.
- The exe finds `resources/` next to itself or in a parent folder, so it runs straight from `build\<Config>`.

### Git

There is no `.git` folder in the root: the repository lives inside the bundled Portable Git folder (`PortableGit-2.55.0.5-64-bit.7z/`, a folder despite the name). `init.bat` sets `GIT_DIR` to it and puts its `git` on PATH for one console; without it git reports "not a git repository". From PowerShell: `cmd /c "call <project>\init.bat >nul && git status"` (call it by full path). Work lands on `master` and is pushed to `origin` (github.com/mada-02/iOS-Emulator).

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

`--run` takes `--internet` (off by default) and `--no-web-proxy` (the proxy is on by default). `--send <t> <json>` sends any worker command without waiting for a reply, e.g. `{"cmd":"type","text":"hi"}`, and `--shot <t> <file.png>` saves the screen at that time. In PowerShell 5.1, JSON with spaces gets mangled, so put such a command line in a `.cmd` file. `{"cmd":"ui.dump"}` (a `--request`) lists the front app's views with their text, which shows what typing did. `--run` exits 0 only when iOS shut down cleanly. The iPod touch 2G on 3.1.3 boots fastest. Logs are in `%LOCALAPPDATA%\iOS-Emulator\logs\` (`app.log`, `worker-*.log`, `usbmuxd-*.log`, `firmwarekit.log`). `IOS_EMULATOR_ENGINE_ARGS` passes extra QEMU options to the engine.

**Snapshots skip the boot.** Save one once, then start test runs from it (a restored run is at its lock screen within about 3 s):

```
iOS-Emulator --run <device> --seconds 120 --save-snapshot 90 booted
iOS-Emulator --run <device> --snapshot booted --seconds 20 --tap 0.5,0.9,2 --shot 5 shot.png
```

A `--snapshot` run works on a throwaway copy of the snapshot's disk in `%TEMP%` (the device is never touched, so every run starts identical), counts `t` from the restore, and ends by quitting at once (exit 0 without an iOS shutdown). Touch and the guest agent work at once; USB services (`info`, `files.*`) take up to about 40 s to reconnect.

**The window is tested through screenshots** (environment variables, documented above `EnvText` in `ui.cpp`): `IOS_EMULATOR_UI_SHOT=<file.png>` saves the window after `IOS_EMULATOR_UI_SHOT_AFTER` seconds (default 5) and closes the app. `IOS_EMULATOR_UI_PAGE=add` opens the device grid, `IOS_EMULATOR_UI_PRODUCT=iPhone3,1` a device's version wheel, and `IOS_EMULATOR_UI_START=1` starts the selected device (with `IOS_EMULATOR_UI_LANDSCAPE=1`, `IOS_EMULATOR_UI_DEVTOOLS=1`, `IOS_EMULATOR_UI_FULLSCREEN=1`). Run it with `Start-Process -Wait`. A cold-booted device's display is usually off by the time it's up, and the window can't be clicked from a script, so check what iOS shows with `--run` instead.

## Architecture

One executable, several modes (`wWinMain` in `src/main.cpp`): no arguments opens the ImGui window (`ui.cpp`), `--list/--run/--catalog/--prepare/--import` are console modes, and `--worker` runs a single device.

The window has a sidebar of devices, which the user wants left as it is, and pages:
- **A device:** its picture with working buttons, an icon toolbar under it, and the developer tools behind a tab at its right edge (hidden by default, `Settings::devTools`).
- **Add Device:** a grid of products, then a drum wheel of versions (`DrumPicker`).
- **Settings.**

The toolbar icons are Segoe MDL2 Assets glyphs merged into the UI font (`icon::` in `ui.cpp`). Without that font the buttons fall back to text.

**Why there is a worker process:** the engine (`resources/libqemu-arm.dll`, qemu-ios with a C API declared in `third_party/qemu-ios/*.h`) can start only once per process and can't be unloaded. So each running device is its own `iOS-Emulator.exe --worker` process. `Engine` in `engine.h` resolves the `qemu_ios_*` functions by name. To use a new engine function, add it there.

**A running device (`Session`, `session.cpp`)** launches two children, both in a job object so they die with the app:
1. `lighttouch-usbmuxd.exe`, the emulated USB cable. The guest's USB port connects to it.
2. The worker. It receives the usbmuxd address in its environment.

**UI ↔ worker channels:**
- Commands go to the worker's stdin and events come back on an inherited pipe, one JSON object per line. The full protocol is in the comment at the top of `src/worker.h`. Keep that comment in sync when you add a command or event.
- Screen frames go through a named file mapping (`SharedFrame` in `common.h`, BGRA pixels, with a seqlock `sequence`).
- Developer requests carry an `id`. `Session::Request` calls the reply callback from `Update()` on the caller's thread.

The worker answers only commands that carry an `id`. Input sent before iOS is up (touch, buttons) is dropped silently, since the UI sends it without waiting.

**Device pictures (`skin.cpp`, `resources/skins/*.svg`):** each board is drawn from an SVG front view in tenths of a millimetre (`SkinFile` maps boards to files; the iPod touch 3G reuses the 2G's). It's rasterized with nanosvg (`third_party/nanosvg`) at the size it's shown, in three layers: body, buttons and glare. The app reads only these element ids (matched up to the first `.`, and a group passes its id to its shapes):
- `body`, `screen` (exactly the panel);
- `home`, `power`, `volume-up`, `volume-down`;
- `glare`.

Side buttons are drawn under the body and sink into it when pressed. nanosvg ends a comment at the first `>`, so SVG comments must not contain one. Its other limits: no filters, masks, clip paths, text or `<use>`.

**Landscape** turns the picture a quarter turn anticlockwise (Home on the right). `ToPanel` (`skin.h`) maps shown screen points to the panel's own scan, together with the iPad's already `turned` panel. It drives both the frame's texture corners and touches. iOS is told through the accelerometer (`Motion` in `ui.cpp`):
- landscape stands the device on its side, `x = +g` (checked: Safari on 3.1.3 lays out the right way up);
- portrait lies flat, after one second upright so iOS turns back.

The engine drops a motion sample after 250 ms, so the worker repeats the last one every 100 ms (`PublishThread`).

**Typing (`Worker::TypeText`, `worker.cpp`)** goes through `it_typein` (`resources/guest-tools/it_typein.dylib`, one fat armv6 + armv7 build for iOS 3 to 7), a library loaded into every app and SpringBoard. The guest agent routes its `type` (UTF-8 text, inserted into the focused field; `\n` is Return), `backspace` and `uidump` ops to it in the app in front, which answers from its main run loop. Queued text goes out in one request, so fast typing doesn't fall behind. A failure becomes a `typing` event (`InputError` maps the statuses: locked, no focused field, -138 when no `it_typein` answers). firmwarekit installs it (`system_edits::install_typein`), and the loader (`TypeinLoader`) depends on the iOS version: a Substrate extension filtered to UIKit on jailbroken 3.x to 6.x (no `DYLD_` variable for Cydia's cydo to refuse); SpringBoard's `DYLD_INSERT_LIBRARIES` on 2.x and 3.x, whose apps inherit SpringBoard's environment; on 4.x and later SpringBoard's job plus the boot job `com.qemu.it-typein`, which runs `launchctl setenv DYLD_INSERT_LIBRARIES` so launchd hands it to every app. `/etc/launchd.conf` doesn't work: 7.x's launchd ignores it, and a variable set for every process from boot on hangs 3.1.3. The guest package's seed (`guest_package::seed`) makes the guest tools' build the package's hook of `/usr/lib/it_typein.dylib`, since `it_boot` keeps the package's hooks in place (2.x keeps the itpack's old build). Devices prepared before this don't have it and must be prepared again. Tested on the iPod touch 2G (3.1.3), iPhone 3GS (6.1.3, plain and jailbroken) and iPhone 4 (7.1.2).

**Developer services (`services.cpp`)** run inside the worker, not the UI. They use libimobiledevice through usbmuxd: lockdown info, AFC2 (root filesystem on jailbroken devices) or AFC (Media folder only), app install via AppSync, syslog_relay, and SSH port forwarding. The libimobiledevice DLLs are delay-loaded from `resources/`, and only worker mode calls `SetDllDirectoryW(ResourcesDir())`. Calling services from the UI process would fail to load them.

**Web proxy (`proxy.cpp`)** runs inside the worker too. firmwarekit gives iOS a proxy auto-config file that sends web traffic to `10.0.2.100:3128` first, and the engine forwards that address (a `guestfwd` in `BuildBootArgs`) to the proxy's Unix socket. The forward must always be there: with nothing answering, every new connection in iOS waits out a timeout. Modes: Off (internet off, refuses), Direct (passes bytes through), Bridge (the default: re-encrypts HTTPS, old TLS to iOS and modern TLS to the website, which Windows' trust store checks). For Bridge, the worker installs the proxy's CA in iOS through the guest agent (`put`, then the bundled `ittrust add`). The CA is kept in `%LOCALAPPDATA%\iOS-Emulator\network\<id>\proxy-ca.pem`. OpenSSL is MSYS2's `mingw-w64-x86_64-openssl` 3.6.5, the same DLLs as in `resources/` (delay-loaded like libimobiledevice). `third_party/openssl/` holds that package's headers and import libraries made from the DLLs (`dumpbin /exports` into a `.def`, then `lib /def`). Those DLLs use another C runtime, so never pass them a `FILE*`. Use memory BIOs instead.

**Screen recording (`recorder.cpp`)** runs inside the worker, the one place that has both the frames and the sound, so it goes on while another device is on screen. The toolbar's Record button sends `{"cmd":"record","to":<file>}` and `{"cmd":"record"}`, and a `recording` event tells how it went. It writes an H.264 and AAC `.mp4` to `Videos\iOS-Emulator` with Media Foundation (built into Windows, delay-loaded because "N" editions lack it). The video has a fixed size, so it keeps the orientation the device had at the start. A thread of its own encodes at 30 fps, repeating the last frame while the screen stands still, and fills iOS's silences so the sound stays with the picture. Mute doesn't affect it.

**Snapshots (`snapshot.cpp`)** are a folder `records\<id>\snapshots\<name>\`: `machine.state` (qemu's migration stream) and a copy of `state\` taken while the guest is stopped, since the disk must match the memory. The engine's `qemu_ios_snapshot_save2` and `-incoming file:` can't work on Windows (qemu's file channel has no Windows code for blocking mode: saves fail, restores abort), so both go over local TCP: saving drives qemu's monitor (QMP), which the worker opens only with `--save-snapshots` since it gives full control of the VM, and restoring starts the engine with `-incoming tcp:` and feeds it the file. `-global migration.store-global-state=off` makes a restored machine run by itself. Saving wakes the display first, because whether it sleeps isn't in the snapshot.

**Adding a developer feature** usually touches: the request handler in `worker.cpp`, the implementation in `services.cpp`, the protocol comment in `worker.h`, and the tab in `ui.cpp`. It is then reachable from `--run --request` automatically.

**Device preparation (`firmware.cpp`):** the catalog comes from `resources/firmware-catalog.json`. `Preparation` downloads the IPSW (and a boot ROM for the oldest boards) on a background thread, then runs `resources/firmwarekit.exe`. That tool decrypts the IPSW, builds the NAND, installs `resources/guest-tools/` and, with `--jailbreak`, Cydia and AFC2. `library.cpp` then moves the output into the library.

**Device library (`library.cpp`):** `%LOCALAPPDATA%\iOS-Emulator\library\records\<id>\` holds `record.json`, `base/`, `state/`, `assets/` and `snapshots/`. `base/` is never written after preparation. Everything the guest writes goes to `state/` (NAND overlay, NOR copy), so **Erase** just clears `state/`. The layout deliberately matches LightTouch's, so LightTouch device folders can be imported. Keep that compatibility.

Each board's boot command line is built in `BuildBootArgs` (`engine.cpp`).
