/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AIO one-shot BHs are a stack within each poll slice. One drain owns the
 * portable input FIFO so admission order survives delayed guest execution. */
#if !defined(__APPLE__)
typedef struct IosQueuedInput {
    QEMUBHFunc *callback;
    GDestroyNotify discard;
    void *opaque;
} IosQueuedInput;

static GMutex ios_input_mutex;
static GQueue ios_input_queue = G_QUEUE_INIT;
static bool ios_input_scheduled;

static void ios_input_drain(void *unused)
{
    /* Yield after a bounded batch, preserving the tail for the next poll. */
    for (unsigned n = 0; n < 64; n++) {
        g_mutex_lock(&ios_input_mutex);
        IosQueuedInput *item = g_queue_pop_head(&ios_input_queue);
        if (!item) {
            ios_input_scheduled = false;
            g_mutex_unlock(&ios_input_mutex);
            return;
        }
        g_mutex_unlock(&ios_input_mutex);
        /* Drain and VM teardown run under the BQL. A request detached from
         * the queue must still be disposed if the session is no longer live. */
        if (qemu_ios_ui_ready()) {
            item->callback(item->opaque);
        } else if (item->discard) {
            item->discard(item->opaque);
        }
        g_free(item);
    }
    g_mutex_lock(&ios_input_mutex);
    if (g_queue_is_empty(&ios_input_queue)) {
        ios_input_scheduled = false;
    } else {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_input_drain, NULL);
    }
    g_mutex_unlock(&ios_input_mutex);
}

static void ios_input_discard_pending(void)
{
    for (;;) {
        g_mutex_lock(&ios_input_mutex);
        IosQueuedInput *item = g_queue_pop_head(&ios_input_queue);
        if (!item) {
            /* An already scheduled drain retains its flag until it observes
             * this empty queue. New admissions are closed by ios_vm_alive. */
            g_mutex_unlock(&ios_input_mutex);
            return;
        }
        g_mutex_unlock(&ios_input_mutex);
        if (item->discard) {
            item->discard(item->opaque);
        }
        g_free(item);
    }
}
#endif

bool ios_input_enqueue(QEMUBHFunc *callback, void *opaque,
                       GDestroyNotify discard)
{
#if defined(__APPLE__)
    aio_bh_schedule_oneshot(qemu_get_aio_context(), callback, opaque);
    return true;
#else
    IosQueuedInput *item = g_new(IosQueuedInput, 1);
    *item = (IosQueuedInput) { callback, discard, opaque };
    g_mutex_lock(&ios_input_mutex);
    if (!qemu_ios_ui_ready()) {
        g_mutex_unlock(&ios_input_mutex);
        g_free(item);
        return false; /* Caller still owns opaque on rejected admission. */
    }
    g_queue_push_tail(&ios_input_queue, item);
    if (!ios_input_scheduled) {
        ios_input_scheduled = true;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_input_drain, NULL);
    }
    g_mutex_unlock(&ios_input_mutex);
    return true;
#endif
}
