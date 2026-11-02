/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The N72's MBX on the production board, with no guest code: interrupts, the MMU handshake and page-table walk, the
 * 2D core's fills, copies and blends from its slave-port stream, a 3D blit, reset and migration.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/bswap.h"

#define MBX 0x3b000000ULL
#define STATUS (MBX + 0x12c)
#define MASK (MBX + 0x130)
#define ACK (MBX + 0x134)
#define MMU_DIR (MBX + 0x1000)
#define MMU_CTRL (MBX + 0x1020)
#define PORT_2D (MBX + 0xa00000)
#define VIC_RAW 0x38e01008ULL
#define MBX_IRQ (1u << 21) /* N72 IRQ 0x35, second VIC input 21 */
#define DONE_2D 0x400
#define DONE_3D 0x4c
static char *rom, *nor, *nand;

static QTestState *start_board(bool incoming)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s %s -display none -audio driver=none -nic none",
                       rom, nor, nand, incoming ? "-incoming defer" : "");
}

static void irq(QTestState *q, bool high)
{
    g_assert_cmphex(qtest_readl(q, VIC_RAW) & MBX_IRQ, ==, high ? MBX_IRQ : 0);
}

static void stream(QTestState *q, const uint32_t *words, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        qtest_writel(q, PORT_2D + i * 4, words[i]);
    }
}

/* STATUS holds what happened, the line follows STATUS & MASK, CLEAR is write-1-to-clear. */
static void interrupts(void)
{
    QTestState *q = start_board(false);
    qtest_writel(q, STATUS, 0x408);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x408);
    qtest_writel(q, MASK, 0x400);
    irq(q, true);
    g_assert_cmphex(qtest_readl(q, MASK), ==, 0x400);
    qtest_writel(q, ACK, 0x400);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 8);
    qtest_writel(q, MASK, 8);
    irq(q, true);
    qtest_writel(q, ACK, 8);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    /* The kernel spins on these after kicking them: region and parameter-buffer setups finish at once. */
    qtest_writel(q, MBX + 0x814, 1);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x100);
    qtest_writel(q, MBX + 0x838, 1);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x140);
    /* 1.x's reset path starts a region with a 0 write and spins on 0x100. */
    qtest_writel(q, ACK, 0x140);
    qtest_writel(q, MBX + 0x85c, 0x80);
    qtest_writel(q, MBX + 0x81c, 0);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x100);
    qtest_quit(q);
}

/*
 * The full-screen opaque-black fill 3.1.3's SpringBoard submits (captured from 7E18): ARGB8888 320x480 at GPU address
 * 0x823000, stride 1280, mapped through directory entry 2 onto 150 pages that are not contiguous in RAM.
 */
static const uint32_t captured_fill[] = {
    0xa0060500, 0x823000, 0x94060500, 0, 0x30000000, 0x60800200, 0x8000f0f0, 0xff000000, 0, 0x014001e0,
    0x70000000, 0x70000000,
};
#define TABLE 0x08100000
#define PAGES 0x08200000

static void map_screen(QTestState *q)
{
    qtest_memset(q, TABLE, 0, 0x1000);
    for (unsigned i = 0; i < 150; i++) {
        qtest_writel(q, TABLE + (0x23 + i) * 4, PAGES + i * 8192);
        qtest_memset(q, PAGES + i * 8192, 0x5a, 8192);
    }
    qtest_writel(q, MMU_DIR + 2 * 4, TABLE);
    g_assert_cmphex(qtest_readl(q, MMU_CTRL), ==, 0);
    qtest_writel(q, MMU_CTRL, 1);
    g_assert_cmphex(qtest_readl(q, MMU_CTRL), ==, 0x10001);
}

static void screen_filled(QTestState *q)
{
    uint8_t bytes[8192];
    for (unsigned i = 0; i < 150; i++) {
        qtest_memread(q, PAGES + i * 8192, bytes, sizeof bytes);
        for (unsigned j = 0; j < 4096; j += 4) {
            g_assert_cmphex((uint32_t)ldl_le_p(bytes + j), ==, 0xff000000u);
        }
        for (unsigned j = 4096; j < sizeof bytes; j++) {
            g_assert_cmphex(bytes[j], ==, 0x5a);
        }
    }
}

static void flushed(QTestState *q)
{
    qtest_writel(q, PORT_2D, 0xf0000000);
    g_assert_cmphex(qtest_readl(q, STATUS) & DONE_2D, ==, 0);
    qtest_clock_step(q, 20 * 1000);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, DONE_2D);
    irq(q, true);
    qtest_writel(q, ACK, DONE_2D);
    irq(q, false);
}

