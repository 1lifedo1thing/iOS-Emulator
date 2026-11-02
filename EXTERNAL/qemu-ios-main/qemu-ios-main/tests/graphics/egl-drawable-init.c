/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded diagnostic of the production Android drawable recipe at its actual
 * NPOT size. Comparators change one factor; none selects production behavior.
 */
#include <signal.h>
#include <string.h>
#include <unistd.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_BGRA GL_BGRA_EXT
#ifndef GLES_HOST_EGL_ES
#define GLES_HOST_EGL_ES 1
#endif
#include "../../hw/arm/gles-host-egl.c.inc"
#include "../../hw/arm/gles-host-native.c.inc"

static void expired(int sig)
{
    static const char message[]="FAIL drawable-init signal/timeout; last stage above\n";
    (void)write(STDERR_FILENO,message,sizeof(message)-1);
    _Exit(128+sig);
}
static bool checked(const char *stage)
{
    GLenum peek=gles_native_peek_error();
    GLenum error=glGetError(), after=glGetError();
    fprintf(stderr,"[drawable-init] stage=%s error=0x%x peek=0x%x after=0x%x\n",stage,error,peek,after);
    return error==GL_NO_ERROR && peek==GL_NO_ERROR && after==GL_NO_ERROR;
}
#define STEP(call) do { fprintf(stderr,"[drawable-init] begin=%s\n",#call); call; if (!checked(#call)) goto failed; } while (0)

