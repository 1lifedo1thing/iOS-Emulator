/* LGPL-2.1-or-later. Never select system usbmuxd or a physical device.
 * Native Windows execution and ownership qualification remain outstanding. */
#define WIN32_LEAN_AND_MEAN
#include "windows-connect.h"
#include "windows-peer.h"
#include <ws2tcpip.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int decimal(const char *text, uint64_t limit, uint64_t *result)
{
    uint64_t value = 0;
    if (!text || !*text || *text == '0') return 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (limit - (*text - '0')) / 10) return 0;
        value = value * 10 + (*text - '0');
    }
    *result = value;
    return 1;
}

int ltw_connect_owned_daemon(void)
{
    const char *address = getenv("USBMUXD_SOCKET_ADDRESS");
    uint64_t port, pid, created;
    SOCKET fd = INVALID_SOCKET;
    struct sockaddr_in target = {0};
    u_long nonblocking = 1;
    WSAPOLLFD wait;
    int error = 0, size = sizeof(error);
    /* Strict all-or-nothing configuration before socket creation. No UNIX,
     * DNS, IPv6, default port, whitespace, signs or partial decimal parses. */
    if (!address || strncmp(address, "127.0.0.1:", 10) ||
        !decimal(address + 10, 65535, &port) ||
        !decimal(getenv("LTW_USBMUX_PID"), MAXDWORD, &pid) ||
        !decimal(getenv("LTW_USBMUX_CREATED"), UINT64_MAX, &created)) goto denied;
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons((u_short)port);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET || fd > INT_MAX ||
        !SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0) ||
        ioctlsocket(fd, FIONBIO, &nonblocking)) goto denied;
    if (connect(fd, (struct sockaddr *)&target, sizeof(target)) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) goto denied;
        wait.fd = fd; wait.events = POLLOUT; wait.revents = 0;
        if (WSAPoll(&wait, 1, 3000) <= 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            !(wait.revents & POLLOUT) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) || error) goto denied;
    }
    if (!ltw_peer_admit(fd, (DWORD)pid, created)) goto denied;
    /* The library's existing socket_send/receive helpers expect blocking
     * descriptors and supply their own deadlines. Owner check precedes bytes. */
    nonblocking = 0;
    if (ioctlsocket(fd, FIONBIO, &nonblocking)) goto denied;
    return (int)fd;
denied:
    if (fd != INVALID_SOCKET) closesocket(fd);
    errno = EACCES;
    return -EACCES;
}
