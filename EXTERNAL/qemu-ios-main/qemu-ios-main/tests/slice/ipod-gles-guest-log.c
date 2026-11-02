/* The guest shims' log text arrives in fragments: it is assembled into lines, a repeated line prints three
 * times and then only with its count at each power of ten, and IT_GLES_VERBOSE prints everything.
 *
 * SLICE hw/arm/gles-host.c fn gles_guest_log
 * PKG glib-2.0
 */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
static GString *out;
#define fputs(s, f) g_string_append(out, (s))
#define fprintf(f, ...) g_string_append_printf(out, __VA_ARGS__)
#include "slice.h"

int main(void)
{
    out = g_string_new(NULL);
    for (int i = 0; i < 100; i++) {
        gles_guest_log("[mbxshim] same ");
        gles_guest_log("line\n[mbxshim] other\n");
    }
    unsigned same = 0, counted = 0;
    for (const char *p = out->str; (p = strstr(p, "same line\n")); p++) same++;
    for (const char *p = out->str; (p = strstr(p, "times)\n")); p++) counted++;
    assert(same == 5 && counted == 4);     /* 1, 2, 3, then 10 and 100, for each of the two lines */
    assert(strstr(out->str, "(the guest line below, 100 times)\n[mbxshim] same line\n"));
    puts("PASS: a repeated guest GL log line prints 3 times, then at powers of ten");
    return 0;
}
