/* A Z2 frame read answers with the command the driver read it with (0xEA, or
 * 0xEB, 3.1.3's): both packets carry it, the length packet's u16 sum of bytes
 * 0-13 and the frame packet's sum-to-zero pad still hold.
 *
 * SLICE include/hw/arm/ipod_touch_multitouch.h define MT_CMD_FRAME_READ
 * SLICE include/hw/arm/ipod_touch_multitouch.h range typedef struct MTFrameLengthPacket | #define MT_FRAME_PREFIX_LEN
 * SLICE hw/arm/ipod_touch_multitouch.c fn mt_stamp_frame_cmd
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "slice.h"

static void check(uint8_t cmd)
{
    uint8_t buf[sizeof(MTFrame) + 32] = { 0 };
    MTFrame *f = (MTFrame *)buf;
    uint16_t sum = 0;

    /* as mt_build_frame lays one out */
    f->frame_length.cmd = MT_CMD_FRAME_READ;
    f->frame_length.length1 = 0x34;
    for (int i = 0; i < 14; i++) {
        sum += ((uint8_t *)&f->frame_length)[i];
    }
    f->frame_length.checksum1 = sum & 0xff;
    f->frame_length.checksum2 = sum >> 8;
    f->frame_packet.cmd = MT_CMD_FRAME_READ;
    f->frame_packet.length1 = 0x34;
    f->frame_packet.checksum_pad = -(uint8_t)(MT_CMD_FRAME_READ + 0x34);

    mt_stamp_frame_cmd(f, cmd);
    assert(f->frame_length.cmd == cmd && f->frame_packet.cmd == cmd);
    sum = 0;
    for (int i = 0; i < 14; i++) {
        sum += ((uint8_t *)&f->frame_length)[i];
    }
    assert(sum == (f->frame_length.checksum1 | f->frame_length.checksum2 << 8));
    uint8_t pad = 0;
    for (int i = 0; i < 5; i++) {
        pad += ((uint8_t *)&f->frame_packet)[i];
    }
    assert(pad == 0);
}

int main(void)
{
    check(0xEA);
    check(0xEB);
    puts("PASS: frame reads echo 0xEA and 0xEB with their checksums intact");
    return 0;
}
