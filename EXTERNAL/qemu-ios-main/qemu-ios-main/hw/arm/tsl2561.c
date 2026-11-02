/*
 * TAOS TSL2581 and TSL2561 ambient light sensors: the iPad's and the iPod touch 4G's TSL2581, the iPhone 3GS's
 * TSL2561 and the original iPhone's (m68ap: "als,tsl2561" at I2C0 0x49, INT on GPIO 0x49). Their own file, out
 * of s5l8930_i2c.c, so the S5L8900 machine builds them too.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"

/* ---- TAOS TSL2581 ambient light sensor (I2C2 0x39) ----
 *
 * 7B500 AppleTSL2581 (AppleEmbeddedLightSensor kext at c05e7000). Every
 * access goes through c05ea488/c05ea4b4: the sub-address byte is
 * 0x80 | reg (the part's COMMAND bit), so the register number is the low
 * five bits. probe (c05eac68) reads reg 0 and only checks the transfer
 * succeeded; handleStart writes CONTROL/TIMING/ANALOG and then checks the
 * ALS_INT GPIO (0x405) is not stuck low. The lux curve comes from the DT
 * (slopes/intercepts).
 *
 * Both parts (the TSL2561 below too): the COMMAND byte that opens a write
 * sets the register pointer (TSL2581 bits 4:0, TSL2561 bits 3:0), and reads
 * and further writes run on from it. While powered (TSL2581 CONTROL POWER
 * and ADC_EN, TSL2561 POWER = 3) the ADC integrates continuously; each
 * integration (TSL2581 TIMING x 2.7 ms, TSL2561 INTEG 13.7, 101 or 402 ms)
 * latches CH0 and CH1 for the `lux` property, in proportion to the gain and
 * the integration time, and with the interrupt enabled (INTERRUPT bit 4)
 * raises INT once CH0 has been outside the threshold window for PERSIST
 * integrations (0: every integration). INT holds until a COMMAND clears it
 * (TSL2581 special function 1, 0xe1; TSL2561 CLEAR, bit 6).
 */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930TSL2581State, S5L8930_TSL2581)

#define TSL_CMD         0x80
#define TSL_CONTROL     0x00
#define TSL_TIMING      0x01

struct S5L8930TSL2581State {
    I2CSlave i2c;
    qemu_irq irq;                   /* INT, asserted high (the board inverts it) */
    QEMUTimer *timer;               /* the integration in progress */
    uint8_t regs[32];
    uint8_t reg;
    bool addressing;
    bool int_pending;
    uint8_t out_count;              /* integrations in a row outside the window */
    uint32_t lux;
    /* the part's layout */
    bool tsl2561;
    uint8_t reg_mask, id_reg, id, data0, int_reg, thresh;
};

static unsigned tsl_integration_us(S5L8930TSL2581State *s)
{
    static const unsigned integ_us[4] = { 13700, 101000, 402000, 0 };

    if (s->tsl2561) {
        return integ_us[s->regs[TSL_TIMING] & 3];   /* 3: manual, not modeled */
    }
    return s->regs[TSL_TIMING] * 2700;              /* 0: manual, not modeled */
}

static bool tsl_running(S5L8930TSL2581State *s)
{
    uint8_t ctrl = s->regs[TSL_CONTROL];

    return (ctrl & 3) == 3 && tsl_integration_us(s);   /* TSL2561 POWER = 3; TSL2581 POWER, ADC_EN */
}

/*
 * CH0 for `lux`, with CH1 (infrared) a quarter of it, the first segment of
 * both parts' lux curves: the TSL2561 datasheet's T package gives 0.0215 lux
 * per count at 16x and 402 ms for that ratio, 0.723 counts per lux at 1x over
 * 100 ms, taken for the TSL2581 too (the DT's curves make lux of the counts).
 * Full scale is 65535; the TSL2561's 13.7 and 101 ms integrations stop at
 * 5047 and 37177.
 */
#define TSL_COUNTS_PER_KLUX 723     /* at 1x gain over 100 ms */
static uint16_t tsl_ch0(S5L8930TSL2581State *s)
{
    static const unsigned gain2581[4] = { 1, 8, 16, 111 };
    unsigned gain = s->tsl2561 ? (s->regs[TSL_TIMING] & 0x10 ? 16 : 1) : gain2581[s->regs[7] & 3];
    unsigned us = tsl_integration_us(s);
    unsigned full = !s->tsl2561 || us > 101000 ? 65535 : us > 13700 ? 37177 : 5047;
    uint64_t count = (uint64_t)s->lux * TSL_COUNTS_PER_KLUX * gain * us / (1000 * 100000ULL);

    return MIN(count, full);
}

static void tsl_update_irq(S5L8930TSL2581State *s)
{
    if (!s->tsl2561) {
        s->regs[TSL_CONTROL] = deposit32(s->regs[TSL_CONTROL], 5, 1, s->int_pending);  /* ADC_INTR */
    }
    qemu_set_irq(s->irq, s->int_pending);
}

static void tsl_update_timer(S5L8930TSL2581State *s)
{
    if (!tsl_running(s)) {
        timer_del(s->timer);
        s->out_count = 0;
    } else if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + tsl_integration_us(s) * 1000LL);
    }
}

