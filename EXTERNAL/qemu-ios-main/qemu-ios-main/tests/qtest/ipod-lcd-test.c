/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 RGB1 control readback on the actual board; no firmware executes. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define LCD 0x38900000ULL
#define CONTROL (LCD + 0x40)
static char *rom, *rom45, *nor, *nand;

static QTestState *start_board(const char *option, bool incoming)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s%s "
                       "%s -display none -audio driver=none -nic none",
                       rom, nor, nand, option,
                       incoming ? "-incoming defer" : "");
}

static void assert_property(QTestState *q, bool enabled)
{
    QDict *reply = qtest_qmp(q, "{'execute':'qom-get','arguments':"
                               "{'path':'/machine','property':'lcd-planes'}}");
    g_assert_cmpint(qdict_get_bool(reply, "return"), ==, enabled);
    qobject_unref(reply);
}

static void readback(QTestState *q, bool option)
{
    uint32_t values[] = {0x00310700, 0, 1, 0x80000000, 0xffffffff, 0xdeadbeef};
    assert_property(q, option);
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        qtest_writel(q, CONTROL, values[i]);
        g_assert_cmphex(qtest_readl(q, CONTROL), ==, values[i]);
    }
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        /* Stock 5F138: geometry programming followed by rotation RMW. */
        qtest_writel(q, CONTROL, 0x00310700);
        qtest_writel(q, LCD + 0x48, 320);
        qtest_writel(q, LCD + 0x44, 0x0f496000);
        qtest_writel(q, LCD + 0x50, 0x014001e0);
        qtest_writel(q, LCD + 0x54, 0);
        qtest_writel(q, LCD + 0x4c, 0);
        uint32_t control = qtest_readl(q, CONTROL);
        qtest_writel(q, CONTROL, control | (rotation << 22));
        g_assert_cmphex(qtest_readl(q, CONTROL), ==,
                        0x00310700 | (rotation << 22));
    }
    /* Only control becomes independently readable; pitch keeps its option. */
    g_assert_cmphex(qtest_readl(q, LCD + 0x48), ==, option ? 320 : 0);
    qtest_writel(q, LCD + 0x20, 0x00200700);
    g_assert_cmphex(qtest_readl(q, LCD + 0x20), ==, 0x00200700);
}

static void default_readback(void)
{
    QTestState *q = start_board("", false);
    readback(q, true);
    qtest_quit(q);
}

static void option_readback(void)
{
    QTestState *q = start_board(",lcd-planes=off", false);
    readback(q, false);
    qtest_quit(q);
}

static void reset_control(void)
{
    QTestState *q = start_board("", false);
    qtest_writel(q, CONTROL, 0xdeadbeef);
    qtest_writel(q, LCD + 0x20, 0x00200700);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(q, LCD + 0x20), ==, 0);
    qtest_quit(q);
}

