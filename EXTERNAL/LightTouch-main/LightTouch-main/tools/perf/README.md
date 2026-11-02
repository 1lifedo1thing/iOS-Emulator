# Performance harness

Headless benchmarks for the qemu-ios engine on desktop Linux and on Android
phones over adb. Measurements behind the 2026-10-16 `dev/perf` changes.

- `ltbench.py` boots a standalone `qemu-system-arm` (n72ap-7E18) locally or on a
  phone (`adb shell run-as org.lighttouch.android`, using the installed app's
  prepared base). QMP over stdio; screendumps detect the lock screen.
  Workloads: `idle` (per-thread CPU and context switches at the home screen),
  `apps` (open/close latency), `scroll` with `--fling` (frames/s from the
  `ipod_touch_lcd_frame_log` trace while flicking Settings). `--wrap` prefixes
  the engine (e.g. `taskset f0`, `perf stat ...`); `--vcpu-mask` pins only the
  vCPU thread. Set `ANDROID_SERIAL` when several phones are attached.
- `uirate.py` flings the app's own UI (Settings, the Store...) and reports the panel rate and the
  UI's real frame rate from gfxinfo framestats, with how many frames took 2+ refreshes. Use it
  to check that the 60 Hz cap lifts off the guest view and that scrolling stays smooth.
- `devbench.sh SERIAL RECORD tag:binary:mask[:vcpumask] ...` runs boot + fling
  per spec on one phone; engines go in the app's `files/bench/`.
- `appab.py label=apk ...` installs APKs in turn and times an in-app boot to
  the lock screen (buttons found by text via uiautomator).
- `gamefps.py SECS` reads delivered guest fps while a game is on screen;
  `fcbench.py APK SECS` installs an APK, boots, unlocks, starts Flight Control
  (first icon on Home page 2) and measures it. `FC_FRAMES=chart.png` also saves
  the exact frame window as `chart.png.frames.log`. See docs/PERFORMANCE.md.
- `gameprof.sh OUTDIR [SECONDS]` attaches simpleperf to the running engine worker
  (`SP_CALLGRAPH=fp` or `dwarf` for call stacks, `SP_FREQ` for the rate, default 4000 Hz).
  Resolve samples with the matching unstripped engine library.
- Hitch analyzers (`hitchsched.py`, `hitchcpu.py`, `framepath.py`, `guestpc.py`,
  `guestsymbols.py`, `guestprocs.py` and the per-diagnostic readers such as `audiopath.py`
  and `btpath.py`) read the engine's opt-in diagnostics and Perfetto traces offline. Which
  switch each one reads, and how to capture it: [PROFILING.md](../../docs/PROFILING.md).
  Each script's `--help` lists its inputs.

## Manual hitch captures

Use **Start profile / Stop profile** above the Android FPS graph. Each recording saves the
frontmost iOS app name, engine frames, GL totals/outliers, BQL waits/holds and Android presentation
stages. `profile.py --pull build/profiles` retrieves them; `profile.py CAPTURE --chart report.png
--json report.json` reports the longest frames and expensive calls. See
[PROFILING.md](../../docs/PROFILING.md) for capture workflow, file formats and timing limitations.

## Results (n72ap-7E18, boot to lock screen / Settings fling fps)

| Host | Original engine, unpinned | dev/perf, big cores |
| --- | --- | --- |
| Poco F3 (Snapdragon 865-class) | 55 s / 18-22 | 36-40 s / 25-28 |
| Pixel 10 (Tensor G5) | 57-74 s / 15-24 | 19-20 s / 27-29 |
| Ryzen 3700X | 23-24 s / 40-42 | 21 s (19-20 s with LTO) |

In-app on the Poco: v0.0.1 50-52 s, big-core build 39-41 s.

Measured and rejected: a 16K jump cache;
lock-free timer counter reads; isolating the vCPU on the prime core (worse
animation on the Pixel); LTO on Android (no measurable phone gain).
