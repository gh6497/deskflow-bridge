/* scrcpy wire / bridge regression over local sockets, never Android or USB.
 * Protocol references: scrcpy control_msg / device_msg and HID descriptors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET df_socket;
static int df_socket_init(void) { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data); }
static void df_socket_cleanup(void) { WSACleanup(); }
static void df_close_socket(df_socket fd) { closesocket(fd); }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
typedef int df_socket;
static int df_socket_init(void) { return 0; }
static void df_socket_cleanup(void) {}
static void df_close_socket(df_socket fd) { close(fd); }
#endif
#include "uhid.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int socket_suppress_sigpipe(df_socket fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
    return 0;
#endif
}
static int df_socket_wait_timeout(df_socket fd, int ms)
{
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    struct timeval timeout = {ms / 1000, (ms % 1000) * 1000};
    return select((int)fd + 1, &set, NULL, NULL, &timeout);
}
static int io_read_full(df_socket fd, void *data, size_t size)
{
    char *p = data;
    while (size) {
        int n = (int)recv(fd, p, (int)size, 0);
        if (n <= 0) return 0;
        p += n; size -= (size_t)n;
    }
    return 1;
}
static int io_write_full(df_socket fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int n = (int)send(fd, p, (int)size, flags);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

/* The UHID path must never open or send to an AOA device. */
bool aoa_hid_init(struct aoa_hid *a) { (void)a; abort(); }
bool aoa_hid_open(struct aoa_hid *a, const char *s) { (void)a; (void)s; abort(); }
void aoa_hid_destroy(struct aoa_hid *a) { (void)a; abort(); }
bool aoa_hid_register(struct aoa_hid *a, uint16_t id, const uint8_t *p, uint16_t n)
{ (void)a; (void)id; (void)p; (void)n; abort(); }
bool aoa_hid_send(struct aoa_hid *a, uint16_t id, const uint8_t *p, uint16_t n)
{ (void)a; (void)id; (void)p; (void)n; abort(); }
bool aoa_hid_poll(struct aoa_hid *a) { (void)a; abort(); }
bool aoa_hid_is_disconnected(const struct aoa_hid *a) { (void)a; abort(); }

static void pair(df_socket fds[2])
{
#ifdef _WIN32
    df_socket listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(listener, 1) == 0);
    int n = sizeof(a);
    CHECK(!getsockname(listener, (struct sockaddr *)&a, &n));
    fds[0] = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(!connect(fds[0], (struct sockaddr *)&a, sizeof(a)));
    fds[1] = accept(listener, NULL, NULL); df_close_socket(listener);
#else
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
#endif
    CHECK(!socket_suppress_sigpipe(fds[1]));
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void read_create(df_socket fd, unsigned id, bool absolute)
{
    uint8_t header[8], name[128], length[2], desc[4096];
    CHECK(io_read_full(fd, header, sizeof(header)) == 1);
    CHECK(header[0] == 12 && be16(header + 1) == id && header[7] < 128);
    CHECK(io_read_full(fd, name, header[7]) == 1);
    CHECK(!memcmp(name, "Deskflow UHID ", 14));
    CHECK(io_read_full(fd, length, 2) == 1);
    unsigned size = be16(length);
    CHECK(size >= 6 && size <= sizeof(desc));
    CHECK(io_read_full(fd, desc, size) == 1);
    CHECK(desc[0] == 5);
    if (id == 1) CHECK(desc[1] == 1 && desc[3] == 6); /* Keyboard */
    else CHECK(desc[1] == (absolute ? 0x0d : 1) && desc[3] == (absolute ? 1 : 2));
}

static void bridge_wire(bool absolute)
{
    df_socket fds[2]; pair(fds);
    struct otg_clipboard_channel channel;
    struct otg_uhid hid;
    struct otg_bridge bridge;
    CHECK(!otg_clipboard_channel_open(&channel, (intptr_t)fds[0]));
    otg_uhid_init(&hid, &channel);
    CHECK(!otg_bridge_open_transport(&bridge, otg_uhid_ops(), &hid, 1080, 2340,
                                     absolute ? OTG_MOUSE_ABSOLUTE : OTG_MOUSE_RELATIVE));
    read_create(fds[1], 1, absolute); read_create(fds[1], 2, absolute);
    CHECK(otg_bridge_ready(&bridge));
    /* Absolute transport carries authoritative corner coordinates. Relative
     * mode is covered at registration without flooding the socket with warp. */
    if (absolute) {
        otg_bridge_enter(&bridge, 0, 0, 0);
        otg_bridge_key(&bridge, 'a', true);
        otg_bridge_mouse_move(&bridge, 1079, 2339);
        otg_bridge_mouse_button(&bridge, 1, true);
        otg_bridge_mouse_wheel(&bridge, 0, 120);
    }
    otg_bridge_close(&bridge);
    unsigned destroyed = 0, key_down = 0, key_release = 0, corner = 0, drag = 0, wheel = 0, out = 0;
    while (destroyed != 6) {
        uint8_t header[5], report[8];
        CHECK(io_read_full(fds[1], header, 3) == 1);
        unsigned id = be16(header + 1);
        CHECK(id == 1 || id == 2);
        if (header[0] == 14) { destroyed |= 1U << id; continue; }
        CHECK(header[0] == 13 && !destroyed);
        CHECK(io_read_full(fds[1], header + 3, 2) == 1 && be16(header + 3) == 8);
        CHECK(io_read_full(fds[1], report, 8) == 1);
        if (id == 1) {
            if (report[2] == 4) ++key_down;
            if (key_down && !memcmp(report, "\0\0\0\0\0\0\0\0", 8)) ++key_release;
        } else {
            if (report[2] == 0xff && report[3] == 0x7f && report[4] == 0xff && report[5] == 0x7f) ++corner;
            if (report[0] == 3 && report[1] == 1) ++drag;
            if (report[6] == 1) ++wheel;
            if (!report[0] && !report[1]) ++out;
        }
    }
    if (absolute) CHECK(key_down && key_release && corner && drag && wheel && out);
    CHECK(!hid.registered);
    otg_clipboard_channel_close(&channel);
    df_close_socket(fds[0]); df_close_socket(fds[1]);
}

