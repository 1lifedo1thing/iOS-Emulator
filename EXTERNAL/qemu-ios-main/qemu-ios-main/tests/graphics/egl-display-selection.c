/* SPDX-License-Identifier: GPL-2.0-or-later
 * Actual production EGL leaf with a controlled EGL implementation. No driver,
 * display, guest or QEMU build. Also compile -D__ANDROID__ to verify the legacy
 * acquisition order remains unchanged (without loading an Android provider).
 * cc -std=gnu11 -O2 -Wall -Wextra -Werror egl-display-selection.c -o selection
 * cc -std=gnu11 -O2 -Wall -Wextra -Werror -D__ANDROID__ \
 *    egl-display-selection.c -o selection-android-guard
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

enum Failure { OK, ACQUIRE, INITIALIZE, CONFIG, ZERO_CONFIG, CONTEXT, SURFACE, CURRENT };
static enum Failure failure[2];
static unsigned acquired[2], initialized[2], configured[2], terminated[2];
static unsigned destroyed_contexts, destroyed_surfaces, current_candidate;
static EGLint error_state;
static EGLenum bound_api;
static EGLContext bound_context;
static int bind_failure, no_platform_entry, stale_acquisition, query_error, same_display;
static char events[256];
static size_t event_count;
static void event(char kind) { assert(event_count + 1 < sizeof events); events[event_count++] = kind; }
static EGLDisplay display(unsigned index) { return (EGLDisplay)(uintptr_t)(same_display ? 1 : index + 1); }
static EGLDisplay acquire(unsigned index)
{
    current_candidate = index;
    acquired[index]++;
    event(index ? 'D' : 'S');
    if (failure[index] == ACQUIRE) { error_state = EGL_BAD_PARAMETER; return EGL_NO_DISPLAY; }
    if (stale_acquisition) error_state = EGL_BAD_PARAMETER;
    return display(index);
}
static EGLDisplay platform_display(EGLenum platform, void *native, const EGLint *attrs)
{
    assert(platform == EGL_PLATFORM_SURFACELESS_MESA);
    assert(native == (void *)EGL_DEFAULT_DISPLAY && attrs == NULL);
    return acquire(0);
}
EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name)
{
    assert(!strcmp(name, "eglGetPlatformDisplayEXT"));
    return no_platform_entry ? NULL : (__eglMustCastToProperFunctionPointerType)platform_display;
}
EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType native)
{
    assert(native == EGL_DEFAULT_DISPLAY);
    return acquire(1);
}
EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    EGLint error = error_state;
    error_state = EGL_SUCCESS;
    return error;
}
EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    assert(d == display(current_candidate)); initialized[current_candidate]++; event('I');
#ifndef __ANDROID__
    /* Stale BAD_PARAMETER from acquisition must not mask initialize failure. */
    assert(error_state == EGL_SUCCESS);
