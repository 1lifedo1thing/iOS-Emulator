#!/usr/bin/env python3
"""Guest services runs: can the host reach iOS over the emulated USB cable, install an app and drive the guest tools?

  services.py ID [ID ...] [--ipa FILE] [--seconds N]   headless Linux run per ID (fresh state each run)
  services.py table                                    Markdown table of the newest result per ID

Each run boots a fresh device with a private usbmuxd-qemu bridge, as the desktop and Android app services do,
and grades how far it got:
  usb       usbmuxd enumerated the guest and lockdown answered (ProductType, ProductVersion)
  install   the IPA installed through installation_proxy and is in a fresh installed-app list
  launch    the guest tools launched Settings and then reported it in front
  clean     iOS shut down cleanly when asked

Bases come from compat.py's BASES (imported once into WORK/ID/base). The bridge and libimobiledevice come from a
LightTouch Linux AppDir (LT_SERVICES_ROOT, default the extracted 0.0.10 AppDir): usr/bin/usbmuxd-qemu,
usr/lib/libimobiledevice-1.0.so.6 and usr/lib/libplist-2.0.so.4. Installs use Desktop/app_services/helper.py.
"""
import argparse, datetime, json, os, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compat import REPO, DEV, WORK, ROMS, BASES, LIT, TOOLS, arch, catalog_entry, import_base  # noqa: E402

RESULTS = Path(os.environ.get("LT_SERVICES_RESULTS", DEV / "services-results"))
ROOT = Path(os.environ.get("LT_SERVICES_ROOT", Path.home() / "Documents/LightTouch-Development/release-0.0.10/appdir/squashfs-root"))
LEVELS = ["usb", "install", "launch", "clean"]
SETTINGS = "com.apple.Preferences"


