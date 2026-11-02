/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"
#include "tb-cache-clear-stats.h"

/*
 * 16K entries (128 page groups of 128 under softmmu) per address space. An
 * iOS game's indirect-branch working set (objc_msgSend, UIKit, the game)
 * thrashes upstream's 4K: Flight Control on a Poco F3 ran 19.7 fps at 4K,
 * 20.8 at 8K and 21.6 at 16K; 64K (16 MiB across the per-ASID caches) was
 * slower than 4K.
 */
#ifndef TB_JMP_CACHE_BITS
#define TB_JMP_CACHE_BITS 14
#endif
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCacheBank CPUJumpCacheBank;

typedef struct CPUJumpCache {
    struct rcu_head rcu;
#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
    TBClearStats clear_stats;
#endif
    CPUJumpCacheBank *bank;
    /* The bank epoch this cache was last emptied in; stale if behind. */
    uint64_t epoch;
    /* cpu->neg.ibtc_ctx while current, and the bank ibtc_inval it saw. */
    uint32_t ibtc_id;
    uint32_t ibtc_inval;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/*
 * One jump cache per guest address space (e.g. an ARM ASID), so that a
 * context switch, which must flush the softmmu TLB, can switch caches
 * instead of emptying the only one. A guest switching processes hundreds
 * of times a second otherwise sends every indirect branch after each
 * switch to the QHT; for an iOS 3 guest that was ~80% of all jump-cache
 * clears and ~15-20% of host CPU time.
 *
 * Entries map a virtual PC to a TB, so a cache is valid only for its own
 * address space, and only until a mapping in it changes. Every operation
 * that may change mappings still clears: a page flush clears that page in
 * every cache, and a full flush (TLBI ALL/ASID, tb_flush, reset, CF_PCREL
 * invalidation) bumps @epoch, empties the current cache now and the
 * others when they are next switched to. Like the TLB, this relies on the
 * guest invalidating an ASID before reusing it.
 *
 * cpu->tb_jmp_cache always points at the current member. Only the owning
 * vCPU switches; other threads use the pointer only to reach the bank.
 */
#define TB_JMP_CACHE_CONTEXTS 16

struct CPUJumpCacheBank {
    struct rcu_head rcu;
    uint64_t epoch;                         /* atomic */
    uint32_t ibtc_inval;                    /* atomic; bumped by other threads */
    uint64_t clock;                         /* owner vCPU only */
    uint64_t key[TB_JMP_CACHE_CONTEXTS];    /* owner vCPU only */
    uint64_t used[TB_JMP_CACHE_CONTEXTS];   /* owner vCPU only; 0: unkeyed */
    CPUJumpCache cache[TB_JMP_CACHE_CONTEXTS];
};

/*
 * Give the current cache @jc of @cpu a new indirect-branch context id, so
 * that no site cache filled under the old one matches. Owning vCPU, or with
 * it stopped.
 */
static inline void tb_jmp_cache_new_ibtc_id(CPUState *cpu, CPUJumpCache *jc)
{
    static uint32_t next;
    uint32_t id;

    do {
        id = qatomic_fetch_inc(&next) + 1;
    } while (id == 0);
    jc->ibtc_id = id;
    jc->ibtc_inval = qatomic_read(&jc->bank->ibtc_inval);
    qatomic_set(&cpu->neg.ibtc_ctx, id);
}

static inline void tb_jmp_cache_empty(CPUJumpCache *jc)
{
    for (int i = 0; i < TB_JMP_CACHE_SIZE; i++) {
        qatomic_set(&jc->array[i].tb, NULL);
    }
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
