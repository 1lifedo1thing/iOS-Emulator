/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Production attachment handlers and outgoing-surface dirty marking. This is
 * an offline ownership/writeback regression, not a guest rendering test. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLint;
typedef struct CPUState { int unused; } CPUState;
enum {
    GL_FRAMEBUFFER_EXT=1, GL_COLOR_ATTACHMENT0_EXT, GL_DEPTH_ATTACHMENT_EXT,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT,
    GL_TEXTURE, GL_RENDERBUFFER_EXT, GLES_SURFACE_BGRA32, GLES_SURFACE_RGBA32,
    GLES_SURFACE_RGB565, GLES_SURFACE_RGB555,
    GLES_SLOT_FB_RENDERBUFFER, GLES_SLOT_FB_TEXTURE_2D
};
typedef struct GLESSurface { bool dirty,detached; unsigned format; } GLESSurface;
static struct {
    unsigned bound_framebuffer; bool fb_dirty;
    GHashTable *surfaces,*rb_sized;
} gh;
static GLESSurface surfaces[3];
static unsigned attachment,texture_mutations,rb_mutations,refusals;
static uint32_t gpu[3],ram[3];
static bool drawable;
static void glGetFramebufferAttachmentParameterivEXT(GLenum target,GLenum point,
                                                      GLenum pname,GLint *value)
{
    assert(target==GL_FRAMEBUFFER_EXT && point==GL_COLOR_ATTACHMENT0_EXT);
    *value=pname==GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT ?
        (attachment?GL_TEXTURE:GL_RENDERBUFFER_EXT) : (GLint)attachment;
}
static void gles_refuse(const char *fmt,const char *value)
{ (void)fmt;(void)value;refusals++; }
static char *gles_fourcc(unsigned format,char *s)
{ (void)format;strcpy(s,"bad");return s; }
#include "../../hw/arm/gles-host-surface-sync.c.inc"
static int64_t gles_sync_surface(CPUState *cpu) { return gles_sync_surface_1(cpu); }
static void gles_set_drawable(unsigned fb,bool value)
{ assert(fb==gh.bound_framebuffer);drawable=value;if(value)attachment=0; }
static bool gles_is_drawable(unsigned fb) { assert(fb==gh.bound_framebuffer);return drawable; }
static void gles_trace_attach(const char *name,unsigned point,unsigned object)
{ (void)name;(void)point;(void)object; }
static void glFramebufferTexture2DEXT(GLenum target,GLenum point,GLenum type,GLuint name,int level)
{
    assert(target==GL_FRAMEBUFFER_EXT && type==GL_TEXTURE && level==0);
    texture_mutations++;if(point==GL_COLOR_ATTACHMENT0_EXT)attachment=name;
}
static void glFramebufferRenderbufferEXT(GLenum target,GLenum point,GLenum type,GLuint name)
{
    assert(target==GL_FRAMEBUFFER_EXT && type==GL_RENDERBUFFER_EXT);
    rb_mutations++;if(point==GL_COLOR_ATTACHMENT0_EXT)attachment=0;
    (void)name;
}
#ifndef SURFACE_ATTACHMENTS_SOURCE
#define SURFACE_ATTACHMENTS_SOURCE "../../hw/arm/gles-host-surface-attachments.c.inc"
#endif
static int64_t mutate(unsigned slot,const uint32_t *a)
{
    CPUState *cpu=NULL;
    (void)cpu; /* The predecessor negative control had no outgoing sync. */
    switch(slot) {
#include SURFACE_ATTACHMENTS_SOURCE
    default:assert(false);
    }
    return 0;
}
static void reset(void)
{
    memset(surfaces,0,sizeof surfaces);memset(gpu,0,sizeof gpu);memset(ram,0,sizeof ram);
    g_hash_table_remove_all(gh.surfaces);g_hash_table_remove_all(gh.rb_sized);
    gh.bound_framebuffer=7;gh.fb_dirty=false;attachment=1;drawable=false;
    texture_mutations=rb_mutations=refusals=0;
    for(unsigned i=1;i<=2;i++) {
        surfaces[i].format=GLES_SURFACE_BGRA32;
        g_hash_table_insert(gh.surfaces,GUINT_TO_POINTER(i),&surfaces[i]);
    }
}
static void finish_frame(void)
{
    assert(gles_sync_surface(NULL)==0);
    /* Model only the existing deferred flush's dirty selection. The production
     * readback is separate: do not invent a frame/pixel qualification here. */
    for(unsigned i=1;i<=2;i++)if(surfaces[i].dirty) {
        ram[i]=gpu[i];surfaces[i].dirty=false;
    }
}
int main(void)
{
    gh.surfaces=g_hash_table_new(g_direct_hash,g_direct_equal);
    gh.rb_sized=g_hash_table_new(g_direct_hash,g_direct_equal);
    const uint32_t texture[]={GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE,2,0};
    const uint32_t renderbuffer[]={GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_RENDERBUFFER_EXT,10,0};
    reset();gpu[1]=0xff0000;
    assert(mutate(GLES_SLOT_FB_TEXTURE_2D,texture)==0);
    assert(attachment==2 && surfaces[1].dirty && !surfaces[2].dirty);
    assert(ram[1]==0 && ram[2]==0); /* No midframe publication. */
    gpu[2]=0x00ff00;finish_frame();
    assert(ram[1]==0xff0000 && ram[2]==0x00ff00);
    reset();gpu[1]=0xff00;
    assert(mutate(GLES_SLOT_FB_RENDERBUFFER,renderbuffer)==0);
    assert(surfaces[1].dirty && drawable && rb_mutations==0 && ram[1]==0);
    finish_frame();assert(ram[1]==0xff00);
    reset();g_hash_table_add(gh.rb_sized,GUINT_TO_POINTER(10));
    assert(mutate(GLES_SLOT_FB_RENDERBUFFER,renderbuffer)==0);
    assert(surfaces[1].dirty && !drawable && rb_mutations==1);
    for(unsigned slot=GLES_SLOT_FB_RENDERBUFFER;slot<=GLES_SLOT_FB_TEXTURE_2D;slot++) {
        reset();surfaces[1].format=0; /* Failure must leave the old attachment/role intact. */
        assert(mutate(slot,slot==GLES_SLOT_FB_TEXTURE_2D?texture:renderbuffer)==-1);
        assert(attachment==1 && !drawable && !gh.fb_dirty && refusals==1);
        assert(texture_mutations==0 && rb_mutations==0);
        reset();uint32_t depth[5];memcpy(depth,slot==GLES_SLOT_FB_TEXTURE_2D?texture:renderbuffer,sizeof depth);
        depth[1]=GL_DEPTH_ATTACHMENT_EXT;
        assert(mutate(slot,depth)==0 && !surfaces[1].dirty && attachment==1);
    }
    reset();surfaces[1].detached=true;
    assert(mutate(GLES_SLOT_FB_TEXTURE_2D,texture)==0);
    assert(!g_hash_table_contains(gh.surfaces,GUINT_TO_POINTER(1)) && !surfaces[1].dirty);
    reset();gh.bound_framebuffer=0;
    /* Default drawable has no tracked outgoing surface. */
    assert(mutate(GLES_SLOT_FB_TEXTURE_2D,texture)==0 && !surfaces[1].dirty);
    g_hash_table_destroy(gh.surfaces);g_hash_table_destroy(gh.rb_sized);
    puts("PASS production surface attachment dirty ownership/deferred writeback");
    return 0;
}
