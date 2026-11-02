/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "tcg/ir-cache-store.h"
#include <glib/gstdio.h>

static uint8_t build[32] = { 0x12 }, key[32] = { 0x34 };
static char *directory;

static void setup(void)
{
    directory = g_dir_make_tmp("lighttouch-ir-store-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_assert_cmpint(chmod(directory, 0700), ==, 0);
}

static void cleanup(void)
{
    GDir *d = g_dir_open(directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(d))) {
        g_autofree char *p = g_build_filename(directory, name, NULL);
        g_assert_cmpint(unlink(p), ==, 0);
    }
    g_dir_close(d);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_clear_pointer(&directory, g_free);
}

static char *record_path(void)
{
    char name[65];
    for (int i = 0; i < 32; i++) { sprintf(name + i * 2, "%02x", key[i]); }
    return g_build_filename(directory, name, NULL);
}

static void put_record(void)
{
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    GBytes *b = g_bytes_new_static("symbolic guest integers", 23);
    g_assert_nonnull(s);
    g_assert_true(tcg_ir_store_can_put(s, key));
    g_assert_true(tcg_ir_store_put(s, key, b));
    g_assert_false(tcg_ir_store_can_put(s, key));
    g_assert_false(tcg_ir_store_put(s, key, b));
    g_bytes_unref(b);
    tcg_ir_store_close(s);
}

static void roundtrip(void)
{
    setup();
    put_record();
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    GBytes *b = tcg_ir_store_lookup(s, key);
    size_t n;
    const char *p = g_bytes_get_data(b, &n);
    g_assert_cmpmem(p, n, "symbolic guest integers", 23);
    g_bytes_unref(b);
    tcg_ir_store_close(s);
    cleanup();
}

static void stale_engine(void)
{
    setup(); put_record();
    uint8_t changed[32] = { 0x99 };
    TCGIRCacheStore *s = tcg_ir_store_open(directory, changed);
    g_assert_nonnull(s);
    g_assert_null(tcg_ir_store_lookup(s, key));
    tcg_ir_store_close(s);
    cleanup();
}

static void checksum_failure(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    int fd = open(path, O_RDWR);
    g_assert_cmpint(pwrite(fd, "X", 1, 105), ==, 1);
    close(fd);
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    g_assert_null(tcg_ir_store_lookup(s, key));
    tcg_ir_store_close(s);
    cleanup();
}

static void private_directory(void)
{
    setup(); chmod(directory, 0755);
    g_assert_null(tcg_ir_store_open(directory, build));
    chmod(directory, 0700);
    g_autofree char *link = g_strconcat(directory, "-link", NULL);
    g_assert_cmpint(symlink(directory, link), ==, 0);
    g_assert_null(tcg_ir_store_open(link, build));
    unlink(link);
    g_assert_null(tcg_ir_store_open(".", build));
    cleanup();
}

static void private_record(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    chmod(path, 0644);
    g_assert_null(tcg_ir_store_open(directory, build));
    cleanup();
}

static void exclusive_owner(void)
{
    setup();
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    g_assert_null(tcg_ir_store_open(directory, build));
    tcg_ir_store_close(s);
    s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    tcg_ir_store_close(s);
    cleanup();
}

static void interrupted_write(void)
{
    setup(); put_record();
    g_autofree char *p = g_build_filename(directory, ".tmp-crashed", NULL);
    g_assert_true(g_file_set_contents(p, "partial", 7, NULL));
    chmod(p, 0600);
    g_assert_null(tcg_ir_store_open(directory, build));
    cleanup();
}

static void truncated_record(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    g_assert_cmpint(truncate(path, 103), ==, 0);
    g_assert_null(tcg_ir_store_open(directory, build));
    cleanup();
}

static void huge_record(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    g_assert_cmpint(truncate(path, TCG_IR_CACHE_MAX_RECORD + 105), ==, 0);
    g_assert_null(tcg_ir_store_open(directory, build));
    cleanup();
}

