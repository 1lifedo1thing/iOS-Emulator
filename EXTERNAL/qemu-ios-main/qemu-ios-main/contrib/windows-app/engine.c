/* SPDX-License-Identifier: GPL-2.0-or-later
 * Fresh Windows shared desktop worker. No embedding-entry/RCU constructor
 * override: initialize QEMU on the real main thread, then hand BQL/replay to
 * the main-loop thread exactly as system/main.c does for native host UIs.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "system/replay.h"
#include "system/system.h"
#include "system/runstate.h"
#include "qapi/qapi-commands-control.h"
#include "hw/arm/ipod-agent.h"
#include "../ios-app/qemu-ios-ui.h"
#include "engine.h"

/* Other QEMU sources refer to this main-loop hook. Original system/main.o is
 * replaced by this worker's frontend main at link, not linked a second time. */
int (*qemu_main)(void);
static QemuThread loop_thread;
static QEMUTimer *progress_timer;
static QEMUBH *control_bh;
static int pause_request = -1, stop_request;
static int loop_running, pc_pending;
static bool initialized, started, joined, stop_requested;
static bool shutdown_confirmed, storage_failed;
static int loop_exit;
static uint64_t progress_id;

static void progress_pc(CPUState *cpu, run_on_cpu_data data)
{
    /* Called on this vCPU; never read live TCG registers from the SDL thread. */
    vaddr pc = CPU_GET_CLASS(cpu)->get_pc(cpu);
    fprintf(stderr, "[windows-progress] sample=%" PRIu64 " cpu=%d pc=0x%" PRIx64 "\n",
            (uint64_t)data.host_ulong, cpu->cpu_index, (uint64_t)pc);
    qatomic_set(&pc_pending, 0);
}

static void progress(void *unused)
{
    bool confirmed = qemu_ios_ui_guest_shutdown_confirmed();
    bool failed = qemu_ios_ui_storage_failed();
    int state = failed ? 5 : confirmed || stop_requested ? 4 :
                runstate_check(RUN_STATE_PAUSED) ? 3 : 2;
    /* Shared launcher protocol; progress diagnostics alone aren't UI status. */
    printf("status state=%d confirmed=%d storage_failed=%d\n", state, confirmed, failed);
    fflush(stdout);
    if (confirmed && !failed) {
        /* Finish guest-slider shutdown too. Unlike a bare shutdown request,
         * qmp_quit changes -no-shutdown's PAUSE action to POWEROFF. */
        qmp_quit(NULL);
    }
    timer_mod(progress_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    /* Detailed diagnostics stay at five seconds, independent of UI status. */
    if (++progress_id % 5 != 1) {
        return;
    }
    QemuIosPortableFrame frame = { .size = sizeof(frame), .version = QEMU_IOS_PORTABLE_VERSION };
    int result = qemu_ios_portable_frame_copy(NULL, 0, 0, &frame);
    fprintf(stderr, "[windows-progress] sample=%" PRIu64 " realtime_ms=%" PRId64
            " virtual_ms=%" PRId64 " state=%s frame_result=%d serial=%" PRIu64
            " size=%ux%u sleeping=%d confirmed=%d storage_failed=%d pc_pending=%d\n",
            progress_id, qemu_clock_get_ms(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), RunState_str(runstate_get()),
            result, frame.serial, frame.width, frame.height,
            qemu_ios_ui_display_sleeping(), qemu_ios_ui_guest_shutdown_confirmed(),
            qemu_ios_ui_storage_failed(), qatomic_read(&pc_pending));
    if (first_cpu && !qatomic_read(&pc_pending)) {
        qatomic_set(&pc_pending, 1);
        async_run_on_cpu(first_cpu, progress_pc, RUN_ON_CPU_HOST_ULONG(progress_id));
    }
}

static void apply_control(void *unused)
{
    if (!qemu_ios_ui_ready()) return;
    if (qemu_ios_ui_storage_failed()) {
        qatomic_set(&stop_request, 0);
        qatomic_set(&pause_request, -1);
        fprintf(stderr, "[windows-control] storage failure latched; resume/Stop refused, writable state retained\n");
        return;
    }
    if (qatomic_xchg(&stop_request, 0)) {
        qemu_ios_ui_cancel_input();
        if (qemu_ios_ui_guest_shutdown_confirmed()) {
            stop_requested = true;
            qmp_quit(NULL);
            return;
        }
        if (!runstate_is_running()) vm_start();
        if (!stop_requested) {
            /* As qemu_ios_ui_shutdown: the agent's halt (where SpringBoard's slider ends), else the power-off gesture. */
            IPodAgent *a = ipod_agent_acquire();
            bool agent = a && !strcmp(ipod_agent_status(a, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)), "alive") &&
                         !ipod_agent_submit(a, "qemu-ios-shutdown halt\n");
            ipod_agent_free(a);
            stop_requested = true;
            if (!agent) {
                qemu_system_powerdown_request();
            }
            fprintf(stderr, "[windows-stop] requested (%s); waiting for actual guest PMU power-off\n",
                    agent ? "agent halt" : "power-off gesture");
        }
    }
    int pause = qatomic_xchg(&pause_request, -1);
    if (pause >= 0 && !stop_requested && !qemu_ios_ui_guest_shutdown_confirmed()) {
        qemu_ios_ui_cancel_input();
        if (pause) vm_stop(RUN_STATE_PAUSED); else vm_start();
    }
}

