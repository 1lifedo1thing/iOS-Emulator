/* Portable qtest adapted from upstream 1b451cc86c77c153fae1d7be2d4f151e693a9d75. */
#ifdef __APPLE__
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot-args on the production N72 board, without guest code: before a NAND
 * read made from iBoot (the CPU sits at the staged iBoot's entry), the
 * snprintf format slot of a synthetic iBoot points at commandLine, which holds
 * the arguments with '%' doubled; with no boot-args, or a read from outside
 * iBoot, nothing is touched.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define IBOOT 0x0ff00000ULL
#define CODE 0x100                  /* the snprintf call */
#define SLOT (IBOOT + 0x144)        /* its format literal */
#define FORMAT (IBOOT + 0x200)      /* "%s " */
#define COMMAND (IBOOT + 0x800)     /* gBootArgs.commandLine */
#define FMSS 0x38a00000ULL
#define RAM 0x08010000ULL
static char *rom, *nor, *nand, *iboot;

/* 7E18's form: ldr r3, =commandLine; ldr r2, ="%s "; adds r0, r3, #0; str r3, [sp];
 * movs r1, #0x80; lsls r1, r1, #1; adds r3, r6, #0; bl */
static void write_iboot(void)
{
    static const uint16_t code[] = { 0x4b0f, 0x4a10, 0x1c18, 0x9300, 0x2180, 0x0049, 0x1c33, 0xf000, 0xf800 };
    g_autofree uint8_t *image = g_malloc0(4096);
    for (size_t i = 0; i < G_N_ELEMENTS(code); i++) {
        image[CODE + 2 * i] = code[i];
        image[CODE + 2 * i + 1] = code[i] >> 8;
    }
    uint32_t literals[2] = { cpu_to_le32(COMMAND), cpu_to_le32(FORMAT) };
    memcpy(image + 0x140, literals, sizeof(literals));
    memcpy(image + (FORMAT - IBOOT), "%s ", 4);
    g_assert_true(g_file_set_contents(iboot, (const char *)image, 4096, NULL));
}

static QTestState *start_board(bool direct, const char *args)
{
    return qtest_initf("-machine 'iPod-Touch,bootrom=%s,direct-iboot=%s,nor=%s,nand=%s,boot-args=%s' "
                       "-display none -audio driver=none -nic none",
                       rom, direct ? iboot : "", nor, nand, args);
}

/* One page read through the FMSS, as iBoot's NAND driver makes it. */
static void nand_read(QTestState *q)
{
    qtest_writel(q, RAM + 0x80, 0);
    qtest_writel(q, RAM + 0x84, 1);
    qtest_writel(q, RAM + 0x1800, RAM + 0x30000);
    qtest_writel(q, RAM + 0x1804, RAM + 0x30800);
    qtest_writel(q, FMSS + 0xd0c, RAM + 0x80);
    qtest_writel(q, FMSS + 0xd10, RAM + 0x84);
    qtest_writel(q, FMSS + 0xd18, 1);
    qtest_writel(q, FMSS + 0xd20, RAM + 0x1800);
    qtest_writel(q, FMSS + 0xd1c, RAM + 0x2800);
    qtest_writel(q, FMSS + 0xd30, 0xa01);
    qtest_writel(q, FMSS + 0xd38, 1);
}

static void expect_command(QTestState *q, const char *want)
{
    char command[256];
    qtest_memread(q, COMMAND, command, sizeof(command));
    g_assert_cmpstr(command, ==, want);
    for (size_t i = strlen(want); i < sizeof(command); i++) {
        g_assert_cmpint(command[i], ==, 0);
    }
}

static void handed_over(void)
{
    QTestState *q = start_board(true, "serial=3 -v 100%");
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, COMMAND);
    expect_command(q, "serial=3 -v 100%%");
    /* A reset stages iBoot again, which clears its BSS when it starts: the next read fills both again. */
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    qtest_writeb(q, COMMAND, 0);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, COMMAND);
    expect_command(q, "serial=3 -v 100%%");
    qtest_quit(q);
}

static void disabled(void)
{
    QTestState *q = start_board(true, "");
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    expect_command(q, "");
    qtest_quit(q);
}

