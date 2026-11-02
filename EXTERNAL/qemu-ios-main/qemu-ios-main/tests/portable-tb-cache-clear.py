#!/usr/bin/env python3
"""Actual invalidation functions with stubbed MMU surroundings, not a guest test.

--prepare-only writes exact slices and input hashes without invoking a compiler.
Each later compiler/run command has a 30-second timeout; an external operator
must supply the aggregate resource bound. No engine build/cache is touched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, position = 1, opening + 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[start:position] + '\n'


FUNCTIONS = {
    'accel/tcg/tb-hash.h': ['static inline unsigned int tb_jmp_cache_hash_page(',
                           'static inline unsigned int tb_jmp_cache_hash_func('],
    'accel/tcg/translate-all.c': ['void tcg_flush_jmp_cache('],
    'accel/tcg/cputlb.c': ['static void tb_jmp_cache_clear_page(',
                         'static void tlb_flush_by_mmuidx_async_work(',
                         'static void tlb_flush_page_by_mmuidx_async_0(',
                         'static void tlb_flush_range_by_mmuidx_async_0('],
    'accel/tcg/tb-maint.c': ['void tb_flush__exclusive_or_serial(',
                           'static void tb_jmp_cache_inval_tb('],
    'accel/tcg/tcg-accel-ops.c': ['static void tcg_cpu_reset_hold('],
}

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "qemu/compiler.h"
_Noreturn void QEMU_ERROR("code path is reachable") qemu_build_not_reached_always(void);
/* BUILD_ASSERT_FROM_OSDEP */
#include "qemu/atomic.h"
typedef uint64_t vaddr;
typedef struct TranslationBlock { vaddr pc; unsigned cflags; } TranslationBlock;
#include "accel/tcg/tb-jmp-cache.h"
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1ULL << TARGET_PAGE_BITS)
#define TB_JMP_PAGE_BITS (TB_JMP_CACHE_BITS / 2)
#define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_PAGE_MASK (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE)
#define NB_MMU_MODES 4
#define ALL_MMUIDX_BITS 15
typedef unsigned MMUIdxMap;
typedef union { unsigned host_int; } run_on_cpu_data;
typedef struct {
    vaddr addr, len; MMUIdxMap idxmap; unsigned bits;
} TLBFlushRangeData;
typedef struct CPUState {
    CPUJumpCache *tb_jmp_cache;
    struct { struct { struct {
        int lock; MMUIdxMap dirty;
        unsigned full_flush_count, part_flush_count, elide_flush_count;
    } c; } tlb; } neg;
} CPUState;
static CPUState cpus[2], *current_cpu;
#define CPU_FOREACH(cpu) for ((cpu)=cpus; (cpu)<cpus+2; ++(cpu))
#define tlb_debug(...) ((void)0)
static void assert_cpu_is_self(CPUState *cpu) { assert(cpu == current_cpu); }
static void qemu_spin_lock(int *p) { assert(!*p); *p=1; }
static void qemu_spin_unlock(int *p) { assert(*p); *p=0; }
static int64_t get_clock_realtime(void) { return 123; }
static unsigned ctz32(unsigned v) { return __builtin_ctz(v); }
static unsigned ctpop16(unsigned v) { return __builtin_popcount(v); }
static void tlb_flush_one_mmuidx_locked(CPUState *cpu, int idx, int64_t now) {
    assert(cpu->neg.tlb.c.lock && idx < NB_MMU_MODES && now == 123);
}
static void tlb_flush_page_locked(CPUState *cpu, int idx, vaddr addr) {
    (void)addr; assert(cpu->neg.tlb.c.lock && idx < NB_MMU_MODES);
}
static void tlb_flush_range_locked(CPUState *cpu, int idx, vaddr addr,
                                   vaddr len, unsigned bits) {
    (void)addr; (void)len; (void)bits;
    assert(cpu->neg.tlb.c.lock && idx < NB_MMU_MODES);
}
#define CF_PCREL 1
static unsigned tb_cflags(TranslationBlock *tb) { return tb->cflags; }
static bool tcg_enabled(void) { return true; }
static bool runstate_is_running(void) { return false; }
static bool cpu_in_serial_context(CPUState *cpu) { return cpu == current_cpu; }
static void trace_tb_flush(void) { }
static struct { int htable, io_insn_lock, io_insn_limits;
                unsigned tb_flush_count; } tb_ctx;
#define CODE_GEN_HTABLE_SIZE 1024
static void qht_reset_size(int *p, unsigned size) {
    assert(p == &tb_ctx.htable && size == 1024);
}
static void qemu_mutex_lock(int *p) { qemu_spin_lock(p); }
static void qemu_mutex_unlock(int *p) { qemu_spin_unlock(p); }
static void g_hash_table_remove_all(int p) { (void)p; }
static void tb_remove_all(void) { }
static void tcg_region_reset_all(void) { }
static void qemu_plugin_flush_cb(void) { }
static void tlb_flush(CPUState *cpu);
'''

TEST = r'''
static void tlb_flush(CPUState *cpu) {
    tlb_flush_by_mmuidx_async_work(cpu, (run_on_cpu_data){.host_int=15});
}
static TranslationBlock a = {.pc=0x40000}, b = {.pc=0x40000};
#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
#define COUNT(cpu, name) qatomic_read(&(cpu)->tb_jmp_cache->clear_stats.events[TB_CLEAR_##name])
#define EXPECT(cpu, name, value) assert(COUNT(cpu, name) == (value))
#else
#define EXPECT(cpu, name, value) ((void)0)
#endif
static void fill(CPUState *cpu) {
    for (unsigned i=0; i<TB_JMP_CACHE_SIZE; ++i) {
        qatomic_set(&cpu->tb_jmp_cache->array[i].tb, &a);
    }
}
static void all_clear(CPUState *cpu) {
    for (unsigned i=0; i<TB_JMP_CACHE_SIZE; ++i) {
        assert(qatomic_read(&cpu->tb_jmp_cache->array[i].tb) == NULL);
    }
}
static void check_page_groups(CPUState *cpu, vaddr addr, unsigned pages) {
    bool empty[TB_JMP_CACHE_SIZE] = {false};
    for (unsigned n=0; n<pages; ++n) {
        unsigned start=tb_jmp_cache_hash_page(addr + n*TARGET_PAGE_SIZE);
        for (unsigned j=0; j<TB_JMP_PAGE_SIZE; ++j) { empty[start+j]=true; }
    }
    for (unsigned i=0; i<TB_JMP_CACHE_SIZE; ++i) {
        assert(qatomic_read(&cpu->tb_jmp_cache->array[i].tb) == (empty[i] ? NULL : &a));
    }
}
static void *foreign_flush(void *opaque) {
    for (unsigned i=0; i<100; ++i) { tcg_flush_jmp_cache(opaque); }
    return NULL;
}
int main(void) {
    /* Existing early-initialization NULL-cache behavior remains safe. */
    CPUState early = {0}; tcg_flush_jmp_cache(&early);
    tb_jmp_cache_clear_page(&early, 0);
    int evaluated=0;
#ifndef QEMU_EXPERIMENTAL_TB_CACHE_STATS
    TB_CACHE_CLEAR_NOTE((++evaluated, &early), undefined_event);
    struct LegacyCache { struct rcu_head rcu;
        struct { TranslationBlock *tb; vaddr pc; } array[TB_JMP_CACHE_SIZE]; };
    _Static_assert(sizeof(CPUJumpCache) == sizeof(struct LegacyCache), "default layout changed");
    _Static_assert(offsetof(CPUJumpCache, array) == offsetof(struct LegacyCache, array), "offset changed");
#endif
    assert(evaluated == 0);
    for (unsigned i=0; i<2; ++i) {
        cpus[i].tb_jmp_cache=calloc(1, sizeof(CPUJumpCache));
        assert(cpus[i].tb_jmp_cache);
    }
    CPUState *cpu=current_cpu=&cpus[0];
    fill(cpu);
    tlb_flush_page_by_mmuidx_async_0(cpu, 0x40000, 3);
    check_page_groups(cpu, 0x40000-TARGET_PAGE_SIZE, 2);
    EXPECT(cpu, page_total, 2); EXPECT(cpu, page_previous, 1); EXPECT(cpu, page_current, 1);
    fill(cpu);
    tlb_flush_range_by_mmuidx_async_0(cpu, (TLBFlushRangeData){0x80000, 2*TARGET_PAGE_SIZE, 3, 32});
    check_page_groups(cpu, 0x80000-TARGET_PAGE_SIZE, 3);
    EXPECT(cpu, page_total, 5); EXPECT(cpu, page_previous, 2); EXPECT(cpu, page_range, 2);
    fill(cpu);
    tlb_flush_range_by_mmuidx_async_0(cpu, (TLBFlushRangeData){0, TARGET_PAGE_SIZE*TB_JMP_CACHE_SIZE, 3, 32});
    all_clear(cpu); EXPECT(cpu, full_range, 1); EXPECT(cpu, full_total, 1);
    /* An already-clean MMU request STILL clears the jumpcache in production. */
    fill(cpu); tlb_flush_by_mmuidx_async_work(cpu, (run_on_cpu_data){.host_int=15});
    all_clear(cpu); EXPECT(cpu, tlb_clean, 1);
    cpu->neg.tlb.c.dirty=3;
    tlb_flush_by_mmuidx_async_work(cpu, (run_on_cpu_data){.host_int=1});
    assert(cpu->neg.tlb.c.dirty == 2); EXPECT(cpu, tlb_partial, 1);
    cpu->neg.tlb.c.dirty=15;
    tlb_flush_by_mmuidx_async_work(cpu, (run_on_cpu_data){.host_int=15});
    EXPECT(cpu, tlb_all, 1); EXPECT(cpu, full_tlb, 3); EXPECT(cpu, full_total, 4);
    fill(cpu); fill(&cpus[1]);
    unsigned h=tb_jmp_cache_hash_func(a.pc);
    qatomic_set(&cpus[1].tb_jmp_cache->array[h].tb, &b);
    tb_jmp_cache_inval_tb(&a);
    assert(qatomic_read(&cpu->tb_jmp_cache->array[h].tb) == NULL);
    assert(qatomic_read(&cpus[1].tb_jmp_cache->array[h].tb) == &b);
    EXPECT(cpu, tb_check, 1); EXPECT(cpu, tb_match, 1);
    EXPECT(&cpus[1], tb_check, 1); EXPECT(&cpus[1], tb_match, 0);
    a.cflags=CF_PCREL; tb_jmp_cache_inval_tb(&a); a.cflags=0;
    all_clear(cpu); all_clear(&cpus[1]);
    EXPECT(cpu, full_pcrel, 1); EXPECT(&cpus[1], full_pcrel, 1);
    fill(cpu); fill(&cpus[1]); tb_flush__exclusive_or_serial();
    all_clear(cpu); all_clear(&cpus[1]); EXPECT(cpu, full_tb, 1);
    fill(cpu); tcg_cpu_reset_hold(cpu); all_clear(cpu);
    EXPECT(cpu, full_reset, 1); EXPECT(cpu, full_total, 8); EXPECT(cpu, full_tlb, 4);
    /* Atomically-counted remote invalidators do not lose increments. */
    pthread_t threads[4];
    for (unsigned i=0; i<4; ++i) { assert(!pthread_create(&threads[i], NULL, foreign_flush, cpu)); }
    for (unsigned i=0; i<4; ++i) { assert(!pthread_join(threads[i], NULL)); }
    all_clear(cpu); EXPECT(cpu, full_total, 408);
#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
    FILE *stream=tmpfile(); assert(stream);
    tb_cache_clear_dump(stream, 12.3, 7, TB_JMP_CACHE_SIZE, &cpu->tb_jmp_cache->clear_stats);
    rewind(stream); char line[1024]; assert(fgets(line, sizeof(line), stream));
    assert(strstr(line, "[tb-clear] 12.3s cpu=7 entries=4096 "));
    assert(strstr(line, " full_total=408 "));
    assert(strstr(line, " tb_check=1 tb_match=1\n"));
    assert(fgetc(stream) == EOF); fclose(stream);
#endif
    for (unsigned i=0; i<2; ++i) { free(cpus[i].tb_jmp_cache); }
    puts("production clear paths and counters PASS");
    return 0;
}
'''


def prepare(output):
    output.mkdir(parents=True, exist_ok=False)
    for path, body in {'qemu/rcu.h': 'struct rcu_head { void *fixture[2]; };\n',
                       'exec/cpu-common.h': '/* Types supplied by the fixture. */\n'}.items():
        target = output / 'stubs' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(body)
    slices = ''.join(function((ROOT / path).read_text(), signature)
                     for path, signatures in FUNCTIONS.items() for signature in signatures)
    # Atomic width assertions normally arrive through osdep.h. Extract its
    # exact optimized definitions rather than weakening/stubbing the check.
    osdep = (ROOT / 'include/qemu/osdep.h').read_text().splitlines()
    assertions = '\n'.join(next(line for line in osdep if line.startswith(prefix))
                           for prefix in ['#define qemu_build_not_reached()  qemu_build_not_reached_always()',
                                          '#define qemu_build_assert(test)'])
    source = PRELUDE.replace('/* BUILD_ASSERT_FROM_OSDEP */', assertions) + slices + TEST
    (output / 'clear.c').write_text(source)
    # Negative controls must fail real production state/counter assertions.
    old = '    TB_CACHE_CLEAR_NOTE(jc, TB_CLEAR_full_total);\n'
    assert source.count(old) == 1
    (output / 'negative-counter.c').write_text(source.replace(old, ''))
    old = '    tb_jmp_cache_clear_page(cpu, addr - TARGET_PAGE_SIZE);\n'
    assert source.count(old) == 1
    (output / 'negative-previous-page.c').write_text(source.replace(old, ''))
    inputs = list(FUNCTIONS) + ['accel/tcg/tb-jmp-cache.h',
                               'accel/tcg/tb-cache-clear-stats.h',
                               'include/qemu/atomic.h', 'include/qemu/compiler.h',
                               'include/qemu/osdep.h',
                               str(Path(__file__).relative_to(ROOT))]
    receipt = {'schema': 1, 'scope': 'production functions; stubbed external MMU/TLB work',
               'inputs': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in inputs},
               'generated': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in output.glob('*.c')}, 'compiled': False}
    (output / 'prepared.json').write_text(json.dumps(receipt, indent=2) + '\n')


def run(output, cc):
    # Expected-failure assertion children must not leave core files.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    stages = []
    for name, macro, sanitize in [('default', False, False), ('stats', True, False),
                                  ('stats-sanitize', True, True),
                                  ('default-sanitize', False, True),
                                  ('negative-counter', True, False),
                                  ('negative-previous-page', True, False)]:
        source = name if name.startswith('negative') else 'clear'
        command = [cc, '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                   '-pthread', '-I' + str(output / 'stubs'), '-I' + str(ROOT),
                   '-I' + str(ROOT / 'include'), str(output / (source + '.c')),
                   '-o', str(output / name)]
        if macro:
            command.append('-DQEMU_EXPERIMENTAL_TB_CACHE_STATS=1')
        if sanitize:
            command.extend(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])
        stages.append({'command': command, 'exit': None})
        (output / 'results.json').write_text(json.dumps(stages, indent=2) + '\n')
        built = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (output / (name + '.compile.stderr')).write_text(built.stderr)
        stages[-1]['exit'] = built.returncode
        (output / 'results.json').write_text(json.dumps(stages, indent=2) + '\n')
        assert built.returncode == 0, built.stderr
        stages.append({'command': [str(output / name)], 'exit': None})
        (output / 'results.json').write_text(json.dumps(stages, indent=2) + '\n')
        tested = subprocess.run([str(output / name)], capture_output=True, text=True, timeout=30)
        (output / (name + '.stdout')).write_text(tested.stdout)
        (output / (name + '.stderr')).write_text(tested.stderr)
        stages[-1]['exit'] = tested.returncode
        (output / 'results.json').write_text(json.dumps(stages, indent=2) + '\n')
        expected = 'full_total' if name == 'negative-counter' else 'empty[i]'
        if name.startswith('negative'):
            assert tested.returncode < 0 and expected in tested.stderr, tested.stderr
        else:
            assert tested.returncode == 0, tested.stderr
    (output / 'results.json').write_text(json.dumps(stages, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    prepare(args.output.resolve())
    if not args.prepare_only:
        run(args.output.resolve(), args.cc)
    print(args.output.resolve())
