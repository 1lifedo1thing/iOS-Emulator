/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../hw/arm/ipod-gpio-wake-trace.c.inc"

int main(void)
{
    IpodGPIOWakeTraceBudget reads = {0}, edges = {0};
    for (unsigned i = 0; i < 1000; i++) {
        assert(!ipod_gpio_wake_trace_take(&reads, false, true, 128));
        assert(!ipod_gpio_wake_trace_take(&reads, true, false, 128));
    }
    assert(reads.used == 0);
    for (unsigned i = 0; i < 128; i++) {
        assert(ipod_gpio_wake_trace_take(&reads, true, true, 128));
        assert(reads.used == i + 1);
    }
    for (unsigned i = 0; i < 1000; i++) {
        assert(!ipod_gpio_wake_trace_take(&reads, true, true, 128));
        /* Toggling tracing cannot refill the process-wide budget. */
        assert(!ipod_gpio_wake_trace_take(&reads, false, true, 128));
    }
    assert(reads.used == 128);
    for (unsigned i = 0; i < 16; i++) {
        assert(ipod_gpio_wake_trace_take(&edges, true, true, 16));
    }
    assert(!ipod_gpio_wake_trace_take(&edges, true, true, 16));
    assert(edges.used == 16 && reads.used == 128);
    puts("PASS GPIO wake tracing is disabled by default, filtered, independently bounded and cannot be refilled by toggling");
    return 0;
}