#endif
    if (failure[current_candidate] == INITIALIZE) { error_state = EGL_NOT_INITIALIZED; return EGL_FALSE; }
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}
EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay d, EGLint attribute)
{
    assert(d == display(current_candidate));
    if (query_error) error_state = EGL_BAD_PARAMETER;
    if (attribute == EGL_VENDOR) return current_candidate ? "default-vendor" : "surfaceless-vendor";
    if (attribute == EGL_VERSION) return "1.5 controlled";
    assert(attribute == EGL_CLIENT_APIS);
    return "OpenGL OpenGL_ES";
}
EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay d)
{
    assert(d == display(current_candidate)); terminated[current_candidate]++; event('T');
    return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
    event('B');
    if (bind_failure) { error_state = EGL_BAD_PARAMETER; return EGL_FALSE; }
    bound_api = api;
    return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay d, const EGLint *attrs,
                                               EGLConfig *config, EGLint capacity, EGLint *count)
{
    assert(d == display(current_candidate) && capacity == 1);
    configured[current_candidate]++; event('C');
#ifndef __ANDROID__
    assert(bound_api == EGL_OPENGL_API && error_state == EGL_SUCCESS);
#else
    assert(bound_api == 0); // Preserve Android's original config-before-bind order.
#endif
    assert(attrs[0] == EGL_SURFACE_TYPE && attrs[1] == EGL_PBUFFER_BIT);
    assert(attrs[2] == EGL_RENDERABLE_TYPE && attrs[3] == EGL_OPENGL_BIT);
    if (failure[current_candidate] == CONFIG) { error_state = EGL_BAD_ATTRIBUTE; return EGL_FALSE; }
    *count = failure[current_candidate] == ZERO_CONFIG ? 0 : 1;
    *config = (EGLConfig)(uintptr_t)(current_candidate + 10);
    return EGL_TRUE;
}
EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay d, EGLConfig config,
                                                EGLContext share, const EGLint *attrs)
{
    assert(d == display(current_candidate));
    assert(config == (EGLConfig)(uintptr_t)(current_candidate + 10));
    assert(share == EGL_NO_CONTEXT && bound_api == EGL_OPENGL_API);
    assert(attrs[0] == EGL_CONTEXT_MAJOR_VERSION && attrs[1] == 2);
    event('X');
    if (failure[current_candidate] == CONTEXT) { error_state = EGL_BAD_MATCH; return EGL_NO_CONTEXT; }
    return (EGLContext)(uintptr_t)20;
}
EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay d, EGLConfig config, const EGLint *attrs)
{
    assert(d == display(current_candidate) && config != NULL);
    assert(attrs[0] == EGL_WIDTH && attrs[1] == 1 && attrs[2] == EGL_HEIGHT && attrs[3] == 1);
    event('P');
    if (failure[current_candidate] == SURFACE) { error_state = EGL_BAD_ALLOC; return EGL_NO_SURFACE; }
    return (EGLSurface)(uintptr_t)30;
}
EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c)
{
    assert(d == display(current_candidate) && draw == read); event('M');
    if (c && failure[current_candidate] == CURRENT) { error_state = EGL_BAD_ACCESS; return EGL_FALSE; }
    bound_context = c;
    return EGL_TRUE;
}
EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void) { return bound_context; }
EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay d, EGLContext c)
{
    assert(d == display(current_candidate) && c != bound_context);
    destroyed_contexts++; return EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay d, EGLSurface surface)
{
    assert(d == display(current_candidate) && surface != EGL_NO_SURFACE);
    destroyed_surfaces++; return EGL_TRUE;
}
#include "../../hw/arm/gles-host-egl.c.inc"

