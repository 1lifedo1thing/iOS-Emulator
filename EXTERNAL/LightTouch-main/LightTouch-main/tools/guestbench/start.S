/* ARM-state process entry for LC_UNIXTHREAD (iOS 3.1.3 has no LC_MAIN): argc, argv
 * from the initial stack, as Linux/tests/guest-install-probe/start.S.
 * SPDX-License-Identifier: GPL-2.0-or-later */
.syntax unified
.arm
.globl _start
.align 2
_start:
    ldr r0, [sp]
    add r1, sp, #4
    bic sp, sp, #7
    bl _bench_main
    bl __exit
1:  b 1b
