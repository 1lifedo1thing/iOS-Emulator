/* glDrawElements fetches only the vertices between the smallest and the largest index.
 *
 * gles_rebase_indices returns first = the smallest index and nverts = largest - smallest + 1, and an
 * index list rebased to first: the caller's own list when first is 0, otherwise gh.ibuf, rewritten in
 * place when the list already sits there and copied there (grown if need be) when it sits in a buffer
 * object, which is never written. A 16-bit list in a buffer object can start at an odd offset.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_rebase_indices
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
static inline int lduw_le_p(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
static inline void stw_he_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
#include "slice.h"
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write) { return -1; }

static uint16_t u16(const uint8_t *p, unsigned i) { uint16_t v; memcpy(&v, p + i * 2, 2); return v; }

int main(void) {
    uint32_t first, n;
    const uint8_t *out;

    /* 8-bit, smallest 0: the caller's list as it is, every vertex up to the largest */
    static const uint8_t b0[] = { 3, 0, 7, 2 };
    out = gles_rebase_indices(b0, 4, 1, &first, &n);
    assert(out == b0 && first == 0 && n == 8);

    /* 8-bit in a buffer object, smallest 100: copied to gh.ibuf, which grows; the buffer is untouched */
    static const uint8_t b1[] = { 104, 100, 120, 101, 100 };
    out = gles_rebase_indices(b1, 5, 1, &first, &n);
    assert(out == gh.ibuf && gh.ibuf_size >= 5 && first == 100 && n == 21);
    assert(out[0] == 4 && out[1] == 0 && out[2] == 20 && out[3] == 1 && out[4] == 0);
    assert(b1[0] == 104 && b1[2] == 120);

    /* 16-bit already in gh.ibuf: rebased in place, reaching vertex 15000 of a level-sized array */
    uint8_t *ib = gh.ibuf = g_realloc(gh.ibuf, 6);
    gh.ibuf_size = 6;
    stw_he_p(ib, 15000); stw_he_p(ib + 2, 14930); stw_he_p(ib + 4, 14999);
    out = gles_rebase_indices(ib, 3, 2, &first, &n);
    assert(out == ib && gh.ibuf == ib && first == 14930 && n == 71);
    assert(u16(out, 0) == 70 && u16(out, 1) == 0 && u16(out, 2) == 69);

    /* 16-bit at an odd offset in a buffer object, little-endian as the guest wrote it */
    static uint8_t vbo[1 + 8];
    const uint8_t le[] = { 0x10, 0x27, 0x0f, 0x27, 0x12, 0x27, 0x0f, 0x27 };   /* 10000 9999 10002 9999 */
    memcpy(vbo + 1, le, 8);
    out = gles_rebase_indices(vbo + 1, 4, 2, &first, &n);
    assert(out == gh.ibuf && gh.ibuf_size >= 8 && first == 9999 && n == 4);
    assert(u16(out, 0) == 1 && u16(out, 1) == 0 && u16(out, 2) == 3 && u16(out, 3) == 0);
    assert(!memcmp(vbo + 1, le, 8));

    /* a single index: one vertex, at that index */
    static const uint8_t one[] = { 0x34, 0x12 };
    out = gles_rebase_indices(one, 1, 2, &first, &n);
    assert(first == 0x1234 && n == 1 && u16(out, 0) == 0);

    /* no indices: nothing to rebase, the list as it is */
    out = gles_rebase_indices(b1, 0, 1, &first, &n);
    assert(out == b1 && first == 0 && n == 1);

    g_free(gh.ibuf);
    puts("PASS: glDrawElements' index list is rebased to its smallest index (8/16-bit, in place, copied, unaligned, empty)");
}