class Bridge:
    """usbmuxd-qemu on owner-only sockets in a fresh directory (short, for the AF_UNIX path limit)."""

    def __init__(self, log):
        self.dir = Path(tempfile.mkdtemp(prefix="lt-usb-"))
        self.mux, self.qemu = self.dir / "mux.sock", self.dir / "qemu.sock"
        env = dict(os.environ, USBMUXD_QEMU_ADDR=str(self.qemu), LD_LIBRARY_PATH=str(ROOT / "usr/lib"))
        (self.dir / "pair").mkdir(mode=0o700)
        self.child = subprocess.Popen([str(ROOT / "usr/bin/usbmuxd-qemu"), "-f", "-S", str(self.mux), "-C",
                                       str(self.dir / "pair"), "-P", str(self.dir / "daemon.pid")]
                                      + os.environ.get("LT_USBMUXD_ARGS", "").split(),  # e.g. "-v -v"
                                      stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env)
        deadline = time.monotonic() + 5
        while not (self.mux.exists() and self.qemu.exists()):
            if self.child.poll() is not None or time.monotonic() > deadline:
                self.close()
                raise RuntimeError("usbmuxd-qemu did not start; see its log")
            time.sleep(.025)

    def helper(self, action, scratch, ipa=None, timeout=600):
        """Desktop's app services helper; its JSON lines as a list of dicts."""
        command = [sys.executable, "-m", "app_services.helper", action, "--socket", str(self.mux),
                   "--library", str(ROOT / "usr/lib/libimobiledevice-1.0.so.6"),
                   "--plist-library", str(ROOT / "usr/lib/libplist-2.0.so.4"), "--scratch", str(scratch)]
        if ipa:
            command += ["--ipa", str(ipa)]
        env = dict(os.environ, LD_LIBRARY_PATH=str(ROOT / "usr/lib"))
        try:
            run = subprocess.run(command, cwd=REPO / "Desktop", env=env, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return [{"error": f"{action} timed out after {timeout} s"}]
        lines = [json.loads(l) for l in run.stdout.splitlines() if l.startswith("{")]
        return lines or [{"error": (run.stderr.strip().splitlines() or ["helper failed"])[-1]}]

    def close(self):
        if self.child and self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(5)
            except subprocess.TimeoutExpired:
                self.child.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def grade(result):
    reached = {"usb": "target" in result, "install": result.get("installed", False),
               "launch": result.get("frontmost") == SETTINGS, "clean": result.get("clean", False)}
    result["reached"] = [level for level in LEVELS if reached[level]]
    result["level"] = next((level for level in reversed(LEVELS) if reached[level]
                            and all(reached[l] for l in LEVELS[:LEVELS.index(level)])), "none")
    return result


def save(result):
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RESULTS / f"{stamp}-{result['host']}-{result['id']}.json"
    path.write_text(json.dumps(result, indent=1) + "\n")
    print(f"{result['id']}: {result['level']} ({', '.join(result['reached']) or 'nothing'})"
          + (f" — {result['error']}" if result.get("error") else ""))


def run(args):
    build = REPO / "build"
    exe, engine = build / "linux/lighttouch-linux", build / "linux/libqemu-arm.so"
    for path in (exe, engine, ROOT / "usr/bin/usbmuxd-qemu", args.ipa):
        if not Path(path).exists():
            sys.exit(f"{path} missing")
    for firmware_id in args.ids:
        entry = catalog_entry(firmware_id)
        work = WORK / firmware_id
        base, state, out = work / "base", work / "services-state", work / "services"
        result = {"id": firmware_id, "board": entry["board"], "build": entry["build"], "version": entry["version"],
                  "host": f"linux-{socket.gethostname()}", "date": datetime.datetime.now().isoformat(timespec="seconds"),
                  "ipa": Path(args.ipa).name}
        if not import_base(exe, firmware_id, base, result):
            save(grade(result)); continue
        for path in (state, out):
            shutil.rmtree(path, ignore_errors=True)
        out.mkdir(parents=True)
        scratch = Path(tempfile.mkdtemp(prefix="lt-scratch-"))
        # Settings is launched near the end, after the install has had its time, then asked for. SpringBoard
        # launches nothing while locked: Home wakes the screen, a throwaway tap (the first touch after a wake is
        # often dropped), then the slider.
        launch = args.seconds - 45
        lines = [f"{t} screenshot {out}/{t:03d}.bmp" for t in range(30, launch - 10, 30)]
        lines += [f"{launch - 10} button 0 1", f"{launch - 9.85} button 0 0",
                  f"{launch - 8} touch 1 0 0.5 0.3", f"{launch - 7.92} touch 1 2 0.5 0.3",
                  f"{launch - 6.5} touch 1 0 0.150 0.89"]
        lines += [f"{launch - 6.5 + .04 * i:.2f} touch 1 1 {0.15 + 0.04 * i:.3f} 0.89" for i in range(1, 20)]
        lines += [f"{launch - 5.7:.2f} touch 1 2 0.950 0.89", f"{launch - 2} screenshot {out}/unlocked.bmp"]
        lines += [f"{launch} guest launch {SETTINGS}", f"{launch + 12} guest frontmost",
                  f"{launch + 14} screenshot {out}/launch.bmp", f"{args.seconds - 15} stop"]
        (out / "script.txt").write_text("\n".join(lines) + "\n")
        package = Path(args.package or build / f"guest-package/{arch(entry['board'])}.itpack")
        command = [str(exe), "--base", str(base), "--state", str(state), "--engine", str(engine), "--package", str(package),
                   "--headless", "--seconds", str(args.seconds + 60), "--script", str(out / "script.txt"), "--no-audio"]
        if entry["board"] in ROMS:
            command += ["--rom", str(BASES / ROMS[entry["board"]])]
        with (out / "usbmuxd.log").open("w") as mux_log, (out / "run.log").open("w") as log:
            bridge = Bridge(mux_log)
            try:
                started = time.monotonic()
                guest = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                         env=dict(os.environ, IT_USB_TCP=str(bridge.qemu)))
                # Lockdown answers once iOS's USB stack and lockdownd are up: ask every few seconds.
                last = None
                while guest.poll() is None and time.monotonic() - started < launch - 10:
                    lines = bridge.helper("info", scratch, timeout=60)
                    if any("target" in l for l in lines) and not any("error" in l for l in lines):
                        result["target"] = next(l["target"] for l in lines if "target" in l)
                        result["usb_s"] = round(time.monotonic() - started, 1)
                        break
                    last = next((l["error"] for l in lines if "error" in l), last)
                    time.sleep(5)
                if "target" in result:
                    t = time.monotonic()
                    lines = bridge.helper("install", scratch, ipa=args.ipa, timeout=600)
                    errors = [l["error"] for l in lines if "error" in l]
                    done = next((l["result"] for l in lines if "result" in l), None)
                    result["install_s"] = round(time.monotonic() - t, 1)
                    result["installed"] = bool(done) and not errors
                    if errors:
                        result["error"] = "install: " + errors[-1]
                elif last:
                    result["error"] = "usb: " + last
                guest.wait(args.seconds + 120)
            finally:
                bridge.close()
                shutil.rmtree(scratch, ignore_errors=True)
        text = (out / "run.log").read_text(errors="replace")
        if m := LIT.search(text):
            result["lit_wall_s"] = float(m[1])
        if m := TOOLS.search(text):
            result["guest_package"] = {"serial": int(m[1]), "loader": int(m[2]), "verdict": m[3]}
        front = [m[1].strip() for m in re.finditer(r"guest request=\d+ state=\d+ status=0 bytes=\d+ output=(\S+)", text)]
        if front:
            result["frontmost"] = front[-1]
        if result.get("frontmost") == "com.apple.purplebuddy" and not result.get("error"):
            result["error"] = "launch: Setup Assistant is in front (a base prepared without --skip-setup)"
        final = re.search(r"FINAL .* confirmed=(-?\d+) dirty=(-?\d+)", text)
        result["clean"] = bool(final and final[1] == "1" and final[2] == "0")
        mux = (out / "usbmuxd.log").read_text(errors="replace")
        result["usb_enumeration_failures"] = len(re.findall(r"Enumeration attempt \d+ failed", mux))
        # A few NAKed SETUPs are normal while iOS is starting; only a guest that never got further is diagnosed.
        if "SETUP never accepted" in mux and "Connected to v" not in mux and result.get("error", "").startswith("usb: find guest"):
            result["error"] = (result.get("error", "") + " (guest never answered USB SETUP: its USB stack did not start)").strip()
        result["log"] = str(out)
        save(grade(result))


def table(args):
    newest = {}
    for path in sorted(RESULTS.glob("*.json")):
        r = json.loads(path.read_text())
        newest[(r["id"], r["host"])] = r
    print("| Config | iOS | Host | Level | USB up (s) | Install (s) | Notes |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for (firmware_id, host), r in sorted(newest.items()):
        print(f"| {firmware_id} | {r['version']} | {host} | {r['level']} | {r.get('usb_s', '')} | {r.get('install_s', '')} | "
              f"{r.get('error', '')} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ids", nargs="+", help="firmware IDs, or 'table'")
    parser.add_argument("--seconds", type=int, default=300, help="how long the guest runs before it is shut down")
    parser.add_argument("--ipa", default=str(REPO / "build/ltbench/LTBench.ipa"), help="the app to install")
    parser.add_argument("--package", help="guest tools (default build/guest-package/<arch>.itpack)")
    args = parser.parse_args()
    table(args) if args.ids == ["table"] else run(args)


if __name__ == "__main__":
    main()
