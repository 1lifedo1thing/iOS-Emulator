/* SPDX-License-Identifier: GPL-2.0-or-later */

/* GL names belong to state->cgl. EGL can refuse to acquire it, for example
 * while it remains current on another thread. Deleting against whichever
 * context the caller had current would damage that unrelated context. The
 * native context release below this helper still owns final EGL destruction;
 * its texture cleanup already distinguishes successful context acquisition.
 * Keep the established non-EGL/Mac path unchanged. */
static void gles_context_delete_objects(GLESHost *state)
{
#ifdef GLES_HOST_EGL
    if (CGLSetCurrentContext(state->cgl) != 0) {
        return;
    }
#else
    CGLSetCurrentContext(state->cgl);
#endif
    glDeleteFramebuffersEXT(1, &state->fbo);
    if (state->sync_fbo) glDeleteFramebuffersEXT(1, &state->sync_fbo);
    if (state->copy_fbo) glDeleteFramebuffersEXT(1, &state->copy_fbo);
    glDeleteTextures(1, &state->tex);
    glDeleteRenderbuffersEXT(1, &state->depth);
#ifdef GLES_HOST_EGL_ES
    if (state->stencil && state->stencil != state->depth)
        glDeleteRenderbuffersEXT(1, &state->stencil);
#endif
    CGLSetCurrentContext(NULL);
}
