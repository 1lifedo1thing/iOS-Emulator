/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Normal foreign pthread loads the DSO and exits. No RCU node manipulation. */
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void *library;
static const char *path;
static bool explicit_pair;
static int expected_initial;
static int (*balance)(void);
static void (*release_loader)(void), (*reader_pairing)(void), (*entry_pairing)(void);
static void (*fork_pairing)(void);
static void *load(void *unused)
{
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); abort(); }
    balance = dlsym(library, "rcu_unit_balance");
    release_loader = dlsym(library, "rcu_unit_release_loader");
    reader_pairing = dlsym(library, "rcu_unit_reader_pairing");
    entry_pairing = dlsym(library, "rcu_unit_entry_pairing");
    fork_pairing = dlsym(library, "rcu_unit_fork_pairing");
    assert(balance && release_loader && reader_pairing && entry_pairing && fork_pairing);
    while (balance() < expected_initial) usleep(1000);
    assert(balance() == expected_initial);
    if (explicit_pair) release_loader();
    return NULL;
}
int main(int argc, char **argv)
{
    pthread_t thread;
    assert(argc == 4);
    alarm(15);
    path = argv[1]; explicit_pair = atoi(argv[2]) != 0;
    expected_initial = atoi(argv[3]);
    assert(!pthread_create(&thread, NULL, load, NULL));
    assert(!pthread_join(thread, NULL));
    fprintf(stderr, "RCU registrations after foreign loader exit: %d (expected1)\n", balance());
    assert(balance() == 1);
    reader_pairing();
    entry_pairing();
    fork_pairing();
    puts("PASS loader cleanup and registered main-reader grace-period pairing");
    return 0; /* DSO has a permanent worker, so keep it mapped until exit. */
}
