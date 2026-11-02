/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#ifdef GLES_HOST_EGL_ES
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_BGRA GL_BGRA_EXT
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include "../../hw/arm/gles-host-egl.c.inc"
#ifdef GLES_HOST_EGL_ES
#include "../../hw/arm/gles-host-native.c.inc"
#endif

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log); abort();
    }
    return s;
}
int main(void)
{
    CGLContextObj c1, c2;
    assert(gles_egl_create(NULL, 1, &c1));
    fprintf(stderr,"[probe] ES1 renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    if (getenv("LT_PROBE_DIAGNOSTIC")) fprintf(stderr,"[probe] extensions=%s\n",glGetString(GL_EXTENSIONS));
#ifdef GLES_HOST_EGL_ES
    assert(!gles_native_extension("GL_OES_framebuffer_object_other","GL_OES_framebuffer_object"));
    assert(gles_native_extension("GL_first GL_OES_framebuffer_object GL_last","GL_OES_framebuffer_object"));
    if (!gles_native_require_fbo()) {
        CGLReleaseContext(c1);
        return 2;
    }
#endif
    GLuint texture=0x40000000u;
    if (getenv("LT_PROBE_GENERATED")) glGenTextures(1,&texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    fprintf(stderr,"[probe] bind texture %u error=0x%x\n",texture,glGetError());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    /* Exercise actual per-context FBO, not the one-pixel EGL dummy drawable. */
    GLuint fb=0x40000000u;
    if (getenv("LT_PROBE_GENERATED")) glGenFramebuffers(1,&fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    fprintf(stderr,"[probe] bind framebuffer %u error=0x%x\n",fb,glGetError());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    fprintf(stderr,"[probe] color FBO=0x%x error=0x%x\n",
            glCheckFramebufferStatus(GL_FRAMEBUFFER),glGetError());
    GLuint depth=0x40000000u;
    if (getenv("LT_PROBE_GENERATED")) glGenRenderbuffers(1,&depth);
    glBindRenderbuffer(GL_RENDERBUFFER,depth);
    fprintf(stderr,"[probe] bind renderbuffer %u error=0x%x\n",depth,glGetError());
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,32,32);
    fprintf(stderr,"[probe] packed depth/stencil storage error=0x%x\n",glGetError());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth);
    GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER), error=glGetError();
    fprintf(stderr,"[probe] complete FBO status=0x%x error=0x%x\n",status,error);
    if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,0);
        glBindRenderbuffer(GL_RENDERBUFFER,depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,32,32);
        fprintf(stderr,"[probe] depth16 storage error=0x%x\n",glGetError());
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
        fprintf(stderr,"[probe] depth16 only status=0x%x error=0x%x\n",glCheckFramebufferStatus(GL_FRAMEBUFFER),glGetError());
        GLuint stencil=0x40000001u;
        glBindRenderbuffer(GL_RENDERBUFFER,stencil);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_STENCIL_INDEX8,32,32);
        fprintf(stderr,"[probe] stencil8 storage error=0x%x\n",glGetError());
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,stencil);
        fprintf(stderr,"[probe] separate depth16+stencil8 status=0x%x error=0x%x\n",glCheckFramebufferStatus(GL_FRAMEBUFFER),glGetError());
    }
    assert(status == GL_FRAMEBUFFER_COMPLETE && error == GL_NO_ERROR);
#ifdef GLES_HOST_EGL_ES
    glActiveTexture(GL_TEXTURE1);
    glEnable(0xdead);  /* Unit validation must preserve a preexisting error. */
    GLint verified_units,verified_active;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS,&verified_units);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&verified_active);
    assert(verified_units>=2 && verified_active==GL_TEXTURE1);
    assert(glGetError()==GL_INVALID_ENUM && glGetError()==GL_NO_ERROR);
    GLfloat verified_float; glGetFloatv(GL_MAX_TEXTURE_UNITS,&verified_float);
    assert(verified_float==verified_units);
    glActiveTexture(GL_TEXTURE0);
    fprintf(stderr,"[probe] verified fixed-function texture units=%d, prior error and active unit preserved\n",verified_units);
