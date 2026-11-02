# Compare the monitor on one existing worker

`scripts/collect-monitor-phase.py` is an operator-run observation tool. It never
launches an Activity, invokes instrumentation, requests a performance Bundle,
changes the panel, injects input, or pauses/stops/restarts a guest. Run it only
while the designated phone owner has authorized the collection. No root, system
properties, security changes or profiler permissions are needed. Unavailable
`smaps_rollup` or `CLK_TCK` stays unavailable; raw counter evidence is retained.

Use a **current** `before.json` produced by the strict shutdown evidence collector
as the binding, with the installed APK, selected record, worker UID/PID/start token
and device boot ID. Creating this binding does not request Stop. A previous
worker's binding must not be reused. The helper checks installed main bytes and
record metadata before settling, then checks the process start token throughout.

From the repository root, first inspect the plan without contacting ADB:

```sh
python3 Android/scripts/collect-monitor-phase.py \
  --serial "$SERIAL" --binding "$CURRENT_BEFORE/before.json" \
  --phase a1-closed --out "$PRIVATE_EVIDENCE/a1-closed"
```

Keep the same guest scene, orientation, worker and running state. Manually close
the Performance panel, then repeat the command with `--execute`. Manually open
the panel and collect `--phase b-open` into a fresh directory; manually close it
and collect `--phase a2-closed` into another fresh directory. Each phase settles
for five seconds and observes for twenty seconds at the same two-second cadence.
Do not use ActivityScenario during the comparison: closing it can pause the guest.
Do not interact with the guest or run another collector/profiler concurrently.

For a separate lifecycle check, keep the panel open, manually Pause through the
production UI, and collect `--phase paused-open` with the same worker binding.
Host CPU can remain nonzero while the guest is paused. Manually Resume and
collect another `b-open` into a **new** directory. For an actual worker replacement,
first preserve the old phase/Stop evidence and create a new binding: do not splice
counter intervals across PIDs or hide a reset inside one phase.

The collector captures bounded UI XML observations before and after the counter
window and the visible metric
text/ages, including the PID. It rejects the wrong panel or running/paused state.
Both label layouts are explicit: original candidate25's six rows with **Emulator
CPU**, or current builds' seven rows with **QEMU worker CPU** and cumulative
**QEMU worker CPU time**. The result retains the recognized layout; mixed or
missing CPU rows fail. Neither layout represents genuine iOS CPU/RAM accounting.
It records independent process CPU tick deltas and positive PSS KiB samples,
not guest usage. CPU bounds use the actual `CLK_TCK` and device `/proc/uptime`
precision/read brackets; the UI's elapsedRealtime age is kept as visible text,
not subtracted from host or native monotonic clocks. PSS and CPU reads are not
atomic. The app's own PSS sampler and the external rollup reader both have cost.
XML dumping is kept outside the counter window so its variable latency cannot
silently lengthen a two-second sample interval. Identical external observation
is used in all phases; collection costs and late
cadence are retained. A missed cadence fails with partial evidence rather than
silently extending the window or catching up with a burst of reads.

Compare all three counter intervals, CPU ranges, PSS samples and UI ages while
retaining each raw receipt. A1 versus A2 drift, guest animation, thermal/scheduler
changes and the common observer cost limit attribution. A static Photo scene
means interaction-idle, not CPU-idle. `CAPTURED_NOT_BENCHMARK_PASS` means only that
the stated collection completed. It does not prove acceptable overhead, sound,
guest scene, responsiveness or lifecycle acceptance. Screenshots and visual
inspection remain separate owner-operated evidence. Closed phases make no
performance Binder request and do not wake the monitor sampler.

Pure parser/counter regressions are in
`scripts/tests/test_monitor_phase.py`. They do not operate a device or qualify
actual sampling permissions, Compose accessibility output, or target performance.
