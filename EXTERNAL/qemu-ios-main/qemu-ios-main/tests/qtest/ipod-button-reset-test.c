/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 buttons across a reset of the SoC; no firmware runs. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define PAD_C 0x3cf00184ULL      /* GPIO pad 0xc: voldown 0, home 1, power 2 */
#define POWER (1u << 2)
static char *rom, *nor, *nand;

static void key(QTestState *q, const char *qcode, bool down)
{
    qtest_qmp_assert_success(q, "{ 'execute': 'input-send-event', 'arguments': { 'events': ["
                             "{ 'type': 'key', 'data': { 'down': %i, 'key': "
                             "{ 'type': 'qcode', 'data': %s } } }] } }", down, qcode);
}

/* Pressing a button on a hibernating AP wakes it with a reset of the SoC,
 * while the button is still down. The GPIO reset clears the guest's pad
 * state but not a level driven from outside, so the woken guest reads the
 * button held, and its release is an edge. */
static void held_through_reset(void)
{
    QTestState *q = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                                "-display none -audio driver=none -nic none", rom, nor, nand);
    key(q, "meta_l", true);
    key(q, "l", true);                              /* Command+L: power */
    g_assert_cmphex(qtest_readl(q, PAD_C) & POWER, ==, POWER);
    qtest_qmp_assert_success(q, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(q, "RESET");
    g_assert_cmphex(qtest_readl(q, PAD_C) & POWER, ==, POWER);
    key(q, "l", false);
    g_assert_cmphex(qtest_readl(q, PAD_C) & POWER, ==, 0);
    /* released: the next reset leaves it at rest */
    qtest_qmp_assert_success(q, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(q, "RESET");
    g_assert_cmphex(qtest_readl(q, PAD_C) & POWER, ==, 0);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-btn-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-btn-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-btn-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/buttons/held-through-reset", held_through_reset);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