/* An integration completes: latch both channels, then the threshold window. */
static void tsl_integrated(void *opaque)
{
    S5L8930TSL2581State *s = opaque;
    uint16_t ch0 = tsl_ch0(s);
    uint8_t intr = s->regs[s->int_reg];
    unsigned persist = intr & 0xf;

    stw_le_p(&s->regs[s->data0], ch0);
    stw_le_p(&s->regs[s->data0 + 2], ch0 / 4);
    if (!s->tsl2561) {
        s->regs[TSL_CONTROL] |= 0x10;               /* ADC_VALID */
    }
    if (ch0 < lduw_le_p(&s->regs[s->thresh]) || ch0 > lduw_le_p(&s->regs[s->thresh + 2])) {
        s->out_count = MIN(s->out_count + 1, 15);
    } else {
        s->out_count = 0;
    }
    if ((intr & 0x30) == 0x10 && (persist == 0 || s->out_count >= persist)) {
        s->int_pending = true;
        tsl_update_irq(s);
    }
    tsl_update_timer(s);
}

static int tsl2581_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(i2c);

    if (event == I2C_START_SEND) {
        s->addressing = true;
    }
    return 0;
}

static uint8_t tsl2581_recv(I2CSlave *i2c)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(i2c);

    return s->regs[s->reg++ & s->reg_mask];
}

static void tsl_command(S5L8930TSL2581State *s, uint8_t cmd)
{
    s->reg = cmd;
    if (s->tsl2561 ? (cmd & 0x40) : (cmd & 0x7f) == 0x61) {  /* CLEAR; special function 1 */
        s->int_pending = false;
        tsl_update_irq(s);
    }
}

static int tsl2581_send(I2CSlave *i2c, uint8_t data)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(i2c);
    uint8_t reg;

    if (s->addressing) {
        s->addressing = false;
        tsl_command(s, data);
        return 0;
    }
    reg = s->reg++ & s->reg_mask;
    if (reg >= s->id_reg) {
        return 0;                   /* ID and the data registers are read-only */
    }
    if (reg == TSL_CONTROL && !s->tsl2561) {
        data = (data & 0x03) | (s->regs[reg] & 0x30);   /* ADC_VALID and ADC_INTR are the part's */
        if (!(data & 2)) {
            data &= ~0x10;
        }
    }
    s->regs[reg] = data;
    if (reg == TSL_TIMING) {
        timer_del(s->timer);        /* a new integration time starts a new integration */
    }
    if (reg == TSL_CONTROL || reg == TSL_TIMING) {
        tsl_update_timer(s);
    }
    return 0;
}

static void tsl2581_reset(DeviceState *dev)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[s->id_reg] = s->id;
    s->reg = 0;
    s->addressing = true;
    s->int_pending = false;
    s->out_count = 0;
    tsl_update_irq(s);
}

static int tsl2581_post_load(void *opaque, int version_id)
{
    S5L8930TSL2581State *s = opaque;

    tsl_update_timer(s);            /* a version 1 state had no integration running */
    return 0;
}

static const VMStateDescription vmstate_s5l8930_tsl2581 = {
    .name = TYPE_S5L8930_TSL2581,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = tsl2581_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, S5L8930TSL2581State),
        VMSTATE_UINT8_ARRAY(regs, S5L8930TSL2581State, 32),
        VMSTATE_UINT8(reg, S5L8930TSL2581State),
        VMSTATE_BOOL(addressing, S5L8930TSL2581State),
        VMSTATE_BOOL_V(int_pending, S5L8930TSL2581State, 2),
        VMSTATE_UINT8_V(out_count, S5L8930TSL2581State, 2),
        VMSTATE_TIMER_PTR_V(timer, S5L8930TSL2581State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void tsl2581_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8930_tsl2581;
    device_class_set_legacy_reset(dc, tsl2581_reset);
    k->event = tsl2581_event;
    k->recv = tsl2581_recv;
    k->send = tsl2581_send;
}

static void tsl2581_init(Object *obj)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(obj);

    s->reg_mask = 0x1f;
    s->id_reg = 0x12;               /* PARTNO[7:4] = 9 (TSL2581), REVNO 0 */
    s->id = 0x90;
    s->data0 = 0x14;                /* CH0 (visible + IR), CH1 (IR) at 0x16 */
    s->int_reg = 0x02;
    s->thresh = 0x03;               /* low at 0x03, high at 0x05 */
    s->lux = 300;                   /* indoor light; qom-set lux at run time */
    object_property_add_uint32_ptr(obj, "lux", &s->lux, OBJ_PROP_FLAG_READWRITE);
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tsl_integrated, s);
}

static const TypeInfo s5l8930_tsl2581_info = {
    .name          = TYPE_S5L8930_TSL2581,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S5L8930TSL2581State),
    .instance_init = tsl2581_init,
    .class_init    = tsl2581_class_init,
};

/*
 * The TSL2561 (iPhone 3GS, "als,tsl2561" at I2C2 0x49): the same COMMAND bit
 * and CONTROL, TIMING with INTEG and a 16x GAIN bit, thresholds at 0x02 and
 * 0x04, INTERRUPT at 0x06 (INTR 01: level), ID at 0x0a (PARTNO 5: T/FN/CL)
 * and the channels at 0x0c/0x0e.
 */
static void tsl2561_init(Object *obj)
{
    S5L8930TSL2581State *s = S5L8930_TSL2581(obj);

    s->tsl2561 = true;
    s->reg_mask = 0x0f;
    s->id_reg = 0x0a;
    s->id = 0x50;
    s->data0 = 0x0c;
    s->int_reg = 0x06;
    s->thresh = 0x02;
}

static const TypeInfo s5l8930_tsl2561_info = {
    .name          = TYPE_S5L8930_TSL2561,
    .parent        = TYPE_S5L8930_TSL2581,
    .instance_init = tsl2561_init,
};

static void tsl_register_types(void)
{
    type_register_static(&s5l8930_tsl2581_info);
    type_register_static(&s5l8930_tsl2561_info);
}

type_init(tsl_register_types)
