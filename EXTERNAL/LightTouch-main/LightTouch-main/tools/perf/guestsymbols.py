#!/usr/bin/env python3
"""Name ARMv6/ARMv7 guest TB addresses from a matching kernel and dyld v1 cache.

The iOS 6 armv7 cache keeps local symbols in a cache-wide table instead of each
image's symtab; they are merged with the per-image symbols. Pass --perf-report
(text from `perf report --no-children --sort symbol --stdio -g none`) to rank the
`guest-0xADDR` JIT rows by function and image. Cache images are named
`image!symbol+off`; user addresses outside the cache are `app` (or `dyld`).
Only code-section ranges are named; symbol extents end at the next symbol or
section boundary. Indirect stubs are labeled separately. Addresses are unslid.
This does not identify processes, unwind guest stacks, or give instruction PCs
within a sampled translation block. Firmware inputs stay local.
"""
import argparse
import bisect
from collections import defaultdict
import hashlib
import itertools
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def macho_text_range(data, base):
    """(lo, hi) of __TEXT for a 32-bit Mach-O at `base`, or None."""
    ncmds = struct.unpack_from("<I", data, base + 16)[0]
    pos = base + 28
    for _ in range(min(ncmds, 4096)):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 1 and data[pos + 8:pos + 24].rstrip(b"\0") == b"__TEXT":
            lo, length = struct.unpack_from("<II", data, pos + 24)
            return lo, lo + length
        pos += size
    return None


def macho_symbols(data, base, fallback, extra=()):
    """Code ranges for one Mach-O. `extra` adds (type, sect, value, name) nlist
    tuples that live outside the image's own symtab (cache local symbols)."""
    if struct.unpack_from("<I", data, base)[0] != 0xfeedface:
        raise ValueError("expected little-endian 32-bit Mach-O")
    ncmds, sizeofcmds = struct.unpack_from("<II", data, base + 16)
    if ncmds > 4096 or base + 28 + sizeofcmds > len(data):
        raise ValueError("invalid Mach-O load-command span")
    pos, sections, symtab, indirect, name = base + 28, [], None, None, fallback
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if size < 8 or pos + size > base + 28 + sizeofcmds:
            raise ValueError("invalid load command")
        if cmd == 1:  # LC_SEGMENT
            nsects = struct.unpack_from("<I", data, pos + 48)[0]
            if 56 + nsects * 68 > size:
                raise ValueError("invalid Mach-O section span")
            for i in range(nsects):
                offset = pos + 56 + i * 68
                lo, length = struct.unpack_from("<II", data, offset + 32)
                flags, reserved1, reserved2 = struct.unpack_from("<III", data, offset + 56)
                sections.append((lo, lo + length, flags, reserved1, reserved2))
        elif cmd == 2:  # LC_SYMTAB: cache file offsets are already absolute
            symtab = struct.unpack_from("<IIII", data, pos + 8)
        elif cmd == 0xb:  # LC_DYSYMTAB
            indirect = struct.unpack_from("<II", data, pos + 56)
        elif cmd == 0xd:  # LC_ID_DYLIB: canonical identity, not cache alias path
            offset = struct.unpack_from("<I", data, pos + 8)[0]
            if offset < 24 or offset >= size:
                raise ValueError("invalid dylib name")
            name = data[pos + offset:data.index(0, pos + offset, pos + size)].decode("latin1")
        pos += size
    if not symtab and not extra:
        return []
    symoff, count, stroff, strsize = symtab or (0, 0, 0, 0)
    if symoff + count * 12 > len(data) or stroff + strsize > len(data):
        raise ValueError("symbol table outside input")

    def symbol(index):
        if index >= count:
            raise ValueError("indirect symbol index outside symbol table")
        strx, typ, sect, desc, value = struct.unpack_from("<IBBHI", data, symoff + index * 12)
        if strx >= strsize:
            raise ValueError("symbol string outside table")
        end = data.index(0, stroff + strx, stroff + strsize)
        return typ, sect, value & ~1, data[stroff + strx:end].decode("latin1")

    groups = defaultdict(lambda: defaultdict(set))
    for typ, sect, value, label in itertools.chain(map(symbol, range(count)), extra):
        if typ & 0xe0 or typ & 0xe != 0xe or not 0 < sect <= len(sections):
            continue  # ignore debug, undefined, absolute and non-section entries
        lo, hi, flags, *_ = sections[sect - 1]
        if flags & 0x80000400 and lo <= value < hi:
            groups[sect][value].add(label)
    output = []
    for sect, rows in groups.items():
        addresses = sorted(rows)
        for i, address in enumerate(addresses):
            end = addresses[i + 1] if i + 1 < len(addresses) else sections[sect - 1][1]
            output.append((address, end, name, "|".join(sorted(rows[address]))))
    if indirect:
        table, length = indirect
        if table + length * 4 > len(data):
            raise ValueError("indirect table outside input")
        for lo, hi, flags, first, stride in sections:
            if flags & 0xff != 8:  # S_SYMBOL_STUBS
                continue
            if not stride or (hi - lo) % stride or first + (hi - lo) // stride > length:
                raise ValueError("invalid indirect stub section")
            for i, address in enumerate(range(lo, hi, stride)):
                index = struct.unpack_from("<I", data, table + (first + i) * 4)[0]
                if index & 0xc0000000:
                    continue  # INDIRECT_SYMBOL_LOCAL / ABS
                output.append((address, address + stride, name, "stub:" + symbol(index)[3]))
    return output


