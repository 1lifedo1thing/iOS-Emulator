/* SPDX-License-Identifier: GPL-2.0-or-later
 * Isolate two-unit GLES1 combine using only EGL lifetime and raw driver APIs.
 * No texture/array/pixel portability wrappers participate in this diagnostic.
 */
#include <signal.h>
#include <string.h>
#include <unistd.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#define GLES_HOST_EGL_ES 1
#include "../../hw/arm/gles-host-egl.c.inc"
static void expired(int sig)
{
    static const char message[]="FAIL raw GLES1 combine signal/deadline\n";
    (void)write(STDERR_FILENO,message,sizeof(message)-1); _Exit(128+sig);
}
static bool extension(const char *extensions, const char *name)
{
    size_t length=strlen(name);
    for (const char *p=extensions;p && (p=strstr(p,name));p+=length)
        if ((p==extensions || p[-1]==' ') && (!p[length] || p[length]==' ')) return true;
    return false;
}
/* Diagnose a broken capability query without supplying a guessed replacement.
 * All generated probe errors are reported/consumed, and both selectors are
 * restored before the existing real two-unit draw runs. */
static bool diagnose_units(void)
{
    GLenum prior=glGetError();
    fprintf(stderr,"[raw-units] init-error=0x%x uid=%u\n",prior,(unsigned)getuid());
    if (prior) return false;
    GLint reported=-1,active=-1,client=-1;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS,&reported);
    GLenum query_error=glGetError();
    fprintf(stderr,"[raw-units] MAX_TEXTURE_UNITS=%d error=0x%x\n",reported,query_error);
    void (*get)(GLenum,GLint *)=(void *)eglGetProcAddress("glGetIntegerv");
    if (get) {
        GLint alternate=-1;get(GL_MAX_TEXTURE_UNITS,&alternate);
        fprintf(stderr,"[raw-units] GetProcAddress MAX_TEXTURE_UNITS=%d error=0x%x\n",alternate,glGetError());
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE,&client);
    GLenum error=glGetError();
    fprintf(stderr,"[raw-units] saved-active=0x%x client=0x%x error=0x%x\n",active,client,error);
    if (error || active<GL_TEXTURE0 || client<GL_TEXTURE0) return false;
    unsigned usable=0;
    for (unsigned unit=0;unit<8;unit++) {
        glActiveTexture(GL_TEXTURE0+unit);GLenum server_error=glGetError();
        glClientActiveTexture(GL_TEXTURE0+unit);GLenum client_error=glGetError();
        fprintf(stderr,"[raw-units] unit=%u server-error=0x%x client-error=0x%x\n",unit,server_error,client_error);
        if (server_error || client_error) break;
        usable++;
    }
    glActiveTexture(active);glClientActiveTexture(client);
    error=glGetError();
    GLint after_active=-1,after_client=-1;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&after_active);glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE,&after_client);
    GLenum after_error=glGetError();
    fprintf(stderr,"[raw-units] usable-selectors=%u restored=0x%x/0x%x error=0x%x/0x%x; actual two-unit pixels required next\n",
        usable,after_active,after_client,error,after_error);
    return usable>=2 && !error && !after_error && after_active==active && after_client==client;
}
int main(int argc,char **argv)
{
    bool units=argc==2 && !strcmp(argv[1],"--diagnose-units");
    if (argc!=1 && !units) { fprintf(stderr,"usage: egl-combine [--diagnose-units]\n");return 64; }
    setbuf(stderr,NULL); signal(SIGALRM,expired); signal(SIGSEGV,expired); alarm(20);
    fprintf(stderr,"[raw-combine] entry\n");
    CGLContextObj context=NULL;
    if (!gles_egl_create(NULL,1,&context)) return 2;
    fprintf(stderr,"[raw-combine] renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    if (units && !diagnose_units()) { CGLReleaseContext(context);return 2; }
    PFNGLGENFRAMEBUFFERSOESPROC gen=(PFNGLGENFRAMEBUFFERSOESPROC)eglGetProcAddress("glGenFramebuffersOES");
    PFNGLBINDFRAMEBUFFEROESPROC bind=(PFNGLBINDFRAMEBUFFEROESPROC)eglGetProcAddress("glBindFramebufferOES");
    PFNGLFRAMEBUFFERTEXTURE2DOESPROC attach=(PFNGLFRAMEBUFFERTEXTURE2DOESPROC)eglGetProcAddress("glFramebufferTexture2DOES");
    PFNGLCHECKFRAMEBUFFERSTATUSOESPROC status=(PFNGLCHECKFRAMEBUFFERSTATUSOESPROC)eglGetProcAddress("glCheckFramebufferStatusOES");
    bool passed=false;
    if (!gen || !bind || !attach || !status ||
        !extension((const char *)glGetString(GL_EXTENSIONS),"GL_OES_framebuffer_object")) {
        fprintf(stderr,"[raw-combine] required OES framebuffer dispatch absent\n"); goto done;
    }
    GLuint textures[3]={0},framebuffer=0;
    glGenTextures(3,textures); glBindTexture(GL_TEXTURE_2D,textures[0]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    gen(1,&framebuffer); bind(GL_FRAMEBUFFER_OES,framebuffer);
    attach(GL_FRAMEBUFFER_OES,GL_COLOR_ATTACHMENT0_OES,GL_TEXTURE_2D,textures[0],0);
    GLenum complete=status(GL_FRAMEBUFFER_OES),error=glGetError();
    fprintf(stderr,"[raw-combine] FBO status=0x%x error=0x%x\n",complete,error);
    if (complete!=GL_FRAMEBUFFER_COMPLETE_OES || error) goto done;
    const GLubyte colors[2][4]={{0,0,255,255},{255,0,0,255}};
    const GLfloat vertices[]={-1,-1, 1,-1, 0,1},coords[]={0,0, 1,0, .5f,1};
    glViewport(0,0,32,32); glClearColor(0,0,0,1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor4f(1,1,1,1); glVertexPointer(2,GL_FLOAT,0,vertices); glEnableClientState(GL_VERTEX_ARRAY);
    for (unsigned unit=0;unit<2;unit++) {
        glActiveTexture(GL_TEXTURE0+unit); glClientActiveTexture(GL_TEXTURE0+unit);
        glBindTexture(GL_TEXTURE_2D,textures[unit+1]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,colors[unit]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexEnvf(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,unit ? GL_ADD : GL_REPLACE);
        glEnable(GL_TEXTURE_2D); glTexCoordPointer(2,GL_FLOAT,0,coords); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    error=glGetError(); fprintf(stderr,"[raw-combine] two-unit setup error=0x%x\n",error);
    if (error) goto done;
    passed=true;
    for (unsigned active=0;active<2;active++) {
        glActiveTexture(GL_TEXTURE0+active); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES,0,3);
        unsigned char pixel[4]={0}; glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        error=glGetError();
        fprintf(stderr,"[raw-combine] active=%u actual rgba=%u,%u,%u,%u error=0x%x expected=255,0,255,255\n",
                active,pixel[0],pixel[1],pixel[2],pixel[3],error);
        if (error || pixel[0]<241 || pixel[1]>4 || pixel[2]<241 || pixel[3]<241) passed=false;
    }
done:
    CGLReleaseContext(context); alarm(0);
    puts(passed ? "PASS raw GLES1 two-unit ADD independent of active state" : "FAIL raw GLES1 two-unit combine; no production qualification");
    return passed ? 0 : 2;
}
