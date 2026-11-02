/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CPU-only evidence from real copied buffers. No GL state or pixel writes. */
typedef struct GlesFramePixels {
    uint64_t pixels, nonzero_rgb, nonzero_alpha, opaque;
    uint64_t mean[4], hash;
} GlesFramePixels;

static bool gles_frame_pixels_packed(const uint8_t *frame, size_t stride,
                                    uint32_t width, uint32_t height,
                                    unsigned bpp, bool bgra, bool rgb555,
                                    GlesFramePixels *result)
{
    uint64_t sums[4] = { 0 };
    GlesFramePixels value = { .hash = UINT64_C(14695981039346656037) };

    if (!frame || !result || !width || !height || (bpp != 2 && bpp != 4) ||
        (rgb555 && bpp != 2) ||
        width > SIZE_MAX / bpp || stride < (size_t)width * bpp ||
        height > SIZE_MAX / stride ||
        (uint64_t)width * height > UINT64_MAX / 255) {
        return false;
    }
    value.pixels = (uint64_t)width * height;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = frame + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = row + (size_t)x * bpp;
            uint8_t rgba[4];
            if (bpp == 2) {
                uint16_t packed;
                memcpy(&packed, p, sizeof(packed));
                if (rgb555) {
                    /* The CA writeback packs BGRA: blue low, red high.
                     * Bit15 is unused; there is no stored alpha channel. */
                    rgba[0] = ((packed >> 10) & 31) * 255 / 31;
                    rgba[1] = ((packed >> 5) & 31) * 255 / 31;
                    rgba[2] = (packed & 31) * 255 / 31;
                } else {
                    rgba[0] = ((packed >> 11) & 31) * 255 / 31;
                    rgba[1] = ((packed >> 5) & 63) * 255 / 63;
                    rgba[2] = (packed & 31) * 255 / 31;
                }
                rgba[3] = 255;
            } else {
                rgba[0] = p[bgra ? 2 : 0];
                rgba[1] = p[1];
                rgba[2] = p[bgra ? 0 : 2];
                rgba[3] = p[3];
            }
            value.nonzero_rgb += !!(rgba[0] | rgba[1] | rgba[2]);
            value.nonzero_alpha += rgba[3] != 0;
            value.opaque += rgba[3] == 255;
            for (unsigned c = 0; c < 4; c++) {
                sums[c] += rgba[c];
                value.hash = (value.hash ^ rgba[c]) * UINT64_C(1099511628211);
            }
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        value.mean[c] = sums[c] / value.pixels;
    }
    *result = value;
    return true;
}

static bool gles_frame_pixels(const uint8_t *frame, size_t stride,
                              uint32_t width, uint32_t height,
                              unsigned bpp, bool bgra, GlesFramePixels *result)
{
    return gles_frame_pixels_packed(frame, stride, width, height, bpp, bgra,
                                   false, result);
}