void lt_windows_engine_initialize(int argc, char **argv)
{
    assert(!initialized);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    qemu_init(argc, argv);
    autostart = 1;
    qemu_ios_ui_vm_started();
    control_bh = qemu_bh_new(apply_control, NULL);
    progress_timer = timer_new_ms(QEMU_CLOCK_REALTIME, progress, NULL);
    timer_mod(progress_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    initialized = true;
    bql_unlock();
    replay_mutex_unlock();
}

static void *main_loop(void *unused)
{
    bool owns_rcu = !rcu_thread_is_registered();
    if (owns_rcu) rcu_register_thread();
    replay_mutex_lock();
    bql_lock();
    loop_exit = qemu_main_loop();
    shutdown_confirmed = qemu_ios_ui_guest_shutdown_confirmed();
    storage_failed = qemu_ios_ui_storage_failed();
    timer_del(progress_timer);
    timer_free(progress_timer);
    progress_timer = NULL;
    qemu_ios_ui_vm_stopped();
    qemu_bh_delete(control_bh);
    control_bh = NULL;
    qemu_cleanup(loop_exit);
    /* Include failures latched while cleanup drains writable storage. */
    storage_failed |= qemu_ios_ui_storage_failed();
    bql_unlock();
    replay_mutex_unlock();
    if (owns_rcu) rcu_unregister_thread();
    /* Release after every cleanup write; UI joins before reading final proof. */
    qatomic_set(&loop_running, 0);
    return NULL;
}

void lt_windows_engine_start(void)
{
    assert(initialized && !started);
    started = true;
    qatomic_set(&loop_running, 1);
    qemu_thread_create(&loop_thread, "lt-windows-loop", main_loop, NULL, QEMU_THREAD_JOINABLE);
}

bool lt_windows_engine_running(void) { return qatomic_read(&loop_running) != 0; }

bool lt_windows_engine_pause(bool paused)
{
    bool accepted = false;
    bql_lock();
    if (qemu_ios_ui_ready() && !qemu_ios_ui_storage_failed() &&
        !qemu_ios_ui_guest_shutdown_confirmed() && !stop_requested && !qatomic_read(&stop_request)) {
        qatomic_set(&pause_request, paused ? 1 : 0);
        qemu_bh_schedule(control_bh);
        accepted = true; /* queued, not an execution receipt */
    }
    bql_unlock();
    return accepted;
}

bool lt_windows_engine_stop(void)
{
    bool accepted = false;
    bql_lock();
    if (qemu_ios_ui_ready() && !qemu_ios_ui_storage_failed()) {
        qatomic_set(&stop_request, 1);
        qemu_bh_schedule(control_bh);
        accepted = true;
    }
    bql_unlock();
    return accepted;
}

int lt_windows_engine_join(void)
{
    assert(started && !joined && !lt_windows_engine_running());
    qemu_thread_join(&loop_thread);
    joined = true;
    int clean = shutdown_confirmed && !storage_failed && loop_exit == 0;
    fprintf(stdout, "ENGINE_FINAL confirmed=%d storage_failed=%d engine_exit=%d clean=%d\n",
            shutdown_confirmed, storage_failed, loop_exit, clean);
    return clean ? 0 : 3;
}
