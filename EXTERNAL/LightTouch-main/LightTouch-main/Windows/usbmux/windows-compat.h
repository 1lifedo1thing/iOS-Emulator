/* Native Winsock adapter for the pinned usbmuxd QEMU transport. */
#ifndef LTW_SOCKET_COMPAT_H
#define LTW_SOCKET_COMPAT_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
static inline int ltw_socket_error(int result) {
    if (result == SOCKET_ERROR) {
        int e = WSAGetLastError();
        errno = e == WSAEWOULDBLOCK || e == WSAETIMEDOUT ? EAGAIN : e == WSAEINTR ? EINTR : EIO;
    }
    return result;
}
static inline int ltw_socket_handle(SOCKET s) {
    if (s == INVALID_SOCKET) return ltw_socket_error(-1);
    if (s > INT_MAX) { closesocket(s); errno = EMFILE; return -1; }
    if (!SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0)) {
        closesocket(s); errno = EIO; return -1;
    }
    return (int)s;
}
static inline int ltw_socket(int family, int type, int protocol) { return ltw_socket_handle(socket(family,type,protocol)); }
static inline int ltw_accept(int fd, struct sockaddr *a, int *n) { return ltw_socket_handle(accept((SOCKET)fd,a,n)); }
static inline int ltw_recv(int fd, void *p, int n, int flags) { return ltw_socket_error(recv((SOCKET)fd,p,n,flags)); }
static inline int ltw_send(int fd, const void *p, int n, int flags) { return ltw_socket_error(send((SOCKET)fd,p,n,flags)); }
static inline int ltw_setsockopt(int fd, int level, int option, const void *value, int size) { return ltw_socket_error(setsockopt((SOCKET)fd,level,option,(const char*)value,size)); }
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
static inline int ltw_fcntl(int fd, int command, int flags) {
    if (command == F_GETFL) return 0;
    if (command != F_SETFL) { errno = EINVAL; return -1; }
    u_long value = (flags & O_NONBLOCK) != 0;
    return ltw_socket_error(ioctlsocket((SOCKET)fd,FIONBIO,&value));
}
#define socket ltw_socket
#define accept ltw_accept
#define recv ltw_recv
#define send ltw_send
#define setsockopt ltw_setsockopt
#define fcntl ltw_fcntl
static inline int ltw_poll(WSAPOLLFD *fds, ULONG count, int timeout) {
    return ltw_socket_error(WSAPoll(fds, count, timeout));
}
#define poll ltw_poll
/* BSD wire header required by device.c, independent of the host TCP stack. */
struct tcphdr {
    uint16_t th_sport, th_dport;
    uint32_t th_seq, th_ack;
    uint8_t th_x2:4, th_off:4;
    uint8_t th_flags;
    uint16_t th_win, th_sum, th_urp;
} __attribute__((packed));
_Static_assert(sizeof(struct tcphdr) == 20, "TCP wire header layout");
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_ACK 0x10
#endif
