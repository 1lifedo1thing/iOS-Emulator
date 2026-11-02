/* Actual frame publication/capture and AIO enqueue/dequeue/cancel/delete.
 * No GL or guest model is substituted for an actual guest qualification.
 * SLICE:frame-counter-state contrib/ios-app/qemu-ios-ui.c typedef IosFrameCounters
 * SLICE:frame-counter-state contrib/ios-app/qemu-ios-ui.c fn ios_frame_count
 * SLICE:aio util/async.c range enum { | /* Called concurrently from any thread *
 * SLICE:aio util/async.c fn aio_bh_enqueue aio_bh_dequeue qemu_bh_schedule qemu_bh_cancel qemu_bh_delete
 * SLICE:frame contrib/ios-app/qemu-ios-ui.c fn ios_init_frame_lock ios_capture ios_publish ios_publish_bh ios_cancel_publish ios_gfx_update ios_gfx_switch ios_refresh ios_sequence_reset qemu_ios_ui_vm_stopped
 * VARIANT
 * VARIANT -D__APPLE__
 * PKG glib-2.0
 * CFLAGS -I$ROOT/include -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-comment
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define qemu_build_assert(test) _Static_assert((test), "atomic width")
#include "qemu/rcu_queue.h"
typedef struct QEMUBH QEMUBH;
typedef void QEMUBHFunc(void *);
typedef void MemReentrancyGuard;
typedef QSLIST_HEAD(, QEMUBH) BHList;
typedef struct { BHList bh_list; } AioContext;
static AioContext context;
static bool icount_enabled(void) { return false; }
static void icount_notify_exit(void) { }
static void aio_notify(AioContext *ctx) { }
#include "aio.h"
static QEMUBH *new_bh(QEMUBHFunc *cb) {
    QEMUBH *bh = g_new0(QEMUBH, 1);
    bh->ctx = &context; bh->cb = cb; return bh;
}
static unsigned poll_bh(void) {
    BHList slice = QSLIST_HEAD_INITIALIZER(slice);
    unsigned flags, count = 0;
    QEMUBH *bh;
    QSLIST_MOVE_ATOMIC(&slice, &context.bh_list);
    while ((bh = aio_bh_dequeue(&slice, &flags))) {
        if ((flags & (BH_SCHEDULED | BH_DELETED)) == BH_SCHEDULED) {
            bh->cb(bh->opaque); count++;
        }
        if (flags & BH_DELETED) g_free(bh);
    }
    return count;
}
typedef GMutex QemuMutex;
#define qemu_mutex_init g_mutex_init
#define qemu_mutex_lock g_mutex_lock
#define qemu_mutex_unlock g_mutex_unlock
typedef struct { int w, h, stride; uint8_t *data; } DisplaySurface;
typedef struct { DisplaySurface *surface; } QemuConsole;
typedef struct { QemuConsole *con; } DisplayChangeListener;
static int surface_width(DisplaySurface *s) { return s->w; }
static int surface_height(DisplaySurface *s) { return s->h; }
static int surface_stride(DisplaySurface *s) { return s->stride; }
static void *surface_data(DisplaySurface *s) { return s->data; }
static DisplaySurface *qemu_console_surface(QemuConsole *c) { return c->surface; }
#define QEMU_CLOCK_REALTIME 0
static uint64_t qemu_clock_get_ns(int clock) { static uint64_t t; return ++t; }
static void ios_report_first_lit_frame(const uint8_t *p, int w, int h) { }
#define IOS_FRAME_BUFFERS 3
static struct {
    QemuConsole *con; bool attached, pending; QEMUBH *publish_bh;
    QemuMutex frame_lock; void *buf[3]; size_t buf_size; GSList *retired;
    int published, width, height; uint64_t serial, timestamp_ns;
    void (*cb)(void *); void *cb_opaque;
} ios;
static int ios_vm_alive;
static bool painting;
static unsigned callbacks, releases, cancels, discarded, hw_updates;
static void callback(void *opaque) {
    assert(!painting && g_mutex_trylock(&ios.frame_lock));
    g_mutex_unlock(&ios.frame_lock); callbacks++;
}
static void ios_sequence_cancel_current(void) { cancels++; }
static bool ipod_touch_ap_wake_reset_pending(void) { return false; }
static void portable_release_all(void) { releases++; }
static void portable_release_input(bool preserve) { assert(!preserve); releases++; }
static void ios_input_discard_pending(void) { discarded++; }
static void qemu_ios_ui_cancel_input(void) { }
static void ios_gfx_update(DisplayChangeListener *, int, int, int, int);
static void graphic_hw_update(QemuConsole *con) {
    DisplayChangeListener dcl = {con};
    hw_updates++; ios_gfx_update(&dcl, 0, 0, con->surface->w, con->surface->h);
}
#include "frame-counter-state.h"
static IosFrameCounters ios_frame_counters;
#include "frame.h"
static void paint(DisplayChangeListener *dcl, uint8_t value) {
    DisplaySurface *s = dcl->con->surface;
    painting = true;
    memset(s->data, value, (size_t)s->stride * s->h / 2);
    ios_gfx_update(dcl, 0, 0, s->w, s->h / 2);
    memset(s->data + s->stride * s->h / 2, value, (size_t)s->stride * s->h / 2);
    ios_gfx_update(dcl, 0, s->h / 2, s->w, s->h / 2);
    painting = false;
}
int main(void) {
    uint8_t pixels[32] = {0};
    DisplaySurface first = {2, 4, 8, pixels};
    QemuConsole con = {&first}; DisplayChangeListener dcl = {&con};
    ios.con = &con; ios.attached = true; ios_vm_alive = 1;
    ios.cb = callback;
#ifdef __APPLE__
    paint(&dcl, 7); assert(poll_bh() == 0 && ios.serial == 0);
    ios_refresh(&dcl); assert(ios.serial == 1 && callbacks == 1);
    ios_sequence_reset(NULL); assert(cancels == 1 && releases == 1);
    ios_refresh(&dcl); assert(ios.serial == 1);
    qemu_ios_ui_vm_stopped(); assert(!ios_vm_alive && !ios.attached);
    for (int i = 0; i < 3; i++) g_free(ios.buf[i]);
    puts("PASS: Apple retains refresh-boundary publication and reset path");
    return 0;
#else
    uint8_t rotated[48] = {0};
    DisplaySurface second = {3, 4, 12, rotated};
    ios.publish_bh = new_bh(ios_publish_bh);
    paint(&dcl, 7);
    assert(ios.serial == 0 && callbacks == 0);
    assert(poll_bh() == 1 && ios.serial == 1 && callbacks == 1);
    assert(!memcmp(ios.buf[ios.published], pixels, sizeof pixels));
    paint(&dcl, 7); poll_bh(); assert(ios.serial == 1); /* unchanged pixels */
    paint(&dcl, 9);
    ios_refresh(&dcl); assert(ios.serial == 2); /* fallback wins once */
    assert(poll_bh() == 1 && ios.serial == 2);
    con.surface = &second;
    ios_gfx_switch(&dcl, &second);
    assert(poll_bh() == 0 && ios.serial == 2); /* switch is not paint */
    paint(&dcl, 11); poll_bh();
    assert(ios.serial == 3 && ios.width == 3 && ios.height == 4);
    assert(!memcmp(ios.buf[ios.published], rotated, sizeof rotated));
    paint(&dcl, 12); ios_sequence_reset(NULL);
    assert(cancels == 1 && releases == 1 && !ios.pending);
    assert(poll_bh() == 0 && ios.serial == 3); /* cancelled old frame */
    paint(&dcl, 13); poll_bh(); assert(ios.serial == 4);
    paint(&dcl, 14); qemu_ios_ui_vm_stopped();
    assert(!ios_vm_alive && !ios.attached && !ios.publish_bh && !ios.pending);
    assert(discarded == 1 && poll_bh() == 0 && ios.serial == 4);
    ios_publish_bh(NULL); /* defensive guard even if a stale caller invokes it */
    ios_gfx_update(&dcl, 0, 0, 3, 4);
    unsigned before = hw_updates; ios_refresh(&dcl);
    assert(hw_updates == before && poll_bh() == 0 && ios.serial == 4);
    con.surface = NULL; ios_vm_alive = 1; ios.attached = true;
    ios_publish(&con); assert(ios.serial == 4); /* no surface retains pending */
    ios_publish(NULL); assert(ios.serial == 4);
    for (int i = 0; i < 3; i++) g_free(ios.buf[i]);
    g_slist_free_full(ios.retired, g_free);
    puts("PASS: whole-paint BH coalescing, fallback, resize, reset and stop lifetime");
#endif
}
