// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device_info.h"
#include "session.h"
#include "connection_manager.h"
#include "usb_auto_connect.h"
#include "companion_app.h"
#include "phone_clipboard.h"
#include "phone_screenshot.h"
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QVariantMap>

class QCheckBox;
class QAction;
class QSystemTrayIcon;
class QMenu;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSettings;
class QListWidget;
class QTabWidget;
class QTimer;
class TransferDialog;
class WirelessAdbDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
signals:
    void exitReady();
protected:
    virtual bool trayAvailable() const;
    virtual QString bridgeExecutable() const;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void setTranslatedProperty(QObject *object, const char *property, const char *id);
    template<class T> T *translated(T *object, const char *property, const char *id)
    {
        setTranslatedProperty(object, property, id);
        return object;
    }
    void addTranslatedRow(QFormLayout *form, const char *id, QWidget *field);
    void retranslateUi();
    void setupTray();
    void restoreWindow();
    void requestExit();
    void updateTray();
    void copyScreenshot(const QString &id);
    void updateScreenshotActions();
    void transferClipboard(PhoneClipboard::Direction direction);
    void updateClipboardAction();
    void readDisplayFromAdb();
    void connectDevice();
    void startBleService();
    void continueBleDiscovery();
    void refreshBleServiceStatus();
    bool confirmBleIsolation(const ConnectionOptions &options);
    void stopBleService();
    void finishBleServiceStop();
    void updateBleServiceControls(bool sharedIdle);
    void saveSettings();
    void saveSharedBleSettings(QSettings &settings) const;
    QVariantMap deviceProfile() const;
    void applyDeviceProfile(const QVariantMap &profile);
    void setProfileTransport(const QString &transport);
    void acceptBleIdentity(const QString &sourceId, const QString &identity);
    bool lockConnectedUhidUsbSerial(const QString &id);
    void rememberDevice(const QVariantMap &profile);
    void autoConnectDevice(const QString &transport, const QString &serial);
    void updateEnabled();
    void updateConnectionStatus();
    QSize virtualScreenSize() const;
    void updateVirtualSize();
    void addDevice();
    void removeDevice();
    void selectDevice(const QString &id);
    void storeDevice();
    void updateDeviceList();
    void connectAll();
    bool screenInUse(const QString &screen) const;
    bool otherScreenAutoConnect(const QString &screen) const;
    ConnectionOptions connectionOptions(const QVariantMap &profile);
    void persistDevices();
    void updateAutoConnect();
    void regenerateTlsCertificate();
    void setupCompanion(CompanionApp::Action action);
    void setupWirelessAdb();
    void updateCompanionStatus();
    QString adbTargetForCurrentDevice() const;
    QString adbActionTarget(const QString &id, const QVariantMap &profile) const;
    void appendLog(const QString &text, const QString &id = {});
    void refreshLog();
    QWidget *pathField(QLineEdit *&field, const char *placeholderId,
                       const char *dialogId = nullptr);
    struct TextBinding {
        QPointer<QObject> object;
        QByteArray property;
        QByteArray id;
    };
    QList<TextBinding> m_textBindings;
    QComboBox *m_language;
    ConnectionManager m_connections;
    Session *m_session = nullptr;
    QString m_selectedId;
    QMap<QString, QVariantMap> m_records;
    QSet<QString> m_unsavedDevices;
    QMap<QString, QVariantMap> m_pendingProfiles;
    QMap<QString, QString> m_pendingBleIdentities;
    QString m_discoveryId;
    bool m_loadingDevice = false;
    QListWidget *m_deviceSidebar;
    QTabWidget *m_tabs;
    QWidget *m_deviceBox, *m_sharedBox, *m_deskflowBox;
    QLineEdit *m_alias;
    QLabel *m_summary;
    QPushButton *m_connectAll, *m_disconnectAll, *m_addDevice, *m_removeDevice;
    UsbAutoConnect m_autoConnect, m_adbAutoConnect;
    CompanionApp m_companion;
    QString m_companionDeviceId;
    QMap<QString, QString> m_companionStatuses;
    QLineEdit *m_companionApk;
    QWidget *m_companionBox;
    QLabel *m_companionStatus;
    QPushButton *m_companionInstall, *m_companionOpen, *m_companionCheck;
    QPushButton *m_transferFiles;
    QPushButton *m_manualClipboardButton = nullptr;
    PhoneClipboard m_manualClipboard;
    QString m_clipboardDeviceId, m_clipboardDeviceName;
    QPushButton *m_copyScreenshot;
    PhoneScreenshot m_screenshot;
    QString m_screenshotDeviceId, m_screenshotDeviceName;
    QMenu *m_trayScreenshots = nullptr;
    QMap<QString, QAction *> m_screenshotActions;
    QPointer<TransferDialog> m_transferDialog;
    QPointer<WirelessAdbDialog> m_wirelessAdbDialog;
    QMap<QPair<QString, QString>, QVariantMap> m_profiles; // (transport, native serial)
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayDisconnect = nullptr;
    QWidget *m_form;
    QComboBox *m_mouseMode, *m_middleButtonAction, *m_rightButtonAction,
              *m_sideButton1Action, *m_sideButton2Action;
    QCheckBox *m_unlockEnabled, *m_unlockWithSpace, *m_absolutePerAxis;
    QLineEdit *m_unlockPin;
    QCheckBox *m_adbEnabled;
    QLabel *m_adbStatus;
    QPushButton *m_adbTest, *m_adbReadDisplay, *m_adbWirelessSetup;
    QWidget *m_adbBox;
    QString m_transport;
    QLabel *m_transportLabel;
    QComboBox *m_uhidAccess;
    QLineEdit *m_uhidUsbSerial, *m_uhidWirelessTarget;
    QCheckBox *m_bleAutoAttach = nullptr;
    QLineEdit *m_adapter = nullptr;
    QCheckBox *m_bleIsolate = nullptr, *m_bleLegacyHandles = nullptr;
    QCheckBox *m_bleMulti = nullptr;
    bool m_discoverMultipleBle = false;
    QPushButton *m_stopBleService = nullptr;
    QPushButton *m_startBleService = nullptr;
    QLabel *m_bleServiceStatus = nullptr;
    QTimer *m_bleStatusTimer = nullptr;
    quint64 m_bleStatusGeneration = 0;
    bool m_bleStatusPending = false;
    bool m_bleServiceIdle = false;
    bool m_stoppingBleService = false;
    bool m_bleStopQueryPending = false;
    QLineEdit *m_serial, *m_adbSerial, *m_adb, *m_deskflow, *m_deskflowConfig, *m_host, *m_computer, *m_phone;
    QLineEdit *m_tlsCert, *m_tlsFingerprint, *m_tlsFingerprintFile;
    QComboBox *m_tlsKeyLength;
    QPushButton *m_tlsRegenerateCertificate;
    QCheckBox *m_tls;
    QCheckBox *m_audio;
    QSpinBox *m_audioGain;
    QCheckBox *m_clipboard;
    QCheckBox *m_autoRotate;
    QLabel *m_clipboardStatus;
    QSpinBox *m_width, *m_height, *m_port, *m_sideSwipeDistance;
    QDoubleSpinBox *m_sensitivity, *m_motionMultiplier;
    QCheckBox *m_manage;
    QCheckBox *m_autoConnectEnabled, *m_uhidAutoConnectEnabled;
    QPushButton *m_connect, *m_disconnect;
    QLabel *m_status, *m_deviceStatus, *m_connectionStatus;
    QLabel *m_virtualSize;
    QPlainTextEdit *m_log;
    QComboBox *m_logFilter;
    QList<QPair<QString, QString>> m_logHistory;
    QFormLayout *m_deviceForm, *m_displayForm, *m_adbForm, *m_featuresForm, *m_programForm;
    bool m_scanning = false;
    bool m_uiReady = false;
    bool m_monitorsReady = false;
    bool m_sessionStatus = false;
    bool m_closing = false;
    bool m_exitEmitted = false;
    quint64 m_request = 0;
};
