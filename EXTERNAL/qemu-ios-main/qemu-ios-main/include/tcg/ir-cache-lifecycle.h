/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TCG_IR_CACHE_LIFECYCLE_H
#define TCG_IR_CACHE_LIFECYCLE_H

/* ARM embedded engine only: call after qemu_cleanup(), when all vCPU/device
 * activity has stopped, before returning to a host that may use _exit/kill.
 * Emits a final experimental-cache report and releases the disposable cache
 * lease. Does not request or confirm guest shutdown. Idempotent; a disabled or
 * unsupported cache is a no-op. This does not allow a second VM in the process.
 */
void arm_tcg_ir_cache_finish(void);

#endif