static void symlink_record(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(symlink("/dev/null", path), ==, 0);
    /* Refuse the store or ignore the unavailable record; never follow it. */
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    if (s) {
        g_assert_null(tcg_ir_store_lookup(s, key));
        tcg_ir_store_close(s);
    }
    cleanup();
}

static void hardlink_record(void)
{
    setup(); put_record();
    g_autofree char *path = record_path();
    g_autofree char *link_path = g_strconcat(directory, "-hardlink", NULL);
    g_assert_cmpint(link(path, link_path), ==, 0);
    g_assert_null(tcg_ir_store_open(directory, build));
    unlink(link_path);
    cleanup();
}

static void bounded_directory(void)
{
    setup();
    for (unsigned i = 0; i <= TCG_IR_CACHE_MAX_RECORDS; i++) {
        char name[65];
        snprintf(name, sizeof(name), "%064x", i);
        g_autofree char *p = g_build_filename(directory, name, NULL);
        g_assert_cmpint(symlink("/dev/null", p), ==, 0);
    }
    g_assert_null(tcg_ir_store_open(directory, build));
    cleanup();
}

static void count_capacity_keeps_reads(void)
{
    setup();
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    GBytes *data = g_bytes_new_static("x", 1);
    uint8_t candidate[32] = { 0x56 };
    uint8_t first[32];
    TCGIRCacheStoreDiagnostics d, again;
    tcg_ir_store_enable_diagnostics(s);
    memcpy(first, candidate, sizeof(first));
    g_assert_false(tcg_ir_store_can_put(NULL, candidate));
    for (uint32_t i = 0; i < TCG_IR_CACHE_MAX_RECORDS; i++) {
        memcpy(candidate + 1, &i, sizeof(i));
        g_assert_true(tcg_ir_store_can_put(s, candidate));
        g_assert_true(tcg_ir_store_put(s, candidate, data));
    }
    uint32_t beyond = TCG_IR_CACHE_MAX_RECORDS;
    memcpy(candidate + 1, &beyond, sizeof(beyond));
    g_assert_false(tcg_ir_store_can_put(s, candidate));
    g_assert_false(tcg_ir_store_put(s, candidate, data));
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_true(d.capacity_observed);
    g_assert_cmpuint(d.records, ==, TCG_IR_CACHE_MAX_RECORDS);
    g_assert_cmpuint(d.bytes, ==, TCG_IR_CACHE_MAX_RECORDS * 105);
    g_assert_cmpuint(d.put_calls, ==, TCG_IR_CACHE_MAX_RECORDS + 1);
    g_assert_cmpuint(d.put_success, ==, TCG_IR_CACHE_MAX_RECORDS);
    g_assert_cmpuint(d.file_fsync_calls, ==, TCG_IR_CACHE_MAX_RECORDS);
    g_assert_cmpuint(d.directory_fsync_calls, ==, TCG_IR_CACHE_MAX_RECORDS);
    g_assert_cmpuint(d.file_fsync_errors, ==, 0);
    g_assert_cmpuint(d.directory_fsync_errors, ==, 0);
    g_assert_false(tcg_ir_store_can_put(s, candidate));
    tcg_ir_store_get_diagnostics(s, &again);
    g_assert_cmpuint(again.first_capacity_us, ==, d.first_capacity_us);
    GBytes *found = tcg_ir_store_lookup(s, first);
    g_assert_nonnull(found);
    g_assert_cmpmem(g_bytes_get_data(found, NULL), 1, "x", 1);
    g_bytes_unref(found);
    tcg_ir_store_close(s);
    /* Persisted saturation must not turn off reading after process restart. */
    s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    tcg_ir_store_enable_diagnostics(s);
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_true(d.capacity_observed);
    g_assert_cmpuint(d.put_calls, ==, 0);
    g_assert_cmpuint(d.records, ==, TCG_IR_CACHE_MAX_RECORDS);
    g_assert_false(tcg_ir_store_can_put(s, candidate));
    found = tcg_ir_store_lookup(s, first);
    g_assert_nonnull(found);
    g_bytes_unref(found);
    tcg_ir_store_close(s);
    g_bytes_unref(data);
    cleanup();
}

