/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIGHTTOUCH_WINDOWS_ENGINE_H
#define LIGHTTOUCH_WINDOWS_ENGINE_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call initialize on the actual process main thread, before starting the main-loop thread.
 * Normal QEMU argv must select -display none and -no-shutdown. The caller owns
 * base/package admission and the same exclusive running.lock as the preview.
 * This module never constructs a boot recipe or removes a dirty marker. */
void lt_windows_engine_initialize(int argc, char **argv);
void lt_windows_engine_start(void);
bool lt_windows_engine_running(void);
/* Thread-safe controls. Shutdown is cooperative: no timeout kills the worker. */
bool lt_windows_engine_pause(bool paused);
bool lt_windows_engine_stop(void);
/* Join only after running returns false. Return 0 requires confirmed PMU
 * shutdown, no latched storage failure and successful engine cleanup/exit. */
int lt_windows_engine_join(void);
#ifdef __cplusplus
}
#endif
#endif
