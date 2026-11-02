/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_IPOD_SHUTDOWN_DIAGNOSTICS_H
#define HW_ARM_IPOD_SHUTDOWN_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#define IPOD_SHUTDOWN_DIAG_LIMIT 8

typedef enum IPodShutdownCancelReason {
    IPOD_CANCEL_NONE,
    IPOD_CANCEL_NOT_FOUND,
    IPOD_CANCEL_UNDISPATCHED,
    IPOD_CANCEL_CLAIM_RESET,
    IPOD_CANCEL_COMPLETED,
} IPodShutdownCancelReason;

typedef struct IPodShutdownCancelTrace {
    uint64_t serial;
    int64_t monotonic_ms;
    unsigned reason;
} IPodShutdownCancelTrace;

static inline bool ipod_shutdown_diag_flag(const char *value)
{
    return value && value[0] == '1' && value[1] == '\0';
}

static inline bool ipod_shutdown_diag_in_budget(unsigned serial)
{
    return serial > 0 && serial <= IPOD_SHUTDOWN_DIAG_LIMIT;
}

static inline void ipod_shutdown_cancel_record(IPodShutdownCancelTrace *trace,
                                               unsigned reason, int64_t now)
{
    trace->serial++;
    trace->reason = reason;
    trace->monotonic_ms = now;
}

#endif
