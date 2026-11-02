/* SPDX-License-Identifier: GPL-2.0-or-later
 * Raw platform GLES1 server-enable query comparison. No guest/adapter state.
 * Deliberately never calls glIsEnabled: that encoder path has a retained crash.
 */
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void expired(int signal_number) { _Exit(128 + signal_number); }
static bool extension(const char *list, const char *name)
{
    size_t length = strlen(name);
    if (!list) return false;
    for (const char *p = list; (p = strstr(p, name)); p += length)
        if ((p == list || p[-1] == ' ') && (!p[length] || p[length] == ' ')) return true;
    return false;
}
static bool error_free(const char *operation, GLenum cap)
{
    GLenum error = glGetError();
    fprintf(stderr, "[enable-state] %s cap=0x%x error=0x%x\n", operation, cap, error);
    return error == GL_NO_ERROR;
}
static bool query(const char *stage, GLenum cap, GLboolean *value)
{
    *value = GL_FALSE;
    glGetBooleanv(cap, value);
    bool ok = error_free(stage, cap);
    fprintf(stderr, "[enable-state] %s cap=0x%x value=%u\n", stage, cap, *value);
    return ok;
}
static bool exercise(GLenum cap)
{
    GLboolean original, value;
    if (!query("original", cap, &original)) return false;
    bool ok = true;
    glDisable(cap); ok = error_free("disable", cap) && ok;
    bool read = query("disabled", cap, &value);
    ok = read && value == GL_FALSE && ok;
    glEnable(cap); ok = error_free("enable", cap) && ok;
    read = query("enabled", cap, &value);
    ok = read && value == GL_TRUE && ok;
    glDisable(cap); ok = error_free("disable-again", cap) && ok;
    read = query("disabled-again", cap, &value);
    ok = read && value == GL_FALSE && ok;
    if (original) glEnable(cap); else glDisable(cap);
    ok = error_free("restore", cap) && ok;
    read = query("restored", cap, &value);
    return read && value == original && ok;
}
int main(void)
{
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    signal(SIGALRM, expired); alarm(20);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) return 2;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return 3;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return 4;
    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) return 5;
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    bool ok = error_free("initial strings", 0) && version && renderer && extensions;
    fprintf(stderr, "[enable-state] uid=%u EGL=%d.%d version=%s renderer=%s extensions=%s\n",
        (unsigned)getuid(), major, minor, version ? version : "(null)",
        renderer ? renderer : "(null)", extensions ? extensions : "(null)");
    ok = extension(extensions, "GL_OES_texture_cube_map") && ok;
    GLint active = -1, units = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    ok = error_free("saved active", GL_ACTIVE_TEXTURE) && ok;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    ok = error_free("unit limit", GL_MAX_TEXTURE_UNITS) && units >= 2 && ok;
    if (ok) {
        for (unsigned unit = 0; unit < 2; unit++) {
            glActiveTexture(GL_TEXTURE0 + unit);
            bool selected = error_free("select unit", GL_TEXTURE0 + unit);
            GLint selected_active = -1; glGetIntegerv(GL_ACTIVE_TEXTURE, &selected_active);
            selected = error_free("verify selected unit", GL_ACTIVE_TEXTURE) &&
                selected_active == (GLint)(GL_TEXTURE0 + unit) && selected;
            fprintf(stderr, "[enable-state] unit=%u\n", unit);
            if (!selected) { ok = false; break; }
            bool cube = exercise(GL_TEXTURE_CUBE_MAP_OES);
            bool texture_2d = exercise(GL_TEXTURE_2D);
            ok = cube && texture_2d && ok;
        }
        ok = exercise(GL_LIGHTING) && ok;
    }
    if (active >= GL_TEXTURE0) {
        glActiveTexture(active);
        ok = error_free("restore active", GL_ACTIVE_TEXTURE) && ok;
        GLint restored = -1; glGetIntegerv(GL_ACTIVE_TEXTURE, &restored);
        ok = error_free("verify active", GL_ACTIVE_TEXTURE) && restored == active && ok;
    }
    bool cleanup = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    cleanup = eglDestroyContext(display, context) && cleanup;
    cleanup = eglDestroySurface(display, surface) && cleanup;
    cleanup = eglTerminate(display) && cleanup;
    alarm(0);
    if (!ok || !cleanup) { fprintf(stderr, "FAIL raw server-enable query/state restoration\n"); return 6; }
    puts("PASS raw GLES1 GetBooleanv cube-map/2D two-unit and lighting transitions, original states/selectors restored");
    return 0;
}
