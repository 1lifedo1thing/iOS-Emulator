/* Windows CRT has no memmem. These callers search binary firmware, so a
 * NUL-terminated string search would silently change their behavior. */
#ifdef _WIN32
static const void *ltw_memmem(const void *haystack, size_t haystack_size,
                            const void *needle, size_t needle_size)
{
    const unsigned char *p = haystack;
    const unsigned char *n = needle;
    if (!needle_size) return haystack;
    while (haystack_size >= needle_size) {
        const unsigned char *match = memchr(p, n[0], haystack_size - needle_size + 1);
        if (!match) return NULL;
        if (!memcmp(match, needle, needle_size)) return match;
        size_t consumed = (size_t)(match - p) + 1;
        p += consumed;
        haystack_size -= consumed;
    }
    return NULL;
}
#define memmem ltw_memmem
#endif
