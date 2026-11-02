/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TCG_IR_CACHE_STORE_H
#define TCG_IR_CACHE_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

/* These are hard limits, including stale records from a different build. */
#define TCG_IR_CACHE_MAX_BYTES (32 * 1024 * 1024)
#define TCG_IR_CACHE_MAX_RECORDS 4096
#define TCG_IR_CACHE_MAX_RECORD (128 * 1024)

typedef struct TCGIRCacheStore TCGIRCacheStore;

/* Fixed-size opt-in observations only; never influence cache decisions.
 * Durations are monotonic wall microseconds, not guest CPU utilization.
 * Caller serializes these operations, as for put()/lookup().
 */
typedef struct TCGIRCacheStoreDiagnostics {
    bool enabled, capacity_observed;
    uint64_t records, bytes, elapsed_us, first_capacity_us;
    uint64_t put_calls, put_success;
    uint64_t file_fsync_calls, file_fsync_errors, file_fsync_us;
    uint64_t directory_fsync_calls, directory_fsync_errors, directory_fsync_us;
} TCGIRCacheStoreDiagnostics;

/* Existing absolute owner-only directory; never follows the final symlink.
 * A second simultaneous owner gets NULL. Errors always mean normal translation.
 * The caller serializes access, and owns the returned store and lookup GBytes.
 */
TCGIRCacheStore *tcg_ir_store_open(const char *directory,
                                 const uint8_t build[32]);
GBytes *tcg_ir_store_lookup(TCGIRCacheStore *store, const uint8_t key[32]);
/* Cheap preflight before building a record. False when the directory is full,
 * already contains this key, or cannot fit even the smallest nonempty record.
 * Does not disable reads or relax validation. Caller serializes, as for put().
 */
bool tcg_ir_store_can_put(TCGIRCacheStore *store, const uint8_t key[32]);
bool tcg_ir_store_put(TCGIRCacheStore *store, const uint8_t key[32], GBytes *data);
void tcg_ir_store_enable_diagnostics(TCGIRCacheStore *store);
void tcg_ir_store_get_diagnostics(TCGIRCacheStore *store,
                                TCGIRCacheStoreDiagnostics *out);
void tcg_ir_store_close(TCGIRCacheStore *store);

#endif
