#ifndef HW_ARM_PANEL_GAMMA_H
#define HW_ARM_PANEL_GAMMA_H
/*
 * What a panel shows through its controller's output gamma: tables 4-6 (R, G, B) of the A4 CLCD and of the
 * S5L8720/S5L8920 M2 CLCD, 256 entries of f(i - 1) << 10 | f(i), 10 bits each.
 *
 * The table the system loads is the panel's calibration, which the panel's own response undoes: a real unit shows
 * the framebuffer as drawn. (The iPad's 5.x loads a curve at boot that, applied to the framebuffer as it stands,
 * darkened and tinted the screen.) So each channel keeps the last ascending table it was given as its calibration,
 * and a pixel shows as the calibrated panel would: through the loaded table, then the calibration's inverse. White
 * on Black loads the calibration mirrored (entry i from entry 255 - i, its halves swapped), which comes out inverted.
 * ponytail: the panel's response is the exact inverse of the last ascending table; a guest that loaded an ascending
 * table that is not a calibration (a brightness tweak) would have it taken for one.
 */
#include <stdbool.h>
#include <stdint.h>

/* The panel's calibration out of reset: a straight line, f(i) = 4i. */
static inline void panel_gamma_reset(uint32_t calib[3][256])
{
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 256; i++) {
            calib[c][i] = i * 4;
        }
    }
}

/*
 * `out`: each channel's 8-bit value shown for an 8-bit pixel, through the tables `lut` (the R, G and B tables) while
 * `on`; a straight line while the gamma is off. Returns whether that is the identity.
 */
static inline bool panel_gamma(const uint32_t (*lut)[256], uint32_t calib[3][256], bool on, uint8_t out[3][256])
{
    bool identity = true;

    for (int c = 0; c < 3; c++) {
        bool ascending = (lut[c][255] & 0x3ff) > (lut[c][0] & 0x3ff);

        for (int i = 1; i < 256 && ascending; i++) {
            ascending = (lut[c][i] & 0x3ff) >= (lut[c][i - 1] & 0x3ff);
        }
        if (ascending) {
            for (int i = 0; i < 256; i++) {
                calib[c][i] = lut[c][i] & 0x3ff;
            }
        }
        for (int x = 0; x < 256; x++) {
            uint32_t v = lut[c][x] & 0x3ff;
            int lo = 0, hi = 255;

            if (!on || ascending) {         /* off, or the calibration itself: the framebuffer as drawn */
                out[c][x] = x;
                continue;
            }
            while (lo < hi) {               /* the first entry whose calibration reaches v */
                int mid = (lo + hi) / 2;

                if (calib[c][mid] < v) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo > 0 && v - calib[c][lo - 1] < (calib[c][lo] > v ? calib[c][lo] - v : 0)) {
                lo--;                       /* the nearer of the two around v */
            }
            out[c][x] = lo;
            identity = identity && lo == x;
        }
    }
    return identity;
}

#endif
