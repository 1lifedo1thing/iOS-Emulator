/* Standalone regression: cc -Iinclude tests/unit/test-ios-file-identity.c -o test
 * (or x86_64-w64-mingw32-gcc for the native Windows executable).
 * Run with an existing empty writable directory as its only argument.
 * All files are synthetic; no guest/firmware/runtime dependencies required.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "qemu/file-identity.h"
#ifndef _WIN32
#include <unistd.h>
#endif

static void check(const char *a, const char *b, bool expected)
{
    bool same = !expected;
    unsigned long error = qemu_file_paths_equal(a, b, &same);
    if (error || same != expected) {
        fprintf(stderr, "identity error=%lu equal=%d expected=%d: %s / %s\n",
                error, same, expected, a, b);
        abort();
    }
}

int main(int argc, char **argv)
{
    char base[4096], copy[4096], hard[4096], alias[4096], missing[4096];
    assert(argc == 2);
    assert(snprintf(base, sizeof(base), "%s/base.bin", argv[1]) < (int)sizeof(base));
    assert(snprintf(copy, sizeof(copy), "%s/copy.bin", argv[1]) < (int)sizeof(copy));
    assert(snprintf(hard, sizeof(hard), "%s/hard.bin", argv[1]) < (int)sizeof(hard));
    assert(snprintf(alias, sizeof(alias), "%s/./base.bin", argv[1]) < (int)sizeof(alias));
    assert(snprintf(missing, sizeof(missing), "%s/absent.bin", argv[1]) < (int)sizeof(missing));
    FILE *f = fopen(base, "wb");
    assert(f);
    assert(fwrite("synthetic immutable NOR", 1, 23, f) == 23);
    assert(fclose(f) == 0);
    f = fopen(copy, "wb");
    assert(f);
    assert(fwrite("synthetic immutable NOR", 1, 23, f) == 23);
    assert(fclose(f) == 0);
    check(base, base, true);
    check(base, alias, true);
    check(base, copy, false); /* Identical contents are still a private copy. */
    f = fopen(copy, "wb");
    assert(f);
    assert(fwrite("private change", 1, 14, f) == 14);
    assert(fclose(f) == 0);
    f = fopen(base, "rb");
    assert(f);
    char preserved[23];
    assert(fread(preserved, 1, sizeof(preserved), f) == sizeof(preserved));
    assert(fclose(f) == 0);
    assert(memcmp(preserved, "synthetic immutable NOR", 23) == 0);
#ifdef _WIN32
    assert(CreateHardLinkA(hard, base, NULL));
    struct stat a, b;
    assert(stat(base, &a) == 0 && stat(copy, &b) == 0);
    printf("CRT stat: base inode=%llu copy inode=%llu; identities remain distinct\n",
           (unsigned long long)a.st_ino, (unsigned long long)b.st_ino);
    char case_alias[4096];
    assert(snprintf(case_alias, sizeof(case_alias), "%s/BASE.BIN", argv[1]) < (int)sizeof(case_alias));
    if (GetFileAttributesA(case_alias) != INVALID_FILE_ATTRIBUTES) {
        check(base, case_alias, true);
    } else {
        puts("SKIP case alias: test directory is case-sensitive");
    }
    /* UTF-8 paths must reach the same actual Unicode Windows filename. */
    char unicode[4096];
    assert(snprintf(unicode, sizeof(unicode), "%s/\xc3\xa9-NOR.bin", argv[1]) < (int)sizeof(unicode));
    wchar_t wide[4096];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              unicode, -1, wide, 4096));
    HANDLE u = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    assert(u != INVALID_HANDLE_VALUE);
    CloseHandle(u);
    check(unicode, unicode, true);
    check(base, unicode, false);
    bool invalid_unchanged = false;
    assert(qemu_file_paths_equal(base, "\xff", &invalid_unchanged) != 0 && !invalid_unchanged);
    assert(DeleteFileW(wide));
#else
    assert(link(base, hard) == 0);
    char symbolic[4096];
    assert(snprintf(symbolic, sizeof(symbolic), "%s/symbolic.bin", argv[1]) < (int)sizeof(symbolic));
    assert(symlink(base, symbolic) == 0);
    check(base, symbolic, true);
    assert(unlink(symbolic) == 0);
#endif
    check(base, hard, true);
    bool unchanged = false;
    assert(qemu_file_paths_equal(base, missing, &unchanged) != 0 && !unchanged);
    assert(remove(hard) == 0);
    assert(remove(copy) == 0);
    assert(remove(base) == 0);
    puts("PASS: private copies accepted; same-path/alias/hardlink rejected; missing identity fails closed");
    return 0;
}
