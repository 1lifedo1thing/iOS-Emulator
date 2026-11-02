/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The kboot boards' Bluetooth controller answers its HCI on the UART the board's DT hangs it from: the N18's
 * BCM4329 on uart1 (bluetooth,n18), the N88's BCM4325 on uart3. With nothing on the N18's port, 3.1.1's BlueTool
 * never got its HCI_Reset answered, BTServer could not come up, and SpringBoard's status bar waited out two 5 s
 * BTSessionAttach timeouts on every turn of the interface (LightTouchMac `sessions rotation` on n18ap 7C146).
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define UART(n) (0x82500000ULL + (n) * 0x100000)
#define MSEC 1000000LL

static char *kboot;

typedef struct Board {
    const char *machine;
    int uart;
    uint8_t version[8];     /* Read Local Version's return parameters: the board's chip's */
} Board;

/* One H4 command, then its Command Complete: status 0 and `count` return parameters. */
static void exchange(const Board *b, const uint8_t *cmd, size_t cmd_len, const uint8_t *params, size_t count)
{
    const uint64_t u = UART(b->uart);
    const uint8_t evt[] = { 4, 0x0e, 4 + count, 1, cmd[1], cmd[2], 0 };
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                b->machine, kboot);

    qtest_writel(q, u + 4, 1);                                          /* UCON: polled receive, no DMA */
    qtest_writel(q, u + 8, 7);                                          /* UFCON: FIFOs on, both cleared */
    for (size_t i = 0; i < cmd_len; i++) {
        qtest_writeb(q, u + 0x20, cmd[i]);                              /* UTXH */
    }
    qtest_clock_step(q, 10 * MSEC);                                     /* past the controller's latency */
    g_assert_cmpuint(qtest_readl(q, u + 0x18) & 255, ==, sizeof(evt) + count);  /* UFSTAT: received bytes */
    for (size_t i = 0; i < sizeof(evt); i++) {
        g_assert_cmphex(qtest_readb(q, u + 0x24), ==, evt[i]);          /* URXH */
    }
    for (size_t i = 0; i < count; i++) {
        g_assert_cmphex(qtest_readb(q, u + 0x24), ==, params[i]);
    }
    qtest_quit(q);
}

static void hci_reset(const void *data)
{
    const uint8_t cmd[] = { 1, 0x03, 0x0c, 0 };                         /* H4 command: HCI_Reset */

    exchange(data, cmd, sizeof(cmd), NULL, 0);
}

/*
 * From 5.0 BlueTool names the chip by Read Local Version's manufacturer and LMP subversion, and the name picks the
 * firmware and the boot script: Broadcom 0x8107 is its "BCM4325" (the N88's), 0x4217 its "BCM4329B1" (the N18's).
 * The made-up 0x2211 this model used to answer named no chip, so BlueTool found no firmware and BTServer ran it
 * again, all the time the device was up.
 */
static void local_version(const void *data)
{
    const Board *b = data;
    const uint8_t cmd[] = { 1, 0x01, 0x10, 0 };                         /* Read Local Version Information */

    exchange(b, cmd, sizeof(cmd), b->version, sizeof(b->version));
}

/*
 * In DMA receive mode the 5.x and 6.x serial driver learns of a packet the CDMA took only from the receive timeout,
 * and a reply of at least the trigger level must still raise it: the UART used to cancel the timeout at the trigger
 * level, so Read Local Version's 15 bytes never reached BlueTool while HCI_Reset's 7 did.
 */
static void dma_rx_timeout(const void *data)
{
    const Board *b = data;
    const uint64_t u = UART(b->uart);
    const uint8_t cmd[] = { 1, 0x01, 0x10, 0 };
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                b->machine, kboot);

    qtest_writel(q, u + 0x28, 1);                                       /* UBRDIV: a word time to time out by */
    qtest_writel(q, u + 8, 7);                                          /* UFCON: FIFOs on, the lowest Rx trigger */
    qtest_writel(q, u + 4, 0x1000 | 0x80 | 2);                          /* UCON: Rx timeout on, Rx by DMA */
    for (size_t i = 0; i < sizeof(cmd); i++) {
        qtest_writeb(q, u + 0x20, cmd[i]);
    }
    qtest_clock_step(q, 10 * MSEC);
    g_assert_cmpuint(qtest_readl(q, u + 0x18) & 255, ==, 15);           /* the reply, past the trigger level */
    g_assert_cmphex(qtest_readl(q, u + 0x10) & 8, ==, 8);               /* UTRSTAT: Rx timeout */
    qtest_quit(q);
}

/* HCI version 4 (2.1), revision 0 (the ROM's build), LMP version 4, Broadcom (0x000f), the subversion; LE. */
static const Board boards[] = {
    { "n18", 1, { 0x04, 0x00, 0x00, 0x04, 0x0f, 0x00, 0x17, 0x42 } },
    { "n88", 3, { 0x04, 0x00, 0x00, 0x04, 0x0f, 0x00, 0x07, 0x81 } },
};

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle with an empty image: the trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    for (size_t i = 0; i < ARRAY_SIZE(boards); i++) {
        g_autofree char *reset = g_strdup_printf("/s5l8920/bt/%s-hci-reset", boards[i].machine);
        g_autofree char *version = g_strdup_printf("/s5l8920/bt/%s-local-version", boards[i].machine);
        qtest_add_data_func(reset, &boards[i], hci_reset);
        qtest_add_data_func(version, &boards[i], local_version);
        g_autofree char *timeout = g_strdup_printf("/s5l8920/bt/%s-dma-rx-timeout", boards[i].machine);
        qtest_add_data_func(timeout, &boards[i], dma_rx_timeout);
    }
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
