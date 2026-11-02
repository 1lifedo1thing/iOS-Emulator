/* The camera ISP's firmware as the kext and H3ISP.mediacapture talk to it (s5l8930_isp.c): mailbox transactions
 * on the category register file, the command registers the host polls (sensor mode, sensor recognition, autofocus)
 * and the shutter's flags in the frames' statistics records (shutter+strobe: a preflash frame, then the still).
 *
 * SLICE hw/arm/s5l8930_isp.c define ISP_|CPU_|MBX_
 * SLICE hw/arm/s5l8930_isp.c range struct S5L8930ISPState { | void s5l8930_isp_set_dart
 * SLICE hw/arm/s5l8930_isp.c range static const struct { uint8_t cat, reg; } isp_commands | static uint64_t isp_cpu_read
 * SLICE hw/arm/s5l8930_isp.c fn isp_metadata
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int SysBusDevice, MemoryRegion, DeviceState, QEMUTimer;
typedef int *qemu_irq;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static void stw_le_p(void *p, unsigned v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
typedef struct S5L8930ISPState S5L8930ISPState;
static void isp_log(S5L8930ISPState *s, const char *fmt, ...) { (void)s; (void)fmt; }
static uint8_t record[0x1a0];      /* the last statistics record written */
static void isp_dma_write(S5L8930ISPState *s, uint32_t va, const uint8_t *buf, size_t len)
{
    (void)s; assert(va == 0x01041000 && len == sizeof(record)); memcpy(record, buf, len);
}
#include "slice.h"

/* One mailbox transaction of up to seven (category, register, value) words, as AppleH3CamIn sends it. */
static void txn(S5L8930ISPState *s, bool write, unsigned n, const uint32_t *words, uint32_t *out)
{
    s->cpu[CPU_MBX_STATUS / 4] = (write ? MBX_WRITE : 0) | ((1u << n) - 1);
    for (unsigned i = 0; i < n; i++)
        s->cpu[CPU_MBX0 / 4 + i] = words[i];
    isp_mailbox(s);
    assert(s->cpu[CPU_MBX_STATUS / 4] & MBX_DONE);
    assert(!(s->cpu[CPU_MBX_STATUS / 4] & MBX_WRITE));     /* no error */
    for (unsigned i = 0; out && i < n; i++)
        out[i] = s->cpu[CPU_MBX0 / 4 + i] & 0xff;
}
static unsigned rd(S5L8930ISPState *s, unsigned cat, unsigned reg)
{
    uint32_t w = cat << 16 | reg << 8, v;
    txn(s, false, 1, &w, &v);
    return v;
}
static void wr(S5L8930ISPState *s, unsigned cat, unsigned reg, unsigned val)
{
    uint32_t w = cat << 16 | reg << 8 | val;
    txn(s, true, 1, &w, NULL);
}

int main(void)
{
    static S5L8930ISPState s;
    uint32_t words[3] = { 0x200181, 0x200207, 0x2003a5 }, back[3];

    /* A plain register reads back what was written, several to a transaction. */
    txn(&s, true, 3, words, NULL);
    words[0] &= ~0xff; words[1] &= ~0xff; words[2] &= ~0xff;
    txn(&s, false, 3, words, back);
    assert(back[0] == 0x81 && back[1] == 0x07 && back[2] == 0xa5);

    /* Sensor mode: the request at 00:00 is the current mode the host polls at 00:91. */
    wr(&s, 0x00, 0x00, 1);
    assert(rd(&s, 0x00, 0x91) == 1);
    wr(&s, 0x00, 0x00, 0);
    assert(rd(&s, 0x00, 0x91) == 0);
    /* A mode change at 00:0a and the back sensor's recognition at 03:00 are done when they read 0; 03:07 = 0 is
     * "found". */
    wr(&s, 0x00, 0x0a, 1);
    assert(rd(&s, 0x00, 0x0a) == 0);
    wr(&s, 0x03, 0x07, 0xff);
    wr(&s, 0x03, 0x00, 1);
    assert(rd(&s, 0x03, 0x00) == 0 && rd(&s, 0x03, 0x07) == 0);
    /* Autofocus: the request 5 at 24:0c ends at 1. */
    wr(&s, 0x24, 0x0c, 5);
    assert(rd(&s, 0x24, 0x0c) == 1);

    /* No shutter: a settled record. */
    isp_metadata(&s, 0x01041000);
    assert(record[0x7c] == 1 && record[0x86] == 1 && record[0xc2] == 1 && record[0xc5] == 0);
    /* Shutter+strobe (00:34): the preflash frame (count 1), the flash frame (count 2), then none. */
    wr(&s, 0x00, 0x34, 1);
    assert(rd(&s, 0x00, 0x34) == 0);
    isp_metadata(&s, 0x01041000);
    assert(record[0xc5] == 1 && record[0x126] == 1);
    isp_metadata(&s, 0x01041000);
    assert(record[0xc5] == 1 && record[0x126] == 2);
    isp_metadata(&s, 0x01041000);
    assert(record[0xc5] == 0);
    /* Shutter alone (00:33): one frame, no strobe count. */
    wr(&s, 0x00, 0x33, 1);
    isp_metadata(&s, 0x01041000);
    assert(record[0xc5] == 1 && record[0x126] == 0);
    isp_metadata(&s, 0x01041000);
    assert(record[0xc5] == 0);
    printf("ok\n");
    return 0;
}
