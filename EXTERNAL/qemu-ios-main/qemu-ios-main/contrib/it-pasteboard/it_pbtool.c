/*
 * it_pbtool -- the guest's general pasteboard from a command line, for host
 * checks that drive the clipboard through the guest agent's spawn op
 * (LightTouchMac's `sessions clipboard`):
 *
 *   it_pbtool get          the pasteboard's text on stdout, exit 0 (1: no text)
 *   it_pbtool set TEXT...  the arguments, joined by spaces, as the pasteboard's text
 *
 * it_agent's own ObjC plumbing and explicit-UTI calls (THE TRAP there), so a
 * check reads and writes exactly as the agent does. iPhone OS 3.0 and later.
 */
#define IT_AGENT_CLIPBOARD_ONLY
#include "../it-agent/it_agent.c"

extern void _exit(int);
/* No crt1 (armv6.sh's link enters at the entry point outright): argc and argv
 * from the stack, as httpget does; main never returns. */
__attribute__((naked)) void _start(void) { __asm__ volatile("ldr r0,[sp]\n\tadd r1,sp,#4\n\tb _main"); }

int main(int argc, char **argv)
{
    static char text[PB_BUF];

    if (argc < 2 || !objc_setup()) {
        w("usage: it_pbtool get | set TEXT... (iPhone OS 3.0 and later)\n");
        _exit(2);
    }
    id_ pool = m0(m0(cls_pool, sel_alloc), sel_init);
    id_ pb = m0(cls_pasteboard, sel_general);
    int rc = 1;
    if (argv[1][0] == 'g') {
        id_ d = pb ? m1(pb, sel_dataForType, s_uti) : 0;
        if (d) {
            write(1, (const char *)m0(d, sel_bytes), (unsigned)mlong(d, sel_length));
            rc = 0;
        }
    } else if (argv[1][0] == 's' && argc > 2) {
        unsigned n = 0, j;
        int i;
        for (i = 2; i < argc; i++) {
            unsigned k = slen(argv[i]);
            if (n + k + 1 >= sizeof text) {
                break;
            }
            if (i > 2) {
                text[n++] = ' ';
            }
            for (j = 0; j < k; j++) {
                text[n++] = argv[i][j];
            }
        }
        text[n] = 0;
        id_ s = m1s(cls_string, sel_stringWithUTF8, text);
        if (pb && s) {
            m2(pb, sel_setValue, s, s_uti);
            rc = 0;
        }
    }
    m0(pool, sel_release);
    _exit(rc);
    return rc;
}
