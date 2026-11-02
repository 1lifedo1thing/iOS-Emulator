@ CPSR write checks for tests/ipod/tcg-psr-test.sh: a bare-metal program run as the bootrom (at 0, SVC mode, MMU off)
@ on an ARMv6 or ARMv7 board whose RAM starts at RAM (-DRAM=). It raises interrupts through VIC0's SOFTINT while they
@ are masked and unmasks them the ways a kernel does, writes the flags the ways a kernel and VFP code do, and changes
@ mode. One character per check on the semihosting console: its letter in lowercase if it passed, uppercase if not;
@ then 'E', and it exits.
        .syntax unified
        .arch armv6k
        .fpu vfpv2
        .arm
        .text
.if RAM == 0x40000000
        .equ VIC, 0xbf200000            @ S5L8930 VIC0
.else
        .equ VIC, 0x38e00000            @ S5L8720 VIC0
.endif
        .equ INTSELECT, 0x0c
        .equ INTENABLE, 0x10
        .equ SOFTINT, 0x18
        .equ SOFTINTCLEAR, 0x1c
        .equ VECTADDR, 0xf00
        .equ REC, RAM + 0x100           @ where the last IRQ or FIQ was taken (its return address)
        .equ CNT, RAM + 0x104           @ how many were taken
        .equ SYS_SP, RAM + 0xb000

_start:
        b       .Lreset
        b       .Lund
        b       .Lsvc
        b       .
        b       .
        b       .
        b       .Lint                   @ IRQ
