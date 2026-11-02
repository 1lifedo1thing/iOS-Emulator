/* LGPL-2.1-or-later. Windows-only transport boundary. See SOURCES.json.
 * A successful admission is NOT a Windows target qualification receipt. */
#ifndef LIGHTTOUCH_WINDOWS_PEER_H
#define LIGHTTOUCH_WINDOWS_PEER_H
#include <winsock2.h>
#include <windows.h>

/* Both socket endpoints must be IPv4 loopback, with exactly one established
 * reverse TCP owner row. Hold the peer process while checking its creation
 * identity, current-user SID and a second owner-table snapshot. Nonzero
 * expected_pid requires an exact daemon creation time as well. No payload is
 * read or sent by this operation. Failure is closed, including table churn. */
int ltw_peer_admit(SOCKET socket, DWORD expected_pid, ULONGLONG expected_created);
int ltw_windows_stopping(void);
#endif