#endif
    glViewport(0, 0, 32, 32);
    float vertices[] = {-1,-1, 1,-1, 0,1};
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor4f(1,0,0,1); glVertexPointer(2,GL_FLOAT,0,vertices);
    glEnableClientState(GL_VERTEX_ARRAY); glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char px[4];
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    GLenum triangle_error=glGetError();
    fprintf(stderr,"[probe] first triangle rgba=%u,%u,%u,%u error=0x%x\n",px[0],px[1],px[2],px[3],triangle_error);
    assert(triangle_error == GL_NO_ERROR && px[0] > 240 && px[1] < 5 && px[2] < 5 && px[3] > 240);
    printf("GLES1 triangle rgba=%u,%u,%u,%u renderer=%s\n",px[0],px[1],px[2],px[3],glGetString(GL_RENDERER));
    /* ES1 texture units and client coordinate arrays exercise the CA path. */
    const GLfloat uv[]={0,0, 1,0, .5f,1};
    const unsigned char blue[]={0,0,255,255},red[]={255,0,0,255};
    glBindTexture(GL_TEXTURE_2D,0x40000001u);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,blue);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexEnvf(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
    glEnable(GL_TEXTURE_2D); glTexCoordPointer(2,GL_FLOAT,0,uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDrawArrays(GL_TRIANGLES,0,3); glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(glGetError()==GL_NO_ERROR && px[0]<5 && px[1]<5 && px[2]>240);
    glActiveTexture(GL_TEXTURE1); glClientActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,0x40000002u);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,red);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexEnvf(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_ADD);
    glEnable(GL_TEXTURE_2D); glTexCoordPointer(2,GL_FLOAT,0,uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDrawArrays(GL_TRIANGLES,0,3); glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    GLenum multitexture_error=glGetError();
    GLint units,active,client,mode,binding;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS,&units);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active); glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE,&client);
    glGetTexEnviv(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,&mode); glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    fprintf(stderr,"[probe] multitexture rgba=%u,%u,%u,%u error=0x%x units=%d active=0x%x client=0x%x envmode=0x%x tex=%u enabled=%d\n",
            px[0],px[1],px[2],px[3],multitexture_error,units,active,client,mode,binding,glIsEnabled(GL_TEXTURE_2D));
    assert(multitexture_error==GL_NO_ERROR && px[0]>240 && px[1]<5 && px[2]>240);
