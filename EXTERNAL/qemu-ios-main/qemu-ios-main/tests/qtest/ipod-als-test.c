/*
 * The S5L8900 boards' ambient light sensors, as their I2C0 drivers reach them: the original iPhone's TSL2561
 * (m68ap "als,tsl2561" at 0x49, INT on GPIO 0x49) and the iPod touch 1G's ISL29003 (n45ap "als,isl29003" at
 * 0x44, INT on GPIO 0x4c). Each answers its address on the board's I2C0, converts the machine's
 * /machine/lightsensor lux (300 unless the host sets it), and the TSL2561's interrupt reaches the GPIO-IC.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define I2C0      0x3c600000ULL
#define SYSIC     0x39a00000ULL
#define I2CCON    0x00
#define I2CSTAT   0x04
#define I2CDS     0x0c
#define STAT_MT_START 0xf0   /* master transmit, START, TX/RX enable */
#define STAT_MT_STOP  0xd0
#define STAT_MR_START 0xb0   /* master receive */
#define STAT_MR_STOP  0x90
#define STAT_NAK      0x01   /* LASTBIT: no slave pulled SDA low */
#define GPIO_INTSTAT  0xa0
#define GPIO_INTEN    0xc0
#define MSEC 1000000LL

static char *rom, *nor, *nand;

static QTestState *boot(const char *machine)
{
    return qtest_initf("-machine '%s,bootrom=%s,iboot=%s,nand=%s,wifi=off' "
                       "-drive if=pflash,format=raw,readonly=on,file=%s -display none -audio driver=none",
                       machine, rom, rom, nand, nor);
}

/* A write transfer to `addr`: the bytes, then STOP. Returns whether the address was acknowledged. */
static bool i2c_write(QTestState *q, uint8_t addr, const uint8_t *bytes, size_t n)
{
    bool ack;

    qtest_writel(q, I2C0 + I2CDS, addr << 1);
    qtest_writel(q, I2C0 + I2CSTAT, STAT_MT_START);
    ack = !(qtest_readl(q, I2C0 + I2CSTAT) & STAT_NAK);
    for (size_t i = 0; i < n; i++) {
        qtest_writel(q, I2C0 + I2CDS, bytes[i]);
    }
    qtest_writel(q, I2C0 + I2CSTAT, STAT_MT_STOP);
    return ack;
}

/* A read transfer of `n` bytes from `addr`, from wherever its register pointer is. */
static void i2c_read(QTestState *q, uint8_t addr, uint8_t *bytes, size_t n)
{
    qtest_writel(q, I2C0 + I2CDS, addr << 1 | 1);
    qtest_writel(q, I2C0 + I2CSTAT, STAT_MR_START);
    g_assert_false(qtest_readl(q, I2C0 + I2CSTAT) & STAT_NAK);
    for (size_t i = 0; i < n; i++) {
        bytes[i] = qtest_readl(q, I2C0 + I2CDS);
    }
    qtest_writel(q, I2C0 + I2CSTAT, STAT_MR_STOP);
}

static int64_t lux(QTestState *q)
{
    QDict *r = qtest_qmp(q, "{'execute': 'qom-get', 'arguments': "
                            "{'path': '/machine/lightsensor', 'property': 'lux'}}");
    int64_t v;

    g_assert_true(qdict_haskey(r, "return"));
    v = qdict_get_int(r, "return");
    qobject_unref(r);
    return v;
}

/*
 * The TSL2561 as 1.0's AppleTSL2561 drives it: COMMAND (0x80 | reg) opens each access. ID 0x5x (PARTNO 5),
 * then POWER, TIMING 402 ms at 1x and a level interrupt on every integration: CH0 for 300 lux is 871
 * (0.723 counts per lux at 1x over 100 ms), CH1 a quarter, and INT is GPIO-IC line 0x49 (group 2, bit 9).
 */
static void m68_tsl2561(void)
{
    QTestState *q = boot("iPhone-2G,baseband=off");
    uint8_t id, ch[4];

    g_assert_cmpint(lux(q), ==, 300);
    qtest_writel(q, I2C0 + I2CCON, 0xa0);
    g_assert_true(i2c_write(q, 0x49, (const uint8_t[]){ 0x8a }, 1));
    i2c_read(q, 0x49, &id, 1);
    g_assert_cmphex(id & 0xf0, ==, 0x50);

    qtest_writel(q, SYSIC + GPIO_INTEN + 2 * 4, 1u << 9);
    g_assert_true(i2c_write(q, 0x49, (const uint8_t[]){ 0x80, 0x03 }, 2));   /* CONTROL: POWER */
    g_assert_true(i2c_write(q, 0x49, (const uint8_t[]){ 0x81, 0x02 }, 2));   /* TIMING: 402 ms, 1x */
    g_assert_true(i2c_write(q, 0x49, (const uint8_t[]){ 0x86, 0x10 }, 2));   /* INTERRUPT: level, every one */
    g_assert_false(qtest_readl(q, SYSIC + GPIO_INTSTAT + 2 * 4) & (1u << 9));
    qtest_clock_step(q, 403 * MSEC);
    g_assert_true(i2c_write(q, 0x49, (const uint8_t[]){ 0x8c }, 1));
    i2c_read(q, 0x49, ch, 4);
    g_assert_cmpuint(ch[0] | ch[1] << 8, ==, 871);
    g_assert_cmpuint(ch[2] | ch[3] << 8, ==, 871 / 4);
    g_assert_true(qtest_readl(q, SYSIC + GPIO_INTSTAT + 2 * 4) & (1u << 9));
    qtest_quit(q);
}

/*
 * The ISL29003 as 1.x's AppleISL29003 finds it: ENABLE with a 16-bit conversion (2^16 cycles of the
 * oscillator its 499 kOhm resistor slows to 65.7 kHz, 998 ms) and range 0 (1000 lux at 100 kOhm, about 200
 * at 499): 300 lux is past that range's full scale, so the count reads 0xffff.
 */
static void n45_isl29003(void)
{
    QTestState *q = boot("iPod-Touch-1G");
    uint8_t d[2];

    g_assert_cmpint(lux(q), ==, 300);
    g_assert_true(i2c_write(q, 0x44, (const uint8_t[]){ 0x00, 0x80, 0x00 }, 3));   /* COMMAND, CONTROL */
    qtest_clock_step(q, 1000 * MSEC);
    g_assert_true(i2c_write(q, 0x44, (const uint8_t[]){ 0x04 }, 1));
    i2c_read(q, 0x44, d, 2);
    g_assert_cmphex(d[0] | d[1] << 8, ==, 0xffff);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;

    fd = g_file_open_tmp("s5l8900-als-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("s5l8900-als-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 65536, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("s5l8900-als-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/als/m68-tsl2561", m68_tsl2561);
    qtest_add_func("/ipod/als/n45-isl29003", n45_isl29003);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