static void fill_through_mmu(void)
{
    QTestState *q = start_board(false);
    map_screen(q);
    qtest_writel(q, MASK, DONE_2D);
    stream(q, captured_fill, 9);
    g_assert_cmphex(qtest_readl(q, PAGES), ==, 0x5a5a5a5a); /* the blit runs once its block is complete */
    stream(q, captured_fill + 9, G_N_ELEMENTS(captured_fill) - 9);
    screen_filled(q);
    irq(q, false);
    flushed(q);
    /* The MMU disables with its handshake bit, and then GPU addresses are physical. */
    qtest_writel(q, MMU_CTRL, 0);
    g_assert_cmphex(qtest_readl(q, MMU_CTRL), ==, 0);
    qtest_quit(q);
}

#define SRC 0x08300000
#define DST 0x08310000

/* Copies with the MMU off: RGB565 to ARGB8888, straight and turned 180 degrees, then a blended fill. */
static void copy_rotate_blend(void)
{
    static const uint16_t src[8] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0x8410, 0xf81f, 0x07ff };
    static const uint32_t argb[8] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff000000, 0xff848284, 0xffff00ff, 0xff00ffff,
    };
    uint32_t setup[] = { 0x90050008, SRC, 0xa0060010, DST, 0x30000000 };
    uint32_t copy[] = { 0x8000cccc, 0, 0, 0x00040002 };
    uint32_t turned[] = { 0x8400cccc, 0, 0, 0x00040002 };
    uint32_t blend[] = { 0x20000004, 0xff000, 0x8002f0f0, 0x80800000, 0x00010000, 0x00030001 };
    QTestState *q = start_board(false);

    for (unsigned i = 0; i < 8; i++) {
        qtest_writew(q, SRC + i * 2, src[i]);
    }
    qtest_memset(q, DST, 0, 32);
    stream(q, setup, G_N_ELEMENTS(setup));
    stream(q, copy, G_N_ELEMENTS(copy));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, DST + i * 4), ==, argb[i]);
    }
    stream(q, turned, G_N_ELEMENTS(turned));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, DST + i * 4), ==, argb[7 - i]);
    }
    /* Premultiplied 50% red over the second and third pixels of the top row. */
    stream(q, blend, G_N_ELEMENTS(blend));
    g_assert_cmphex(qtest_readl(q, DST + 0), ==, argb[7]);
    g_assert_cmphex(qtest_readl(q, DST + 4), ==, 0xffff007f);   /* over 0xffff00ff */
    g_assert_cmphex(qtest_readl(q, DST + 8), ==, 0xffc14041);   /* over 0xff848284 */
    g_assert_cmphex(qtest_readl(q, DST + 12), ==, argb[4]);
    qtest_quit(q);
}

static void put_float(QTestState *q, uint64_t addr, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    qtest_writel(q, addr, u);
}

/*
 * A 3D blit as AppleMBX sets one up, in GPU addresses the MMU maps onto RAM at 0x08400000: every region points at one
 * object list, which holds a depth-only quad over the whole target with ISP bit 12 set (all visible) and an
 * untextured quad over x 2..6, y 1..3 of an 8x4 RGB565 target.
 */
