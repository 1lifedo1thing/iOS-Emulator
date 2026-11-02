/* Separate SensorManager event streams may carry equal timestamps.
 * SLICE:qemu-ios-ui contrib/ios-app/qemu-ios-ui.h file
 * SLICE contrib/ios-app/qemu-ios-ui.c fn portable_motion_bh
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "qemu-ios-ui.h"
typedef int LIS302DLState;
static LIS302DLState accel;
static uint64_t portable_motion_timestamp[2];
static int portable_pending;
static struct {int frame_lock;} ios;
static uint32_t fields;
static bool running=true;
static bool runstate_is_running(void){return running;}
static LIS302DLState *portable_accel(void){return &accel;}
static void lis302dl_set_host_motion(LIS302DLState *a,uint32_t f,const double x[3],const double w[3]){fields|=f;}
static void qemu_mutex_lock(int *p){}
static void qemu_mutex_unlock(int *p){}
#define g_free free
#include "slice.h"
static void submit(uint64_t t,uint32_t f){QemuIosPortableMotion *m=calloc(1,sizeof(*m));m->timestamp_ns=t;m->fields=f;portable_pending++;portable_motion_bh(m);assert(!portable_pending);}
int main(void){
 submit(10,1);assert(fields==1);submit(10,2);assert(fields==3);
 fields=0;submit(9,3);assert(!fields);submit(11,3);assert(fields==3);
 fields=0;running=false;submit(12,3);assert(!fields);
 puts("PASS: production dispatcher admits equal-time independent acceleration/gyro, coalesced fields, rejects stale and paused samples");
}
