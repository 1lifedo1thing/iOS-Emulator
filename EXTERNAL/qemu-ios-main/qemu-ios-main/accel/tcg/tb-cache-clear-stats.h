/* Experimental diagnostics only; never used to decide cache validity.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TCG_TB_CACHE_CLEAR_STATS_H
#define TCG_TB_CACHE_CLEAR_STATS_H

#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
#include "qemu/atomic.h"

/* Separate atomic observations, not a transaction or unique emptied slots. */
#define TB_CLEAR_FIELDS(X) \
    X(full_total) X(full_tlb) X(tlb_clean) X(tlb_partial) X(tlb_all) \
    X(full_range) X(full_tb) X(full_pcrel) X(full_reset) \
    X(page_total) X(page_previous) X(page_current) X(page_range) \
    X(tb_check) X(tb_match)

typedef enum TBClearEvent {
#define TB_CLEAR_ENUM(name) TB_CLEAR_##name,
    TB_CLEAR_FIELDS(TB_CLEAR_ENUM)
#undef TB_CLEAR_ENUM
    TB_CLEAR_COUNT
} TBClearEvent;

typedef struct TBClearStats {
    uint64_t events[TB_CLEAR_COUNT];
} TBClearStats;

static inline void tb_cache_clear_add(TBClearStats *s, TBClearEvent event)
{
    /* Invalidation can visit a different vCPU's cache. Never use plain ++. */
    qatomic_inc(&s->events[event]);
}

static inline void tb_cache_clear_dump(FILE *stream, double elapsed,
                                     unsigned cpu, unsigned entries,
                                     const TBClearStats *s)
{
    /* One stdio record; its individual atomic loads are not a coherent epoch. */
#define TB_CLEAR_FORMAT(name) " " #name "=%" PRIu64
#define TB_CLEAR_VALUE(name) , qatomic_read(&s->events[TB_CLEAR_##name])
    fprintf(stream, "[tb-clear] %.1fs cpu=%u entries=%u"
            TB_CLEAR_FIELDS(TB_CLEAR_FORMAT) "\n", elapsed, cpu, entries
            TB_CLEAR_FIELDS(TB_CLEAR_VALUE));
#undef TB_CLEAR_FORMAT
#undef TB_CLEAR_VALUE
}

/* CPUJumpCache is complete where this macro is expanded. No runtime flag is
 * read by foreign writers: compilation enables counters; IT_VCPU_ACCT only
 * enables the existing owner-thread dump. Lifetime follows the original cache.
 */
#define TB_CACHE_CLEAR_NOTE(cache, event) do { \
    CPUJumpCache *clear_stats_jc = (cache); \
    if (clear_stats_jc) { \
        tb_cache_clear_add(&clear_stats_jc->clear_stats, (event)); \
    } \
} while (0)
#else
/* Do not even evaluate the arguments in a production build. */
#define TB_CACHE_CLEAR_NOTE(cache, event) do { } while (0)
#endif
#endif
