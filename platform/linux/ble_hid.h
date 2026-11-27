/* Linux Bluetooth LE HID over GATT transport. SPDX-License-Identifier: Apache-2.0 */
#ifndef DESKFLOW_BRIDGE_BLE_HID_H
#define DESKFLOW_BRIDGE_BLE_HID_H

#include "bridge.h"
#include "ble_limits.h"

#ifdef __linux__
#include <stdbool.h>
#include <stdint.h>

struct otg_ble_hid;

struct otg_ble_hid *otg_ble_hid_create(const char *adapter, bool legacy_handles);
struct otg_ble_hid *otg_ble_hid_create_resident(const char *adapter, bool legacy_handles);
/* Directed resident sessions share one GATT application. A route is owned by
 * one IPC connection. Generation changes invalidate its buffered input. */
struct otg_ble_route;
struct otg_ble_hid *otg_ble_hid_create_directed(const char *adapter, bool legacy_handles,
                                                const char *verified_instance);
bool otg_ble_instance_matches(const char *instance);
bool otg_ble_verified_instance(char *token,size_t capacity);
struct otg_ble_route *otg_ble_route_bind(struct otg_ble_hid *hid, const char *address);
void otg_ble_route_enable_discovery(struct otg_ble_hid *hid, bool enabled);
struct otg_ble_route *otg_ble_route_claim_discovered(struct otg_ble_hid *hid);
const char *otg_ble_route_identity(const struct otg_ble_route *route);
char *otg_ble_route_dup_peer_name(const struct otg_ble_hid *ble, const struct otg_ble_route *route);
void otg_ble_route_unbind(struct otg_ble_route *route);
bool otg_ble_route_ready(const struct otg_ble_route *route, uint64_t *generation);
bool otg_ble_route_waiting_for_subscriptions(const struct otg_ble_route *route);
void otg_ble_route_send(struct otg_ble_route *route, uint16_t id, const uint8_t data[8]);
bool otg_ble_hid_set_peer(struct otg_ble_hid *b, const char *address);
const char *otg_ble_hid_identity(const struct otg_ble_hid *b);
char *otg_ble_hid_dup_peer_name(const struct otg_ble_hid *b);
bool otg_ble_hid_set_session(struct otg_ble_hid *b, bool enabled);
void otg_ble_hid_set_interval(struct otg_ble_hid *b, uint64_t us);
void otg_ble_hid_destroy(struct otg_ble_hid *b);
const struct otg_hid_transport_ops *otg_ble_hid_ops(void);
const char *otg_ble_hid_adapter(const struct otg_ble_hid *b);

#endif
#endif
