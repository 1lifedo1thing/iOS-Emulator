/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from BerryPVZ/LightTouchWin7aecedc. Windows lifetime adapter for the desktop GL executor. A hidden drawable per
 * context keeps rendering independent of SDL's presentation thread. */
typedef struct LTWGLContext {
    HWND window;
    HDC dc;
    HGLRC context;
    unsigned refs;
} *CGLContextObj;
static __thread CGLContextObj ltw_current;

static int CGLSetCurrentContext(CGLContextObj c)
{
    if (!wglMakeCurrent(c ? c->dc : NULL, c ? c->context : NULL)) return -1;
    ltw_current = c;
    return 0;
}
static CGLContextObj CGLGetCurrentContext(void) { return ltw_current; }
static CGLContextObj CGLRetainContext(CGLContextObj c) { c->refs++; return c; }
static void CGLReleaseContext(CGLContextObj c)
{
    if (--c->refs) return;
    if (ltw_current == c) CGLSetCurrentContext(NULL);
    if (c->context) wglDeleteContext(c->context);
    if (c->dc) ReleaseDC(c->window, c->dc);
    if (c->window) DestroyWindow(c->window);
    g_free(c);
}
static bool gles_wgl_create(CGLContextObj share, CGLContextObj *out)
{
    g_autofree char *lower = NULL;
    static const wchar_t name[] = L"LightTouchWinOffscreenGL";
    WNDCLASSW wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = name;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    CGLContextObj c = g_new0(struct LTWGLContext, 1);
    c->refs = 1;
    c->window = CreateWindowW(name, name, WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!c->window) goto fail;
    c->dc = GetDC(c->window);
    if (!c->dc) goto fail;
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cAlphaBits = 8; pfd.cDepthBits = 24;
    int format = ChoosePixelFormat(c->dc, &pfd);
    if (!format || !SetPixelFormat(c->dc, format, &pfd)) goto fail;
    c->context = wglCreateContext(c->dc);
    if (!c->context || (share && !wglShareLists(share->context, c->context))) goto fail;
    if (CGLSetCurrentContext(c)) goto fail;
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK || !GLEW_VERSION_2_1 || !GLEW_EXT_framebuffer_object) goto fail;
    /* Refuse the Windows GDI software rasterizer. This lane requires a real
     * hardware driver; no software OpenGL/SwiftShader backend is shipped. */
    if (!DescribePixelFormat(c->dc, format, sizeof(pfd), &pfd)) goto fail;
    if ((pfd.dwFlags & PFD_GENERIC_FORMAT) &&
        !(pfd.dwFlags & PFD_GENERIC_ACCELERATED)) goto fail;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    lower = g_ascii_strdown(renderer ? renderer : "", -1);
    if (!renderer || strstr(lower, "llvmpipe") || strstr(lower, "softpipe") ||
        strstr(lower, "swrast") || strstr(lower, "swiftshader") ||
        strstr(lower, "gdi generic") || strstr(lower, "microsoft basic render")) goto fail;
    while (glGetError() != GL_NO_ERROR) { }
    fprintf(stderr, "[gles-wgl] %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    *out = c;
    return true;
fail:
    fprintf(stderr, "[gles-wgl] Unable to create OpenGL 2.1 compatibility context (Win32 %lu)\n", GetLastError());
    CGLReleaseContext(c);
    return false;
}

