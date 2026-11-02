/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic source-only diagnostic checks. The clock and fsync below are
 * intercepted solely in this harness. Ordinary store tests use real fsync.
 */
#include "qemu/osdep.h"
#include <glib/gstdio.h>

static int64_t clock_us;
static unsigned clock_calls, sync_calls, fail_sync;

static int64_t diagnostic_test_clock(void)
{
    clock_calls++;
    /* A clock implementation must not change fsync's observable errno. */
    errno = ENOTTY;
    return clock_us;
}

static int diagnostic_test_fsync(int fd)
{
    sync_calls++;
    clock_us += 37;
    if (sync_calls == fail_sync) {
        errno = EIO;
        return -1;
    }
    errno = EAGAIN;
    return 0;
}

#define fsync diagnostic_test_fsync
#define g_get_monotonic_time diagnostic_test_clock
#include "../../tcg/ir-cache-store.c"
#undef g_get_monotonic_time
#undef fsync

static uint8_t build[32] = { 0x71 }, key[32] = { 0x19 };
static char *directory;

static TCGIRCacheStore *setup(void)
{
    clock_us = 100;
    clock_calls = sync_calls = fail_sync = 0;
    directory = g_dir_make_tmp("lighttouch-ir-diag-XXXXXX", NULL);
    g_assert_nonnull(directory);
    TCGIRCacheStore *s = tcg_ir_store_open(directory, build);
    g_assert_nonnull(s);
    return s;
}

static void cleanup(TCGIRCacheStore *s)
{
    tcg_ir_store_close(s);
    GDir *dir = g_dir_open(directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        g_autofree char *p = g_build_filename(directory, name, NULL);
        g_assert_cmpint(unlink(p), ==, 0);
    }
    g_dir_close(dir);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_clear_pointer(&directory, g_free);
}

static bool put(TCGIRCacheStore *s)
{
    GBytes *b = g_bytes_new_static("abc", 3);
    bool ok = tcg_ir_store_put(s, key, b);
    g_bytes_unref(b);
    return ok;
}

static void disabled_has_no_clocks(void)
{
    TCGIRCacheStore *s = setup();
    TCGIRCacheStoreDiagnostics d, zero = { 0 };
    g_assert_true(put(s));
    memset(&d, 0xff, sizeof(d));
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_cmpmem(&d, sizeof(d), &zero, sizeof(zero));
    g_assert_cmpuint(clock_calls, ==, 0);
    g_assert_cmpuint(sync_calls, ==, 2);
    cleanup(s);
}

static void successful_publication(void)
{
    TCGIRCacheStore *s = setup();
    TCGIRCacheStoreDiagnostics d;
    tcg_ir_store_enable_diagnostics(s);
    unsigned enabled_clock_calls = clock_calls;
    tcg_ir_store_enable_diagnostics(s); /* Does not reset time or counters. */
    g_assert_cmpuint(clock_calls, ==, enabled_clock_calls);
    g_assert_true(put(s));
    g_assert_false(put(s)); /* Duplicate has no additional sync. */
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_true(d.enabled);
    g_assert_false(d.capacity_observed);
    g_assert_cmpuint(d.records, ==, 1);
    g_assert_cmpuint(d.bytes, ==, 104 + 3);
    g_assert_cmpuint(d.elapsed_us, ==, 74);
    g_assert_cmpuint(d.put_calls, ==, 2);
    g_assert_cmpuint(d.put_success, ==, 1);
    g_assert_cmpuint(d.file_fsync_calls, ==, 1);
    g_assert_cmpuint(d.file_fsync_errors, ==, 0);
    g_assert_cmpuint(d.file_fsync_us, ==, 37);
    g_assert_cmpuint(d.directory_fsync_calls, ==, 1);
    g_assert_cmpuint(d.directory_fsync_errors, ==, 0);
    g_assert_cmpuint(d.directory_fsync_us, ==, 37);
    g_assert_cmpuint(sync_calls, ==, 2);
    cleanup(s);
}

static void file_sync_failure(void)
{
    TCGIRCacheStore *s = setup();
    TCGIRCacheStoreDiagnostics d;
    tcg_ir_store_enable_diagnostics(s);
    fail_sync = 1;
    g_assert_false(put(s));
    g_assert_null(tcg_ir_store_lookup(s, key));
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_cmpuint(d.put_calls, ==, 1);
    g_assert_cmpuint(d.put_success, ==, 0);
    g_assert_cmpuint(d.records, ==, 0);
    g_assert_cmpuint(d.file_fsync_calls, ==, 1);
    g_assert_cmpuint(d.file_fsync_errors, ==, 1);
    g_assert_cmpuint(d.file_fsync_us, ==, 37);
    g_assert_cmpuint(d.directory_fsync_calls, ==, 0);
    cleanup(s);
}

static void directory_sync_failure(void)
{
    TCGIRCacheStore *s = setup();
    TCGIRCacheStoreDiagnostics d;
    tcg_ir_store_enable_diagnostics(s);
    fail_sync = 2;
    /* Existing semantics: directory sync failure leaves a complete published
     * disposable entry usable; diagnostics cannot turn that into a miss. */
    g_assert_true(put(s));
    GBytes *b = tcg_ir_store_lookup(s, key);
    g_assert_nonnull(b);
    g_assert_cmpmem(g_bytes_get_data(b, NULL), 3, "abc", 3);
    g_bytes_unref(b);
    tcg_ir_store_get_diagnostics(s, &d);
    g_assert_cmpuint(d.put_success, ==, 1);
    g_assert_cmpuint(d.directory_fsync_calls, ==, 1);
    g_assert_cmpuint(d.directory_fsync_errors, ==, 1);
    g_assert_cmpuint(d.directory_fsync_us, ==, 37);
    cleanup(s);
}

static void errno_transparency(void)
{
    TCGIRCacheStore *s = setup();
    tcg_ir_store_enable_diagnostics(s);
    fail_sync = 1;
    g_assert_cmpint(observed_fsync(s, -1, false), ==, -1);
    g_assert_cmpint(errno, ==, EIO);
    g_assert_cmpint(observed_fsync(s, -1, true), ==, 0);
    g_assert_cmpint(errno, ==, EAGAIN);
    cleanup(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ir-diagnostics/disabled-no-clocks", disabled_has_no_clocks);
    g_test_add_func("/ir-diagnostics/success", successful_publication);
    g_test_add_func("/ir-diagnostics/file-failure", file_sync_failure);
    g_test_add_func("/ir-diagnostics/directory-failure", directory_sync_failure);
    g_test_add_func("/ir-diagnostics/errno", errno_transparency);
    return g_test_run();
}
