/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARM_TCG_IR_CACHE_H
#define ARM_TCG_IR_CACHE_H

typedef struct ARMTcgIRCacheKey {
    uint8_t bytes[32];
    int max_insns;
    bool eligible;
} ARMTcgIRCacheKey;

bool arm_tcg_ir_cache_load(CPUState *cpu, TranslationBlock *tb, int max_insns,
                           vaddr pc, void *host_pc, ARMTcgIRCacheKey *key);
void arm_tcg_ir_cache_save(CPUState *cpu, TranslationBlock *tb,
                           vaddr pc, void *host_pc,
                           const ARMTcgIRCacheKey *key);
#endif
