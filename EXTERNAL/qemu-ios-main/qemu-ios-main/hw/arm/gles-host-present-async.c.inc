/*
 * The present's copy on a worker (Android, the AHardwareBuffer path; LT_PRESENT_ASYNC=off for A/B).
 *
 * A present waited for the GPU to finish the frame and copied it, 2.4 MB at 960x640, into the
 * guest's IOSurface, all on the vCPU: ~1 ms a frame on the Pixel 10 and the Poco. Here the vCPU
 * takes a native fence after the frame's commands, resolves the surface's pages to host pointers
 * (as a guest store would: plain RAM in the caller's TLB, else the synchronous path) and returns;
 * the worker waits on the fence in AHardwareBuffer_lock, copies and unlocks. Like the synchronous
 * path's guest_rw_ram, only pages with no TLB flags (so no dirty tracking to feed) are taken.
 *
 * Who reads the frame waits for it (gles_present_async_settle): the next present, the game
 * context's next GL call (it would draw into the buffer being read), any surface bind, the scaler
 * before it reads a source and the display before it latches (gles_present_wait_hook). CoreAnimation
 * hands out three surfaces in turn and the scaler turns the one presented before this one, so in
 * practice nothing waits. A guest CPU read of the surface within ~1 ms of its present could see the
 * previous frame there; nothing in iOS's display path reads it that way.
 *
 * With the GL thread (gles-host-queue.c.inc) the present is queued as well (LT_PRESENT_QUEUE=off
 * for A/B): the vCPU makes the present's checks and resolves the surface's pages
 * (gles_present_async_prepare), and the GL thread takes the fence when it gets to the present, so
 * the vCPU no longer waits for the frame's queued calls. Presents are numbered: `issued` counts the
 * ones handed out, `done` the ones whose frame is in guest memory (or that were given up), and a
 * device reading frames waits for done to reach issued. The GL thread itself waits only for what
 * it has taken, never for a present still behind it in the queue.
 */
#include "hw/arm/gles-present.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "system/runstate.h"

static GHashTable *gles_contexts;      /* ctx -> GLESHost (defined with the context ops) */

/* A queued present's surface pages, resolved by the vCPU. */
typedef struct GlesPresentPrep {
    uint64_t seq;
    size_t npages;
    uint8_t *pages[];
} GlesPresentPrep;

typedef EGLSyncKHR (*GlesCreateSync)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*GlesDestroySync)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*GlesDupFence)(EGLDisplay, EGLSyncKHR);

static struct {
    int state;                  /* -1 unknown, 0 unavailable or off, 1 ready */
    GlesCreateSync create_sync;
    GlesDestroySync destroy_sync;
    GlesDupFence dup_fence;
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    bool queued;                /* the worker has not taken it yet */
    bool queue;                 /* presents may be queued on the GL thread */
    uint64_t issued;            /* presents numbered */
    uint64_t done;              /* presents in guest memory or given up (written under the lock) */
    uint64_t taken;             /* the last present the GL thread handled */
    /* the job */
    uint64_t seq;
    uint32_t ctx;
    int fence;
    AHardwareBuffer *buffer;
    bool rgba;
    size_t src_stride;
    uint32_t rw, rh, stride, offset;
    uint8_t **pages;            /* host address of each destination page */
    size_t npages, cap;
    uint8_t *row;               /* the RGBA host's swizzled row */
    size_t row_len;
} gpa = { .state = -1 };

/* The queued present the GL thread is running, if it is running one. */
static __thread GlesPresentPrep *gpa_prep;

static void gles_present_async_mark_done(uint64_t seq)
{
    qemu_mutex_lock(&gpa.lock);
    if (gpa.done < seq) {
        qatomic_store_release(&gpa.done, seq);
    }
    qemu_cond_broadcast(&gpa.cond);
    qemu_mutex_unlock(&gpa.lock);
}

