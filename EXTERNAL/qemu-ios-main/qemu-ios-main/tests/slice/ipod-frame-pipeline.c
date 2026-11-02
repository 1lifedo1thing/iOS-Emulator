/* Diagnostic counters observe actual production scan/capture/copy branches.
 * Synthetic surfaces and dirty-range stubs are not a target guest acceptance.
 * SLICE:counter-types include/hw/arm/ipod_touch_lcd.h typedef IPodLCDFrameCounters
 * SLICE:counter-types contrib/ios-app/qemu-ios-ui.c typedef IosFrameCounters
 * SLICE:portable-api contrib/ios-app/qemu-ios-ui.h file
 * SLICE:lcd hw/arm/ipod_touch_lcd.c fn lcd_frame_count lcd_refresh_rotated lcd_refresh ipod_touch_lcd_reset ipod_touch_lcd_get_frame_scan_counters
 * SLICE:bridge contrib/ios-app/qemu-ios-ui.c fn ios_frame_count ios_capture qemu_ios_portable_frame_copy ios_get_portable_frame_counters ios_add_frame_counter_property
 * PKG glib-2.0
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-comment
 */
#include <assert.h>
#include <glib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "counter-types.h"
#include "portable-api.h"

typedef void Object;
typedef void Error;
typedef void DeviceState;
typedef struct { bool ram; } MemoryRegion;
typedef struct { MemoryRegion *mr; uint64_t offset_within_region; } MemoryRegionSection;
typedef struct { int w, h, stride, bpp; uint8_t *data; } DisplaySurface;
typedef struct { DisplaySurface *surface; } QemuConsole;
static int surface_width(DisplaySurface *s) { return s->w; }
static int surface_height(DisplaySurface *s) { return s->h; }
static int surface_stride(DisplaySurface *s) { return s->stride; }
static int surface_bits_per_pixel(DisplaySurface *s) { return s->bpp; }
static void *surface_data(DisplaySurface *s) { return s->data; }
#define PIXMAN_x8r8g8b8 1
static int surface_format(DisplaySurface *s) { return PIXMAN_x8r8g8b8; }
static DisplaySurface *qemu_console_surface(QemuConsole *c) { return c->surface; }
static void qemu_console_resize(QemuConsole *c, int w, int h) {
    c->surface->w = w; c->surface->h = h; c->surface->stride = w * 4;
}
static unsigned display_updates;
static void dpy_gfx_update(QemuConsole *con, int x, int y, int w, int h) {
    assert(w > 0 && h > 0); display_updates++;
}
#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
#define LCD_VSYNC_PERIOD_NS 16666666
static uint64_t host_ns = 100;
static uint64_t qemu_clock_get_ns(int clock) { return host_ns; }

