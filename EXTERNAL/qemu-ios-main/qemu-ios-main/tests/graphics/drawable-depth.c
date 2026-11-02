/* SPDX-License-Identifier: GPL-2.0-or-later
 * Target-free production helper/resize tests; this is not driver/guest proof.
 * cc -O2 -Wall -Wextra -Werror -DGLES_HOST_EGL_ES drawable-depth.c -o depth
 * Also build without GLES_HOST_EGL_ES for the desktop snapshot default.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLuint, GLenum, GLboolean;
typedef int GLint;
enum {
    GL_NO_ERROR, GL_INVALID_VALUE, GL_OUT_OF_MEMORY,
    GL_DEPTH24_STENCIL8_EXT, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16,
    GL_STENCIL_INDEX8, GL_RENDERBUFFER_EXT, GL_FRAMEBUFFER_EXT,
    GL_DEPTH_ATTACHMENT_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_TEXTURE_2D,
    GL_RGBA, GL_UNSIGNED_BYTE, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
    GL_LINEAR, GL_COLOR_ATTACHMENT0_EXT, GL_FRAMEBUFFER_COMPLETE_EXT,
    GL_TEXTURE_BINDING_2D, GL_RENDERBUFFER_BINDING_EXT, GL_FRAMEBUFFER_BINDING_EXT
};
typedef struct Object { bool live; GLenum format; unsigned width, height, deletes; } Object;
static Object objects[128];
static GLuint bound_rb, bound_tex, bound_fb, next_tex=64, next_fb=96;
static GLuint attached_depth, attached_stencil;
static GLenum error;
static bool fail_stencil;
static struct {
    GLuint tex, depth, stencil, fbo;
    GLenum drawable_depth_format;
    unsigned drawable_width, drawable_height;
    bool drawable_announced, iosurface, fb_dirty, depth_cleared_this_frame;
    void *readback;
} gh;
#ifdef GLES_HOST_EGL_ES
static bool gles_native_extension(const char *extensions, const char *name)
{
    size_t n=strlen(name);
    if (!extensions) return false;
    for (const char *p=extensions; (p=strstr(p,name)); p+=n)
        if ((p==extensions || p[-1]==' ') && (!p[n] || p[n]==' ')) return true;
    return false;
}
#endif
static GLboolean glIsRenderbufferEXT(GLuint n) { return objects[n].live; }
static GLboolean glIsTexture(GLuint n) { return objects[n].live; }
static GLboolean glIsFramebufferEXT(GLuint n) { return objects[n].live; }
static GLuint gles_private_name(GLboolean (*in_use)(GLuint))
{
    GLuint first=in_use==glIsTexture?next_tex:in_use==glIsFramebufferEXT?next_fb:1;
    while (in_use(first)) first++;
    assert(first<128);
    return first;
}
static void glBindRenderbufferEXT(GLenum target,GLuint n)
{
    assert(target==GL_RENDERBUFFER_EXT); bound_rb=n; objects[n].live=n!=0;
}
static void glRenderbufferStorageEXT(GLenum target,GLenum format,unsigned w,unsigned h)
{
    assert(target==GL_RENDERBUFFER_EXT && bound_rb);
    if (fail_stencil && format==GL_STENCIL_INDEX8) { error=GL_OUT_OF_MEMORY; return; }
    objects[bound_rb].format=format;objects[bound_rb].width=w;objects[bound_rb].height=h;
}
static void glFramebufferRenderbufferEXT(GLenum target,GLenum point,GLenum type,GLuint n)
{
    assert(target==GL_FRAMEBUFFER_EXT && type==GL_RENDERBUFFER_EXT);
    if (point==GL_DEPTH_ATTACHMENT_EXT) attached_depth=n;
    else { assert(point==GL_STENCIL_ATTACHMENT_EXT); attached_stencil=n; }
}
static void delete_object(int count,const GLuint *n)
{
    assert(count==1);
    if (*n) { assert(objects[*n].live); objects[*n].live=false; objects[*n].deletes++; }
}
#define glDeleteRenderbuffersEXT delete_object
#define glDeleteTextures delete_object
#define glDeleteFramebuffersEXT delete_object
#include "../../hw/arm/gles-host-drawable-depth.c.inc"
static void glGetIntegerv(GLenum name,GLint *n)
{
    *n=name==GL_TEXTURE_BINDING_2D?bound_tex:name==GL_RENDERBUFFER_BINDING_EXT?bound_rb:bound_fb;
}
static void glBindTexture(GLenum target,GLuint n)
{ assert(target==GL_TEXTURE_2D);bound_tex=n;objects[n].live=n!=0; }
static void glBindFramebufferEXT(GLenum target,GLuint n)
{ assert(target==GL_FRAMEBUFFER_EXT);bound_fb=n;objects[n].live=n!=0; }
static void glTexImage2D(GLenum target,int level,GLenum internal,unsigned w,unsigned h,
                         int border,GLenum format,GLenum type,const void *data)
{
    assert(target==GL_TEXTURE_2D && level==0 && internal==GL_RGBA && border==0);
    assert(format==GL_RGBA && type==GL_UNSIGNED_BYTE && !data);
    objects[bound_tex].width=w;objects[bound_tex].height=h;
}
static void glTexParameteri(GLenum target,GLenum name,GLenum value)
{ assert(target==GL_TEXTURE_2D && (name==GL_TEXTURE_MIN_FILTER || name==GL_TEXTURE_MAG_FILTER) && value==GL_LINEAR); }
static void glFramebufferTexture2DEXT(GLenum target,GLenum point,GLenum type,GLuint n,int level)
{ assert(target==GL_FRAMEBUFFER_EXT && point==GL_COLOR_ATTACHMENT0_EXT && type==GL_TEXTURE_2D && n && level==0); }
static GLenum glCheckFramebufferStatusEXT(GLenum target)
{
    assert(target==GL_FRAMEBUFFER_EXT);
    return objects[attached_stencil].width?GL_FRAMEBUFFER_COMPLETE_EXT:0;
}
static GLenum glGetError(void) { GLenum e=error;error=0;return e; }
static void gles_texture_begin(void) { assert(error==0); }
#define gles_refuse(...) ((void)0)
#define gles_reject(e) (-(int64_t)(e))
#define g_try_malloc0_n(n,s) calloc(n,s)
#define g_free free
#include "../../hw/arm/gles-host-drawable-storage.c.inc"

static void begin(GLenum format)
{
    memset(objects,0,sizeof objects);memset(&gh,0,sizeof gh);
    error=0;fail_stencil=false;
    gh.drawable_depth_format=format;
    gh.tex=64;gh.fbo=96;objects[64].live=objects[96].live=true;
    gles_drawable_depth_allocate(format,320,480,&gh.depth,&gh.stencil);
    /* Initial setup must retain the old private depth binding, not stencil. */
    assert(bound_rb==gh.depth);
    gles_drawable_depth_attach(gh.depth,gh.stencil);
    gh.readback=calloc(320*480,4);assert(gh.readback);
    gh.drawable_width=320;gh.drawable_height=480;
    bound_tex=gh.tex;bound_fb=gh.fbo;
}
static void finish(void)
{
    gles_drawable_depth_delete(gh.depth,gh.stencil);
    glDeleteTextures(1,&gh.tex);glDeleteFramebuffersEXT(1,&gh.fbo);free(gh.readback);
    for (unsigned i=1;i<128;i++) assert(!objects[i].live);
}
int main(void)
{
#ifdef GLES_HOST_EGL_ES
    assert(gles_drawable_depth_select("OpenGL ES-CM 1.1","GL_OES_packed_depth_stencil",1)==GL_DEPTH24_STENCIL8_EXT);
    assert(gles_drawable_depth_select("OpenGL ES-CM 1.1","GL_OES_depth24 GL_OES_stencil8",1)==GL_DEPTH_COMPONENT24);
    assert(gles_drawable_depth_select("OpenGL ES-CM 1.1","GL_OES_stencil8",1)==GL_DEPTH_COMPONENT16);
    assert(gles_drawable_depth_select("OpenGL ES-CM 1.1","GL_OES_depth24",1)==0);
    assert(gles_drawable_depth_select("OpenGL ES 3.2","",2)==GL_DEPTH24_STENCIL8_EXT);
    assert(gles_drawable_depth_select("OpenGL ES 2.0","GL_OES_depth24",2)==GL_DEPTH_COMPONENT24);
    assert(gles_drawable_depth_select("OpenGL ES 2.0","",2)==GL_DEPTH_COMPONENT16);
    assert(gles_drawable_depth_select(NULL,"GL_OES_packed_depth_stencil_suffix",1)==0);
#endif
    const GLenum formats[]={GL_DEPTH24_STENCIL8_EXT,GL_DEPTH_COMPONENT24,GL_DEPTH_COMPONENT16};
    for (unsigned f=0;f<sizeof formats/sizeof *formats;f++) {
#ifndef GLES_HOST_EGL_ES
        if (f) break; /* Desktop always keeps packed storage, including snapshots. */
#endif
        begin(formats[f]);
#ifndef GLES_HOST_EGL_ES
        gh.drawable_depth_format=0; /* Unserialized old desktop snapshot field. */
#endif
        GLuint old_depth=gh.depth,old_stencil=gh.stencil;
        assert(attached_stencil==(old_stencil?old_stencil:old_depth));
        if (old_stencil) bound_rb=old_stencil;
        assert(gles_drawable_storage(480,320)==0);
        assert(gh.drawable_width==480 && gh.drawable_height==320);
        assert(objects[gh.depth].format==formats[f]);
        assert(bound_tex==gh.tex && bound_fb==gh.fbo);
        assert(bound_rb==(old_stencil?gh.stencil:gh.depth));
        assert(objects[old_depth].deletes==1 && (!old_stencil || objects[old_stencil].deletes==1));
        finish();
    }
#ifdef GLES_HOST_EGL_ES
    begin(GL_DEPTH_COMPONENT24);
    /* Failed allocation rolls back both new RBs and all three guest bindings. */
    objects[50].live=objects[51].live=objects[52].live=true;
    bound_tex=50;bound_rb=51;bound_fb=52;
    GLuint old_depth=gh.depth,old_stencil=gh.stencil,old_fbo=gh.fbo;
    void *old_readback=gh.readback;
    fail_stencil=true;
    assert(gles_drawable_storage(480,320)<0);
    assert(bound_tex==50 && bound_rb==51 && bound_fb==52);
    assert(gh.depth==old_depth && gh.stencil==old_stencil && gh.fbo==old_fbo && gh.readback==old_readback);
    assert(gh.drawable_width==320 && gh.drawable_height==480);
    assert(objects[3].deletes==1 && objects[4].deletes==1);
    fail_stencil=false;
    assert(gles_drawable_storage(480,320)==0);
    assert(bound_tex==50 && bound_rb==51 && bound_fb==52);
    objects[50].live=objects[51].live=objects[52].live=false;
    finish();
    /* Fresh context allocation after teardown owns new names independently. */
    begin(GL_DEPTH_COMPONENT24);finish();
#endif
    puts("PASS production private drawable formats, bindings, resize rollback and lifetime");
    return 0;
}
