/*
 * it_install: the iPhone 4S's installation helper (-M iPhone-4S, hw/arm/n94.c), the guest half of the one
 * boot that puts a prepared device on the emulated NAND. Adapted from iPhonEmu
 * (https://github.com/kazin1898/iPhonEmu, GPL-2.0-or-later) for firmwarekit, which prepares the volumes on
 * the host.
 *
 * The 4S kernel only has AppleSwissPPNFTL for its NAND, and the FTL's on-flash layout is its own business,
 * so the host cannot write a volume into the page store offline. Instead firmwarekit boots the IPSW's own
 * restore ramdisk once, with this program as /usr/local/bin/restored_external (the ramdisk's /etc/rc.boot
 * execs it where a real restore hands over to restored). It does what restored's partitioning does, with
 * the volume data coming from the host:
 *
 *   1. reads its table (TABLE_PATH, written by firmwarekit for this device; format below);
 *   2. fills the system partition and a seed partition at the end of the disk with *marker* pages: one
 *      4 KiB page each naming the host file and 4 KiB block that holds its real bytes (the table's magic,
 *      source, block, check). The page store (hw/arm/s5l8940_iop.c) serves such a page from that file, so
 *      the FTL holds the volumes in its own layout while the host keeps the bytes. Source 0 is the system
 *      volume firmwarekit built, source 1 the data volume it built (the seed);
 *   3. writes an MBR: the system volume, the data volume, and the seed;
 *   4. formats the effaceable storage (it_keybag, before the data volume is mounted: AppleKeyStore looks
 *      for its lockers when a protected volume mounts, and refuses every file creation without them);
 *   5. makes the data volume with newfs_hfs, protects it (kHFSContentProtectionBit) and mounts it, then
 *      creates the system keybag (it_keybag again);
 *   6. copies the seed's files into it. They can't simply be served from the host: on a protected volume
 *      xnu gives a file without a key one at its first open and reads it through the FMI's inline AES from
 *      then on, and the FTL's garbage collection turns marker pages into ordinary ones, which the AES
 *      would garble. Copied, each file gets its key at creation, as on a restored device;
 *   7. writes the MBR again without the seed, syncs and halts.
 *
 * Progress goes to the console as "it_install: ..." lines; the end is "it_install: done", a failure
 * "it_install: FAIL <why>", after which it halts (the preparation reads why in the serial log).
 *
 * Table (little-endian): "ITINST02", u32 marker check, char magic[16], u32 system LBA, u32 system blocks,
 * u32 seed blocks, u32 runs, then the runs: {u32 source, u32 first block, u32 count}, the blocks of each
 * source that hold data (a source's block n goes to its partition's block n); then the seed's symbolic links
 * (make_links).
 *
 * Built by build.py beside this file (the MSYS2 clang, a bare-metal armv7 ELF object, lld, then a static
 * Mach-O wrapper): no libc, Darwin's syscalls through svc 0x80 (number in r12, result in r0, carry set on
 * an error). No .data or .bss: the program is one read-only, executable segment, so every state lives on
 * the stack.
 */
#include <stdint.h>

#define BLOCK 4096u
#define CHUNK 32u                      /* marker pages per write (128 KiB on the stack) */

/* Darwin syscall numbers (bsd/kern/syscalls.master, iOS 6). */
#define SYS_exit 1
#define SYS_fork 2
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_wait4 7
#define SYS_unlink 10
#define SYS_chown 16
#define SYS_sync 36
#define SYS_ioctl 54
#define SYS_reboot 55
#define SYS_execve 59
#define SYS_chmod 15
#define SYS_dup2 90
#define SYS_fsync 95
#define SYS_mkdir 136
#define SYS_pread 153                  /* only the IT_PROBE build uses pread/pwrite */
#define SYS_pwrite 154
#define SYS_lseek 199
#define SYS_unmount 159
#define SYS_nanosleep 240

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define RB_HALT 0x08
#define RB_QUICK 0x400
#define DKIOCGETBLOCKSIZE 0x40046418u
#define DKIOCGETBLOCKCOUNT 0x40086419u
#define DKIOCSYNCHRONIZECACHE 0x20006416u


#define DATA_RDEV "/dev/rdisk0s2"

typedef long ssize_t;

