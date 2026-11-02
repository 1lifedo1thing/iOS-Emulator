/* The iPhone 4's /product product-id: kboot fills it as iBoot does, so 7.x knows the device type.
 *
 * MobileGestalt's MGIsDeviceOfType compares that 20-byte value; 7.x CoreMotion places the device by it, and with it
 * zero, as the IPSW DT reserves it, the iPhone 4 has no gyro and CoreLocation's orientation detector never starts.
 * Host only: the 11D257 DeviceTree.bin from the n90 decrypt cache (~/Developer/qemu-ios-files/n90/11D257/dec/, or
 * 10B329's), edited by the machine's own kboot step, must carry the product-id that build's iBoot for n90ap holds
 * just ahead of "DEVICEINFOBBT". Missing images skip.
 *
 * Mutations (each fails this test): no product_id on the N90 board, the kboot step not editing the DT, a wrong byte.
 *
 * SLICE include/hw/arm/s5l8930.h typedef A4DTEdit
 * SLICE hw/arm/ipad1.c fn a4_dt_walk
 * SLICE:id hw/arm/ipad1.c range static const uint8_t a4_n90_product_id[20] = | };
 * SLICE:board hw/arm/ipad1.c range .product_id = a4_n90_product_id | ,
 * SLICE:kboot hw/arm/ipad1.c range if (s->board->product_id) { | if (address_space_write
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

/* The array and the N90 board's field naming it, as ipad1.c has them. */
#include "id.h"
};
static const struct { const uint8_t *product_id; } a4_n90 = {
#include "board.h"
};

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
    struct { const typeof(a4_n90) *board; } machine = { &a4_n90 }, *s = &machine;
    char *data = (char *)dt;
    size_t image_len = dt_len;
    uint32_t load_pa = 0, bootargs_pa = 0;
#include "kboot.h"
}

/* /product's product-id: its 20 bytes, or NULL. */
static const uint8_t *product_id(size_t *off)
{
    uint32_t nprops = ldl_le_p(dt + *off), nchildren = ldl_le_p(dt + *off + 4);
    const uint8_t *id = NULL, *found = NULL;
    bool named = false;

    *off += 8;
    for (uint32_t i = 0; i < nprops; i++) {
        uint32_t plen = ldl_le_p(dt + *off + 32) & 0x7fffffff;
        if (!strcmp((char *)dt + *off, "name")) named = !strcmp((char *)dt + *off + 36, "product");
        if (!strcmp((char *)dt + *off, "product-id") && plen == 20) id = dt + *off + 36;
        *off += 36 + ((plen + 3) & ~3u);
    }
    if (named && id) found = id;
    for (uint32_t i = 0; i < nchildren; i++) {
        const uint8_t *c = product_id(off);
        if (c) found = c;
    }
    return found;
}

static uint8_t *load(const char *build, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/Developer/qemu-ios-files/n90/%s/dec/%s", getenv("HOME"), build, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(0x200000);
    *len = fread(b, 1, 0x200000, f);
    fclose(f);
    return b;
}

int main(void)
{
    static const char *builds[] = { "11D257", "10B329" };
    unsigned seen = 0;

    for (size_t i = 0; i < sizeof(builds) / sizeof(*builds); i++) {
        size_t iboot_len, off = 0;
        uint8_t *iboot = load(builds[i], "iBoot.bin", &iboot_len);
        dt = load(builds[i], "DeviceTree.bin", &dt_len);
        if (!iboot || !dt) {
            printf("SKIP %s: no decrypted iBoot or DeviceTree\n", builds[i]);
            free(iboot), free(dt);
            continue;
        }
        uint8_t *mark = memmem(iboot, iboot_len, "DEVICEINFOBBT", 13);
        assert(mark && mark - iboot >= 20);
        const uint8_t *id = product_id(&off);
        assert(id);
        static const uint8_t zero[20];
        assert(!memcmp(id, zero, 20));                 /* the IPSW DT reserves it */
        kboot_edits();
        assert(!memcmp(id, mark - 20, 20));            /* now as iBoot fills it */
        printf("ok %s: /product product-id is iBoot's\n", builds[i]);
        seen++;
        free(iboot), free(dt);
    }
    printf("%u builds checked\n", seen);
    return 0;
}
