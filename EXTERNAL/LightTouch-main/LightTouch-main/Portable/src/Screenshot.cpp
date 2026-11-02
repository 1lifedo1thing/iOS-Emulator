#include "Internal.hpp"
#include <limits>
extern "C" {
lt_status lt_frame_write_bmp(const lt_frame_info *f, const void *p, uint64_t capacity, const char *path, lt_error *e) {
    return lt::guard(e, [&] {
        lt::abi(f);
        lt::require(p && path && lt::fs::path(path).is_absolute(), "screenshot needs pixels and private absolute path");
        lt::require(f->sequence && f->format == 1 && f->width && f->height && f->width <= 4096 && f->height <= 4096 &&
                        f->stride >= uint64_t(f->width) * 4,
                    "invalid actual screenshot frame");
        uint64_t source_bytes = uint64_t(f->stride) * f->height;
        lt::require(source_bytes <= capacity && source_bytes <= f->bytes && source_bytes <= LT_FRAME_CAPACITY,
                    "screenshot frame exceeds buffer bound");
        uint32_t row = f->width * 4, image = row * f->height;
        std::string bmp(54ull + image, '\0');
        auto le16 = [&](size_t at, uint16_t n) {
            bmp[at] = char(n);
            bmp[at + 1] = char(n >> 8);
        };
        auto le32 = [&](size_t at, uint32_t n) {
            for (unsigned i = 0; i < 4; i++)
                bmp[at + i] = char(n >> (i * 8));
        };
        bmp[0] = 'B';
        bmp[1] = 'M';
        le32(2, uint32_t(bmp.size()));
        le32(10, 54);
        le32(14, 40);
        le32(18, f->width);
        le32(22, uint32_t(-int32_t(f->height)));
        le16(26, 1);
        le16(28, 32);
        le32(34, image);
        auto *bytes = static_cast<const uint8_t *>(p);
        for (uint32_t y = 0; y < f->height; y++)
            memcpy(bmp.data() + 54 + size_t(y) * row, bytes + size_t(y) * f->stride, row);
        lt::atomic_write(path, bmp);
    });
}
}
