/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No QEMU headers/state/longjmp paths cross this C++ boundary. */
#include <algorithm>
#include <exception>
#include <optional>
#include <dynarmic/interface/A32/a32.h>
#include "dynarmic-probe-abi.h"

namespace {
using namespace Dynarmic;
using namespace Dynarmic::A32;

struct Callbacks final : UserCallbacks {
    Jit *jit = nullptr;
    const LTDynarmicProbeStep *step = nullptr;
    bool unsupported = false;
    unsigned reads = 0;
    uint64_t ticks = 1;

    void reject() {
        unsupported = true;
        if (jit) { jit->HaltExecution(HaltReason::MemoryAbort); }
    }
    std::optional<uint32_t> MemoryReadCode(VAddr address) override {
        return step && address == step->pc ? std::optional<uint32_t>(step->opcode)
                                   : std::nullopt;
    }
    uint8_t MemoryRead8(VAddr) override { reject(); return 0; }
    uint16_t MemoryRead16(VAddr) override { reject(); return 0; }
    uint32_t MemoryRead32(VAddr address) override {
        if (!step || !step->has_read || address != step->read_address || reads++) {
            reject();
            return 0;
        }
        return step->read_value;
    }
    uint64_t MemoryRead64(VAddr) override { reject(); return 0; }
    void MemoryWrite8(VAddr, uint8_t) override { reject(); }
    void MemoryWrite16(VAddr, uint16_t) override { reject(); }
    void MemoryWrite32(VAddr, uint32_t) override { reject(); }
    void MemoryWrite64(VAddr, uint64_t) override { reject(); }
    void InterpreterFallback(VAddr, size_t) override { reject(); }
    void CallSVC(uint32_t) override { reject(); }
    void ExceptionRaised(VAddr, Exception) override { reject(); }
    void AddTicks(uint64_t count) override {
        ticks = count < ticks ? ticks - count : 0;
    }
    uint64_t GetTicksRemaining() override { return ticks; }
};
}

extern "C" bool lt_dynarmic_probe_execute(const LTDynarmicProbePlan *plan,
                                         uint32_t regs[16], uint32_t *cpsr)
{
    if (!plan || !plan->count || plan->count > LT_DYNARMIC_PROBE_MAX_STEPS) {
        return false;
    }
    try {
        Callbacks callbacks;
        UserConfig config{};
        config.callbacks = &callbacks;
        config.arch_version = ArchVersion::v6K;
        config.code_cache_size = 8 * 1024 * 1024;
        config.check_halt_on_memory_access = true;
        /* A fresh JIT per capsule avoids persistent invalidation claims.
         * The live QEMU caller currently always supplies count=1. */
        Jit jit(config);
        callbacks.jit = &jit;
        std::copy_n(plan->initial_regs, 16, jit.Regs().begin());
        jit.SetCpsr(plan->initial_cpsr);
        for (unsigned index = 0; index < plan->count; index++) {
            const LTDynarmicProbeStep &step = plan->steps[index];
            if (jit.Regs()[15] != step.pc || (step.pc & 3)) {
                return false;
            }
            callbacks.step = &step;
            callbacks.reads = 0;
            callbacks.ticks = 1;
            const auto halt = jit.Step();
            if (callbacks.unsupported || halt != HaltReason::Step ||
                callbacks.reads != (step.has_read ? 1u : 0u) ||
                jit.Cpsr() != step.expected_cpsr ||
                !std::equal(step.expected_regs, step.expected_regs + 16,
                            jit.Regs().begin())) {
                return false;
            }
        }
        /* Rejection never modifies outputs or live QEMU state. */
        std::copy_n(jit.Regs().begin(), 16, regs);
        *cpsr = jit.Cpsr();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
