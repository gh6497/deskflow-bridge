// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <optional>

// Optional, best-effort status channel. Never connects/pairs ADB or changes HID.
class CompanionStatusSync : public QObject {
    Q_OBJECT
public:
    enum class UsbDependency { None, Required, FromAdb };
    explicit CompanionStatusSync(QObject *parent = nullptr);
    void update(const QString &adb, const QString &target, const QString &state,
                bool playbackReady = true, UsbDependency usb = UsbDependency::None,
                const QString &transport = {});

private:
    void send();
    QTimer m_heartbeat;
    QString m_adb, m_target, m_state, m_transport;
    QString m_connectionId;
    UsbDependency m_usb = UsbDependency::None;
    std::optional<bool> m_usbAdb;
    QElapsedTimer m_playDeadline;
    bool m_playbackReady = false, m_playPending = false, m_playIssued = false;
    quint64 m_revision = 0;
    bool m_busy = false, m_pending = false;
};
