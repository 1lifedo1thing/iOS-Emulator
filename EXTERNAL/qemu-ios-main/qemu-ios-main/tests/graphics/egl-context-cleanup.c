/* SPDX-License-Identifier: GPL-2.0-or-later
 * Target-free test of the production cleanup helper. No EGL/GL driver or
 * guest rendering is simulated as acceptance. Run both builds:
 * cc -O2 -Wall -Wextra -Werror -DGLES_HOST_EGL egl-context-cleanup.c -o cleanup
 * cc -O2 -Wall -Wextra -Werror egl-context-cleanup.c -o cleanup-non-egl
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef unsigned GLuint;
typedef struct Context { unsigned id; } *CGLContextObj;
typedef struct GLESHost {
    CGLContextObj cgl;
    GLuint fbo, sync_fbo, copy_fbo, tex, depth, stencil;
} GLESHost;
static CGLContextObj current, refused, target;
static unsigned bind_calls, deletes;
static GLuint deleted[6];
static int CGLSetCurrentContext(CGLContextObj context)
{
    bind_calls++;
    if (context && context == refused) return -1;
    current = context;
    return 0;
}
static void delete_names(int count, const GLuint *names)
{
    assert(count == 1);
    assert(current == target);
    assert(deletes < 6);
    deleted[deletes++] = *names;
}
#define glDeleteFramebuffersEXT delete_names
#define glDeleteTextures delete_names
#define glDeleteRenderbuffersEXT delete_names
#include "../../hw/arm/gles-host-context-cleanup.c.inc"

static void reset(CGLContextObj previous)
{
    current = previous;
    bind_calls = deletes = 0;
    memset(deleted, 0, sizeof deleted);
}
int main(void)
{
    struct Context victim = { 1 }, unrelated = { 2 };
    GLESHost state = { &victim, 11, 12, 13, 14, 15, 0 };
    target = &victim;
#ifdef GLES_HOST_EGL
    /* A refused context must not delete colliding object names in another
     * current context, nor unbind that context. Also test no current context. */
    refused = &victim;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(current == &unrelated && deletes == 0 && bind_calls == 1);
    reset(NULL);
    gles_context_delete_objects(&state);
    assert(current == NULL && deletes == 0 && bind_calls == 1);
#endif
    refused = NULL;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(current == NULL && bind_calls == 2 && deletes == 5);
    const GLuint expected[] = { 11, 12, 13, 14, 15, 0 };
    assert(memcmp(deleted, expected, sizeof expected) == 0);

    /* Uncreated optional FBOs must not create extra deletion calls. */
    state.sync_fbo = state.copy_fbo = 0;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(current == NULL && bind_calls == 2 && deletes == 3);
    assert(deleted[0] == 11 && deleted[1] == 14 && deleted[2] == 15);
#ifdef GLES_HOST_EGL_ES
    state.stencil = 16;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(deletes == 4 && deleted[3] == 16);
    state.stencil = state.depth;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(deletes == 3); /* Defensive alias is never deleted twice. */
    refused = &victim;
    reset(&unrelated);
    gles_context_delete_objects(&state);
    assert(deletes == 0 && current == &unrelated);
#endif
    puts("PASS production context object cleanup binding admission");
    return 0;
}
