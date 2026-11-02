/* boot-args is a startup property bounded by what iBoot's snprintf copies: at most 255 bytes once every '%' is
 * doubled (the arguments become snprintf's format), set before the machine starts, and empty means iBoot's own
 * command line. The environment plays no part.
 *
 * SLICE hw/arm/ipod_touch_2g.c define BOOT_ARGS_CMDLINE_LEN
 * SLICE hw/arm/ipod_touch_2g.c fn ipod_touch_requested_boot_args boot_args_as_format ipod_touch_set_boot_args
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {void*cpu;bool boot_args_explicit;char boot_args[512];} IPodTouchMachineState;
typedef IPodTouchMachineState Object;
typedef int Error;
#define IPOD_TOUCH_MACHINE(o) (o)
#define error_setg(errp,...) (**(errp)=1)
#include "slice.h"

int main(void){
 IPodTouchMachineState machine={0};
 char format[BOOT_ARGS_CMDLINE_LEN];
 setenv("IT_BOOT_ARGS","-v",1);
 assert(!ipod_touch_requested_boot_args(&machine));
 int error=0;Error*ep=&error;
 ipod_touch_set_boot_args(&machine,"",&ep);
 assert(!error&&machine.boot_args_explicit&&!ipod_touch_requested_boot_args(&machine));
 /* the format: '%' doubled */
 assert(boot_args_as_format("debug=0x8 100% -v",format,sizeof(format))&&!strcmp(format,"debug=0x8 100%% -v"));
 assert(boot_args_as_format("",format,sizeof(format))&&!strcmp(format,""));
 /* 255 bytes fit; 256, or 254 and a '%', don't */
 char exact[256];memset(exact,'x',255);exact[255]=0;
 ipod_touch_set_boot_args(&machine,exact,&ep);assert(!error&&strlen(machine.boot_args)==255);
 char over[257];memset(over,'x',256);over[256]=0;
 ipod_touch_set_boot_args(&machine,over,&ep);assert(error&&strlen(machine.boot_args)==255);error=0;
 exact[254]='%';
 ipod_touch_set_boot_args(&machine,exact,&ep);assert(error);error=0;
 exact[253]='%';exact[254]=0;
 ipod_touch_set_boot_args(&machine,exact,&ep);assert(!error&&!strcmp(machine.boot_args,exact));
 assert(boot_args_as_format(exact,format,sizeof(format))&&strlen(format)==255);
 /* not after the machine starts */
 machine.cpu=&machine;ipod_touch_set_boot_args(&machine,"changed",&ep);
 assert(error&&!strcmp(machine.boot_args,exact));
 puts("PASS: boot-args bounded by iBoot's 255-byte format, '%' doubled, explicit empty, startup only, no environment");
}
