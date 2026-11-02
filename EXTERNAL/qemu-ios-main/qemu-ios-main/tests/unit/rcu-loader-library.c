/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only observation of public RCU registration pairing. Production rcu.c
 * is compiled with function instrumentation; no registry memory is inspected. */
#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include <stdatomic.h>
#include <sys/wait.h>
#ifdef RCU_TEST_EMBEDDED
#include "system/system.h"
#include "system/replay.h"
#include "qemu-ios-ui.h"
#endif

static _Atomic int registrations;
static _Atomic bool started, finished;
int rcu_unit_balance(void);
void rcu_unit_release_loader(void);
void rcu_unit_reader_pairing(void);
void __cyg_profile_func_enter(void *, void *);
void __cyg_profile_func_exit(void *, void *);
void __cyg_profile_func_enter(void *function, void *caller)
{
    if (function == (void *)rcu_register_thread) {
        atomic_fetch_add(&registrations, 1);
    }
}
void __cyg_profile_func_exit(void *function, void *caller)
{
    if (function == (void *)rcu_unregister_thread) {
        atomic_fetch_sub(&registrations, 1);
    }
}
int rcu_unit_balance(void) { return atomic_load(&registrations); }
void rcu_unit_release_loader(void) { rcu_unregister_thread(); }
static void *synchronizer(void *unused)
{
    rcu_register_thread();
    atomic_store(&started, true);
    synchronize_rcu();
    atomic_store(&finished, true);
    rcu_unregister_thread();
    return NULL;
}
static void reader_barrier(void)
{
    pthread_t thread;
    assert(rcu_thread_is_registered());
    atomic_store(&started, false);
    atomic_store(&finished, false);
    rcu_read_lock();
    assert(!pthread_create(&thread, NULL, synchronizer, NULL));
    while (!atomic_load(&started)) usleep(1000);
    usleep(100000);
    assert(!atomic_load(&finished));
    rcu_read_unlock();
    assert(!pthread_join(thread, NULL));
    assert(atomic_load(&finished));
}
void rcu_unit_reader_pairing(void)
{
    assert(!rcu_thread_is_registered());
    rcu_register_thread();
    reader_barrier();
    rcu_unregister_thread();
    assert(rcu_unit_balance() == 1); /* The permanent call_rcu worker only. */
}

void rcu_unit_entry_pairing(void);
void rcu_unit_fork_pairing(void);
void rcu_unit_fork_pairing(void)
{
    int status;
    rcu_register_thread();
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        /* The child registry is rebuilt, even if the parent was registered. */
        assert(rcu_thread_is_registered());
        reader_barrier();
        rcu_unregister_thread();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(rcu_thread_is_registered());
    rcu_unregister_thread();
    assert(rcu_unit_balance() == 1);
}
#ifdef RCU_TEST_EMBEDDED
/* The real shared entry runs with a minimal VM harness. Its RCU ownership,
 * init/loop/cleanup ordering, and grace-period participation are production code. */
int autostart;
void qemu_init(int argc, char **argv) { assert(rcu_thread_is_registered()); }
void qemu_cleanup(int status) { assert(status == 7 && rcu_thread_is_registered()); }
int qemu_main_loop(void) { reader_barrier(); return 7; }
void qemu_ios_ui_vm_started(void) { assert(rcu_thread_is_registered()); }
void qemu_ios_ui_vm_stopped(void) { assert(rcu_thread_is_registered()); }
void replay_mutex_lock(void) { }
void replay_mutex_unlock(void) { }
void rcu_unit_entry_pairing(void)
{
    assert(!rcu_thread_is_registered());
    assert(qemu_ios_main(0, NULL) == 7);
    assert(!rcu_thread_is_registered() && rcu_unit_balance() == 1);
    rcu_register_thread();
    assert(qemu_ios_main(0, NULL) == 7);
    assert(rcu_thread_is_registered() && rcu_unit_balance() == 2);
    rcu_unregister_thread();
    assert(rcu_unit_balance() == 1);
}
#else
void rcu_unit_entry_pairing(void) { }
#endif
