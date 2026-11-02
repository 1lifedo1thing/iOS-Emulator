/* QEMU-only foreground host, adapted from BerryPVZ/LightTouchWin 7aecedc.
 * GPL-2.0-only; see SOURCES.json. No physical USB/system daemon. */
#include "windows-compat.h"
#include "windows-peer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb.h"
#include "device.h"
#include "client.h"
#include "conf.h"
#include "log.h"

int no_preflight = 1;
unsigned int log_level = LL_NOTICE;
static int stopping;
static int control_invalid;
static HANDLE control_input = INVALID_HANDLE_VALUE;
static char command[5];
static DWORD command_used;
/* Only the daemon event-loop thread calls this. Peek first, then read only
 * available bytes from the inherited private pipe. No blocking control thread
 * can outlive shutdown or race CancelSynchronousIo. Transfer retries call this
 * too, so a silent/NAKing guest cannot delay a parent Stop for 30 seconds. */
int ltw_windows_stopping(void)
{
    DWORD available = 0, count = 0;
    if (stopping) return 1;
    if (!PeekNamedPipe(control_input, NULL, 0, NULL, &available, NULL)) {
        control_invalid = command_used != 0 || GetLastError() != ERROR_BROKEN_PIPE;
        return stopping = 1;
    }
    for (DWORD i = 0; i < available && i < 6; i++) {
        char value;
        if (!ReadFile(control_input, &value, 1, &count, NULL) || count != 1) {
            control_invalid = 1; return stopping = 1;
        }
        if (value == '\n') {
            control_invalid = command_used != 4 || memcmp(command, "stop", 4) != 0;
            return stopping = 1;
        }
        if (command_used == sizeof(command)) { control_invalid = 1; return stopping = 1; }
        command[command_used++] = value;
        if (memcmp(command, "stop", command_used > 4 ? 4 : command_used) || command_used > 4) {
            control_invalid = 1; return stopping = 1;
        }
    }
    return 0;
}
void log_enable_syslog(void) {}
void log_disable_syslog(void) {}
void usbmuxd_log(enum loglevel level, const char *fmt, ...)
{
    if ((unsigned)level > log_level) return;
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); fflush(stderr);
}
extern int usb_windows_listen_port(void);

static int hex_identity(const char *text)
{
    if (!text || strlen(text) != 32) return 0;
    for (int i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return 1;
}

static int generation_value(const char *text)
{
    unsigned long long value = 0;
    if (!text || !*text || *text == '0') return 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (9007199254740991ULL - (*text - '0')) / 10) return 0;
        value = value * 10 + (*text - '0');
    }
    return 1;
}

int main(int argc, char **argv)
{
    int listener = -1, initialized = 0, usb_started = 0, result = 2;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    WSADATA wsa;
    struct fdlist list;
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER creation;
    const char *profile = getenv("LTW_SESSION_PROFILE"), *token = getenv("LTW_SESSION_TOKEN");
    const char *generation = getenv("LTW_SESSION_GENERATION");
    if (argc != 2 || input == INVALID_HANDLE_VALUE ||
        GetFileType(input) != FILE_TYPE_PIPE || !hex_identity(profile) ||
        !hex_identity(token) || !generation_value(generation)) return 2;
    control_input = input;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    if (config_set_config_dir(argv[1]) != 0) goto cleanup;
    listener = ltw_socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &yes, sizeof(yes)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 16) ||
        fcntl(listener, F_SETFL, O_NONBLOCK)) goto cleanup;
    int size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &size) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) goto cleanup;
    creation.LowPart = created.dwLowDateTime; creation.HighPart = created.dwHighDateTime;
    if (_putenv_s("USBMUXD_QEMU_ADDR", "127.0.0.1:0") || _putenv_s("USBMUXD_QEMU_DELAY", "0")) goto cleanup;
    client_init(); device_init(); initialized = 1;
    /* usb_shutdown also closes a partially initialized QEMU listener. */
    usb_started = 1;
    if (usb_init() < 0 || usb_windows_listen_port() <= 0) goto cleanup;
    usb_autodiscover(1);
    printf("DAEMON {\"format\":\"lighttouch-windows-usbmux\",\"version\":1,\"pid\":%lu,\"created\":\"%llu\",\"client_port\":%u,\"guest_port\":%d,\"boundary\":\"windows-owner-v1\",\"profile\":\"%s\",\"token\":\"%s\",\"generation\":%s}\n",
        (unsigned long)GetCurrentProcessId(), (unsigned long long)creation.QuadPart,
        ntohs(address.sin_port), usb_windows_listen_port(), profile, token, generation);
    if (fflush(stdout) != 0) goto cleanup;
    fdlist_create(&list);
    result = 0;
    while (!ltw_windows_stopping()) {
        int timeout = usb_get_timeout(), device_timeout = device_get_timeout();
        if (device_timeout < timeout) timeout = device_timeout;
        if (timeout < 0 || timeout > 100) timeout = 100;
        fdlist_reset(&list); fdlist_add(&list, FD_LISTEN, listener, POLLIN);
        usb_get_fds(&list); client_get_fds(&list);
        int ready = poll(list.fds, list.count, timeout);
        if (ready < 0) { result = 2; break; }
        if (ltw_windows_stopping()) break;
        if (usb_process() < 0) { result = 2; break; }
        for (int i = 0; i < list.count && !ltw_windows_stopping(); i++) {
            if (!list.fds[i].revents) continue;
            if (list.owners[i] == FD_LISTEN) client_accept(listener);
            if (list.owners[i] == FD_CLIENT) client_process((int)list.fds[i].fd, list.fds[i].revents);
        }
        device_check_timeouts();
    }
    fdlist_free(&list);
cleanup:
    stopping = 1;
    if (control_invalid) result = 2;
    /* Keep upstream teardown ordering: USB removal still references device
     * and client collections. Never destroy their mutexes first. */
    if (usb_started) usb_shutdown();
    if (initialized) device_shutdown();
    if (initialized) client_shutdown();
    if (listener >= 0) closesocket(listener);
    if (!CloseHandle(input)) result = 2;
    WSACleanup();
    /* Parent must still wait/reap the exact owned child. A line is not proof. */
    if (result == 0) { puts("DAEMON_RETIRED listeners_closed=1 control_closed=1"); fflush(stdout); }
    return result;
}
