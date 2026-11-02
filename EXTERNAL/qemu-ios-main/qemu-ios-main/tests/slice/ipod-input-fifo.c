/* Production input entry points and actual AIO head insertion/dequeue.
 * SLICE:qemu-ios-ui contrib/ios-app/qemu-ios-ui.h file
 * SLICE:aio util/async.c range enum { | /* Called concurrently from any thread *
 * SLICE:aio util/async.c fn aio_bh_enqueue aio_bh_dequeue aio_bh_schedule_oneshot_full
 * SLICE:input contrib/ios-app/qemu-ios-ui.c range static void portable_cancel_bh | struct portable_touch_request {
 * SLICE:input contrib/ios-app/qemu-ios-ui.c fn portable_touch_bh portable_touch_discard qemu_ios_portable_touch qemu_ios_portable_input_status
 * SLICE:fifo contrib/ios-app/qemu-ios-input.c.inc file
 * VARIANT
 * VARIANT -D__APPLE__
 * PKG glib-2.0
 * CFLAGS -I$ROOT/include -Wall -Werror -Wno-unused-function -Wno-comment
 */
#include <glib.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define qemu_build_assert(test) _Static_assert((test), "atomic width")
#include "qemu/rcu_queue.h"
#include "qemu-ios-ui.h"
typedef struct QEMUBH QEMUBH;
typedef void QEMUBHFunc(void *);
typedef void MemReentrancyGuard;
typedef QSLIST_HEAD(, QEMUBH) BHList;
typedef struct AioContext { BHList bh_list; } AioContext;
static AioContext ctx;
static bool icount_enabled(void) { return false; }
static void icount_notify_exit(void) { }
static void aio_notify(AioContext *c) { }
#include "aio.h"
static void aio_bh_schedule_oneshot(AioContext *c, QEMUBHFunc *cb, void *p)
{ aio_bh_schedule_oneshot_full(c, cb, p, "slice"); }
static AioContext *qemu_get_aio_context(void) { return &ctx; }
static unsigned poll(void)
{
    BHList slice = QSLIST_HEAD_INITIALIZER(slice);
    QEMUBH *bh; unsigned flags, count = 0;
    QSLIST_MOVE_ATOMIC(&slice, &ctx.bh_list);
    while ((bh = aio_bh_dequeue(&slice, &flags))) {
        if (flags & BH_SCHEDULED) { bh->cb(bh->opaque); count++; }
        g_free(bh);
    }
    return count;
}
static struct { bool active; uint32_t id; double x,y; } portable_contacts[5];
static uint64_t portable_touch_timestamp, portable_status_id;
static int portable_status, portable_pending;
static struct { void *con; GMutex frame_lock; } ios = {.con=(void*)1};
#define qemu_mutex_lock g_mutex_lock
#define qemu_mutex_unlock g_mutex_unlock
static void ios_init_frame_lock(void) { }
static bool ready = true, running = true, capacity = true;
bool qemu_ios_ui_ready(void) { return ready; }
#define RUN_STATE_SUSPENDED 1
static bool runstate_is_running(void) { return running; }
static bool runstate_check(int state) { return false; }
static void ios_sequence_cancel_current(void) { }
static bool portable_touch_capacity(unsigned b, unsigned e, unsigned m) { return capacity; }
struct portable_touch_request {
    uint64_t id, timestamp; size_t count; QemuIosPortableContact contacts[8];
};
static unsigned emits;
static int phases[512];
static void portable_emit(int slot, int phase, double x, double y)
{ assert(emits < G_N_ELEMENTS(phases)); phases[emits++] = phase; }
static void qemu_input_event_sync_impl(void) { }
void qemu_ios_ui_cancel_input(void)
{
    for (unsigned i=0;i<5;i++) if (portable_contacts[i].active) {
        portable_emit(i,QEMU_IOS_TOUCH_END,0,0); portable_contacts[i].active=false;
    }
    portable_touch_timestamp=0;
}
#include "fifo.h"
#include "input.h"
static int touch(uint64_t id, uint64_t time, uint32_t contact_id, unsigned phase)
{
    QemuIosPortableContact c={sizeof(c),1,contact_id,phase,.3,.5};
    return qemu_ios_portable_touch(id,time,&c,1);
}

