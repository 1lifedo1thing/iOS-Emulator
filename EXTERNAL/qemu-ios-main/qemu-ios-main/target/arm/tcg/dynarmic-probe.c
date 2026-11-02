/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Asset-free diagnostic only. This is not an alternative iOS CPU backend. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/lt-dynarmic-probe.h"
#include "hw/core/boards.h"
#include "system/memory.h"
#include "dynarmic-probe-abi.h"

/* All real QEMU translation and physical-region checks run in C BEFORE the
 * C++ JIT exists. No debug-memory reads, MMIO reads, TCG return addresses or
 * longjmp-capable helper callbacks enter the JIT. */
static bool probe_ram_word(CPUState *cs, uint32_t address,
                           MMUAccessType access, uint32_t *value)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUTLBEntryFull translation;
    AddressSpace *as;
    MemoryRegion *region;
    hwaddr offset, length = 4;

    if ((address & 3) ||
        !arm_cpu_tlb_fill_align(cs, &translation, address, access,
                               arm_to_core_mmu_idx(arm_mmu_idx(&cpu->env)),
                               MO_32, 4, true, 0)) {
        return false;
    }
    as = arm_addressspace(cs, translation.attrs);
    region = address_space_translate(as, translation.phys_addr, &offset,
                                     &length, false, translation.attrs);
    if (length < 4 || !memory_region_is_ram(region) ||
        memory_region_is_ram_device(region)) {
        return false;
    }
    *value = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(region) + offset);
    return true;
}

/* Prepare a deliberately narrow integer/read instruction. No architectural
 * state changes here: a failure hands the original state to ordinary TCG,
 * which performs genuine precise fault or unsupported-instruction delivery. */
static bool prepare_step(CPUState *cs, const uint32_t regs[16], uint32_t cpsr,
                         LTDynarmicProbeStep *step)
{
    uint32_t opcode, result, lhs, immediate;
    unsigned rd, rn;

    memset(step, 0, sizeof(*step));
    step->pc = regs[15];
    if (!probe_ram_word(cs, step->pc, MMU_INST_FETCH, &opcode)) {
        return false;
    }
    step->opcode = opcode;
    memcpy(step->expected_regs, regs, sizeof(step->expected_regs));
    step->expected_cpsr = cpsr;
    step->expected_regs[15] += 4;
    rd = extract32(opcode, 12, 4);
    rn = extract32(opcode, 16, 4);
    immediate = opcode & 255;

    /* AL, no rotation, no flags; only ordinary r0-r12 operands. */
    if ((opcode & 0xffff0f00) == 0xe3a00000 && rd < 13) {
        step->expected_regs[rd] = immediate;
        return true;
    }
    if (rd < 13 && rn < 13) {
        lhs = regs[rn];
        switch (opcode & 0xfff00f00) {
        case 0xe2800000: result = lhs + immediate; break;
        case 0xe2400000: result = lhs - immediate; break;
        case 0xe2000000: result = lhs & immediate; break;
        case 0xe2200000: result = lhs ^ immediate; break;
        case 0xe3800000: result = lhs | immediate; break;
        default: goto not_alu;
        }
        step->expected_regs[rd] = result;
        return true;
    }
not_alu:
    /* CMP Rn,#imm8: retain every status bit except actual NZCV. */
    if ((opcode & 0xfff0ff00) == 0xe3500000 && rn < 13) {
        lhs = regs[rn];
        result = lhs - immediate;
        step->expected_cpsr = (cpsr & ~CPSR_NZCV) |
            (result & CPSR_N) | (result ? 0 : CPSR_Z) |
            (lhs >= immediate ? CPSR_C : 0) |
            (((lhs ^ immediate) & (lhs ^ result) & CPSR_N) ? CPSR_V : 0);
        return true;
    }
    /* Only B/B{EQ,NE}; no BL, PC reads or indirect/interworking branches. */
    if ((opcode & 0xff000000) == 0xea000000 ||
        (opcode & 0xff000000) == 0x0a000000 ||
        (opcode & 0xff000000) == 0x1a000000) {
        bool taken = (opcode >> 28) == 14 ||
                     ((opcode >> 28) == 0 && (cpsr & CPSR_Z)) ||
                     ((opcode >> 28) == 1 && !(cpsr & CPSR_Z));
        if (taken) {
            /* Unsigned arithmetic models A32 PC wrap without C signed UB. */
            uint32_t displacement = (opcode & 0x00ffffff) << 2;
            if (displacement & 0x02000000) {
                displacement |= 0xfc000000;
            }
            step->expected_regs[15] = regs[15] + 8 + displacement;
        }
        return true;
    }
    /* AL LDR Rd,[Rn,#imm12], positive/preindexed/no-writeback/word. */
    if ((opcode & 0xfff00000) == 0xe5900000 && rd < 13 && rn < 13) {
        step->has_read = true;
        step->read_address = regs[rn] + (opcode & 0xfff);
        if (!probe_ram_word(cs, step->read_address, MMU_DATA_LOAD,
                            &step->read_value)) {
            return false;
        }
        step->expected_regs[rd] = step->read_value;
        return true;
    }
    return false;
}

