/*
 * Waiting for a GL present whose copy into guest memory is still running (gles-host's
 * present worker), before a device reads the frame: the scaler and the display.
 */
#ifndef HW_ARM_GLES_PRESENT_H
#define HW_ARM_GLES_PRESENT_H

/* Set by the GL host once it copies presents on a worker; NULL until then. */
extern void (*gles_present_wait_hook)(void);

static inline void gles_present_wait(void)
{
    void (*hook)(void) = qatomic_read(&gles_present_wait_hook);

    if (hook) {
        hook();
    }
}

#endif