#if !defined(__APPLE__)
static void stop_during_drain(void *unused)
{
    ready=false;
    ios_input_discard_pending();
    assert(touch(44,50,1,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_NOT_READY);
}
#endif
static unsigned control_count;
static int controls[256];
static void record_control(void *opaque)
{
    int value=(int)(intptr_t)opaque;
    controls[control_count++]=value;
    if (value==100) assert(ios_input_enqueue(record_control,(void*)200,NULL));
}
int main(void)
{
    assert(touch(1,1,7,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED);
    assert(touch(2,2,7,QEMU_IOS_TOUCH_UPDATE)==QEMU_IOS_QUEUED);
    assert(touch(3,3,7,QEMU_IOS_TOUCH_END)==QEMU_IOS_QUEUED);
    poll();
#if defined(__APPLE__)
    assert(portable_contacts[0].active);
    assert(qemu_ios_portable_input_status(3)==QEMU_IOS_INVALID);
    puts("PASS original Apple one-shot scheduling remains unchanged");
    return 0;
#else
    assert(!portable_contacts[0].active);
    assert(qemu_ios_portable_input_status(3)==QEMU_IOS_OK);
    assert(emits==3 && phases[0]==0 && phases[1]==1 && phases[2]==2);
    puts("PASS burst BEGIN/MOVE/UP preserves applied order and final release");
    /* A cancel barrier must precede reused IDs and a later timestamp clock. */
    assert(touch(4,4,7,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED); poll();
    unsigned before=emits;
    assert(qemu_ios_portable_cancel_input()==QEMU_IOS_QUEUED);
    assert(touch(5,1,7,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED);
    assert(touch(6,2,7,QEMU_IOS_TOUCH_UPDATE)==QEMU_IOS_QUEUED);
    poll();
    assert(qemu_ios_portable_input_status(6)==QEMU_IOS_OK);
    assert(portable_contacts[0].active && portable_contacts[0].id==7);
    assert(emits==before+3 && phases[before]==2 && phases[before+1]==0 && phases[before+2]==1);
    assert(touch(7,3,7,QEMU_IOS_TOUCH_END)==QEMU_IOS_QUEUED); poll();
    assert(!portable_contacts[0].active);
    puts("PASS CANCEL then reused BEGIN/MOVE keeps the release barrier");
    /* Distinct invalid/stale requests retain their errors; no relaxed clocks. */
    assert(touch(8,4,99,QEMU_IOS_TOUCH_UPDATE)==QEMU_IOS_QUEUED); poll();
    assert(qemu_ios_portable_input_status(8)==QEMU_IOS_INVALID);
    assert(touch(9,3,99,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED); poll();
    assert(qemu_ios_portable_input_status(9)==QEMU_IOS_STALE);
    capacity=false;
    assert(touch(10,5,99,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED); poll();
    assert(qemu_ios_portable_input_status(10)==QEMU_IOS_BUSY);
    capacity=true;
    for (unsigned n=0;n<32;n++) assert(touch(11+n,6+n,99,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED);
    assert(touch(43,40,99,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_BUSY);
    assert(portable_status_id==42 && portable_pending==32);
    ready=false; ios_input_discard_pending();
    assert(portable_pending==0 && qemu_ios_portable_input_status(42)==QEMU_IOS_NOT_READY);
    poll(); /* Old scheduled drain owns no freed request. */
    assert(!portable_contacts[0].active);
    puts("PASS errors, pending bound, terminal disposal and latest status conserved");
    ready=true;
    assert(ios_input_enqueue(record_control,(void*)100,NULL));
    assert(ios_input_enqueue(record_control,(void*)101,NULL));
    poll();
    assert(control_count==3 && controls[0]==100 && controls[1]==101 && controls[2]==200);
    control_count=0;
    for (unsigned n=0;n<70;n++) assert(ios_input_enqueue(record_control,(void*)(intptr_t)n,NULL));
    assert(poll()==1 && control_count==64);
    assert(poll()==1 && control_count==70);
    for (unsigned n=0;n<70;n++) assert(controls[n]==(int)n);
    assert(!ios_input_scheduled && g_queue_is_empty(&ios_input_queue));
    puts("PASS control callbacks and reentrant admission stay FIFO across a bounded drain");
    assert(ios_input_enqueue(stop_during_drain,NULL,NULL));
    assert(touch(44,50,1,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED);
    poll();
    assert(portable_pending==0 && !portable_contacts[0].active);
    assert(qemu_ios_portable_input_status(44)==QEMU_IOS_NOT_READY);
    assert(!ios_input_scheduled && g_queue_is_empty(&ios_input_queue));
    ready=true;
    assert(touch(45,51,1,QEMU_IOS_TOUCH_BEGIN)==QEMU_IOS_QUEUED);
    ready=false; poll(); /* Detached requests also check live session state. */
    assert(portable_pending==0 && qemu_ios_portable_input_status(45)==QEMU_IOS_NOT_READY);
    assert(!ios_input_scheduled && !portable_contacts[0].active);
    puts("PASS terminal control closes admission and disposes detached work safely");
    return 0;
#endif
}
