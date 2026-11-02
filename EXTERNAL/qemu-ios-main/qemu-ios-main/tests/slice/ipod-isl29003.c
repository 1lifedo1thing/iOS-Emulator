/* The ISL29003 light sensor as the n72's driver talks to it: a register write
 * sets the pointer and reads start there (each transfer anew); CONTROL takes
 * its range and keeps INT_FLAG clear-only; a conversion takes 2^width cycles of
 * the resistor's oscillator and latches the count for `lux` at that range; and
 * readings outside the threshold window raise the interrupt after PERSIST
 * conversions.
 *
 * SLICE include/hw/arm/ipod_touch_isl29003dl.h define LIGHTSENSOR_
 * SLICE include/hw/arm/ipod_touch_isl29003dl.h typedef ISL29003DLState
 * SLICE hw/arm/ipod_touch_isl29003dl.c define ISL29003_OSC
 * SLICE hw/arm/ipod_touch_isl29003dl.c fn isl29003dl_width_bits isl29003dl_running isl29003dl_conversion_ns isl29003dl_count isl29003dl_update_irq isl29003dl_update_timer isl29003dl_convert isl29003dl_conversion_done isl29003dl_write_reg isl29003dl_event isl29003dl_recv isl29003dl_send
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LIGHT_TRACE(...) do {} while (0)
typedef int I2CSlave, qemu_irq;
typedef struct { bool pending; int64_t when; } QEMUTimer;
enum i2c_event { I2C_START_RECV, I2C_START_SEND, I2C_FINISH };
enum { QEMU_CLOCK_VIRTUAL };
static int64_t now;
static int irq_level;
static int64_t qemu_clock_get_ns(int c) { (void)c; return now; }
static void qemu_set_irq(qemu_irq i, int level) { (void)i; irq_level = level; }
static bool timer_pending(QEMUTimer *t) { return t->pending; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->pending = true; t->when = when; }
#define ISL29003DL(p) ((ISL29003DLState *)(p))
#include "slice.h"

static ISL29003DLState s;
static QEMUTimer timer;

static void wr(uint8_t reg, uint8_t val)
{
    isl29003dl_event(&s.i2c, I2C_START_SEND);
    isl29003dl_send(&s.i2c, reg);
    isl29003dl_send(&s.i2c, val);
}

/* Register select, then a read transfer of n bytes. */
static void rd(uint8_t reg, uint8_t *out, int n)
{
    isl29003dl_event(&s.i2c, I2C_START_SEND);
    isl29003dl_send(&s.i2c, reg);
    isl29003dl_event(&s.i2c, I2C_START_RECV);
    for (int i = 0; i < n; i++) {
        out[i] = isl29003dl_recv(&s.i2c);
    }
}

/* Run the pending conversion. */
static void tick(void)
{
    assert(timer.pending);
    now = timer.when;
    timer.pending = false;
    isl29003dl_conversion_done(&s);
}

int main(void)
{
    uint8_t b[2];

    s.timer = &timer;
    s.rext = 499000;
    s.lux = 300;

    /* CONTROL holds what is written and reads back from its own address. */
    wr(LIGHTSENSOR_CONTROL, 0x04);
    rd(LIGHTSENSOR_CONTROL, b, 1);
    assert(b[0] == 0x04);
    rd(LIGHTSENSOR_CONTROL, b, 1);   /* a second transfer starts at the pointer again */
    assert(b[0] == 0x04);

    /* Off: no conversion. On at 16 bits: one conversion in 2^16 cycles of 327.68 kHz * 100k / 499k. */
    rd(LIGHTSENSOR_LSB_SENSOR, b, 2);
    assert(b[0] == 0 && b[1] == 0 && !timer.pending);
    now = 1000;
    wr(LIGHTSENSOR_IT_HI, 0xff);
    wr(LIGHTSENSOR_COMMAND, LIGHTSENSOR_CMD_ENABLE);
    assert(timer.pending && timer.when == 1000 + 998000000);
    tick();
    /* range 1 (4000 lux at 100k = 801.6 lux at 499k): 300 lux -> 300 * 65536 / 801.6 = 24526 */
    rd(LIGHTSENSOR_LSB_SENSOR, b, 2);
    assert((b[0] | b[1] << 8) == 24526);
    assert(timer.pending);   /* continuous */

    /* Range 0 saturates at 300 lux; 8 bits: 300 * 256 / 801.6 = 95 at range 1. */
    wr(LIGHTSENSOR_CONTROL, 0x00);
    tick();
    rd(LIGHTSENSOR_LSB_SENSOR, b, 2);
    assert((b[0] | b[1] << 8) == 0xffff);
    wr(LIGHTSENSOR_CONTROL, 0x04);
    wr(LIGHTSENSOR_COMMAND, LIGHTSENSOR_CMD_ENABLE | 2);
    tick();
    rd(LIGHTSENSOR_LSB_SENSOR, b, 2);
    assert(b[0] == 95 && b[1] == 0);
    s.lux = 100;
    tick();
    rd(LIGHTSENSOR_LSB_SENSOR, b, 1);
    assert(b[0] == 31);

    /* Threshold window [0x10, 0x20] on 16 bits, persistence 4: the interrupt after four readings outside it. */
    wr(LIGHTSENSOR_COMMAND, LIGHTSENSOR_CMD_ENABLE);
    wr(LIGHTSENSOR_IT_HI, 0x20);
    wr(LIGHTSENSOR_IT_LO, 0x10);
    wr(LIGHTSENSOR_CONTROL, 0x04 | 1);
    s.lux = 300;   /* msb 0x5f */
    for (int i = 0; i < 3; i++) {
        tick();
        assert(!irq_level);
    }
    tick();
    assert(irq_level);
    rd(LIGHTSENSOR_CONTROL, b, 1);
    assert(b[0] & LIGHTSENSOR_CTRL_INT_FLAG);
    /* Writing the flag as 1 doesn't clear it; 0 does (through the 0x41 alias too). */
    wr(LIGHTSENSOR_CONTROL, 0x04 | 1 | LIGHTSENSOR_CTRL_INT_FLAG);
    assert(irq_level);
    wr(0x41, 0x04 | 1);
    assert(!irq_level);
    /* Inside the window: never. */
    s.lux = 60;   /* 60 * 65536 / 801.6 = 4905, msb 0x13 */
    for (int i = 0; i < 20; i++) {
        tick();
        assert(!irq_level);
    }
    /* Power-down stops converting. */
    wr(LIGHTSENSOR_COMMAND, LIGHTSENSOR_CMD_ENABLE | LIGHTSENSOR_CMD_ADC_PD);
    assert(!timer.pending);
    puts("PASS: ISL29003 register pointer, control, conversion timing and count, threshold interrupt");
    return 0;
}
