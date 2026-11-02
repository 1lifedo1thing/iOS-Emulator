/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnostic-only budget; no GPIO, IRQ or guest clock state is modified. */
typedef struct IpodGPIOWakeTraceBudget {
    unsigned used;
} IpodGPIOWakeTraceBudget;

static bool ipod_gpio_wake_trace_take(IpodGPIOWakeTraceBudget *budget,
                                     bool enabled, bool relevant, unsigned limit)
{
    if (!enabled || !relevant || budget->used >= limit) {
        return false;
    }
    budget->used++;
    return true;
}
