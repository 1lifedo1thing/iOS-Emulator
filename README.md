# iOS Emulator

Runs classic iPhone OS and iOS (1.0 to 7.1) on Windows: the iPod touch, iPhone 3GS, iPhone 4 and iPad,
emulated with [qemu-ios](https://github.com/blake-lucas/qemu-ios). Devices can be **jailbroken with Cydia**
when they're prepared, and the app exposes the device to developers: its whole filesystem, a root
terminal, app installs, the system log and SSH, so tweaks can be built and tested without the hardware.

It is a small C++ take on [LightTouch](https://github.com/blake-lucas/LightTouch): the same engine and
preparation tool, driven by a Dear ImGui app.

It is heavily WIP and still has bugs.

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
   | F11 | Fullscreen (F11 or Esc leaves it) |

3. **Stop** shuts iOS down cleanly, like sliding to power off. Each device keeps its own storage; the
   prepared system is never changed, and **Erase** brings a device back to it.

**Fullscreen** (the button or F11) shows only the device's screen, as large as the monitor allows.
Moving the mouse brings up controls for volume, Home, Sleep/Wake and leaving fullscreen; they fade
out a few seconds after the mouse stops.

**Typing**: tap a text field in iOS, then type in the box under the device. The text goes straight
into the field through `it_typein`, a small library the guest tools load into every app (iOS 3 to 7),
so any character works and Enter acts as Return. With **Send keyboard input to iOS** on (in Settings),
what you type while a device is on screen (characters, Backspace and Enter) goes to iOS the same way,
and the shortcuts above take Ctrl (Ctrl+H for Home, Ctrl with the arrow keys to tilt, and so on).
Devices prepared before typing used `it_typein` don't have it: prepare them again to type.

### Internet and the web proxy

Turn **Internet** on (per session, or by default in Settings) for Safari and Cydia. Old iOS only
knows outdated encryption, which today's `https` sites refuse. The **Web proxy** (on by default,
switchable while a device runs) handles that: iOS's web traffic goes to a proxy on your computer,
which speaks the old encryption to iOS and modern encryption to the website, checking the site's
certificate against Windows' own trust store. The device trusts the proxy through a certificate the
app installs in it (each device gets its own, kept in `%LOCALAPPDATA%\iOS-Emulator\network\<id>\`).
With the web proxy off, bytes pass through untouched and iOS talks to websites itself, so mostly plain
`http` sites and Cydia's repositories work. The proxy never connects to your own computer or local
network.

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
iOS-Emulator --run <device> [--seconds N] [--screenshot shot.png] [--internet] [--no-web-proxy]
             [--tap x,y,t] [--swipe x,y,toX,toY,t[,seconds]]
             [--request t '<json>'] [--send t '<json>'] [--shot t shot.png]
             [--save-snapshot t NAME] [--snapshot NAME]
```

`--run` boots a device, performs the taps, swipes and commands at the given times (screen
coordinates from 0 to 1, times in seconds since start), saves screenshots (`--shot` at a given time,
`--screenshot` at the end) and shuts down cleanly. It exits 0 only when iOS shut down cleanly.
Internet is off unless `--internet` is given, and the web proxy is on unless `--no-web-proxy` is.

Commands are the worker commands listed in `src/worker.h`. `--request` sends a developer request and
prints its reply; `--send` sends any command without waiting, for example
`{"cmd":"type","text":"hi"}`. `{"cmd":"ui.dump"}` lists the views of the app in front, with their text:

```
iOS-Emulator --run "My iPod" --seconds 120 --request 90 "{\"cmd\":\"shell\",\"command\":\"ls /var/mobile\"}"
```

To test without waiting for a boot, save the running machine once with `--save-snapshot 90 booted`
(at 90 s, under the name `booted`), then start later runs with `--snapshot booted`: iOS is back where
it was within seconds. Such a run works on a throwaway copy of the snapshot's storage, so the device
itself is never changed and every run starts the same.

In Windows PowerShell 5.1, JSON containing spaces gets mangled on the way to the program; put such a
command line in a `.cmd` file instead.

For engine debugging, `IOS_EMULATOR_ENGINE_ARGS` adds QEMU options (for instance
`-trace enable=ipod_touch_multitouch_log`).

## How it works

```
iOS-Emulator.exe (the window)                 iOS-Emulator.exe --worker (one per running device)
  library: devices on disk                      engine: libqemu-arm.dll runs iOS
  firmware: downloads, firmwarekit.exe          frames -> shared memory -> the window
  session: one running device  ---------------> sound -> Windows
     starts lighttouch-usbmuxd.exe (USB)        services: files, apps, syslog, SSH (libimobiledevice)
     reads frames, sends input and requests     web proxy: iOS's HTTPS <-> modern TLS (OpenSSL)
                                                commands on stdin, events on a pipe (JSON lines)
```

- The engine is a build of QEMU with a C API (`third_party/qemu-ios`). It can start only once per
  process, so each running device gets a worker process; stopping one never affects the app.
- `firmwarekit.exe` turns Apple's IPSW into a device: it decrypts it, builds the NAND image, installs the
  guest tools (an agent, `it_typein` for typing, AppSync, the OpenGL ES front end) and, with
  `--jailbreak`, Cydia and AFC2.
- `lighttouch-usbmuxd.exe` plays the USB cable: the guest's USB port connects to it, and so does
  libimobiledevice for the developer services.
- Prepared devices carry a proxy auto-config file that sends web traffic to `10.0.2.100:3128`. The
  engine always forwards that address to the worker's web proxy (`src/proxy.cpp`), so it answers at
  once even with the proxy off; with nothing there, every new connection in iOS would wait out a
  timeout.

### Source files

| File | What it does |
| --- | --- |
| `src/main.cpp` | Entry point and the command-line modes |
| `src/ui.cpp` | The window: device list, screen and controls, fullscreen, Add Device page, developer panel, settings |
| `src/session.cpp` | A running device as the window sees it: starts the helpers, reads frames and events |
| `src/worker.cpp` | The worker process: runs the engine, sound, commands, typing (through `it_typein`) |
| `src/engine.cpp` | The engine's C API and each board's boot command line |
| `src/services.cpp` | Developer services over USB (libimobiledevice) |
| `src/proxy.cpp` | The guest's web proxy (OpenSSL) |
| `src/firmware.cpp` | The firmware catalog, downloads and firmwarekit |
| `src/library.cpp` | Devices on disk |
| `src/snapshot.cpp` | Saving a running machine and starting from it (for quick test runs) |
| `src/common.cpp` | Shared helpers: folders, files, hashing, child processes, shared frames |

### Where things are

`%LOCALAPPDATA%\iOS-Emulator` holds `library\records\<id>\` (one folder per device: `base\` as prepared,
`state\` with everything it wrote, `snapshots\`, `record.json`), `pairing\` (USB pairing records), `network\<id>\`
(each device's web proxy certificate), `logs\` (`app.log`, `worker-*.log`, `usbmuxd-*.log`,
`firmwarekit.log`) and `settings.json`. Screenshots go to `Pictures\iOS-Emulator`.

## Credits and license

Built on [LightTouch](https://github.com/blake-lucas/LightTouch) by Blake Lucas (its engine, firmwarekit,
guest tools and firmware catalog, in `resources\`), [qemu-ios](https://github.com/blake-lucas/qemu-ios),
[QEMU](https://www.qemu.org), [libimobiledevice](https://libimobiledevice.org),
[Dear ImGui](https://github.com/ocornut/imgui) and [OpenSSL](https://www.openssl.org), with
[nlohmann/json](https://github.com/nlohmann/json).
The licenses of the bundled components are in `resources\licenses` and `third_party`.

Like LightTouch, this program is free software under the GNU GPL, version 2 or (at your option) any
later version: see `LICENSE`.

iPhone, iPod touch, and iPad are trademarks of Apple Inc. iOS is a trademark or registered trademark of Cisco in the U.S. and other countries and is used by Apple under license. This project is not affiliated with Apple, nor does it distribute Apple's proprietary software, firmware, or source code.
