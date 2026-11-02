/* Owner-only loopback admission, not a new usbmux/TUSB protocol.
 * LGPL-2.1-or-later; see SOURCES.json. Actual Windows tests remain required. */
#define WIN32_LEAN_AND_MEAN
#include "windows-peer.h"
#include <iphlpapi.h>
#include <stdlib.h>

#define TABLE_LIMIT (8u * 1024u * 1024u)
#define TOKEN_LIMIT (64u * 1024u)

static DWORD tuple_owner(const struct sockaddr_in *local,
                         const struct sockaddr_in *remote)
{
    DWORD bytes = 0, result, owner = 0, matches = 0;
    MIB_TCPTABLE_OWNER_PID *table = NULL;
    result = GetExtendedTcpTable(NULL, &bytes, FALSE, AF_INET,
                                 TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER) return 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (bytes < sizeof(DWORD) || bytes > TABLE_LIMIT) return 0;
        DWORD capacity = bytes;
        table = malloc(capacity);
        if (!table) return 0;
        result = GetExtendedTcpTable(table, &bytes, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_ALL, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) { free(table); table = NULL; continue; }
        if (result != NO_ERROR || bytes > capacity ||
            table->dwNumEntries > (capacity - sizeof(DWORD)) / sizeof(MIB_TCPROW_OWNER_PID)) {
            free(table); return 0;
        }
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const MIB_TCPROW_OWNER_PID *row = &table->table[i];
            if (row->dwState == MIB_TCP_STATE_ESTAB &&
                row->dwLocalAddr == remote->sin_addr.s_addr &&
                /* MIB_TCPROW_OWNER_PID stores network-order ports in the
                 * low 16 bits; its upper 16 bits are not part of the port. */
                (USHORT)row->dwLocalPort == remote->sin_port &&
                row->dwRemoteAddr == local->sin_addr.s_addr &&
                (USHORT)row->dwRemotePort == local->sin_port) {
                matches++; owner = row->dwOwningPid;
            }
        }
        free(table);
        return matches == 1 ? owner : 0;
    }
    return 0;
}

static TOKEN_USER *user_token(HANDLE process)
{
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    DWORD bytes = 0;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        bytes < sizeof(TOKEN_USER) || bytes > TOKEN_LIMIT) goto out;
    user = malloc(bytes);
    if (!user) goto out;
    if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes) ||
        !IsValidSid(user->User.Sid)) { free(user); user = NULL; }
out:
    CloseHandle(token);
    return user;
}

int ltw_peer_admit(SOCKET socket, DWORD expected_pid, ULONGLONG expected_created)
{
    struct sockaddr_in local = {0}, remote = {0};
    int size = sizeof(local), accepted = 0;
    DWORD pid;
    HANDLE peer = NULL;
    TOKEN_USER *ours = NULL, *theirs = NULL;
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER time;
    if ((expected_pid == 0) != (expected_created == 0) ||
        getsockname(socket, (struct sockaddr *)&local, &size) != 0 || size != (int)sizeof(local)) return 0;
    size = sizeof(remote);
    if (getpeername(socket, (struct sockaddr *)&remote, &size) != 0 || size != (int)sizeof(remote) ||
        local.sin_family != AF_INET || remote.sin_family != AF_INET ||
        local.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        remote.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
    pid = tuple_owner(&local, &remote);
    if (!pid || (expected_pid && pid != expected_pid)) return 0;
    peer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!peer || WaitForSingleObject(peer, 0) != WAIT_TIMEOUT ||
        !GetProcessTimes(peer, &created, &exited, &kernel, &user)) goto out;
    time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
    if (expected_created && time.QuadPart != expected_created) goto out;
    ours = user_token(GetCurrentProcess()); theirs = user_token(peer);
    if (!ours || !theirs || !EqualSid(ours->User.Sid, theirs->User.Sid) ||
        tuple_owner(&local, &remote) != pid || WaitForSingleObject(peer, 0) != WAIT_TIMEOUT) goto out;
    accepted = 1;
out:
    free(ours); free(theirs);
    if (peer) CloseHandle(peer);
    return accepted;
}
