/* The M2 CLCD's output gamma (iPod touch 2G and 3G, iPhone 3GS): iBoot's table load, White on Black's mirror.
 *
 * The n72's iBoot loads each table from one address write and 256 entries; AppleM2CLCD's White on Black reads the
 * three tables back through +0x410, mirrors them and writes them entry by entry (8C148 0x8082d1fc, 0x8082d740).
 * The panel shows every pixel through them: a straight line keeps it as it is, the mirror inverts it.
 *
 * Mutations (each fails this test): the entry not stepping after a data write (iBoot's load lands on entry 0 and
 * the panel's black turns gray, as 3.1.3's Home did), +0x410 reading 0 (the mirror is all black), the high field
 * taken for the output, the tables not applied (lcd_bright_lut_sync ignoring gamma8), the tables applied with +0x400
 * bit0 off.
 *
 * SLICE include/hw/arm/panel-gamma.h file
 * SLICE hw/arm/ipod_touch_lcd.c define (LCD_LUT_|LCD_GAMMA_CTRL)
 * SLICE hw/arm/ipod_touch_lcd.c range static uint8_t lcd_bright_lut[3][256]; | \n
 * SLICE hw/arm/ipod_touch_lcd.c fn lcd_gamma_update lcd_gamma_reset lcd_lut_read lcd_lut_write lcd_bright_lut_sync
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t hwaddr;
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct {
    int invalidate;
    uint32_t fb_base, gamma_ctrl;
    uint32_t lut_addr;
    uint32_t lut[8][256];
    uint32_t calib[3][256];
    uint8_t gamma8[3][256];
    bool gamma_identity;
} IPodTouchLCDState;
#include "slice.h"

/* AppleM2CLCD's mirror (0x8082d1fc): entry i becomes entry 255 - i with its two 10-bit halves swapped. */
static void mirror(uint32_t t[256])
{
    uint32_t c[256];

    memcpy(c, t, sizeof(c));
    for (int i = 0; i < 256; i++) {
        uint32_t e = c[255 - i];
        t[i] = (e & 0x3ff) << 10 | ((e >> 10) & 0x3ff);
    }
}

int main(void)
{
    static IPodTouchLCDState st;
    IPodTouchLCDState *s = &st;
    uint32_t tab[3][256];

    lcd_gamma_reset(s);
    assert(s->gamma_identity && !s->gamma_ctrl);

    /* iBoot (n72, 7E18): +0x408 = 0x14000 once, then the 256 entries of f(i - 1) << 10 | f(i), f(i) = 4i. */
    for (int t = 0; t < 3; t++) {
        lcd_lut_write(s, LCD_LUT_ADDR, 0x14000 + t * 0x1000);
        for (int i = 0; i < 256; i++) {
            lcd_lut_write(s, LCD_LUT_WDATA, 0x80000000u | MAX(i - 1, 0) * 4 << 10 | i * 4);
        }
    }
    lcd_lut_write(s, LCD_GAMMA_CTRL, 1);     /* and turns the gamma on */
    assert(s->gamma_identity && lcd_lut_read(s, LCD_GAMMA_CTRL) == 1);
    lcd_bright_lut_sync(s, 255);
    assert(lcd_bright_lut[0][0] == 0 && lcd_bright_lut[1][128] == 128 && lcd_bright_lut[2][255] == 255);

    /* White on Black: with the gamma off, read back as the kernel does, mirror, write entry by entry; then on. */
    uint32_t saved = lcd_lut_read(s, LCD_GAMMA_CTRL);
    lcd_lut_write(s, LCD_GAMMA_CTRL, 0);
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < 256; i++) {
            lcd_lut_write(s, LCD_LUT_ADDR, (4 + t) << 12 | i);
            tab[t][i] = lcd_lut_read(s, LCD_LUT_RDATA);
        }
        mirror(tab[t]);
        for (int i = 0; i < 256; i++) {
            lcd_lut_write(s, LCD_LUT_ADDR, 0x10000 | (4 + t) << 12 | i);
            lcd_lut_write(s, LCD_LUT_WDATA, tab[t][i] & 0xfffff);
        }
    }
    assert(s->gamma_identity);              /* off: the tables are bypassed */
    lcd_lut_write(s, LCD_GAMMA_CTRL, saved);
    assert(!s->gamma_identity && s->invalidate);
    lcd_bright_lut_sync(s, 255);
    for (int c = 0; c < 3; c++) {
        assert(lcd_bright_lut[c][0] == 254 && lcd_bright_lut[c][161] == 93 && lcd_bright_lut[c][255] == 0);
    }
    /* the backlight still scales what the gamma gives */
    lcd_bright_lut_sync(s, 128);
    assert(lcd_bright_lut[0][0] == lround(254 * 128 / 255.0));
    puts("PASS: iBoot's load keeps the panel as it is, White on Black's mirror inverts it");
    return 0;
}
