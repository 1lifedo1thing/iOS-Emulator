/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Upstream pattern/refusal fixtures adapted from 1b451cc86c77c153fae1d7be2d4f151e693a9d75.
 * Exact production functions, emulated RAM boundary. No guest snprintf/CPU execution.
 * Optional IT_TEST_IBOOT supplies read-only known n72 7E18 image; no fixture included.
 * SLICE hw/arm/it_iboot.c range static uint16_t iboot_u16( | #ifndef IT_IBOOT_HOST_TEST
 * SLICE hw/arm/ipod_touch_2g.c define BOOT_ARGS_CMDLINE_LEN|IBOOT_SCAN_PA_START|IBOOT_SCAN_LEN
 * SLICE hw/arm/ipod_touch_2g.c fn ipod_touch_requested_boot_args boot_args_as_format ipod_touch_boot_args_before_nand_read
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t ram[0x40000];
static unsigned reads,writes,warnings;
typedef struct { struct {uint32_t regs[16];} env; } ARMCPU;
typedef struct {void *unused;} Notifier;
typedef struct { ARMCPU *cpu; char boot_args[512]; bool boot_args_explicit, boot_args_reported; Notifier boot_args_nand_read; uint32_t boot_args_format,boot_args_command_line;} IPodTouchMachineState;
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
static void cleanup_ptr(void *p){free(*(void **)p);}
#define g_autofree __attribute__((cleanup(cleanup_ptr)))
#define g_try_malloc malloc
#define warn_report_once(...) (++warnings)
static void cpu_physical_memory_read(uint32_t at,void *buf,size_t size){assert(at>=0xff00000 && (uint64_t)at-0xff00000+size<=sizeof(ram));memcpy(buf,ram+(at-0xff00000),size);reads++;}
static void cpu_physical_memory_write(uint32_t at,const void *buf,size_t size){assert(at>=0xff00000 && (uint64_t)at-0xff00000+size<=sizeof(ram));memcpy(ram+(at-0xff00000),buf,size);writes++;}
static void stl_le_p(void *out,uint32_t v){uint8_t *p=out;for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
#include "slice.h"
#define BASE 0x0ff00000u
static void put16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p, uint32_t v){put16(p,v);put16(p+2,v>>16);}
static void epoch_fixture(uint8_t *img, size_t acc, size_t call, unsigned floor){
 static const uint8_t accessor[]={0x02,0x48,0x00,0x68,0x40,0x05,0x40,0x0e,0x70,0x47};
 memcpy(img+acc,accessor,sizeof(accessor)); put32(img+acc+12,0x3d100008);
 int32_t off=(int32_t)acc-(int32_t)(call+4);
 put16(img+call,0xf000|((off>>12)&0x7ff)); put16(img+call+2,0xf800|((off>>1)&0x7ff));
 put16(img+call+4,0x2800|(floor-1)); put16(img+call+6,0xd800); put16(img+call+8,0x2000|floor); put16(img+call+10,0xbd80);
}
/* ldr r3, =commandLine; ldr r2, ="%s "; adds r0, r3, #0; str r3, [sp]; movs r1, #0x80; lsls r1, r1, #1; adds r3, r6, #0; bl
 * (7E18's form), its two literals at 64 and 68, "%s " at 128 and commandLine at 0x300 */
static void format_fixture(uint8_t *image, uint32_t base){
 static const uint16_t code[]={0x4b0f,0x4a10,0x1c18,0x9300,0x2180,0x0049,0x1c33,0xf000,0xf800};
 memset(image,0,1024); for(size_t i=0;i<9;i++)put16(image+2*i,code[i]);
 put32(image+64,base+0x300); put32(image+68,base+128); memcpy(image+128,"%s ",4);
}
static void synthetic(void){
 static uint8_t img[0x27000];
 /* epoch: floor after the one call into the fuse accessor */
 memset(img,0,sizeof(img)); epoch_fixture(img,0x143a0,0x1a958,4); assert(it_iboot_find_epoch(img,sizeof(img))==4);
 epoch_fixture(img,0x143a0,0x1a958,3); assert(it_iboot_find_epoch(img,sizeof(img))==3);
 put16(img+0x1a958+6,0xd100); assert(it_iboot_find_epoch(img,sizeof(img))==3);      /* bne form (385.22) */
 put16(img+0x1a958+8,0x2100); assert(!it_iboot_find_epoch(img,sizeof(img)));          /* movs r1: not the floor */
 epoch_fixture(img,0x143a0,0x1a958,3); epoch_fixture(img,0x143a0,0x1b000,4); assert(!it_iboot_find_epoch(img,sizeof(img))); /* ambiguous */
 memset(img,0,sizeof(img)); epoch_fixture(img,0x143a0,0x1a958,3); memcpy(img+0x2000,img+0x143a0,16); assert(!it_iboot_find_epoch(img,sizeof(img)));
 memset(img,0,sizeof(img)); assert(!it_iboot_find_epoch(img,sizeof(img))&&!it_iboot_find_epoch(NULL,16)&&!it_iboot_find_epoch(img,8));
 /* iBoot-204's inline compare: ldr r3,[r3]; lsrs r3,r3,#24; cmp r3,#M; beq; ldr r0/r1 = the panic text */
 memset(img,0,sizeof(img)); put16(img+0x1fb6,0x681b); put16(img+0x1fb8,0x0e1b); put16(img+0x1fba,0x2b03); put16(img+0x1fbc,0xd003);
 put16(img+0x1fbe,0x4905); put32(img+0x1fd4,0x1801a140); memcpy(img+0x1a140,"miu_init: Epoch Mismatch\n",25);
 assert(it_iboot_find_epoch(img,sizeof(img))==3);
 img[0x1a14a]='X'; assert(!it_iboot_find_epoch(img,sizeof(img))); img[0x1a14a]='E';                 /* not the panic */
 put16(img+0x1fb8,0x0e1a); assert(!it_iboot_find_epoch(img,sizeof(img))); put16(img+0x1fb8,0x0e1b); /* another register */
 memcpy(img+0x3000,img+0x1fb6,10); put32(img+0x3020,0x1801a140); assert(!it_iboot_find_epoch(img,sizeof(img))); /* ambiguous */
 /* the command-line format slot */
 uint8_t image[1024];uint32_t base=0x10000000,cmd=0;
 format_fixture(image,base); assert(it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd)==base+68 && cmd==base+0x300);
 format_fixture(image,base+0x1000); assert(it_iboot_find_boot_args_format(image,sizeof(image),base+0x1000,&cmd)==base+0x1044);
 format_fixture(image,base);image[129]='d'; assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd));   /* not "%s " */
 format_fixture(image,base);put16(image+8,0x2181); assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd)); /* not 0x100 */
 format_fixture(image,base);put16(image+2,0x4910); assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd)); /* not r2 */
 format_fixture(image,base);put32(image+64,base+0x302); assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd)); /* unaligned buffer */
 format_fixture(image,base);put16(image+20,0x480b); assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd)); /* the slot is shared */
 format_fixture(image,base);memcpy(image+24,image,18);put16(image+24,0x4b0b);put16(image+26,0x4a0c);put32(image+72,base+0x300);put32(image+76,base+128);
 assert(!it_iboot_find_boot_args_format(image,sizeof(image),base,&cmd));                                                   /* ambiguous */
 assert(!it_iboot_find_boot_args_format(NULL,1024,base,&cmd));
 format_fixture(image,base); for(size_t n=0;n<132;n++)assert(!it_iboot_find_boot_args_format(image,n,base,&cmd));
 assert(!it_iboot_find_boot_args_format(image,1024,UINT32_MAX-512,&cmd));
}



