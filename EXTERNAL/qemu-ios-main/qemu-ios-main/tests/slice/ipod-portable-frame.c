/* Frame ownership: copy must occur before the publisher lock is released.
 * SLICE:frame-counter-state contrib/ios-app/qemu-ios-ui.c typedef IosFrameCounters
 * SLICE:frame-counter-state contrib/ios-app/qemu-ios-ui.c fn ios_frame_count
 * SLICE:qemu-ios-ui contrib/ios-app/qemu-ios-ui.h file
 * SLICE contrib/ios-app/qemu-ios-ui.c fn qemu_ios_portable_frame_copy
 * CFLAGS -Wall -Werror -Wno-unused-function -Wno-comment
 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "qemu-ios-ui.h"
static struct {int frame_lock,published,width,height;uint64_t serial,timestamp_ns;void *buf[3];} ios;
static int locked;
static void ios_init_frame_lock(void){}
static void qemu_mutex_lock(int *lock){assert(!locked);locked=1;}
/* Immediately overwrite publisher memory at unlock: a borrowed pointer copy
 * performed later would capture 0xee instead of the requested frame. */
static void qemu_mutex_unlock(int *lock){assert(locked);locked=0;if(ios.published>=0)memset(ios.buf[ios.published],0xee,8);}
#include "frame-counter-state.h"
static IosFrameCounters ios_frame_counters;
#include "slice.h"
int main(void) {
 uint8_t publisher[8]={0,1,2,3,4,5,6,7},copy[8]={0};
 ios.published=-1;QemuIosPortableFrame f={.size=sizeof(f),.version=1};
 assert(qemu_ios_portable_frame_copy(copy,sizeof(copy),0,&f)==QEMU_IOS_NO_FRAME);
 ios.published=0;ios.width=2;ios.height=1;ios.serial=7;ios.timestamp_ns=99;ios.buf[0]=publisher;
 assert(qemu_ios_portable_frame_copy(copy,4,0,&f)==QEMU_IOS_BUFFER_SMALL);
 assert(f.required_bytes==8&&f.width==2&&f.height==1&&f.stride==8&&f.serial==7&&f.timestamp_ns==99&&copy[0]==0);
 for(int i=0;i<8;i++)publisher[i]=i;
 assert(qemu_ios_portable_frame_copy(copy,8,0,&f)==QEMU_IOS_OK);
 for(int i=0;i<8;i++){assert(copy[i]==i);assert(publisher[i]==0xee);}
 assert(qemu_ios_portable_frame_copy(copy,8,7,&f)==QEMU_IOS_NO_FRAME);
 f.version=99;assert(qemu_ios_portable_frame_copy(copy,8,0,&f)==QEMU_IOS_INVALID);
 f.version=1;f.size=8;assert(qemu_ios_portable_frame_copy(copy,8,0,&f)==QEMU_IOS_INVALID);
 puts("PASS: production frame copies while locked, owned pixels survive publisher overwrite, bounded capacity metadata and version guards");
}