static void byte_capacity_keeps_reads(void)
{
    setup();
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    uint8_t candidate[32] = { 0x78 };
    g_autofree uint8_t *payload = g_malloc0(TCG_IR_CACHE_MAX_RECORD);
    size_t remaining = TCG_IR_CACHE_MAX_BYTES;
    TCGIRCacheStoreDiagnostics d;
    tcg_ir_store_enable_diagnostics(s);
    for (uint32_t i = 0; remaining > 104; i++) {
        size_t length = MIN(TCG_IR_CACHE_MAX_RECORD, remaining - 104);
        GBytes *data = g_bytes_new_static(payload, length);
        memcpy(candidate + 1, &i, sizeof(i));
        g_assert_true(tcg_ir_store_can_put(s, candidate));
        g_assert_true(tcg_ir_store_put(s, candidate, data));
        g_bytes_unref(data);
        remaining -= 104 + length;
    }
    g_assert_cmpuint(remaining, ==, 0);
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_true(d.capacity_observed);
    g_assert_cmpuint(d.bytes, ==, TCG_IR_CACHE_MAX_BYTES);
    g_assert_cmpuint(d.file_fsync_calls, ==, d.records);
    g_assert_cmpuint(d.directory_fsync_calls, ==, d.records);
    candidate[0] ^= 1;
    g_assert_false(tcg_ir_store_can_put(s, candidate));
    candidate[0] ^= 1;
    GBytes *found = tcg_ir_store_lookup(s, candidate);
    g_assert_nonnull(found);
    g_bytes_unref(found);
    tcg_ir_store_close(s);
    s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    g_assert_false(tcg_ir_store_can_put(s, candidate));
    found = tcg_ir_store_lookup(s, candidate);
    g_assert_nonnull(found);
    g_bytes_unref(found);
    tcg_ir_store_close(s);
    cleanup();
}

int main(int argc, char **argv)
{
    /* Python driver invokes these modes as different execs, proving the
     * store is actually usable after all producer process memory is gone. */
    if (argc == 3 && !strcmp(argv[1], "--produce")) {
        directory = argv[2]; put_record(); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--consume")) {
        TCGIRCacheStore *s = tcg_ir_store_open(argv[2], build);
        g_assert_nonnull(s);
        GBytes *b = tcg_ir_store_lookup(s, key);
        g_assert_nonnull(b);
        g_assert_cmpuint(g_bytes_get_size(b), ==, 23);
        g_bytes_unref(b); tcg_ir_store_close(s); return 0;
    }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ir-store/roundtrip", roundtrip);
    g_test_add_func("/ir-store/stale-engine", stale_engine);
    g_test_add_func("/ir-store/checksum", checksum_failure);
    g_test_add_func("/ir-store/private-directory", private_directory);
    g_test_add_func("/ir-store/private-record", private_record);
    g_test_add_func("/ir-store/exclusive-owner", exclusive_owner);
    g_test_add_func("/ir-store/interrupted-write", interrupted_write);
    g_test_add_func("/ir-store/truncated", truncated_record);
    g_test_add_func("/ir-store/huge", huge_record);
    g_test_add_func("/ir-store/symlink-record", symlink_record);
    g_test_add_func("/ir-store/hardlink-record", hardlink_record);
    g_test_add_func("/ir-store/bounded-directory", bounded_directory);
    g_test_add_func("/ir-store/count-capacity-keeps-reads", count_capacity_keeps_reads);
    g_test_add_func("/ir-store/byte-capacity-keeps-reads", byte_capacity_keeps_reads);
    return g_test_run();
}