static void gles_present_async_copy(void)
{
    const uint8_t *frame = NULL;
    size_t row_bytes = (size_t)gpa.rw * 4;

    if (gles_ahb_api.lock(gpa.buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, gpa.fence, NULL,
                          (void **)&frame) != 0) {
        frame = NULL;   /* the lock closes the fence either way */
    }
    gpa.fence = -1;
    if (frame) {
        for (uint32_t y = 0; y < gpa.rh; y++) {
            const uint8_t *src = frame + (size_t)(gpa.rh - 1 - y) * gpa.src_stride;
            uint64_t off = gpa.offset + (uint64_t)y * gpa.stride;

            if (gpa.rgba) {
                const uint32_t *s = (const uint32_t *)src;
                uint32_t *d = (uint32_t *)gpa.row;

                for (uint32_t i = 0; i < gpa.rw; i++) {
                    uint32_t v = s[i];
                    d[i] = (v & 0xff00ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16;
                }
                src = gpa.row;
            }
            for (size_t done = 0; done < row_bytes;) {
                uint64_t at = off + done;
                size_t in = at & ~TARGET_PAGE_MASK, n = MIN(row_bytes - done, TARGET_PAGE_SIZE - in);

                memcpy(gpa.pages[at >> TARGET_PAGE_BITS] + in, src + done, n);
                done += n;
            }
        }
        gles_ahb_api.unlock(gpa.buffer, NULL);
    }
}

static void *gles_present_async_worker(void *opaque)
{
    qemu_mutex_lock(&gpa.lock);
    for (;;) {
        while (!gpa.queued) {
            qemu_cond_wait(&gpa.cond, &gpa.lock);
        }
        gpa.queued = false;
        qemu_mutex_unlock(&gpa.lock);
        gles_present_async_copy();
        qemu_mutex_lock(&gpa.lock);
        qatomic_store_release(&gpa.done, gpa.seq);
        qemu_cond_broadcast(&gpa.cond);
    }
    return NULL;
}

/* Until present `seq` and those before it are in guest memory. */
static void gles_present_async_settle_to(uint64_t seq)
{
    if (qatomic_load_acquire(&gpa.done) >= seq) {
        return;
    }
    qemu_mutex_lock(&gpa.lock);
    while (gpa.done < seq) {
        qemu_cond_wait(&gpa.cond, &gpa.lock);
    }
    qemu_mutex_unlock(&gpa.lock);
}

/* On the GL thread: until the presents it has taken are in guest memory. */
static void gles_present_async_settle(void)
{
    gles_present_async_settle_to(qatomic_read(&gpa.taken));
}

/*
 * A device about to read frames (the scaler, the display): until every present handed out is in
 * guest memory. The GL thread never takes the BQL, but the display waits with it held, so it gives
 * up after a while rather than trust that forever.
 */
static void gles_present_async_hook(void)
{
    uint64_t seq = qatomic_read(&gpa.issued);

    if (qatomic_load_acquire(&gpa.done) >= seq) {
        return;
    }
    if (!bql_locked()) {
        gles_present_async_settle_to(seq);
        return;
    }
    qemu_mutex_lock(&gpa.lock);
    while (gpa.done < seq) {
        if (!qemu_cond_timedwait(&gpa.cond, &gpa.lock, 200)) {
            static bool told;

            if (!told) {
                told = true;
                fprintf(stderr, "[gles] present %" PRIu64 " not in guest memory after 200 ms; "
                        "the display goes on without it\n", seq);
            }
            break;
        }
    }
    qemu_mutex_unlock(&gpa.lock);
}

/* A stopped machine's RAM is about to be saved or read. */
static void gles_present_async_vm_state(void *opaque, bool running, RunState state)
{
    if (!running) {
        gles_present_async_settle_to(qatomic_read(&gpa.issued));
    }
}

static bool gles_present_async_ready(void)
{
    if (gpa.state < 0) {
        const char *v = getenv("LT_PRESENT_ASYNC");

        gpa.state = 0;
        if ((v && (!strcmp(v, "off") || !strcmp(v, "0"))) || getenv("IT_GLES_FRAME_METRICS") ||
            getenv("IT_GLES_VERBOSE") || getenv("IT_GLES_DUMP_DIR") || gles_prof) {
            return false;   /* the frame-pixel reports and dumps read the frame on the vCPU */
        }
        gpa.create_sync = (GlesCreateSync)eglGetProcAddress("eglCreateSyncKHR");
        gpa.destroy_sync = (GlesDestroySync)eglGetProcAddress("eglDestroySyncKHR");
        gpa.dup_fence = (GlesDupFence)eglGetProcAddress("eglDupNativeFenceFDANDROID");
        if (!gpa.create_sync || !gpa.destroy_sync || !gpa.dup_fence) {
            return false;
        }
        qemu_mutex_init(&gpa.lock);
        qemu_cond_init(&gpa.cond);
        gpa.fence = -1;
        v = getenv("LT_PRESENT_QUEUE");
        gpa.queue = !(v && (!strcmp(v, "off") || !strcmp(v, "0"))) &&
                    !getenv("IT_GLES_PANEL_ALSO") && !getenv("LT_GLES_SURFACE_PATH");
        qemu_thread_create(&gpa.thread, "gles-present", gles_present_async_worker, NULL,
                           QEMU_THREAD_DETACHED);
        qatomic_set(&gles_present_wait_hook, gles_present_async_hook);
        qemu_add_vm_change_state_handler(gles_present_async_vm_state, NULL);
        qatomic_store_release(&gpa.state, 1);    /* gpa.queue and the lock first */
    }
    return gpa.state > 0;
}

/*
 * Queue the copy of the drawable's frame into the surface at guest `base` (the calling process's
 * address). False, having changed nothing, when it must be done here. A queued present (gpa_prep)
 * brings its pages and does not fail for want of a fence: it finishes the frame here instead.
 */
static bool gles_present_async_submit(CPUState *cpu, uint32_t base, uint32_t stride, uint32_t rw,
                                      uint32_t rh)
{
    static const EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE,
    };
    GlesPresentPrep *prep = gpa_prep;
    uint64_t first = base & TARGET_PAGE_MASK, end = (uint64_t)base + (uint64_t)(rh - 1) * stride + rw * 4;
    size_t npages = (end - first + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS;
    AHardwareBuffer_Desc desc;
    EGLSyncKHR sync;
    uint64_t seq;
    int fence = -1;

    if (!gles_present_async_ready() || !gh.ahb.buffer || !rh || end > UINT32_MAX ||
        (prep && prep->npages != npages)) {
        return false;
    }
    seq = prep ? prep->seq : qatomic_read(&gpa.issued) + 1;
    gles_present_async_settle_to(seq - 1);
    if (gpa.cap < npages) {
        gpa.pages = g_renew(uint8_t *, gpa.pages, npages);
        gpa.cap = npages;
    }
    if (prep) {
        memcpy(gpa.pages, prep->pages, npages * sizeof(*gpa.pages));
    } else {
        CPUArchState *env = cpu_env(cpu);
        int idx = cpu_mmu_index(cpu, false);

        for (size_t i = 0; i < npages; i++) {
            void *host;

            if (probe_access_flags(env, first + i * TARGET_PAGE_SIZE, 0, MMU_DATA_STORE, idx, true,
                                   &host, 0) || !host) {
                return false;
            }
            gpa.pages[i] = host;
        }
    }
    sync = gpa.create_sync(gles_egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync != EGL_NO_SYNC_KHR) {
        glFlush();
        fence = gpa.dup_fence(gles_egl_display, sync);
        gpa.destroy_sync(gles_egl_display, sync);
    }
    if (fence < 0) {
        if (!prep) {
            return false;
        }
        glFinish();
    }
    if (!prep) {
        qatomic_set(&gpa.issued, seq);
    }
    gles_ahb_api.describe(gh.ahb.buffer, &desc);
    if (gh.ahb.rgba && gpa.row_len < (size_t)rw * 4) {
        g_free(gpa.row);
        gpa.row = g_malloc((size_t)rw * 4);
        gpa.row_len = (size_t)rw * 4;
    }
    qemu_mutex_lock(&gpa.lock);
    gpa.seq = seq;
    gpa.ctx = gles_cur_ctx;
    gpa.fence = fence;
    gpa.buffer = gh.ahb.buffer;
    gpa.rgba = gh.ahb.rgba;
    gpa.src_stride = (size_t)desc.stride * 4;
    gpa.rw = rw;
    gpa.rh = rh;
    gpa.stride = stride;
    gpa.offset = base & ~TARGET_PAGE_MASK;
    gpa.npages = npages;
    qatomic_set(&gpa.taken, seq);
    gpa.queued = true;
    qemu_cond_broadcast(&gpa.cond);
    qemu_mutex_unlock(&gpa.lock);
    return true;
}

/* A GL call from the context whose frame is still being read would draw into that buffer. */
static inline void gles_present_async_before_call(uint32_t ctx)
{
    if (ctx == gpa.ctx && qatomic_load_acquire(&gpa.done) < gpa.seq) {
        gles_present_async_settle_to(gpa.seq);
    }
}

/*
 * On the vCPU: a present the GL thread can run later, or NULL to run it now. The checks are the
 * present's own (gles_present_to_surface_impl's fast path) on the context's state, which only
 * calls the vCPU waits for change; the surface's pages are resolved through the vCPU's TLB as a
 * guest store would.
 */
static GlesPresentPrep *gles_present_async_prepare(CPUState *cpu, uint32_t ctx, const uint32_t *a)
{
    GLESHost *h = gles_contexts ? g_hash_table_lookup(gles_contexts, GUINT_TO_POINTER(ctx)) : NULL;
    uint32_t base = a[0], stride = a[1], w = a[2], rh = a[3];
    uint64_t first = base & TARGET_PAGE_MASK, end = (uint64_t)base + (uint64_t)(rh - 1) * stride + w * 4;
    CPUArchState *env = cpu_env(cpu);
    int idx = cpu_mmu_index(cpu, false);
    GlesPresentPrep *prep;
    size_t npages;

    if (qatomic_load_acquire(&gpa.state) <= 0 || !qatomic_read(&gpa.queue) || !h ||
        !h->ahb.buffer || !h->iosurface || gles_swizzle || a[4] != GLES_SURFACE_BGRA32 || !base || !w || !rh ||
        !h->drawable_announced || w != h->drawable_width || rh != h->drawable_height ||
        stride < w * 4 || end > UINT32_MAX) {
        return NULL;
    }
    npages = (end - first + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS;
    prep = g_malloc(sizeof(*prep) + npages * sizeof(prep->pages[0]));
    prep->npages = npages;
    for (size_t i = 0; i < npages; i++) {
        void *host;

        if (probe_access_flags(env, first + i * TARGET_PAGE_SIZE, 0, MMU_DATA_STORE, idx, true,
                               &host, 0) || !host) {
            g_free(prep);
            return NULL;
        }
        prep->pages[i] = host;
    }
    prep->seq = qatomic_read(&gpa.issued) + 1;
    qatomic_set(&gpa.issued, prep->seq);
    return prep;
}

/* On the GL thread, around a queued present: one that never reached the copy counts as done. */
static void gles_present_async_run(GlesPresentPrep *prep, bool begin)
{
    if (begin) {
        gpa_prep = prep;
        return;
    }
    gpa_prep = NULL;
    if (qatomic_read(&gpa.taken) < prep->seq) {
        gles_present_async_settle_to(prep->seq - 1);
        qatomic_set(&gpa.taken, prep->seq);
        gles_present_async_mark_done(prep->seq);
    }
}

/* The GL thread refused a queued present's guest access: present on the vCPU's turn from now on. */
static void gles_present_async_no_queue(void)
{
    qatomic_set(&gpa.queue, false);
}
