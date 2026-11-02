#ifndef HW_ARM_IT_IBOOT_H
#define HW_ARM_IT_IBOOT_H

/*
 * Board-agnostic pattern matches over a decrypted iBoot image loaded at
 * `base` (the S5L8720 iPod loads it at 0x0ff00000 and jumps in directly;
 * the iPad runs its real iBoot from its own image and needs none of this).
 * Every finder is derived from what the image contains -- a string, the one
 * literal that references it, the Thumb `ldr rN, [pc, #imm]` that loads the
 * literal -- never from a build's offsets. Each returns 0 for absent,
 * ambiguous or out-of-window matches, and tests/slice/ipod-iboot-literals.c
 * pins the answer for every iPod touch 2G iBoot (2.1.1 .. 4.2.1).
 */

#include <stddef.h>
#include <stdint.h>

/*
 * The boot security epoch this iBoot demands of SYSIC POWER_ID[31:24] in
 * miu_init ("Epoch Mismatch" otherwise): the floor its epoch helper applies
 * to the chip ID fuse field. The LLB latches that byte on a real boot; a
 * board that jumps straight into iBoot has to synthesize it. 0 = not found.
 */
uint32_t it_iboot_find_epoch(const uint8_t *image, size_t size);

/*
 * The same demand in an armv7 iBoot (the A4's, iBoot-817 .. 1219): miu_init
 * does `bl epoch; cmp.w r0, rN, lsr #24` against PMGR POWER_ID, and epoch()
 * is `bl fuse_field; cmp r0, #N; it <cc>; mov r0, #M`, the fused field
 * floored at the build's epoch. Returns what epoch() computes for `fuse`
 * (the CHIPID field the caller models), which is what LLB latches into
 * POWER_ID[31:24] on hardware; 0 = not found, or found more than once.
 */
uint32_t it_iboot_find_miu_epoch(const uint8_t *image, size_t size,
                                 uint32_t fuse);

/*
 * Where iBoot's `snprintf(gBootArgs.commandLine, 0x100, "%s ", args)` keeps
 * the format's address: the guest address of that literal-pool slot, with
 * gBootArgs.commandLine through *command_line. Release iBoots format an empty
 * string there, so pointing the slot at commandLine, filled with the wanted
 * arguments ('%' doubled), makes iBoot hand them to the kernel itself. Found
 * in every iPod touch 2G iBoot, 2.1.1 .. 4.2.1; 0 when absent, ambiguous, or
 * when another load shares the slot.
 */
uint32_t it_iboot_find_boot_args_format(const uint8_t *image, size_t size,
                                        uint32_t base, uint32_t *command_line);

#ifndef IT_IBOOT_HOST_TEST
#include "exec/hwaddr.h"
#include "system/memory.h"

/* it_iboot_find_epoch over the image staged at [base, base + image_size). */
uint32_t it_iboot_epoch(AddressSpace *as, uint32_t base, size_t image_size);
#endif

#endif
