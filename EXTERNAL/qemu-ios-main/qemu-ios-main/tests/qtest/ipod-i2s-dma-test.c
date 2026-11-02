/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 audio path: a PL080 memory->I2S TX FIFO descriptor, paced by the I2S's
 * request line, delivers every byte in order and completes; no firmware runs. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define RAM   0x08000000ULL
#define DMAC0 0x38200000ULL
#define I2S0  0x3ca00000ULL
#define I2S_REQ 10
#define ELEMENTS 4000            /* 16000 bytes: several FIFO refills */
static char *rom, *nor, *nand, *dump;

static void paced_descriptor(void)
{
    g_autofree uint8_t *pattern = g_malloc(ELEMENTS * 4);
    g_autofree char *got = NULL;
    gsize len = 0;
    QTestState *q;

    for (unsigned i = 0; i < ELEMENTS * 4; i++) {
        pattern[i] = i * 7 + (i >> 8);
    }
    g_setenv("IT_I2S_DUMP", dump, true);
    q = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                    "-display none -audio driver=none -nic none", rom, nor, nand);
    g_unsetenv("IT_I2S_DUMP");
    qtest_memwrite(q, RAM + 0x100000, pattern, ELEMENTS * 4);
    qtest_writel(q, I2S0 + 0x00, 1);                /* ENABLE: the FIFO drains */
    qtest_writel(q, I2S0 + 0x08, 6);                /* TXCOM: run */
    qtest_writel(q, DMAC0 + 0x30, 1);               /* controller on */
    qtest_writel(q, DMAC0 + 0x100, RAM + 0x100000); /* channel 0 */
    qtest_writel(q, DMAC0 + 0x104, I2S0 + 0x10);    /* TX FIFO */
    qtest_writel(q, DMAC0 + 0x108, 0);
    qtest_writel(q, DMAC0 + 0x10c, (1u << 31) | (1u << 26) | (2u << 21) | (2u << 18) | ELEMENTS);
    qtest_writel(q, DMAC0 + 0x110, 1 | (I2S_REQ << 6) | (1u << 11) | (1u << 15));
    /* Paced: the descriptor is not drained in zero time. */
    g_assert_cmpuint(qtest_readl(q, DMAC0 + 0x10c) & 0xfff, >, 0);
    qtest_clock_step(q, 500 * 1000 * 1000ULL);
    g_assert_cmpuint(qtest_readl(q, DMAC0 + 0x10c) & 0xfff, ==, 0);
    g_assert_cmpuint(qtest_readl(q, DMAC0 + 0x110) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(q, DMAC0 + 0x14) & 1, ==, 1);   /* RawIntTCStatus */
    qtest_writel(q, I2S0 + 0x08, 0);                /* halt flushes the dump */
    qtest_quit(q);
    g_assert_true(g_file_get_contents(dump, &got, &len, NULL));
    g_assert_cmpuint(len, ==, ELEMENTS * 4);
    g_assert_true(memcmp(got, pattern, len) == 0);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-i2s-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-i2s-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-i2s-pcm-XXXXXX", &dump, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-i2s-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/i2s/paced-descriptor", paced_descriptor);
    result = g_test_run();
    unlink(rom); unlink(nor); unlink(dump); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand); g_free(dump);
    return result;
}
