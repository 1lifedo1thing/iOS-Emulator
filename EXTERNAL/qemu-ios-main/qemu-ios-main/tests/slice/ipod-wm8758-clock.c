/* The WM8758's frame clock comes from its own registers, without a guest.
 *
 * As the master (R6 bit 0) the codec makes LRCLK = SYSCLK / 256, SYSCLK its MCLK or, with R6 CLKSEL, its PLL's
 * MCLK * (N + K / 2^24) / 4, over R6 MCLKDIV. 1.x's AppleWM8758Audio writes R6 0x14d (PLL, MCLKDIV 2, master),
 * R36 0x00a, R37-R39 0x001 0x1fd 0x1e8 (N.K 10.0311875) for 44.1 kHz and has K 0xeb15aa for 48 kHz in the same
 * table: both 256 fs from the M68's 9.0036 MHz MCLK. Before the PLL is set it writes R6 0x00d (MCLK / 1).
 *
 * SLICE hw/arm/ipod_touch_wm8758.c define WM8758_(CLOCK_GEN|PLL_N)[[:space:]]
 * SLICE hw/arm/ipod_touch_wm8758.c fn wm8758_lrclk
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "slice.h"

#define MCLK 9003600

static void pll(uint16_t *reg, unsigned n, uint32_t k)
{
    reg[WM8758_PLL_N] = n;
    reg[37] = k >> 18;
    reg[38] = k >> 9 & 0x1ff;
    reg[39] = k & 0x1ff;
}

int main(void)
{
    uint16_t reg[64] = { 0 };

    /* Reset: the slave, no clock. */
    assert(wm8758_lrclk(reg, MCLK) == 0);
    /* Master straight off MCLK, MCLKDIV 1. */
    reg[WM8758_CLOCK_GEN] = 0x00d;
    assert(wm8758_lrclk(reg, MCLK) == 35170);
    /* The driver's 44.1 kHz: the PLL at N.K 10.0311875, MCLKDIV 2. */
    pll(reg, 0x00a, 0x7fbe8);
    reg[WM8758_CLOCK_GEN] = 0x14d;
    assert(wm8758_lrclk(reg, MCLK) == 44100);
    /* Its 48 kHz. */
    pll(reg, 0x00a, 0xeb15aa);
    assert(wm8758_lrclk(reg, MCLK) == 48000);
    /* MCLKDIV 4 halves it, PRESCALE halves the PLL's input. */
    reg[WM8758_CLOCK_GEN] = 0x18d;
    assert(wm8758_lrclk(reg, MCLK) == 24000);
    reg[WM8758_CLOCK_GEN] = 0x14d;
    reg[WM8758_PLL_N] |= 0x10;
    assert(wm8758_lrclk(reg, MCLK) == 24000);
    /* The slave drives nothing, whatever its PLL. */
    reg[WM8758_CLOCK_GEN] = 0x14c;
    assert(wm8758_lrclk(reg, MCLK) == 0);
    /* No MCLK, no clock. */
    reg[WM8758_CLOCK_GEN] = 0x14d;
    assert(wm8758_lrclk(reg, 0) == 0);
    puts("PASS: the WM8758 drives LRCLK from its clock and PLL registers as the master, none as the slave");
    return 0;
}
