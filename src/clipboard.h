/* Deskflow clipboard and scrcpy control wire helpers (independent implementation).
 * Protocol references: Deskflow IClipboard/ClipboardChunk, scrcpy control_msg.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_CLIPBOARD_H
#define OTG_CLIPBOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* scrcpy 5.0.1 SET_CLIPBOARD: 256 KiB minus the 14-byte header. */
#define OTG_CLIPBOARD_TEXT_MAX ((1U << 18) - 14U)
#define OTG_CLIPBOARD_WIRE_MAX (4U * 1024U * 1024U)

bool otg_clipboard_valid_text(const char *text, size_t size);
uint32_t otg_clipboard_be32(const uint8_t *p);
void otg_clipboard_put32(uint8_t *p, uint32_t value);
/* 1 = UTF-8 text found, 0 = no text format, -1 = malformed/oversize. */
int otg_clipboard_decode(const uint8_t *data, size_t size, const char **text, size_t *length);
uint8_t *otg_clipboard_encode(const char *text, size_t size, size_t *length);

struct otg_control_packet;
/* Main-thread-only, nonblocking shared scrcpy control channel. Does not own fd.
 * Clipboard and UHID use a single writer; partial messages never interleave. */
struct otg_clipboard_channel {
    intptr_t fd;
    bool ready;
    uint8_t *rx, *tx, *queued, *last_set;
    size_t rx_size, tx_size, tx_pos, queued_size;
    size_t last_set_size;
    uint64_t sequence, pending, deadline_ms;
    struct otg_control_packet *control_head, *control_tail;
    unsigned control_count;
    bool tx_control, clipboard_disabled, shared_uhid;
    uint64_t tx_deadline_ms;
    uint64_t write_order, queued_order;
};
int otg_clipboard_channel_open(struct otg_clipboard_channel *c, intptr_t fd);
void otg_clipboard_channel_close(struct otg_clipboard_channel *c);
int otg_clipboard_channel_set(struct otg_clipboard_channel *c, const char *text, size_t size);
bool otg_clipboard_channel_busy(const struct otg_clipboard_channel *c);
/* Disable only clipboard policy, keeping a shared UHID stream framed/alive. */
void otg_clipboard_channel_disable(struct otg_clipboard_channel *c);
/* Copies one complete control message into a bounded FIFO. No socket reads. */
int otg_control_channel_send(struct otg_clipboard_channel *c, const uint8_t *data, size_t size);
int otg_control_channel_flush(struct otg_clipboard_channel *c);
bool otg_control_channel_pending(const struct otg_clipboard_channel *c);
/* Returns -1 on EOF/protocol/write error, +1 on a shared clipboard ACK timeout.
 * Never waits for socket input. UHID output reports are framed and drained. */
int otg_clipboard_channel_poll(struct otg_clipboard_channel *c,
    void (*on_text)(void *, const char *, size_t), void *ud);
#endif
