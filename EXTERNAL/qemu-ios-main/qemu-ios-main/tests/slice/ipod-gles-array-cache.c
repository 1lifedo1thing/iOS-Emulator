/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Array caching must refresh data and remove stale enables before every draw.
 * SLICE:types hw/arm/gles-host.c range typedef struct {\n\x20\x20\x20\x20uint8_t *data; | typedef struct {\n\x20\x20\x20\x20uint32_t base, stride
 * SLICE:types hw/arm/gles-host.c define GLES_MAX_TEXUNITS|GLES_TEXCOORD_BIT|GLES_FIXED
 * SLICE hw/arm/gles-host.c fn gles_type_size gles_needs_widen gles_array_name gles_pointer_ok gles_client_active gles_array_enable gles_bind_array gles_bind_all_arrays gles_unbind_arrays
 * PKG glib-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <glib.h>
#include "types.h"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
typedef void CPUState;
typedef uint64_t hwaddr;
typedef struct {
    GLESArray vertex, color, normal, texcoord[GLES_MAX_TEXUNITS];
    unsigned client_active_unit, host_client_active_unit;
    uint64_t t_fetch, t_err, t_arrays, array_binds, array_pointers, array_enables, array_units;
} Context;
static Context contexts[2], *current = &contexts[0];
#define gh (*current)
static bool gles_array_cache = true, fault;
static int gles_strict = 1, gles_prof;
static unsigned enables, disables, pointers, selects, errors;
static GLenum pointer_error, pending_error;
static uint8_t guest[8192];
static uint64_t gles_t(void) { return 0; }
static bool gles_guest_fault_pending(void) { return fault; }
static int gles_guest_rw(CPUState *cpu, hwaddr addr, void *out, size_t n, bool write)
{
    if (addr + n > sizeof(guest)) { fault = true; return -1; }
    memcpy(out, guest + addr, n); return 0;
}
static bool gles_refuse(const char *fmt, ...) { return false; }
static void gles_debug_mark(void) {}
static void enable(GLenum cap) { enables++; }
static void disable(GLenum cap) { disables++; }
static void select_unit(GLenum unit) { selects++; }
static void pointer(GLint size, GLenum type, GLsizei stride, const void *data)
{ pointers++; pending_error = pointer_error; }
static void normal_pointer(GLenum type, GLsizei stride, const void *data)
{ pointer(3, type, stride, data); }
static GLenum error(void)
{ GLenum e = pending_error; pending_error = 0; errors++; return e; }
#define glEnableClientState enable
#define glDisableClientState disable
#define glClientActiveTexture select_unit
#define glVertexPointer pointer
#define glColorPointer pointer
#define glTexCoordPointer pointer
#define glNormalPointer normal_pointer
#define glGetError error
#include "slice.h"
static void init(Context *c)
{
    c->vertex = (GLESArray){ .enabled = 1, .size = 2, .type = GL_FLOAT,
        .ptr = 16, .client_state = GL_VERTEX_ARRAY };
    c->color.client_state = GL_COLOR_ARRAY;
    c->normal.client_state = GL_NORMAL_ARRAY;
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) {
        c->texcoord[i].client_state = GL_TEXTURE_COORD_ARRAY;
    }
    c->texcoord[1] = (GLESArray){ .enabled = 1, .size = 2, .type = GL_FLOAT,
        .ptr = 64, .client_state = GL_TEXTURE_COORD_ARRAY };
}
int main(void)
{
    init(&contexts[0]); init(&contexts[1]);
    uint32_t mask = 1 | (1 << (GLES_TEXCOORD_BIT + 1));
    assert(gles_bind_all_arrays(NULL, 0, 4) == mask);
    assert(enables == 2 && pointers == 2 && errors == 4 && selects == 2);
    gles_unbind_arrays(mask);
    assert(!disables);
    guest[16] = 123;
    assert(gles_bind_all_arrays(NULL, 0, 4) == mask);
    assert(gh.vertex.buf[0] == 123 && pointers == 2 && errors == 4);
    /* Guest disable and a failed fetch must disable the previous host array. */
    gh.texcoord[1].enabled = 0;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1 && disables == 1);
    gh.vertex.ptr = sizeof(guest) - 1;
    assert(!gles_bind_all_arrays(NULL, 0, 4) && !gh.vertex.host_enabled);
    fault = false; gh.vertex.ptr = 16;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1 && gh.vertex.host_enabled);
    /* Scratch growth and converted formats must bind the current allocation. */
    assert(gles_bind_all_arrays(NULL, 0, 100) == 1);
    assert(gh.vertex.host_pointer == gh.vertex.buf);
    gh.vertex.type = GLES_FIXED;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1);
    assert(gh.vertex.host_pointer == gh.vertex.fbuf && gh.vertex.host_type == GL_FLOAT);
    gh.vertex.size = 1;
    assert(!gles_bind_all_arrays(NULL, 0, 4) && !gh.vertex.host_enabled);
    gh.vertex.size = 2;
    /* A rejected host pointer never becomes a cache hit on retry. */
    pointer_error = GL_INVALID_OPERATION;
    gh.vertex.type = GL_FLOAT; gh.vertex.stride = 16;
    assert(!gles_bind_all_arrays(NULL, 0, 4) && !gh.vertex.host_pointer_valid);
    pointer_error = 0;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1);
    /* A context switch must use the other context's pointer/enable state. */
    unsigned before = pointers;
    current = &contexts[1];
    assert(gles_bind_all_arrays(NULL, 0, 4) == mask && pointers == before + 2);
    current = &contexts[0]; before = pointers;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1 && pointers == before);
    /* VBO offsets use their resolved host address; overrun disables the array. */
    GLESBuffer vbo = { .data = guest, .size = sizeof(guest) };
    gh.vertex.vbo = &vbo; gh.vertex.ptr = 0; gh.vertex.type = GL_FLOAT;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1 && gh.vertex.host_pointer == guest);
    gh.vertex.ptr = sizeof(guest) - 1;
    assert(!gles_bind_all_arrays(NULL, 0, 4) && !gh.vertex.host_enabled);
    /* A/B off restores enable/pointer/check/disable on each draw. */
    gh.vertex.vbo = NULL; gh.vertex.ptr = 16; gles_array_cache = false;
    before = pointers;
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1);
    gles_unbind_arrays(1);
    assert(gles_bind_all_arrays(NULL, 0, 4) == 1 && pointers == before + 2);
    for (unsigned i = 0; i < 2; i++) {
        g_free(contexts[i].vertex.buf); g_free(contexts[i].vertex.fbuf);
        g_free(contexts[i].texcoord[1].buf);
    }
    puts("PASS: array data refresh, disables, faults, growth, conversion, rejection, contexts, VBOs, A/B");
}