typedef struct IPodTouchLCDState {
    QemuConsole *con; MemoryRegion *sysmem; MemoryRegionSection fbsection;
    IPodLCDFrameCounters frame_counters;
    uint32_t pw, ph, scanout_base, fbsection_base, fb_base;
    int rotation, invalidate, last_bright; void *last_surface;
    int64_t last_present_ns, next_vsync;
    bool gamma_identity, s5l8900, gather;
    uint64_t gles_gen;
    uint8_t *rotbuf;
    uint32_t plane_regs[192], plane_scanout[192];
    uint32_t lcd_con, render, irq_enable, irq_status;
    uint32_t w1_display_resolution_info, w1_framebuffer_base, w1_hspan, w1_display_depth_info;
    uint32_t ftl, ftl_last_base;
} IPodTouchLCDState;
#define IPOD_TOUCH_LCD(obj) ((IPodTouchLCDState *)(obj))
static bool lcd_in_vsync_present, lcd_line_is_copy;
static int it_display_rotation_req;
static uint8_t lcd_bright_lut[3][256];
static int brightness = 255;
static int lcd_bright_effective(void) { return brightness; }
static int lcd_frametrace(void) { return 0; }
static void lcd_ft(const char *event, uint32_t arg) { }
static void lcd_bright_lut_sync(IPodTouchLCDState *lcd, int bri) { }
static bool compose;
static bool lcd_needs_plane_composition(const uint32_t *r, int w, int h) { return compose; }
static bool lcd_compose_planes(IPodTouchLCDState *lcd, const uint32_t *r, void *dst) {
    memset(dst, 17, lcd->pw * lcd->ph * 4); return true;
}
static void lcd_gather(IPodTouchLCDState *lcd, void *dst) { memset(dst, 23, lcd->pw * lcd->ph * 4); }
static void cpu_physical_memory_read(uint32_t base, void *dst, size_t bytes) { memset(dst, 31, bytes); }
static uint32_t rgb_to_pixel32(uint8_t r, uint8_t g, uint8_t b) { return r << 16 | g << 8 | b; }
static bool memory_region_is_ram(MemoryRegion *mr) { return mr->ram; }
static uint64_t memory_region_get_ram_addr(MemoryRegion *mr) { return 0; }
static uint64_t current_gles_gen;
static uint64_t gles_host_ram_gen(uint64_t start, uint64_t size) { return current_gles_gen; }
static MemoryRegion memory = {.ram = true};
static void framebuffer_update_memory_section(MemoryRegionSection *section, MemoryRegion *sysmem,
                                               uint32_t base, int height, int width) {
    section->mr = height ? &memory : NULL;
}
typedef void (*drawfn)(void);
static void draw_line32_32(void) { }
static int dirty_first = -1, dirty_last = -1;
static unsigned framebuffer_checks;
static void framebuffer_update_display(DisplaySurface *surface, MemoryRegionSection *section,
        int width, int height, int src_width, int line_size, int dest_width, int invalidate,
        drawfn draw, void *opaque, int *first, int *last) {
    framebuffer_checks++;
    *first = invalidate ? 0 : dirty_first;
    *last = invalidate ? height - 1 : dirty_last;
}
static void lcd_changebrightness(int bri) { brightness = bri; }
static void lcd_gamma_reset(IPodTouchLCDState *lcd) { }
static void lcd_update_irq(IPodTouchLCDState *lcd) { }
static void frame_timeline_reset(uint32_t *timeline) { *timeline = 0; }
static int lcd_trace(void) { return 0; }
#define TRACE_PRINTF(...) ((void)0)
#include "lcd.h"

typedef int QemuMutex;
#define IOS_FRAME_BUFFERS 3
static struct {
    QemuMutex frame_lock; void *buf[3]; size_t buf_size; GSList *retired;
    int published, width, height; uint64_t serial, timestamp_ns;
    bool pending; void (*cb)(void *); void *cb_opaque;
} ios = {.published = -1};
static IosFrameCounters ios_frame_counters;
static bool getter_active, bql;
static void qemu_mutex_lock(QemuMutex *lock) { assert(!*lock); assert(!getter_active || bql); *lock = 1; }
static bool overwrite_on_unlock;
static void qemu_mutex_unlock(QemuMutex *lock) {
    assert(*lock); *lock = 0;
    if (overwrite_on_unlock && ios.published >= 0)
        memset(ios.buf[ios.published], 0xee, (size_t)ios.width * ios.height * 4);
}
static void ios_init_frame_lock(void) { }
static void ios_report_first_lit_frame(const uint8_t *p, int w, int h) { }
#define TYPE_IPOD_TOUCH_LCD "ipodtouch.lcd"
static Object *resolved_lcd;
static unsigned property_adds;
static char *(*property_getter)(Object *, Error **);
static Object *object_resolve_path_type(const char *path, const char *type, bool *ambiguous) {
    assert(!strcmp(path, "") && !strcmp(type, TYPE_IPOD_TOUCH_LCD)); return resolved_lcd;
}
static void *object_property_find(Object *obj, const char *name) {
    assert(obj == resolved_lcd && !strcmp(name, "portable-frame-counters"));
    return property_adds ? &property_adds : NULL;
}
static void object_property_add_str(Object *obj, const char *name,
        char *(*getter)(Object *, Error **), void *setter) {
    assert(obj == resolved_lcd && !strcmp(name, "portable-frame-counters") && !setter);
    property_adds++; property_getter = getter;
}
static void object_property_set_description(Object *obj, const char *name, const char *description) {
    assert(obj == resolved_lcd && !strcmp(name, "portable-frame-counters"));
    assert(strstr(description, "metadata-only is not a pixel copy"));
}
#include "bridge.h"

