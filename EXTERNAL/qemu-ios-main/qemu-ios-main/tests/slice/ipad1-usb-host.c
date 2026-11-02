/* The USB keyboard's EHCI on 6.x and 7.x A4 boards: kboot's DT edits put it up next to device mode.
 *
 * AppleEmbeddedUSBArbitrator publishes usb-ehci when its publish-criteria meet the arbitrator's mask, which has 0x8
 * while usb-complex has hsic-enabled; 4.3-5.1.1 DTs carry 0xc, 6.0 on 0x4 (host mode only), and 6.0 on hangs the root
 * port off a usb-ehci-port1 node with port-type 2, a dock port left unpowered in device mode. Either way the keyboard
 * never enumerates (n81ap 10B500: EHCI untouched; with the criteria alone, port power cleared mid-reset).
 * Host only: the DeviceTree.bin of n81 10B500 and n90 11D257 / 10B329 from the decrypt caches
 * (~/Developer/qemu-ios-files/<board>/<build>/dec/), edited by the machine's own kboot step. Missing images skip.
 *
 * Mutations (each fails this test): no criteria edit, criteria 0x4, no port-type edit, port-type 2.
 *
 * SLICE include/hw/arm/s5l8930.h typedef A4DTEdit
 * SLICE hw/arm/ipad1.c fn a4_dt_walk
 * SLICE:usb hw/arm/ipad1.c range static const uint8_t usb_criteria[4] | if (address_space_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

#include "slice.h"

static uint8_t *dt;
static size_t dt_len;

/* The machine's a4_dt_edit finds the DT through boot_args; here it is the image. */
static void a4_dt_edit(uint8_t *image, size_t image_len, uint32_t load_pa, uint32_t bootargs_pa, const A4DTEdit *e)
{
    (void)image, (void)image_len, (void)load_pa, (void)bootargs_pa;
    assert(a4_dt_walk(dt, dt_len, 0, e, 0) == dt_len);
}

static void kboot_edits(void)
{
    char *data = (char *)dt;
    size_t image_len = dt_len;
    uint32_t load_pa = 0, bootargs_pa = 0;
    {
#include "usb.h"
}

/* NODE's 4-byte PROP, or -1. */
static int64_t prop(size_t *off, const char *node, const char *name)
{
    uint32_t nprops = ldl_le_p(dt + *off), nchildren = ldl_le_p(dt + *off + 4);
    int64_t value = -1, found = -1;
    bool named = false;

    *off += 8;
    for (uint32_t i = 0; i < nprops; i++) {
        uint32_t plen = ldl_le_p(dt + *off + 32) & 0x7fffffff;
        if (!strcmp((char *)dt + *off, "name")) named = !strcmp((char *)dt + *off + 36, node);
        if (!strcmp((char *)dt + *off, name) && plen == 4) value = ldl_le_p(dt + *off + 36);
        *off += 36 + ((plen + 3) & ~3u);
    }
    if (named) found = value;
    for (uint32_t i = 0; i < nchildren; i++) {
        int64_t c = prop(off, node, name);
        if (c >= 0) found = c;
    }
    return found;
}

static int64_t dt_prop(const char *node, const char *name)
{
    size_t off = 0;
    return prop(&off, node, name);
}

int main(void)
{
    static const char *builds[] = { "n81/10B500", "n90/11D257", "n90/10B329" };
    unsigned seen = 0;

    for (size_t i = 0; i < sizeof(builds) / sizeof(*builds); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/Developer/qemu-ios-files/%s/dec/DeviceTree.bin", getenv("HOME"), builds[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("SKIP %s: no decrypted DeviceTree\n", builds[i]);
            continue;
        }
        dt = malloc(0x200000);
        dt_len = fread(dt, 1, 0x200000, f);
        fclose(f);
        assert(dt_prop("usb-ehci", "publish-criteria") == 0x4);    /* the IPSW's: host mode only */
        assert(dt_prop("usb-ehci-port1", "port-type") == 2);       /* a dock port */
        kboot_edits();
        assert(dt_prop("usb-ehci", "publish-criteria") == 0xc);
        assert(dt_prop("usb-ehci-port1", "port-type") == 0);
        printf("ok %s: usb-ehci published with hsic-enabled, its port powered as a built-in one\n", builds[i]);
        seen++;
        free(dt);
    }
    printf("%u builds checked\n", seen);
    return 0;
}
