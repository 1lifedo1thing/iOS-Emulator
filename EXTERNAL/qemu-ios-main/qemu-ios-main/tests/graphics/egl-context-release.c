/* SPDX-License-Identifier: GPL-2.0-or-later
 * Includes the production EGL lifetime leaf against an injected EGL driver.
 * Requires EGL development headers, but no driver/library/guest/threads.
 * cc -O2 -Wall -Wextra -Werror egl-context-release.c -o release-test
 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

static EGLContext bound;
static EGLContext refused;
static unsigned next_context, destroyed_contexts, destroyed_surfaces;
static EGLint injected_error;
static unsigned cleanup_count;
static EGLContext cleanup_expected;

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void) { return bound; }
EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay d, EGLSurface draw,
                                              EGLSurface read, EGLContext c)
{
    (void)d; (void)draw; (void)read;
    if (c == refused) { injected_error = EGL_BAD_ACCESS; return EGL_FALSE; }
    bound = c;
    return EGL_TRUE;
}
EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    EGLint e = injected_error; injected_error = EGL_SUCCESS; return e;
}
EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d; assert(c != bound); destroyed_contexts++; return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    (void)d; (void)s; destroyed_surfaces++; return EGL_TRUE;
}
EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType native)
{
    (void)native; return (EGLDisplay)(uintptr_t)1;
}
EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay d, EGLint *a, EGLint *b)
{
    (void)d; (void)a; (void)b; return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay d, const EGLint *a,
                                               EGLConfig *c, EGLint n, EGLint *found)
{
    (void)d; (void)a; assert(n == 1); *c = (EGLConfig)(uintptr_t)1;
    *found = 1; return EGL_TRUE;
}
EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay d, EGLint attribute)
{
    (void)d; (void)attribute; return "controlled EGL";
}
EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay d)
{
    (void)d; return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
    (void)api; return EGL_TRUE;
}
EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay d, EGLConfig c,
                                                EGLContext share, const EGLint *a)
{
    (void)d; (void)c; (void)share; (void)a;
    return (EGLContext)(uintptr_t)++next_context;
}
EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay d, EGLConfig c,
                                                       const EGLint *a)
{
    (void)d; (void)c; (void)a;
    return (EGLSurface)(uintptr_t)next_context;
}
EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name)
{
    (void)name; return NULL;
}
#include "../../hw/arm/gles-host-egl.c.inc"

static void cleanup(CGLContextObj c, bool current)
{
    assert(current && bound == c->context && c->context == cleanup_expected);
    cleanup_count++;
}
static CGLContextObj create(CGLContextObj share)
{
    CGLContextObj c = NULL;
    assert(gles_egl_create(share, 1, &c));
    c->texture_cleanup = cleanup;
    return c;
}
static void refuse(EGLContext context)
{
    refused = context;
}
int main(void)
{
    /* Use an impossible EGL handle so ordinary binding/unbinding succeeds. */
    refuse((EGLContext)(uintptr_t)UINTPTR_MAX);
    CGLContextObj c = create(NULL);
    cleanup_expected = c->context;
    unsigned before = destroyed_contexts;
    refuse(EGL_NO_CONTEXT);
    CGLReleaseContext(c);
    assert(c == gles_egl_current && c->refs == 1 && c->release_pending);
    assert(c->texture_cleanup == NULL && c->textures->refs == 1);
    assert(cleanup_count == 1 && destroyed_contexts == before);

    /* A failed replacement also leaves the retained wrapper untouched. */
    refuse((EGLContext)(uintptr_t)999);
    struct GLESEGLContext failed = { .context = refused };
    assert(CGLSetCurrentContext(&failed) != 0);
    assert(gles_egl_current == c && c->refs == 1 && c->release_pending);
    assert(cleanup_count == 1 && destroyed_contexts == before);

    /* Explicit retry unbinds/destroys exactly once, without repeating cleanup. */
    refuse((EGLContext)(uintptr_t)UINTPTR_MAX);
    CGLReleaseContext(c);
    assert(gles_egl_current == NULL && bound == EGL_NO_CONTEXT);
    assert(destroyed_contexts == before + 1 && cleanup_count == 1);

    /* Repeated failed-release -> successful-switch cycles must not accumulate
     * orphan wrappers. The newer shared context keeps the texture list alive. */
    for (unsigned i = 0; i < 32; i++) {
        CGLContextObj old = create(NULL);
        cleanup_expected = old->context;
        refuse(EGL_NO_CONTEXT);
        before = destroyed_contexts;
        CGLReleaseContext(old);
        assert(old->release_pending && old->textures->refs == 1);
        refuse((EGLContext)(uintptr_t)UINTPTR_MAX);
        CGLContextObj next = create(old);
        assert(destroyed_contexts == before + 1);
        assert(next == gles_egl_current && next->textures->refs == 1);
        assert(next->textures->contexts == next && next->next_context == NULL);
        cleanup_expected = next->context;
        /* Retaining an ordinary context preserves its existing ownership. */
        CGLRetainContext(next);
        CGLReleaseContext(next);
        assert(next->refs == 1 && !next->release_pending);
        CGLReleaseContext(next);
        assert(gles_egl_current == NULL && destroyed_contexts == before + 2);
    }

    /* Restoring another live context can fail after cleanup. A later successful
     * restore reclaims only the retired victim, preserving the other's refs. */
    CGLContextObj first = create(NULL);
    CGLContextObj second = create(first);
    cleanup_expected = first->context;
    refuse(second->context);
    before = destroyed_contexts;
    CGLReleaseContext(first);
    assert(gles_egl_current == first && first->release_pending);
    assert(first->textures->refs == 2 && second->refs == 1);
    refuse((EGLContext)(uintptr_t)UINTPTR_MAX);
    assert(CGLSetCurrentContext(second) == 0);
    assert(gles_egl_current == second && second->textures->refs == 1);
    assert(destroyed_contexts == before + 1);
    cleanup_expected = second->context;
    CGLReleaseContext(second);
    assert(gles_egl_current == NULL && destroyed_contexts == before + 2);
    /* Releasing another live context while a retired binding is retained must
     * not try to restore the previous wrapper that acquisition just reclaimed. */
    CGLContextObj live = create(NULL);
    CGLContextObj retired = create(live);
    cleanup_expected = retired->context;
    refuse(EGL_NO_CONTEXT);
    before = destroyed_contexts;
    CGLReleaseContext(retired);
    assert(retired->release_pending && retired == gles_egl_current);
    refuse((EGLContext)(uintptr_t)UINTPTR_MAX);
    cleanup_expected = live->context;
    CGLReleaseContext(live);
    assert(gles_egl_current == NULL && destroyed_contexts == before + 2);
    assert(destroyed_contexts == destroyed_surfaces);
    puts("PASS production EGL retained final release, retry, shared switch and bounded cycles");
    return 0;
}
