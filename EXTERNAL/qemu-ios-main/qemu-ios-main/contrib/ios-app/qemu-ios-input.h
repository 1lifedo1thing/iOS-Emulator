/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Internal shared-engine scheduling; not part of the frontend ABI. */
#ifndef QEMU_IOS_INPUT_H
#define QEMU_IOS_INPUT_H

#include "qemu/aio.h"

bool ios_input_enqueue(QEMUBHFunc *callback, void *opaque,
                       GDestroyNotify discard);

#endif
