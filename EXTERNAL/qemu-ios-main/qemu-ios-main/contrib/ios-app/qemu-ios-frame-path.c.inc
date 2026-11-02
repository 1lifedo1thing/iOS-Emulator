/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Bounded one-shot diagnostic, independent of user profiling. Producers perform
 * no file I/O/allocation/locks. Dump only after qemu_cleanup has stopped vCPU
 * and device activity. A killed worker leaves a header without the footer. */
#define IOS_FP_CAPACITY 131072
static unsigned ios_fp_capacity = IOS_FP_CAPACITY;
static struct IOSFramePathRecord {
    uint64_t ns, a, b, c;
    unsigned event;
    unsigned ready;
} *ios_fp_records;
static FILE *ios_fp_file;
static uint64_t ios_fp_count;
static bool ios_fp_on;
static bool ios_fp_context;
/* LT_FRAME_PATH_DELAY_MS: record nothing until this long after start, so a capture of a game's steady state
 * is not spent on boot and loading (the context trace fills its buffer in under a minute). */
static int64_t ios_fp_start_ns;

void qemu_ios_frame_path_init(void)
{
    const char *path = getenv("LT_FRAME_PATH");
    if (!path || !*path || !strcmp(path, "off") || !strcmp(path, "0")) {
        return;
    }
    ios_fp_file = fopen(path, "w");
    if (!ios_fp_file) {
        fprintf(stderr, "[frame-path] open failed: %s\n", strerror(errno));
        return;
    }
    const char *context = getenv("LT_FRAME_PATH_CONTEXT");
    const char *audio = getenv("LT_AUDIO_PATH_PROF");
    const char *guest = getenv("LT_GUEST_TRACE_PC");
    const char *wait = getenv("LT_NATIVE_WAIT_PROF");
    const char *ahb = getenv("LT_GLES_AHB_PATH");
    const char *fmss = getenv("LT_FMSS_PATH");
    const char *surface = getenv("LT_GLES_SURFACE_PATH");
    const char *bt = getenv("LT_BT_PATH");
    bool audio_on = audio && strcmp(audio, "off") && strcmp(audio, "0");
    bool guest_on = guest && *guest && strcmp(guest, "off") && strcmp(guest, "0");
    bool wait_on = wait && (!strcmp(wait, "on") || !strcmp(wait, "conditions"));
    bool ahb_on = ahb && !strcmp(ahb, "on");
    bool fmss_on = fmss && !strcmp(fmss, "on");
    bool surface_on = surface && !strcmp(surface, "on");
    bool bt_on = bt && !strcmp(bt, "on");
    ios_fp_context = context && strcmp(context, "off") && strcmp(context, "0");
    ios_fp_capacity = IOS_FP_CAPACITY *
                      (audio_on || guest_on || wait_on ? 16 :
                       ios_fp_context || ahb_on || fmss_on || surface_on || bt_on ? 8 : 1);
    ios_fp_records = g_try_new0(struct IOSFramePathRecord, ios_fp_capacity);
    if (!ios_fp_records) {
        fprintf(stderr, "[frame-path] allocation failed\n");
        fclose(ios_fp_file);
        ios_fp_file = NULL;
        return;
    }
    const char *delay = getenv("LT_FRAME_PATH_DELAY_MS");
    if (delay && *delay) {
        ios_fp_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + strtoll(delay, NULL, 10) * SCALE_MS;
    }
    fprintf(ios_fp_file, "# LT_FRAME_PATH 1 capacity=%u begin-ns=%" PRId64 "\n",
            ios_fp_capacity, MAX(ios_fp_start_ns, qemu_clock_get_ns(QEMU_CLOCK_REALTIME)));
    fflush(ios_fp_file);
    qatomic_store_release(&ios_fp_on, true);
    if (qemu_ios_native_wait_init) {
        qemu_ios_native_wait_init();
    }
    fprintf(stderr, "[frame-path] on capacity=%u context=%s; writes deferred until cleanup\n",
            ios_fp_capacity, ios_fp_context ? "on" : "off");
}

void qemu_ios_frame_path_event(unsigned event, uint64_t a, uint64_t b, uint64_t c)
{
    if (!qatomic_load_acquire(&ios_fp_on)) {
        return;
    }
    if ((event == IOS_FP_CONTEXT_SWITCH || event == IOS_FP_TTBR0) && !ios_fp_context) {
        return;
    }
    if (ios_fp_start_ns && qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < ios_fp_start_ns) {
        return;
    }
    uint64_t index = qatomic_fetch_inc(&ios_fp_count);
    if (index >= ios_fp_capacity) {
        return;
    }
    struct IOSFramePathRecord *record = &ios_fp_records[index];
    record->ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    record->a = a;
    record->b = b;
    record->c = c;
    record->event = event;
    qatomic_store_release(&record->ready, 1);
}

void qemu_ios_frame_path_finish(void)
{
    static const char *const names[] = {
        "invalid", "present-begin", "present-end", "lcd-flip", "vsync-begin",
        "vsync-end", "scan-begin", "scan-end", "capture-begin", "capture-same",
        "publish", "context-switch", "ttbr0", "vfp-sample", "vfp-result",
        "alignment", "alignment-begin", "alignment-return", "alignment-abandon",
        "alignment-args", "alignment-caller",
        "guest-pc", "guest-regs", "guest-stack",
        "native-wait",
        "ahb-stage-begin", "ahb-stage-end",
        "fmss-begin", "fmss-end",
        "surface-map-begin", "surface-map-end",
        "bt-command", "bt-queue", "bt-flush", "bt-state",
    };
    qatomic_store_release(&ios_fp_on, false);
    if (qemu_ios_native_wait_finish) {
        qemu_ios_native_wait_finish();
    }
    if (qemu_ios_bql_spin_finish) {
        qemu_ios_bql_spin_finish();
    }
    if (!ios_fp_file) {
        return;
    }
    uint64_t count = qatomic_read(&ios_fp_count);
    uint64_t retained = MIN(count, ios_fp_capacity), incomplete = 0;
    for (uint64_t i = 0; i < retained; i++) {
        struct IOSFramePathRecord *r = &ios_fp_records[i];
        if (!qatomic_load_acquire(&r->ready)) {
            incomplete++;
            continue;
        }
        const char *name = r->event < ARRAY_SIZE(names) ? names[r->event] : "invalid";
        fprintf(ios_fp_file, "%" PRIu64 " %s %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                r->ns, name, r->a, r->b, r->c);
    }
    fprintf(ios_fp_file, "# end records=%" PRIu64 " dropped=%" PRIu64
            " incomplete=%" PRIu64 " end-ns=%" PRId64 "\n",
            retained - incomplete, count - retained, incomplete,
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    bool failed = ferror(ios_fp_file);
    if (fclose(ios_fp_file)) {
        failed = true;
    }
    fprintf(stderr, "[frame-path] finished records=%" PRIu64 " dropped=%" PRIu64
            " incomplete=%" PRIu64 " write-error=%s\n",
            retained - incomplete, count - retained, incomplete,
            failed ? "yes" : "no");
    ios_fp_file = NULL;
    g_clear_pointer(&ios_fp_records, g_free);
}
