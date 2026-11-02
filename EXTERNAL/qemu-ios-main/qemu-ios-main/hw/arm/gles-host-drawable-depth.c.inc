/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Private drawable storage only: never translate guest renderbuffer formats or
 * advertise a guest extension. GLES1 packed depth/stencil is optional; separate
 * depth24 + stencil8 preserves precision where both formats are supported. */
#ifdef GLES_HOST_EGL_ES
static GLenum gles_drawable_depth_select(const char *version,
                                         const char *extensions, unsigned api)
{
    unsigned major = 0;
    if (version) sscanf(version, "OpenGL ES %u", &major);
    if (major >= 3 ||
        gles_native_extension(extensions, "GL_OES_packed_depth_stencil")) {
        return GL_DEPTH24_STENCIL8_EXT;
    }
    if (api < 2 && !gles_native_extension(extensions, "GL_OES_stencil8")) {
        return 0;
    }
    return gles_native_extension(extensions, "GL_OES_depth24") ?
        GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16;
}
#endif

static void gles_drawable_depth_allocate(GLenum format, unsigned width,
                                         unsigned height, GLuint *depth,
                                         GLuint *stencil)
{
    *stencil = 0;
    *depth = gles_private_name(glIsRenderbufferEXT);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, *depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, format, width, height);
#ifdef GLES_HOST_EGL_ES
    if (format != GL_DEPTH24_STENCIL8_EXT) {
        *stencil = gles_private_name(glIsRenderbufferEXT);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, *stencil);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_STENCIL_INDEX8,
                                 width, height);
        /* Match the pre-existing initial private renderbuffer binding. */
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, *depth);
    }
#endif
}

static void gles_drawable_depth_attach(GLuint depth, GLuint stencil)
{
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, stencil ? stencil : depth);
}

static void gles_drawable_depth_delete(GLuint depth, GLuint stencil)
{
    glDeleteRenderbuffersEXT(1, &depth);
    if (stencil && stencil != depth) glDeleteRenderbuffersEXT(1, &stencil);
}

/* A successful resize replaces private bindings but preserves guest bindings.
 * Zero is not a private separate stencil name when packed storage is used. */
static GLint gles_drawable_depth_remap(GLint binding, GLuint old_depth,
                                      GLuint old_stencil, GLuint depth,
                                      GLuint stencil)
{
    if ((GLuint)binding == old_depth) return depth;
    if (old_stencil && (GLuint)binding == old_stencil) return stencil;
    return binding;
}
