/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded private symbolic IR store. Never contains executable/native code. */
#include "qemu/osdep.h"
#include "tcg/ir-cache-store.h"

#ifndef _WIN32
#include <dirent.h>
#include <sys/file.h>

#define RECORD_HEADER 104
static const uint8_t magic[8] = { 'L', 'T', 'I', 'R', 1, 0, 0, 0 };

struct TCGIRCacheStore {
    int directory_fd;
    int lock_fd;
    uint8_t build[32];
    GHashTable *entries;
    size_t disk_bytes;
    unsigned disk_records;
    unsigned temporary;
    int64_t diagnostics_origin_us;
    TCGIRCacheStoreDiagnostics diagnostics;
};

static void observe_capacity(TCGIRCacheStore *s)
{
    if (s->diagnostics.enabled && !s->diagnostics.capacity_observed &&
        (s->disk_records >= TCG_IR_CACHE_MAX_RECORDS ||
         s->disk_bytes >= TCG_IR_CACHE_MAX_BYTES - RECORD_HEADER)) {
        s->diagnostics.capacity_observed = true;
        s->diagnostics.first_capacity_us =
            g_get_monotonic_time() - s->diagnostics_origin_us;
    }
}

void tcg_ir_store_enable_diagnostics(TCGIRCacheStore *s)
{
    if (s && !s->diagnostics.enabled) {
        s->diagnostics.enabled = true;
        s->diagnostics_origin_us = g_get_monotonic_time();
        observe_capacity(s);
    }
}

void tcg_ir_store_get_diagnostics(TCGIRCacheStore *s,
                                TCGIRCacheStoreDiagnostics *out)
{
    memset(out, 0, sizeof(*out));
    if (s && s->diagnostics.enabled) {
        *out = s->diagnostics;
        out->records = s->disk_records;
        out->bytes = s->disk_bytes;
        out->elapsed_us = g_get_monotonic_time() - s->diagnostics_origin_us;
    }
}

static int observed_fsync(TCGIRCacheStore *s, int fd, bool directory)
{
    int ret, saved_errno;
    int64_t begin;
    uint64_t elapsed;
    if (!s->diagnostics.enabled) {
        return fsync(fd);
    }
    begin = g_get_monotonic_time();
    ret = fsync(fd);
    saved_errno = errno;
    elapsed = g_get_monotonic_time() - begin;
    if (directory) {
        s->diagnostics.directory_fsync_calls++;
        s->diagnostics.directory_fsync_errors += ret != 0;
        s->diagnostics.directory_fsync_us += elapsed;
    } else {
        s->diagnostics.file_fsync_calls++;
        s->diagnostics.file_fsync_errors += ret != 0;
        s->diagnostics.file_fsync_us += elapsed;
    }
    errno = saved_errno;
    return ret;
}

static bool private_inode(int fd, bool directory)
{
    struct stat st;

    return fstat(fd, &st) == 0 && st.st_uid == geteuid() &&
           !(st.st_mode & 0077) &&
           (directory ? S_ISDIR(st.st_mode) :
                        (S_ISREG(st.st_mode) && st.st_nlink == 1));
}

static void digest(const void *data, size_t len, uint8_t out[32])
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    size_t n = 32;

    g_checksum_update(c, data, len);
    g_checksum_get_digest(c, out, &n);
    g_checksum_free(c);
}

static char *key_name(const uint8_t key[32])
{
    char *name = g_malloc(65);
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        name[i * 2] = hex[key[i] >> 4];
        name[i * 2 + 1] = hex[key[i] & 15];
    }
    name[64] = 0;
    return name;
}

