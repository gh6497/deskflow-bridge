/* Offline BLE coalescing tests: no system bus or adapter access.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>

static uint64_t test_now_ns;

static int fake_clock_gettime(clockid_t clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = test_now_ns / UINT64_C(1000000000);
    ts->tv_nsec = test_now_ns % UINT64_C(1000000000);
    return 0;
}

#define clock_gettime fake_clock_gettime
#include "../platform/linux/ble_hid.c"
#undef clock_gettime

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void check_fractional_interval(uint64_t interval_us)
{
    struct otg_ble_hid b = {0};
    b.app_registered = b.notifying[0] = b.notifying[1] = true;
    otg_ble_hid_set_interval(&b, interval_us);
    const uint64_t start = UINT64_C(1000000000), ms = UINT64_C(1000000);
    uint8_t report[8] = {2};
    CHECK(queue_pointer(&b, report, sizeof(report), start));
    unsigned sent = 0;
    for (unsigned tick = 1; tick <= 150; tick++) {
        /* Mouse moves at 1 kHz. The last move in an interval may be flushed
         * by a timer tick or replaced by a fresh report at the deadline. */
        uint64_t now = start + tick * ms;
        bool timer = (sent % 2 == 0) && pointer_due(&b, now);
        if (timer) CHECK(b.values[1][2] == tick - 1);
        report[2] = (uint8_t)tick;
        bool immediate = queue_pointer(&b, report, sizeof(report), now);
        CHECK(!(timer && immediate));
        if (timer || immediate) sent++;
        CHECK(sent == tick * 1000 / interval_us);
    }
    /* After a stall, send the current position once and allow a full period
     * before the next one, even if the old cadence had a slot just ahead. */
    uint64_t restart = start + 299 * ms;
    CHECK(queue_pointer(&b, report, sizeof(report), restart));
    report[2]++;
    CHECK(!queue_pointer(&b, report, sizeof(report), restart + ms));
    CHECK(!pointer_due(&b, restart + interval_us * 1000 - 1));
    CHECK(pointer_due(&b, restart + interval_us * 1000));
}