/* Bounded snapshot-only block preparation. The actual execution-loop entry
 * below requests ONE step. Increasing that budget requires a separately
 * reviewed non-single-step caller and interrupt/lifecycle acceptance. */
static bool prepare_plan(CPUState *cs, const uint32_t regs[16], uint32_t cpsr,
                         unsigned budget, LTDynarmicProbePlan *plan)
{
    uint32_t next_regs[16];
    uint32_t next_cpsr = cpsr;

    if (!budget || budget > LT_DYNARMIC_PROBE_MAX_STEPS) {
        return false;
    }
    memset(plan, 0, sizeof(*plan));
    memcpy(plan->initial_regs, regs, sizeof(plan->initial_regs));
    memcpy(next_regs, regs, sizeof(next_regs));
    plan->initial_cpsr = cpsr;
    while (plan->count < budget) {
        LTDynarmicProbeStep *step = &plan->steps[plan->count];

        /* Finish the safe prefix before a fault/unsupported operation.
         * Normal TCG then sees the exact next PC and register state. */
        if (qatomic_load_acquire(&cs->exit_request) ||
            !prepare_step(cs, next_regs, next_cpsr, step)) {
            break;
        }
        memcpy(next_regs, step->expected_regs, sizeof(next_regs));
        next_cpsr = step->expected_cpsr;
        plan->count++;
    }
    return plan->count != 0;
}

bool lt_dynarmic_probe_try(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    LTDynarmicProbePlan plan;
    uint32_t regs[16], cpsr;
    static unsigned executions;
    const char *enabled = g_getenv("LT_DYNARMIC_PROBE");

    /* Real boards, privileged/Thumb/big-endian execution and all debugger
     * break/watchpoint instrumentation remain ordinary TCG. The generic
     * hook additionally requires single-step/single-thread/no-icount/replay. */
    if (!enabled || strcmp(enabled, "1") || executions >= 8 ||
        !object_dynamic_cast(OBJECT(qdev_get_machine()), MACHINE_TYPE_NAME("none")) ||
        cpu->midr != 0x410fb767 || env->aarch64 || env->thumb ||
        (env->cp15.sctlr_el[1] & SCTLR_B) || (env->regs[15] & 3) ||
        !QTAILQ_EMPTY(&cs->breakpoints) || !QTAILQ_EMPTY(&cs->watchpoints)) {
        return false;
    }
    cpsr = cpsr_read(env);
    if ((cpsr & CPSR_M) != ARM_CPU_MODE_USR ||
        (cpsr & (CPSR_E | CPSR_IT_0_1 | CPSR_IT_2_7))) {
        return false;
    }
    memcpy(regs, env->regs, sizeof(regs));
    if (!prepare_plan(cs, regs, cpsr, 1, &plan) ||
        qatomic_load_acquire(&cs->exit_request) ||
        !lt_dynarmic_probe_execute(&plan, regs, &cpsr)) {
        return false;
    }
    memcpy(env->regs, regs, sizeof(regs));
    cpsr_write(env, cpsr, CPSR_NZCV | CPSR_Q | CPSR_GE, CPSRWriteRaw);
    executions++;
    fprintf(stderr, "LT_DYNARMIC_PROBE executed=%u pc=0x%08x opcode=0x%08x r0=%u\n",
            executions, plan.steps[0].pc, plan.steps[0].opcode, regs[0]);
    return true;
}
