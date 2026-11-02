/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded diagnostic of core-FBO interop in a real GLES1 context. The optional
 * --oes path checks advertised extension dispatch. Neither selects production
 * behavior; both require real fixed-function pixels and packed depth/stencil.
 */
#include <signal.h>
#include <string.h>
#include <unistd.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#ifndef GLES_HOST_EGL_ES
#define GLES_HOST_EGL_ES 1
#endif
#include "../../hw/arm/gles-host-egl.c.inc"

static void expired(int sig)
{
    static const char message[]="FAIL core-FBO probe signal/timeout; last phase above\n";
    (void)write(STDERR_FILENO,message,sizeof(message)-1);
    _Exit(128+sig);
}
static bool extension(const char *extensions, const char *name)
{
    size_t length=strlen(name);
    for (const char *p=extensions;p && (p=strstr(p,name));p+=length)
        if ((p==extensions || p[-1]==' ') && (!p[length] || p[length]==' ')) return true;
    return false;
}
static bool checked(const char *phase)
{
    GLenum error=glGetError();
    fprintf(stderr,"[core-fbo] %s error=0x%x\n",phase,error);
    return error==GL_NO_ERROR;
}
#define STEP(call) do { fprintf(stderr,"[core-fbo] begin %s\n",#call); call; if (!checked(#call)) goto failed; } while (0)

int main(int argc, char **argv)
{
    bool oes=argc==2 && !strcmp(argv[1],"--oes");
    if (argc!=1 && !oes) { fprintf(stderr,"usage: egl-core-fbo [--oes]\n"); return 64; }
    setbuf(stderr,NULL);
    signal(SIGALRM,expired); signal(SIGSEGV,expired); signal(SIGABRT,expired);
    alarm(20);
    CGLContextObj context=NULL;
    if (!gles_egl_create(NULL,1,&context)) return 2;
    fprintf(stderr,"[core-fbo] mode=%s renderer=%s version=%s extensions=%s\n",
            oes ? "OES" : "experimental-core",glGetString(GL_RENDERER),glGetString(GL_VERSION),glGetString(GL_EXTENSIONS));
    PFNGLGENFRAMEBUFFERSOESPROC gen_fb=oes ? (PFNGLGENFRAMEBUFFERSOESPROC)eglGetProcAddress("glGenFramebuffersOES") : glGenFramebuffers;
    PFNGLBINDFRAMEBUFFEROESPROC bind_fb=oes ? (PFNGLBINDFRAMEBUFFEROESPROC)eglGetProcAddress("glBindFramebufferOES") : glBindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DOESPROC attach_tex=oes ? (PFNGLFRAMEBUFFERTEXTURE2DOESPROC)eglGetProcAddress("glFramebufferTexture2DOES") : glFramebufferTexture2D;
    PFNGLCHECKFRAMEBUFFERSTATUSOESPROC status_fb=oes ? (PFNGLCHECKFRAMEBUFFERSTATUSOESPROC)eglGetProcAddress("glCheckFramebufferStatusOES") : glCheckFramebufferStatus;
    PFNGLGENRENDERBUFFERSOESPROC gen_rb=oes ? (PFNGLGENRENDERBUFFERSOESPROC)eglGetProcAddress("glGenRenderbuffersOES") : glGenRenderbuffers;
    PFNGLBINDRENDERBUFFEROESPROC bind_rb=oes ? (PFNGLBINDRENDERBUFFEROESPROC)eglGetProcAddress("glBindRenderbufferOES") : glBindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEOESPROC storage_rb=oes ? (PFNGLRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glRenderbufferStorageOES") : glRenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFEROESPROC attach_rb=oes ? (PFNGLFRAMEBUFFERRENDERBUFFEROESPROC)eglGetProcAddress("glFramebufferRenderbufferOES") : glFramebufferRenderbuffer;
    if (!gen_fb || !bind_fb || !attach_tex || !status_fb || !gen_rb || !bind_rb || !storage_rb || !attach_rb ||
        (oes && !extension((const char *)glGetString(GL_EXTENSIONS),"GL_OES_framebuffer_object"))) {
        fprintf(stderr,"FAIL required advertised FBO dispatch unavailable\n"); goto failed;
    }
    GLuint texture=0,framebuffer=0,renderbuffer=0;
    STEP(glGenTextures(1,&texture)); STEP(glBindTexture(GL_TEXTURE_2D,texture));
    STEP(glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL));
    STEP(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST));
    STEP(gen_fb(1,&framebuffer)); fprintf(stderr,"[core-fbo] framebuffer=%u\n",framebuffer);
    if (!framebuffer) goto failed;
    STEP(bind_fb(GL_FRAMEBUFFER,framebuffer));
    STEP(attach_tex(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0));
    GLenum status=status_fb(GL_FRAMEBUFFER);
    fprintf(stderr,"[core-fbo] color-only status=0x%x\n",status);
    if (!checked("color-only status") || status!=GL_FRAMEBUFFER_COMPLETE) goto failed;
    STEP(gen_rb(1,&renderbuffer)); fprintf(stderr,"[core-fbo] renderbuffer=%u\n",renderbuffer);
    if (!renderbuffer) goto failed;
    STEP(bind_rb(GL_RENDERBUFFER,renderbuffer));
    STEP(storage_rb(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8_OES,32,32));
    STEP(attach_rb(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,renderbuffer));
    STEP(attach_rb(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,renderbuffer));
    status=status_fb(GL_FRAMEBUFFER); fprintf(stderr,"[core-fbo] packed status=0x%x\n",status);
    if (!checked("packed status") || status!=GL_FRAMEBUFFER_COMPLETE) goto failed;
    const GLfloat vertices[]={-1,-1, 1,-1, 0,1};
    STEP(glViewport(0,0,32,32)); STEP(glClearColor(0,0,0,1)); STEP(glClear(GL_COLOR_BUFFER_BIT));
    STEP(glMatrixMode(GL_PROJECTION)); STEP(glLoadIdentity());
    STEP(glMatrixMode(GL_MODELVIEW)); STEP(glLoadIdentity()); STEP(glColor4f(1,0,0,1));
    STEP(glVertexPointer(2,GL_FLOAT,0,vertices)); STEP(glEnableClientState(GL_VERTEX_ARRAY));
    STEP(glDrawArrays(GL_TRIANGLES,0,3));
    unsigned char pixel[4]={0};
    STEP(glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel));
    fprintf(stderr,"[core-fbo] actual fixed-function rgba=%u,%u,%u,%u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
    if (pixel[0]<241 || pixel[1]>4 || pixel[2]>4 || pixel[3]<241) goto failed;
    CGLReleaseContext(context); alarm(0);
    puts("PASS real GLES1 fixed-function pixel and packed FBO experiment; production qualification still required");
    return 0;
failed:
    CGLReleaseContext(context); alarm(0);
    fprintf(stderr,"FAIL core-FBO experiment; production capability guard unchanged\n");
    return 2;
}