static void wait_migration(QTestState *q)
{
    for (unsigned i = 0; i < 5000; i++) {
        QDict *reply = qtest_qmp(q, "{'execute':'query-migrate'}");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"),
                                              "status");
        bool complete = status && !strcmp(status, "completed");
        g_assert_false(status && !strcmp(status, "failed"));
        qobject_unref(reply);
        if (complete) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void restored_control(void)
{
    for (unsigned option = 0; option < 2; option++) {
        const char *setting = option ? ",lcd-planes=on" : ",lcd-planes=off";
        g_autofree char *state = NULL;
        g_autofree char *uri = NULL;
        int fd = g_file_open_tmp("n72-lcd-state-XXXXXX", &state, NULL);
        QTestState *from = start_board(setting, false);
        QTestState *to;

        g_assert_cmpint(fd, >=, 0);
        close(fd);
        uri = g_strdup_printf("file:%s", state);
        qtest_writel(from, CONTROL, 0x00f10700);
        qtest_writel(from, LCD + 0x48, 320);
        qtest_writel(from, LCD + 0x20, 0x00200700);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        wait_migration(from);
        qtest_quit(from);
        to = start_board(setting, true);
        qtest_writel(to, CONTROL, 0xdeadbeef);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        wait_migration(to);
        assert_property(to, option);
        g_assert_cmphex(qtest_readl(to, CONTROL), ==, 0x00f10700);
        g_assert_cmphex(qtest_readl(to, LCD + 0x20), ==, 0x00200700);
        g_assert_cmphex(qtest_readl(to, LCD + 0x48), ==, option ? 320 : 0);
        qtest_quit(to);
        unlink(state);
    }
}

static void n45_control(void)
{
    QTestState *q = qtest_initf(
        "-machine iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,wifi=off "
        "-drive if=pflash,format=raw,file=%s -display none -audio driver=none",
        rom45, rom45, nand, nor);
    qtest_writel(q, CONTROL, 0x12345678);
    g_assert_cmphex(qtest_readl(q, CONTROL), ==, 0x12345678);
    qtest_writel(q, LCD + 0x5c, 0x700);
    qtest_writel(q, LCD + 0x60, 0x08500000);
    qtest_writel(q, LCD + 0x64, 0x014001e0);
    qtest_writel(q, LCD + 0x68, 320);
    g_assert_cmphex(qtest_readl(q, LCD + 0x5c), ==, 0x700);
    g_assert_cmphex(qtest_readl(q, LCD + 0x60), ==, 0x08500000);
    g_assert_cmphex(qtest_readl(q, LCD + 0x64), ==, 0x014001e0);
    g_assert_cmphex(qtest_readl(q, LCD + 0x68), ==, 320);
    qtest_quit(q);
}

/* The pixel at (x, y) of a QMP screendump (PPM P6), as 0xRRGGBB. */
static uint32_t screen_pixel(QTestState *q, unsigned x, unsigned y)
{
    g_autofree char *path = NULL, *data = NULL;
    gsize len;
    unsigned w, h, max;
    int n = 0, fd = g_file_open_tmp("n72-lcd-shot-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qtest_qmp_assert_success(q, "{'execute':'screendump','arguments':{'filename':%s}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    unlink(path);
    g_assert_cmpint(sscanf(data, "P6 %u %u %u%n", &w, &h, &max, &n), ==, 3);
    g_assert_true(x < w && y < h && max == 255);
    g_assert_cmpuint(n + 1 + (gsize)w * h * 3, <=, len);
    const uint8_t *p = (const uint8_t *)data + n + 1 + ((gsize)y * w + x) * 3;
    return p[0] << 16 | p[1] << 8 | p[2];
}

/*
 * A page committed (+0x4 bit 4) again while the panel shows it is drawn in place: the panel shows the frame as
 * committed, not the next one the guest has started in the same page (issue 98: 3.0's software CoreAnimation with one
 * page showed cleared and half-drawn frames in every animation). A commit of another page scans out live.
 */
static void committed_frame(void)
{
    const uint64_t fb = 0x0f496000, other = 0x0f600000, size = 320 * 480 * 4;
    QTestState *q = start_board("", false);

    qtest_writel(q, LCD + 0x20, 0x00200700);
    qtest_writel(q, LCD + 0x28, 320);
    qtest_writel(q, LCD + 0x30, 0x014001e0);
    qtest_writel(q, LCD + 0x24, fb);
    qtest_memset(q, fb, 0x40, size);            /* frame 1, grey */
    qtest_writel(q, LCD + 0x4, 0x10);
    qtest_clock_step(q, 20 * 1000 * 1000);      /* a frame interrupt: the panel takes the window */
    qtest_writel(q, LCD + 0x24, fb);
    qtest_writel(q, LCD + 0x4, 0x10);           /* the same page committed again */
    g_assert_cmphex(screen_pixel(q, 160, 240), ==, 0x404040);
    qtest_memset(q, fb, 0x00, size / 2);        /* the next frame begun in place: cleared, half drawn */
    qtest_memset(q, fb, 0xc0, size / 4);
    qtest_clock_step(q, 20 * 1000 * 1000);
    g_assert_cmphex(screen_pixel(q, 160, 100), ==, 0x404040);
    g_assert_cmphex(screen_pixel(q, 160, 200), ==, 0x404040);
    qtest_writel(q, LCD + 0x4, 0x10);           /* ... and committed */
    g_assert_cmphex(screen_pixel(q, 160, 100), ==, 0xc0c0c0);
    g_assert_cmphex(screen_pixel(q, 160, 200), ==, 0x000000);
    /* Page flipping: another page committed is scanned out as it is. */
    qtest_memset(q, other, 0x80, size);
    qtest_writel(q, LCD + 0x24, other);
    qtest_writel(q, LCD + 0x4, 0x10);
    qtest_clock_step(q, 20 * 1000 * 1000);
    qtest_memset(q, other, 0x20, size);
    g_assert_cmphex(screen_pixel(q, 160, 240), ==, 0x202020);
    qtest_quit(q);
}

/*
 * The panel scans out as is whatever way the device is held, as on hardware: iOS turns its UI inside its portrait
 * framebuffer and the host turns the picture with the device it shows. Turning the accelerometer to landscape
 * (UIDeviceOrientation 4) leaves the console 320x480 and its top rows on top; it used to turn the console to 480x320
 * and the picture with it.
 */
static void scanout_fixed(void)
{
    const uint64_t fb = 0x0f496000, size = 320 * 480 * 4;
    QTestState *q = start_board("", false);

    qtest_writel(q, LCD + 0x20, 0x00200700);
    qtest_writel(q, LCD + 0x28, 320);
    qtest_writel(q, LCD + 0x30, 0x014001e0);
    qtest_writel(q, LCD + 0x24, fb);
    qtest_memset(q, fb, 0x00, size);
    qtest_memset(q, fb, 0xff, 320 * 40 * 4);    /* the top 40 rows white */
    qtest_qmp_assert_success(q, "{'execute':'qom-set','arguments':"
                                "{'path':'/machine','property':'accel-orientation','value':4}}");
    qtest_clock_step(q, 20 * 1000 * 1000);
    g_assert_cmphex(screen_pixel(q, 300, 20), ==, 0xffffff);
    g_assert_cmphex(screen_pixel(q, 160, 400), ==, 0x000000);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-lcd-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    fd = g_file_open_tmp("n72-lcd-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    fd = g_file_open_tmp("n45-lcd-rom-XXXXXX", &rom45, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom45, zero, 65536, NULL));
    nand = g_dir_make_tmp("n72-lcd-nand-XXXXXX", NULL);
    g_setenv("IT_LCD_BRIGHT", "255", true);     /* the screendumps at full backlight, whatever the PMU says */
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/lcd/default-control-readback-rmw", default_readback);
    qtest_add_func("/ipod/lcd/option-control-readback-rmw", option_readback);
    qtest_add_func("/ipod/lcd/control-reset", reset_control);
    qtest_add_func("/ipod/lcd/control-restored", restored_control);
    qtest_add_func("/ipod/lcd/committed-frame", committed_frame);
    qtest_add_func("/ipod/lcd/scanout-fixed", scanout_fixed);
    if (qtest_has_machine("iPod-Touch-1G")) {
        qtest_add_func("/ipod/lcd/n45-mapped-control", n45_control);
    }
    result = g_test_run();
    unlink(rom);
    unlink(rom45);
    unlink(nor);
    rmdir(nand);
    g_free(rom);
    g_free(rom45);
    g_free(nor);
    g_free(nand);
    return result;
}