static unsigned copies;
static void on_text(void *ud, const char *text, size_t size)
{ (void)ud; CHECK(size == 1 && text[0] == 'x'); ++copies; }

static void multiplex(void)
{
    df_socket fds[2]; pair(fds);
    struct otg_clipboard_channel c;
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    c.shared_uhid = true;
    /* winsock2.h/rpcndr.h defines `small` as a macro, so keep another name. */
    int sndbuf = 4096;
    CHECK(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf)));
    size_t text_size = 200000;
    char *text = malloc(text_size); CHECK(text); memset(text, 'a', text_size);
    CHECK(!otg_clipboard_channel_set(&c, text, text_size));
    free(text);
    CHECK(!otg_control_channel_flush(&c));
    /* Force a partial clipboard message before a HID press and release. */
    const uint8_t press[] = {13, 0, 1, 0, 8, 0, 0, 4, 0, 0, 0, 0, 0};
    uint8_t release[sizeof(press)]; memcpy(release, press, sizeof(press)); release[7] = 0;
    CHECK(!otg_control_channel_send(&c, press, sizeof(press)));
    CHECK(!otg_control_channel_send(&c, release, sizeof(release)));
    size_t total = text_size + 14 + sizeof(press) * 2, got = 0;
    uint8_t *wire = malloc(total); CHECK(wire);
    for (unsigned i = 0; got < total && i < 2000; ++i) {
        CHECK(!otg_control_channel_flush(&c));
        if (df_socket_wait_timeout(fds[1], 1) > 0) {
            int n = (int)recv(fds[1], (char *)wire + got, (int)(total - got), 0);
            CHECK(n > 0); got += (size_t)n;
        }
    }
    CHECK(got == total && wire[0] == 9 && otg_clipboard_be32(wire + 10) == text_size);
    for (size_t i = 14; i < 14 + text_size; ++i) CHECK(wire[i] == 'a');
    CHECK(!memcmp(wire + 14 + text_size, press, sizeof(press)));
    CHECK(!memcmp(wire + 14 + text_size + sizeof(press), release, sizeof(release)));
    uint8_t messages[] = {2, 0, 1, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 'x'};
    memcpy(messages + 7, wire + 1, 8); free(wire);
    /* Fragmented output/ACK/text share one reader; LED output isn't clipboard. */
    for (size_t i = 0; i < sizeof(messages); ++i) {
        CHECK(!io_write_full(fds[1], messages + i, 1));
        CHECK(!otg_clipboard_channel_poll(&c, on_text, NULL));
    }
    CHECK(copies == 1 && !otg_clipboard_channel_busy(&c));
    CHECK(!otg_clipboard_channel_set(&c, "timeout", 7));
    CHECK(!otg_control_channel_flush(&c)); c.deadline_ms = 0;
    CHECK(otg_clipboard_channel_poll(&c, on_text, NULL) == 1);
    CHECK(c.ready && c.clipboard_disabled && !otg_clipboard_channel_busy(&c));
    CHECK(!otg_control_channel_send(&c, press, sizeof(press))); /* HID survives clipboard failure. */
    df_close_socket(fds[1]);
    CHECK(otg_clipboard_channel_poll(&c, on_text, NULL) == -1 && !c.ready);
    otg_clipboard_channel_close(&c); df_close_socket(fds[0]);
}

static void backpressure(void)
{
    df_socket fds[2]; pair(fds);
    struct otg_clipboard_channel c;
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    int sndbuf = 1024;
    CHECK(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf)));
    uint8_t data[4096] = {13};
    unsigned i;
    for (i = 0; i < 4096; ++i)
        if (otg_control_channel_send(&c, data, sizeof(data))) break;
    CHECK(i < 4096 && !c.ready); /* Bounded failure, never drop a release silently. */
    df_close_socket(fds[0]); df_close_socket(fds[1]);
    pair(fds);
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    CHECK(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf)));
    while (!c.tx) CHECK(!otg_control_channel_send(&c, data, sizeof(data)));
    c.tx_deadline_ms = 0;
    CHECK(otg_control_channel_flush(&c) == -1 && !c.ready);
    df_close_socket(fds[0]); df_close_socket(fds[1]);
}

int main(void)
{
    CHECK(!df_socket_init());
    bridge_wire(true); bridge_wire(false); multiplex(); backpressure();
    df_socket_cleanup();
    puts("PASS: UHID descriptors/reports/release, shared partial writes, LED/ACK/text, timeout/EOF/backpressure");
    return 0;
}