/* The compiler may still open-code these names; give it the bodies (no libc here). */
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *p = d;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) {
        *p++ = *q++;
    }
    return d;
}

/* Four register arguments and up to two on the stack, as the kernel's ARM munge expects them. */
static long sys6(long n, long a, long b, long c, long d, long e, long f, int *err)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r12 __asm__("r12") = n;
    long carry;

    __asm__ volatile(
        "sub sp, sp, #16\n\t"
        "str %[e], [sp]\n\t"
        "str %[f], [sp, #4]\n\t"
        "svc #0x80\n\t"
        "add sp, sp, #16\n\t"
        "mov %[cy], #0\n\t"
        "movcs %[cy], #1\n\t"
        : "+r"(r0), [cy] "=&r"(carry)
        : "r"(r1), "r"(r2), "r"(r3), "r"(r12), [e] "r"(e), [f] "r"(f)
        : "memory", "cc");
    *err = (int)carry;
    return r0;
}

static long sys(long n, long a, long b, long c, long d)
{
    int err;
    long r = sys6(n, a, b, c, d, 0, 0, &err);
    return err ? -r : r;
}

/*
 * Positioned I/O on the block device: lseek(2) then read/write. The ARM kernel takes a syscall's
 * arguments as packed 32-bit words (APCS, as its own lseek handler reads them: fd, offset low,
 * offset high, whence), so a 64-bit offset is two consecutive words with no alignment pad. pread
 * and pwrite on the raw device returned EOF and EIO whatever the offset's placement (the
 * IT_PROBE build below records that), while lseek + read/write and the buffered /dev/disk0 work.
 */
#define SEEK_SET 0

/* 0, or -1 on an error. The result is the new offset, 64 bits in r0:r1, so only the carry tells an error: its
 * low word read as a long is negative past 2 GiB. */
static int lseek64(int fd, uint64_t off)
{
    int err;

    sys6(SYS_lseek, fd, (long)(uint32_t)off, (long)(uint32_t)(off >> 32), SEEK_SET, 0, 0, &err);
    return err ? -1 : 0;
}

static ssize_t xfer(int write, int fd, void *buf, uint32_t len, uint64_t off)
{
    if (lseek64(fd, off) < 0) {
        return -1;
    }
    return sys(write ? SYS_write : SYS_read, fd, (long)buf, len, 0);
}


int memcmp(const void *a, const void *b, unsigned long n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}


/*
 * fork(2) raw: Darwin returns the pid in r0 to both sides and tells them apart in r1 (1 in
 * the child, 0 in the parent); libc's stub turns the child's r0 into 0, done here instead.
 */
static long sys_fork(int *child)
{
    register long r0 __asm__("r0");
    register long r1 __asm__("r1");
    register long r12 __asm__("r12") = SYS_fork;
    long carry;

    __asm__ volatile(
        "svc #0x80\n\t"
        "mov %[cy], #0\n\t"
        "movcs %[cy], #1\n\t"
        : "=r"(r0), "=r"(r1), [cy] "=&r"(carry)
        : "r"(r12)
        : "memory", "cc");
    if (carry) {
        return -r0;
    }
    *child = r1 != 0;
    return r0;
}

static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void say(int con, const char *s)
{
    if (con >= 0) {
        sys(SYS_write, con, (long)s, slen(s), 0);
    }
}

