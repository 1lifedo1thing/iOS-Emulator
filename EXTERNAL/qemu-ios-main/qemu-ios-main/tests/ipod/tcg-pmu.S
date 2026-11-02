@ PMU checks for tests/ipod/tcg-pmu-test.sh: a bare-metal program run as the Cortex-A8 bootrom (at 0, SVC mode, MMU
@ off; RAM at RAM, -DRAM=). The cycle counter runs through a thousand exceptions taken and returned (svc, movs pc, lr:
@ the PMU's EL-change hooks), and an overflow reaches PMOVSR while the program only polls it (the counter's timer).
@ One character per check on the semihosting console: its letter in lowercase if it passed, uppercase if not; then
@ 'E', and it exits.
        .syntax unified
        .arch armv7-a
        .arm
        .text

_start:
        b       .Lreset
        b       .
        b       .Lsvc
        b       .
        b       .
        b       .
        b       .
        b       .
.Lsvc:
        add     r9, r9, #1
        movs    pc, lr

.Lreset:
        cpsid   aif
        ldr     sp, =RAM + 0xf000
        mov     r10, #0
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x2000         @ low vectors
        mcr     p15, 0, r0, c1, c0, 0
        isb
        mov     r0, #5                  @ PMCR: E, C (the cycle counter reset)
        mcr     p15, 0, r0, c9, c12, 0
        mov     r0, #0x80000000         @ PMCNTENSET: the cycle counter
        mcr     p15, 0, r0, c9, c12, 1
        isb

        @ a: the count grows across 1000 svc/movs pc, lr round trips, and every one was taken
        mov     r9, #0
        mrc     p15, 0, r4, c9, c13, 0
        ldr     r5, =1000
1:      svc     #0
        subs    r5, r5, #1
        bne     1b
        mrc     p15, 0, r6, c9, c13, 0
        ldr     r1, =1000
        cmp     r6, r4
        movls   r1, #0                  @ didn't grow: fail
        mov     r0, r9
        mov     r2, #'a'
        bl      .Lcheck

        @ b: PMCCNTR set 4096 cycles short of wrapping; PMOVSR's bit 31 sets with nothing but reads of PMOVSR
        mvn     r0, #0
        mcr     p15, 0, r0, c9, c12, 3  @ PMOVSR: clear
        ldr     r0, =0xfffff000
        mcr     p15, 0, r0, c9, c13, 0
        ldr     r5, =20000000
2:      mrc     p15, 0, r0, c9, c12, 3
        tst     r0, #0x80000000
        bne     3f
        subs    r5, r5, #1
        bne     2b
3:      and     r0, r0, #0x80000000
        mov     r1, #0x80000000
        mov     r2, #'b'
        bl      .Lcheck

        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #0x18               @ SYS_EXIT
        ldr     r1, =0x20026            @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

@ r0 = got, r1 = expected, r2 = the check's letter
.Lcheck:
        cmp     r0, r1
        subne   r2, r2, #0x20
        addne   r10, r10, #1
        mov     r0, r2
@ r0 = character, to the semihosting console (SYS_WRITEC)
.Lputc:
        push    {r0, r1, r2, r3, r12, lr}
        mov     r1, sp
        mov     r0, #3
        svc     0x123456
        pop     {r0, r1, r2, r3, r12, pc}

        .ltorg