#define GPU_RAM 0x08400000
static void blit_3d(void)
{
    const uint32_t target = 0, objects = 0x10000, regions = 0x20000, list = 0x30000;
    const float quad[4][2] = { { 2, 1 }, { 6, 1 }, { 2, 3 }, { 6, 3 } };
    const float whole[4][2] = { { 0, 0 }, { 8, 0 }, { 0, 4 }, { 8, 4 } };
    QTestState *q = start_board(false);
    uint64_t va;

    for (unsigned i = 0; i < 0x40; i++) {
        qtest_writel(q, TABLE + i * 4, GPU_RAM + i * 0x1000);
    }
    qtest_writel(q, MMU_DIR, TABLE);
    qtest_writel(q, MMU_CTRL, 1);
    qtest_memset(q, GPU_RAM + target, 0, 64);
    qtest_writel(q, GPU_RAM + regions + 4, list);
    qtest_writel(q, GPU_RAM + list, 0x00200000);          /* layout 0x02 (depth only) at word 0 */
    qtest_writel(q, GPU_RAM + list + 4, 0x00a00000 | 16); /* layout 0x0a (no textures) at word 16 */
    qtest_writel(q, GPU_RAM + list + 8, 0xf0000000);
    va = GPU_RAM + objects;
    qtest_writel(q, va, 1 << 12);
    for (unsigned i = 0; i < 4; i++) {
        put_float(q, va + 4 + i * 8, whole[i][0]);
        put_float(q, va + 8 + i * 8, whole[i][1]);
    }
    va = GPU_RAM + objects + 16 * 4 + 4;                  /* past the header word: the ISP word */
    qtest_writel(q, va, 0);
    for (unsigned i = 0; i < 4; i++) {
        put_float(q, va + 4 + i * 8, quad[i][0]);
        put_float(q, va + 8 + i * 8, quad[i][1]);
        qtest_writel(q, va + 4 + 64 + i * 4, 0xff00ff00);  /* per-vertex color, after x/y, z and 1/w */
    }
    qtest_writel(q, MBX + 0x608, regions);
    qtest_writel(q, MBX + 0x60c, objects);
    qtest_writel(q, MBX + 0x650, 1);                      /* RGB565 */
    qtest_writel(q, MBX + 0x654, 8 << 16);
    qtest_writel(q, MBX + 0x658, 4 << 16);
    qtest_writel(q, MBX + 0x65c, target);
    qtest_writel(q, MBX + 0x660, 4);                      /* stride in words */
    qtest_writel(q, MASK, DONE_3D);
    qtest_writel(q, MBX + 0x680, 1);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 8; x++) {
            bool in = x >= 2 && x < 6 && y >= 1 && y < 3;
            g_assert_cmphex(qtest_readw(q, GPU_RAM + target + y * 16 + x * 2), ==, in ? 0x07e0 : 0);
        }
    }
    irq(q, false);
    qtest_clock_step(q, 100 * 1000);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, DONE_3D);
    irq(q, true);
    qtest_quit(q);
}

static void wait_migration(QTestState *q)
{
    for (unsigned i = 0; i < 5000; i++) {
        QDict *reply = qtest_qmp(q, "{'execute':'query-migrate'}");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        bool done = status && !strcmp(status, "completed");
        g_assert_false(status && !strcmp(status, "failed"));
        qobject_unref(reply);
        if (done) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static QTestState *migrated(QTestState *from)
{
    g_autofree char *state = NULL;
    int fd = g_file_open_tmp("n72-mbx-state-XXXXXX", &state, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    qtest_quit(from);
    QTestState *to = start_board(true);
    qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(to);
    unlink(state);
    return to;
}

/* A block half-written to the slave port, the MMU and a completion still pending all survive a save and restore. */
static void restored(void)
{
    QTestState *q = start_board(false);
    map_screen(q);
    qtest_writel(q, MASK, DONE_2D);
    stream(q, captured_fill, 8);
    q = migrated(q);
    g_assert_cmphex(qtest_readl(q, MMU_CTRL), ==, 0x10001);
    g_assert_cmphex(qtest_readl(q, MASK), ==, DONE_2D);
    stream(q, captured_fill + 8, 2);
    screen_filled(q);
    qtest_writel(q, PORT_2D, 0xf0000000);
    q = migrated(q);
    irq(q, false);
    qtest_clock_step(q, 20 * 1000);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, DONE_2D);
    irq(q, true);
    qtest_quit(q);
}

static void reset(void)
{
    QTestState *q = start_board(false);
    map_screen(q);
    qtest_writel(q, MASK, DONE_2D);
    qtest_writel(q, STATUS, DONE_2D);
    irq(q, true);
    stream(q, captured_fill, 8);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, MASK), ==, 0);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(q, MMU_CTRL), ==, 0);
    g_assert_cmphex(qtest_readl(q, MMU_DIR + 8), ==, 0);
    /* The half-written block went with the reset: these two words start no blit. */
    stream(q, captured_fill + 8, 2);
    g_assert_cmphex(qtest_readl(q, PAGES), ==, 0x5a5a5a5a);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd = g_file_open_tmp("n72-mbx-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    fd = g_file_open_tmp("n72-mbx-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-mbx-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/mbx/interrupts", interrupts);
    qtest_add_func("/ipod/mbx/fill-through-mmu", fill_through_mmu);
    qtest_add_func("/ipod/mbx/copy-rotate-blend", copy_rotate_blend);
    qtest_add_func("/ipod/mbx/blit-3d", blit_3d);
    qtest_add_func("/ipod/mbx/restored", restored);
    qtest_add_func("/ipod/mbx/reset", reset);
    int result = g_test_run();
    unlink(rom);
    unlink(nor);
    rmdir(nand);
    g_free(rom);
    g_free(nor);
    g_free(nand);
    return result;
}