static void say_num(int con, const char *s, uint32_t v)
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do {
        buf[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    say(con, s);
    say(con, buf + i);
    say(con, "\n");
}

static void msleep(uint32_t ms)
{
    uint32_t ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    sys(SYS_nanosleep, (long)ts, 0, 0, 0);
}

static void park(int con, const char *why)
{
    say(con, "it_install: FAIL ");
    say(con, why);
    say(con, "\n");
    sys(SYS_sync, 0, 0, 0, 0);
    msleep(2000);               /* the console's lines out, then off: the preparation reads why in its log */
    sys(SYS_reboot, RB_HALT | RB_QUICK, 0, 0, 0);
    for (;;) {
        msleep(10000);
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}


static int wait_open(const char *path, int flags, uint32_t tries)
{
    for (;;) {
        long fd = sys(SYS_open, (long)path, flags, 0, 0);
        if (fd >= 0) {
            return (int)fd;
        }
        if (!tries--) {
            return -1;
        }
        msleep(500);
    }
}


/* fork, exec and wait; the child's exit status, or -1. */
static int run(int con, const char *path, const char *const argv[])
{
    static const char *const envp[] = { 0 };
    int child = 0, status = -1;
    long pid = sys_fork(&child);

    if (pid < 0) {
        return -1;
    }
    if (child) {
        /* the child's stdout and stderr go to the console, so the tools' messages reach the log */
        long c = sys(SYS_open, (long)"/dev/console", O_RDWR, 0, 0);

        if (c >= 0) {
            sys(SYS_dup2, c, 1, 0, 0);
            sys(SYS_dup2, c, 2, 0, 0);
        }
        sys(SYS_execve, (long)path, (long)argv, (long)envp, 0);
        sys(SYS_exit, 127, 0, 0, 0);
    }
    while (sys(SYS_wait4, pid, (long)&status, 0, 0) < 0) {
        msleep(100);
    }
    say(con, "it_install: ran ");
    say(con, path);
    say_num(con, " -> status ", (uint32_t)status);
    return status;
}

/*
 * kHFSContentProtectionBit on or off in the data volume's header, the volume unmounted (the kernel goes
 * by the primary header; the alternate one is left as it is). On before the install mounts it, as a
 * restore leaves it: on a protected volume xnu gives a file without a cprotect attribute a new class D
 * key when it is first opened and reads its data through that key from then on, so a file this helper
 * wrote with the bit off (plaintext, no attribute) read as garbage on the next boot once the page store
 * applied the FMI's inline AES (the system keybag: "FATAL KEYBAG ERROR: kb_load"). Written with the bit
 * on, after it_keybag has formatted the effaceable storage, each file gets its key at creation and its
 * pages go through the AES like those of any file the daemons write. 0 on success.
 */
static int data_protect(uint8_t *buf, int on)
{
    int fd = (int)sys(SYS_open, (long)DATA_RDEV, O_RDWR, 0, 0);
    int st = 0;

    if (fd < 0) {
        return -1;
    }
    if (xfer(0, fd, buf, BLOCK, 0) != BLOCK || buf[1024] != 'H') {
        st = -2;
    } else {
        buf[1024 + 4] = on ? (buf[1024 + 4] | 0x40) : (buf[1024 + 4] & ~0x40);
        if (xfer(1, fd, buf, BLOCK, 0) != BLOCK) {
            st = -3;
        }
        sys(SYS_fsync, fd, 0, 0, 0);
    }
    sys(SYS_close, fd, 0, 0, 0);
    return st;
}

static void mkdir_as(const char *path, int mode, int uid)
{
    sys(SYS_mkdir, (long)path, mode, 0, 0);
    sys(SYS_chmod, (long)path, mode, 0, 0);
    if (uid) {
        sys(SYS_chown, (long)path, uid, uid, 0);
    }
}


/* ---- the table ----------------------------------------------------------------------------------- */

#define TABLE_PATH "/usr/local/share/it_install.table"
#define TABLE_HEAD 44
#define RUN_BYTES 12
#define RUNS_PER_READ 256

typedef struct {
    uint32_t check;
    uint8_t magic[16];
    uint32_t system_lba, system_blocks, seed_blocks, runs;
} Table;

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_full(int fd, void *buf, uint32_t len)
{
    uint8_t *p = buf;

    while (len) {
        long n = sys(SYS_read, fd, (long)p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

/* The table's header; the open file is left at its first run. */
static int table_open(Table *t)
{
    uint8_t h[TABLE_HEAD];
    int fd = (int)sys(SYS_open, (long)TABLE_PATH, O_RDONLY, 0, 0);

    if (fd < 0 || read_full(fd, h, TABLE_HEAD) != 0 || memcmp(h, "ITINST02", 8) != 0) {
        return -1;
    }
    t->check = get32(h + 8);
    memcpy(t->magic, h + 12, 16);
    t->system_lba = get32(h + 28);
    t->system_blocks = get32(h + 32);
    t->seed_blocks = get32(h + 36);
    t->runs = get32(h + 40);
    return fd;
}

static void marker(const Table *t, uint8_t *page, uint32_t source, uint32_t block)
{
    memset(page, 0, BLOCK);
    memcpy(page, t->magic, 16);
    put32(page + 16, source);
    put32(page + 20, block);
    put32(page + 24, source ^ block ^ t->check);
}

/* Up to three Apple_HFS (0xAF) entries, LBAs in 4 KiB sectors; a count of 0 leaves the entry out. */
static void mbr(uint8_t *sector, const uint32_t start[3], const uint32_t count[3])
{
    uint8_t *e = sector + 446;

    memset(sector, 0, 512);
    for (int i = 0; i < 3; i++, e += 16) {
        if (!count[i]) {
            continue;
        }
        e[0] = 0x00; e[1] = 0xfe; e[2] = 0xff; e[3] = 0xff; e[4] = 0xaf; e[5] = 0xfe; e[6] = 0xff; e[7] = 0xff;
        put32(e + 8, start[i]);
        put32(e + 12, count[i]);
    }
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static void write_mbr(int con, int disk, uint8_t *buf, const uint32_t start[3], const uint32_t count[3])
{
    memset(buf, 0, BLOCK);
    mbr(buf, start, count);
    if (xfer(1, disk, buf, BLOCK, 0) != BLOCK) {
        park(con, "MBR write");
    }
    sys(SYS_fsync, disk, 0, 0, 0);
    sys(SYS_ioctl, disk, DKIOCSYNCHRONIZECACHE, 0, 0);
}

#define DATA_MNT "/mnt2"

/* ---- copying the seed into the protected data volume ---------------------------------------------- */

#define SYS_symlink 57
#define SYS_fchown 123
#define SYS_fchmod 124
#define SYS_lchown 364
#define SYS_lstat64 340
#define SYS_getdirentries64 344
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define PATH_MAX 1024

/* What the copy needs of a struct stat64: st_mode at 4, st_uid at 16, st_gid at 20 (before any 64-bit field
 * whose alignment could move them). */
typedef struct {
    uint32_t mode, uid, gid;
} Owner;

static int owner_of(const char *path, Owner *o)
{
    uint8_t st[160];

    if (sys(SYS_lstat64, (long)path, (long)st, 0, 0) < 0) {
        return -1;
    }
    o->mode = (uint32_t)(st[4] | st[5] << 8) & 07777;
    o->uid = get32(st + 16);
    o->gid = get32(st + 20);
    return 0;
}

static uint32_t append(char *path, uint32_t len, const char *name, uint32_t n)
{
    if (len + 1 + n + 1 > PATH_MAX) {
        return 0;
    }
    path[len] = '/';
    memcpy(path + len + 1, name, n);
    path[len + 1 + n] = 0;
    return len + 1 + n;
}

/* The volume's own files at its root, which newfs_hfs made anew. */
static int volume_private(const char *name, uint32_t n)
{
    static const char *const names[] = { ".journal", ".journal_info_block", ".Trashes", ".fseventsd",
                                         ".HFS+ Private Directory Data\r", 0 };

    static const char links[] = "HFS+ Private Data";   /* the hard links' directory, after four U+2400 */
    uint32_t k = sizeof(links) - 1;

    if (!n || !name[0] || (n > k && memcmp(name + n - k, links, k) == 0)) {
        return 1;
    }
    for (int i = 0; names[i]; i++) {
        uint32_t k = slen(names[i]);
        if (k == n && memcmp(name, names[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 0, or a negative errno (-1000 - errno when the destination's open failed). */
static int copy_file(const char *from, const char *to, const Owner *o, uint8_t *buf, uint32_t size)
{
    int in = (int)sys(SYS_open, (long)from, O_RDONLY, 0, 0), out, ok = 0;
    long n;

    if (in < 0) {
        return in;
    }
    out = (int)sys(SYS_open, (long)to, O_WRONLY | O_CREAT | O_TRUNC, (long)(o->mode | 0600), 0);
    if (out < 0) {
        sys(SYS_close, in, 0, 0, 0);
        return out - 1000;
    }
    while ((n = sys(SYS_read, in, (long)buf, size, 0)) > 0) {
        if (sys(SYS_write, out, (long)buf, n, 0) != n) {
            ok = -1;
            break;
        }
    }
    if (n < 0) {
        ok = -1;
    }
    sys(SYS_fchown, out, o->uid, o->gid, 0);
    sys(SYS_fchmod, out, o->mode, 0, 0);
    sys(SYS_close, out, 0, 0, 0);
    sys(SYS_close, in, 0, 0, 0);
    return ok;
}

typedef struct {
    int con;
    uint8_t *buf;                       /* file data */
    uint32_t size;
    uint32_t files, failed;
} Copy;

/* An entry not copied: counted, and named on the console with the step and the error. */
static void miss(Copy *c, const char *what, const char *path, long err)
{
    c->failed++;
    say(c->con, "it_install: not copied (");
    say(c->con, what);
    say(c->con, ") ");
    say(c->con, path);
    say_num(c->con, " error ", (uint32_t)-err);
}

/* Copies the directory at `from` into the one at `to` (both PATH_MAX buffers, extended in place). */
static void copy_tree(Copy *c, char *from, uint32_t fl, char *to, uint32_t tl, int depth)
{
    uint8_t dents[4096] __attribute__((aligned(8)));
    uint64_t base = 0;
    long got;
    int dir = (int)sys(SYS_open, (long)from, O_RDONLY, 0, 0);

    if (dir < 0 || depth > 64) {
        miss(c, "open dir", from, dir);
        return;
    }
    while ((got = sys(SYS_getdirentries64, dir, (long)dents, sizeof(dents), (long)&base)) > 0) {
        for (long at = 0; at < got;) {
            uint8_t *d = dents + at;
            uint32_t reclen = d[16] | d[17] << 8, n = d[18] | d[19] << 8, type = d[20];
            const char *name = (const char *)d + 21;
            uint32_t f2, t2;
            Owner o;

            if (!reclen) {
                break;
            }
            at += reclen;
            if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.') ||
                (depth == 0 && volume_private(name, n))) {
                continue;
            }
            f2 = append(from, fl, name, n);
            t2 = append(to, tl, name, n);
            if (!f2 || !t2 || owner_of(from, &o) != 0) {
                miss(c, "lstat", from, 0);
            } else if (type == DT_DIR) {
                sys(SYS_mkdir, (long)to, (long)(o.mode | 0700), 0, 0);
                copy_tree(c, from, f2, to, t2, depth + 1);
                sys(SYS_chown, (long)to, o.uid, o.gid, 0);
                sys(SYS_chmod, (long)to, o.mode, 0, 0);
            } else if (type == DT_REG) {
                int r = copy_file(from, to, &o, c->buf, c->size);

                if (r == 0) {
                    c->files++;
                } else {
                    miss(c, "copy", to, r);
                }
            }
            from[fl] = 0;
            to[tl] = 0;
        }
    }
    sys(SYS_close, dir, 0, 0, 0);
}

/*
 * The seed's symbolic links, from the table (after the runs): the kernel won't read a link's target off the seed, so
 * firmwarekit lists them. Each is {u16 path length, u16 target length, u32 uid, u32 gid, path, target}, the path
 * relative to the data volume's root. The copy above leaves links out.
 */
static void make_links(Copy *c, int table)
{
    char path[PATH_MAX], target[PATH_MAX];
    uint8_t head[12];
    uint32_t count, made = 0;

    if (read_full(table, head, 4) != 0) {
        miss(c, "table", "links", 0);
        return;
    }
    count = get32(head);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pl, tl, base = sizeof(DATA_MNT) - 1;

        if (read_full(table, head, 12) != 0) {
            miss(c, "table", "links", 0);
            return;
        }
        pl = head[0] | head[1] << 8;
        tl = head[2] | head[3] << 8;
        if (base + 1 + pl >= PATH_MAX || tl >= PATH_MAX) {
            miss(c, "table", "link too long", 0);
            return;
        }
        memcpy(path, DATA_MNT "/", base + 1);
        if (read_full(table, path + base + 1, pl) != 0 || read_full(table, target, tl) != 0) {
            miss(c, "table", "links", 0);
            return;
        }
        path[base + 1 + pl] = 0;
        target[tl] = 0;
        if (sys(SYS_symlink, (long)target, (long)path, 0, 0) < 0) {
            miss(c, "symlink", path, 0);
            continue;
        }
        sys(SYS_lchown, (long)path, get32(head + 4), get32(head + 8), 0);
        made++;
    }
    say_num(c->con, "it_install: symbolic links made ", made);
}

/* ---- the install ---------------------------------------------------------------------------------- */

#define DATA_DEV "/dev/disk0s2"
#define SEED_DEV "/dev/disk0s3"
#define SEED_MNT "/mnt1"
#define KEYBAG_HELPER "/usr/bin/TiSerialFlasher"   /* it_keybag's slot on the ramdisk (firmwarekit n94_recipe.rs) */

/* Writes the marker pages of every run, `chunk` pages per write; partition start of source 0, 1. */
static void write_markers(int con, int disk, int table, const Table *t, const uint32_t start[2], uint8_t *buf,
                          uint32_t chunk)
{
    uint8_t runs[RUNS_PER_READ * RUN_BYTES];
    uint32_t done = 0, pages = 0, seed = 0;

    while (done < t->runs) {
        uint32_t batch = t->runs - done < RUNS_PER_READ ? t->runs - done : RUNS_PER_READ;

        if (read_full(table, runs, batch * RUN_BYTES) != 0) {
            park(con, "table runs");
        }
        for (uint32_t r = 0; r < batch; r++) {
            uint32_t source = get32(runs + r * RUN_BYTES), first = get32(runs + r * RUN_BYTES + 4);
            uint32_t count = get32(runs + r * RUN_BYTES + 8), n;

            if (source > 1) {
                park(con, "table source");
            }
            if (source == 1 && !seed) {
                seed = 1;
                say_num(con, "it_install: the seed's runs, from disk block ", start[1] + first);
            }
            for (uint32_t i = 0; i < count; i += n) {
                n = count - i < chunk ? count - i : chunk;
                for (uint32_t k = 0; k < n; k++) {
                    marker(t, buf + k * BLOCK, source, first + i + k);
                }
                ssize_t w = xfer(1, disk, buf, n * BLOCK, (uint64_t)(start[source] + first + i) * BLOCK);

                if (w != (ssize_t)(n * BLOCK)) {
                    say_num(con, "it_install: write of source ", source);
                    say_num(con, "it_install:   at block ", first + i);
                    say_num(con, "it_install:   disk block ", start[source] + first + i);
                    say_num(con, "it_install:   pages ", n);
                    say_num(con, "it_install:   returned ", (uint32_t)w);
                    park(con, "marker write");
                }
                pages += n;
                if ((pages & 0xffff) < n) {
                    say_num(con, "it_install: marker pages ", pages);
                }
            }
        }
        done += batch;
    }
    say_num(con, "it_install: markers written, pages ", pages);
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    static const char *const newfs[] = { "newfs_hfs", "-s", "-J", "-v", "Data", DATA_RDEV, 0 };
    static const char *const mount_data[] = { "mount_hfs", DATA_DEV, DATA_MNT, 0 };
    /* read-write: a journaled volume is refused read-only while its journal needs replaying; the seed is
     * thrown away after the copy */
    static const char *const mount_seed[] = { "mount_hfs", SEED_DEV, SEED_MNT, 0 };
    static const char *const keybag[] = { "it_keybag", 0 };
    uint8_t buf[CHUNK * BLOCK] __attribute__((aligned(16)));
    char from[PATH_MAX], to[PATH_MAX];
    uint32_t start[3], count[3], bsize = 0, chunk;
    uint64_t blocks = 0;
    Table t;
    Copy c;
    int disk, con, table;

    con = wait_open("/dev/console", O_WRONLY, 20);
    say(con, "it_install: hello\n");
    table = table_open(&t);
    if (table < 0) {
        park(con, "no table at " TABLE_PATH);
    }
    disk = wait_open("/dev/rdisk0", O_RDWR, 240);
    if (disk < 0) {
        park(con, "no /dev/rdisk0");
    }
    if (sys(SYS_ioctl, disk, DKIOCGETBLOCKSIZE, (long)&bsize, 0) < 0 ||
        sys(SYS_ioctl, disk, DKIOCGETBLOCKCOUNT, (long)&blocks, 0) < 0) {
        park(con, "DKIOCGETBLOCK*");
    }
    say_num(con, "it_install: blocks ", (uint32_t)blocks);
    /* system, then data, then the seed at the end (a whole number of 8 blocks: 32 KiB) */
    start[0] = t.system_lba;
    count[0] = t.system_blocks;
    start[2] = (uint32_t)((blocks - t.seed_blocks) & ~7ull);
    count[2] = t.seed_blocks;
    start[1] = start[0] + count[0];
    if (bsize != BLOCK || blocks > 0xffffffffull || start[2] < start[1] + 65536) {
        park(con, "disk geometry");
    }
    count[1] = start[2] - start[1];

    /* How much the block device takes per write: the largest power of two up to CHUNK that works. */
    for (chunk = CHUNK; chunk; chunk >>= 1) {
        for (uint32_t i = 0; i < chunk; i++) {
            marker(&t, buf + i * BLOCK, 0, i);
        }
        if (xfer(1, disk, buf, chunk * BLOCK, (uint64_t)start[0] * BLOCK) == (ssize_t)(chunk * BLOCK)) {
            break;
        }
    }
    if (!chunk) {
        park(con, "no write size works");
    }
    {
        uint32_t starts[2] = { start[0], start[2] };
        write_markers(con, disk, table, &t, starts, buf, chunk);
    }
    write_mbr(con, disk, buf, start, count);
    sys(SYS_close, disk, 0, 0, 0);      /* closing the whole disk after a write re-probes its partitions */
    say(con, "it_install: MBR written\n");

    /* The data volume, protected before anything is written there (see data_protect). */
    {
        int fd = wait_open(DATA_RDEV, O_RDONLY, 120);

        if (fd < 0) {
            park(con, "no " DATA_RDEV);
        }
        sys(SYS_close, fd, 0, 0, 0);
    }
    if (run(con, "/sbin/newfs_hfs", newfs) != 0) {
        park(con, "newfs_hfs");
    }
    say_num(con, "it_install: content protection on -> ", (uint32_t)data_protect(buf, 1));
    say(con, "it_install: effaceable storage first (the keybag waits for the mount)\n");
    run(con, KEYBAG_HELPER, keybag);
    if (run(con, "/sbin/mount_hfs", mount_data) != 0) {
        park(con, "mount " DATA_DEV);
    }
    mkdir_as(DATA_MNT "/keybags", 0755, 0);
    if (run(con, KEYBAG_HELPER, keybag) != 0) {
        park(con, "no system keybag");
    }
    say(con, "it_install: system keybag in place\n");

    /* The seed's files into the data volume. */
    sys(SYS_mkdir, (long)SEED_MNT, 0755, 0, 0);
    if (run(con, "/sbin/mount_hfs", mount_seed) != 0) {
        park(con, "mount " SEED_DEV);
    }
    memcpy(from, SEED_MNT, sizeof(SEED_MNT));
    memcpy(to, DATA_MNT, sizeof(DATA_MNT));
    c.con = con;
    c.buf = buf;
    c.size = sizeof(buf);
    c.files = c.failed = 0;
    copy_tree(&c, from, slen(from), to, slen(to), 0);
    say_num(con, "it_install: seed copied, entries ", c.files);
    make_links(&c, table);
    sys(SYS_close, table, 0, 0, 0);
    if (c.failed) {
        say_num(con, "it_install: entries not copied ", c.failed);
        park(con, "seed copy");
    }
    sys(SYS_sync, 0, 0, 0, 0);
    say_num(con, "it_install: unmount seed -> ", (uint32_t)sys(SYS_unmount, (long)SEED_MNT, 0, 0, 0));
    say_num(con, "it_install: unmount data -> ", (uint32_t)sys(SYS_unmount, (long)DATA_MNT, 0, 0, 0));

    /* The MBR without the seed: its pages stay in the FTL until the guest writes over them. */
    disk = wait_open("/dev/rdisk0", O_RDWR, 20);
    if (disk < 0) {
        park(con, "no /dev/rdisk0");
    }
    count[2] = 0;
    write_mbr(con, disk, buf, start, count);
    sys(SYS_close, disk, 0, 0, 0);
    sys(SYS_sync, 0, 0, 0, 0);
    msleep(1000);
    say(con, "it_install: done\n");
    sys(SYS_sync, 0, 0, 0, 0);
    msleep(1000);
    sys(SYS_reboot, RB_HALT | RB_QUICK, 0, 0, 0);
    for (;;) {
        msleep(10000);
    }
}
