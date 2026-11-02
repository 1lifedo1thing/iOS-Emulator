/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../hw/arm/gles-host-pixels.c.inc"

int main(void)
{
    /* Padding is deliberately colored; it must never enter the summary. */
    const uint8_t rgba[24] = {255, 0, 0, 0, 0, 255, 0, 128, 255, 255, 255, 255,
                             0, 0, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    const uint8_t bgra[24] = {0, 0, 255, 0, 0, 255, 0, 128, 255, 255, 255, 255,
                             255, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    const uint16_t packed[6] = {0xf800, 0x07e0, 0xffff, 0x001f, 0, 0xffff};
    const uint16_t packed555[6] = {0x7c00, 0x03e0, 0xffff, 0x001f, 0x8000, 0xffff};
    GlesFramePixels r, b, p, p555, cropped, unchanged;

    assert(gles_frame_pixels(rgba, 12, 2, 2, 4, false, &r));
    assert(r.pixels == 4 && r.nonzero_rgb == 3);
    assert(r.nonzero_alpha == 3 && r.opaque == 2);
    assert(r.mean[0] == 63 && r.mean[1] == 63 && r.mean[2] == 63 && r.mean[3] == 159);
    assert(gles_frame_pixels(bgra, 12, 2, 2, 4, true, &b));
    assert(!memcmp(&r, &b, sizeof(r)));
    assert(gles_frame_pixels((const uint8_t *)packed, 6, 2, 2, 2, false, &p));
    assert(p.pixels == 4 && p.nonzero_rgb == 3);
    assert(p.nonzero_alpha == 4 && p.opaque == 4 && p.mean[3] == 255);
    assert(p.mean[0] == 63 && p.mean[1] == 63 && p.mean[2] == 63);
    assert(p.hash != r.hash); /* Equal RGB, different actual alpha. */
    assert(gles_frame_pixels_packed((const uint8_t *)packed555, 6, 2, 2,
                                   2, false, true, &p555));
    assert(!memcmp(&p, &p555, sizeof(p))); /* Same actual colors, different packing. */
    assert(gles_frame_pixels_packed((const uint8_t *)packed555, 6, 1, 1,
                                   2, false, true, &cropped));
    assert(cropped.mean[0] == 255 && cropped.mean[1] == 0 &&
           cropped.mean[2] == 0 && cropped.mean[3] == 255);
    assert(gles_frame_pixels(rgba, 12, 1, 1, 4, false, &cropped));
    assert(cropped.pixels == 1 && cropped.mean[0] == 255 && cropped.mean[3] == 0);
    unchanged = r;
    assert(!gles_frame_pixels(rgba, 7, 2, 2, 4, false, &r));
    assert(!gles_frame_pixels(rgba, 12, 0, 2, 4, false, &r));
    assert(!gles_frame_pixels(rgba, 12, 2, 2, 3, false, &r));
    assert(!gles_frame_pixels_packed(rgba, 12, 2, 2, 4, false, true, &r));
    assert(!gles_frame_pixels(NULL, 12, 2, 2, 4, false, &r));
    assert(!gles_frame_pixels(rgba, 0, 2, 2, 4, false, &r));
    assert(!gles_frame_pixels(rgba, SIZE_MAX, 2, 2, 4, false, &r));
    assert(!memcmp(&r, &unchanged, sizeof(r)));
    puts("PASS real frame summaries distinguish color/alpha, formats, padding and cropped dimensions");
    return 0;
}
