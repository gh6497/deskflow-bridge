/* scrcpy UHID transport; descriptors/reports remain owned by bridge.c.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_UHID_H
#define OTG_UHID_H
#include "bridge.h"
#include "clipboard.h"

struct otg_uhid {
    struct otg_clipboard_channel *channel;
    unsigned registered;
};
void otg_uhid_init(struct otg_uhid *hid, struct otg_clipboard_channel *channel);
const struct otg_hid_transport_ops *otg_uhid_ops(void);
#endif
