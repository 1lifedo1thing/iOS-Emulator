# iOS Emulator

Runs classic iPhone OS and iOS (1.0 to 7.1) on Windows: the iPod touch, iPhone 3GS, iPhone 4 and iPad,
emulated with [qemu-ios](https://github.com/blake-lucas/qemu-ios). Devices can be **jailbroken with Cydia**
when they're prepared, and the app exposes the device to developers: its whole filesystem, a root
terminal, app installs, the system log and SSH, so tweaks can be built and tested without the hardware.

It is a small C++ take on [LightTouch](https://github.com/blake-lucas/LightTouch): the same engine and
preparation tool, driven by a Dear ImGui app.

## Requirements

- Windows 10 (1903 or later) or 11, 64-bit
- A GPU with a real OpenGL driver (the guest's 3D graphics run on it)
- Internet access to download iOS (from Apple or archive.org) and, for jailbroken devices, Cydia's files
- To build: Visual Studio 2026 with the C++ desktop workload (toolset v145)

## Building and running

Open `iOS-Emulator.slnx` in Visual Studio and build **x64**, or from a Developer Command Prompt:

```
msbuild iOS-Emulator.vcxproj -p:Configuration=Release -p:Platform=x64
build\Release\iOS-Emulator.exe
```

The program finds the `resources` folder next to it or in a parent folder, so it runs straight from
`build\Release`. To ship it, copy `iOS-Emulator.exe` and `resources\` into one folder.

## Using it

1. **Add a device**: pick a device and an iOS version (versions marked *tested* run best; the iPod touch
   2G on 3.1.3 is the fastest). Leave **Jailbreak** on to get Cydia, apt, bash and root file access.
   The app downloads the IPSW and prepares the device; it takes one to a few minutes.
   Devices made by LightTouch can be imported from the same page.
2. **Start** it. The mouse is your finger:

   | Input | Does |
   | --- | --- |
   | Click, drag | Tap, swipe |
   | Alt + drag | Pinch or rotate with two fingers |
   | H / P | Home / Sleep-Wake (held while the key is) |
   | + / - | Volume |
   | Arrow keys | Tilt the device (for games steered by tilting) |
   | The box under the device | Types text into iOS |

3. **Stop** shuts iOS down cleanly, like sliding to power off. Each device keeps its own storage; the
   prepared system is never changed, and **Erase** brings a device back to it.

Turn **Internet** on (per session, or by default in Settings) for Safari and Cydia. Plain `http` sites
and Cydia's repositories work; modern `https` sites mostly don't, as old iOS can't speak today's TLS.

## Developer tools

While a device runs, the panel beside it has:

| Tab | What it does |
| --- | --- |
| Info | Everything lockdown reports (model, iOS build, UDID, serial, ECID, MACs, disk space) and the kernel log |
| Files | Browse, download, upload, rename and delete files. Jailbroken devices show the whole filesystem as root (AFC2); others only the Media folder |
| Apps | Installed apps, open one, install an `.ipa` (AppSync is preinstalled, so unsigned apps install too) |
| Terminal | Runs commands as root through the guest agent (`/bin/sh` on jailbroken devices), no network needed |
| Logs | The live system log (syslog), with a filter |
| SSH | Forwards a local port to the device's port 22 over USB: `ssh root@localhost -p 2222` (install OpenSSH from Cydia first; the password is `alpine`) |

Because the services speak the normal iOS protocols through the emulated USB cable, other
libimobiledevice-based tools work against the same device while the app runs.

### Command line

Everything also works without the window, for scripts and automated tests:

```
iOS-Emulator --list                                    devices in the library
iOS-Emulator --catalog [iPod3,1]                       firmwares that can be prepared
iOS-Emulator --prepare n18ap-8C148 --jailbreak         prepares a device (--name, --ipsw FILE)
iOS-Emulator --import <LightTouch device folder>       copies a LightTouch device in
iOS-Emulator --run <device> [--seconds N] [--screenshot shot.png] [--internet]
             [--tap x,y,t] [--swipe x,y,toX,toY,t[,seconds]] [--request t '<json>']
```

`--run` boots a device, performs the taps, swipes and developer requests at the given times (screen
coordinates from 0 to 1, times in seconds since start), saves a screenshot and shuts down cleanly.
Requests are the worker commands listed in `src/worker.h`, for example:

```
iOS-Emulator --run "My iPod" --seconds 120 --request 90 "{\"cmd\":\"shell\",\"command\":\"ls /var/mobile\"}"
```

For engine debugging, `IOS_EMULATOR_ENGINE_ARGS` adds QEMU options (for instance
`-trace enable=ipod_touch_multitouch_log`).

## How it works

```
iOS-Emulator.exe (the window)                 iOS-Emulator.exe --worker (one per running device)
  library: devices on disk                      engine: libqemu-arm.dll runs iOS
  firmware: downloads, firmwarekit.exe          frames -> shared memory -> the window
  session: one running device  ---------------> sound -> Windows
     starts lighttouch-usbmuxd.exe (USB)        services: files, apps, syslog, SSH (libimobiledevice)
     reads frames, sends input and requests     commands on stdin, events on a pipe (JSON lines)
```

- The engine is a build of QEMU with a C API (`third_party/qemu-ios`). It can start only once per
  process, so each running device gets a worker process; stopping one never affects the app.
- `firmwarekit.exe` turns Apple's IPSW into a device: it decrypts it, builds the NAND image, installs the
  guest tools (an agent, AppSync, the OpenGL ES front end) and, with `--jailbreak`, Cydia and AFC2.
- `lighttouch-usbmuxd.exe` plays the USB cable: the guest's USB port connects to it, and so does
  libimobiledevice for the developer services.

### Source files

| File | What it does |
| --- | --- |
| `src/main.cpp` | Entry point and the command-line modes |
| `src/ui.cpp` | The window: device list, screen and controls, Add Device page, developer panel, settings |
| `src/session.cpp` | A running device as the window sees it: starts the helpers, reads frames and events |
| `src/worker.cpp` | The worker process: runs the engine, sound, commands |
| `src/engine.cpp` | The engine's C API and each board's boot command line |
| `src/services.cpp` | Developer services over USB (libimobiledevice) |
| `src/firmware.cpp` | The firmware catalog, downloads and firmwarekit |
| `src/library.cpp` | Devices on disk |
| `src/common.cpp` | Shared helpers: folders, files, hashing, child processes, shared frames |

### Where things are

`%LOCALAPPDATA%\iOS-Emulator` holds `library\records\<id>\` (one folder per device: `base\` as prepared,
`state\` with everything it wrote, `record.json`), `pairing\` (USB pairing records), `logs\`
(`app.log`, `worker-*.log`, `usbmuxd-*.log`, `firmwarekit.log`) and `settings.json`. Screenshots go to
`Pictures\iOS-Emulator`.

## Credits and license

Built on [LightTouch](https://github.com/blake-lucas/LightTouch) by Blake Lucas (its engine, firmwarekit,
guest tools and firmware catalog, in `resources\`), [qemu-ios](https://github.com/blake-lucas/qemu-ios),
[QEMU](https://www.qemu.org), [libimobiledevice](https://libimobiledevice.org) and
[Dear ImGui](https://github.com/ocornut/imgui), with [nlohmann/json](https://github.com/nlohmann/json).
The licenses of the bundled components are in `resources\licenses` and `third_party`.

Like LightTouch, this program is free software under the GNU GPL, version 2 or (at your option) any
later version: see `LICENSE`.

iPhone, iPod touch and iPad are trademarks of Apple Inc. This project is not affiliated with Apple.
