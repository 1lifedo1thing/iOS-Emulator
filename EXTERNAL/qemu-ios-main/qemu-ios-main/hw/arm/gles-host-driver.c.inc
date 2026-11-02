/* SPDX-License-Identifier: GPL-2.0-or-later
 * Android provider boundary. Native remains the default; explicit ANGLE uses
 * public EGL/GetProcAddress APIs from the packaged, suffixed runtime only.
 * The table is fixed before any guest context and lives for this worker process.
 */
#include <dlfcn.h>
#include <string.h>
#include <GLES/gl.h>
#include <GLES3/gl3.h>

typedef struct GLESDriver {
#define GLES_DRIVER_EGL(name) __typeof__(&name) raw_##name;
#define GLES_DRIVER_GL(name) __typeof__(&name) raw_##name;
#include "gles-host-driver-entries.c.inc"
#undef GLES_DRIVER_EGL
#undef GLES_DRIVER_GL
} GLESDriver;
static GLESDriver gles_driver = {
#define GLES_DRIVER_EGL(name) .raw_##name = name,
#define GLES_DRIVER_GL(name) .raw_##name = name,
#include "gles-host-driver-entries.c.inc"
#undef GLES_DRIVER_EGL
#undef GLES_DRIVER_GL
};
static int gles_driver_state;
static const char *gles_driver_selected;
static EGLint gles_driver_backend;
static void *gles_driver_handles[3];
static PFNEGLGETPLATFORMDISPLAYEXTPROC gles_driver_platform_display;
#define GLES_DRIVER_RAW(name) gles_driver.raw_##name

static bool gles_driver_owned(const char *name, const void *address, const char *library)
{
    Dl_info owner;
    if (!address || !dladdr(address, &owner) || !owner.dli_fname) {
        fprintf(stderr,"[gles-egl] required provider entry missing: %s\n",name);
        return false;
    }
    const char *base=strrchr(owner.dli_fname,'/');
    base=base ? base+1 : owner.dli_fname;
    if (strcmp(base,library)) {
        fprintf(stderr,"[gles-egl] wrong provider for %s: %s expected %s\n",name,owner.dli_fname,library);
        return false;
    }
    if (getenv("LT_GLES_DRIVER_DIAGNOSTIC"))
        fprintf(stderr,"[gles-egl] public entry %s owner=%s\n",name,owner.dli_fname);
    return true;
}

static bool gles_driver_prepare(void)
{
    const char *selection=getenv("LIGHTTOUCH_GLES_PROVIDER");
    if (!selection) selection="platform";
    if (gles_driver_state) {
        if (gles_driver_state > 0 && strcmp(selection,gles_driver_selected)) {
            fprintf(stderr,"[gles-egl] changing provider requires a fresh worker\n");
            return false;
        }
        return gles_driver_state > 0;
    }
    /* The renderer's engine thread owns setup and all guest contexts. */
    gles_driver_state=-1;
    if (!strcmp(selection,"platform")) {
        gles_driver_selected="platform";
        gles_driver_state=1;
        fprintf(stderr,"[gles-egl] provider=platform; linked native EGL/GLES\n");
        return true;
    }
    if (!strcmp(selection,"angle-gl")) {
        gles_driver_backend=0x320E; gles_driver_selected="angle-gl";
    } else if (!strcmp(selection,"angle-vulkan")) {
        gles_driver_backend=0x3450; gles_driver_selected="angle-vulkan";
    }
    else {
        fprintf(stderr,"[gles-egl] unsupported provider selection: %s\n",selection);
        return false;
    }
    static const char *libraries[]={"libGLESv2_lighttouch_angle.so",
                                    "libEGL_lighttouch_angle.so",
                                    "libGLESv1_CM_lighttouch_angle.so"};
    for (unsigned i=0;i<3;i++) {
        gles_driver_handles[i]=dlopen(libraries[i],RTLD_NOW|RTLD_LOCAL);
        if (!gles_driver_handles[i]) {
            fprintf(stderr,"[gles-egl] required provider library %s: %s\n",libraries[i],dlerror());
            goto failed;
        }
    }
    GLESDriver next=gles_driver;
    /* ANGLE's public resolver returns both API1/API2 implementations from its
     * shared GLESv2 state. Verify each actual address owner, not symbol ordering.
     * No private prefixed entry-point name is used to resolve the table. */
#define GLES_DRIVER_EGL(name) do { \
    void *address=dlsym(gles_driver_handles[1],#name); \
    if (!gles_driver_owned(#name,address,libraries[1])) goto failed; \
    next.raw_##name=(__typeof__(next.raw_##name))address; \
} while (0);
#define GLES_DRIVER_GL(name) do { \
    void *address=(void *)next.raw_eglGetProcAddress(#name); \
    if (!gles_driver_owned(#name,address,libraries[0])) goto failed; \
    next.raw_##name=(__typeof__(next.raw_##name))address; \
} while (0);
#include "gles-host-driver-entries.c.inc"
#undef GLES_DRIVER_EGL
#undef GLES_DRIVER_GL
#define GLES_DRIVER_EXTENSIONS
#define GLES_DRIVER_EXTENSION(name) do { \
    void *address=(void *)next.raw_eglGetProcAddress(#name); \
    if (!gles_driver_owned(#name,address,libraries[0])) goto failed; \
} while (0);
#include "gles-host-driver-entries.c.inc"
#undef GLES_DRIVER_EXTENSION
#undef GLES_DRIVER_EXTENSIONS
    void *platform=(void *)next.raw_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!gles_driver_owned("eglGetPlatformDisplayEXT",platform,libraries[0])) goto failed;
    gles_driver_platform_display=(PFNEGLGETPLATFORMDISPLAYEXTPROC)platform;
    gles_driver=next;
    gles_driver_state=1;
    fprintf(stderr,"[gles-egl] provider=%s; verified public EGL/GLES dispatch\n",selection);
    return true;
failed:
    for (unsigned i=3;i>0;i--)
        if (gles_driver_handles[i-1]) {
            dlclose(gles_driver_handles[i-1]); gles_driver_handles[i-1]=NULL;
        }
    return false;
}

static EGLDisplay gles_driver_display(void)
{
    if (!gles_driver_backend) return gles_driver.raw_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    /* Values are the public EGL_ANGLE_platform_angle extension constants. */
    const EGLint attributes[]={0x3203,gles_driver_backend,EGL_NONE};
    return gles_driver_platform_display(0x3202,EGL_DEFAULT_DISPLAY,attributes);
}

#define GLES_DRIVER_REMAP
#include "gles-host-driver-entries.c.inc"
#undef GLES_DRIVER_REMAP
