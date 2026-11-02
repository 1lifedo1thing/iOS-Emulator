/* SPDX-License-Identifier: GPL-2.0-or-later
 * Actual GL texture/readback regressions for the production GLES adapter. */
#include <assert.h>
#include <string.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_BGRA GL_BGRA_EXT
static void (*driver_get_integer)(GLenum, GLint *)=glGetIntegerv;
#include "../../hw/arm/gles-host-egl.c.inc"
#include "../../hw/arm/gles-host-native.c.inc"
int main(void)
{
    CGLContextObj context;
    assert(gles_egl_create(NULL,2,&context));
#if defined(__ANDROID__)
    driver_get_integer=GLES_DRIVER_RAW(glGetIntegerv);
#endif
    GLuint tex,fb;
    glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    /* Unsupported targets must raise an error without changing a valid binding.
     * Query the actual driver state, independent of the logical binding cache. */
    GLint original_binding,after_binding;
    driver_get_integer(GL_TEXTURE_BINDING_2D,&original_binding);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    assert(gles_native_peek_error()==GL_INVALID_ENUM);
    assert(gles_native_peek_error()==GL_INVALID_ENUM);
    assert(glGetError()==GL_INVALID_ENUM);
    driver_get_integer(GL_TEXTURE_BINDING_2D,&after_binding);
    assert(after_binding==original_binding);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    assert(glGetError()==GL_INVALID_ENUM);
    driver_get_integer(GL_TEXTURE_BINDING_2D,&after_binding);
    assert(after_binding==original_binding && glGetError()==GL_NO_ERROR);
    /* Two RGB pixels plus one unused pixel and 3 alignment bytes per row. */
    const unsigned char input[]={255,0,0, 0,0,255, 13,14,15, 99,98,97,
                                  0,255,0, 255,255,0, 16,17,18, 96,95,94};
    glPixelStorei(GL_UNPACK_ALIGNMENT,4); glPixelStorei(GL_UNPACK_ROW_LENGTH,3);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,2,2,0,GL_RGB,GL_UNSIGNED_BYTE,input);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    unsigned char actual[16];
    glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    const unsigned char expected[]={255,0,0,255, 0,0,255,255, 0,255,0,255, 255,255,0,255};
    assert(glGetError()==GL_NO_ERROR && !memcmp(actual,expected,16));
    glCopyTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,0,0,1,1,0);
    assert(glGetError()==GL_INVALID_ENUM);
    driver_get_integer(GL_TEXTURE_BINDING_2D,&after_binding);
    assert(after_binding==original_binding && glGetError()==GL_NO_ERROR);
    fprintf(stderr,"PASS native GLES2 rejected texture target preserves actual binding/error semantics\n");
    glPixelStorei(GL_UNPACK_ROW_LENGTH,-1);
    glPixelStorei(GL_UNPACK_ALIGNMENT,3);
    assert(context->unpack_row_length==3 && context->unpack_alignment==4);
    assert(glGetError()==GL_INVALID_VALUE && glGetError()==GL_NO_ERROR);
    /* Error raised before a successful converted read must remain observable. */
    glEnable(0xdead);
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_BYTE,actual);
    assert(actual[0]==0 && actual[2]==255);
    assert(glGetError()==GL_INVALID_ENUM && glGetError()==GL_NO_ERROR);
    /* An incomplete FBO read must leave the caller's output untouched. */
    GLuint empty;
    glGenFramebuffers(1,&empty); glBindFramebuffer(GL_FRAMEBUFFER,empty);
    memset(actual,0x5a,sizeof(actual));
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_BYTE,actual);
    for (unsigned i=0;i<sizeof(actual);i++) assert(actual[i]==0x5a);
    assert(glGetError()==GL_INVALID_FRAMEBUFFER_OPERATION && glGetError()==GL_NO_ERROR);
    glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0); glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    const unsigned char bgra[]={0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_BGRA,GL_UNSIGNED_BYTE,bgra);
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_BYTE,actual);
    assert(glGetError()==GL_NO_ERROR && !memcmp(actual,bgra,16));
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    unsigned short packed[4];
    glReadPixels(0,0,2,2,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,packed);
    const unsigned short expected565[]={0xf800,0x07e0,0x001f,0xffff};
    assert(!memcmp(packed,expected565,sizeof(packed)));
    glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,packed);
    const unsigned short expected4444[]={0xf00f,0x0f0f,0x00ff,0xffff};
    assert(!memcmp(packed,expected4444,sizeof(packed)));
    glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_SHORT_5_5_5_1,packed);
    const unsigned short expected5551[]={0xf801,0x07c1,0x003f,0xffff};
    assert(!memcmp(packed,expected5551,sizeof(packed)));
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_SHORT_4_4_4_4_REV,packed);
    const unsigned short expectedrev4444[]={0xff00,0xf0f0,0xf00f,0xffff};
    assert(!memcmp(packed,expectedrev4444,sizeof(packed)));
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_SHORT_1_5_5_5_REV,packed);
    const unsigned short expectedrev5551[]={0xfc00,0x83e0,0x801f,0xffff};
    assert(!memcmp(packed,expectedrev5551,sizeof(packed)));
    unsigned int words[4];
    glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_INT_8_8_8_8,words);
    const unsigned int expected8888[]={0xff0000ff,0x00ff00ff,0x0000ffff,0xffffffff};
    assert(!memcmp(words,expected8888,sizeof(words)));
    glReadPixels(0,0,2,2,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,words);
    const unsigned int expectedrev8888[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
    assert(!memcmp(words,expectedrev8888,sizeof(words)));
    unsigned char rgb[12];
    glReadPixels(0,0,2,2,GL_RGB,GL_UNSIGNED_BYTE,rgb);
    const unsigned char expectedrgb[]={255,0,0,0,255,0,0,0,255,255,255,255};
    assert(!memcmp(rgb,expectedrgb,sizeof(rgb)));
    GLint vectors,range[2],precision;
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS,&vectors); assert(vectors>=128);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS,&vectors); assert(vectors>=16);
    glGetIntegerv(GL_MAX_VARYING_VECTORS,&vectors); assert(vectors>=8);
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_MEDIUM_FLOAT,range,&precision);
    assert(range[0]>=10 && range[1]>=10 && precision>=10);
    assert(glGetError()==GL_NO_ERROR);
    /* A bound sibling and an unbound framebuffer retain a deleted object. */
    CGLContextObj sibling;
    assert(gles_egl_create(context,2,&sibling));
    glBindTexture(GL_TEXTURE_2D,tex);
    GLuint sibling_fb; glGenFramebuffers(1,&sibling_fb); glBindFramebuffer(GL_FRAMEBUFFER,sibling_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(CGLSetCurrentContext(context)==0);
    glBindFramebuffer(GL_FRAMEBUFFER,empty);
    glDeleteTextures(1,&tex); assert(!glIsTexture(tex));
    glBindTexture(GL_TEXTURE_2D,tex);
    const unsigned char blue[]={0,0,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,blue);
    glBindFramebuffer(GL_FRAMEBUFFER,fb);
    GLint retained; glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&retained);
    assert(retained==(GLint)tex);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    assert(actual[0]==255 && actual[1]==0 && actual[2]==0);
    assert(CGLSetCurrentContext(sibling)==0);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    assert(actual[0]==255 && actual[1]==0 && actual[2]==0);
    CGLReleaseContext(sibling); assert(CGLSetCurrentContext(context)==0);
    /* Copy operations replace storage in the new object, not the retained one. */
    glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,0,0,2,2,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    glReadPixels(0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    assert(actual[0]==255 && actual[4]==0 && actual[5]==255);
    assert(glGetError()==GL_NO_ERROR);
    fprintf(stderr,"PASS native shared deletion/name reuse, retained FBO query, copy texture storage and context teardown\n");
    CGLReleaseContext(context);
    puts("PASS multirow RGB stride+alignment, invalid pixel-store state/error, preserved prior errors, failed read output, BGRA upload/readback, packed pixel formats, native shader limits");
    return 0;
}
