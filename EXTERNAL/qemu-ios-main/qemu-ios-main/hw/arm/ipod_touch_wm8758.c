/*
 * Wolfson WM8758 audio codec, 2-wire control interface (iPod touch 1G, I2C1
 * address 0x1A: the device tree's audio0@1A, AppleWM8758Audio).
 *
 * The control port is write-only: every transfer is two bytes, B[15:9] the
 * register address and B[8:0] its 9-bit value (WM8758 datasheet, "Control
 * interface"). Writing register 0 resets the register file. There is no
 * read-back, so recv answers 0xff like an undriven bus.
 *
 * The kernel's driver needs the codec to ACK its setup writes before it
 * publishes the IOAudio device mediaserverd waits for; the analog side
 * (headphone output) is not modeled.
 *
 * The codec masters the I2S frame clock (its DT audio-data's TX config has
 * bit 20 set, the Samsung I2S's external clock; see hw/arm/s5l8930_i2s.c),
 * so "lrclk" is what its I2S port plays at. In master mode (R6 bit 0) LRCLK
 * is SYSCLK / 256: SYSCLK is the MCLK input or, with R6 CLKSEL, the PLL's
 * MCLK * (N + K / 2^24) / 4 (R36 N and PRESCALE, R37-R39 K), over R6 MCLKDIV
 * (Linux's sound/soc/codecs/wm8985.c, which drives the WM8758: its PLL
 * targets 4 * 2 * SYSCLK at MCLKDIV 2). "mclk-hz" is the input: the PLL
 * table in AppleWM8758Audio (1.0, 1.1, 1.1.4) has N.K 10.0311875 for
 * 44.1 kHz and 10.9182913 for 48 kHz at MCLKDIV 2, 256 fs from 9.0036 MHz
 * for both. ponytail: R1 PLLEN is not checked (the driver never sets bit 5
 * and plays); as the slave the codec drives no clock.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_WM8758 "wm8758"
OBJECT_DECLARE_SIMPLE_TYPE(WM8758State, WM8758)

#define WM8758_NREGS 64
#define WM8758_CLOCK_GEN 6      /* CLKSEL 8, MCLKDIV 7:5, MS 0 */
#define WM8758_PLL_N     36     /* PRESCALE 4, N 3:0; K in 37 (5:0), 38, 39 */

struct WM8758State {
    I2CSlave i2c;
    uint8_t nbytes;
    uint8_t first;
    uint16_t reg[WM8758_NREGS];
    uint32_t mclk_hz;           /* property: the MCLK input; 0 drives no clock */
    Clock *lrclk;
};

/* The frame clock the registers make from an MCLK of mclk_hz; 0 as the slave. */
static unsigned wm8758_lrclk(const uint16_t *reg, uint32_t mclk_hz)
{
    static const unsigned mclkdiv2[8] = { 2, 3, 4, 6, 8, 12, 16, 24 };   /* MCLKDIV x 2 */
    uint16_t clk = reg[WM8758_CLOCK_GEN];
    uint64_t sysclk = (uint64_t)mclk_hz << 24;          /* before MCLKDIV, x 2^24 */

    if (!(clk & 1)) {
        return 0;
    }
    if (clk & 0x100) {
        uint64_t nk = (uint64_t)(reg[WM8758_PLL_N] & 0xf) << 24 | (uint64_t)(reg[37] & 0x3f) << 18 |
                      (reg[38] & 0x1ff) << 9 | (reg[39] & 0x1ff);

        sysclk = mclk_hz * nk / 4 / (reg[WM8758_PLL_N] & 0x10 ? 2 : 1);
    }
    sysclk = sysclk * 2 / mclkdiv2[clk >> 5 & 7];
    return (sysclk + (128ull << 24)) / (256ull << 24);
}

static int wm8758_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8758(i2c)->nbytes = 0;
    return 0;
}

static int wm8758_send(I2CSlave *i2c, uint8_t data)
{
    WM8758State *s = WM8758(i2c);

    if (s->nbytes++ == 0) {
        s->first = data;
        return 0;
    }
    if (s->nbytes == 2) {
        unsigned r = s->first >> 1;
        uint16_t v = ((s->first & 1) << 8) | data;
        unsigned rate;

        if (r == 0) {
            memset(s->reg, 0, sizeof(s->reg));
        } else if (r < WM8758_NREGS) {
            s->reg[r] = v;
        }
        rate = wm8758_lrclk(s->reg, s->mclk_hz);
        if (rate) {
            clock_update_hz(s->lrclk, rate);
        }
    }
    return 0;
}

static uint8_t wm8758_recv(I2CSlave *i2c)
{
    return 0xff;
}

static void wm8758_reset(DeviceState *dev)
{
    WM8758State *s = WM8758(dev);

    s->nbytes = 0;
    memset(s->reg, 0, sizeof(s->reg));
}

static void wm8758_init(Object *obj)
{
    WM8758(obj)->lrclk = qdev_init_clock_out(DEVICE(obj), "lrclk");
}

static const Property wm8758_properties[] = {
    DEFINE_PROP_UINT32("mclk-hz", WM8758State, mclk_hz, 0),
};

static const VMStateDescription vmstate_wm8758 = {
    .name = TYPE_WM8758,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, WM8758State),
        VMSTATE_UINT8(nbytes, WM8758State),
        VMSTATE_UINT8(first, WM8758State),
        VMSTATE_UINT16_ARRAY(reg, WM8758State, WM8758_NREGS),
        VMSTATE_CLOCK_V(lrclk, WM8758State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void wm8758_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_wm8758;
    device_class_set_legacy_reset(dc, wm8758_reset);
    device_class_set_props(dc, wm8758_properties);
    k->event = wm8758_event;
    k->send = wm8758_send;
    k->recv = wm8758_recv;
}

static const TypeInfo wm8758_info = {
    .name          = TYPE_WM8758,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(WM8758State),
    .instance_init = wm8758_init,
    .class_init    = wm8758_class_init,
};

static void wm8758_register_types(void)
{
    type_register_static(&wm8758_info);
}

type_init(wm8758_register_types)
