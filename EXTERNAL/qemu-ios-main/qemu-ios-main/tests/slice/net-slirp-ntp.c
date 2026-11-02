/* The user netdev's time server on a dated machine (net/slirp.c slirp_ntp_reply): a guest's NTP client request,
 * over IPv4 or IPv6 to any server, gets a server reply from the machine's clock, addressed back to the guest
 * with valid IP and UDP checksums and the client's transmit time as its originate time; anything else (other
 * ports, other modes, fragments, short frames) is left to slirp.
 *
 * SLICE net/checksum.c fn net_checksum_add_cont net_checksum_finish
 * SLICE include/net/checksum.h range static inline uint32_t\nnet_checksum_add(int | /**
 * SLICE net/slirp.c define NTP_
 * SLICE net/slirp.c fn ntp_stamp slirp_ntp_reply
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint16_t lduw_be_p(const void *p) { const uint8_t *b = p; return b[0] << 8 | b[1]; }
static void stw_be_p(void *p, uint16_t v) { uint8_t *b = p; b[0] = v >> 8; b[1] = v; }
static void stl_be_p(void *p, uint32_t v) { uint8_t *b = p; b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static uint32_t ldl(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
#include "slice.h"

static const uint8_t guest_mac[6] = {0x02, 0, 0, 0, 0, 0x15}, router_mac[6] = {0x52, 0x55, 10, 0, 2, 2};
static const uint8_t client_tx[8] = {0xd5, 0x5f, 0x2a, 0x10, 0x12, 0x34, 0x56, 0x78};

/* Ones' complement sum, written out here rather than taken from the slice. */
static uint32_t sum16(const uint8_t *p, size_t n, uint32_t s)
{
    for (size_t i = 0; i < n; i += 2) {
        s += (uint32_t)p[i] << 8 | (i + 1 < n ? p[i + 1] : 0);
    }
    return s;
}
static uint16_t fold(uint32_t s)
{
    while (s >> 16) {
        s = (s & 0xffff) + (s >> 16);
    }
    return s;
}

/* An NTP client request (SNTP v4, mode 3) from guest port 49152 to `server`, IPv4 (4-byte addresses) or IPv6. */
static size_t request(uint8_t *f, const uint8_t *server, int alen, uint16_t dport, uint8_t mode)
{
    static const uint8_t guest4[4] = {10, 0, 2, 15};
    static const uint8_t guest6[16] = {0xfe, 0xc0, [15] = 0x15};
    size_t ip = 14, iplen = alen == 4 ? 20 : 40;
    uint8_t *u = f + ip + iplen, *n = u + 8;

    memset(f, 0, 200);
    memcpy(f, router_mac, 6);
    memcpy(f + 6, guest_mac, 6);
    if (alen == 4) {
        stw_be_p(f + 12, 0x0800);
        f[ip] = 0x45;
        stw_be_p(f + ip + 2, 20 + 8 + 48);
        stw_be_p(f + ip + 6, 0x4000);           /* DF */
        f[ip + 8] = 255;
        f[ip + 9] = 17;
        memcpy(f + ip + 12, guest4, 4);
        memcpy(f + ip + 16, server, 4);
    } else {
        stw_be_p(f + 12, 0x86dd);
        f[ip] = 0x60;
        stw_be_p(f + ip + 4, 8 + 48);
        f[ip + 6] = 17;
        f[ip + 7] = 255;
        memcpy(f + ip + 8, guest6, 16);
        memcpy(f + ip + 24, server, 16);
    }
    stw_be_p(u, 49152);
    stw_be_p(u + 2, dport);
    stw_be_p(u + 4, 8 + 48);
    n[0] = 4 << 3 | mode;
    n[2] = 6;                                   /* poll 64 s */
    memcpy(n + 40, client_tx, 8);
    return 14 + iplen + 8 + 48;
}