static void assert_json_uint(const char *json, const char *key, uint64_t expected) {
    char *field = g_strdup_printf("\"%s\":%" PRIu64, key, expected);
    const char *found = strstr(json, field);
    assert(found && (found[strlen(field)] == ',' || found[strlen(field)] == '}'));
    g_free(field);
}
static void test_scan_branches(void) {
    uint8_t pixels[64] = {0}; DisplaySurface surface = {2, 2, 8, 32, pixels};
    QemuConsole console = {&surface};
    IPodTouchLCDState lcd = {.con = &console, .sysmem = &memory, .pw = 2, .ph = 2,
        .scanout_base = 4096, .last_bright = -1, .gamma_identity = true};
    surface.bpp = 0; lcd_refresh(&lcd);
    assert(!lcd.frame_counters.scan_attempts); surface.bpp = 32;
    /* A fresh surface forces a repaint, even without a dirty guest range. */
    lcd_refresh(&lcd);
    assert(lcd.frame_counters.scan_attempts == 1 && lcd.frame_counters.poll_scans == 1);
    assert(lcd.frame_counters.framebuffer_checks == 1 && lcd.frame_counters.forced_checks == 1);
    assert(lcd.frame_counters.updated_regions == 1 && display_updates == 1 && !lcd.invalidate);
    /* No change is a completed dirty check, not a missing acquisition. */
    lcd_refresh(&lcd);
    assert(lcd.frame_counters.unchanged_checks == 1 && display_updates == 1);
    dirty_first = dirty_last = 1; lcd_in_vsync_present = true; lcd_refresh(&lcd);
    assert(lcd.frame_counters.vsync_scans == 1 && lcd.frame_counters.updated_regions == 2);
    assert(lcd.last_present_ns == (int64_t)host_ns);
    lcd_in_vsync_present = false;
    unsigned prior_checks = framebuffer_checks; lcd_refresh(&lcd);
    assert(lcd.frame_counters.duplicate_poll_skips == 1 && framebuffer_checks == prior_checks);
    /* Brightness and GL generation still use their existing invalidation. */
    brightness = 254; lcd_refresh(&lcd);
    assert(lcd.frame_counters.forced_checks == 2 && lcd.last_bright == 254);
    host_ns += 3 * LCD_VSYNC_PERIOD_NS; current_gles_gen = 7; dirty_first = dirty_last = -1;
    lcd_refresh(&lcd);
    assert(lcd.gles_gen == 7 && lcd.frame_counters.forced_checks == 3);
    unsigned before = display_updates;
    it_display_rotation_req = 90; lcd_refresh(&lcd);
    assert(lcd.rotation == 90 && lcd.frame_counters.rotated_updates == 1 && display_updates == before + 1);
    /* A not-yet-resized rotated surface does not claim an update. */
    surface.w = 1; lcd_refresh_rotated(&lcd, &surface, 90, false);
    assert(lcd.frame_counters.rotated_updates == 1 && display_updates == before + 1);
    surface.w = surface.h = 2;
    IPodTouchLCDState snapshot = lcd;
    char *json = ipod_touch_lcd_get_frame_scan_counters((Object *)&lcd, NULL);
    assert(!memcmp(&snapshot, &lcd, sizeof lcd));
    assert_json_uint(json, "scanAttempts", lcd.frame_counters.scan_attempts);
    assert_json_uint(json, "glesGeneration", 7);
    assert(strstr(json, "\"updatedRegionsProveGuestWrites\":false")); g_free(json);
    IPodLCDFrameCounters retained = lcd.frame_counters;
    ipod_touch_lcd_reset((DeviceState *)&lcd);
    assert(lcd.frame_counters.resets == retained.resets + 1);
    retained.resets++;
    assert(!memcmp(&retained, &lcd.frame_counters, sizeof retained));
    assert(lcd.scanout_base == 0 && lcd.invalidate && it_display_rotation_req == 0);
    g_free(lcd.rotbuf);
}
static void test_capture_copy_branches(void) {
    uint8_t pixels[24]; memset(pixels, 7, sizeof pixels);
    DisplaySurface surface = {2, 2, 12, 32, pixels};
    QemuIosPortableFrame frame = {.size = sizeof frame, .version = 1};
    uint8_t owned[16] = {0};
    assert(qemu_ios_portable_frame_copy(owned, sizeof owned, 0, &frame) == QEMU_IOS_NO_FRAME);
    assert(ios_frame_counters.valid_copy_calls == 1 && ios_frame_counters.no_frame == 1);
    ios_capture(&surface); ios_capture(&surface);
    assert(ios.serial == 1 && ios_frame_counters.capture_attempts == 2);
    assert(ios_frame_counters.publications == 1 && ios_frame_counters.identical_suppressed == 1);
    pixels[11]++; ios_capture(&surface); /* row padding is not a new frame */
    assert(ios_frame_counters.identical_suppressed == 2 && ios.serial == 1);
    pixels[12]++; ios_capture(&surface);
    assert(ios_frame_counters.publications == 2 && ios.serial == 2);
    assert(qemu_ios_portable_frame_copy(NULL, 0, 0, &frame) == QEMU_IOS_BUFFER_SMALL);
    assert(ios_frame_counters.metadata_only == 1 && !ios_frame_counters.pixel_copies);
    assert(qemu_ios_portable_frame_copy(owned, sizeof owned - 1, 0, &frame) == QEMU_IOS_BUFFER_SMALL);
    assert(ios_frame_counters.buffer_small == 1 && !ios_frame_counters.pixel_copies);
    overwrite_on_unlock = true;
    assert(qemu_ios_portable_frame_copy(owned, sizeof owned, 0, &frame) == QEMU_IOS_OK);
    overwrite_on_unlock = false;
    assert(owned[0] == 7 && owned[8] == 8 && ((uint8_t *)ios.buf[ios.published])[0] == 0xee);
    assert(ios_frame_counters.pixel_copies == 1 && ios_frame_counters.last_copied_serial == 2);
    assert(qemu_ios_portable_frame_copy(owned, sizeof owned, 2, &frame) == QEMU_IOS_NO_FRAME);
    IosFrameCounters before = ios_frame_counters;
    ios_add_frame_counter_property(); assert(!property_adds); /* non-CLCD board */
    resolved_lcd = (Object *)&surface;
    ios_add_frame_counter_property(); ios_add_frame_counter_property();
    assert(property_adds == 1 && property_getter == ios_get_portable_frame_counters);
    frame.version = 99;
    assert(qemu_ios_portable_frame_copy(owned, sizeof owned, 0, &frame) == QEMU_IOS_INVALID);
    assert(!memcmp(&before, &ios_frame_counters, sizeof before));
    assert(before.valid_copy_calls == before.no_frame + before.metadata_only + before.buffer_small + before.pixel_copies);
    /* Queries neither copy nor publish frames, and release the existing lock. */
    uint64_t serial = ios.serial; int published = ios.published; ios.pending = true;
    bql = getter_active = true;
    char *json = ios_get_portable_frame_counters(NULL, NULL);
    getter_active = bql = false;
    assert(!memcmp(&before, &ios_frame_counters, sizeof before));
    assert(!ios.frame_lock && ios.serial == serial && ios.published == published && ios.pending);
    assert_json_uint(json, "pixelCopies", 1); assert_json_uint(json, "lastCopiedSerial", 2);
    assert(strstr(json, "\"pending\":true") && strstr(json, "\"atomicWithLcd\":false"));
    g_free(json);
    for (int i = 0; i < IOS_FRAME_BUFFERS; i++) g_free(ios.buf[i]);
    g_slist_free_full(ios.retired, g_free);
}
int main(void) {
    uint64_t bounded = UINT64_MAX - 1;
    lcd_frame_count(&bounded); lcd_frame_count(&bounded); assert(bounded == UINT64_MAX);
    bounded = UINT64_MAX - 1;
    ios_frame_count(&bounded); ios_frame_count(&bounded); assert(bounded == UINT64_MAX);
    test_scan_branches(); test_capture_copy_branches();
    puts("PASS: production scan/capture/copy branches, exact dedupe/copy behavior, read-only snapshots, reset lifetime and saturation");
}