static void check_directed_cadence(uint64_t interval_us)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
    struct otg_ble_route route = {
        .ready = true,
        .fd = {-1, fds[0]},
        .pointer_interval_ns = interval_us * 1000,
    };
    const uint64_t start = UINT64_C(1000000000), ms = UINT64_C(1000000);
    uint8_t report[8] = {2}, received[8];
    test_now_ns = start;
    otg_ble_route_send(&route, AOA_HID_ID_MOUSE, report);
    CHECK(recv(fds[1], received, sizeof(received), 0) == sizeof(received));

    unsigned sent = 0;
    for (unsigned tick = 1; tick <= 150; ++tick) {
        test_now_ns = start + tick * ms;
        const bool flush_first = sent % 2 == 0;
        if (flush_first) directed_flush_motion(&route, test_now_ns);
        report[2] = (uint8_t)tick;
        otg_ble_route_send(&route, AOA_HID_ID_MOUSE, report);
        if (recv(fds[1], received, sizeof(received), 0) == sizeof(received)) {
            CHECK(received[2] == tick - flush_first);
            CHECK(recv(fds[1], received, sizeof(received), 0) < 0 && errno == EAGAIN);
            ++sent;
        }
        CHECK(sent == tick * 1000 / interval_us);
    }

    /* A final position must arrive even when no more Deskflow input follows. */
    const uint64_t due = start + 150 * ms + route.pointer_interval_ns;
    test_now_ns += ms;
    report[2]++;
    otg_ble_route_send(&route, AOA_HID_ID_MOUSE, report);
    directed_flush_motion(&route, due - 1);
    CHECK(recv(fds[1], received, sizeof(received), 0) < 0 && errno == EAGAIN);
    directed_flush_motion(&route, due);
    CHECK(recv(fds[1], received, sizeof(received), 0) == sizeof(received));
    CHECK(memcmp(received, report, sizeof(report)) == 0);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    check_fractional_interval(7500);
    check_fractional_interval(12500);
    check_directed_cadence(7500);
    check_directed_cadence(12500);
    check_directed_cadence(15000);
    struct otg_ble_hid b = {0};
    b.app_registered = b.notifying[0] = b.notifying[1] = true;
    uint64_t t = UINT64_C(1000000000);
    uint8_t report[8] = {2, 0, 10, 0, 20, 0, 0, 0};
    CHECK(queue_pointer(&b, report, 8, t));
    for (int i = 1; i <= 10; ++i) {
        report[2] = (uint8_t)i;
        CHECK(!queue_pointer(&b, report, 8, t + i * UINT64_C(1000000)));
    }
    CHECK(b.values[1][2] == 10);
    CHECK(!pointer_due(&b, t + BLE_POINTER_INTERVAL_NS - 1));
    CHECK(pointer_due(&b, t + BLE_POINTER_INTERVAL_NS));
    CHECK(!pointer_due(&b, t + BLE_POINTER_INTERVAL_NS + 1));

    t += BLE_POINTER_INTERVAL_NS + 2;
    report[0] = 3;
    report[1] = 1;
    CHECK(queue_pointer(&b, report, 8, t)); /* press is immediate */
    report[2] = 80;
    CHECK(!queue_pointer(&b, report, 8, t + 1)); /* drag coalesces */
    report[0] = 2;
    report[1] = 0;
    CHECK(queue_pointer(&b, report, 8, t + 2)); /* release is immediate */
    CHECK(!b.pointer_pending);
    report[6] = 1;
    CHECK(queue_pointer(&b, report, 8, t + 3)); /* wheel is immediate */
    CHECK(queue_pointer(&b, report, 8, t + 4)); /* repeated wheel preserved */
    report[6] = 0;
    report[0] = 0;
    CHECK(queue_pointer(&b, report, 8, t + 5)); /* range exit immediate */

    report[0] = 2;
    CHECK(queue_pointer(&b, report, 8, t + 6));
    CHECK(!queue_pointer(&b, report, 8, t + 7));
    b.notifying[0] = false;
    CHECK(!pointer_due(&b, t + 2 * BLE_POINTER_INTERVAL_NS));
    CHECK(!b.pointer_pending); /* disconnected input never replayed */

    /* Real unprivileged IPC sockets, but still no Bluetooth hardware access. */
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0);
    b.latency_fd = fds[0];
    b.adapter = "hci0";
    latency_state(&b, true);
    char message[64] = {0};
    CHECK(recv(fds[1], message, sizeof(message), 0) == 11);
    CHECK(strcmp(message, "ready:hci0\n") == 0);
    const char slow[] = "interval_us=45000\n";
    CHECK(send(fds[1], slow, sizeof(slow) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(45000000));
    const char fast[] = "interval_us=7500\n";
    CHECK(send(fds[1], fast, sizeof(fast) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(7500000));
    const char invalid[] = "interval_us=0\n";
    CHECK(send(fds[1], invalid, sizeof(invalid) - 1, 0) > 0);
    latency_poll(&b);
    CHECK(pointer_interval(&b) == UINT64_C(7500000));
    b.notifying[0] = true;
    t += UINT64_C(1000000000);
    CHECK(queue_pointer(&b, report, 8, t));
    report[2]++;
    CHECK(!queue_pointer(&b, report, 8, t + 1));
    CHECK(!pointer_due(&b, t + UINT64_C(7499999)));
    CHECK(pointer_due(&b, t + UINT64_C(7500000)));
    otg_ble_hid_set_interval(&b, 12500);
    CHECK(pointer_interval(&b) == UINT64_C(12500000));
    otg_ble_hid_set_interval(&b, 15000);
    CHECK(pointer_interval(&b) == UINT64_C(15000000));
    latency_state(&b, false);
    CHECK(pointer_interval(&b) == BLE_POINTER_INTERVAL_NS);
    close(fds[0]);
    close(fds[1]);
    b.latency_fd = -1;
    b.peer_connected = true;
    /* A fresh process has no subscription evidence. Connected/Paired alone
     * must not fabricate CCC state, even when the phone reports connected. */
    b.notifying[0] = b.notifying[1] = false;
    update_channel_state(&b);
    CHECK(!ready_ble(&b));
    b.notifying[0] = true;
    update_channel_state(&b);
    CHECK(!ready_ble(&b));
    b.notifying[0] = b.notifying[1] = true;
    update_channel_state(&b);
    CHECK(ready_ble(&b));
    b.values[0][2] = 4; /* Held key and button from the previous connection. */
    b.values[1][1] = 1;
    b.pointer_pending = true;
    b.peer_connected = false;
    update_channel_state(&b);
    CHECK(!ready_ble(&b));
    CHECK(b.notifying[0] && b.notifying[1]); /* Bonded BlueZ CCCs are retained. */
    CHECK(!b.values[0][2] && !b.values[1][1] && !b.pointer_pending);
    b.peer_connected = true;
    update_channel_state(&b);
    CHECK(ready_ble(&b)); /* No second StartNotify needed on cached reconnect. */

    struct ble_object map={&b,BLE_MAP,0};
    struct ble_object keys={&b,BLE_REPORT,0};
    struct ble_object pointer={&b,BLE_REPORT,1};
    struct ble_object led_ref={&b,BLE_REF,2};
    CHECK(object_handle(&map)==0x102);
    CHECK(object_handle(&keys)==0x106 && object_handle(&pointer)==0x10a);
    CHECK(object_handle(&led_ref)==0x10f);
    CHECK(!strcmp(path_for(&led_ref),ROOT "/hid/leds/reference"));
    GVariant *properties=g_variant_ref_sink(props(&keys,iface_for(BLE_REPORT)));
    guint16 handle=0;
    CHECK(g_variant_lookup(properties,"Handle","q",&handle) && handle==0x106);
    g_variant_unref(properties);
    b.adapter_path="/org/bluez/hci0";
    CHECK(!otg_ble_hid_set_peer(&b,"invalid"));
    CHECK(otg_ble_hid_set_peer(&b,"aa:bb:cc:dd:ee:ff"));
    CHECK(!strcmp(b.peer_address,"AA:BB:CC:DD:EE:FF") && !b.peer_path);
    CHECK(otg_ble_hid_set_peer(&b,"AA:BB:CC:DD:EE:FF"));
    CHECK(!otg_ble_hid_set_peer(&b,"11:22:33:44:55:66"));
    GVariant *empty=g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0));
    CHECK(!target_options(&b,empty));g_variant_unref(empty);
    b.peer_path=g_strdup("/org/bluez/hci0/dev_52_00_00_00_00_01");
    GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path(b.peer_path));
    GVariant *target=g_variant_ref_sink(g_variant_builder_end(&options));
    CHECK(target_options(&b,target));g_variant_unref(target);g_free(b.peer_path);
    puts("PASS: latest coordinates, drag, release, wheel, range and cancellation");
    return 0;
}