/* The reply to `req` (len) at Unix time `us`, checked field by field; returns the reply's NTP block. */
static const uint8_t *check_reply(const uint8_t *req, size_t len, uint64_t us, uint8_t *out, int alen)
{
    size_t iplen = alen == 4 ? 20 : 40, n = slirp_ntp_reply(req, len, us, out);
    const uint8_t *ip = out + 14, *rip = req + 14, *u = ip + iplen, *ntp = u + 8;

    assert(n == 14 + iplen + 8 + 48);
    assert(!memcmp(out, guest_mac, 6) && !memcmp(out + 6, router_mac, 6));
    assert(!memcmp(out + 12, req + 12, 2));
    if (alen == 4) {
        assert(ip[0] == 0x45 && ip[9] == 17 && ip[8] > 0 && lduw_be_p(ip + 2) == 20 + 8 + 48);
        assert(!memcmp(ip + 12, rip + 16, 4) && !memcmp(ip + 16, rip + 12, 4));
        assert(fold(sum16(ip, 20, 0)) == 0xffff);   /* the header checksum verifies */
    } else {
        assert(ip[0] >> 4 == 6 && ip[6] == 17 && ip[7] > 0 && lduw_be_p(ip + 4) == 8 + 48);
        assert(!memcmp(ip + 8, rip + 24, 16) && !memcmp(ip + 24, rip + 8, 16));
    }
    assert(lduw_be_p(u) == 123 && lduw_be_p(u + 2) == 49152 && lduw_be_p(u + 4) == 8 + 48);
    /* UDP checksum over the pseudo-header: addresses, protocol, length; nonzero (IPv6 requires one) */
    assert(lduw_be_p(u + 6) != 0);
    uint32_t s = sum16(ip + (alen == 4 ? 12 : 8), 2 * alen, 17 + 8 + 48);
    assert(fold(sum16(u, 8 + 48, s)) == 0xffff);

    assert((ntp[0] & 7) == 4 && (ntp[0] >> 3 & 7) == 4 && ntp[0] >> 6 == 0);   /* server, v4, no leap warning */
    assert(ntp[1] >= 1 && ntp[1] <= 15);       /* a synchronized stratum */
    assert(!memcmp(ntp + 24, client_tx, 8));   /* originate = the client's transmit */
    for (int t = 16; t <= 40; t += 8) {        /* reference, receive, transmit: the machine's clock */
        if (t == 24) {
            continue;
        }
        assert(ldl(ntp + t) == us / 1000000 + 2208988800u);
        uint64_t frac = (uint64_t)ldl(ntp + t + 4) * 1000000 >> 32;
        assert(frac + 1 >= us % 1000000 && frac <= us % 1000000);
    }
    return ntp;
}

int main(void)
{
    static const uint8_t apple4[4] = {17, 253, 4, 125};
    static const uint8_t apple6[16] = {0x26, 0x20, 0x01, 0x49, [15] = 0x1f};
    uint8_t req[200], out[200];
    size_t len;
    const uint64_t beta = 1370894400ull * 1000000 + 250000;   /* 2013-06-10 20:00:00.25 UTC, a 7.0 beta 1 date */

    len = request(req, apple4, 4, 123, 3);
    check_reply(req, len, beta, out, 4);
    len = request(req, apple6, 16, 123, 3);
    check_reply(req, len, beta, out, 16);
    /* a frame padded past the datagram (a short Ethernet frame's padding, trailing bytes) is still answered */
    len = request(req, apple4, 4, 123, 3);
    check_reply(req, len + 6, beta, out, 4);

    /* left to slirp */
    len = request(req, apple4, 4, 53, 3);
    assert(!slirp_ntp_reply(req, len, beta, out));            /* not NTP */
    len = request(req, apple4, 4, 123, 4);
    assert(!slirp_ntp_reply(req, len, beta, out));            /* a server's packet, not a request */
    len = request(req, apple4, 4, 123, 3);
    assert(!slirp_ntp_reply(req, len - 1, beta, out));        /* truncated */
    assert(!slirp_ntp_reply(req, 13, beta, out));
    stw_be_p(req + 14 + 6, 0x2000);                           /* a first fragment (MF) */
    assert(!slirp_ntp_reply(req, len, beta, out));
    len = request(req, apple4, 4, 123, 3);
    req[14 + 9] = 6;                                          /* TCP */
    assert(!slirp_ntp_reply(req, len, beta, out));
    len = request(req, apple6, 16, 123, 3);
    req[14 + 6] = 0;                                          /* an IPv6 extension header first */
    assert(!slirp_ntp_reply(req, len, beta, out));
    stw_be_p(req + 12, 0x0806);                               /* ARP */
    assert(!slirp_ntp_reply(req, len, beta, out));

    puts("net-slirp-ntp: ok");
    return 0;
}