def local_symbols(cache, mappings, header_size):
    """Cache-wide local symbols (armv7 caches): image address -> nlist tuples."""
    local_offset, local_size = struct.unpack_from("<QQ", cache, 72) if header_size >= 88 else (0, 0)
    if not local_size:
        return {}
    if local_offset + local_size > len(cache):
        raise ValueError("local symbols outside cache")
    nlist_off, nlist_count, str_off, str_size, entry_off, entry_count = \
        struct.unpack_from("<6I", cache, local_offset)
    if (nlist_off + nlist_count * 12 > local_size or str_off + str_size > local_size
            or entry_off + entry_count * 12 > local_size):
        raise ValueError("invalid local symbol table")
    strings = local_offset + str_off
    found = {}
    for i in range(entry_count):
        dylib_offset, first, count = struct.unpack_from("<3I", cache, local_offset + entry_off + i * 12)
        if first + count > nlist_count:
            raise ValueError("invalid local symbol entry")
        rows = []
        for j in range(first, first + count):
            strx, typ, sect, _, value = struct.unpack_from(
                "<IBBHI", cache, local_offset + nlist_off + j * 12)
            if strx >= str_size:
                raise ValueError("local symbol string outside table")
            end = cache.index(0, strings + strx, strings + str_size)
            rows.append((typ, sect, value & ~1, cache[strings + strx:end].decode("latin1")))
        found[mappings[0][0] + dylib_offset] = rows
    return found