#ifdef GLES_HOST_EGL_ES
    const GLint crop[8][4]={{0,0,1,1},{0,0,1,1}};
    const bool has_crop[8]={true,true};
    /* Production DrawTex preserves original array pointers, units and matrices. */
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_native_draw_tex(0,0,0,32,32,crop,has_crop,8));
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    GLenum drawtex_error=glGetError();
    fprintf(stderr,"[probe] production DrawTex rgba=%u,%u,%u,%u error=0x%x\n",px[0],px[1],px[2],px[3],drawtex_error);
    assert(drawtex_error==GL_NO_ERROR && px[0]>240 && px[1]<5 && px[2]>240);
    GLint restored_active,restored_client,restored_mode;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&restored_active); glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE,&restored_client);
    glGetTexEnviv(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,&restored_mode);
    assert(restored_active==active && restored_client==client && restored_mode==mode);
    glDrawArrays(GL_TRIANGLES,0,3); glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(glGetError()==GL_NO_ERROR && px[0]>240 && px[1]<5 && px[2]>240);
    const GLfloat fog[]={0,1,0,1};
    glFogf(GL_FOG_MODE,GL_LINEAR); glFogf(GL_FOG_START,-1); glFogf(GL_FOG_END,1);
    glFogfv(GL_FOG_COLOR,fog); glEnable(GL_FOG);
    assert(gles_native_draw_tex(0,0,.75f,32,32,crop,has_crop,8));
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    fprintf(stderr,"[probe] production DrawTex fog rgba=%u,%u,%u,%u\n",px[0],px[1],px[2],px[3]);
    assert(glIsEnabled(GL_FOG) && glGetError()==GL_NO_ERROR);
    assert(px[0]>=120 && px[0]<=135 && px[1]>=120 && px[1]<=135 && px[2]>=120 && px[2]<=135);
    glDisable(GL_FOG);

    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0); glClientActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB,43);
    const unsigned char rectangle[]={255,0,0,255, 0,255,0,255,
                                      0,0,255,255, 255,255,0,255};
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,rectangle);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    GLint binding_2d,binding_rect,wrap;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding_2d);
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB,&binding_rect);
    glGetTexParameteriv(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_WRAP_S,&wrap);
    assert(binding_2d==0x40000001u && binding_rect==43 && wrap==GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,43);
    assert(glGetError()==GL_INVALID_OPERATION);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    const GLfloat quad[]={-1,-1, 1,-1, -1,1, 1,1};
    const GLfloat pixels[]={0,0, 2,0, 0,2, 2,2};
    glVertexPointer(2,GL_FLOAT,0,quad); glTexCoordPointer(2,GL_FLOAT,0,pixels);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    const unsigned locations[][2]={{8,8},{24,8},{8,24},{24,24}};
    const unsigned char expected_colors[][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}};
    for (unsigned i=0;i<4;i++) {
        glReadPixels(locations[i][0],locations[i][1],1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
        fprintf(stderr,"[probe] rectangle corner%u rgba=%u,%u,%u,%u\n",i,px[0],px[1],px[2],px[3]);
        assert(!memcmp(px,expected_colors[i],3));
    }
    glMatrixMode(GL_TEXTURE); glLoadIdentity(); glTranslatef(.25f,0,0);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    GLfloat matrix[16]; glGetFloatv(GL_TEXTURE_MATRIX,matrix);
    assert(px[0]==255 && px[1]==0 && px[2]==0 && matrix[12]==.25f);
    glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(px[0]==0 && px[1]==0 && px[2]==255);
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB,&binding_rect);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding_2d);
    assert(binding_rect==43 && binding_2d==0x40000001u && glGetError()==GL_NO_ERROR);
    fprintf(stderr,"[probe] rectangle pixel coordinates, matrix order, immutable targets and separate bindings/enables PASS\n");
    /* Name reuse must preserve the old object in sibling bindings and unbound
     * framebuffer attachments; native aliases remain invisible to the guest. */
    fprintf(stderr,"[probe] lifetime: create retained FBO\n");
    GLuint retained_fbo;
    glGenFramebuffers(1,&retained_fbo); glBindFramebuffer(GL_FRAMEBUFFER,retained_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_RECTANGLE_ARB,43,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER,0x40000000u);
    CGLContextObj sibling;
    fprintf(stderr,"[probe] lifetime: create shared GLES1 context\n");
    assert(gles_egl_create(c1,1,&sibling));
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB,43);
    assert(CGLSetCurrentContext(c1)==0);
    fprintf(stderr,"[probe] lifetime: delete and reuse guest name\n");
    GLuint reused=43; glDeleteTextures(1,&reused); assert(!glIsTexture(43));
    glBindTexture(GL_TEXTURE_2D,43);
    const unsigned char fresh[]={0,0,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,fresh);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,retained_fbo);
    fprintf(stderr,"[probe] lifetime: query retained attachment\n");
    GLint retained_name;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&retained_name);
    assert(retained_name==43);
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(px[0]==255 && px[1]==0 && px[2]==0);
    glBindFramebuffer(GL_FRAMEBUFFER,0x40000000u);
    glDeleteFramebuffers(1,&retained_fbo);
    fprintf(stderr,"[probe] lifetime: render retained sibling rectangle\n");
    assert(CGLSetCurrentContext(sibling)==0);
    GLuint sibling_fbo;
    glGenFramebuffers(1,&sibling_fbo); glBindFramebuffer(GL_FRAMEBUFFER,sibling_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glViewport(0,0,32,32); glColor4f(1,1,1,1);
    glEnable(GL_TEXTURE_RECTANGLE_ARB); glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,quad); glTexCoordPointer(2,GL_FLOAT,0,pixels);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(px[0]==255 && px[1]==0 && px[2]==0 && glGetError()==GL_NO_ERROR);
    fprintf(stderr,"[probe] lifetime: release sibling and render new object\n");
    CGLReleaseContext(sibling); assert(CGLSetCurrentContext(c1)==0);
    glBindTexture(GL_TEXTURE_2D,43); glDisable(GL_TEXTURE_RECTANGLE_ARB);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(px[0]==0 && px[1]==0 && px[2]==255);
    glDeleteTextures(1,&reused); assert(!glIsTexture(43));
    GLuint generated; glGenTextures(1,&generated);
    assert(generated && generated!=texture && generated!=0x40000001u);
    glDeleteTextures(1,&generated);
    assert(glGetError()==GL_NO_ERROR);
    fprintf(stderr,"[probe] shared deleted texture, reused name, retained FBO, guest name query and context teardown PASS\n");


