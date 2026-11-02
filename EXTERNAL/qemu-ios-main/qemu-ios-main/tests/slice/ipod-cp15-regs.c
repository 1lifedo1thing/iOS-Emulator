/* The iPod touch 2G's extra cp15 registers keep no state in the CPU: an
 * ARMCPRegInfo's fieldoffset is an offset into CPUARMState, so one computed
 * from the machine's state (as these were) has every raw write and reset land
 * on whatever CPU field sits there (the victim TLB). Every entry is either a
 * NOP or reached only through its read and write functions, with no raw
 * access.
 *
 * SLICE:regs hw/arm/ipod_touch_2g.c range #define IT2G_CP | const int S5L8900_GPIO_IRQS
 * SLICE:regs hw/arm/ipod_touch_2g.c range static const ARMCPRegInfo it2g_cp_reginfo_tcg[] = { | static void ipod_touch_cpu_setup
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct CPUARMState CPUARMState;
typedef struct ARMCPRegInfo ARMCPRegInfo;
struct ARMCPRegInfo {
    const char *name;
    int cp, opc0, opc1, crn, crm, opc2, access, state, type;
    uint64_t resetvalue;
    ptrdiff_t fieldoffset;
    uint64_t (*readfn)(CPUARMState *, const ARMCPRegInfo *);
    void (*writefn)(CPUARMState *, const ARMCPRegInfo *, uint64_t);
};
enum { PL0_RW = 1, PL1_RW = 2, ARM_CP_STATE_AA32 = 1 };
enum { ARM_CP_NOP = 1, ARM_CP_OVERRIDE = 2, ARM_CP_IO = 4, ARM_CP_NO_RAW = 8, ARM_CP_RAISES_EXC = 16 };
/* what the old definitions referred to */
#define IT2G_CPREG_VAR_NAME(name) cpreg_##name
typedef struct { char other[4096]; uint64_t cpreg_REG0, cpreg_REG1, cpreg_QEMU_CALL; } IPodTouchMachineState;
typedef struct { char parent[512]; int env; } ARMCPU;
static uint64_t qemu_call_status(CPUARMState *e, const ARMCPRegInfo *r) { (void)e; (void)r; return 0; }
static void qemu_call(CPUARMState *e, const ARMCPRegInfo *r, uint64_t v) { (void)e; (void)r; (void)v; }
static uint64_t host_gmt_seconds(CPUARMState *e, const ARMCPRegInfo *r) { (void)e; (void)r; return 0; }
static void host_gmt_write(CPUARMState *e, const ARMCPRegInfo *r, uint64_t v) { (void)e; (void)r; (void)v; }
#include "regs.h"

int main(void)
{
    size_t n = sizeof(it2g_cp_reginfo_tcg) / sizeof(it2g_cp_reginfo_tcg[0]);

    assert(n == 6);
    for (size_t i = 0; i < n; i++) {
        const ARMCPRegInfo *r = &it2g_cp_reginfo_tcg[i];
        assert(r->fieldoffset == 0);
        assert((r->type & ARM_CP_NOP) || (r->readfn && r->writefn));
    }
    puts("PASS: the n72's cp15 additions keep no state at a CPU field offset");
    return 0;
}