static bool record_name(const char *name)
{
    if (strlen(name) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (!g_ascii_isdigit(name[i]) && !(name[i] >= 'a' && name[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool transfer(int fd, void *data, size_t len, bool writing)
{
    uint8_t *p = data;

    while (len) {
        ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

static void read_record(TCGIRCacheStore *s, const char *name)
{
    struct stat st;
    uint8_t hash[32];
    g_autofree uint8_t *record = NULL;
    g_autofree char *expected_name = NULL;
    int fd = openat(s->directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        return;
    }
    if (!private_inode(fd, false) || fstat(fd, &st) ||
        st.st_size < RECORD_HEADER ||
        st.st_size > RECORD_HEADER + TCG_IR_CACHE_MAX_RECORD ||
        ++s->disk_records > TCG_IR_CACHE_MAX_RECORDS ||
        st.st_size > TCG_IR_CACHE_MAX_BYTES - s->disk_bytes) {
        close(fd);
        /* Refuse this store, rather than allocating from attacker sizes. */
        s->disk_bytes = TCG_IR_CACHE_MAX_BYTES + 1;
        return;
    }
    s->disk_bytes += st.st_size;
    record = g_malloc(st.st_size);
    if (!transfer(fd, record, st.st_size, false)) {
        close(fd);
        return;
    }
    close(fd);
    if (memcmp(record, magic, 8) || memcmp(record + 8, s->build, 32)) {
        return;
    }
    expected_name = key_name(record + 40);
    digest(record + RECORD_HEADER, st.st_size - RECORD_HEADER, hash);
    if (strcmp(expected_name, name) || memcmp(hash, record + 72, 32)) {
        return;
    }
    g_hash_table_insert(s->entries, g_strdup(name),
                        g_bytes_new(record + RECORD_HEADER,
                                    st.st_size - RECORD_HEADER));
}

TCGIRCacheStore *tcg_ir_store_open(const char *directory,
                                 const uint8_t build[32])
{
    TCGIRCacheStore *s;
    DIR *dir;
    struct dirent *entry;
    unsigned scanned_records = 0;
    int fd;

    if (!directory || directory[0] != '/') {
        return NULL;
    }
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return NULL;
    }
    if (!private_inode(fd, true)) {
        close(fd);
        return NULL;
    }
    s = g_new0(TCGIRCacheStore, 1);
    s->directory_fd = fd;
    s->lock_fd = openat(fd, ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                        0600);
    s->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)g_bytes_unref);
    memcpy(s->build, build, 32);
    if (s->lock_fd < 0 || !private_inode(s->lock_fd, false) ||
        flock(s->lock_fd, LOCK_EX | LOCK_NB)) {
        tcg_ir_store_close(s);
        return NULL;
    }
    dir = fdopendir(dup(fd));
    if (!dir) {
        tcg_ir_store_close(s);
        return NULL;
    }
    while ((entry = readdir(dir))) {
        if (record_name(entry->d_name)) {
            if (++scanned_records > TCG_IR_CACHE_MAX_RECORDS) {
                closedir(dir);
                tcg_ir_store_close(s);
                return NULL;
            }
            read_record(s, entry->d_name);
            if (s->disk_bytes > TCG_IR_CACHE_MAX_BYTES) {
                closedir(dir);
                tcg_ir_store_close(s);
                return NULL;
            }
        } else if (strcmp(entry->d_name, ".") &&
                   strcmp(entry->d_name, "..") &&
                   strcmp(entry->d_name, ".lock")) {
            /* Includes interrupted temporary files: bound the entire store,
             * never delete unknown files or follow their links. */
            closedir(dir);
            tcg_ir_store_close(s);
            return NULL;
        }
    }
    closedir(dir);
    return s;
}

GBytes *tcg_ir_store_lookup(TCGIRCacheStore *s, const uint8_t key[32])
{
    g_autofree char *name = key_name(key);
    GBytes *result = g_hash_table_lookup(s->entries, name);

    return result ? g_bytes_ref(result) : NULL;
}

bool tcg_ir_store_can_put(TCGIRCacheStore *s, const uint8_t key[32])
{
    g_autofree char *name = NULL;

    if (s) {
        observe_capacity(s);
    }
    if (!s || s->disk_records >= TCG_IR_CACHE_MAX_RECORDS ||
        s->disk_bytes >= TCG_IR_CACHE_MAX_BYTES - RECORD_HEADER) {
        return false;
    }
    name = key_name(key);
    return !g_hash_table_contains(s->entries, name);
}

bool tcg_ir_store_put(TCGIRCacheStore *s, const uint8_t key[32], GBytes *data)
{
    g_autofree char *name = key_name(key);
    g_autofree char *temporary = NULL;
    g_autofree uint8_t *record = NULL;
    size_t len;
    const void *payload = g_bytes_get_data(data, &len);
    int fd;
    bool ok;

    if (s && s->diagnostics.enabled) {
        s->diagnostics.put_calls++;
    }
    if (!tcg_ir_store_can_put(s, key) || !len ||
        len > TCG_IR_CACHE_MAX_RECORD ||
        len + RECORD_HEADER > TCG_IR_CACHE_MAX_BYTES - s->disk_bytes) {
        return false;
    }
    temporary = g_strdup_printf(".tmp-%ld-%u", (long)getpid(), s->temporary++);
    fd = openat(s->directory_fd, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return false;
    }
    record = g_malloc(RECORD_HEADER + len);
    memcpy(record, magic, 8);
    memcpy(record + 8, s->build, 32);
    memcpy(record + 40, key, 32);
    digest(payload, len, record + 72);
    memcpy(record + RECORD_HEADER, payload, len);
    ok = transfer(fd, record, RECORD_HEADER + len, true) &&
         observed_fsync(s, fd, false) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) {
        ok = renameat(s->directory_fd, temporary, s->directory_fd, name) == 0;
    }
    if (!ok) {
        unlinkat(s->directory_fd, temporary, 0);
        return false;
    }
    /* A directory fsync failure loses only a disposable cache entry on crash;
     * the published file is complete and checksummed. */
    observed_fsync(s, s->directory_fd, true);
    s->disk_bytes += RECORD_HEADER + len;
    s->disk_records++;
    if (s->diagnostics.enabled) {
        s->diagnostics.put_success++;
        observe_capacity(s);
    }
    g_hash_table_insert(s->entries, g_strdup(name), g_bytes_ref(data));
    return true;
}

void tcg_ir_store_close(TCGIRCacheStore *s)
{
    if (s) {
        if (s->lock_fd >= 0) {
            close(s->lock_fd);
        }
        close(s->directory_fd);
        g_hash_table_unref(s->entries);
        g_free(s);
    }
}
#else
struct TCGIRCacheStore { char unused; };
TCGIRCacheStore *tcg_ir_store_open(const char *directory,
                                 const uint8_t build[32])
{
    return NULL;
}
GBytes *tcg_ir_store_lookup(TCGIRCacheStore *s, const uint8_t key[32])
{
    return NULL;
}
bool tcg_ir_store_can_put(TCGIRCacheStore *s, const uint8_t key[32])
{
    return false;
}
bool tcg_ir_store_put(TCGIRCacheStore *s, const uint8_t key[32], GBytes *data)
{
    return false;
}
void tcg_ir_store_close(TCGIRCacheStore *s) { }
void tcg_ir_store_enable_diagnostics(TCGIRCacheStore *s) { }
void tcg_ir_store_get_diagnostics(TCGIRCacheStore *s,
                                TCGIRCacheStoreDiagnostics *out)
{
    memset(out, 0, sizeof(*out));
}
#endif
