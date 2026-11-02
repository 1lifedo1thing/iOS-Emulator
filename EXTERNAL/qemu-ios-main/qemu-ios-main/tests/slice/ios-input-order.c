/* Host input reaches the machine in the order the host sent it, though QEMU runs the bottom halves scheduled before
 * one poll in reverse (aio_bh_enqueue inserts at the head of the list aio_bh_poll walks from the head; the stub below
 * does the same). A tap's press and release sent together used to run release first and leave the touch down.
 *
 * Mutation (named, must fail this test): schedule fn itself in qemu_ios_ui_input_later instead of ios_input_bh.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c range struct ios_input { | static void ios_input_bh(
 * SLICE contrib/ios-app/qemu-ios-ui.c fn ios_input_bh qemu_ios_ui_input_later
 * PKG glib-2.0
 * CFLAGS -Wall -Werror -Wno-unused-function
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
typedef struct AioContext AioContext;
static AioContext *qemu_get_aio_context(void) { return NULL; }
/* Pending bottom halves, run newest first as aio_bh_poll does. */
static struct { void (*fn)(void *); void *opaque; } pending[16];
static int npending;
static void aio_bh_schedule_oneshot(AioContext *ctx, void (*fn)(void *), void *opaque)
{
    pending[npending].fn = fn;
    pending[npending++].opaque = opaque;
}
static void poll(void)
{
    while (npending) {
        npending--;
        pending[npending].fn(pending[npending].opaque);
    }
}
#include "slice.h"

static char got[16];
static void input(void *what) { strcat(got, what); }

int main(void)
{
    qemu_ios_ui_input_later(input, "D");   /* press */
    qemu_ios_ui_input_later(input, "U");   /* release, before the main loop polls */
    poll();
    qemu_ios_ui_input_later(input, "d");
    poll();
    qemu_ios_ui_input_later(input, "u");
    poll();
    printf("%s\n", got);
    assert(!strcmp(got, "DUdu"));
    puts("PASS: host input sent in one burst runs in the order it was sent");
}
