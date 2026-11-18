// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QTimer>

class QProcess;

// Each instance observes one transport's attachments. AOA prefers bridge
// hotplug notifications; wired UHID uses the authorized ADB device list.
class UsbAutoConnect : public QObject {
    Q_OBJECT
public:
    enum class Source { Aoa, Adb };
    explicit UsbAutoConnect(QObject *parent = nullptr, Source source = Source::Aoa);
    ~UsbAutoConnect() override;
    void setProgram(const QString &program);
    void setEnabled(bool enabled);
    void setReady(bool ready);
    void setMultipleDevices(bool enabled) { m_multiple = enabled; }
    void setKnownSerials(const QStringList &serials);
    void suppressSerial(const QString &serial);
    void suppressAttachment(const QString &id, const QString &serial);
    bool acceptSnapshot(const QByteArray &data);
    void poll();
    bool usesHotplug() const { return m_hotplug; }
signals:
    void connectRequested(const QString &serial);
    void scanFailed();
    void detectionModeChanged(bool hotplug);
private:
    void startWatcher();
    void stopWatcher();
    void fallbackToPolling();
    void readNotifications();
    void scheduleScan(int attempts = 1);
    const Source m_source;
    QTimer m_timer;
    QTimer m_watchDeadline, m_rescan;
    QProcess *m_watcher = nullptr;
    QByteArray m_notifications;
    int m_rescansLeft = 0;
    QString m_program;
    QSet<QString> m_known, m_attempted, m_suppressed;
    QMap<QString, QString> m_present;
    bool m_enabled = false;
    bool m_multiple = false;
    bool m_ready = false;
    bool m_scanning = false;
    bool m_scanPending = false;
    bool m_hotplug = false;
    bool m_reportedError = false;
    quint64 m_generation = 0;
};
