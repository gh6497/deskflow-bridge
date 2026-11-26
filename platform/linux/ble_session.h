/* Native resident GATT service and local bridge sessions.
 * BlueZ owns CCCs; keeping its application registered preserves their lifetime.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef OTG_BLE_SESSION_H
#define OTG_BLE_SESSION_H
#include "bridge.h"
#include <signal.h>
#include <sys/types.h>
struct otg_ble_session;
int otg_ble_service_run(const char *adapter, bool legacy_handles, volatile sig_atomic_t *stop);
int otg_ble_multi_service_run(const char *adapter,bool legacy_handles,const char *instance,
                              int supervisor_fd,bool watch_startup,volatile sig_atomic_t *stop);
int otg_ble_supervisor_fd(void);
struct otg_ble_session *otg_ble_directed_session_open(const char *adapter,bool legacy_handles,
                                                      const char *peer,const char *screen);
struct otg_ble_session *otg_ble_session_open(const char *adapter, bool legacy_handles);
bool otg_ble_session_set_peer(struct otg_ble_session *session, const char *address);
bool otg_ble_session_waiting_for_subscriptions(const struct otg_ble_session *session);
/* Read-only, same UID status; never attaches or changes Bluetooth settings. */
int otg_ble_service_status(const char *adapter);
int otg_ble_service_stop(const char *adapter);
const struct otg_hid_transport_ops *otg_ble_session_ops(void);
void otg_ble_session_destroy(struct otg_ble_session *session);
/* Supervisor's read-only barrier before restoring modes after a child exits. */
bool otg_ble_service_wait_idle(uid_t uid, const char *adapter);
bool otg_ble_service_single_idle(uid_t uid,const char *adapter);
#endif
