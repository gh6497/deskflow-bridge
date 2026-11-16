/* scrcpy control_msg.c / UhidManager.java: CREATE(12), INPUT(13), DESTROY(14).
 * No SDK input injection: send the bridge's original keyboard/tablet reports.
 * SPDX-License-Identifier: Apache-2.0 */
#include "uhid.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static bool register_report(void *ctx, uint16_t id, const uint8_t *desc, uint16_t size)
{
    struct otg_uhid *hid = ctx;
    if (id < 1 || id > 2 || !desc || !size || size > 4096 ||
        (hid->registered & (1U << id))) return false;
    const char *name = id == 1 ? "Deskflow UHID keyboard" : "Deskflow UHID pointer";
    size_t length = strlen(name);
    uint8_t message[10 + 127 + 4096] = {12};
    put16(message + 1, id);
    /* Generic virtual HID IDs; avoid vendor-specific kernel quirks. */
    put16(message + 3, 0); put16(message + 5, 0);
    message[7] = (uint8_t)length;
    memcpy(message + 8, name, length);
    put16(message + 8 + length, size);
    memcpy(message + 10 + length, desc, size);
    if (otg_control_channel_send(hid->channel, message, 10 + length + size)) return false;
    hid->registered |= 1U << id;
    return true;
}

static bool send_report(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{
    struct otg_uhid *hid = ctx;
    if (id < 1 || id > 2 || !(hid->registered & (1U << id)) ||
        !data || !size || size > 4096) return false;
    uint8_t message[5 + 4096] = {13};
    put16(message + 1, id); put16(message + 3, size);
    memcpy(message + 5, data, size);
    return otg_control_channel_send(hid->channel, message, 5 + size) == 0;
}

static bool poll_transport(void *ctx)
{
    struct otg_uhid *hid = ctx;
    /* Main owns the reader and dispatches clipboard/LED messages together. */
    return otg_control_channel_flush(hid->channel) == 0;
}

static bool ready(void *ctx)
{
    struct otg_uhid *hid = ctx;
    return hid->registered == 6 && hid->channel->ready;
}

static void close_transport(void *ctx)
{
    struct otg_uhid *hid = ctx;
    /* Bridge leave ends the pointer range; AOA unregister/BLE close have
     * their own keyboard release. Do the same explicitly before UHID destroy,
     * even if Deskflow disconnected while a key was held outside the screen. */
    if (hid->registered & (1U << 1)) {
        const uint8_t released[8] = {0};
        (void)send_report(hid, 1, released, sizeof(released));
    }
    for (uint16_t id = 1; id <= 2; ++id) {
        if (!(hid->registered & (1U << id))) continue;
        uint8_t message[3] = {14};
        put16(message + 1, id);
        (void)otg_control_channel_send(hid->channel, message, sizeof(message));
    }
    hid->registered = 0;
    /* Session stop drains pending releases/destroys with a bounded deadline,
     * then closes the socket. Server EOF also destroys all UHID devices. */
}

void otg_uhid_init(struct otg_uhid *hid, struct otg_clipboard_channel *channel)
{
    memset(hid, 0, sizeof(*hid));
    hid->channel = channel;
    channel->shared_uhid = true;
}

const struct otg_hid_transport_ops *otg_uhid_ops(void)
{
    static const struct otg_hid_transport_ops ops = {
        register_report, send_report, poll_transport, ready, close_transport
    };
    return &ops;
}
