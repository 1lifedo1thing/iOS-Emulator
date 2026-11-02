/* Native host boundary used by FMSS: staged-file publication must replace an
 * existing destination atomically. Run in a fresh empty directory (argv[1]).
 * cc test-ios-file-replace.c $(pkg-config --cflags --libs glib-2.0) -o test
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

static void contents(const char *path, const char *expected)
{
    char *bytes = NULL;
    gsize size = 0;
    assert(g_file_get_contents(path, &bytes, &size, NULL));
    assert(size == strlen(expected) && memcmp(bytes, expected, size) == 0);
    g_free(bytes);
}

static void replacement(const char *directory, const char *name)
{
    char *target = g_build_filename(directory, name, NULL);
    char *stage = g_strconcat(target, ".tmp", NULL);
    char *missing = g_strconcat(target, ".absent", NULL);
    char *blocked = g_strconcat(target, ".directory", NULL);
    char *sentinel = g_build_filename(blocked, "keep", NULL);
    assert(g_file_set_contents(target, "old", 3, NULL));
    for (unsigned i = 0; i < 4; ++i) {
        char *next = g_strdup_printf("complete replacement %u", i);
        assert(g_file_set_contents(stage, next, -1, NULL));
        assert(g_rename(stage, target) == 0);
        assert(!g_file_test(stage, G_FILE_TEST_EXISTS));
        contents(target, next);
        g_free(next);
    }
    /* A failed publication must retain the previously committed destination. */
    assert(g_rename(missing, target) == -1);
    contents(target, "complete replacement 3");
    assert(g_mkdir(blocked, 0700) == 0);
    assert(g_file_set_contents(sentinel, "preserve", 8, NULL));
    assert(g_file_set_contents(stage, "new staged bytes", -1, NULL));
    assert(g_rename(stage, blocked) == -1);
    contents(stage, "new staged bytes");
    contents(sentinel, "preserve");
    contents(target, "complete replacement 3");
    assert(g_remove(stage) == 0);
    assert(g_remove(sentinel) == 0);
    assert(g_rmdir(blocked) == 0);
    assert(g_remove(target) == 0);
    g_free(target); g_free(stage); g_free(missing);
    g_free(blocked); g_free(sentinel);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    replacement(argv[1], "509.page");
    replacement(argv[1], "blk1.erased");
    replacement(argv[1], "\xc3\xa9-NAND.page");
    g_print("PASS: repeated page/erase-marker/Unicode replacements; failed publication preserves old destination and staged bytes\n");
    return 0;
}