/* The bootrom's reset vector, not iBoot, is running: an iBoot image in RAM is left alone. */
static void not_from_iboot(void)
{
    g_autofree char *image = NULL;
    gsize len;
    QTestState *q = start_board(false, "serial=3");
    g_assert_true(g_file_get_contents(iboot, &image, &len, NULL));
    qtest_memwrite(q, IBOOT, image, len);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    expect_command(q, "");
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    int fd, result;
    g_autofree char *zero = g_malloc0(1048576);
    fd = g_file_open_tmp("n72-args-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    fd = g_file_open_tmp("n72-args-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    fd = g_file_open_tmp("n72-args-iboot-XXXXXX", &iboot, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    write_iboot();
    nand = g_dir_make_tmp("n72-args-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/boot-args/handed-over", handed_over);
    qtest_add_func("/ipod/boot-args/disabled", disabled);
    qtest_add_func("/ipod/boot-args/not-from-iboot", not_from_iboot);
    result = g_test_run();
    unlink(rom); unlink(nor); unlink(iboot); rmdir(nand);
    g_free(rom); g_free(nor); g_free(iboot); g_free(nand);
    return result;
}

#else
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot-args on the production N72 board, without guest code: before a NAND
 * read made from iBoot (the CPU sits at the staged iBoot's entry), the
 * snprintf format slot of a synthetic iBoot points at commandLine, which holds
 * the arguments with '%' doubled; with no boot-args, or a read from outside
 * iBoot, nothing is touched.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define IBOOT 0x0ff00000ULL
#define CODE 0x100                  /* the snprintf call */
#define SLOT (IBOOT + 0x144)        /* its format literal */
#define FORMAT (IBOOT + 0x200)      /* "%s " */
#define COMMAND (IBOOT + 0x800)     /* gBootArgs.commandLine */
#define FMSS 0x38a00000ULL
#define RAM 0x08010000ULL
static char *rom, *nor, *nand, *iboot;

/* 7E18's form: ldr r3, =commandLine; ldr r2, ="%s "; adds r0, r3, #0; str r3, [sp];
 * movs r1, #0x80; lsls r1, r1, #1; adds r3, r6, #0; bl */
static void write_iboot(void)
{
    static const uint16_t code[] = { 0x4b0f, 0x4a10, 0x1c18, 0x9300, 0x2180, 0x0049, 0x1c33, 0xf000, 0xf800 };
    g_autofree uint8_t *image = g_malloc0(4096);
    for (size_t i = 0; i < G_N_ELEMENTS(code); i++) {
        image[CODE + 2 * i] = code[i];
        image[CODE + 2 * i + 1] = code[i] >> 8;
    }
    uint32_t literals[2] = { cpu_to_le32(COMMAND), cpu_to_le32(FORMAT) };
    memcpy(image + 0x140, literals, sizeof(literals));
    memcpy(image + (FORMAT - IBOOT), "%s ", 4);
    g_assert_true(g_file_set_contents(iboot, (const char *)image, 4096, NULL));
}

static QTestState *start_board(bool direct, const char *args)
{
    return qtest_initf("-machine 'iPod-Touch,bootrom=%s,direct-iboot=%s,nor=%s,nand=%s,boot-args=%s' "
                       "-display none -audio driver=none -nic none",
                       rom, direct ? iboot : "", nor, nand, args);
}

/* One page read through the FMSS, as iBoot's NAND driver makes it. */
static void nand_read(QTestState *q)
{
    qtest_writel(q, RAM + 0x80, 0);
    qtest_writel(q, RAM + 0x84, 1);
    qtest_writel(q, RAM + 0x1800, RAM + 0x30000);
    qtest_writel(q, RAM + 0x1804, RAM + 0x30800);
    qtest_writel(q, FMSS + 0xd0c, RAM + 0x80);
    qtest_writel(q, FMSS + 0xd10, RAM + 0x84);
    qtest_writel(q, FMSS + 0xd18, 1);
    qtest_writel(q, FMSS + 0xd20, RAM + 0x1800);
    qtest_writel(q, FMSS + 0xd1c, RAM + 0x2800);
    qtest_writel(q, FMSS + 0xd30, 0xa01);
    qtest_writel(q, FMSS + 0xd38, 1);
}

static void expect_command(QTestState *q, const char *want)
{
    char command[256];
    qtest_memread(q, COMMAND, command, sizeof(command));
    g_assert_cmpstr(command, ==, want);
    for (size_t i = strlen(want); i < sizeof(command); i++) {
        g_assert_cmpint(command[i], ==, 0);
    }
}

static void handed_over(void)
{
    QTestState *q = start_board(true, "serial=3 -v 100%");
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, COMMAND);
    expect_command(q, "serial=3 -v 100%%");
    /* A reset stages iBoot again, which clears its BSS when it starts: the next read fills both again. */
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    qtest_writeb(q, COMMAND, 0);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, COMMAND);
    expect_command(q, "serial=3 -v 100%%");
    qtest_quit(q);
}

static void disabled(void)
{
    QTestState *q = start_board(true, "");
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    expect_command(q, "");
    qtest_quit(q);
}

/* The bootrom's reset vector, not iBoot, is running: an iBoot image in RAM is left alone. */
static void not_from_iboot(void)
{
    g_autofree char *image = NULL;
    gsize len;
    QTestState *q = start_board(false, "serial=3");
    g_assert_true(g_file_get_contents(iboot, &image, &len, NULL));
    qtest_memwrite(q, IBOOT, image, len);
    nand_read(q);
    g_assert_cmphex(qtest_readl(q, SLOT), ==, FORMAT);
    expect_command(q, "");
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    int fd, result;
    g_autofree char *zero = g_malloc0(1048576);
    fd = g_file_open_tmp("n72-args-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    fd = g_file_open_tmp("n72-args-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    fd = g_file_open_tmp("n72-args-iboot-XXXXXX", &iboot, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    write_iboot();
    nand = g_dir_make_tmp("n72-args-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/boot-args/handed-over", handed_over);
    qtest_add_func("/ipod/boot-args/disabled", disabled);
    qtest_add_func("/ipod/boot-args/not-from-iboot", not_from_iboot);
    result = g_test_run();
    unlink(rom); unlink(nor); unlink(iboot); rmdir(nand);
    g_free(rom); g_free(nor); g_free(iboot); g_free(nand);
    return result;
}

#endif
