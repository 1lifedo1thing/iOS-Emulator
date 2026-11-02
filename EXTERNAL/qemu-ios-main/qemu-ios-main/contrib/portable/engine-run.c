/* SPDX-License-Identifier: GPL-2.0-or-later
 * Thin private acceptance runner. One fresh process per engine session.
 * Usage: engine-run /absolute/libqemu-arm.so qemu [QEMU arguments...]
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: engine-run LIB qemu [arguments...]\n"); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "engine load: %s\n", dlerror()); return 1; }
    uint32_t (*api)(void) = (uint32_t (*)(void))dlsym(lib, "qemu_ios_api_version");
    uint32_t (*portable)(void) = (uint32_t (*)(void))dlsym(lib, "qemu_ios_portable_version");
    int (*entry)(int, char **) = (int (*)(int, char **))dlsym(lib, "qemu_ios_main");
    if (!api || !portable || !entry || api() >> 16 != 2 || portable() != 1) {
        fprintf(stderr, "incompatible engine ABI\n"); return 1;
    }
    return entry(argc - 2, argv + 2);
}
