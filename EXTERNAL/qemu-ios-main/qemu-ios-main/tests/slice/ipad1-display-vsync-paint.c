/* Production A4 VBL and display_update: latch/IRQs precede visible painting.
 * The existing ipad1-display-relatch slice separately tests actual compositing.
 * SLICE:constants hw/arm/s5l8930_display.c define (DP_IRQ_STATUS|DP_IRQ_VBL|VBL_PERIOD_NS|QUIET_RELATCH_VBLS|CLCD_IRQ_FRAME|CLCD_IRQ_ENABLE|CLCD_IRQ_MASK)[[:space:]]
 * SLICE hw/arm/s5l8930_display.c fn clcd_update_irq gamma_row vbl_tick display_update
 * PKG glib-2.0
 * VARIANT
 * VARIANT -DTEST_STALL
 * VARIANT -D__APPLE__
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NANOSECONDS_PER_SECOND 1000000000LL
#include "constants.h"
typedef struct { uint32_t regs[0x1030 / 4]; bool swap_pending; uint32_t swap_id; } DisplayPipe;
typedef struct { unsigned w, h; uint32_t pixels[4]; } DisplaySurface;
typedef struct S5L8930DisplayState S5L8930DisplayState;
typedef struct { bool visible; DisplaySurface surface; S5L8930DisplayState *state; } QemuConsole;
struct S5L8930DisplayState {
    DisplayPipe pipe[2]; char vsync_input[64]; unsigned stop_after, quiet_vbls, swaps;
    int ftl; void *vbl; QemuConsole *con;
    uint8_t *front; uint32_t front_key[4]; bool front_valid; uint64_t front_gen;
    unsigned shown; uint64_t shown_gen; DisplaySurface *shown_surface;
    bool dirty, lit, gamma_identity;
    uint32_t clcd[2][0x2000 / 4], clcd_status; int clcd_irq;
    uint8_t gamma8[3][256];
};
static unsigned latches, ends, irqs, hw_updates, publications, rearms, abs_inputs, buttons, syncs, stops;
static bool recorded_new, clcd_level;
static void qemu_set_irq(int irq, bool level) { clcd_level = level; }
static int64_t now = 1234;
static void qemu_input_queue_abs(void *con, int axis, int value, int min, int max) { abs_inputs++; }
static void qemu_input_queue_btn(void *con, int button, bool down) { buttons++; }
static void qemu_input_event_sync(void) { syncs++; }
#define INPUT_AXIS_X 0
#define INPUT_AXIS_Y 1
#define INPUT_BUTTON_LEFT 0
#define RUN_STATE_PAUSED 1
static void qemu_system_vmstop_request_prepare(void) { stops++; }
static void qemu_system_vmstop_request(int state) { assert(state == RUN_STATE_PAUSED); stops++; }
static void frame_timeline_record(int *timeline, unsigned id, bool fresh) { recorded_new = fresh; }
static void pipe_update_irq(DisplayPipe *p) { irqs++; }
static void pipe_frame_end(DisplayPipe *p) { ends++; }
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(void *timer, int64_t deadline) { assert(deadline == now + VBL_PERIOD_NS); rearms++; }
static void front_latch(S5L8930DisplayState *s) {
    latches++;
    if (s->dirty) { s->front_gen++; s->front_valid = true; s->dirty = false; }
}
static bool qemu_console_is_visible(QemuConsole *con) { return con->visible; }
static void display_update(void *opaque);
static void graphic_hw_update(QemuConsole *con) {
    assert(ends % 2 == 0 && !con->state->pipe[0].swap_pending && !con->state->pipe[1].swap_pending);
    hw_updates++; display_update(con->state);
}
static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h) { *w = 2; *h = 2; }
static DisplaySurface *qemu_console_surface(QemuConsole *con) { return &con->surface; }
static unsigned surface_width(DisplaySurface *s) { return s->w; }
static unsigned surface_height(DisplaySurface *s) { return s->h; }
static unsigned surface_bits_per_pixel(DisplaySurface *s) { return 32; }
static unsigned surface_stride(DisplaySurface *s) { return s->w * 4; }
static void *surface_data(DisplaySurface *s) { return s->pixels; }
static void qemu_console_resize(QemuConsole *con, unsigned w, unsigned h) { con->surface.w = w; con->surface.h = h; }
static bool compose(S5L8930DisplayState *s, unsigned w, unsigned h, uint32_t *out, uint32_t *key) {
    if (key) memset(key, 0, 4 * sizeof(*key));
    if (out) memcpy(out, s->front, 16);
    return s->lit;
}
static void dpy_gfx_update(QemuConsole *con, int x, int y, int w, int h) { publications++; }
#include "slice.h"
int main(void) {
    uint32_t front[4] = {7, 8, 9, 10};
    S5L8930DisplayState s = {.front = (uint8_t *)front, .dirty = true, .lit = true, .gamma_identity = true};
    s.clcd[0][CLCD_IRQ_ENABLE / 4] = 1;
    s.clcd[0][CLCD_IRQ_MASK / 4] = CLCD_IRQ_FRAME;
    QemuConsole console = {.visible = true, .surface = {.w = 2, .h = 2}, .state = &s};
    s.con = &console; s.pipe[0].swap_pending = true; s.pipe[1].swap_pending = true;
#ifdef TEST_STALL
    assert(!setenv("IT_JANK_STALL_EVERY", "1", 1));
    vbl_tick(&s);
    assert(s.pipe[0].swap_pending && latches == 0 && ends == 0 && irqs == 2);
    assert(hw_updates == 0 && publications == 0 && rearms == 1 && !recorded_new);
    puts("PASS: injected held-swap path rearms without painting or completing swap");
#else
    assert(!unsetenv("IT_JANK_STALL_EVERY"));
    strcpy(s.vsync_input, "abs 10 20"); s.stop_after = 1;
    vbl_tick(&s);
    assert(abs_inputs == 2 && syncs == 1 && stops == 2 && !s.vsync_input[0]);
    assert(clcd_level && (s.clcd_status & CLCD_IRQ_FRAME));
    assert(recorded_new && latches == 1 && ends == 2 && rearms == 1 && s.swaps == 1);
#ifdef __APPLE__
    assert(hw_updates == 0 && publications == 0);
    display_update(&s);  /* Original Apple GUI refresh still paints. */
#else
    assert(hw_updates == 1 && publications == 1);
#endif
    assert(!memcmp(console.surface.pixels, front, sizeof(front)));
    unsigned before = hw_updates;
    strcpy(s.vsync_input, "btn 0"); vbl_tick(&s);
    assert(buttons == 1 && syncs == 2 && !recorded_new);
#ifdef __APPLE__
    assert(hw_updates == before);
#else
    assert(hw_updates == before + 1);
#endif
    assert(publications == 1 && latches == 1); /* unchanged latch not republished */
    console.visible = false; s.pipe[0].swap_pending = true; s.dirty = true;
    before = hw_updates; vbl_tick(&s);
    assert(latches == 2 && hw_updates == before && rearms == 3 && s.swaps == 2);
    console.visible = true; vbl_tick(&s);
#ifndef __APPLE__
    assert(hw_updates == before + 1 && publications == 2);
#endif
    s.quiet_vbls = QUIET_RELATCH_VBLS - 1; before = publications;
    vbl_tick(&s); assert(latches == 3 && publications == before); /* quiet relatch, unchanged bytes */
    s.con = NULL; before = hw_updates; vbl_tick(&s);
    assert(hw_updates == before && rearms == 6 && ends == 12);
    puts("PASS: actual A4 VBL visible/hidden/null, unchanged-front skip, callbacks and rearm");
#endif
}
