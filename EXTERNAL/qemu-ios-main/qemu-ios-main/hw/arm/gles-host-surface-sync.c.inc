/* SPDX-License-Identifier: GPL-2.0-or-later */

static int64_t gles_sync_surface_1(CPUState *cpu)
{
    GLint kind = 0, texture = 0;
    (void)cpu;
    if (!gh.surfaces || !gh.bound_framebuffer) return 0;
    glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT,
        GL_COLOR_ATTACHMENT0_EXT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &kind);
    if (kind != GL_TEXTURE) return 0;
    glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT,
        GL_COLOR_ATTACHMENT0_EXT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &texture);
    GLESSurface *s = g_hash_table_lookup(gh.surfaces, GUINT_TO_POINTER(texture));
    if (!s) return 0;
    if (s->detached) {                  /* rendering into a plain texture: no longer the surface's copy */
        g_hash_table_remove(gh.surfaces, GUINT_TO_POINTER(texture));
        return 0;
    }
    if (s->format != GLES_SURFACE_BGRA32 && s->format != GLES_SURFACE_RGBA32 &&
        s->format != GLES_SURFACE_RGB565 && s->format != GLES_SURFACE_RGB555) {
        char fourcc[12];
        gles_refuse("surface:render:%s", gles_fourcc(s->format, fourcc));
        return -1;
    }
    s->dirty = true;
    return 0;
}