static void reset(void)
{
    assert(gles_egl_current == NULL && bound_context == EGL_NO_CONTEXT);
    memset(failure, 0, sizeof failure); memset(acquired, 0, sizeof acquired);
    memset(initialized, 0, sizeof initialized); memset(configured, 0, sizeof configured);
    memset(terminated, 0, sizeof terminated); memset(events, 0, sizeof events);
    destroyed_contexts = destroyed_surfaces = event_count = current_candidate = 0;
    bind_failure = no_platform_entry = stale_acquisition = query_error = same_display = 0;
    gles_egl_display = EGL_NO_DISPLAY; gles_egl_config = NULL;
    bound_api = 0; error_state = EGL_SUCCESS;
}
static char messages[16384];
static bool run(bool expected)
{
    FILE *capture = tmpfile(); assert(capture);
    fflush(stderr); int previous = dup(STDERR_FILENO); assert(previous >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    CGLContextObj context = NULL;
    bool result = gles_egl_create(NULL, 1, &context);
    assert(result == expected);
    if (result) {
        assert(context && context == gles_egl_current);
        CGLRetainContext(context); CGLReleaseContext(context); CGLReleaseContext(context);
    } else assert(context == NULL);
    fflush(stderr); assert(dup2(previous, STDERR_FILENO) >= 0); close(previous);
    rewind(capture); size_t size = fread(messages, 1, sizeof messages - 1, capture);
    assert(size < sizeof messages - 1); messages[size] = 0; fclose(capture);
    assert(gles_egl_current == NULL);
    return result;
}
int main(void)
{
#ifdef __ANDROID__
    reset(); run(true);
    assert(!acquired[0] && acquired[1] == 1 && !terminated[1]);
    assert(!strcmp(events, "DICBXPMM"));
    reset(); failure[1] = INITIALIZE; run(false);
    assert(!acquired[0] && acquired[1] == 1 && !terminated[1]);
    assert(!strcmp(events, "DI"));
    reset(); failure[1] = ZERO_CONFIG; run(false);
    assert(!acquired[0] && !terminated[1] && bound_api == 0);
    puts("PASS production Android acquisition/config/bind ordering unchanged; no alternate provider");
#else
    reset(); run(true);
    assert(acquired[0] == 1 && !acquired[1] && !terminated[0]);
    assert(!strcmp(events, "BSICXPMM"));
    assert(strstr(messages, "vendor=surfaceless-vendor") && strstr(messages, "apis=OpenGL OpenGL_ES"));
    reset(); no_platform_entry = 1; run(true);
    assert(!acquired[0] && acquired[1] == 1 && strstr(messages, "entry point unavailable"));
    for (enum Failure f = ACQUIRE; f <= ZERO_CONFIG; f++) {
        reset(); failure[0] = f; run(true);
        assert(acquired[0] == 1 && acquired[1] == 1);
        assert(terminated[0] == (f != ACQUIRE) && !terminated[1]);
        assert(gles_egl_display == display(1));
        assert(strstr(messages, "selected display=default"));
        if (f == ZERO_CONFIG) assert(strstr(messages, "zero matches (success)"));
    }
    reset(); failure[0] = INITIALIZE; failure[1] = CONFIG; stale_acquisition = query_error = 1; run(false);
    assert(gles_egl_display == EGL_NO_DISPLAY && gles_egl_config == NULL);
    assert(terminated[0] == 1 && terminated[1] == 1);
    assert(strstr(messages, "initialize failed: 0x3001") && strstr(messages, "choose-config failed: 0x3004"));
    assert(!strstr(messages, "0x300c")); // Neither stale acquisition nor metadata error leaks.
    reset(); failure[0] = ACQUIRE; failure[1] = ACQUIRE; run(false);
    assert(!terminated[0] && !terminated[1] && gles_egl_display == EGL_NO_DISPLAY);
    assert(strstr(messages, "display=surfaceless-mesa acquire failed: 0x300c"));
    assert(strstr(messages, "display=default acquire failed: 0x300c"));
    reset(); failure[0] = INITIALIZE; same_display = 1; run(true);
    assert(terminated[0] == 1 && acquired[1] == 1); // Default may return the same opaque handle.
    reset(); bind_failure = 1; run(false);
    assert(!acquired[0] && !acquired[1] && strstr(messages, "bind-api=0x30a2 failed: 0x300c"));
    for (enum Failure f = CONTEXT; f <= CURRENT; f++) {
        reset(); failure[0] = f; run(false);
        assert(acquired[0] == 1 && !acquired[1] && !terminated[0]);
        assert(destroyed_contexts == (f != CONTEXT));
        assert(destroyed_surfaces == (f == CURRENT));
        if (f == CONTEXT) assert(strstr(messages, "create-context api=1 client=0x30a2 failed: 0x3009"));
        if (f == SURFACE) assert(strstr(messages, "create-pbuffer 1x1 failed: 0x3003"));
        if (f == CURRENT) assert(strstr(messages, "make-current failed: 0x3002"));
    }
    puts("PASS production EGL stages, default-display retry, cleanup, stale errors and context ownership; host mocks only");
#endif
    return 0;
}