static void original_finder(void) {
 uint8_t image[1024]={0};
 put16(image,0x4c0f);put16(image+2,0x4810);put16(image+4,0x1c21);put16(image+6,0xf000);put16(image+8,0xf800);
 put32(image+64,BASE+0x300);put32(image+68,BASE+128);
 strcpy((char *)image+128,"gBootArgs.commandLine = [%s]\n");
 assert(it_iboot_find_command_line(image,sizeof(image),BASE)==BASE+0x300);
 image[128]='x';assert(!it_iboot_find_command_line(image,sizeof(image),BASE));
}
int main(void) {
 synthetic();original_finder();format_fixture(ram,BASE);
 uint32_t expected_slot=BASE+68,expected_command=BASE+0x300;
 const char *fixture=getenv("IT_TEST_IBOOT");
 if(fixture){
  FILE *f=fopen(fixture,"rb");assert(f);memset(ram,0,sizeof(ram));
  assert(fread(ram,1,sizeof(ram),f)==159744 && fgetc(f)==EOF);assert(!fclose(f));
  assert(memmem(ram,sizeof(ram),"iBoot-636.66.33",14));expected_slot=0xff11de0;expected_command=0xff28404;
 }
 uint8_t *original=malloc(sizeof(ram));assert(original);memcpy(original,ram,sizeof(ram));
 uint32_t cmd=0;assert(it_iboot_find_boot_args_format(ram,sizeof(ram),BASE,&cmd)==expected_slot && cmd==expected_command);
 ARMCPU cpu={0};IPodTouchMachineState s={.cpu=&cpu};cpu.env.regs[15]=BASE;strcpy(s.boot_args,"serial=3 100% -v");
 ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);
 assert(reads==1 && writes==2 && s.boot_args_format==expected_slot && s.boot_args_command_line==expected_command);
 assert(iboot_u32(ram+(expected_slot-BASE))==expected_command && !strcmp((char *)ram+(expected_command-BASE),"serial=3 100%% -v"));
 for(unsigned k=strlen((char *)ram+(expected_command-BASE));k<256;k++)assert(!ram[expected_command-BASE+k]);
 memset(ram+(expected_command-BASE),0,256);ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);assert(reads==1 && writes==4);
 unsigned before=writes;cpu.env.regs[15]=0xc0000100;
 for(unsigned i=0;i<200;i++)ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);
 assert(writes==before && reads==1); /* no post-handoff writes or scan */
 cpu.env.regs[15]=BASE;s.boot_args[0]=0;ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);assert(writes==before);
 memcpy(ram,original,sizeof(ram));s.boot_args_format=0;s.boot_args_reported=false;strcpy(s.boot_args,"serial=3");
 ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);assert(writes==before+2 && reads==2);
 memset(ram,0,sizeof(ram));s.boot_args_format=0;before=writes;ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);assert(writes==before && warnings==1);
 /* Defense in depth: invalid internal state must not write partial format data. */
 memcpy(ram,original,sizeof(ram));memset(s.boot_args,'%',128);s.boot_args[128]=0;s.boot_args_format=0;
 ipod_touch_boot_args_before_nand_read(&s.boot_args_nand_read,NULL);assert(writes==before);
 free(original);
 puts("PASS production early-handoff finder/hook: unique pattern, near-misses, old finder, ordered NAND cache/restage, percent bounds and no post-kernel/empty/absent writes");
}