int main(int argc,char **argv)
{
    bool generated=false,clamp=false,separate=false,separate24=false;
    unsigned width=320,height=480;
    for (int n=1;n<argc;n++) {
        if (!strcmp(argv[n],"--generated")) generated=true;
        else if (!strcmp(argv[n],"--clamp")) clamp=true;
        else if (!strcmp(argv[n],"--separate")) separate=true;
        else if (!strcmp(argv[n],"--separate24")) separate=separate24=true;
        else if (!strcmp(argv[n],"--size=32x32")) width=height=32;
        else if (!strcmp(argv[n],"--size=320x480")) { width=320;height=480; }
        else { fprintf(stderr,"usage: egl-drawable-init [--size=32x32|--size=320x480] [--generated] [--clamp] [--separate|--separate24]\n"); return 64; }
    }
    setbuf(stderr,NULL);signal(SIGALRM,expired);signal(SIGSEGV,expired);signal(SIGABRT,expired);alarm(20);
    CGLContextObj context=NULL;
    GLuint texture=0x40000000u,framebuffer=0x40000000u,depth=0x40000000u,stencil=0;
    if (!gles_egl_create(NULL,1,&context)) return 2;
    fprintf(stderr,"[drawable-init] uid=%u euid=%u\n",(unsigned)getuid(),(unsigned)geteuid());
    FILE *identity=fopen("/proc/self/attr/current","r");char line[1024];
    if (identity) { if (fgets(line,sizeof(line),identity)) fprintf(stderr,"[drawable-init] selinux=%s",line);fclose(identity); }
    FILE *maps=fopen("/proc/self/maps","r");
    if (maps) {
        while (fgets(line,sizeof(line),maps))
            if (strstr(line,"/vendor/") && (strstr(line,"GLES") || strstr(line,"EGL") || strstr(line,"pvr")))
                fprintf(stderr,"[drawable-init] driver-map=%s",line);
        fclose(maps);
    }
    const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
    fprintf(stderr,"[drawable-init] size=%ux%u names=%s wrap=%s depth=%s renderer=%s version=%s\n",
            width,height,generated?"generated":"private",clamp?"clamp":"default-repeat",
            separate?(separate24?"depth24+stencil8":"depth16+stencil8"):"packed24+8",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    fprintf(stderr,"[drawable-init] extensions=%s\n",extensions?extensions:"(null)");
    fprintf(stderr,"[drawable-init] caps packed=%u full-npot=%u limited-npot=%u stencil8=%u\n",
        gles_native_extension(extensions,"GL_OES_packed_depth_stencil"),gles_native_extension(extensions,"GL_OES_texture_npot"),
        gles_native_extension(extensions,"GL_APPLE_texture_2D_limited_npot"),gles_native_extension(extensions,"GL_OES_stencil8"));
    if (!gles_native_require_fbo()) goto failed;
    /* Verify the production failure diagnostic leaves an existing error intact. */
    glEnable(0xdead);
    GLenum prior1=gles_native_peek_error(),prior2=gles_native_peek_error(),prior3=glGetError(),prior4=glGetError();
    fprintf(stderr,"[drawable-init] error-conservation peek=0x%x/0x%x consumed=0x%x next=0x%x\n",prior1,prior2,prior3,prior4);
    if (prior1!=GL_INVALID_ENUM || prior2!=GL_INVALID_ENUM || prior3!=GL_INVALID_ENUM || prior4!=GL_NO_ERROR) goto failed;
    if (generated) STEP(glGenFramebuffers(1,&framebuffer));
    else while (glIsFramebuffer(framebuffer)) framebuffer++;
    STEP(glBindFramebuffer(GL_FRAMEBUFFER,framebuffer));
    if (generated) STEP(glGenTextures(1,&texture));
    else while (glIsTexture(texture)) texture++;
    STEP(glBindTexture(GL_TEXTURE_2D,texture));
    STEP(glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL));
    STEP(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR));
    STEP(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR));
    if (clamp) {
        STEP(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE));
        STEP(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE));
    }
    STEP(glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0));
    GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr,"[drawable-init] color-only status=0x%x\n",status);
    if (!checked("color-only-status") || status!=GL_FRAMEBUFFER_COMPLETE) goto failed;
    if (generated) STEP(glGenRenderbuffers(1,&depth));
    else while (glIsRenderbuffer(depth)) depth++;
    STEP(glBindRenderbuffer(GL_RENDERBUFFER,depth));
    STEP(glRenderbufferStorage(GL_RENDERBUFFER,separate?(separate24?GL_DEPTH_COMPONENT24:GL_DEPTH_COMPONENT16):GL_DEPTH24_STENCIL8,width,height));
    STEP(glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth));
    if (separate) {
        stencil=0x40000001u;
        if (generated) STEP(glGenRenderbuffers(1,&stencil));
        STEP(glBindRenderbuffer(GL_RENDERBUFFER,stencil));
        STEP(glRenderbufferStorage(GL_RENDERBUFFER,GL_STENCIL_INDEX8,width,height));
    }
    STEP(glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,separate?stencil:depth));
    status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr,"[drawable-init] full status=0x%x fbo=%u color=%u depth=%u stencil=%u\n",status,framebuffer,texture,depth,separate?stencil:depth);
    if (!checked("full-status") || status!=GL_FRAMEBUFFER_COMPLETE) goto failed;
    /* Actual color + depth + stencil behavior, not just a COMPLETE enum. */
    const GLfloat vertices[]={-1,-1,1,-1,0,1};unsigned char pixel[4]={0};
    STEP(glViewport(0,0,width,height));STEP(glClearColor(1,0,0,1));STEP(glClearDepthf(1));STEP(glClearStencil(0));
    STEP(glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT));
    STEP(glMatrixMode(GL_PROJECTION));STEP(glLoadIdentity());STEP(glMatrixMode(GL_MODELVIEW));STEP(glLoadIdentity());
    STEP(glEnable(GL_DEPTH_TEST));STEP(glDepthFunc(GL_LESS));STEP(glEnable(GL_STENCIL_TEST));
    STEP(glStencilFunc(GL_ALWAYS,3,255));STEP(glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE));
    STEP(glColor4f(0,1,0,1));STEP(glVertexPointer(2,GL_FLOAT,0,vertices));STEP(glEnableClientState(GL_VERTEX_ARRAY));
    STEP(glDrawArrays(GL_TRIANGLES,0,3));STEP(glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel));
    fprintf(stderr,"[drawable-init] depth+stencil first rgba=%u,%u,%u,%u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
    if (pixel[0]>5 || pixel[1]<250 || pixel[2]>5 || pixel[3]<250) goto failed;
    STEP(glDepthFunc(GL_EQUAL));STEP(glStencilFunc(GL_EQUAL,3,255));STEP(glColor4f(0,0,1,1));
    STEP(glDrawArrays(GL_TRIANGLES,0,3));STEP(glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel));
    fprintf(stderr,"[drawable-init] depth+stencil equal rgba=%u,%u,%u,%u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
    if (pixel[0]>5 || pixel[1]>5 || pixel[2]<250 || pixel[3]<250) goto failed;
    STEP(glDisableClientState(GL_VERTEX_ARRAY));STEP(glDeleteFramebuffers(1,&framebuffer));
    STEP(glDeleteRenderbuffers(1,&depth));if (stencil) STEP(glDeleteRenderbuffers(1,&stencil));STEP(glDeleteTextures(1,&texture));
    CGLReleaseContext(context);alarm(0);
    fprintf(stderr,"PASS drawable-init full-render size=%ux%u names=%s wrap=%s depth=%s\n",width,height,generated?"generated":"private",clamp?"clamp":"default-repeat",separate?(separate24?"depth24+stencil8":"depth16+stencil8"):"packed24+8");
    return 0;
failed:
    fprintf(stderr,"FAIL drawable-init; no production fallback or guest acceptance selected\n");
    CGLReleaseContext(context);return 1;
}
