/* SPDX-License-Identifier: GPL-2.0-or-later
 * Instrumentation-only bounded read-only QMP snapshot. No arbitrary commands.
 * Usage: engine-qmp-snapshot /owned/private/work/qmp-PID.sock
 * Define LIGHTTOUCH_QMP_SNAPSHOT_NO_MAIN to call the function from debug JNI.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT_MAX_LINE 65536
#define SNAPSHOT_MAX_EVENTS 32
#define SNAPSHOT_TIMEOUT_MS 3000

static int64_t snapshot_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int snapshot_wait(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t left = deadline - snapshot_now();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { fd, events, 0 };
        int result = poll(&p, 1, (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (!result) { errno = ETIMEDOUT; return -1; }
        if (result < 0) return -1;
        if (p.revents & events) return 0;
        errno = ECONNRESET;
        return -1;
    }
}

static int snapshot_send(int fd, const char *text, int64_t deadline)
{
    size_t left = strlen(text);
    while (left) {
        if (snapshot_wait(fd, POLLOUT, deadline)) return -1;
        ssize_t n = send(fd, text, left, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return -1;
        text += n; left -= n;
    }
    return 0;
}

static int snapshot_line(int fd, char *line, int64_t deadline)
{
    size_t used = 0;
    /* One-byte reads intentionally avoid stealing a later response from the
     * socket. Only seven bounded responses are requested by this diagnostic. */
    while (used < SNAPSHOT_MAX_LINE - 1) {
        if (snapshot_wait(fd, POLLIN, deadline)) return -1;
        ssize_t n = recv(fd, line + used, 1, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { errno = ECONNRESET; return -1; }
        if (line[used++] == '\n') { line[used] = 0; return 0; }
    }
    errno = EOVERFLOW;
    return -1;
}

/* Recognize only the fixed top-level response ID, skipping escaped strings
 * and nested result objects. The trusted engine remains the JSON producer. */
static int snapshot_has_id(const char *text, const char *wanted)
{
    unsigned depth = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { if (!depth) return 0; depth--; }
        else if (*p == '"') {
            const char *start = ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (!*p) return 0;
            if (depth != 1 || p - start != 2 || memcmp(start, "id", 2)) continue;
            const char *value = p + 1;
            while (isspace((unsigned char)*value)) value++;
            if (*value++ != ':') continue;
            while (isspace((unsigned char)*value)) value++;
            if (*value++ != '"') return 0;
            size_t size = strlen(wanted);
            return !strncmp(value, wanted, size) && value[size] == '"';
        }
    }
    return 0;
}

int lighttouch_debug_qmp_snapshot(const char *logical_path, int output_fd)
{
    int parent = -1, fd = -1, status = -1;
    char *path = logical_path ? strdup(logical_path) : NULL;
    char *line = malloc(SNAPSHOT_MAX_LINE);
    if (!path || !line) { errno = ENOMEM; goto done; }
    char *slash = strrchr(path, '/');
    if (!slash || !slash[1]) { errno = EINVAL; goto done; }
    const char *name = slash + 1;
    if (slash == path) parent = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    else { *slash = 0; parent = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); }
    if (parent < 0) goto done;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int size = snprintf(address.sun_path, sizeof(address.sun_path),
                        "/proc/self/fd/%d/%s", parent, name);
    if (size < 0 || (size_t)size >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; goto done; }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) goto done;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS || snapshot_wait(fd, POLLOUT, snapshot_now() + SNAPSHOT_TIMEOUT_MS)) goto done;
        int error = 0; socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length)) goto done;
        if (error) { errno = error; goto done; }
    }
    struct ucred peer; socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size)) goto done;
    if (peer.uid != geteuid()) { errno = EACCES; goto done; }
    if (snapshot_line(fd, line, snapshot_now() + SNAPSHOT_TIMEOUT_MS)) goto done;
    if (dprintf(output_fd, "%s", line) < 0) goto done;
    static const struct { const char *id, *request; } requests[] = {
        { "lt-cap", "{\"execute\":\"qmp_capabilities\",\"id\":\"lt-cap\"}\n" },
        { "lt-status", "{\"execute\":\"query-status\",\"id\":\"lt-status\"}\n" },
        { "lt-cpus", "{\"execute\":\"query-cpus-fast\",\"id\":\"lt-cpus\"}\n" },
        { "lt-registers", "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"info registers\"},\"id\":\"lt-registers\"}\n" },
        { "lt-cpu-state", "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"info cpus\"},\"id\":\"lt-cpu-state\"}\n" },
        { "lt-instructions", "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"x /16i 0xc0567e20\"},\"id\":\"lt-instructions\"}\n" },
        { "lt-caller", "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"x /12i 0xc0567ca0\"},\"id\":\"lt-caller\"}\n" },
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        int64_t deadline = snapshot_now() + SNAPSHOT_TIMEOUT_MS;
        if (snapshot_send(fd, requests[i].request, deadline)) goto done;
        unsigned skipped = 0;
        do {
            if (snapshot_line(fd, line, deadline)) goto done;
            if (++skipped > SNAPSHOT_MAX_EVENTS) { errno = EOVERFLOW; goto done; }
        } while (!snapshot_has_id(line, requests[i].id));
        if (dprintf(output_fd, "%s", line) < 0) goto done;
    }
    status = 0;
done:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (parent >= 0) close(parent);
        free(path); free(line); errno = saved;
    }
    return status;
}

#ifndef LIGHTTOUCH_QMP_SNAPSHOT_NO_MAIN
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: engine-qmp-snapshot PRIVATE_SOCKET\n"); return 2; }
    if (lighttouch_debug_qmp_snapshot(argv[1], STDOUT_FILENO)) {
        perror("bounded QMP snapshot"); return 1;
    }
    return 0;
}
#endif
