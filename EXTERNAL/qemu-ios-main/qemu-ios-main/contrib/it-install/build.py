#!/usr/bin/env python3
"""Builds the iPhone 4S's install helpers (it_install.c, it_keybag.c) as armv7 Mach-O executables.

Usage: build.py OUT_DIR    (with clang and ld.lld on the PATH: MSYS2 UCRT64's mingw-w64-ucrt-x86_64-clang and -lld)

  it_install  a static Mach-O (no dyld, no libc), the install boot's restored_external
  it_keybag   a dyld-linked Mach-O whose three dl* slots dyld binds from libSystem; it loads MobileKeyBag
              and IOKit from the restore ramdisk itself

There is no Apple toolchain here: clang compiles each source as a bare-metal armv7 ELF object, ld.lld links it
at the addresses the Mach-O will map, and the writer below wraps the ELF's bytes in a Mach-O. firmwarekit ships
the results in resources/guest-tools/ (n94_recipe.rs puts them on the install ramdisk). The ELF reader and the
Mach-O writer are iPhonEmu's (https://github.com/kazin1898/iPhonEmu, tools/ios-prep/guest_install.py,
GPL-2.0-or-later).
"""
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGEZERO = 0x4000
HEADER_ROOM = 0x1000        # the Mach-O header's page before the code
TEXT_VM = PAGEZERO
CODE_VM = TEXT_VM + HEADER_ROOM
KEYBAG_IMPORTS = {"it_dlopen": "_dlopen", "it_dlsym": "_dlsym", "it_dlerror": "_dlerror"}
LIBSYSTEM = "/usr/lib/libSystem.B.dylib"
DYLD = "/usr/lib/dyld"