@ FIQ (and IRQ): acknowledge, clear SOFTINT, record where it was taken
.Lint:
        push    {r0, r1}
        ldr     r1, =VIC
        ldr     r0, [r1, #VECTADDR]
        mov     r0, #1
        str     r0, [r1, #SOFTINTCLEAR]
        str     r0, [r1, #VECTADDR]
        ldr     r1, =REC
        sub     r0, lr, #4
        str     r0, [r1]
        ldr     r0, [r1, #4]
        add     r0, r0, #1
        str     r0, [r1, #4]
        pop     {r0, r1}
        subs    pc, lr, #4
.Lund:
        add     r7, r7, #1
        movs    pc, lr
.Lsvc:
        bx      lr                      @ stay in SVC

        .macro  raise
        mov     r0, #1
        str     r0, [r11, #SOFTINT]
        .endm
@ the interrupt was taken at label \at, before the instruction there ran
        .macro  taken_at at, letter
        ldr     r1, =REC
        ldr     r0, [r1]
        adr     r1, \at
        mov     r2, #\letter
        bl      .Lcheck
        .endm
@ the flags are \nzcv << 28
        .macro  flags nzcv, letter
        mrs     r0, cpsr
        and     r0, r0, #0xf0000000
        mov     r1, #\nzcv << 28
        mov     r2, #\letter
        bl      .Lcheck
        .endm

.Lreset:
        cpsid   aif
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x2000         @ low vectors
        mcr     p15, 0, r0, c1, c0, 0
        cps     #0x12                   @ IRQ
        ldr     sp, =RAM + 0xe000
        cps     #0x11                   @ FIQ
        ldr     sp, =RAM + 0xd000
        cps     #0x1b                   @ Undefined
        ldr     sp, =RAM + 0xc000
        cps     #0x1f                   @ System
        ldr     sp, =SYS_SP
        cps     #0x13                   @ SVC
        ldr     sp, =RAM + 0xf000
        mov     r10, #0
        ldr     r11, =VIC
        mov     r0, #1
        str     r0, [r11, #INTENABLE]

        @ a: a pending IRQ is taken right after cpsie i
        bl      .Lclear
        raise
        nop
        cpsie   i
1:      nop
        cpsid   i
        taken_at 1b, 'a'

        @ b: and right after msr cpsr_c clears I
        bl      .Lclear
        raise
        mrs     r4, cpsr
        bic     r4, r4, #0x80
        msr     cpsr_c, r4
1:      nop
        cpsid   i
        taken_at 1b, 'b'

        @ c, d: and right after msr cpsr_fc (a kernel lock's restore), whose flags land
        bl      .Lclear
        mrs     r4, cpsr
        bic     r4, r4, #0xf0000000
        bic     r4, r4, #0x80
        orr     r4, r4, #0x60000000     @ Z, C
        msr     cpsr_f, #0x90000000     @ N, V
        raise
        msr     cpsr_fc, r4
1:      mrs     r5, cpsr
        cpsid   i
        taken_at 1b, 'c'
        and     r0, r5, #0xf0000000
        mov     r1, #0x60000000
        mov     r2, #'d'
        bl      .Lcheck

        @ e, f: cpsid i inside a block masks an IRQ raised after it until cpsie i, which takes it at once
        bl      .Lclear
        cpsie   i
        nop
        cpsid   i
        raise
        b       2f                      @ a block boundary, where an unmasked IRQ would be taken
2:      ldr     r1, =CNT
        ldr     r5, [r1]
        cpsie   i
1:      nop
        cpsid   i
        mov     r0, r5
        mov     r1, #0
        mov     r2, #'e'
        bl      .Lcheck
        taken_at 1b, 'f'

        @ g: a pending FIQ is taken right after cpsie f
        bl      .Lclear
        mov     r0, #1
        str     r0, [r11, #INTSELECT]
        raise
        nop
        cpsie   f
1:      nop
        cpsid   f
        mov     r0, #0
        str     r0, [r11, #INTSELECT]
        taken_at 1b, 'g'

        @ h, i, j: VMRS APSR_nzcv after VCMP: less, equal, unordered
        mrc     p15, 0, r0, c1, c0, 2
        orr     r0, r0, #0xf00000       @ CP10, CP11
        mcr     p15, 0, r0, c1, c0, 2
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 4   @ ISB
        mov     r0, #0x40000000
        vmsr    fpexc, r0
        ldr     r0, =0x3f800000         @ 1.0
        vmov    s0, r0
        ldr     r0, =0x40000000         @ 2.0
        vmov    s1, r0
        ldr     r0, =0x7fc00000         @ NaN
        vmov    s2, r0
        msr     cpsr_f, #0x70000000
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        flags   0x8, 'h'
        msr     cpsr_f, #0x90000000
        vcmp.f32 s0, s0
        vmrs    APSR_nzcv, fpscr
        flags   0x6, 'i'
        msr     cpsr_f, #0xc0000000
        vcmp.f32 s0, s2
        vmrs    APSR_nzcv, fpscr
        flags   0x3, 'j'

        @ k, l: cps to System sees its sp, cps back to SVC sees SVC's
        mov     r4, sp
        cps     #0x1f
        mov     r5, sp
        cps     #0x13
        mov     r6, sp
        mov     r0, r5
        ldr     r1, =SYS_SP
        mov     r2, #'k'
        bl      .Lcheck
        mov     r0, r6
        mov     r1, r4
        mov     r2, #'l'
        bl      .Lcheck

        @ m: the same through msr cpsr_c
        msr     cpsr_c, #0xdf
        mov     r5, sp
        msr     cpsr_c, #0xd3
        mov     r0, r5
        ldr     r1, =SYS_SP
        mov     r2, #'m'
        bl      .Lcheck

        @ n, o, p: msr cpsr_c to User: a CP15 access there is undefined, svc returns to SVC
        mov     r7, #0
        msr     cpsr_c, #0xd0
        mrs     r6, cpsr
        mrc     p15, 0, r0, c1, c0, 0
        svc     #0
        and     r0, r6, #0x1f
        mov     r1, #0x10
        mov     r2, #'n'
        bl      .Lcheck
        mov     r0, r7
        mov     r1, #1
        mov     r2, #'o'
        bl      .Lcheck
        mrs     r0, cpsr
        and     r0, r0, #0x1f
        mov     r1, #0x13
        mov     r2, #'p'
        bl      .Lcheck

        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #'\n'
        bl      .Lputc
        mov     r0, #0x18                   @ SYS_EXIT
        ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

.Lclear:
        ldr     r1, =REC
        mov     r0, #0
        str     r0, [r1]
        str     r0, [r1, #4]
        bx      lr

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
