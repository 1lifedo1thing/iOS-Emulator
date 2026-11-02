/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARM_DYNARMIC_PROBE_ABI_H
#define ARM_DYNARMIC_PROBE_ABI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot-only bounded diagnostic. No QEMU helper or live memory callback
 * crosses this ABI. The execution-loop caller still permits exactly ONE
 * instruction per GDB step. A larger capsule prepares for a separately
 * reviewed caller; it does not permit skipping interrupt/debug boundaries. */
#define LT_DYNARMIC_PROBE_MAX_STEPS 8

typedef struct LTDynarmicProbeStep {
    uint32_t pc;
    uint32_t opcode;
    bool has_read;
    uint32_t read_address;
    uint32_t read_value;
    uint32_t expected_regs[16];
    uint32_t expected_cpsr;
} LTDynarmicProbeStep;

typedef struct LTDynarmicProbePlan {
    unsigned count;
    uint32_t initial_regs[16];
    uint32_t initial_cpsr;
    LTDynarmicProbeStep steps[LT_DYNARMIC_PROBE_MAX_STEPS];
} LTDynarmicProbePlan;

bool lt_dynarmic_probe_execute(const LTDynarmicProbePlan *plan,
                              uint32_t regs[16], uint32_t *cpsr);

#ifdef __cplusplus
}
#endif
#endif
