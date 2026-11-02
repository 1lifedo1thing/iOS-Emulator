/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_IOS_FRAME_PATH_H
#define QEMU_IOS_FRAME_PATH_H

/* Optional embedding diagnostic; frame boundaries and selected focused paths. */
enum IOSFramePathEvent {
    IOS_FP_PRESENT_BEGIN = 1,
    IOS_FP_PRESENT_END,
    IOS_FP_LCD_FLIP,
    IOS_FP_VSYNC_BEGIN,
    IOS_FP_VSYNC_END,
    IOS_FP_SCAN_BEGIN,
    IOS_FP_SCAN_END,
    IOS_FP_CAPTURE_BEGIN,
    IOS_FP_CAPTURE_SAME,
    IOS_FP_PUBLISH,
    IOS_FP_CONTEXT_SWITCH,
    IOS_FP_TTBR0,
    IOS_FP_VFP_SAMPLE,
    IOS_FP_VFP_RESULT,
    IOS_FP_ALIGNMENT,
    IOS_FP_ALIGNMENT_BEGIN,
    IOS_FP_ALIGNMENT_RETURN,
    IOS_FP_ALIGNMENT_ABANDON,
    IOS_FP_ALIGNMENT_ARGS,
    IOS_FP_ALIGNMENT_CALLER,
    IOS_FP_GUEST_PC,
    IOS_FP_GUEST_REGS,
    IOS_FP_GUEST_STACK,
    IOS_FP_NATIVE_WAIT,
    IOS_FP_AHB_STAGE_BEGIN,
    IOS_FP_AHB_STAGE_END,
    IOS_FP_FMSS_BEGIN,
    IOS_FP_FMSS_END,
    IOS_FP_SURFACE_MAP_BEGIN,
    IOS_FP_SURFACE_MAP_END,
    IOS_FP_BT_COMMAND,
    IOS_FP_BT_QUEUE,
    IOS_FP_BT_FLUSH,
    IOS_FP_BT_STATE,
};

/*
 * Weak so a host may leave them out (Android defines them in its own library). On Windows a weak definition
 * is not enough: PE weak externals give every referencing object its own null default and ld may pick one
 * over a weak definition, so calls jump to address zero. A unit that defines hooks says which before
 * including this header, and its definitions are strong there; references elsewhere stay weak.
 */
#if defined(_WIN32) && defined(IOS_FRAME_PATH_DEFINES_EVENT)
#define IOS_FRAME_PATH_EVENT_HOOK
#else
#define IOS_FRAME_PATH_EVENT_HOOK __attribute__((weak))
#endif
#if defined(_WIN32) && defined(IOS_FRAME_PATH_DEFINES_WAIT)
#define IOS_FRAME_PATH_WAIT_HOOK
#else
#define IOS_FRAME_PATH_WAIT_HOOK __attribute__((weak))
#endif

void qemu_ios_native_wait_init(void) IOS_FRAME_PATH_WAIT_HOOK;
void qemu_ios_native_wait_finish(void) IOS_FRAME_PATH_WAIT_HOOK;
void qemu_ios_bql_spin_finish(void) IOS_FRAME_PATH_WAIT_HOOK;

void qemu_ios_frame_path_event(unsigned event, uint64_t a, uint64_t b,
                               uint64_t c) IOS_FRAME_PATH_EVENT_HOOK;

static inline void ios_frame_path(unsigned event, uint64_t a, uint64_t b,
                                   uint64_t c)
{
    if (qemu_ios_frame_path_event) {
        qemu_ios_frame_path_event(event, a, b, c);
    }
}

#endif
