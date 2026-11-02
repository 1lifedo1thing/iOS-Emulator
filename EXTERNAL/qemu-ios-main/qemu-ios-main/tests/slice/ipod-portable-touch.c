/* Exercise production stable-ID admission and batching, not a copied mapper.
 * SLICE:qemu-ios-ui contrib/ios-app/qemu-ios-ui.h file
 * SLICE contrib/ios-app/qemu-ios-ui.c fn portable_touch_bh portable_touch_discard qemu_ios_portable_touch qemu_ios_portable_input_status
 * CFLAGS -Wall -Werror -Wno-unused-function -Wno-comment
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "qemu-ios-ui.h"
#define g_new0(T,n) ((T*)calloc((n),sizeof(T)))
#define g_free free
static struct { bool active; uint32_t id; double x,y; } portable_contacts[5];
static uint64_t portable_touch_timestamp, portable_status_id;
static int portable_status, portable_pending;
static struct {void *con; int frame_lock;} ios = {.con=(void*)1};
static bool running=true,ready=true;
#define RUN_STATE_SUSPENDED 1
static bool runstate_is_running(void){return running;}
static bool runstate_check(int state){return false;}
bool qemu_ios_ui_ready(void){return ready;}
static void qemu_mutex_lock(int *lock){}
static void qemu_mutex_unlock(int *lock){}
static void ios_init_frame_lock(void){}
static void ios_sequence_cancel_current(void){}
static void *qemu_get_aio_context(void){return NULL;}
struct portable_touch_request {
 uint64_t id,timestamp; size_t count;
 QemuIosPortableContact contacts[8];
};
static bool capacity=true;
static bool portable_touch_capacity(unsigned begins,unsigned ends,unsigned moves){return capacity;}
static unsigned emits,syncs;
static struct {int slot,phase;double x,y;} events[100];
static void portable_emit(int slot,int phase,double x,double y) {
 events[emits].slot=slot;events[emits].phase=phase;events[emits].x=x;events[emits].y=y;emits++;
}
static void qemu_input_event_sync_impl(void){syncs++;}
static bool ios_input_enqueue(void(*cb)(void*),void*arg,void(*discard)(void*)){cb(arg);return true;}
#include "slice.h"
static QemuIosPortableContact contact(uint32_t id,int phase,double x,double y) {
 return (QemuIosPortableContact){.size=sizeof(QemuIosPortableContact),.version=1,.contact_id=id,.phase=phase,.x=x,.y=y};
}
int main(void) {
 QemuIosPortableContact c[8];
 c[0]=contact(0xffffffffu,0,.2,.3);c[1]=contact(700,0,.8,.3);
 assert(qemu_ios_portable_touch(1,1,c,2)==QEMU_IOS_QUEUED);
 assert(qemu_ios_portable_input_status(1)==QEMU_IOS_OK && emits==2 && syncs==1);
 assert(events[0].slot==0 && events[1].slot==1);
 c[0]=contact(700,1,.1,.3);c[1]=contact(0xffffffffu,1,.9,.3);
 qemu_ios_portable_touch(2,2,c,2);
 assert(events[2].slot==1 && events[3].slot==0 && syncs==2);
 c[0]=contact(700,2,.1,.3);qemu_ios_portable_touch(3,3,c,1);
 assert(!portable_contacts[1].active && portable_contacts[0].active);
 c[0]=contact(9,0,.1,.3);c[1]=contact(88,1,.2,.3);
 qemu_ios_portable_touch(4,4,c,2);
 assert(qemu_ios_portable_input_status(4)==QEMU_IOS_INVALID && emits==5 && syncs==3);
 assert(!portable_contacts[1].active); /* malformed second event aborts first */
 c[0]=contact(9,0,.1,.3);c[1]=c[0];
 assert(qemu_ios_portable_touch(5,5,c,2)==QEMU_IOS_INVALID && emits==5);
 for(int i=0;i<5;i++) c[i]=contact(i,0,.2,.2);
 qemu_ios_portable_touch(6,6,c,5);
 assert(qemu_ios_portable_input_status(6)==QEMU_IOS_UNSUPPORTED && emits==5);
 c[0]=contact(0xffffffffu,1,.3,.3);qemu_ios_portable_touch(7,2,c,1);
 assert(qemu_ios_portable_input_status(7)==QEMU_IOS_STALE && emits==5);
 running=false;c[0]=contact(0xffffffffu,1,.3,.3);qemu_ios_portable_touch(8,8,c,1);
 assert(qemu_ios_portable_input_status(8)==QEMU_IOS_NOT_READY && emits==5);
 c[0]=contact(0xffffffffu,3,.3,.3);qemu_ios_portable_touch(9,9,c,1);
 assert(qemu_ios_portable_input_status(9)==QEMU_IOS_OK && !portable_contacts[0].active && emits==6 && syncs==4);
 running=true;capacity=false;c[0]=contact(123,0,.4,.4);
 qemu_ios_portable_touch(10,10,c,1);
 assert(qemu_ios_portable_input_status(10)==QEMU_IOS_BUSY && emits==6 && syncs==4);
 assert(!portable_contacts[0].active && portable_touch_timestamp==9);
 capacity=true;
 ready=false;assert(qemu_ios_portable_touch(11,11,c,1)==QEMU_IOS_NOT_READY);
 puts("PASS: production stable 32-bit IDs, crossing, independent lift, atomic batch rejection, board limits, stale timestamps, paused cancellation");
}
