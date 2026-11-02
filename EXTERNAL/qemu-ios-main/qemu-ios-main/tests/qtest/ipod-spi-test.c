/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 SPI controller; no firmware runs. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define SPI1 0x3ce00000ULL       /* nothing attached: every byte clocked in reads 0 */
#define CTRL 0x00
#define CFG 0x04
#define STATUS 0x08
#define RXCNT 0x34
static char *rom, *nor, *nand;

/* RUN with an empty TX FIFO starts a receive-only transfer when CFG asks for
 * sentinel bytes (AGD), as OpeniBoot's driver programs it, instead of waiting
 * for an unrelated STATUS or CFG write. Without AGD it still waits for TX. */
static void receive_only_run(void)
{
    QTestState *q = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                                "-display none -audio driver=none -nic none", rom, nor, nand);
    qtest_writel(q, SPI1 + RXCNT, 4);
    qtest_writel(q, SPI1 + CFG, 0);
    qtest_writel(q, SPI1 + CTRL, 1);
    g_assert_cmpuint(qtest_readl(q, SPI1 + RXCNT), ==, 4);
    qtest_writel(q, SPI1 + CTRL, 0);
    qtest_writel(q, SPI1 + CFG, 1);                 /* AGD, not running yet */
    g_assert_cmpuint(qtest_readl(q, SPI1 + RXCNT), ==, 4);
    qtest_writel(q, SPI1 + CTRL, 1);
    g_assert_cmpuint(qtest_readl(q, SPI1 + RXCNT), ==, 0);
    g_assert_cmpuint(qtest_readl(q, SPI1 + STATUS) >> 11 & 0x1f, ==, 4);
    g_assert_cmphex(qtest_readl(q, SPI1 + STATUS) & (1u << 22), ==, 1u << 22);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-spi-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-spi-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-spi-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/spi/receive-only-run", receive_only_run);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
