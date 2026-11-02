"""LLDB command for a guest proc/task/pmap snapshot after a frame capture.

Inside LLDB: command script import /path/to/guestprocs.py
Then: lt-guest-procs --kernel KERNEL --xnu QEMU/imgtools/lldb/xnu.py
                    --connect connect://127.0.0.1:PORT --json OUT
Attaching pauses the guest. Use only after the timed window, before clean Stop.
The existing qemu-ios helper derives offsets from the matching guest kernel.
Optional task thread enumeration requires explicit list offsets from that
kernel's disassembly and --task-thread-kernel-sha256 to pin their source.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex

import lldb


def snapshot(debugger, command, context, result, internal_dict):
    ap = argparse.ArgumentParser(prog="lt-guest-procs", description=__doc__)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--xnu", type=Path, required=True)
    ap.add_argument("--connect", required=True)
    ap.add_argument("--json", type=Path, required=True)
    ap.add_argument("--thread", type=lambda s: int(s, 0), action="append", default=[],
                    help="also read this guest thread's task owner (repeatable, accepts hex)")
    ap.add_argument("--task-thread-layout",
                    help="enumerate task threads with matching-kernel head,count,next offsets (comma-separated)")
    ap.add_argument("--task-thread-kernel-sha256",
                    help="required kernel pin for --task-thread-layout")
    try:
        args = ap.parse_args(shlex.split(command))
        if len(args.thread) > 4096 or any(t <= 0 or t > 0xffffffff or t & 3
                                          for t in args.thread):
            ap.error("thread addresses must be aligned ARM32 pointers, at most 4096")
        layout = None
        if args.task_thread_layout:
            try:
                layout = tuple(int(value, 0) for value in args.task_thread_layout.split(','))
            except ValueError:
                ap.error("invalid task thread layout")
            if (len(layout) != 3 or any(value < 0 or value > 0x4000 or value & 3
                                        for value in layout)):
                ap.error("need three aligned head,count,next offsets, at most 0x4000")
            if not args.task_thread_kernel_sha256:
                ap.error("task thread enumeration requires a matching kernel SHA256 pin")
        elif args.task_thread_kernel_sha256:
            ap.error("kernel pin requires --task-thread-layout")
    except SystemExit:
        result.SetError("invalid guest snapshot arguments")
        return
    process = None
    report = None
    failure = None
    try:
        if args.json.exists():
            raise ValueError("snapshot output already exists")
        kernel_hash = hashlib.sha256(args.kernel.read_bytes()).hexdigest()
        if layout and kernel_hash != args.task_thread_kernel_sha256:
            raise ValueError("task thread layout kernel pin mismatch")
        target = debugger.CreateTargetWithFileAndArch(str(args.kernel), "armv6-apple-ios")
        if not target.IsValid():
            raise ValueError("cannot create ARMv6 kernel target")
        debugger.SetSelectedTarget(target)
        debugger.SetAsync(False)
        error = lldb.SBError()
        process = target.ConnectRemote(debugger.GetListener(), args.connect, "gdb-remote", error)
        if error.Fail() or not process.IsValid() or process.GetState() != lldb.eStateStopped:
            raise ValueError("cannot stop guest for snapshot: " + str(error))
        target.SetModuleLoadAddress(target.GetModuleAtIndex(0), 0)
        spec = importlib.util.spec_from_file_location("lt_xnu_snapshot", args.xnu)
        xnu = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(xnu)
        rows = xnu.procs()
        if not rows or any(row[4] is None or row[5] is None for row in rows):
            raise ValueError("incomplete proc/pmap snapshot: " + repr(xnu.OFF))
        registers = {name: xnu._reg(name) for name in
                     ("TTBR0_EL1", "TTBR1_EL1", "TTBCR", "CONTEXTIDR_EL1", "TPIDRPRW", "pc", "cpsr")}
        if any(registers[k] is None for k in ("TTBR0_EL1", "TTBR1_EL1", "TTBCR")):
            raise ValueError("missing translation registers")
        n = registers["TTBCR"] & 7
        mask = ~((1 << (14 - n)) - 1)
        if not any(row[5] == registers["TTBR0_EL1"] & mask for row in rows):
            raise ValueError("active TTBR0 does not match a process pmap")
        if not any(row[2] == "kernel_task" and
                   row[5] & ~0x3fff == registers["TTBR1_EL1"] & ~0x3fff for row in rows):
            raise ValueError("kernel pmap does not match TTBR1")
        owners = {}
        for p, pid, name, task, pmap, ttb in rows:
            owners.setdefault(task, []).append(dict(proc=p, pid=pid, name=name))
        threads = []
        enumerated = []
        selected = set(args.thread)
        if (selected or layout) and ("thread_task" not in xnu.OFF or "task_proc" not in xnu.OFF):
            raise ValueError("matching kernel did not supply thread/task offsets")
        if layout:
            # Read only after the timed window with the guest stopped. These
            # offsets must come from this kernel's task_threads disassembly;
            # they are deliberately neither guessed nor universal defaults.
            head_offset, count_offset, next_offset = layout
            seen = set()
            for _, pid, name, task, _, _ in rows:
                count = xnu._u32(task + count_offset)
                thread = xnu._u32(task + head_offset)
                if count is None or count > 4096 or len(seen) + count > 4096:
                    raise ValueError("invalid/excessive task thread count")
                addresses = []
                for _ in range(count):
                    if not thread or thread > 0xffffffff or thread & 3 or thread in seen:
                        raise ValueError("invalid/repeated task thread pointer")
                    if xnu._u32(thread + xnu.OFF['thread_task']) != task:
                        raise ValueError("task thread list disagrees with thread task owner")
                    seen.add(thread)
                    addresses.append(thread)
                    thread = xnu._u32(thread + next_offset)
                enumerated.append(dict(task=task, pid=pid, name=name, count=count,
                                       threads=addresses, next_after_last=thread))
            if registers['TPIDRPRW'] not in seen:
                raise ValueError("task thread lists do not contain the current thread")
            selected.update(seen)
        if selected:
            for thread in sorted(selected):
                task = xnu._u32(thread + xnu.OFF["thread_task"])
                bsd_proc = xnu._u32(task + xnu.OFF["task_proc"]) if task else None
                candidates = owners.get(task, [])
                matches = [p for p in candidates if p["proc"] == bsd_proc]
                owner = matches[0] if len(matches) == 1 else None
                status = ("matched" if owner else
                          "bsd-proc-mismatch" if candidates else
                          "task-not-in-proc-snapshot" if task else "unreadable-task")
                threads.append(dict(thread=thread, task=task, bsd_proc=bsd_proc,
                                    owner=owner, owner_candidates=candidates, status=status))
        report = dict(kernel_sha256=kernel_hash,
                      xnu_helper_sha256=hashlib.sha256(args.xnu.read_bytes()).hexdigest(),
                      offsets=xnu.OFF, registers=registers, ttbr0_alignment_bits=14 - n,
                      processes=[dict(proc=p, pid=pid, name=name, task=task, pmap=pmap, ttbr0=ttb)
                                 for p, pid, name, task, pmap, ttb in rows],
                      threads=threads, task_thread_layout=layout,
                      task_thread_enumeration=enumerated)
    except Exception as error:
        failure = str(error)
    finally:
        if process is not None and process.IsValid() and process.GetState() != lldb.eStateDetached:
            detached = process.Detach()
            if detached.Fail():
                failure = "guest detach failed: " + str(detached) + ("; " + failure if failure else "")
            elif report is not None:
                report["detached"] = True
    if failure:
        result.SetError(failure)
        return
    try:
        with args.json.open("x") as output:
            json.dump(report, output, indent=2)
            output.write("\n")
        for row in report["processes"]:
            result.AppendMessage(f"{row['pid']:5} {row['name']:18} ttbr0=0x{row['ttbr0']:08x}")
        for row in report["threads"]:
            label = row["owner"]["name"] if row["status"] == "matched" else row["status"]
            result.AppendMessage(f"thread 0x{row['thread']:08x}: {label}")
        result.AppendMessage("SNAPSHOT_COMPLETE; guest detached")
    except OSError as error:
        result.SetError(str(error))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f guestprocs.snapshot lt-guest-procs")