class GuestSymbols:
    def __init__(self, kernel, cache):
        cache_data = cache.read_bytes()
        self.hashes = {"dyld_cache": hashlib.sha256(cache_data).hexdigest()}
        ranges = []
        if kernel:
            kernel_data = kernel.read_bytes()
            self.hashes["kernel"] = hashlib.sha256(kernel_data).hexdigest()
            ranges = macho_symbols(kernel_data, 0, "kernel")
        if cache_data[:16].rstrip(b"\0") not in (b"dyld_v1   armv6", b"dyld_v1   armv7"):
            raise ValueError("expected ARMv6 or ARMv7 dyld v1 cache")
        mapping_offset, mapping_count, image_offset, image_count = struct.unpack_from("<4I", cache_data, 16)
        if mapping_count > 32 or image_count > 4096:
            raise ValueError("invalid dyld cache counts")
        mappings = [struct.unpack_from("<3Q2I", cache_data, mapping_offset + i * 32)
                    for i in range(mapping_count)]
        locals_ = local_symbols(cache_data, mappings, mapping_offset)
        self.cache_lo = mappings[0][0]
        self.cache_hi = max(va + size for va, size, *_ in mappings)
        self.images = []  # (text lo, hi, path)
        seen = set()
        for i in range(image_count):
            address, _, _, path_offset, _ = struct.unpack_from("<3Q2I", cache_data, image_offset + i * 32)
            if address in seen:
                continue  # libSystem has multiple cache aliases with the same Mach-O
            seen.add(address)
            path = cache_data[path_offset:cache_data.index(0, path_offset)].decode("latin1")
            offsets = [offset + address - va for va, size, offset, _, _ in mappings
                       if va <= address < va + size]
            if len(offsets) != 1:
                raise ValueError("dyld image has no unique mapping")
            ranges.extend(macho_symbols(cache_data, offsets[0], path, locals_.get(address, ())))
            text = macho_text_range(cache_data, offsets[0])
            if text:
                self.images.append((*text, path))
        self.images.sort()
        self.image_starts = [row[0] for row in self.images]
        # Explicit stub ranges override any nearby exported symbol approximation.
        unique = {}
        for row in sorted(set(ranges), key=lambda r: r[3].startswith("stub:")):
            unique[(row[0], row[1])] = row
        self.ranges = sorted(unique.values())
        self.starts = [row[0] for row in self.ranges]
        self.max_ends = []
        end = 0
        for row in self.ranges:
            end = max(end, row[1])
            self.max_ends.append(end)

    def matches(self, pc):
        index = bisect.bisect_right(self.starts, pc) - 1
        matches = []
        while index >= 0 and self.max_ends[index] > pc:
            row = self.ranges[index]
            if row[0] <= pc < row[1]:
                matches.append(row)
            index -= 1
        stubs = [row for row in matches if row[3].startswith("stub:")]
        return stubs or matches

    def lookup(self, pc):
        names = {f"{row[2]}:{row[3]}" for row in self.matches(pc)}
        return next(iter(names)) if len(names) == 1 else None

    def locate(self, pc):
        """(image path, symbol or None, offset) for a cache image address, else None.

        Symbol None means the address is in the image's __TEXT but before/outside
        any named range; the offset is then from the image's __TEXT start."""
        chosen = self.matches(pc)
        if chosen:
            row = max(chosen)  # innermost start wins if ranges nest
            names = sorted(row[3].split("|"), key=lambda n: n == "<redacted>")
            # The cache redacts most local names; name those by their (unslid) entry.
            symbol = names[0] if names[0] != "<redacted>" else f"<redacted@0x{row[0]:x}>"
            return row[2], symbol, pc - row[0]
        index = bisect.bisect_right(self.image_starts, pc) - 1
        if index >= 0 and pc < self.images[index][1]:
            return self.images[index][2], None, pc - self.images[index][0]
        return None

    def symbolize(self, pc):
        found = self.locate(pc)
        if not found:
            return None
        image, symbol, offset = found
        image = image.rsplit("/", 1)[-1]
        return f"{image}!{symbol or '?'}+0x{offset:x}"


def estimate_slide(symbols, rows):
    """Guess the dyld shared cache slide (iOS 6 slides the cache at boot).

    rows are (weight, pc). The slide that lands the most weight exactly on named
    function entries wins (TBs start at entries and call returns far more often
    than at random offsets). Returns (slide, entry weight, runner-up weight)."""
    starts = {row[0] for row in symbols.ranges if not row[3].startswith("stub:")}
    scores = []
    for slide in range(0, 0x40000000 - symbols.cache_hi + 1, 0x1000):
        weight = sum(w for w, pc in rows if pc - slide in starts)
        scores.append((weight, slide))
    scores.sort(reverse=True)
    return scores[0][1], scores[0][0], scores[1][0]


GUEST_ROW = re.compile(r"^\s*(\d+\.\d+)%\s+\[.\]\s+guest-0x([0-9a-fA-F]+)(?:\s.*)?$")


