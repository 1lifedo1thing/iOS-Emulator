/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Optional single-vCPU wait attribution. No hooks are installed by default.
 * Only completed waits >=10us enter the bounded frame-path buffer. Site names
 * are dumped after all vCPUs have stopped, never from a timed wait producer.
 */
#define NATIVE_WAIT_SITES 128
static struct {
    const char *file;
    int line;
    unsigned kind;
    void *object;
    QemuMutex *mutex;
} native_wait_sites[NATIVE_WAIT_SITES];
static unsigned native_wait_site_count;
static uint64_t native_wait_calls, native_wait_slow, native_wait_site_overflow;
static int native_wait_tid;
static bool native_wait_on;
static bool native_wait_conditions_only;
static QemuMutexLockFunc native_wait_bql_original, native_wait_mutex_original;

static int64_t native_wait_begin(void)
{
    return current_cpu && current_cpu->cpu_index == 0 ? get_clock() : 0;
}

static void native_wait_end(int64_t begin, unsigned kind, void *object,
                            QemuMutex *mutex, const char *file, int line)
{
    if (!begin) {
        return;
    }
    int64_t end = get_clock();
    native_wait_calls++;
    if (end - begin < 10000) {
        return;
    }
    native_wait_slow++;
    native_wait_tid = current_cpu->thread_id;
    unsigned site;
    for (site = 0; site < native_wait_site_count; site++) {
        if (native_wait_sites[site].file == file &&
            native_wait_sites[site].line == line &&
            native_wait_sites[site].kind == kind &&
            native_wait_sites[site].object == object &&
            native_wait_sites[site].mutex == mutex) {
            break;
        }
    }
    if (site == NATIVE_WAIT_SITES) {
        native_wait_site_overflow++;
        return;
    }
    if (site == native_wait_site_count) {
        native_wait_sites[site].file = file;
        native_wait_sites[site].line = line;
        native_wait_sites[site].kind = kind;
        native_wait_sites[site].object = object;
        native_wait_sites[site].mutex = mutex;
        native_wait_site_count++;
    }
    ios_frame_path(IOS_FP_NATIVE_WAIT, begin, end, site);
}

static void native_wait_mutex(QemuMutex *mutex, const char *file, int line)
{
    int64_t begin = native_wait_begin();
    qemu_mutex_lock_impl(mutex, file, line);
    native_wait_end(begin, mutex == &bql ? 1 : 2, mutex, mutex, file, line);
}

static void native_wait_cond(QemuCond *cond, QemuMutex *mutex,
                              const char *file, int line)
{
    int64_t begin = native_wait_begin();
    qemu_cond_wait_impl(cond, mutex, file, line);
    native_wait_end(begin, 3, cond, mutex, file, line);
}

static bool native_wait_timedcond(QemuCond *cond, QemuMutex *mutex, int ms,
                                  const char *file, int line)
{
    int64_t begin = native_wait_begin();
    bool result = qemu_cond_timedwait_impl(cond, mutex, ms, file, line);
    native_wait_end(begin, 4, cond, mutex, file, line);
    return result;
}

void qemu_ios_native_wait_init(void)
{
    const char *mode = getenv("LT_NATIVE_WAIT_PROF");
    bool conditions = mode && !strcmp(mode, "conditions");
    if (!conditions && (!mode || strcmp(mode, "on"))) {
        return;
    }
    /* Conditions mode leaves both mutex hooks intact, so an existing BQL spin
     * experiment remains effective. Never replace another condition profiler.
     * Full mode retains the original refusal of any preexisting interposer. */
    native_wait_bql_original = qatomic_read(&bql_mutex_lock_func);
    native_wait_mutex_original = qatomic_read(&qemu_mutex_lock_func);
    if ((!conditions && (native_wait_bql_original != qemu_mutex_lock_impl ||
                         native_wait_mutex_original != qemu_mutex_lock_impl)) ||
        qatomic_read(&qemu_cond_wait_func) != qemu_cond_wait_impl ||
        qatomic_read(&qemu_cond_timedwait_func) != qemu_cond_timedwait_impl) {
        fprintf(stderr, "[native-wait] refused: wait hooks already replaced\n");
        return;
    }
    native_wait_conditions_only = conditions;
    if (!conditions) {
        qatomic_set(&bql_mutex_lock_func, native_wait_mutex);
        qatomic_set(&qemu_mutex_lock_func, native_wait_mutex);
    }
    qatomic_set(&qemu_cond_wait_func, native_wait_cond);
    qatomic_set(&qemu_cond_timedwait_func, native_wait_timedcond);
    native_wait_on = true;
    fprintf(stderr, "[native-wait] on cpu=0 threshold-ns=10000 sites=%u; "
            "site names deferred until cleanup\n", NATIVE_WAIT_SITES);
    fprintf(stderr, "[native-wait] mode=%s mutex-hooks=%s\n",
            conditions ? "conditions" : "full",
            conditions ? "unchanged" : "instrumented");
}

void qemu_ios_native_wait_finish(void)
{
    if (!native_wait_on) {
        return;
    }
    /* The embedding calls this only after qemu_cleanup has joined vCPUs. */
    bool hooks = qatomic_read(&bql_mutex_lock_func) ==
                    (native_wait_conditions_only ? native_wait_bql_original :
                                                  native_wait_mutex) &&
                 qatomic_read(&qemu_mutex_lock_func) ==
                    (native_wait_conditions_only ? native_wait_mutex_original :
                                                  native_wait_mutex) &&
                 qatomic_read(&qemu_cond_wait_func) == native_wait_cond &&
                 qatomic_read(&qemu_cond_timedwait_func) == native_wait_timedcond;
    for (unsigned i = 0; i < native_wait_site_count; i++) {
        fprintf(stderr, "[native-wait-site] id=%u kind=%u object=0x%" PRIxPTR
                " mutex=0x%" PRIxPTR " file=%s line=%d\n", i,
                native_wait_sites[i].kind,
                (uintptr_t)native_wait_sites[i].object,
                (uintptr_t)native_wait_sites[i].mutex,
                native_wait_sites[i].file, native_wait_sites[i].line);
    }
    fprintf(stderr, "[native-wait] finished tid=%d calls=%" PRIu64
            " retained=%" PRIu64 " site-overflow=%" PRIu64 " hooks=%s\n",
            native_wait_tid, native_wait_calls, native_wait_slow,
            native_wait_site_overflow, hooks ? "unchanged" : "replaced");
}
