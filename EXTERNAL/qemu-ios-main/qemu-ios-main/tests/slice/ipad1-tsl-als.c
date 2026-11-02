/* The TSL2561 (3GS) and TSL2581 (iPad, iPod touch 4G) light sensors as their drivers talk to them. 6.1.3's
 * AppleTSL2561 powers the part (CONTROL 3), sets TIMING 0x12 (16x, 402 ms) or 0x10 (13.7 ms), thresholds 0/0
 * through word writes (0xa2, 0xa4) and INTERRUPT 0x13/0x11/0x1d (level, persistence 3/1/13), and reads only
 * once INT fires: the part must integrate, latch the channels for `lux` and raise INT after PERSIST
 * integrations outside the window, held until a COMMAND with CLEAR. The TSL2581's clear is special function
 * 1 (0xe1), and its CONTROL reports ADC_VALID and ADC_INTR.
 *
 * SLICE hw/arm/tsl2561.c define TSL_
 * SLICE hw/arm/tsl2561.c range struct S5L8930TSL2581State { | static unsigned tsl_integration_us
 * SLICE hw/arm/tsl2561.c fn tsl_integration_us tsl_running tsl_ch0 tsl_update_irq tsl_update_timer tsl_integrated tsl2581_event tsl2581_recv tsl_command tsl2581_send tsl2581_reset
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef struct { int unused; } I2CSlave;
typedef int qemu_irq;
typedef struct { bool pending; int64_t when; } QEMUTimer;
typedef struct S5L8930TSL2581State S5L8930TSL2581State;
typedef S5L8930TSL2581State DeviceState;
enum i2c_event { I2C_START_RECV, I2C_START_SEND, I2C_FINISH };
enum { QEMU_CLOCK_VIRTUAL };
#define S5L8930_TSL2581(p) ((S5L8930TSL2581State *)(p))
static int64_t now;
static int irq_level;
static int64_t qemu_clock_get_ns(int c) { (void)c; return now; }
static void qemu_set_irq(qemu_irq i, int level) { (void)i; irq_level = level; }
static bool timer_pending(QEMUTimer *t) { return t->pending; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->pending = true; t->when = when; }
static uint32_t deposit32(uint32_t v, int start, int len, uint32_t f)
{
    uint32_t mask = ((1u << len) - 1) << start;
    return (v & ~mask) | ((f << start) & mask);
}
static void stw_le_p(void *p, uint16_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static uint16_t lduw_le_p(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
#include "slice.h"

static S5L8930TSL2581State s;
static QEMUTimer timer;

static void wr(uint8_t cmd, uint8_t val)
{
    tsl2581_event((I2CSlave *)&s, I2C_START_SEND);
    tsl2581_send((I2CSlave *)&s, cmd);
    tsl2581_send((I2CSlave *)&s, val);
}

static void cmd(uint8_t c)
{
    tsl2581_event((I2CSlave *)&s, I2C_START_SEND);
    tsl2581_send((I2CSlave *)&s, c);
}

static uint16_t rd16(uint8_t c)
{
    cmd(c);
    tsl2581_event((I2CSlave *)&s, I2C_START_RECV);
    uint16_t lo = tsl2581_recv((I2CSlave *)&s);
    return lo | tsl2581_recv((I2CSlave *)&s) << 8;
}

static void tick(void)
{
    assert(timer.pending);
    now = timer.when;
    timer.pending = false;
    tsl_integrated(&s);
}

static void setup(bool tsl2561)
{
    memset(&s, 0, sizeof(s));
    s.timer = &timer;
    s.lux = 300;
    s.tsl2561 = tsl2561;
    s.reg_mask = tsl2561 ? 0x0f : 0x1f;
    s.id_reg = tsl2561 ? 0x0a : 0x12;
    s.id = tsl2561 ? 0x50 : 0x90;
    s.data0 = tsl2561 ? 0x0c : 0x14;
    s.int_reg = tsl2561 ? 0x06 : 0x02;
    s.thresh = tsl2561 ? 0x02 : 0x03;
    now = 0;
    tsl2581_reset((DeviceState *)&s);
}

int main(void)
{
    /* TSL2561, 6.1.3's sequence */
    setup(true);
    wr(0x80, 0x03);                     /* power */
    wr(0x81, 0x12);                     /* 16x, 402 ms */
    assert(timer.pending && timer.when == 402000000);
    wr(0xa2, 0x00); tsl2581_send((I2CSlave *)&s, 0x00);   /* word writes: thresholds 0/0 */
    wr(0xa4, 0x00); tsl2581_send((I2CSlave *)&s, 0x00);
    wr(0x86, 0x13);                     /* level, persistence 3 */
    tick(); tick();
    assert(!irq_level);
    tick();                             /* third integration above the window */
    assert(irq_level);
    /* 300 lux at 16x over 402 ms: 300 * 0.723 * 16 * 4.02 */
    uint16_t ch0 = rd16(0xac);
    assert(ch0 == 13951 && rd16(0xae) == ch0 / 4);
    tick();
    assert(irq_level);                  /* held until cleared */
    cmd(0xc0);                          /* CLEAR */
    assert(!irq_level);
    /* inside the window: no interrupt, however long */
    wr(0xa2, 0x00); tsl2581_send((I2CSlave *)&s, 0x00);
    wr(0xa4, 0xff); tsl2581_send((I2CSlave *)&s, 0xff);
    for (int i = 0; i < 5; i++) tick();
    assert(!irq_level);
    /* 13.7 ms stops at 5047; persistence 0 interrupts every integration */
    s.lux = 100000;
    wr(0x81, 0x10);
    wr(0x86, 0x10);
    tick();                             /* the 402 ms integration already running */
    cmd(0xc0);
    tick();
    assert(irq_level && rd16(0xac) == 5047);
    /* power-down stops the integrations */
    wr(0x80, 0x00);
    assert(!timer.pending);
    /* interrupt disabled: none */
    setup(true);
    wr(0x80, 0x03); wr(0x81, 0x10); wr(0x86, 0x00);
    for (int i = 0; i < 3; i++) tick();
    assert(!irq_level && rd16(0xac) > 0);

    /* TSL2581: POWER and ADC_EN, TIMING in 2.7 ms cycles, ANALOG gain, thresholds at 0x03/0x05 */
    setup(false);
    wr(0x81, 0x25);                     /* 37 cycles: 99.9 ms */
    wr(0x80, 0x01);                     /* POWER alone: no integration */
    assert(!timer.pending);
    wr(0x80, 0x03);
    assert(timer.pending && timer.when == 99900000);
    wr(0x87, 0x01);                     /* 8x */
    wr(0x83, 0x00); wr(0x84, 0x00); wr(0x85, 0x00); wr(0x86, 0x00);
    wr(0x82, 0x11);                     /* interrupt on, persistence 1 */
    assert(!(s.regs[0] & 0x10));
    tick();
    assert(irq_level && (s.regs[0] & 0x30) == 0x30);   /* ADC_VALID, ADC_INTR */
    assert(rd16(0x94) == 1733);         /* 300 * 0.723 * 8 * 0.999 */
    wr(0x80, 0x33);                     /* the status bits are the part's */
    assert((s.regs[0] & 0x33) == 0x33);
    cmd(0xe1);                          /* special function 1: clear */
    assert(!irq_level && !(s.regs[0] & 0x20));
    wr(0x80, 0x01);                     /* ADC_EN off: no longer valid, stopped */
    assert(!(s.regs[0] & 0x10) && !timer.pending);
    tsl2581_reset((DeviceState *)&s);
    assert(!irq_level && s.regs[0x12] == 0x90);
    puts("PASS: TSL2561/TSL2581 pointer, integration, lux counts, threshold interrupt with persistence and clear");
    return 0;
}
