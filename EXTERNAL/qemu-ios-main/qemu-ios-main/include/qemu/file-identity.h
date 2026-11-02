/* Host file identity for protecting immutable iOS prepared images. */
#ifndef QEMU_FILE_IDENTITY_H
#define QEMU_FILE_IDENTITY_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>

/* CRT stat() reports st_ino == 0 on Windows, even for unrelated files. Use
 * the filesystem's identity instead. Wide paths preserve non-ASCII names;
 * opening normally follows links, so alternate names cannot bypass protection.
 * FileIdInfo includes the full 128-bit identity (also needed by ReFS).
 */
static inline unsigned long qemu_win32_file_identity(const char *path,
                                                     FILE_ID_INFO *identity)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   path, -1, NULL, 0);
    if (!count) {
        return GetLastError();
    }
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            path, -1, wide, count)) {
        DWORD error = GetLastError();
        free(wide);
        return error;
    }
    HANDLE file = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    DWORD error = GetFileInformationByHandleEx(file, FileIdInfo, identity,
                                              sizeof(*identity)) ?
                  ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}
#endif

/* Returns zero on success; an errno (POSIX) or GetLastError code (Windows)
 * otherwise. An unqueryable identity must never be treated as a private copy.
 */
static inline unsigned long qemu_file_paths_equal(const char *left,
                                                const char *right,
                                                bool *equal)
{
#ifdef _WIN32
    FILE_ID_INFO a, b;
    unsigned long error = qemu_win32_file_identity(left, &a);
    if (!error) {
        error = qemu_win32_file_identity(right, &b);
    }
    if (error) {
        return error;
    }
    *equal = a.VolumeSerialNumber == b.VolumeSerialNumber &&
             memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
#else
    struct stat a, b;
    if (stat(left, &a) < 0 || stat(right, &b) < 0) {
        return errno;
    }
    *equal = a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
    return 0;
}

#endif