def demangle(text):
    """Demangle a C++ symbol in `image!symbol` text with c++filt, if installed."""
    image, sep, name = text.partition("!")
    if not sep or not name.startswith("__Z") or not shutil.which("c++filt"):
        return text
    out = subprocess.run(["c++filt", "-n", name[1:]], capture_output=True, text=True)
    return f"{image}!{out.stdout.strip()}" if out.returncode == 0 and out.stdout.strip() else text


def rank_perf_report(symbols, lines, top, slide=None):
    """Rank `perf report --stdio` guest-0xADDR rows (user addresses) by function.

    slide None estimates the cache slide from the rows."""
    rows = [(float(m[1]), int(m[2], 16)) for m in map(GUEST_ROW.match, lines) if m]
    if slide is None:
        slide, hit, second = estimate_slide(symbols, [r for r in rows if r[1] < 0x80000000])
        print(f"estimated cache slide 0x{slide:x}: {hit:.2f}% of samples on function entries "
              f"(runner-up {second:.2f}%)")
    funcs, images, kernel, total_guest = defaultdict(float), defaultdict(float), 0.0, 0.0
    app = defaultdict(float)
    for percent, pc in rows:
        total_guest += percent
        if pc >= 0x80000000:
            kernel += percent
            continue
        in_cache = symbols.cache_lo + slide <= pc < symbols.cache_hi + slide
        found = symbols.locate(pc - slide) if in_cache else None
        if found:
            image = found[0].rsplit("/", 1)[-1]
            func = f"{image}!{found[1] or '?'}"
        elif 0x2fe00000 <= pc < 0x30000000:
            image, func = "dyld", "dyld"
        else:
            image, func = "app", "app"  # code outside the shared cache
            app[pc] += percent
        funcs[(image, func)] += percent
        images[image] += percent
    print(f"guest TB rows: {total_guest:.2f}% of samples; kernel (>=0x80000000): {kernel:.2f}%; "
          f"user: {sum(images.values()):.2f}%")
    print("\ntop user functions (summed % of all samples; image!symbol, cache addresses unslid;")
    print("<redacted@ADDR> = function whose name the cache strips, ADDR is its entry; '?' = unnamed code)")
    ranked = sorted(funcs.items(), key=lambda kv: -kv[1])[:top]
    for (image, func), percent in ranked:
        print(f"{percent:6.2f}%  {demangle(func)}")
    if app:
        print("\napp (outside the cache) hottest translation-block starts, % of all samples")
        for pc, percent in sorted(app.items(), key=lambda kv: -kv[1])[:8]:
            print(f"{percent:6.2f}%  0x{pc:x}")
    print("\nuser samples by image")
    for image, percent in sorted(images.items(), key=lambda kv: -kv[1])[:top]:
        print(f"{percent:6.2f}%  {image}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", type=Path, help="optional; only needed to name >=0x80000000")
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--pc", action="append", default=[])
    ap.add_argument("--json", type=Path)
    ap.add_argument("--perf-report", type=Path,
                    help="`perf report --no-children --sort symbol --stdio -g none` text")
    ap.add_argument("--slide", type=lambda v: int(v, 0),
                    help="cache slide for --perf-report (default: estimate from the samples)")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    symbols = GuestSymbols(args.kernel, args.cache)
    print(f"{len(symbols.ranges)} named code ranges; input SHA256: {symbols.hashes}")
    for pc in args.pc:
        value = int(pc, 0)
        print(f"{pc}: {symbols.symbolize(value) or symbols.lookup(value) or 'unknown/ambiguous'}")
    if args.perf_report:
        rank_perf_report(symbols, args.perf_report.read_text().splitlines(), args.top, args.slide)
    if args.json:
        args.json.write_text(json.dumps(dict(input_sha256=symbols.hashes,
                                              code_ranges=symbols.ranges), indent=2) + "\n")


if __name__ == "__main__":
    main()
