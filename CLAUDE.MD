# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Instructions

This is a C++ copy of [LightTouch](https://github.com/blake-lucas/LightTouch) by blake-lucas.

- `resources/` contains all the qemu-ios prebuilt DLLs and LightTouch tools. They should be all you need.
- `EXTERNAL/` contains the source of both qemu-ios (prebuilt into `resources/`) and LightTouch. Use it as a guiding reference. It is not part of the app's build. Both sources are committed, since they're the GPLv2 source of the binaries in `resources/` (both carry this project's changes, such as the iPhone 4S). `EXTERNAL/build.sh` rebuilds those binaries; its toolchain and build folders (`EXTERNAL/msys64/`, `EXTERNAL/build/`, `EXTERNAL/rust/`) are git-ignored.
- `bullshido/` (git-ignored, may be absent) is [iPhonEmu](https://github.com/kazin1898/iPhonEmu), the qemu-ios fork that first booted the iPhone 4S. Its machine is merged into `EXTERNAL/qemu-ios-main`, so it's only a reference.

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

The repository is the `.git` folder in the project root. Git itself is the bundled Portable Git in the parent folder (`..\PortableGit-2.55.0.5-64-bit.7z\`, a folder despite the name), which `..\init.bat` puts on PATH for one console. `init.bat` also sets `GIT_DIR` to that folder, and git reads `GIT_DIR` as the repository's location ("not a git repository"), so clear it first: `set GIT_DIR=` in cmd, `Remove-Item Env:GIT_DIR` in PowerShell, `unset GIT_DIR` in bash. Work lands on `master` and is pushed to `origin` (github.com/mada-02/iOS-Emulator).

### Rebuilding the binaries in `resources/`

`EXTERNAL/build.sh` builds the engine (`libqemu-arm.dll` and its DLLs), `firmwarekit.exe` and the iPhone 4S's install helpers from `EXTERNAL/`, with a portable MSYS2 (UCRT64) in `EXTERNAL/msys64`. Nothing is installed outside the project (no installer, PATH or registry changes). Set the toolchain up once: extract https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe into `EXTERNAL\` (`msys2-x86_64-latest.sfx.exe -y -o<project>\EXTERNAL\`), then run the `toolchain` step. From any shell, in the project folder:

```
EXTERNAL\msys64\usr\bin\bash.exe EXTERNAL/build.sh toolchain           (once)
EXTERNAL\msys64\usr\bin\bash.exe EXTERNAL/build.sh all                 (ffmpeg, engine, firmwarekit, helpers, install)
EXTERNAL\msys64\usr\bin\bash.exe EXTERNAL/build.sh firmwarekit install
```

The comment at the top of `build.sh` lists the steps. `install` copies the results into `resources/`, and into `build\<Config>\resources\` where that folder exists (a plain git-ignored copy, which the exe prefers), then rewrites `resources/runtime-manifest.json`: the engine's DLLs with their hashes and imports, a record nothing reads.

Everything is built against the UCRT, so the engine, its DLLs and firmwarekit are replaced together: firmwarekit boots the engine during preparation, and it crashed on exit with an engine on the older msvcrt. The libimobiledevice DLLs and `lighttouch-usbmuxd.exe` stay LightTouch's prebuilt msvcrt ones. firmwarekit's C sources need GCC, so its target is `x86_64-pc-windows-gnu`, not MSVC.

A new engine DLL changes `EngineVersion()`, so existing save states stop loading. Check `firmwarekit.log` shows a firmwarekit change took. Devices keep the kernel and system they were prepared with, so a firmwarekit fix reaches only newly prepared devices.

### Testing

The headless command-line modes are the test harness (`src/main.cpp` documents them):

```
iOS-Emulator --list
iOS-Emulator --catalog [iPod3,1]
iOS-Emulator --prepare n18ap-8C148 --jailbreak
iOS-Emulator --run <device> --seconds 120 --screenshot shot.png --tap 0.5,0.9,60 --request 90 "{\"cmd\":\"shell\",\"command\":\"ls /\"}"
```

`--run` takes `--internet` (off by default) and `--no-web-proxy` (the proxy is on by default). `--send <t> <json>` sends any worker command without waiting for a reply, e.g. `{"cmd":"type","text":"hi"}`, and `--shot <t> <file.png>` saves the screen at that time. In PowerShell 5.1, JSON with spaces gets mangled, so put such a command line in a `.cmd` file. `{"cmd":"ui.dump"}` (a `--request`) lists the front app's views with their text, which shows what typing did. A long request's progress prints with the seconds since the start. `--run` exits 0 only when iOS shut down cleanly. The iPod touch 2G on 3.1.3 boots fastest. Logs are in `%LOCALAPPDATA%\iOS-Emulator\logs\` (`app.log`, `worker-*.log`, `usbmuxd-*.log`, `firmwarekit.log`). `IOS_EMULATOR_ENGINE_ARGS` passes extra QEMU options to the engine.

**Snapshots skip the boot.** Save one once, then start test runs from it (a restored run is at its lock screen within about 3 s):

```
iOS-Emulator --run <device> --seconds 120 --save-snapshot 90 booted
iOS-Emulator --run <device> --snapshot booted --seconds 20 --tap 0.5,0.9,2 --shot 5 shot.png
```

A `--snapshot` run works on a throwaway copy of the snapshot's disk in `%TEMP%` (the device is never touched, so every run starts identical), counts `t` from the restore, and ends by quitting at once (exit 0 without an iOS shutdown). Touch works at once. The guest agent can need several seconds ("The guest tools aren't answering yet"), so check a `shell` reply before trusting a run that depends on it. USB services (`info`, `files.*`) take up to about 40 s to reconnect. After unlocking, wait about 10 s before pressing an icon, and hold it (`--swipe x,y,x,y,t,0.1`), since a `--tap` during the unlock animation is lost.

`--load-state <name>` is the same restore over the device's real state (see **Save states** below): it counts `t` from the start, and `--run` ends with a normal iOS shutdown. `--save-snapshot` slots work with both. A stock device has no `/bin/sh` or `date`, so give a `shell` request a program's full path with `"sh":false`, and read the clock from `{"cmd":"info"}` (`TimeIntervalSince1970`).

**Stutter is measured with the engine's frame trace:** `LT_FRAME_PATH=<file.tsv>` in the environment writes the vsyncs, captures, publishes and guest process switches when the engine stops (`EXTERNAL/LightTouch-main/LightTouch-main/docs/PROFILING.md`). `IOS_EMULATOR_ENGINE_ARGS="-trace enable=ipod_touch_lcd_log -msg timestamp=on"` adds the guest's panel-register writes to the worker log. `--swipe` moves the finger only about 21 times a second, so don't read frame rates above that from a drag. A frame gap means a stall only while something is moving: an idle screen publishes nothing, so run drags back to back (a 1.2 s `--swipe` takes about 1.85 s, and it rests 0.2 s at each end, which shows as a gap of about 0.2 s per turnaround). On the home screen, start drags between icon rows (y about 0.235): on an icon a stall reads as a long press, and the row of page dots takes the touch. To find a guest daemon behind a stall, stop or unload groups of them with a `shell` request in `--snapshot` runs, which are thrown away.

**The window is tested through screenshots** (environment variables, documented above `EnvText` in `ui.cpp`): `IOS_EMULATOR_UI_SHOT=<file.png>` saves the window after `IOS_EMULATOR_UI_SHOT_AFTER` seconds (default 5) and closes the app. `IOS_EMULATOR_UI_PAGE=add` opens the device grid, `IOS_EMULATOR_UI_PRODUCT=iPhone3,1` a device's version wheel, `IOS_EMULATOR_UI_START=1` starts the selected device (with `IOS_EMULATOR_UI_LANDSCAPE=1`, `IOS_EMULATOR_UI_DEVTOOLS=1`, `IOS_EMULATOR_UI_FULLSCREEN=1`), and `IOS_EMULATOR_UI_DROP=<file>` drops a file on the window after `IOS_EMULATOR_UI_DROP_AFTER` seconds. `IOS_EMULATOR_UI_SLOTS=1` opens the selected device's save states, `IOS_EMULATOR_UI_SAVE_AFTER=<s>` quick saves and `IOS_EMULATOR_UI_ASK_LOAD_AFTER=<s>` does what F9 does. Run it with `Start-Process -Wait`. A cold-booted device's display is usually off by the time it's up, so check what iOS shows with `--run` instead.

A window can be driven by posting it messages (`PostMessage`): `WM_KEYDOWN`/`WM_KEYUP`, `WM_CHAR`, and clicks, sending `WM_MOUSEMOVE`, `WM_LBUTTONDOWN` and `WM_LBUTTONUP` together, ahead of the `WM_MOUSELEAVE` that follows while the real cursor is elsewhere. Ctrl can't be posted, because the backend reads it with `GetKeyState`.

**Test on your own instance.** The user may have the app open from Visual Studio, and two instances share the data folder. So set `IOS_EMULATOR_DATA` to a folder of your own holding its own `settings.json` and a junction to one test device, find the window from the process you started (never by class with `FindWindow`), and build with `-p:OutDir=build\Test\` while their window holds `build\Release\iOS-Emulator.exe`.

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

**Keyboard modes and controls (`keymap.cpp`, `ui.cpp`):** `Settings::keyboard` is Shortcuts, Typing or Controls, and shortcuts take Ctrl in the last two. F2 and the mode badge under the device toggle Controls (`ToggleControlsMode`). In Controls mode, keys bound to controls hold fingers on the screen. A `Keymap` holds a portrait and a landscape `Layout`, in `keymap.json` in the device's folder (keys stored by name, since `ImGuiKey` numbers change between ImGui versions). A control's `spots` hold one point per finger (one for a Tap, 2 to 5 for a MultiTap), on the screen as shown, before `ToPanel`. Files written before that have `x`, `y`, `fingers` and `spread`, which `SpotsFrom` turns into a row. Each frame, `ScreenTouch` gathers the mouse, Alt and key fingers into one list, and `SendFingers` sends only the differences from `fingers_`, in a single `touch` command. Three engine rules shape that function:
- A board holds 5 fingers (`max_contacts`, the worker's `maxContacts`). `QEMU_IOS_PORTABLE_MAX_CONTACTS` (8) is the size of one command, not the number of fingers: lifts plus presses in one frame can pass it, so the rest wait a frame.
- The engine takes a command whole or not at all. A 6th finger, or a press or move while paused, loses the lifts in the same command too and leaves fingers stuck. So fingers lift while paused, and the panel's limit is applied before sending.
- Lifts go first in a command, so their slots are free for the presses after them.

`WatchHeldInput`, at the start of each frame, lifts everything when the device, mode, orientation or editor changes, when the window loses focus, or when the device stops. A control's key only counts from a press made in Controls mode, and a released hardware button stays ignored until its key comes up, so nothing presses itself again. The editor's circles are submitted before the screen's `InvisibleButton`, because the first item under the mouse gets it. `editorPopup_` keeps the keyboard from iOS while the editor's popup is open, which also stops a key being bound from acting as a shortcut.

**Developer services (`services.cpp`)** run inside the worker, not the UI. They use libimobiledevice through usbmuxd: lockdown info, AFC2 (root filesystem on jailbroken devices) or AFC (Media folder only), app install via AppSync, syslog_relay, and SSH port forwarding. The libimobiledevice DLLs are delay-loaded from `resources/`, and only worker mode calls `SetDllDirectoryW(ResourcesDir())`. Calling services from the UI process would fail to load them.

**App installs (`Services::InstallApp`, `ipa.cpp`):** the worker first reads the `.ipa` on Windows (`ReadIpa`) and refuses an app that needs a newer iOS, since installd only checks that after unpacking: Candy Crush took 5 minutes to fail on a 3GS. installd then says nothing while it unpacks, the longest step (it sits at 15%, `ExtractingPackage`), so progress comes from the fall in AFC's free space against the size `ReadIpa` added up. The upload stays on AFC: `putpart` through the guest agent looks faster but isn't, because `it_agent` takes one request per 250 ms loop. Measured on the 3GS with a 52.5 MB IPA: AFC 34 to 48 s, `putpart` 63 s at its largest chunk. `USBMUXD_QEMU_POLL_MS` made no measurable difference. The guest has no `appinst`, `ipainstaller` or `unzip`.

**Dropped files (`App::DropFiles`, `ui.cpp`):** an `.ipa` or `.deb` dropped on the window installs on the selected device, which must be running, and the banner above the page follows it. An `.ipa` is an `apps.install`. A `.deb` is a `packages.install` (`Worker::InstallPackage`), for jailbroken devices: AFC2 copies it to `/private/var/tmp`, then `kInstallDeb` runs `dpkg -i`. The guest agent ends a `spawn` within a minute (status -60) and takes `&` and `nohup` children with it, so that script runs as a `launchctl submit` job, which launchd would restart forever unless it removes itself, and the worker polls for its status file. A package with missing dependencies is removed again; an app gets `uicache` and a tweak a respring, read from the package's file list. Tested on iOS 4.2.1 and 6.1.6.

**Web proxy (`proxy.cpp`)** runs inside the worker too. firmwarekit gives iOS a proxy auto-config file that sends web traffic to `10.0.2.100:3128` first, and the engine forwards that address (a `guestfwd` in `BuildBootArgs`) to the proxy's Unix socket. The forward must always be there: with nothing answering, every new connection in iOS waits out a timeout. Modes: Off (internet off, refuses), Direct (passes bytes through), Bridge (the default: re-encrypts HTTPS, old TLS to iOS and modern TLS to the website, which Windows' trust store checks). For Bridge, the worker installs the proxy's CA in iOS through the guest agent (`put`, then the bundled `ittrust add`). The CA is kept in `%LOCALAPPDATA%\iOS-Emulator\network\<id>\proxy-ca.pem`. OpenSSL is MSYS2's `mingw-w64-ucrt-x86_64-openssl` 3.6.5, the same DLLs as in `resources/` (delay-loaded like libimobiledevice). `third_party/openssl/` holds that version's headers and import libraries made from the DLLs (`dumpbin /exports` into a `.def`, then `lib /def`; they were made from the msvcrt build, whose exports are the same). Those DLLs use another C runtime, so never pass them a `FILE*`. Use memory BIOs instead.

**Screen recording (`recorder.cpp`)** runs inside the worker, the one place that has both the frames and the sound, so it goes on while another device is on screen. The toolbar's Record button sends `{"cmd":"record","to":<file>}` and `{"cmd":"record"}`, and a `recording` event tells how it went. It writes an H.264 and AAC `.mp4` to `Videos\iOS-Emulator` with Media Foundation (built into Windows, delay-loaded because "N" editions lack it). The video has a fixed size, so it keeps the orientation the device had at the start. A thread of its own encodes at 30 fps, repeating the last frame while the screen stands still, and fills iOS's silences so the sound stays with the picture. Mute doesn't affect it.

**Snapshots (`snapshot.cpp`)** are a folder `records\<id>\snapshots\<name>\`: `machine.state` (qemu's migration stream), a copy of `state\` taken while the guest is stopped, since the disk must match the memory, then `thumbnail.png` and `slot.json` (time, orientation, size, engine). The engine's `qemu_ios_snapshot_save2` and `-incoming file:` can't work on Windows (qemu's file channel has no Windows code for blocking mode: saves fail, restores abort), so both go over local TCP: saving drives qemu's monitor (QMP), and restoring starts the engine with `-incoming tcp:` and feeds it the file. `-global migration.store-global-state=off` makes a restored machine run by itself. Saving wakes the display first, because whether it sleeps isn't in the snapshot.

The monitor gives full control of the VM. It's on 127.0.0.1, the worker opens it only with `--save-snapshots` (every window session has that), and it connects as the engine starts and stays connected, since qemu serves one client at a time. Keep that connection held.

`engine_->paused()` is just qemu's run state, so a save reads as paused, and `cont` ends it. So `Worker::SaveSnapshot` pauses a paused device again, and `saving_` stops `Shutdown` and `pause` from resuming the guest mid-save.

**Save states (`snapshot.h`, `App::LoadSlot`)** are those same folders, so the window's list shows `--save-snapshot` slots too. One without `slot.json` is hidden, though `--snapshot` still runs it, because its engine isn't known. `kQuickSlot` is the quick slot. A load can only happen as the engine starts: the UI ends the worker without shutting iOS down, and a new `--load-state <folder>` worker copies the slot over `state\` and restores it, so the window stays responsive. `<device>\loading` holds the slot's name during the copy. It lives outside `state\`, which is emptied, and if it's left over (`InterruptedLoad`) the device won't start until it's loaded again or erased. A slot loads only with the `EngineVersion()` that saved it, the sha256 of the DLL itself; the UI checks that before ending the worker. Erase deletes the slots, which it needn't for them to work (a slot only needs `base\`), by the user's choice.

A slot can't be replaced while a file in it is open, which is why `RestoreSnapshot` closes `machine.state` before waiting for the engine, which doesn't hang up. The clock needs nothing after a load: `it_agent` compares it with the host's about once a second (`agent-ops.h`) and sets it when it's over 2 s off. Only the status bar lags, until the minute changes.

**Adding a developer feature** usually touches: the request handler in `worker.cpp`, the implementation in `services.cpp`, the protocol comment in `worker.h`, and the tab in `ui.cpp`. It is then reachable from `--run --request` automatically.

**Device preparation (`firmware.cpp`):** the catalog comes from `resources/firmware-catalog.json`. `Preparation` downloads the IPSW (and a boot ROM for the oldest boards) on a background thread, then runs `resources/firmwarekit.exe`. That tool decrypts the IPSW, builds the NAND, installs `resources/guest-tools/` and, with `--jailbreak`, Cydia and AFC2. `library.cpp` then moves the output into the library. Preparations can't run side by side: each one starts by replacing `firmwarekit.log`.

**The iPhone 4S (`n94ap`, experimental):** the one A5 board (`Family::A5` in `engine.cpp`). Its machine (`-M iPhone-4S`: `hw/arm/n94.c` and `s5l8940_*.c` in the engine) is iPhonEmu's, merged into qemu-ios. It boots from firmwarekit's kboot bundle (`base\kboot.bin`) and a PPN NAND page store in `base\`, with the guest's writes in overlays in `state\nand`, on two cores (`-smp 2`). It has no Wi-Fi model and its devices don't save their state, so `HasNetwork` and `HasSaveStates` hide internet, the web proxy and save states, and the worker gets no `--save-snapshots`. `Experimental` tags it in the grid and the version wheel. The skin is the iPhone 4's.
- firmwarekit's `n94_recipe.rs` and `n94nand.rs` prepare it. The kernel only knows its own NAND layout (AppleSwissPPNFTL), so the volumes can't be written offline: the preparation boots the IPSW's restore ramdisk once in the engine, with `guest-tools/it_install-n94` as its `restored_external` and `it_keybag-n94` for the keybag (their sources and the whole procedure are in `qemu-ios-main/contrib/it-install/`, built by `build.sh helpers`). The install log is in `firmwarekit.log` as `it_install: ...` lines, ending `it_install: done`.
- Setup is skipped as on the other boards, plus Siri's page (`AssistantPresented`, `setup.rs`), which only the 4S has.
- The catalog has its builds from 5.0 to 9.3.6, with the keys from The Apple Wiki. Only 6.1.3 (10B329) is `experimental`, the rest `untested`. Tested on 6.1.3, plain and jailbroken: home screen, `shell`, `info`, AFC (AFC2 when jailbroken), typing into Notes, Cydia, clean shutdown (with Cydia's first run in front, shutdown route 1 didn't halt it and the stop timed out). Tried and failing: 5.1.1 and 7.1.2 panic in the install boot (5.1.1 a kernel abort, 7.1.2 `handleSwapCleanup - can't free 1 channels`), 8.4.1 has no activation patch in firmwarekit, and 9.3.5's OpenGLES has exports the GL front end lacks (`ca_ogl`).

**Bluetooth and SpringBoard stalls:** SpringBoard's BluetoothManager calls BTServer on its main thread, so a BTServer that keeps dying stops the whole UI: on the iPod touch 3G the screen froze for 1 s in every 2 while anything moved, though touch and the panel were fine. On jailbroken iOS 4 and 5 the kernel killed BTServer each time it started BlueTool (`Sandbox: hook..execve() killing pid N: already in a sandbox`, in `serial.log`), because BlueTool's entitlement asks for a sandbox profile of its own. Stock 4.2.1 and jailbroken 6.1.6 don't. `system_edits::unsandbox_bluetool` removes that entitlement. With it, Bluetooth also switches on in Settings. A healthy device shows `launchctl list` with a PID for `com.apple.BTServer`, not `-9`. Tested on the iPod touch 3G with 4.2.1 and the iPhone 3GS with 5.1.1, where it takes a moving drag from 13 fps with 1.1 s frozen in every 2 to 26 fps with no stall.

**Device library (`library.cpp`):** `%LOCALAPPDATA%\iOS-Emulator\library\records\<id>\` holds `record.json`, `keymap.json`, `base/`, `state/`, `assets/` and `snapshots/`. `base/` is never written after preparation. Everything the guest writes goes to `state/` (NAND overlay, NOR copy), so **Erase** just clears `state/` and `snapshots/`. The layout deliberately matches LightTouch's, so LightTouch device folders can be imported. Keep that compatibility.

Each board's boot command line is built in `BuildBootArgs` (`engine.cpp`).
