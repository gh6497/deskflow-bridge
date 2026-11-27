// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QSize>
#include <QTemporaryFile>
#include <functional>
#include <memory>
#include "deskflow_server.h"
#include "companion_status_sync.h"
class QJsonObject;

struct SharedBleLaunch;
struct BleStartupCoordinator {
    std::weak_ptr<SharedBleLaunch> pending;
};

struct ConnectionOptions {
    QString deskflow;
    QString deskflowConfig;
    QString bridge;
    QString adb;
    QString scrcpyServer;
    QString scrcpyVersion;
    QString serial;
    QString adbSerial;
    QString host = "127.0.0.1";
    QString computer = "computer";
    QString phone = "android";
    QString mouseMode = "absolute";
    bool absolutePerAxis = false;
    QString middleButtonAction = "home";
    QString rightButtonAction = "back";
    QString sideButton1Action = "swipe-up";
    QString sideButton2Action = "swipe-down";
    int sideSwipeDistancePercent = 17;
    QString unlockPin;
    bool unlockAdb = false;
    bool unlockWithSpace = true;
    QString tlsCertificate;
    QString tlsFingerprint;
    QString tlsFingerprintFile;
    QString transport = "usb";
#ifdef Q_OS_WIN
    QString adapter; // Windows BLE only supports the default adapter.
#else
    QString adapter = "hci0";
#endif
    QString blePeer;
    QString bleIdentity;
    bool bleAutoAttach = true;
    bool bleDiscovery = false;
    bool bleLegacyHandles = false;
    bool requireDirectedBle = false;
    bool preferDirectedBle = false;
    QString windowsBleToken; // Private GUI-owned shared provider, never persisted.
    QString blePrivilegeProgram = "/usr/bin/pkexec";
    std::function<bool(const QString &)> confirmSharedBleService;
    int port = 24800;
    int width = 1080;
    int height = 1920;
    double motionMultiplier = 1.0;
    bool manageServer = true;
    bool tls = false;
    bool audio = false;
    int audioGainDb = 0;
    bool clipboard = false;
    bool autoRotate = false;
    // Non-GUI callers historically enabled ADB implicitly by selecting an
    // auxiliary feature. The GUI always overrides this from the device
    // profile, so retain that API behavior for CLI/test integrations.
    bool adbEnabled = true;
};

// Only owned bridge/server processes are stopped. A resident BLE service is
// deliberately retained across sessions and GUI exits to preserve BlueZ CCCs.
// Windows BLE is parked between GUI sessions; only explicit service stop or
// GUI shutdown terminates its provider. It is not a detached system service.
class Session : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Starting, Connected, Stopping };
    explicit Session(QObject *parent = nullptr, std::shared_ptr<BleStartupCoordinator> bleStartup = {});
    ~Session() override;
    State state() const { return m_state; }
    QString statusMessage() const;
    QString clipboardStatusMessage() const;
    bool hasFailure() const { return !m_failureId.isEmpty(); }
    void start(const ConnectionOptions &options);
    void stop();
    void shutdown();
    void stopBleProvider();
    bool configureBle(const ConnectionOptions &options);
    bool hasBleProvider() const { return m_bleProvider.state() != QProcess::NotRunning; }
    const char *bleProviderStatusId() const;

signals:
    void changed(Session::State state, const QString &message);
    void log(const QString &message);
    void usbAttached(const QString &id, const QString &serial);
    void bleProviderChanged();
    void bleIdentityDetected(const QString &identity, const QString &name);
    void clipboardChanged();
    void displaySizeChanged(const QSize &size);

private:
    void setState(State state, const char *id, const QString &detail = {});
    void syncCompanionStatus();
    void fail(const char *id, const QString &detail = {});
    void launchBridge();
    void prepareBle(bool started = false, int attempts = 0);
#ifdef Q_OS_LINUX
    void launchDirectedBle(const QString &token, const QString &logPath);
#endif
    void startConnection();
    void stopServer();
    void finishStop();
    void readBridgeEvents();
    void readBleEvents();
    void handleBridgeEvent(const QJsonObject &object);
    bool windowsBle() const;
    void endBleProvider();
    ConnectionOptions m_options;
    State m_state = State::Idle;
    DeskflowServer m_server;
    CompanionStatusSync m_companionStatus;
    QProcess m_bridge, m_bleProvider;
    QTimer m_deadline, m_killBridge, m_killBle;
    QTemporaryFile m_unlockPinFile;
    QByteArray m_events;
    QByteArray m_bleEvents;
    QString m_bleProgram, m_blePeer;
    QStringList m_bleFixedSettings;
    bool m_bleActive = false, m_shutdown = false, m_stoppingBleProvider = false;
    bool m_bleDiscovery = false;
    bool m_audioReady = false;
    bool m_directedBle = false;
    std::shared_ptr<BleStartupCoordinator> m_bleStartup;
    std::shared_ptr<SharedBleLaunch> m_bleLaunch;
    bool m_adbServerStarted = false, m_adbTargetPrepared = false;
    int m_adbTargetAttempts = 0;
    QByteArray m_failureId, m_statusId;
    QByteArray m_clipboardStatusId = "otg.clipboard.off";
    QString m_failureDetail, m_statusDetail;
    quint64 m_generation = 0;
};