#endif
    printf("GLES1 texture/multitexture rgba=%u,%u,%u,%u\n",px[0],px[1],px[2],px[3]);
    assert(gles_egl_create(c1, 2, &c2));
    assert(glIsTexture(texture));
    GLuint fb2; glGenFramebuffers(1,&fb2); glBindFramebuffer(GL_FRAMEBUFFER,fb2);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
#ifdef GLES_HOST_EGL_ES
    const char *vsrc = "attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}";
    const char *fsrc = "precision mediump float; void main(){gl_FragColor=vec4(0.,1.,0.,1.);}";
#else
    const char *vsrc = "#version 120\nattribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}";
    const char *fsrc = "#version 120\nvoid main(){gl_FragColor=vec4(0.,1.,0.,1.);}";
#endif
    GLuint vs=shader(GL_VERTEX_SHADER,vsrc), fs=shader(GL_FRAGMENT_SHADER,fsrc), program=glCreateProgram();
    glAttachShader(program,vs); glAttachShader(program,fs); glBindAttribLocation(program,0,"position"); glLinkProgram(program);
    GLint ok; glGetProgramiv(program,GL_LINK_STATUS,&ok); assert(ok);
    glUseProgram(program); glViewport(0,0,32,32); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices); glEnableVertexAttribArray(0); glDrawArrays(GL_TRIANGLES,0,3);
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    assert(glGetError()==GL_NO_ERROR && px[0]<5 && px[1]>240 && px[2]<5 && px[3]>240);
    printf("GLES2 triangle rgba=%u,%u,%u,%u renderer=%s\n",px[0],px[1],px[2],px[3],glGetString(GL_RENDERER));
    assert(CGLSetCurrentContext(c1)==0); glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px); assert(px[1]>240);
#ifdef GLES_HOST_EGL_ES
    glReadPixels(16,16,1,1,GL_BGRA,GL_UNSIGNED_BYTE,px);
    assert(px[0]<5 && px[1]>240 && px[2]<5 && px[3]>240);
    unsigned short rgb565;
    glReadPixels(16,16,1,1,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,&rgb565);
    assert(rgb565==0x07e0);
#endif
    CGLRetainContext(c1); CGLReleaseContext(c1);
    CGLReleaseContext(c2); CGLReleaseContext(c1);
    assert(eglGetCurrentContext()==EGL_NO_CONTEXT);
    puts("PASS private object names, packed depth/stencil, GLES1 textures/multitexture, context switching, cross-version texture sharing, render/readback, teardown");
    return 0;
}
