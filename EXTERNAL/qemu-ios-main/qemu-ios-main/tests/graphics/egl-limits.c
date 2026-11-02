/* SPDX-License-Identifier: GPL-2.0-or-later
 * Reproduce a translator reporting eight units while accepting only four.
 * No EGL display is needed: only the capability calls are a bounded fake.
 */
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
#include "../../hw/arm/gles-host-egl.c.inc"
static GLint fake_active=GL_TEXTURE1;
static GLenum fake_error=GL_INVALID_VALUE;
static unsigned active_calls;
void glGetIntegerv(GLenum pname, GLint *out)
{
    if (pname==GL_MAX_TEXTURE_UNITS) *out=8;
    else { assert(pname==GL_ACTIVE_TEXTURE); *out=fake_active; }
}
GLenum glGetError(void)
{
    GLenum error=fake_error; fake_error=GL_NO_ERROR; return error;
}
void glActiveTexture(GLenum texture)
{
    active_calls++;
    if (texture>=GL_TEXTURE0 && texture<GL_TEXTURE0+4) fake_active=texture;
    else if (!fake_error) fake_error=GL_INVALID_ENUM;
}
#include "../../hw/arm/gles-host-native.c.inc"
int main(void)
{
    struct GLESEGLContext context={.api=1};
    gles_egl_current=&context;
    GLint limit;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS,&limit);
    assert(limit==4 && fake_active==GL_TEXTURE1);
    assert(glGetError()==GL_INVALID_VALUE && glGetError()==GL_NO_ERROR);
    unsigned calls=active_calls;
    GLfloat float_limit;
    glGetFloatv(GL_MAX_TEXTURE_UNITS,&float_limit);
    assert(float_limit==4 && active_calls==calls && fake_active==GL_TEXTURE1);
    assert(glGetError()==GL_NO_ERROR);
    puts("PASS misreported unit limit, retained active unit/prior error, cached integer/float capability");
    return 0;
}