CFLAGS = ["--target=armv7-none-eabi", "-marm", "-mcpu=cortex-a9", "-mfloat-abi=soft", "-O2",
          "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-stack-protector", "-fno-pic",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-jump-tables", "-Wall", "-Wextra"]

# One read-only, executable segment, _start first.
LINKER_SCRIPT = """ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS {
  . = 0x%x;
  .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  .data : { *(.data .data.*) } :text
  .bss : { *(.bss .bss.* COMMON) } :text
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
"""

# The dyld-linked helper: the code as above, then a writable segment on the next page (its first bytes the dl*
# slots dyld binds), so the Mach-O gets __TEXT and __DATA at the same addresses.
LINKER_SCRIPT_DYLD = """ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x%x;
  .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  . = ALIGN(0x1000);
  .data : { KEEP(*(.data.binds)) *(.data .data.*) } :data
  .bss : { *(.bss .bss.* COMMON) } :data
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
"""


# ---- ELF ----------------------------------------------------------------------------------------------

def elf_segments(path):
    """(entry, [(vaddr, flags, file bytes, memsz)]) of an ELF32 LE's PT_LOAD segments, and the ELF."""
    with open(path, "rb") as f:
        e = f.read()
    if e[:4] != b"\x7fELF" or e[4] != 1 or e[5] != 1:
        raise ValueError(f"{path}: not an ELF32 LE")
    e_entry, e_phoff = struct.unpack_from("<II", e, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", e, 0x2a)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(
            "<8I", e, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            segs.append((p_vaddr, p_flags, e[p_offset:p_offset + p_filesz], p_memsz))
    if not segs:
        raise ValueError(f"{path}: no loadable segment")
    return e_entry, segs, e


def elf_symbols(e):
    """{name: value} from an ELF32 LE's .symtab (the dyld helper's import slots)."""
    e_shoff = struct.unpack_from("<I", e, 0x20)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", e, 0x2e)
    sections = [struct.unpack_from("<10I", e, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    syms = {}
    for sh in sections:
        if sh[1] != 2:                                   # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            st_name, st_value = struct.unpack_from("<II", e, off)
            end = e.index(b"\0", strtab[4] + st_name)
            syms[e[strtab[4] + st_name:end].decode()] = st_value
    return syms


# ---- the Mach-O writer --------------------------------------------------------------------------------

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_EXECUTE = 2
MH_NOUNDEFS, MH_DYLDLINK, MH_TWOLEVEL = 1, 4, 0x80
LC_SEGMENT, LC_SYMTAB, LC_UNIXTHREAD, LC_DYSYMTAB = 1, 2, 5, 0xb
LC_LOAD_DYLIB, LC_LOAD_DYLINKER, LC_DYLD_INFO_ONLY = 0xc, 0xe, 0x80000022
VM_PROT_READ, VM_PROT_WRITE, VM_PROT_EXECUTE = 1, 2, 4
ARM_THREAD_STATE = 1
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_DO_BIND = 0x90
BIND_TYPE_POINTER = 1
PAGE = 0x1000


def page_round(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


def segment_cmd(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot):
    return struct.pack("<II16sIIIIIIII", LC_SEGMENT, 56, name.encode(), vmaddr, vmsize, fileoff, filesize,
                       maxprot, initprot, 0, 0)


def string_cmd(cmd, head, s):
    """A load command whose payload is `head` (packed) then a C string, padded to 8 bytes."""
    body = head + s.encode() + b"\0"
    size = (8 + len(body) + 7) & ~7
    return struct.pack("<II", cmd, size) + body.ljust(size - 8, b"\0")


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def thread_cmd(entry):
    # ARM_THREAD_STATE: r0-r12, sp, lr, pc, cpsr. sp 0 = the kernel's default stack; cpsr 0x10 = ARM mode.
    regs = [0] * 13 + [0, 0, entry, 0x10]
    return struct.pack("<IIII", LC_UNIXTHREAD, 16 + 4 * len(regs), ARM_THREAD_STATE, len(regs)) + \
        struct.pack("<%dI" % len(regs), *regs)


def static_macho(code, entry):
    """A static armv7 Mach-O executable mapping `code` at CODE_VM (its header on the page before)."""
    text_size = page_round(HEADER_ROOM + len(code))
    cmds = segment_cmd("__PAGEZERO", 0, PAGEZERO, 0, 0, 0, 0)
    cmds += segment_cmd("__TEXT", TEXT_VM, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE,
                        VM_PROT_READ | VM_PROT_EXECUTE)
    cmds += thread_cmd(entry)
    header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_EXECUTE, 3, len(cmds), MH_NOUNDEFS)
    return ((header + cmds).ljust(HEADER_ROOM, b"\0") + code).ljust(text_size, b"\0")


def dyld_macho(code, entry, data, data_vm, data_memsz, binds):
    """A dyld-linked armv7 Mach-O executable: `code` at CODE_VM, the writable `data` (memsz bytes) at data_vm,
    and `binds` = [(slot address, symbol)] bound by dyld from libSystem through LC_DYLD_INFO bind opcodes. Not
    PIE: the segments are mapped at their link addresses, so the bare-metal code's absolute addresses hold."""
    text_size = page_round(HEADER_ROOM + len(code))
    if data_vm != TEXT_VM + text_size:
        raise ValueError("the data must follow the code on the next page")
    data_size = page_round(max(data_memsz, len(data), 1))
    linkedit_vm = data_vm + data_size
    ops = bytearray([BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER])
    for addr, sym in binds:
        if not data_vm <= addr < data_vm + data_size:
            raise ValueError(f"bind slot 0x{addr:x} for {sym} is outside __DATA")
        ops += bytes([BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 2]) + uleb(addr - data_vm)
        ops += bytes([BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM]) + sym.encode() + b"\0"
        ops.append(BIND_OPCODE_DO_BIND)
    ops.append(BIND_OPCODE_DONE)
    ops = bytes(ops).ljust((len(ops) + 7) & ~7, b"\0")
    strtab = b"\0\0\0\0"                              # an empty symbol table: dyld binds by LC_DYLD_INFO
    linkedit = ops + strtab
    linkedit_off = text_size + data_size
    cmds = segment_cmd("__PAGEZERO", 0, PAGEZERO, 0, 0, 0, 0)
    cmds += segment_cmd("__TEXT", TEXT_VM, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE,
                        VM_PROT_READ | VM_PROT_EXECUTE)
    cmds += segment_cmd("__DATA", data_vm, data_size, text_size, data_size, VM_PROT_READ | VM_PROT_WRITE,
                        VM_PROT_READ | VM_PROT_WRITE)
    cmds += segment_cmd("__LINKEDIT", linkedit_vm, page_round(len(linkedit)), linkedit_off, len(linkedit),
                        VM_PROT_READ, VM_PROT_READ)
    cmds += struct.pack("<12I", LC_DYLD_INFO_ONLY, 48, 0, 0, linkedit_off, len(ops), 0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<6I", LC_SYMTAB, 24, linkedit_off + len(ops), 0, linkedit_off + len(ops), len(strtab))
    cmds += struct.pack("<20I", LC_DYSYMTAB, 80, *([0] * 18))
    cmds += string_cmd(LC_LOAD_DYLINKER, struct.pack("<I", 12), DYLD)
    cmds += string_cmd(LC_LOAD_DYLIB, struct.pack("<IIII", 24, 2, 0x10000, 0x10000), LIBSYSTEM)
    cmds += thread_cmd(entry)
    header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_EXECUTE, 10, len(cmds),
                         MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL)
    blob = header + cmds
    if len(blob) > HEADER_ROOM:
        raise ValueError("Mach-O header too large")
    return ((blob.ljust(HEADER_ROOM, b"\0") + code).ljust(text_size, b"\0") + data.ljust(data_size, b"\0")
            + linkedit)


# ---- the build ----------------------------------------------------------------------------------------

def tool(name):
    path = shutil.which(name)
    if not path:
        raise SystemExit(f"{name} not found: put MSYS2 UCRT64's clang and lld on the PATH")
    return path


def compile_link(name, script_text, out_dir):
    """The source NAME.c beside this file, compiled and linked under `script_text`: the ELF's path."""
    obj, elf, script = (os.path.join(out_dir, name + ext) for ext in (".o", ".elf", ".ld"))
    with open(script, "w") as f:
        f.write(script_text % CODE_VM)
    subprocess.run([tool("clang")] + CFLAGS + ["-c", os.path.join(HERE, name + ".c"), "-o", obj], check=True)
    subprocess.run([tool("ld.lld"), "-m", "armelf", "--no-dynamic-linker", "-nostdlib", "-static", "-T", script,
                    "-o", elf, obj], check=True)
    for p in (obj, script):
        os.remove(p)
    return elf


def build_install(out_dir):
    elf = compile_link("it_install", LINKER_SCRIPT, out_dir)
    entry, segs, _ = elf_segments(elf)
    if any(flags & 2 for _, flags, _, _ in segs) or min(v for v, _, _, _ in segs) != CODE_VM or entry != CODE_VM:
        raise SystemExit("it_install: expected one read-only segment with _start first at 0x%x" % CODE_VM)
    hi = max(v + m for v, _, _, m in segs)
    image = bytearray(hi - CODE_VM)
    for v, _, data, _ in segs:
        image[v - CODE_VM:v - CODE_VM + len(data)] = data
    with open(os.path.join(out_dir, "it_install"), "wb") as f:
        f.write(static_macho(bytes(image), entry))
    os.remove(elf)


def build_keybag(out_dir):
    elf = compile_link("it_keybag", LINKER_SCRIPT_DYLD, out_dir)
    entry, segs, e = elf_segments(elf)
    text = [s for s in segs if not s[1] & 2]
    data = [s for s in segs if s[1] & 2]
    if len(text) != 1 or len(data) != 1 or text[0][0] != CODE_VM or entry != CODE_VM:
        raise SystemExit("it_keybag: expected one code segment at 0x%x (entry there) and one data segment" % CODE_VM)
    syms = elf_symbols(e)
    binds = [(syms[slot], sym) for slot, sym in KEYBAG_IMPORTS.items()]
    with open(os.path.join(out_dir, "it_keybag"), "wb") as f:
        f.write(dyld_macho(text[0][2], entry, data[0][2], data[0][0], data[0][3], binds))
    os.remove(elf)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    build_install(out)
    build_keybag(out)
    print(out)
