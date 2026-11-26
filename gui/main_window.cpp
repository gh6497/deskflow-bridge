// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "audio_gain.h"
#include "ble_limits.h"
#include "transfer_dialog.h"
#include "process_utils.h"
#include "i18n.h"
#include "build_version.h"
#include "tls_certificate.h"
#include "wireless_adb_dialog.h"

#include <QCheckBox>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QSettings>
#include <QStandardPaths>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QRegularExpressionValidator>
#include <QSplitter>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QListWidget>
#include <QTabWidget>
#include <QUuid>
#include <QWheelEvent>
#include <algorithm>
#include <initializer_list>
#include <iterator>

namespace {

constexpr const char *legacyBleSettingKeys[] = {"bleAdapter", "bleLegacyHandles"};
#ifdef Q_OS_WIN
constexpr const char *bleSetupHint = "otg.ble.windows_setup_hint";
#else
constexpr const char *bleSetupHint = "otg.ble.setup_hint";
#endif

int storedAudioGain(const QVariant &value)
{
    bool ok;
    const int gain = value.toInt(&ok);
    return ok && gain >= 0 && gain <= OTG_AUDIO_GAIN_MAX_DB ? gain : 0;
}

template <typename Widget>
class NoWheelWidget : public Widget {
public:
    using Widget::Widget;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

class AddDeviceDialog : public QDialog {
public:
    using QDialog::QDialog;
    std::function<bool()> canReject;

    void reject() override
    {
        if (!canReject || canReject()) QDialog::reject();
    }
};

QString defaultTlsClientCertificate()
{
    auto directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (directory.isEmpty())
        directory = QDir::homePath() + QStringLiteral("/.config/deskflow-bridge");
    return QDir(directory).filePath(QStringLiteral("tls/client.pem"));
}

bool isWirelessAdbTarget(const QString &target)
{
    return QRegularExpression("^(?:\\[[^\\]]+\\]|[^:]+):[0-9]+$").match(target).hasMatch();
}

void migrateUhidTarget(QVariantMap &profile)
{
    if (profile.value("transport") != "uhid" || profile.contains("uhidAccess")) return;
    const auto target = profile.value("adbSerial").toString().trimmed();
    const bool usb = target.isEmpty() || isUsbAdbSerial(target);
    profile["uhidAccess"] = usb ? "usb" : "wireless";
    profile[usb ? "uhidUsbSerial" : "uhidWirelessTarget"] = target;
    profile.remove("adbSerial");
}

QString profileAdbTarget(const QVariantMap &profile)
{
    if (profile.value("transport") != "uhid") return profile.value("adbSerial").toString().trimmed();
    const bool wireless = profile.value("uhidAccess") == "wireless";
    const auto target = profile.value(wireless ? "uhidWirelessTarget" : "uhidUsbSerial").toString().trimmed();
    if (wireless ? isUsbAdbSerial(target) : !isUsbAdbSerial(target)) return {};
    return target;
}

QString screenKey(const QVariantMap &profile)
{
    return profile.value("phone", "android").toString().trimmed().toCaseFolded();
}

QPair<QString, QString> autoConnectKey(const QVariantMap &profile)
{
    const auto transport = profile.value("transport", "usb").toString();
    if (transport == "usb") return {transport, profile.value("serial").toString()};
    const auto target = profile.value("uhidUsbSerial").toString();
    if (transport == "uhid" && isUsbAdbSerial(target)) return {transport, target};
    return {};
}

bool autoConnectOptedIn(const QVariantMap &profile)
{
    const auto transport = profile.value("transport", "usb").toString();
    if (transport == "uhid") return profile.value("uhidUsbAutoConnect").toBool();
    return transport == "usb" && profile.value("autoConnect").toBool();
}

QString defaultTransport()
{
#if defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_UHID)
    return QStringLiteral("uhid");
#else
    return QStringLiteral("usb");
#endif
}

const char *transportLabelId(const QString &transport)
{
    if (transport == "ble") {
#ifdef Q_OS_WIN
        return "otg.transport.ble_windows";
#else
        return "otg.transport.ble";
#endif
    }
    if (transport == "uhid") {
#ifdef Q_OS_WIN
        return "otg.transport.uhid_windows";
#else
        return "otg.transport.uhid";
#endif
    }
    return "otg.transport.usb";
}

bool supportsTransport(const QString &transport)
{
    if (transport == "usb") return true;
#ifdef OTG_GUI_HAVE_UHID
    if (transport == "uhid") return true;
#endif
#if defined(OTG_GUI_HAVE_BLE) || defined(OTG_GUI_HAVE_BLE_WIN)
    if (transport == "ble") return true;
#endif
    return false;
}

QString deviceAdbTarget(const QString &transport, const QString &explicitTarget, const QString &usbSerial)
{
    if (!explicitTarget.isEmpty()) return explicitTarget;
#ifndef Q_OS_WIN
    if (transport == "usb") return usbSerial;
#else
    Q_UNUSED(transport);
    Q_UNUSED(usbSerial);
#endif
    return {};
}

QString actionDeviceName(const QVariantMap &profile)
{
    const auto screen = profile.value("phone").toString();
    const auto alias = profile.value("alias").toString();
    return alias.isEmpty() || alias == screen ? screen : QStringLiteral("%1 (%2)").arg(alias, screen);
}

QString bleScreenName(const QString &identity)
{
    return identity.trimmed().toUpper().replace(':', '-');
}

} // namespace

void MainWindow::setTranslatedProperty(QObject *object, const char *property, const char *id)
{
    for (auto &binding : m_textBindings) {
        if (binding.object == object && binding.property == property) {
            binding.id = id;
            object->setProperty(property, qtTrId(id));
            return;
        }
    }
    m_textBindings.append({object, property, id});
    object->setProperty(property, qtTrId(id));
}

void MainWindow::addTranslatedRow(QFormLayout *form, const char *id, QWidget *field)
{
    form->addRow(translated(new QLabel, "text", id), field);
}

void MainWindow::setProfileTransport(const QString &transport)
{
    m_transport = transport;
    m_transportLabel->setText(transport.isEmpty() ? QString() : qtTrId(transportLabelId(transport)));
}

void MainWindow::retranslateUi()
{
    for (const auto &binding : m_textBindings) {
        if (binding.object)
            binding.object->setProperty(binding.property.constData(), qtTrId(binding.id.constData()));
    }
    m_mouseMode->setItemText(0, qtTrId("otg.mouse.absolute"));
    m_mouseMode->setItemText(1, qtTrId("otg.mouse.relative"));
    m_middleButtonAction->setItemText(0, qtTrId("otg.middle_button.normal"));
    m_middleButtonAction->setItemText(1, qtTrId("otg.middle_button.home"));
    m_rightButtonAction->setItemText(0, qtTrId("otg.right_button.normal"));
    m_rightButtonAction->setItemText(1, qtTrId("otg.right_button.back"));
    m_sideButton1Action->setItemText(0, qtTrId("otg.side_button.normal"));
    m_sideButton1Action->setItemText(1, qtTrId("otg.side_button.swipe_up"));
    m_sideButton1Action->setItemText(2, qtTrId("otg.side_button.swipe_down"));
    m_sideButton2Action->setItemText(0, qtTrId("otg.side_button.normal"));
    m_sideButton2Action->setItemText(1, qtTrId("otg.side_button.swipe_up"));
    m_sideButton2Action->setItemText(2, qtTrId("otg.side_button.swipe_down"));
    m_uhidAccess->setItemText(0, qtTrId("otg.uhid.usb_access"));
    m_uhidAccess->setItemText(1, qtTrId("otg.uhid.wireless_access"));
    if (m_uhidUsbSerial->isReadOnly()) m_uhidUsbSerial->setToolTip(qtTrId("otg.uhid.usb_serial_locked"));
    setProfileTransport(m_transport);
    QSignalBlocker languageBlocker(m_language);
    m_language->setItemText(0, qtTrId("otg.language.chinese"));
    m_language->setItemText(1, qtTrId("otg.language.english"));
    if (m_sessionStatus)
        m_status->setText(m_connections.status(m_selectedId));
    updateConnectionStatus();
    if (m_tabs) {
        m_tabs->setTabText(0, qtTrId("otg.multi.device_tab"));
        m_tabs->setTabText(1, qtTrId("otg.multi.server_tab"));
        updateDeviceList();
    }
    updateVirtualSize();
    if (m_session) m_clipboardStatus->setText(m_session->clipboardStatusMessage());
    updateCompanionStatus();
    updateTray();
}

bool MainWindow::trayAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QString MainWindow::bridgeExecutable() const
{
    return bridgePath();
}

void MainWindow::setupTray()
{
    const QIcon icon(":/icons/deskflow-bridge.svg");
    setWindowIcon(icon);
    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setObjectName("systemTray");
    auto *menu = new QMenu(this);
    auto *restore = translated(menu->addAction(QString()), "text", "otg.tray.show");
    restore->setObjectName("trayShow");
    connect(restore, &QAction::triggered, this, &MainWindow::restoreWindow);
    m_trayScreenshots = translated(menu->addMenu(QString()), "title", "otg.screenshot.copy");
    m_trayScreenshots->setObjectName("trayScreenshots");
    m_trayDisconnect = translated(menu->addAction(QString()), "text", "otg.action.disconnect");
    m_trayDisconnect->setObjectName("trayDisconnect");
    setTranslatedProperty(m_trayDisconnect, "text", "otg.multi.disconnect_all");
    connect(m_trayDisconnect, &QAction::triggered, &m_connections, &ConnectionManager::stopAll);
    menu->addSeparator();
    auto *quit = translated(menu->addAction(QString()), "text", "otg.tray.quit");
    quit->setObjectName("trayQuit");
    connect(quit, &QAction::triggered, this, &MainWindow::requestExit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            restoreWindow();
    });
    m_tray->show();
}

void MainWindow::restoreWindow()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::requestExit()
{
    m_closing = true;
    close();
}

void MainWindow::updateTray()
{
    if (!m_tray)
        return;
    m_tray->setToolTip(QString("Deskflow Bridge · %1\n%2").arg(m_summary->text(), m_connections.status(m_selectedId)));
    m_trayDisconnect->setEnabled(m_connections.busy());
    updateScreenshotActions();
}

void MainWindow::updateScreenshotActions()
{
    const bool available = !m_closing && !m_screenshot.busy() && !m_adb->text().trimmed().isEmpty();
    m_copyScreenshot->setEnabled(false);
    bool any = false;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto id = it.key();
        const auto profile = id == m_selectedId ? deviceProfile() : it.value();
        const bool enabled = available && !adbActionTarget(id, profile).isEmpty();
        if (id == m_selectedId) m_copyScreenshot->setEnabled(enabled);
        auto *&action = m_screenshotActions[id];
        if (!action) {
            action = m_trayScreenshots->addAction(QString());
            action->setData(id);
            connect(action, &QAction::triggered, this, [this, id] { copyScreenshot(id); });
        }
        action->setText(actionDeviceName(profile).replace('&', "&&"));
        action->setEnabled(enabled);
        any |= enabled;
    }
    for (auto it = m_screenshotActions.begin(); it != m_screenshotActions.end();) {
        if (m_records.contains(it.key())) { ++it; continue; }
        m_trayScreenshots->removeAction(it.value());
        it.value()->deleteLater();
        it = m_screenshotActions.erase(it);
    }
    m_trayScreenshots->menuAction()->setEnabled(any);
}

void MainWindow::copyScreenshot(const QString &id)
{
    if (m_closing || m_screenshot.busy() || !m_records.contains(id)) return;
    const auto profile = id == m_selectedId ? deviceProfile() : m_records.value(id);
    // Snapshot identity at click time. A later selection (or tray capture of
    // another phone) must not switch either the ADB target or the result label.
    m_screenshotDeviceId = id;
    m_screenshotDeviceName = actionDeviceName(profile);
    if (m_screenshot.start(m_adb->text().trimmed(), adbActionTarget(id, profile))) {
        if (m_screenshot.busy()) statusBar()->showMessage(qtTrId("otg.screenshot.working").arg(m_screenshotDeviceName));
        updateScreenshotActions();
    }
}

void MainWindow::updateClipboardAction()
{
    if (m_manualClipboardButton)
        m_manualClipboardButton->setEnabled(!m_closing && !m_manualClipboard.busy() &&
            !m_adb->text().trimmed().isEmpty() && !adbActionTarget(m_selectedId, deviceProfile()).isEmpty());
}

void MainWindow::transferClipboard(PhoneClipboard::Direction direction)
{
    if (m_closing || m_manualClipboard.busy()) return;
    const auto profile = deviceProfile();
    m_clipboardDeviceId = m_selectedId;
    m_clipboardDeviceName = actionDeviceName(profile);
    m_manualClipboard.start(m_adb->text().trimmed(), adbActionTarget(m_selectedId, profile),
                            findScrcpyServer(), findScrcpyVersion(), direction);
    if (m_manualClipboard.busy())
        statusBar()->showMessage(qtTrId("otg.manual_clipboard.working").arg(m_clipboardDeviceName));
    updateClipboardAction();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_uiReady)
        retranslateUi();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_bleServiceStatus && m_bleStatusTimer) {
        if (event->type() == QEvent::Show) {
            m_bleStatusTimer->start();
            QTimer::singleShot(0, this, &MainWindow::refreshBleServiceStatus);
        } else if (event->type() == QEvent::Hide) {
            m_bleStatusTimer->stop();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QWidget *MainWindow::pathField(QLineEdit *&field, const char *placeholderId, const char *dialogId)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit;
    setTranslatedProperty(field, "placeholderText", placeholderId);
    auto *browse = translated(new QPushButton, "text", "otg.action.browse");
    layout->addWidget(field, 1);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this, field, dialogId] {
        const auto title = qtTrId(dialogId ? dialogId : "otg.dialog.select_executable");
        const auto path = QFileDialog::getOpenFileName(this, title, field->text());
        if (!path.isEmpty())
            field->setText(path);
    });
    return row;
}

MainWindow::MainWindow() : m_connections(this), m_autoConnect(this),
    m_adbAutoConnect(this, UsbAutoConnect::Source::Adb), m_companion(this)
{
    QSettings settings;
    const bool hasDeviceList = settings.contains("devices/size");
    const int deviceCount = settings.beginReadArray("devices");
    if (deviceCount > 0) {
        settings.setArrayIndex(0);
        m_selectedId = settings.value("id", "initial").toString();
        if (m_selectedId.isEmpty()) m_selectedId = "initial";
    }
    settings.endArray();
    if (!hasDeviceList && m_selectedId.isEmpty() &&
        (settings.contains("transport") || settings.contains("serial") ||
         settings.contains("adbSerial") || settings.contains("phone")))
        m_selectedId = "initial";
    if (!m_selectedId.isEmpty()) m_session = m_connections.addDevice(m_selectedId);
    m_autoConnect.setMultipleDevices(true);
    m_adbAutoConnect.setMultipleDevices(true);
    auto &languages = LanguageManager::instance();
    const QString savedLanguage = settings.value("language", LanguageManager::systemLanguage()).toString();
    if (!languages.setLanguage(savedLanguage))
        languages.setLanguage(LanguageManager::systemLanguage());
    setTranslatedProperty(this, "windowTitle", "otg.window.title");
    resize(1000, 820);
    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(20, 16, 20, 16);
    auto *heading = new QLabel("Deskflow Bridge");
    QFont titleFont = heading->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    heading->setFont(titleFont);
    auto *headingRow = new QHBoxLayout;
    headingRow->addWidget(heading, 1);
    headingRow->addWidget(translated(new QLabel, "text", "otg.label.language"));
    m_language = new NoWheelWidget<QComboBox>;
    m_language->setObjectName("languageSelector");
    m_language->addItem(qtTrId("otg.language.chinese"), "zh_CN");
    m_language->addItem(qtTrId("otg.language.english"), "en");
    m_language->setCurrentIndex(m_language->findData(languages.language()));
    headingRow->addWidget(m_language);
    auto *about = translated(new QPushButton, "text", "otg.action.about");
    about->setObjectName("aboutButton");
    connect(about, &QPushButton::clicked, this, [this] {
        QMessageBox dialog(QMessageBox::Information, qtTrId("otg.about.title"),
                           qtTrId("otg.about.text").arg(OTG_VERSION, OTG_COMMIT),
                           QMessageBox::Ok, this);
        auto *github = dialog.addButton(qtTrId("otg.about.github"), QMessageBox::ActionRole);
        connect(github, &QAbstractButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/gh6497/deskflow-bridge")));
        });
        dialog.exec();
    });
    headingRow->addWidget(about);
    root->addLayout(headingRow);
    auto *intro = translated(new QLabel, "text", "otg.window.intro");
    root->addWidget(intro);

    m_summary = new QLabel;
    m_summary->setObjectName("connectionSummary");
    root->addWidget(m_summary);
    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 8, 0);
    m_deviceSidebar = new QListWidget;
    m_deviceSidebar->setObjectName("deviceSidebar");
    m_deviceSidebar->setMinimumWidth(180);
    sidebarLayout->addWidget(m_deviceSidebar, 1);
    m_addDevice = translated(new QPushButton, "text", "otg.multi.add_device");
    m_addDevice->setObjectName("addDeviceButton");
    sidebarLayout->addWidget(m_addDevice);
    m_removeDevice = translated(new QPushButton, "text", "otg.multi.remove_device");
    m_removeDevice->setObjectName("removeDeviceButton");
    sidebarLayout->addWidget(m_removeDevice);
    m_connectAll = translated(new QPushButton, "text", "otg.multi.connect_all");
    m_connectAll->setObjectName("connectAllButton");
    m_disconnectAll = translated(new QPushButton, "text", "otg.multi.disconnect_all");
    m_disconnectAll->setObjectName("disconnectAllButton");
    sidebarLayout->addWidget(m_connectAll);
    sidebarLayout->addWidget(m_disconnectAll);
    splitter->addWidget(sidebar);
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget;
    m_tabs->setObjectName("deviceTabs");
    rightLayout->addWidget(m_tabs, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 3);

    m_form = new QWidget;
    auto *formLayout = new QVBoxLayout(m_form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    auto *deviceBox = translated(new QGroupBox, "title", "otg.section.device");
    m_deviceBox = deviceBox;
    auto *deviceForm = new QFormLayout(deviceBox);
    m_deviceForm = deviceForm;
    deviceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_connectionStatus = new QLabel;
    m_connectionStatus->setObjectName("deviceConnectionStatus");
    m_connectionStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    deviceForm->addRow(translated(new QLabel, "text", "otg.label.connection_status"), m_connectionStatus);
    m_alias = new QLineEdit;
    m_alias->setObjectName("deviceAlias");
    addTranslatedRow(deviceForm, "otg.multi.alias", m_alias);
    m_transportLabel = new QLabel;
    m_transportLabel->setObjectName("deviceTransport");
    addTranslatedRow(deviceForm, "otg.label.transport", m_transportLabel);
    m_uhidAccess = new NoWheelWidget<QComboBox>;
    m_uhidAccess->setObjectName("uhidAccessSelector");
    m_uhidAccess->addItem(qtTrId("otg.uhid.usb_access"), "usb");
    m_uhidAccess->addItem(qtTrId("otg.uhid.wireless_access"), "wireless");
    addTranslatedRow(deviceForm, "otg.uhid.access_label", m_uhidAccess);
    m_bleAutoAttach = translated(new QCheckBox, "text", "otg.ble.auto_attach");
    m_bleAutoAttach->setObjectName("bleAutoAttach");
    m_bleAutoAttach->setChecked(true);
    setTranslatedProperty(m_bleAutoAttach, "toolTip", "otg.ble.auto_attach_hint");
    deviceForm->addRow(m_bleAutoAttach);
    m_serial = new QLineEdit;
    m_serial->setObjectName("usbSerial");
    addTranslatedRow(deviceForm, "otg.label.usb_serial", m_serial);
    m_adbSerial = new QLineEdit;
    m_adbSerial->setObjectName("adbSerial");
#ifdef Q_OS_WIN
    setTranslatedProperty(m_adbSerial, "placeholderText", "otg.adb_serial.windows_placeholder");
#else
    setTranslatedProperty(m_adbSerial, "placeholderText", "otg.adb_serial.placeholder");
#endif
    setTranslatedProperty(m_adbSerial, "toolTip", "otg.adb_serial.tooltip");
    m_deviceStatus = new QLabel;
    m_deviceStatus->setWordWrap(true);
    deviceForm->addRow(m_deviceStatus);
    m_autoConnectEnabled = translated(new QCheckBox, "text", "otg.auto_connect.enabled");
    m_autoConnectEnabled->setObjectName("autoConnectEnabled");
    setTranslatedProperty(m_autoConnectEnabled, "toolTip", "otg.auto_connect.tooltip");
    deviceForm->addRow(m_autoConnectEnabled);
    m_uhidAutoConnectEnabled = translated(new QCheckBox, "text", "otg.auto_connect.enabled");
    m_uhidAutoConnectEnabled->setObjectName("uhidUsbAutoConnect");
    setTranslatedProperty(m_uhidAutoConnectEnabled, "toolTip", "otg.uhid.auto_connect_hint");
    deviceForm->addRow(m_uhidAutoConnectEnabled);
    formLayout->addWidget(deviceBox);

    auto *adbBox = translated(new QGroupBox, "title", "otg.section.adb");
    m_adbBox = adbBox;
    auto *adbForm = new QFormLayout(adbBox);
    m_adbForm = adbForm;
    adbForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_adbEnabled = translated(new QCheckBox, "text", "otg.adb.enabled");
    m_adbEnabled->setObjectName("adbEnabled");
    setTranslatedProperty(m_adbEnabled, "toolTip", "otg.adb.enabled.tooltip");
    adbForm->addRow(m_adbEnabled);
    auto *adbTargetLabel = translated(new QLabel, "text", "otg.label.adb_serial");
#ifdef Q_OS_WIN
    setTranslatedProperty(adbTargetLabel, "text", "otg.label.adb_wireless");
#endif
    adbForm->addRow(adbTargetLabel, m_adbSerial);
    m_uhidUsbSerial = new QLineEdit;
    m_uhidUsbSerial->setObjectName("uhidUsbSerial");
    addTranslatedRow(adbForm, "otg.uhid.usb_serial", m_uhidUsbSerial);
    m_uhidWirelessTarget = translated(new QLineEdit, "placeholderText", "otg.uhid.wireless_hint");
    m_uhidWirelessTarget->setObjectName("uhidWirelessTarget");
    addTranslatedRow(adbForm, "otg.uhid.wireless_target", m_uhidWirelessTarget);
    auto *adbActions = new QWidget;
    auto *adbActionsLayout = new QHBoxLayout(adbActions);
    adbActionsLayout->setContentsMargins(0, 0, 0, 0);
    m_adbTest = translated(new QPushButton, "text", "otg.adb.test");
    m_adbTest->setObjectName("adbTest");
    m_adbReadDisplay = translated(new QPushButton, "text", "otg.adb.read_display");
    m_adbReadDisplay->setObjectName("adbReadDisplay");
    m_adbWirelessSetup = translated(new QPushButton, "text", "otg.adb.wireless_setup");
    m_adbWirelessSetup->setObjectName("adbWirelessSetup");
    adbActionsLayout->addWidget(m_adbWirelessSetup);
    adbActionsLayout->addWidget(m_adbTest);
    adbActionsLayout->addWidget(m_adbReadDisplay);
    adbActionsLayout->addStretch();
    adbForm->addRow(adbActions);
    m_adbStatus = new QLabel;
    m_adbStatus->setObjectName("adbStatus");
    m_adbStatus->setWordWrap(true);
    adbForm->addRow(m_adbStatus);

    m_audio = translated(new QCheckBox, "text", "otg.audio.enabled");
    m_audio->setObjectName("audioEnabled");
    setTranslatedProperty(m_audio, "toolTip", "otg.audio.tooltip");
    adbForm->addRow(m_audio);

    m_audioGain = new NoWheelWidget<QSpinBox>;
    m_audioGain->setObjectName("audioGainDb");
    m_audioGain->setRange(0, OTG_AUDIO_GAIN_MAX_DB);
    m_audioGain->setSuffix(" dB");
    m_audioGain->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setTranslatedProperty(m_audioGain, "toolTip", "otg.audio.gain_tooltip");
    addTranslatedRow(adbForm, "otg.audio.gain", m_audioGain);

    m_clipboard = translated(new QCheckBox, "text", "otg.clipboard.enabled");
    m_clipboard->setObjectName("clipboardEnabled");
    setTranslatedProperty(m_clipboard, "toolTip", "otg.clipboard.tooltip");
    adbForm->addRow(m_clipboard);

    m_clipboardStatus = new QLabel;
    m_clipboardStatus->setObjectName("clipboardStatus");
    m_clipboardStatus->setWordWrap(true);
    adbForm->addRow(m_clipboardStatus);

    m_autoRotate = translated(new QCheckBox, "text", "otg.display.auto_rotate");
    m_autoRotate->setObjectName("autoRotate");
    setTranslatedProperty(m_autoRotate, "toolTip", "otg.display.auto_rotate.tooltip");
    adbForm->addRow(m_autoRotate);

    m_unlockEnabled = translated(new QCheckBox, "text", "otg.unlock.enabled");
    m_unlockEnabled->setObjectName("unlockEnabled");
    setTranslatedProperty(m_unlockEnabled, "toolTip", "otg.unlock.tooltip");
    adbForm->addRow(m_unlockEnabled);
    m_unlockPin = new QLineEdit;
    m_unlockPin->setObjectName("unlockPin");
    m_unlockPin->setEchoMode(QLineEdit::Password);
    m_unlockPin->setMaxLength(32);
    m_unlockPin->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,32}"), m_unlockPin));
    setTranslatedProperty(m_unlockPin, "placeholderText", "otg.unlock.pin_hint");
    setTranslatedProperty(m_unlockPin, "toolTip", "otg.unlock.tooltip");
    addTranslatedRow(adbForm, "otg.label.unlock_pin", m_unlockPin);
    m_unlockWithSpace = translated(new QCheckBox, "text", "otg.unlock.with_space");
    m_unlockWithSpace->setObjectName("unlockWithSpace");
    m_unlockWithSpace->setChecked(true);
    adbForm->addRow(m_unlockWithSpace);
    auto *unlockHint = translated(new QLabel, "text", "otg.unlock.adb_hint");
    unlockHint->setWordWrap(true);
    adbForm->addRow(unlockHint);
    connect(m_unlockEnabled, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    formLayout->addWidget(adbBox);

    auto *displayBox = translated(new QGroupBox, "title", "otg.section.display");
    auto *displayForm = new QFormLayout(displayBox);
    m_displayForm = displayForm;
    displayForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *sizeRow = new QWidget;
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    m_width = new NoWheelWidget<QSpinBox>;
    m_height = new NoWheelWidget<QSpinBox>;
    m_width->setObjectName("screenWidth");
    m_height->setObjectName("screenHeight");
    m_width->setRange(1, 32767);
    m_height->setRange(1, 32767);
    sizeLayout->addWidget(m_width);
    sizeLayout->addWidget(new QLabel("×"));
    sizeLayout->addWidget(m_height);
    auto *swap = translated(new QPushButton, "text", "otg.action.swap_dimensions");
    swap->setObjectName("swapDimensions");
    sizeLayout->addWidget(swap);
    sizeLayout->addStretch();
    connect(swap, &QPushButton::clicked, this, [this] {
        const int width = m_width->value();
        m_width->setValue(m_height->value());
        m_height->setValue(width);
    });
    addTranslatedRow(displayForm, "otg.label.original_size", sizeRow);

    m_sensitivity = new NoWheelWidget<QDoubleSpinBox>;
    m_sensitivity->setObjectName("sensitivityMultiplier");
    m_sensitivity->setRange(1.0, 10.0);
    m_sensitivity->setDecimals(2);
    m_sensitivity->setSingleStep(0.25);
    m_sensitivity->setSuffix(" ×");
    setTranslatedProperty(m_sensitivity, "toolTip", "otg.sensitivity.tooltip");
    addTranslatedRow(displayForm, "otg.label.sensitivity", m_sensitivity);

    m_motionMultiplier = new NoWheelWidget<QDoubleSpinBox>;
    m_motionMultiplier->setObjectName("motionMultiplier");
    m_motionMultiplier->setRange(1.0, 10.0);
    m_motionMultiplier->setDecimals(2);
    m_motionMultiplier->setSingleStep(0.25);
    m_motionMultiplier->setSuffix(" ×");
    setTranslatedProperty(m_motionMultiplier, "toolTip", "otg.motion_multiplier.tooltip");
    addTranslatedRow(displayForm, "otg.label.motion_multiplier", m_motionMultiplier);

    m_virtualSize = new QLabel;
    m_virtualSize->setObjectName("virtualScreenSize");
    m_virtualSize->setWordWrap(true);
    addTranslatedRow(displayForm, "otg.label.virtual_size", m_virtualSize);

    m_phone = new QLineEdit;
    m_phone->setObjectName("screenName");
    addTranslatedRow(displayForm, "otg.label.phone_name", m_phone);
    formLayout->addWidget(displayBox);

    auto *featuresBox = translated(new QGroupBox, "title", "otg.section.features");
    auto *featuresForm = new QFormLayout(featuresBox);
    m_featuresForm = featuresForm;
    featuresForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_mouseMode = new NoWheelWidget<QComboBox>;
    m_mouseMode->setObjectName("mouseMode");
    m_mouseMode->addItem(qtTrId("otg.mouse.absolute"), "absolute");
    m_mouseMode->addItem(qtTrId("otg.mouse.relative"), "relative");
    addTranslatedRow(featuresForm, "otg.label.mouse_mode", m_mouseMode);

    m_middleButtonAction = new NoWheelWidget<QComboBox>;
    m_middleButtonAction->setObjectName("middleButtonAction");
    m_middleButtonAction->addItem(qtTrId("otg.middle_button.normal"), "normal");
    m_middleButtonAction->addItem(qtTrId("otg.middle_button.home"), "home");
    setTranslatedProperty(m_middleButtonAction, "toolTip", "otg.middle_button.tooltip");
    addTranslatedRow(featuresForm, "otg.label.middle_button_action", m_middleButtonAction);

    m_rightButtonAction = new NoWheelWidget<QComboBox>;
    m_rightButtonAction->setObjectName("rightButtonAction");
    m_rightButtonAction->addItem(qtTrId("otg.right_button.normal"), "normal");
    m_rightButtonAction->addItem(qtTrId("otg.right_button.back"), "back");
    setTranslatedProperty(m_rightButtonAction, "toolTip", "otg.right_button.tooltip");
    addTranslatedRow(featuresForm, "otg.label.right_button_action", m_rightButtonAction);

    m_sideButton1Action = new NoWheelWidget<QComboBox>;
    m_sideButton1Action->setObjectName("sideButton1Action");
    m_sideButton1Action->addItem(qtTrId("otg.side_button.normal"), "normal");
    m_sideButton1Action->addItem(qtTrId("otg.side_button.swipe_up"), "swipe-up");
    m_sideButton1Action->addItem(qtTrId("otg.side_button.swipe_down"), "swipe-down");
    setTranslatedProperty(m_sideButton1Action, "toolTip", "otg.side_button.tooltip");
    addTranslatedRow(featuresForm, "otg.label.side_button_1_action", m_sideButton1Action);

    m_sideButton2Action = new NoWheelWidget<QComboBox>;
    m_sideButton2Action->setObjectName("sideButton2Action");
    m_sideButton2Action->addItem(qtTrId("otg.side_button.normal"), "normal");
    m_sideButton2Action->addItem(qtTrId("otg.side_button.swipe_up"), "swipe-up");
    m_sideButton2Action->addItem(qtTrId("otg.side_button.swipe_down"), "swipe-down");
    setTranslatedProperty(m_sideButton2Action, "toolTip", "otg.side_button.tooltip");
    addTranslatedRow(featuresForm, "otg.label.side_button_2_action", m_sideButton2Action);

    m_sideSwipeDistance = new NoWheelWidget<QSpinBox>;
    m_sideSwipeDistance->setObjectName("sideSwipeDistance");
    m_sideSwipeDistance->setRange(1, 100);
    m_sideSwipeDistance->setValue(17);
    m_sideSwipeDistance->setSuffix(QStringLiteral(" %"));
    setTranslatedProperty(m_sideSwipeDistance, "toolTip", "otg.side_swipe_distance.tooltip");
    addTranslatedRow(featuresForm, "otg.label.side_swipe_distance", m_sideSwipeDistance);

    m_absolutePerAxis = translated(new QCheckBox, "text", "otg.mouse.per_axis");
    m_absolutePerAxis->setObjectName("absolutePerAxis");
    setTranslatedProperty(m_absolutePerAxis, "toolTip", "otg.mouse.per_axis_hint");
    featuresForm->addRow(m_absolutePerAxis);

    formLayout->addWidget(featuresBox);

    auto *companionBox = translated(new QGroupBox, "title", "otg.companion.title");
    m_companionBox = companionBox;
    auto *companionLayout = new QVBoxLayout(companionBox);
    auto *companionHint = translated(new QLabel, "text", "otg.companion.hint");
    companionHint->setWordWrap(true);
    companionLayout->addWidget(companionHint);
    auto *apkRow = new QWidget;
    auto *apkLayout = new QHBoxLayout(apkRow);
    apkLayout->setContentsMargins(0, 0, 0, 0);
    m_companionApk = new QLineEdit;
    m_companionApk->setObjectName("companionApk");
    setTranslatedProperty(m_companionApk, "placeholderText", "otg.companion.apk_hint");
    m_companionApk->setText(CompanionApp::preferredApk(settings.value("companionApk").toString()));
    auto *browseApk = translated(new QPushButton, "text", "otg.action.browse");
    apkLayout->addWidget(m_companionApk, 1);
    apkLayout->addWidget(browseApk);
    connect(browseApk, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, qtTrId("otg.companion.select_apk"),
                                                       m_companionApk->text(), "APK (*.apk)");
        if (!path.isEmpty()) m_companionApk->setText(path);
    });
    companionLayout->addWidget(apkRow);
    auto *companionButtons = new QHBoxLayout;
    m_companionInstall = translated(new QPushButton, "text", "otg.companion.install");
    m_companionOpen = translated(new QPushButton, "text", "otg.companion.open");
    m_companionCheck = translated(new QPushButton, "text", "otg.companion.check");
    m_companionInstall->setObjectName("companionInstall");
    m_companionOpen->setObjectName("companionOpen");
    m_companionCheck->setObjectName("companionCheck");
    companionButtons->addWidget(m_companionInstall);
    companionButtons->addWidget(m_companionOpen);
    companionButtons->addWidget(m_companionCheck);
    companionLayout->addLayout(companionButtons);
    m_companionStatus = new QLabel;
    m_companionStatus->setObjectName("companionStatus");
    m_companionStatus->setWordWrap(true);
    companionLayout->addWidget(m_companionStatus);
    formLayout->addWidget(companionBox);
    connect(m_companionInstall, &QPushButton::clicked, this, [this] { setupCompanion(CompanionApp::Action::Install); });
    connect(m_companionOpen, &QPushButton::clicked, this, [this] { setupCompanion(CompanionApp::Action::Open); });
    connect(m_companionCheck, &QPushButton::clicked, this, [this] { setupCompanion(CompanionApp::Action::Check); });
    connect(&m_companion, &CompanionApp::completed, this, [this](const QString &id, const QString &detail) {
        m_companionStatuses[m_companionDeviceId] = id;
        appendLog(qtTrId(id.toLatin1().constData()) + (detail.isEmpty() ? QString() : "\n" + detail), m_companionDeviceId);
        updateEnabled();
        if (m_closing) QTimer::singleShot(0, this, &QWidget::close);
    });
    auto *serverBox = translated(new QGroupBox, "title", "otg.section.connection");
    m_deskflowBox = serverBox;
    auto *serverForm = new QFormLayout(serverBox);
    m_manage = translated(new QCheckBox, "text", "otg.server.manage");
    m_manage->setObjectName("manageServer");
    serverForm->addRow(m_manage);
    addTranslatedRow(serverForm, "otg.server.config_file",
                     pathField(m_deskflowConfig, "otg.server.config_hint", "otg.server.config_select"));
    m_deskflowConfig->setObjectName("deskflowConfigFile");
    m_computer = new QLineEdit;
    m_host = new QLineEdit;
    m_port = new NoWheelWidget<QSpinBox>;
    m_port->setObjectName("serverPort");
    m_port->setRange(1, 65535);
    addTranslatedRow(serverForm, "otg.label.computer_name", m_computer);
    addTranslatedRow(serverForm, "otg.label.server_address", m_host);
    addTranslatedRow(serverForm, "otg.label.server_port", m_port);
    m_tls = translated(new QCheckBox, "text", "otg.tls.enabled");
    m_tls->setObjectName("tlsEnabled");
    serverForm->addRow(m_tls);
    auto *clientCertificateRow = new QWidget;
    auto *clientCertificateLayout = new QHBoxLayout(clientCertificateRow);
    clientCertificateLayout->setContentsMargins(0, 0, 0, 0);
    m_tlsCert = new QLineEdit;
    m_tlsCert->setObjectName("tlsCertificate");
    setTranslatedProperty(m_tlsCert, "placeholderText", "otg.tls.certificate_hint");
    auto *browseClientCertificate = translated(new QPushButton, "text", "otg.action.browse");
    browseClientCertificate->setObjectName("tlsBrowseCertificate");
    clientCertificateLayout->addWidget(m_tlsCert, 1);
    clientCertificateLayout->addWidget(browseClientCertificate);
    addTranslatedRow(serverForm, "otg.tls.certificate", clientCertificateRow);
    connect(browseClientCertificate, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, qtTrId("otg.tls.select_certificate"),
                                                       m_tlsCert->text(),
                                                       QStringLiteral("PEM (*.pem);;All files (*)"), nullptr,
                                                       QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty())
            m_tlsCert->setText(path);
    });
    auto *tlsKeyRow = new QWidget;
    auto *tlsKeyLayout = new QHBoxLayout(tlsKeyRow);
    tlsKeyLayout->setContentsMargins(0, 0, 0, 0);
    m_tlsKeyLength = new NoWheelWidget<QComboBox>;
    m_tlsKeyLength->setObjectName("tlsKeyLength");
    m_tlsKeyLength->addItem(QStringLiteral("2048"), 2048);
    m_tlsKeyLength->addItem(QStringLiteral("4096"), 4096);
    m_tlsRegenerateCertificate = translated(new QPushButton, "text", "otg.tls.regenerate");
    m_tlsRegenerateCertificate->setObjectName("tlsRegenerateCertificate");
    tlsKeyLayout->addWidget(m_tlsKeyLength);
    tlsKeyLayout->addWidget(m_tlsRegenerateCertificate);
    tlsKeyLayout->addStretch();
    addTranslatedRow(serverForm, "otg.tls.key_length", tlsKeyRow);
    m_tlsFingerprint = new QLineEdit;
    m_tlsFingerprint->setObjectName("tlsFingerprint");
    setTranslatedProperty(m_tlsFingerprint, "placeholderText", "otg.tls.fingerprint_hint");
    addTranslatedRow(serverForm, "otg.tls.fingerprint", m_tlsFingerprint);
    m_tlsFingerprintFile = new QLineEdit;
    m_tlsFingerprintFile->setObjectName("tlsFingerprintFile");
    setTranslatedProperty(m_tlsFingerprintFile, "placeholderText", "otg.tls.fingerprint_file_hint");
    addTranslatedRow(serverForm, "otg.tls.fingerprint_file", m_tlsFingerprintFile);
    connect(m_tls, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_tlsRegenerateCertificate, &QPushButton::clicked, this, &MainWindow::regenerateTlsCertificate);
    m_sharedBox = new QWidget;
    auto *sharedLayout = new QVBoxLayout(m_sharedBox);
    sharedLayout->addWidget(serverBox);

    auto *advancedBox = translated(new QGroupBox, "title", "otg.section.programs");
    auto *advancedForm = new QFormLayout(advancedBox);
    m_programForm = advancedForm;
    advancedForm->addRow("ADB", pathField(m_adb, "otg.path.adb_hint"));
    m_adb->setObjectName("adbExecutable");
    advancedForm->addRow("Deskflow", pathField(m_deskflow, "otg.path.deskflow_hint"));
    m_deskflow->setObjectName("deskflowExecutable");
#if (defined(Q_OS_LINUX) && defined(OTG_GUI_HAVE_BLE)) || (defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN))
    auto *bleBox = translated(new QGroupBox, "title", "otg.ble.shared_title");
    bleBox->setObjectName("sharedBluetoothService");
    auto *bleForm = new QFormLayout(bleBox);
#ifdef Q_OS_LINUX
    m_adapter = new QLineEdit;
    m_adapter->setObjectName("bleAdapter");
    addTranslatedRow(bleForm, "otg.ble.adapter", m_adapter);
    m_bleLegacyHandles = translated(new QCheckBox, "text", "otg.ble.legacy_handles");
    m_bleLegacyHandles->setObjectName("bleLegacyHandles");
    setTranslatedProperty(m_bleLegacyHandles, "toolTip", "otg.ble.legacy_handles_hint");
    bleForm->addRow(m_bleLegacyHandles);
#else
    bleForm->addRow(translated(new QLabel, "text", "otg.ble.default_adapter"));
#endif
#ifdef Q_OS_LINUX
    // Linux always uses the shared directed service. The multi-phone choice
    // remains a Windows provider option because Windows has a separate
    // provider lifetime.
#else
    m_bleMulti = translated(new QCheckBox, "text", "otg.ble.multi_mode");
    m_bleMulti->setObjectName("bleMultiDevice");
    setTranslatedProperty(m_bleMulti, "toolTip", "otg.ble.windows_multi_hint");
    bleForm->addRow(m_bleMulti);
#endif
    m_bleServiceStatus = translated(new QLabel, "text", "otg.ble.status_checking");
    m_bleServiceStatus->setObjectName("bleServiceStatus");
    m_bleServiceStatus->setWordWrap(true);
    addTranslatedRow(bleForm, "otg.ble.status_label", m_bleServiceStatus);
    if (m_adapter) {
        m_bleStatusTimer = new QTimer(this);
        m_bleStatusTimer->setInterval(2000);
        m_bleServiceStatus->installEventFilter(this);
        connect(m_bleStatusTimer, &QTimer::timeout, this, &MainWindow::refreshBleServiceStatus);
        connect(m_adapter, &QLineEdit::textChanged, this, [this] {
            ++m_bleStatusGeneration;
            m_bleServiceIdle = false;
            setTranslatedProperty(m_bleServiceStatus, "text", "otg.ble.status_checking");
            m_stopBleService->setEnabled(false);
        });
        connect(m_adapter, &QLineEdit::editingFinished, this, &MainWindow::refreshBleServiceStatus);
    }
    m_startBleService = translated(new QPushButton, "text", "otg.ble.start_service");
    m_startBleService->setObjectName("startBleService");
    bleForm->addRow(m_startBleService);
    connect(m_startBleService, &QPushButton::clicked, this, &MainWindow::startBleService);
    m_stopBleService = translated(new QPushButton, "text", "otg.ble.stop_service");
    m_stopBleService->setObjectName("stopBleService");
    bleForm->addRow(m_stopBleService);
    connect(m_stopBleService, &QPushButton::clicked, this, &MainWindow::stopBleService);
    sharedLayout->addWidget(bleBox);
#endif
    sharedLayout->addWidget(advancedBox);
    sharedLayout->addStretch();
    formLayout->addStretch();
    for (auto *page : {m_form, m_sharedBox}) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        m_tabs->addTab(scroll, QString());
    }

    auto *buttons = new QHBoxLayout;
    m_status = translated(new QLabel, "text", m_session ? "otg.status.not_connected" : "otg.multi.add_first");
    m_status->setWordWrap(true);
    m_connect = translated(new QPushButton, "text", "otg.action.connect");
    m_connect->setObjectName("connectButton");
    m_connect->setDefault(true);
    m_disconnect = translated(new QPushButton, "text", "otg.action.disconnect");
    buttons->addWidget(m_status, 1);
#ifdef OTG_GUI_HAVE_SCRCPY
    m_manualClipboardButton = translated(new QPushButton, "text", "otg.manual_clipboard.title");
    m_manualClipboardButton->setObjectName("manualClipboard");
    setTranslatedProperty(m_manualClipboardButton, "toolTip", "otg.manual_clipboard.hint");
    auto *clipboardMenu = new QMenu(m_manualClipboardButton);
    m_manualClipboardButton->setMenu(clipboardMenu);
    auto *fromPhone = translated(clipboardMenu->addAction(QString()), "text", "otg.manual_clipboard.from_phone");
    fromPhone->setObjectName("clipboardFromPhone");
    connect(fromPhone, &QAction::triggered, this, [this] { transferClipboard(PhoneClipboard::Direction::FromPhone); });
    auto *toPhone = translated(clipboardMenu->addAction(QString()), "text", "otg.manual_clipboard.to_phone");
    toPhone->setObjectName("clipboardToPhone");
    connect(toPhone, &QAction::triggered, this, [this] { transferClipboard(PhoneClipboard::Direction::ToPhone); });
    connect(&m_manualClipboard, &PhoneClipboard::completed, this, [this](const QString &messageId) {
        if (m_closing) { QTimer::singleShot(0, this, &QWidget::close); return; }
        updateClipboardAction();
        if (messageId.isEmpty()) return;
        const auto message = qtTrId(messageId.toLatin1().constData()).arg(m_clipboardDeviceName);
        appendLog(message, m_clipboardDeviceId);
        statusBar()->showMessage(message, 6000);
    });
    buttons->addWidget(m_manualClipboardButton);
#endif
    m_copyScreenshot = translated(new QPushButton, "text", "otg.screenshot.copy");
    m_copyScreenshot->setObjectName("copyScreenshot");
    setTranslatedProperty(m_copyScreenshot, "toolTip", "otg.screenshot.hint");
    buttons->addWidget(m_copyScreenshot);
    connect(m_copyScreenshot, &QPushButton::clicked, this, [this] { copyScreenshot(m_selectedId); });
    connect(&m_screenshot, &PhoneScreenshot::completed, this, [this](const QString &messageId) {
        if (m_closing) { QTimer::singleShot(0, this, &QWidget::close); return; }
        updateScreenshotActions();
        if (messageId.isEmpty()) return;
        const auto message = qtTrId(messageId.toLatin1().constData()).arg(m_screenshotDeviceName);
        appendLog(message, m_screenshotDeviceId);
        statusBar()->showMessage(message, 6000);
        if (!isVisible() && m_tray->isVisible() && QSystemTrayIcon::supportsMessages())
            m_tray->showMessage(windowTitle(), message);
    });
    m_transferFiles = translated(new QPushButton, "text", "otg.transfer.title");
    m_transferFiles->setObjectName("transferFiles");
    buttons->addWidget(m_transferFiles);
    connect(m_transferFiles, &QPushButton::clicked, this, [this] {
        if (m_transferDialog) { m_transferDialog->show(); m_transferDialog->raise(); return; }
        const auto target = adbTargetForCurrentDevice();
        auto *dialog = new TransferDialog(m_adb->text().trimmed(), target, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_transferDialog = dialog;
        connect(dialog, &TransferDialog::stopped, this, [this] {
            m_transferDialog = nullptr;
            if (m_closing) QTimer::singleShot(0, this, &QWidget::close);
        });
        dialog->show();
    });
    buttons->addWidget(m_connect);
    buttons->addWidget(m_disconnect);
    rightLayout->addLayout(buttons);
    auto *logHeader = new QHBoxLayout;
    logHeader->addWidget(translated(new QLabel, "text", "otg.log.title"), 1);
    m_logFilter = new NoWheelWidget<QComboBox>;
    m_logFilter->setObjectName("logFilter");
    logHeader->addWidget(m_logFilter);
    auto *clearLog = translated(new QPushButton, "text", "otg.action.clear_log");
    clearLog->setObjectName("clearLogButton");
    logHeader->addWidget(clearLog);
    root->addLayout(logHeader);
    m_log = new QPlainTextEdit;
    m_log->setObjectName("runtimeLog");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1200);
    m_log->setMinimumHeight(100);
    setTranslatedProperty(m_log, "placeholderText", "otg.log.placeholder");
    root->addWidget(m_log, 1);
    connect(clearLog, &QPushButton::clicked, this, [this] { m_logHistory.clear(); m_log->clear(); });
    connect(m_logFilter, &QComboBox::currentIndexChanged, this, &MainWindow::refreshLog);
    setCentralWidget(central);

    QString configuredAdb = settings.value("adb", findAdb()).toString();
    if (!configuredAdb.isEmpty() && !QFileInfo(configuredAdb).isExecutable())
        configuredAdb = findAdb();
    m_adb->setText(configuredAdb);
    m_adbSerial->setText(settings.value("adbSerial").toString());
    m_adbEnabled->setChecked(settings.value("adbEnabled", false).toBool());
    m_audio->setChecked(settings.value("audio", false).toBool());
    m_audioGain->setValue(storedAudioGain(settings.value("audioGainDb")));
    m_clipboard->setChecked(settings.value("clipboard", false).toBool());
    m_autoRotate->setChecked(settings.value("autoRotate", false).toBool());
    connect(m_audio, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_audioGain, &QSpinBox::valueChanged, this, [this] {
        if (m_uiReady && !m_loadingDevice) { storeDevice(); persistDevices(); }
    });
    connect(m_clipboard, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_autoRotate, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_adbEnabled, &QCheckBox::toggled, this, [this] {
        if (m_uiReady && !m_loadingDevice) { storeDevice(); persistDevices(); }
        updateEnabled();
    });
    m_deskflow->setText(settings.value("deskflow", findDeskflow()).toString());
    m_deskflowConfig->setText(settings.value("deskflowConfigFile").toString());
    m_serial->setText(settings.value("serial").toString());
    // Preserve existing AOA profiles; only fresh Windows profiles prefer UHID.
    if (m_session) setProfileTransport(settings.value("transport", "usb").toString());
    if (m_transport == "uhid") m_adbEnabled->setChecked(true);
    m_width->setValue(settings.value("originalWidth", settings.value("width", 1080)).toInt());
    m_height->setValue(settings.value("originalHeight", settings.value("height", 1920)).toInt());
    m_sensitivity->setValue(settings.value("sensitivityMultiplier", 1.0).toDouble());
    m_motionMultiplier->setValue(settings.value("motionMultiplier", 1.0).toDouble());
    m_computer->setText(settings.value("computer", "computer").toString());
    m_phone->setText(settings.value("phone", "android").toString());
    m_host->setText(settings.value("host", "127.0.0.1").toString());
    m_port->setValue(settings.value("port", 24800).toInt());
    m_manage->setChecked(settings.value("manageServer", false).toBool());
    m_tls->setChecked(settings.value("tls", false).toBool());
    m_tlsKeyLength->setCurrentIndex(qMax(0, m_tlsKeyLength->findData(settings.value("tlsKeyLength", 2048))));
    m_tlsCert->setText(settings.value("tlsCertificate").toString());
    m_tlsFingerprint->setText(settings.value("tlsFingerprint").toString());
    m_tlsFingerprintFile->setText(settings.value("tlsFingerprintFile").toString());
    m_mouseMode->setCurrentIndex(qMax(0, m_mouseMode->findData(settings.value("mouseMode", "absolute"))));
    m_absolutePerAxis->setChecked(settings.value("absolutePerAxis", false).toBool());
    m_middleButtonAction->setCurrentIndex(qMax(0, m_middleButtonAction->findData(
        settings.value("middleButtonAction", "home"))));
    m_rightButtonAction->setCurrentIndex(qMax(0, m_rightButtonAction->findData(
        settings.value("rightButtonAction", "back"))));
    m_sideButton1Action->setCurrentIndex(qMax(0, m_sideButton1Action->findData(
        settings.value("sideButton1Action", "swipe-up"))));
    m_sideButton2Action->setCurrentIndex(qMax(0, m_sideButton2Action->findData(
        settings.value("sideButton2Action", "swipe-down"))));
    m_sideSwipeDistance->setValue(settings.value("sideSwipeDistancePercent", 17).toInt());
    m_unlockEnabled->setChecked(settings.value("unlockEnabled", false).toBool());
    m_unlockPin->setText(settings.value("unlockPin").toString());
    if (m_transport == "ble") m_mouseMode->setCurrentIndex(0);
    const int profileCount = settings.beginReadArray("rememberedDevices");
    for (int i = 0; i < profileCount; ++i) {
        settings.setArrayIndex(i);
        auto profile = settings.value("profile").toMap();
        for (const auto *key : legacyBleSettingKeys) profile.remove(key);
        profile.remove("blePeer");
        profile.remove("neighbor");
        profile.remove("direction");
        migrateUhidTarget(profile);
        const auto key = autoConnectKey(profile);
        if (!key.second.isEmpty()) m_profiles.insert(key, profile);
    }
    settings.endArray();
    m_autoConnectEnabled->setChecked(settings.value("autoConnect", false).toBool());
    m_alias->setText(m_phone->text());
    if (m_session) {
        auto profile = deviceProfile();
        if (m_transport == "uhid") {
            profile.remove("uhidAccess");
            profile.remove("uhidUsbSerialLocked");
            migrateUhidTarget(profile);
        }
        m_records.insert(m_selectedId, profile);
    }
    QVariantMap legacyBle;
    settings.beginReadArray("devices");
    for (int i = 0; i < deviceCount; ++i) {
        settings.setArrayIndex(i);
        const auto id = settings.value("id").toString();
        auto profile = settings.value("profile").toMap();
        if (!id.isEmpty() && !profile.isEmpty()) {
            if (legacyBle.isEmpty() && profile.value("transport") == "ble") legacyBle = profile;
            for (const auto *key : legacyBleSettingKeys) profile.remove(key);
            profile.remove("blePeer");
            profile.remove("neighbor");
            profile.remove("direction");
            migrateUhidTarget(profile);
            if (i == 0) {
                // Reuse the initial Session so existing single-device settings migrate naturally.
                m_records[m_selectedId] = profile;
            } else {
                m_records[id] = profile;
                m_connections.addDevice(id);
            }
        }
    }
    settings.endArray();
    for (auto &profile : m_records) {
        if (profile.value("transport") == "uhid" && !profile.contains("uhidUsbSerialLocked")) {
            const auto key = autoConnectKey(profile);
            profile["uhidUsbSerialLocked"] = !key.second.isEmpty() && m_profiles.contains(key);
        }
    }
    if (m_bleMulti) m_bleMulti->setChecked(settings.value("sharedBle/multiDevice", false).toBool());
    if (m_adapter) {
        const auto sharedValue = [&](const char *sharedKey, const char *oldKey, const QVariant &fallback) {
            return settings.value(sharedKey, legacyBle.value(oldKey, settings.value(oldKey, fallback)));
        };
        m_adapter->setText(sharedValue("sharedBle/adapter", "bleAdapter", "hci0").toString());
        m_bleLegacyHandles->setChecked(sharedValue("sharedBle/legacyHandles", "bleLegacyHandles", false).toBool());
        // Save before a device-only update can discard the legacy profile fields.
        saveSharedBleSettings(settings);
    }
    if (m_session) applyDeviceProfile(m_records.value(m_selectedId));
    auto saveAutoConnect = [this] {
        if (m_loadingDevice) return;
        storeDevice();
        if (!m_unsavedDevices.contains(m_selectedId))
            QSettings().setValue("autoConnect", m_autoConnectEnabled->isChecked());
        persistDevices();
        updateAutoConnect();
    };
    connect(m_autoConnectEnabled, &QCheckBox::toggled, this, saveAutoConnect);
    connect(m_uhidAutoConnectEnabled, &QCheckBox::toggled, this, saveAutoConnect);
    connect(&m_autoConnect, &UsbAutoConnect::connectRequested, this, [this](const QString &serial) {
        autoConnectDevice("usb", serial);
    });
    connect(&m_adbAutoConnect, &UsbAutoConnect::connectRequested, this, [this](const QString &serial) {
        autoConnectDevice("uhid", serial);
    });
    connect(&m_adbAutoConnect, &UsbAutoConnect::scanFailed, this, [this] {
        appendLog(qtTrId("otg.uhid.auto_scan_failed"));
    });
    connect(&m_autoConnect, &UsbAutoConnect::scanFailed, this, [this] {
        appendLog(qtTrId("otg.auto_connect.scan_failed"));
    });
    connect(&m_autoConnect, &UsbAutoConnect::detectionModeChanged, this, [this](bool hotplug) {
        appendLog(hotplug ? qtTrId("otg.auto_connect.hotplug") : qtTrId("otg.auto_connect.polling"));
    });
    connect(m_width, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_height, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_sensitivity, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_motionMultiplier, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_mouseMode, &QComboBox::currentIndexChanged, this, &MainWindow::updateVirtualSize);
    updateVirtualSize();
    connect(m_manage, &QCheckBox::toggled, this, &MainWindow::updateEnabled);
    connect(m_serial, &QLineEdit::textChanged, this, [this] {
        m_companionStatuses.remove(m_selectedId);
        updateEnabled();
    });
    const auto targetChanged = [this] {
        if (m_loadingDevice) return;
        m_companionStatuses.remove(m_selectedId);
        updateEnabled();
    };
    connect(m_adbSerial, &QLineEdit::textChanged, this, targetChanged);
    connect(m_uhidUsbSerial, &QLineEdit::textChanged, this, targetChanged);
    connect(m_uhidWirelessTarget, &QLineEdit::textChanged, this, targetChanged);
    connect(m_adb, &QLineEdit::textChanged, this, &MainWindow::updateEnabled);
    connect(m_absolutePerAxis, &QCheckBox::toggled, this, [this] {
        if (!m_loadingDevice) { storeDevice(); persistDevices(); }
        updateVirtualSize();
    });
    connect(m_bleAutoAttach, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_loadingDevice) return;
        storeDevice();
        persistDevices();
        if (enabled && m_pendingBleIdentities.contains(m_selectedId) &&
            m_connections.state(m_selectedId) == Session::State::Starting) {
            auto profile = deviceProfile();
            auto options = connectionOptions(profile);
            options.bleDiscovery = false;
            m_connections.attachBle(m_selectedId, options);
        }
        updateEnabled();
    });
    connect(m_uhidAccess, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loadingDevice) return;
        ++m_request;
        m_scanning = false;
        m_companionStatuses.remove(m_selectedId);
        m_deviceStatus->clear();
        storeDevice();
        persistDevices();
        updateEnabled();
    });
    connect(m_adbTest, &QPushButton::clicked, this, [this] {
        if (!m_adbEnabled->isChecked() || m_adb->text().trimmed().isEmpty() ||
            adbTargetForCurrentDevice().isEmpty() || m_scanning)
            return;
        m_scanning = true;
        updateEnabled();
        const auto adb = m_adb->text().trimmed();
        const auto target = adbTargetForCurrentDevice();
        runCommand(this, adb, {"start-server"}, [this, adb, target](bool started, const QString &startOutput) {
            if (!started) {
                m_scanning = false;
                m_adbStatus->setText(qtTrId("otg.adb.status.failed"));
                appendLog(withProcessOutput(qtTrId("otg.log.adb.server_start_failed"), "ADB", startOutput));
                updateEnabled();
                return;
            }
            runCommand(this, adb, {"-s", target, "get-state"}, [this, target](bool ok, const QString &output) {
                m_scanning = false;
                const bool ready = ok && output.trimmed() == "device";
                m_adbStatus->setText(qtTrId(ready ? "otg.adb.status.ready" : "otg.adb.status.failed"));
                appendLog(withProcessOutput(qtTrId(ready ? "otg.log.adb.target_ready" : "otg.log.adb.target_unavailable").arg(target),
                                            "ADB", output));
                updateEnabled();
            });
        });
    });
    connect(m_adbReadDisplay, &QPushButton::clicked, this, &MainWindow::readDisplayFromAdb);
    connect(m_adbWirelessSetup, &QPushButton::clicked, this, &MainWindow::setupWirelessAdb);
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectDevice);
    connect(m_disconnect, &QPushButton::clicked, this, [this] { m_connections.stop(m_selectedId); });
    connect(&m_connections, &ConnectionManager::log, this, [this](const QString &id, const QString &message) {
        const auto name = m_records.value(id).value("phone", id).toString();
        appendLog(id.isEmpty() ? message : "[" + name + "] " + message, id);
    });
    connect(&m_connections, &ConnectionManager::usbAttached, &m_autoConnect, &UsbAutoConnect::suppressAttachment);
    connect(&m_connections, &ConnectionManager::displaySizeChanged, this, [this](const QString &id, const QSize &size) {
        if (m_closing || !m_records.contains(id)) return;
        auto &profile = m_records[id];
        profile["originalWidth"] = size.width();
        profile["originalHeight"] = size.height();
        if (m_pendingProfiles.contains(id)) m_pendingProfiles[id] = profile;
        if (id == m_selectedId) {
            m_width->setValue(size.width());
            m_height->setValue(size.height());
        }
        // Keep remembered USB configurations in the same orientation, including
        // phones rotating while another device is selected or the window hidden.
        if (!m_unsavedDevices.contains(id) && m_profiles.contains(autoConnectKey(profile)))
            rememberDevice(profile);
        persistDevices();
    });
    connect(&m_connections, &ConnectionManager::changed, this, [this](const QString &id) {
        m_sessionStatus = true;
        m_status->setText(m_connections.status(m_selectedId));
        updateConnectionStatus();
        const auto state = m_connections.state(id);
        if (state == Session::State::Connected) {
            if (lockConnectedUhidUsbSerial(id)) persistDevices();
            if (m_unsavedDevices.remove(id)) {
                if (id == m_selectedId) storeDevice();
                persistDevices();
            }
            if (m_pendingProfiles.contains(id)) {
                const auto profile = m_pendingProfiles.take(id);
                if (profile.value("transport") != "uhid" ||
                    m_connections.activeUhidTarget(id) == autoConnectKey(profile).second)
                    rememberDevice(profile);
            }
        }
        updateEnabled();
        updateDeviceList();
        if (m_stoppingBleService) QTimer::singleShot(0, this, &MainWindow::finishBleServiceStop);
        if (m_discoverMultipleBle && m_discoveryId.isEmpty() && m_records.contains(id) &&
            m_records.value(id).value("transport") == "ble" && m_connections.state(id) == Session::State::Idle)
            QTimer::singleShot(0, this, &MainWindow::continueBleDiscovery);
        if (m_closing && m_connections.settled())
            QTimer::singleShot(0, this, &QWidget::close);
    });
    connect(&m_connections, &ConnectionManager::bleIdentityDetected, this, &MainWindow::acceptBleIdentity);
    connect(m_addDevice, &QPushButton::clicked, this, &MainWindow::addDevice);
    connect(m_removeDevice, &QPushButton::clicked, this, &MainWindow::removeDevice);
    connect(m_deviceSidebar, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) {
        if (current) selectDevice(current->data(Qt::UserRole).toString());
    });
    connect(m_connectAll, &QPushButton::clicked, this, &MainWindow::connectAll);
    connect(m_disconnectAll, &QPushButton::clicked, this, [this] {
        m_discoverMultipleBle = false;
        m_connections.stopAll();
    });
    connect(m_phone, &QLineEdit::editingFinished, this, [this] {
        storeDevice();
        persistDevices();
        updateDeviceList();
        updateEnabled();
    });
    connect(m_language, &QComboBox::currentIndexChanged, this, [this] {
        const QString language = m_language->currentData().toString();
        if (LanguageManager::instance().setLanguage(language)) {
            QSettings().setValue("language", language);
            retranslateUi();
        }
    });
    m_uiReady = true;
    setupTray();
    retranslateUi();
    updateEnabled();
    QTimer::singleShot(0, this, [this] {
        m_autoConnect.setProgram(bridgeExecutable());
        m_monitorsReady = true;
        updateAutoConnect();
        if (m_transport == "ble") startBleService();
    });
}

void MainWindow::appendLog(const QString &text, const QString &id)
{
    if (text.isEmpty()) return;
    const auto line = QTime::currentTime().toString("HH:mm:ss ") + text;
    m_logHistory.append({id, line});
    if (m_logHistory.size() > 1200) m_logHistory.removeFirst();
    const auto filter = m_logFilter->currentData().toString();
    if (filter == "*" || filter == id) m_log->appendPlainText(line);
}

void MainWindow::refreshLog()
{
    const auto filter = m_logFilter->currentData().toString();
    QStringList lines;
    for (const auto &entry : m_logHistory)
        if (filter == "*" || filter == entry.first) lines.append(entry.second);
    m_log->setPlainText(lines.join('\n'));
}

void MainWindow::storeDevice()
{
    if (m_loadingDevice || !m_uiReady || !m_session) return;
    auto profile = deviceProfile();
    if (autoConnectOptedIn(profile) && otherScreenAutoConnect(screenKey(profile))) {
        auto *checkbox = m_transport == "uhid" ? m_uhidAutoConnectEnabled : m_autoConnectEnabled;
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(false);
        profile = deviceProfile();
        appendLog(qtTrId("otg.multi.auto_connect_conflict").arg(profile.value("phone").toString()), m_selectedId);
    }
    m_records[m_selectedId] = profile;
}

void MainWindow::persistDevices()
{
    // Keep the stable initial ID first; selection must never change record identity.
    QStringList ids = m_records.keys();
    ids.removeIf([this](const QString &id) { return m_unsavedDevices.contains(id); });
    if (ids.removeAll("initial")) ids.prepend("initial");
    QSettings settings;
    const int previousCount = settings.value("devices/size").toInt();
    for (int index = ids.size() + 1; index <= previousCount; ++index)
        settings.remove(QStringLiteral("devices/%1").arg(index));
    settings.beginWriteArray("devices", ids.size());
    int index = 0;
    for (const auto &id : ids) {
        settings.setArrayIndex(index++);
        settings.setValue("id", id);
        settings.setValue("profile", m_records.value(id));
    }
    settings.endArray();
}

void MainWindow::updateAutoConnect()
{
    if (!m_monitorsReady || m_loadingDevice) return;
    auto records = m_records;
    if (m_session) records[m_selectedId] = deviceProfile();
    QMap<QPair<QString, QString>, int> counts;
    QMap<QString, int> screenCounts;
    for (const auto &record : records) {
        ++counts[autoConnectKey(record)];
        if (autoConnectOptedIn(record)) ++screenCounts[screenKey(record)];
    }
    QStringList aoa, adb;
    bool watchAdb = false;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const auto key = autoConnectKey(it.value());
        if (key.second.isEmpty() || counts.value(key) != 1 || !autoConnectOptedIn(it.value()) ||
            screenCounts.value(screenKey(it.value())) != 1) continue;
        if (key.first == "usb") {
            if (!m_unsavedDevices.contains(it.key()) && m_profiles.contains(key)) aoa.append(key.second);
#ifdef OTG_GUI_HAVE_UHID
        } else if (key.first == "uhid") {
            watchAdb = true; // Also observe the current attachment during manual connect/cleanup.
            if (!m_unsavedDevices.contains(it.key()) && m_profiles.contains(key)) adb.append(key.second);
#endif
        }
    }
    m_autoConnect.setKnownSerials(aoa);
    m_autoConnect.setEnabled(!aoa.isEmpty() && !m_closing);
    m_adbAutoConnect.setProgram(m_adb->text().trimmed());
    m_adbAutoConnect.setKnownSerials(adb);
    m_adbAutoConnect.setReady(!m_closing && !m_scanning && !m_companion.busy() &&
                              m_connections.serverState() != DeskflowServer::State::Stopping);
    m_adbAutoConnect.setEnabled(watchAdb && !m_closing && !m_adb->text().trimmed().isEmpty());
}

void MainWindow::autoConnectDevice(const QString &transport, const QString &serial)
{
    const auto key = qMakePair(transport, serial);
    if (m_closing || !m_profiles.contains(key)) return;
    storeDevice();
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        if (m_unsavedDevices.contains(it.key()) || (m_wirelessAdbDialog && it.key() == m_selectedId) ||
            autoConnectKey(it.value()) != key || !autoConnectOptedIn(it.value()) ||
            screenInUse(screenKey(it.value()))) continue;
        const auto id = it.key();
        auto profile = transport == "uhid" ? it.value() : m_profiles.value(key);
        // Preserve the current screen name and opt-ins when restoring connection parameters.
        for (const auto *field : {"phone", "alias", "autoConnect", "uhidUsbAutoConnect"})
            profile[field] = it->value(field);
        // Use the currently configured gain, even if edited since the last connection.
        if (it->contains("audioGainDb")) profile["audioGainDb"] = it->value("audioGainDb");
        // A remembered AOA profile may have been created before the user
        // enabled ADB enhancements. These are per-device feature settings,
        // so restore the current record rather than the older successful
        // connection snapshot. Otherwise auto-connect silently unchecks ADB
        // and drops audio/clipboard on the next insertion.
        for (const auto *field : {"adbEnabled", "adbSerial", "audio", "clipboard", "autoRotate",
                                  "unlockEnabled", "unlockPin", "unlockWithSpace"})
            if (it->contains(field)) profile[field] = it->value(field);
        m_records[id] = profile;
        m_pendingProfiles[id] = profile;
        appendLog(qtTrId("otg.auto_connect.connecting").arg(serial), id);
        if (id == m_selectedId) applyDeviceProfile(profile);
        auto options = connectionOptions(profile);
        if (transport == "uhid") options.adbSerial = serial;
        m_connections.start(id, options);
        break;
    }
}

void MainWindow::addDevice()
{
    if (m_closing || m_stoppingBleService || m_companion.busy()) return;
    storeDevice();
    auto profile = deviceProfile();
    QString name;
    int number = m_records.size() + 1;
    do {
        name = number == 1 ? QStringLiteral("android") : "android" + QString::number(number);
        ++number;
    } while (std::any_of(m_records.cbegin(), m_records.cend(), [&](const QVariantMap &record) {
        return screenKey(record) == name;
    }) || name.compare(m_computer->text().trimmed(), Qt::CaseInsensitive) == 0);

    AddDeviceDialog dialog(this);
    dialog.setObjectName("addDeviceDialog");
    dialog.setWindowTitle(qtTrId("otg.multi.add_device"));
    dialog.setMinimumWidth(510);
    auto *form = new QFormLayout(&dialog);
    auto *method = new NoWheelWidget<QComboBox>;
    method->setObjectName("newDeviceTransport");
    method->addItem(qtTrId(transportLabelId("usb")), "usb");
    if (supportsTransport("uhid")) method->addItem(qtTrId(transportLabelId("uhid")), "uhid");
    method->setCurrentIndex(method->findData(defaultTransport()));
    form->addRow(qtTrId("otg.label.transport"), method);
    auto *screen = new NoWheelWidget<QComboBox>;
    screen->setObjectName("newDeviceScreen");
    screen->setEditable(true);
    screen->addItem(name);
    for (const auto &record : std::as_const(m_records)) {
        const auto existing = record.value("phone").toString().trimmed();
        if (validScreenName(existing) && screen->findText(existing, Qt::MatchFixedString) < 0)
            screen->addItem(existing);
    }
    form->addRow(qtTrId("otg.label.phone_name"), screen);
    auto *alias = new QLineEdit;
    alias->setObjectName("newDeviceAlias");
    alias->setPlaceholderText(name);
    form->addRow(qtTrId("otg.multi.alias"), alias);

    auto *access = new NoWheelWidget<QComboBox>;
    access->setObjectName("newDeviceAdbAccess");
    access->addItem(qtTrId("otg.uhid.usb_access"), "usb");
    access->addItem(qtTrId("otg.uhid.wireless_access"), "wireless");
    form->addRow(qtTrId("otg.uhid.access_label"), access);

    auto *deviceRow = new QWidget;
    auto *deviceLayout = new QHBoxLayout(deviceRow);
    deviceLayout->setContentsMargins(0, 0, 0, 0);
    auto *devices = new NoWheelWidget<QComboBox>;
    devices->setObjectName("newDeviceSelector");
    auto *refresh = new QPushButton(qtTrId("otg.action.refresh"));
    refresh->setObjectName("newDeviceRefresh");
    deviceLayout->addWidget(devices, 1);
    deviceLayout->addWidget(refresh);
    form->addRow(qtTrId("otg.label.device"), deviceRow);
    auto *serial = new QLineEdit;
    serial->setObjectName("newDeviceUsbSerial");
    auto *serialLabel = new QLabel;
    form->addRow(serialLabel, serial);
    auto *address = new QLineEdit;
    address->setObjectName("newDeviceWirelessTarget");
    address->setPlaceholderText(qtTrId("otg.uhid.wireless_hint"));
    form->addRow(qtTrId("otg.uhid.wireless_target"), address);
    auto *wirelessSetup = new QPushButton(qtTrId("otg.adb.wireless_setup"));
    form->addRow(QString(), wirelessSetup);

    auto *dimensions = new QWidget;
    auto *dimensionLayout = new QHBoxLayout(dimensions);
    dimensionLayout->setContentsMargins(0, 0, 0, 0);
    auto *width = new NoWheelWidget<QSpinBox>;
    auto *height = new NoWheelWidget<QSpinBox>;
    width->setRange(1, 32767);
    height->setRange(1, 32767);
    width->setValue(m_width->value());
    height->setValue(m_height->value());
    dimensionLayout->addWidget(width);
    dimensionLayout->addWidget(new QLabel("×"));
    dimensionLayout->addWidget(height);
    auto *readSize = new QPushButton(qtTrId("otg.adb.read_display"));
    dimensionLayout->addWidget(readSize);
    dimensionLayout->addStretch();
    form->addRow(qtTrId("otg.label.original_size"), dimensions);

    auto *hint = new QLabel(qtTrId("otg.multi.add_hint"));
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *status = new QLabel;
    status->setObjectName("newDeviceStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    form->addRow(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *connectButton = buttons->button(QDialogButtonBox::Ok);
    connectButton->setText(qtTrId("otg.action.connect"));
    connectButton->setObjectName("newDeviceConnect");
    buttons->button(QDialogButtonBox::Cancel)->setText(qtTrId("otg.action.cancel"));
    form->addRow(buttons);

    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto adb = m_adb->text().trimmed();
    const auto bridge = bridgeExecutable();
    QList<AndroidDevice> candidates;
    QList<AndroidDevice> adbDevices;
    bool probing = false;
    bool canceling = false;
    bool screenNameEdited = false;
    bool aliasEdited = false;
    QTimer wirelessModelTimer;
    wirelessModelTimer.setSingleShot(true);
    quint64 wirelessModelRequest = 0;
    bool readingWirelessModel = false;
    QString wirelessModel;
    const auto uhid = [method] { return method->currentData() == "uhid"; };
    const auto wireless = [uhid, access] { return uhid() && access->currentData() == "wireless"; };
    const auto target = [wireless, serial, address] { return (wireless() ? address : serial)->text().trimmed(); };
    const auto suggestNames = [&] {
        const auto value = target();
        const auto model = std::find_if(adbDevices.cbegin(), adbDevices.cend(), [&](const AndroidDevice &device) {
            return device.serial == value;
        });
        const bool hasModel = model != adbDevices.cend() && model->state == "device" &&
            std::none_of(std::next(model), adbDevices.cend(), [&](const AndroidDevice &device) {
                return device.serial == value;
            });
        auto displayName = hasModel ? model->model : QString();
        if (wireless() && !wirelessModel.isEmpty()) displayName = wirelessModel;
        if (!screenNameEdited) {
            const auto base = screenNameForAdbModel(displayName);
            auto suggestion = name;
            if (!base.isEmpty()) {
                suggestion = base;
                QSet<QString> reserved{m_computer->text().trimmed().toCaseFolded()};
                for (const auto &record : std::as_const(m_records)) reserved.insert(screenKey(record));
                for (int number = 2; reserved.contains(suggestion.toCaseFolded()); ++number) {
                    const auto suffix = '-' + QString::number(number);
                    suggestion = base.left(63 - suffix.size()) + suffix;
                }
            }
            const QSignalBlocker blocker(screen);
            screen->setEditText(suggestion);
        }
        if (!aliasEdited) alias->setText(displayName);
        alias->setPlaceholderText(screen->currentText().trimmed());
    };
    const auto updateDialog = [&] {
        const bool idle = m_connections.state(id) == Session::State::Idle && !probing && !canceling;
        const auto text = screen->currentText().trimmed();
        const auto value = target();
        const bool targetValid = wireless() ? isWirelessAdbTarget(value) :
                                 uhid() ? isUsbAdbSerial(value) : !value.isEmpty();
        const auto matches = std::count_if(candidates.cbegin(), candidates.cend(), [&](const AndroidDevice &device) {
            return device.serial == value;
        });
        const bool waitingForNames = readingWirelessModel && (!screenNameEdited || !aliasEdited);
        form->setRowVisible(access, uhid());
        form->setRowVisible(serial, !wireless());
        form->setRowVisible(address, wireless());
        form->setRowVisible(wirelessSetup, wireless());
        serialLabel->setText(qtTrId(uhid() ? "otg.uhid.usb_serial" : "otg.label.usb_serial"));
        readSize->setVisible(uhid());
        for (auto *field : std::initializer_list<QWidget *>{method, screen, alias, deviceRow, access, serial, address, dimensions})
            field->setEnabled(idle);
        readSize->setEnabled(targetValid && !adb.isEmpty());
        wirelessSetup->setEnabled(idle && !adb.isEmpty());
        connectButton->setEnabled(idle && !waitingForNames && targetValid && matches < 2 && validScreenName(text) &&
            text.compare(m_computer->text().trimmed(), Qt::CaseInsensitive) != 0 &&
            m_connections.serverState() != DeskflowServer::State::Stopping && !m_closing);
    };
    const auto scheduleWirelessModel = [&] {
        ++wirelessModelRequest;
        wirelessModelTimer.stop();
        readingWirelessModel = !m_closing && !probing && wireless() && !adb.isEmpty() &&
            isWirelessAdbTarget(target()) && (!screenNameEdited || !aliasEdited);
        if (readingWirelessModel) wirelessModelTimer.start(300);
    };
    connect(&wirelessModelTimer, &QTimer::timeout, &dialog, [&] {
        const auto request = wirelessModelRequest;
        runCommand(&dialog, adb, {"-s", target(), "shell", "getprop", "ro.product.model"},
                   [&, request](bool ok, const QString &output) {
            if (request != wirelessModelRequest || m_closing) return;
            readingWirelessModel = false;
            const auto model = output.trimmed();
            static const QRegularExpression controlCharacters("[\\p{Cc}\\p{Zl}\\p{Zp}]");
            if (ok && model.size() <= 256 && !model.contains(controlCharacters)) wirelessModel = model;
            suggestNames();
            updateDialog();
        }, 1500);
    });
    const auto resetDevices = [&] {
        candidates.clear();
        adbDevices.clear();
        const QSignalBlocker blocker(devices);
        devices->clear();
        devices->addItem(qtTrId(uhid() ? "otg.uhid.manual" : "otg.device.manual"));
    };
    resetDevices();
    const auto wirelessTargetChanged = [&] {
        wirelessModel.clear();
        scheduleWirelessModel();
        suggestNames();
        updateDialog();
    };
    const auto resetDiscovery = [&] {
        resetDevices();
        status->clear();
        wirelessTargetChanged();
    };
    connect(method, &QComboBox::currentIndexChanged, &dialog, [&] {
        serial->clear();
        resetDiscovery();
    });
    connect(access, &QComboBox::currentIndexChanged, &dialog, resetDiscovery);
    connect(screen, &QComboBox::currentTextChanged, &dialog, [&] {
        screenNameEdited = true;
        alias->setPlaceholderText(screen->currentText().trimmed());
        updateDialog();
    });
    connect(screen, &QComboBox::textActivated, &dialog, [&] { screenNameEdited = true; updateDialog(); });
    connect(alias, &QLineEdit::textEdited, &dialog, [&] { aliasEdited = true; updateDialog(); });
    connect(serial, &QLineEdit::textChanged, &dialog, [&] { suggestNames(); updateDialog(); });
    connect(address, &QLineEdit::textChanged, &dialog, wirelessTargetChanged);
    connect(devices, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
        if (index <= 0) return;
        const auto &device = candidates.at(index - 1);
        auto *selectedTarget = wireless() ? address : serial;
        if (device.state == "device") {
            selectedTarget->setText(device.serial);
            status->clear();
        } else {
            selectedTarget->clear();
            status->setText(qtTrId(device.state == "unauthorized" ? "otg.device.authorize_hint" : "otg.device.offline_hint"));
        }
    });
    const auto populateDevices = [&] {
        int selected = 0;
        for (int i = 0; i < candidates.size(); ++i) {
            const auto &device = candidates.at(i);
            const char *stateId = device.state == "device" ? "otg.device.online" :
                device.state == "unauthorized" ? "otg.device.unauthorized" :
                device.state == "offline" ? "otg.device.offline" : "otg.device.access_denied";
            const auto label = device.model.isEmpty() ? device.serial : device.model + " · " + device.serial;
            devices->addItem(QStringLiteral("%1 [%2]").arg(label, qtTrId(stateId)));
            if (device.serial == target()) selected = i + 1;
        }
        status->setText(candidates.isEmpty() ? qtTrId("otg.device.none_found") : QString());
        if (target().isEmpty() && candidates.size() == 1) selected = 1;
        devices->setCurrentIndex(selected);
        suggestNames();
        probing = false;
        updateDialog();
    };
    connect(refresh, &QPushButton::clicked, &dialog, [&] {
        const bool useAdb = uhid();
        const auto program = useAdb ? adb : bridge;
        if (program.isEmpty()) {
            status->setText(qtTrId(useAdb ? "otg.device.adb_not_found" : "otg.error.bridge_not_found"));
            return;
        }
        resetDevices();
        probing = true;
        status->setText(qtTrId("otg.device.refreshing"));
        updateDialog();
        runCommand(&dialog, program, useAdb ? QStringList{"devices", "-l"} : QStringList{"--usb-scan"},
                   [&, useAdb](bool ok, const QString &output) {
            if (!ok) {
                probing = false;
                status->setText(qtTrId("otg.device.scan_failed"));
                appendLog(withProcessOutput(qtTrId(useAdb ? "otg.log.adb.scan_failed" : "otg.log.usb_scan_failed"),
                                            useAdb ? "ADB" : qtTrId("otg.log.bridge_source"), output));
                suggestNames();
                updateDialog();
            } else if (useAdb) {
                adbDevices = parseAdbDevices(output);
                candidates = adbDevices;
                candidates.removeIf([&](const AndroidDevice &device) {
                    return wireless() ? !isWirelessAdbTarget(device.serial) : !isUsbAdbSerial(device.serial);
                });
                populateDevices();
            } else {
                candidates = parseUsbScanDevices(output);
                if (adb.isEmpty() || (candidates.isEmpty() && target().isEmpty())) {
                    populateDevices();
                    return;
                }
                // AOA discovery is independent of ADB. Use it only to look up a matching model.
                runCommand(&dialog, adb, {"devices", "-l"}, [&](bool adbOk, const QString &adbOutput) {
                    if (adbOk) adbDevices = parseAdbDevices(adbOutput);
                    populateDevices();
                }, 1500);
            }
        });
    });
    connect(wirelessSetup, &QPushButton::clicked, &dialog, [&] {
        WirelessAdbDialog setup(adb, &dialog);
        connect(&setup, &WirelessAdbDialog::logMessage, &dialog, [this](const QString &message) { appendLog(message); });
        connect(&setup, &WirelessAdbDialog::targetReady, &setup, [&](const QString &value) {
            address->setText(value);
            setup.accept();
        });
        if (setup.exec() == QDialog::Accepted) {
            scheduleWirelessModel();
            updateDialog();
        }
    });
    connect(readSize, &QPushButton::clicked, &dialog, [&] {
        const auto currentTarget = target();
        probing = true;
        status->setText(qtTrId("otg.device.reading"));
        updateDialog();
        runCommand(&dialog, adb, {"-s", currentTarget, "shell", "wm", "size"}, [&, currentTarget](bool ok, const QString &output) {
            const auto size = ok ? parseScreenSize(output) : QSize();
            if (!size.isValid()) {
                probing = false;
                status->setText(qtTrId("otg.device.size_failed"));
                updateDialog();
                return;
            }
            runCommand(&dialog, adb, {"-s", currentTarget, "shell", "dumpsys", "input"}, [&, size](bool rotationOk, const QString &input) {
                const auto rotation = rotationOk ? parseRotation(input) : -1;
                const auto oriented = rotation == 1 || rotation == 3 ? size.transposed() : size;
                width->setValue(oriented.width());
                height->setValue(oriented.height());
                probing = false;
                if (!readingWirelessModel && wirelessModel.isEmpty()) scheduleWirelessModel();
                status->setText(qtTrId(rotation < 0 ? "otg.device.orientation_unknown" : "otg.uhid.device_ready"));
                updateDialog();
            });
        });
    });

    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!connectButton->isEnabled()) return;
        saveSettings(); // Save the shared server and previously saved devices only.
        const auto screenName = screen->currentText().trimmed();
        profile["phone"] = screenName;
        const auto displayName = alias->text().trimmed();
        profile["alias"] = displayName.isEmpty() ? screenName : displayName;
        profile["transport"] = method->currentData();
        profile["serial"] = uhid() ? QString() : target();
        profile["adbSerial"] = "";
        profile["adbEnabled"] = uhid();
        profile["uhidUsbSerial"] = uhid() ? serial->text().trimmed() : QString();
        profile["uhidUsbSerialLocked"] = false;
        profile["uhidWirelessTarget"] = uhid() ? address->text().trimmed() : QString();
        profile["uhidAccess"] = access->currentData();
        profile["originalWidth"] = width->value();
        profile["originalHeight"] = height->value();
        profile["audio"] = false;
        profile["audioGainDb"] = 0;
        profile["clipboard"] = false;
        profile["autoRotate"] = false;
        profile["bleIdentity"] = "";
        profile["bleAutoAttach"] = true;
        profile["absolutePerAxis"] = false;
        profile["autoConnect"] = false;
        profile["uhidUsbAutoConnect"] = false;
        profile["unlockEnabled"] = false;
        profile["unlockPin"] = "";
        const auto options = connectionOptions(profile);
        if (options.bridge.isEmpty()) {
            status->setText(qtTrId("otg.error.bridge_not_found"));
            return;
        }
        m_connections.addDevice(id);
        if (!m_connections.start(id, options)) status->setText(m_connections.status(id));
        updateDialog();
    });
    dialog.canReject = [&] {
        if (m_connections.state(id) == Session::State::Idle) return true;
        if (!canceling) {
            canceling = true;
            updateDialog();
            m_connections.stop(id);
        }
        return false;
    };
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&m_connections, &ConnectionManager::changed, &dialog, [&](const QString &changedId) {
        if (changedId == id) {
            status->setText(m_connections.status(id));
            const auto state = m_connections.state(id);
            if ((canceling || m_closing) && state == Session::State::Idle) dialog.reject();
            else if (!canceling && !m_closing && state == Session::State::Connected) dialog.accept();
        }
        updateDialog();
    });
    updateDialog();
    if (dialog.exec() != QDialog::Accepted) {
        m_connections.removeDevice(id);
        return;
    }

    m_records[id] = profile;
    lockConnectedUhidUsbSerial(id);
    if (profile.value("transport") == "usb" || profile.value("uhidAccess") == "usb") rememberDevice(profile);
    persistDevices();
    selectDevice(id);
}

void MainWindow::removeDevice()
{
    if (m_closing || m_companion.busy() || m_records.size() <= 1) return;
    if (!m_connections.removeDevice(m_selectedId)) return;
    m_records.remove(m_selectedId);
    m_unsavedDevices.remove(m_selectedId);
    m_pendingProfiles.remove(m_selectedId);
    m_pendingBleIdentities.remove(m_selectedId);
    m_selectedId.clear();
    m_loadingDevice = true;
    selectDevice(m_records.firstKey());
    m_loadingDevice = false;
    persistDevices();
    updateAutoConnect();
}

void MainWindow::selectDevice(const QString &id)
{
    if (id == m_selectedId || !m_records.contains(id) || m_closing) return;
    storeDevice();
    m_selectedId = id;
    m_session = m_connections.session(id);
    applyDeviceProfile(m_records.value(id));
    m_sessionStatus = true;
    m_status->setText(m_connections.status(id));
    updateConnectionStatus();
    updateDeviceList();
    updateEnabled();
}

void MainWindow::updateDeviceList()
{
    QSignalBlocker blocker(m_deviceSidebar);
    m_deviceSidebar->clear();
    int connected = 0;
    const char *states[] = {"otg.status.not_connected", "otg.status.connecting", "otg.multi.connected", "otg.status.disconnecting"};
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto state = m_connections.state(it.key());
        connected += state == Session::State::Connected;
        const auto alias = it->value("alias").toString();
        const auto name = it->value("phone").toString();
        const auto transport = it->value("transport", "usb").toString().toUpper();
        auto *item = new QListWidgetItem(QString("%1\n%2 · %3\n%4")
            .arg(alias.isEmpty() ? name : alias, transport, name,
                 qtTrId(m_connections.hasFailure(it.key()) ? "otg.multi.failed" : states[int(state)])), m_deviceSidebar);
        item->setData(Qt::UserRole, it.key());
        item->setToolTip(m_connections.status(it.key()));
        if (it.key() == "initial") {
            m_deviceSidebar->takeItem(m_deviceSidebar->row(item));
            m_deviceSidebar->insertItem(0, item);
        }
        if (it.key() == m_selectedId) m_deviceSidebar->setCurrentItem(item);
    }
    m_summary->setText(qtTrId("otg.multi.summary").arg(connected).arg(m_records.size()).arg(
        m_manage->isChecked() ? qtTrId(m_connections.serverState() == DeskflowServer::State::Running ?
                                    "otg.multi.server_running" : "otg.multi.server_stopped") : qtTrId("otg.multi.external_server")));
    const auto filter = m_logFilter->currentData();
    QSignalBlocker logBlocker(m_logFilter);
    m_logFilter->clear();
    m_logFilter->addItem(qtTrId("otg.multi.all_logs"), "*");
    m_logFilter->addItem(qtTrId("otg.multi.service_logs"), "");
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
        m_logFilter->addItem(it->value("phone").toString(), it.key());
    m_logFilter->setCurrentIndex(qMax(0, m_logFilter->findData(filter)));
    updateTray();
}

ConnectionOptions MainWindow::connectionOptions(const QVariantMap &profile)
{
    ConnectionOptions options;
    options.bridge = bridgeExecutable();
    options.adb = m_adb->text().trimmed();
    options.scrcpyServer = findScrcpyServer();
    options.scrcpyVersion = findScrcpyVersion();
    options.deskflow = m_deskflow->text().trimmed();
    options.deskflowConfig = m_deskflowConfig->text().trimmed();
    options.computer = m_computer->text().trimmed();
    options.host = m_host->text().trimmed();
    options.port = m_port->value();
    options.manageServer = m_manage->isChecked();
    options.tls = m_tls->isChecked();
    options.tlsCertificate = m_tlsCert->text().trimmed();
    options.tlsFingerprint = m_tlsFingerprint->text().trimmed();
    options.tlsFingerprintFile = m_tlsFingerprintFile->text().trimmed();
    options.phone = profile.value("phone", "android").toString();
    options.serial = profile.value("serial").toString();
    options.adbSerial = profileAdbTarget(profile);
    options.adbEnabled = profile.value("adbEnabled", false).toBool();
    options.transport = profile.value("transport", "usb").toString();
    if (options.transport == "uhid") options.adbEnabled = true;
#ifdef OTG_GUI_HAVE_AUDIO
    options.audio = options.adbEnabled && profile.value("audio", false).toBool();
    if (options.audio) options.audioGainDb = storedAudioGain(profile.value("audioGainDb"));
#else
    options.audio = false;
#endif
#ifdef OTG_GUI_HAVE_CLIPBOARD
    options.clipboard = options.adbEnabled && profile.value("clipboard", false).toBool();
#endif
    options.autoRotate = options.transport != "ble" &&
        options.adbEnabled && (options.transport == "uhid" || options.audio || options.clipboard) &&
        profile.value("autoRotate", false).toBool();
    options.mouseMode = options.transport == "ble" ? "absolute" : profile.value("mouseMode", "absolute").toString();
    options.absolutePerAxis = options.mouseMode == "absolute" && profile.value("absolutePerAxis", false).toBool();
    options.middleButtonAction = profile.value("middleButtonAction", "home").toString();
    options.rightButtonAction = profile.value("rightButtonAction", "back").toString();
    options.sideButton1Action = profile.value("sideButton1Action", "swipe-up").toString();
    options.sideButton2Action = profile.value("sideButton2Action", "swipe-down").toString();
    options.sideSwipeDistancePercent = profile.value("sideSwipeDistancePercent", 17).toInt();
    options.unlockPin = options.adbEnabled && profile.value("unlockEnabled", false).toBool() ?
        profile.value("unlockPin").toString() : QString();
    options.unlockAdb = !options.unlockPin.isEmpty();
    options.unlockWithSpace = profile.value("unlockWithSpace", true).toBool();
    if (options.transport == "ble") {
        options.bleIdentity = profile.value("bleIdentity").toString().trimmed().toUpper();
        options.bleAutoAttach = profile.value("bleAutoAttach", true).toBool();
        options.blePeer = options.bleIdentity;
#ifdef Q_OS_LINUX
        options.preferDirectedBle = true;
#else
        options.preferDirectedBle = m_bleMulti && m_bleMulti->isChecked();
#endif
    }
    if (options.transport == "ble" && m_adapter) {
        options.adapter = m_adapter->text().trimmed();
#ifdef Q_OS_LINUX
        options.preferDirectedBle = true;
        options.confirmSharedBleService = [this](const QString &adapter) {
            return confirmSharedBleService(adapter);
        };
#else
        options.preferDirectedBle = m_bleMulti->isChecked();
#endif
        options.bleLegacyHandles = m_bleLegacyHandles->isChecked();
    }
    const bool absolute = options.mouseMode == "absolute";
    const double resolutionMultiplier = absolute ?
        qMin(10.0, qMax(1.0, profile.value("sensitivityMultiplier", 1.0).toDouble())) : 1.0;
    options.motionMultiplier = absolute ?
        qMin(10.0, qMax(1.0, profile.value("motionMultiplier", 1.0).toDouble())) : 1.0;
    const int originalWidth = qBound(1, profile.value("originalWidth", 1080).toInt(), 32767);
    const int originalHeight = qBound(1, profile.value("originalHeight", 1920).toInt(), 32767);
    options.width = qMax(1, qRound(originalWidth / resolutionMultiplier));
    options.height = qMax(1, qRound(originalHeight / resolutionMultiplier));
    return options;
}

void MainWindow::connectAll()
{
    if (m_closing || m_scanning || m_stoppingBleService || m_companion.busy()) return;
    saveSettings();
    const auto selected = m_selectedId;
    QMap<QString, QStringList> candidates;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto transport = it->value("transport", "usb").toString();
        if (!supportsTransport(transport) || transport == "ble") continue;
        const auto target = transport == "uhid" ? profileAdbTarget(it.value()) : it->value("serial").toString();
        if (!target.isEmpty()) candidates[screenKey(it.value())].append(it.key());
    }
    QStringList skipped;
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        if (screenInUse(it.key())) continue;
        QString id;
        if (it->size() == 1) {
            id = it->first();
        } else {
            QStringList preferred;
            for (const auto &candidate : it.value())
                if (autoConnectOptedIn(m_records.value(candidate))) preferred.append(candidate);
            if (preferred.size() == 1) id = preferred.first();
        }
        if (id.isEmpty()) {
            skipped.append(m_records.value(it->first()).value("phone").toString());
            continue;
        }
        selectDevice(id);
        connectDevice();
    }
    selectDevice(selected);
    if (!skipped.isEmpty()) {
        const auto message = qtTrId("otg.multi.choose_connection").arg(skipped.join(", "));
        appendLog(message);
        statusBar()->showMessage(message, 10000);
    }
}

bool MainWindow::screenInUse(const QString &screen) const
{
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
        if (screenKey(it.value()) == screen && m_connections.state(it.key()) != Session::State::Idle)
            return true;
    return false;
}

bool MainWindow::otherScreenAutoConnect(const QString &screen) const
{
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
        if (it.key() != m_selectedId && screenKey(it.value()) == screen && autoConnectOptedIn(it.value()))
            return true;
    return false;
}

void MainWindow::updateEnabled()
{
    const bool idle = m_connections.state(m_selectedId) == Session::State::Idle;
    const bool bleDiscovery = m_connections.state(m_selectedId) == Session::State::Starting &&
        m_transport == "ble";
    const bool sharedIdle = !m_connections.busy() && m_connections.serverState() == DeskflowServer::State::Idle;
    const bool ble = m_transport == "ble";
    const bool uhid = m_transport == "uhid";
    const bool hasDevice = m_session != nullptr;
    updateConnectionStatus();
    m_deviceForm->setRowVisible(m_bleAutoAttach, ble);
    m_deviceForm->setRowVisible(m_uhidAccess, uhid);
    m_deviceForm->setRowVisible(m_serial, !ble && !uhid);
    // File sharing and companion status may use an explicit wireless ADB target
    // even with BLE HID, independently of the optional clipboard build feature.
    const bool adbFeatures = m_adbEnabled->isChecked();
    m_adbBox->setVisible(true);
    m_deviceForm->setRowVisible(m_autoConnectEnabled, !ble && !uhid);
    m_deviceForm->setRowVisible(m_uhidAutoConnectEnabled, uhid);
    setTranslatedProperty(m_adbEnabled, "text", uhid ? "otg.uhid.adb_required" : "otg.adb.enabled");
    setTranslatedProperty(m_adbEnabled, "toolTip", uhid ? "otg.uhid.setup_hint" : "otg.adb.enabled.tooltip");
#ifdef Q_OS_WIN
    setTranslatedProperty(m_adbForm->labelForField(m_adbSerial), "text", "otg.label.adb_wireless");
#else
    setTranslatedProperty(m_adbForm->labelForField(m_adbSerial), "text", "otg.label.adb_serial");
#endif
    m_adbForm->setRowVisible(m_adbSerial, !uhid);
    m_adbForm->setRowVisible(m_uhidUsbSerial, uhid);
    m_adbForm->setRowVisible(m_uhidWirelessTarget, uhid);
    m_programForm->setRowVisible(m_adb->parentWidget(), true);
#ifdef OTG_GUI_HAVE_CLIPBOARD
    m_adbForm->setRowVisible(m_clipboard, true);
    m_adbForm->setRowVisible(m_clipboardStatus, true);
#else
    m_adbForm->setRowVisible(m_clipboard, false);
    m_adbForm->setRowVisible(m_clipboardStatus, false);
#endif
    m_clipboardStatus->setText(idle ? qtTrId(m_clipboard->isChecked() ?
        "otg.clipboard.disconnected" : "otg.clipboard.off") : m_session->clipboardStatusMessage());
#ifdef OTG_GUI_HAVE_AUDIO
    // Audio uses the selected ADB target independently of the HID transport.
    m_adbForm->setRowVisible(m_audio, true);
    m_adbForm->setRowVisible(m_audioGain, adbFeatures && m_audio->isChecked());
#else
    m_adbForm->setRowVisible(m_audio, false);
    m_adbForm->setRowVisible(m_audioGain, false);
#endif
    m_featuresForm->setRowVisible(m_mouseMode, !ble);
    m_adbForm->setRowVisible(m_autoRotate, !ble);
    m_companionBox->setVisible(!ble);
    m_autoConnect.setReady(!m_closing && !m_companion.busy());
    updateAutoConnect();
    m_form->setVisible(hasDevice);
    m_form->setEnabled(hasDevice && (idle || bleDiscovery) && !m_scanning && !m_closing && !m_stoppingBleService && !m_companion.busy());
    const bool sharedEditable = sharedIdle && !m_closing && !m_stoppingBleService && !m_companion.busy();
    m_deskflowBox->setEnabled(sharedEditable);
    m_programForm->parentWidget()->setEnabled(sharedEditable);
    m_deskflowConfig->parentWidget()->setEnabled(m_manage->isChecked());
    updateBleServiceControls(sharedEditable);
    const bool deviceListEditable = !m_closing && !m_companion.busy();
    m_addDevice->setEnabled(deviceListEditable && !m_stoppingBleService);
    m_removeDevice->setEnabled(deviceListEditable && m_records.size() > 1 &&
                               m_connections.canRemoveDevice(m_selectedId));
    m_deviceSidebar->setEnabled(deviceListEditable);
    bool connectable = false;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto profile = it.key() == m_selectedId ? deviceProfile() : it.value();
        const auto mode = profile.value("transport", "usb").toString();
        const auto identity = mode == "uhid" ? profileAdbTarget(profile) : profile.value("serial").toString();
        connectable |= supportsTransport(mode) && !screenInUse(screenKey(profile)) && mode != "ble" && !identity.isEmpty();
    }
    m_connectAll->setEnabled(connectable && !m_closing && !m_scanning && !m_stoppingBleService &&
                             !m_companion.busy() && m_connections.serverState() != DeskflowServer::State::Stopping);
    m_disconnectAll->setEnabled(m_connections.busy() && !m_closing);
    const bool knownBle = ble && m_startBleService && !m_records.value(m_selectedId).value("bleIdentity").toString().isEmpty();
    setTranslatedProperty(m_connect, "text", knownBle ? "otg.ble.reconnect" : "otg.action.connect");
    m_connect->setEnabled(hasDevice && supportsTransport(m_transport) && idle && !m_scanning && !m_closing &&
                          !m_stoppingBleService && !m_companion.busy() && !screenInUse(screenKey(deviceProfile())) &&
                          m_connections.serverState() != DeskflowServer::State::Stopping &&
                          (ble ? knownBle : uhid ? !adbTargetForCurrentDevice().isEmpty() :
                           !m_serial->text().trimmed().isEmpty()));
    m_disconnect->setEnabled(!idle && m_connections.state(m_selectedId) != Session::State::Stopping);
    m_host->setEnabled(!m_manage->isChecked());
    m_tlsCert->parentWidget()->setEnabled(m_tls->isChecked());
    m_tlsKeyLength->setEnabled(m_tls->isChecked());
    m_tlsRegenerateCertificate->setEnabled(m_tls->isChecked());
    m_tlsFingerprint->setEnabled(m_tls->isChecked());
    m_tlsFingerprintFile->setEnabled(m_tls->isChecked());
    m_computer->setEnabled(m_manage->isChecked());
    m_phone->setEnabled(sharedIdle || !m_manage->isChecked());
    m_serial->setEnabled(!ble);
    m_adbEnabled->setEnabled(!uhid);
    m_adbSerial->setEnabled(adbFeatures);
    const bool uhidUsbSerialLocked = uhid && m_records.value(m_selectedId).value("uhidUsbSerialLocked").toBool();
    m_uhidUsbSerial->setReadOnly(uhidUsbSerialLocked);
    m_uhidUsbSerial->setToolTip(uhidUsbSerialLocked ? qtTrId("otg.uhid.usb_serial_locked") : QString());
    m_adb->setEnabled(true);
    m_adbTest->setEnabled(adbFeatures && !m_adb->text().trimmed().isEmpty() &&
                           !adbTargetForCurrentDevice().isEmpty() && idle);
    m_adbReadDisplay->setEnabled(adbFeatures && !m_adb->text().trimmed().isEmpty() &&
                                 !adbTargetForCurrentDevice().isEmpty() && idle);
    m_adbWirelessSetup->setEnabled(!m_wirelessAdbDialog && !m_adb->text().trimmed().isEmpty() && idle);
    setTranslatedProperty(m_adbStatus, "text", !adbFeatures ? "otg.adb.status.off" :
                          adbTargetForCurrentDevice().isEmpty() ? "otg.adb.status.target_missing" :
                          "otg.adb.status.ready_hint");
    m_audio->setEnabled(adbFeatures);
    m_clipboard->setEnabled(adbFeatures);
    m_autoRotate->setEnabled(adbFeatures && !ble && (uhid || m_audio->isChecked() || m_clipboard->isChecked()) && idle);
    const bool otherAuto = otherScreenAutoConnect(screenKey(deviceProfile()));
    m_autoConnectEnabled->setEnabled(!ble && !uhid && (!otherAuto || m_autoConnectEnabled->isChecked()));
    m_uhidAutoConnectEnabled->setEnabled(uhid && isUsbAdbSerial(m_uhidUsbSerial->text().trimmed()) &&
                                        (!otherAuto || m_uhidAutoConnectEnabled->isChecked()));
    m_bleAutoAttach->setEnabled(ble && (idle || bleDiscovery));
    m_mouseMode->setEnabled(!ble);
    m_unlockEnabled->setEnabled(adbFeatures);
    m_unlockPin->setEnabled(adbFeatures && m_unlockEnabled->isChecked());
    m_unlockWithSpace->setVisible(m_unlockEnabled->isChecked());
    m_unlockWithSpace->setEnabled(adbFeatures);
    const bool companionTarget = adbFeatures && !m_adb->text().trimmed().isEmpty() &&
        !adbTargetForCurrentDevice().isEmpty();
    m_transferFiles->setEnabled(adbFeatures && !m_closing && !m_companion.busy() &&
        !m_adb->text().trimmed().isEmpty() &&
        !adbTargetForCurrentDevice().isEmpty());
    m_companionInstall->setEnabled(companionTarget);
    m_companionOpen->setEnabled(companionTarget);
    m_companionCheck->setEnabled(companionTarget);
    updateCompanionStatus();
    updateClipboardAction();
    updateTray();
}

void MainWindow::updateConnectionStatus()
{
    if (!m_connectionStatus)
        return;
    const auto state = m_connections.state(m_selectedId);
    const bool connected = state == Session::State::Connected;
    const bool starting = state == Session::State::Starting;
    auto text = connected ? qtTrId("otg.status.connected_short") : m_connections.status(m_selectedId);
    const auto activeTarget = m_connections.activeUhidTarget(m_selectedId);
    if (!activeTarget.isEmpty()) text += QStringLiteral(" · ") +
        qtTrId(isUsbAdbSerial(activeTarget) ? "otg.uhid.usb_access" : "otg.uhid.wireless_access");
    m_connectionStatus->setText(QStringLiteral("● ") + text);
    if (connected) {
        m_connectionStatus->setStyleSheet(
            "QLabel { color: #1f7a35; background: #e6f4ea; border: 1px solid #b7dfc2; "
            "border-radius: 10px; padding: 3px 9px; }");
    } else if (starting) {
        m_connectionStatus->setStyleSheet(
            "QLabel { color: #8a5a00; background: #fff4ce; border: 1px solid #e6d58b; "
            "border-radius: 10px; padding: 3px 9px; }");
    } else {
        m_connectionStatus->setStyleSheet(
            "QLabel { color: #666666; background: #eeeeee; border: 1px solid #cccccc; "
            "border-radius: 10px; padding: 3px 9px; }");
    }
}

void MainWindow::updateCompanionStatus()
{
    const auto id = m_companion.busy() && m_companionDeviceId == m_selectedId ?
        QString("otg.companion.working") : m_companionStatuses.value(m_selectedId, "otg.companion.optional");
    m_companionStatus->setText(qtTrId(id.toLatin1().constData()));
}

QString MainWindow::adbTargetForCurrentDevice() const
{
    if (m_transport == "uhid") return adbActionTarget(m_selectedId, deviceProfile());
    return deviceAdbTarget(m_transport, m_adbSerial->text().trimmed(), m_serial->text().trimmed());
}

QString MainWindow::adbActionTarget(const QString &id, const QVariantMap &profile) const
{
    const auto transport = profile.value("transport", "usb").toString();
    if (transport == "uhid") {
        const auto active = m_connections.activeUhidTarget(id);
        return active.isEmpty() ? profileAdbTarget(profile) : active;
    }
    if (!profile.value("adbEnabled").toBool()) return {};
    const auto target = deviceAdbTarget(transport, profileAdbTarget(profile), profile.value("serial").toString());
    if (transport == "ble" && !isWirelessAdbTarget(target)) return {};
    return target;
}

void MainWindow::setupCompanion(CompanionApp::Action action)
{
    if (m_closing || m_scanning || m_companion.busy() || m_transport == "ble" ||
        m_connections.state(m_selectedId) != Session::State::Idle) return;
    saveSettings();
    m_companionDeviceId = m_selectedId;
    const auto target = adbTargetForCurrentDevice();
    m_companion.start(action, m_adb->text().trimmed(), target, m_companionApk->text().trimmed());
    updateEnabled();
}

void MainWindow::setupWirelessAdb()
{
    if (m_closing || m_scanning || m_companion.busy() ||
        m_connections.state(m_selectedId) != Session::State::Idle || m_wirelessAdbDialog)
        return;
    const auto adb = m_adb->text().trimmed();
    if (adb.isEmpty()) {
        m_adbStatus->setText(qtTrId("otg.wireless.status.adb_missing"));
        return;
    }

    auto *dialog = new WirelessAdbDialog(adb, this);
    dialog->setWindowModality(Qt::WindowModal);
    m_wirelessAdbDialog = dialog;
    connect(dialog, &WirelessAdbDialog::logMessage, this, [this](const QString &message) {
        appendLog(message);
    });
    const auto id = m_selectedId;
    connect(dialog, &WirelessAdbDialog::targetReady, this, [this, id](const QString &target) {
        if (m_closing || id != m_selectedId || m_connections.state(id) != Session::State::Idle) return;
        if (m_transport == "uhid") {
            m_uhidWirelessTarget->setText(target);
            m_uhidAccess->setCurrentIndex(m_uhidAccess->findData("wireless"));
        } else {
            m_adbSerial->setText(target);
        }
        m_adbEnabled->setChecked(true);
        m_adbStatus->setText(qtTrId("otg.adb.status.ready"));
        saveSettings();
        appendLog(qtTrId("otg.wireless.log.saved").arg(target));
        updateEnabled();
    });
    connect(dialog, &QDialog::finished, this, [this](int) {
        m_wirelessAdbDialog = nullptr;
        if (!m_closing)
            updateEnabled();
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    updateEnabled();
}

QSize MainWindow::virtualScreenSize() const
{
    const double multiplier = m_mouseMode->currentData() == "absolute" ? m_sensitivity->value() : 1.0;
    return QSize(qMax(1, qRound(m_width->value() / multiplier)),
                 qMax(1, qRound(m_height->value() / multiplier)));
}

void MainWindow::updateVirtualSize()
{
    const bool absolute = m_mouseMode->currentData() == "absolute";
    m_sensitivity->setEnabled(absolute);
    m_motionMultiplier->setEnabled(absolute);
    m_absolutePerAxis->setEnabled(absolute);
    const auto size = virtualScreenSize();
    m_virtualSize->setText(QString("%1 × %2%3").arg(size.width()).arg(size.height()).arg(
        absolute ? qtTrId("otg.size.absolute_note") : qtTrId("otg.size.relative_note")));
}

void MainWindow::regenerateTlsCertificate()
{
    if (!m_tls->isChecked() || m_closing || m_connections.busy() ||
        m_connections.serverState() != DeskflowServer::State::Idle)
        return;
    QString path = m_tlsCert->text().trimmed();
    if (path.isEmpty())
        path = defaultTlsClientCertificate();

    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, qtTrId("otg.tls.regenerate_confirm_title"),
                              qtTrId("otg.tls.regenerate_confirm").arg(path),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    // The confirmation runs a nested event loop; auto-connect may have started.
    if (m_closing || m_connections.busy() || m_connections.serverState() != DeskflowServer::State::Idle)
        return;
    QString error;
    if (!TlsCertificate::generate(path, m_tlsKeyLength->currentData().toInt(), &error)) {
        QMessageBox::critical(this, qtTrId("otg.tls.generate_failed_title"),
                              qtTrId("otg.tls.generate_failed").arg(error));
        return;
    }

    m_tlsCert->setText(path);
    saveSettings();
    const auto newFingerprint = TlsCertificate::fingerprint(path);
    QMessageBox::information(this, qtTrId("otg.tls.generated_title"),
                             qtTrId("otg.tls.generated").arg(newFingerprint));
}

void MainWindow::readDisplayFromAdb()
{
    if (!m_adbEnabled->isChecked() || m_adb->text().trimmed().isEmpty() ||
        adbTargetForCurrentDevice().isEmpty() || m_scanning || m_closing)
        return;
    m_scanning = true;
    ++m_request;
    const auto request = m_request;
    const auto adb = m_adb->text().trimmed();
    const auto target = adbTargetForCurrentDevice();
    setTranslatedProperty(m_deviceStatus, "text", "otg.device.reading");
    m_adbStatus->setText(qtTrId("otg.adb.status.checking"));
    updateEnabled();
    runCommand(this, adb, {"start-server"}, [this, request, adb, target](bool started, const QString &startOutput) {
        if (request != m_request) return;
        if (!started) {
            m_scanning = false;
            m_adbStatus->setText(qtTrId("otg.adb.status.failed"));
            appendLog(withProcessOutput(qtTrId("otg.log.adb.server_start_failed"), "ADB", startOutput));
            updateEnabled();
            return;
        }
        const auto readDisplay = [this, request, adb, target] {
            runCommand(this, adb, {"-s", target, "shell", "wm", "size"},
                       [this, request, adb, target](bool sizeOk, const QString &text) {
                if (request != m_request) return;
                const QSize size = sizeOk ? parseScreenSize(text) : QSize();
                if (!size.isValid()) {
                    m_scanning = false;
                    m_adbStatus->setText(qtTrId("otg.adb.status.failed"));
                    setTranslatedProperty(m_deviceStatus, "text", "otg.device.size_failed");
                    appendLog(withProcessOutput(qtTrId("otg.log.adb.size_failed").arg(target), "ADB", text));
                    updateEnabled();
                    return;
                }
                runCommand(this, adb, {"-s", target, "shell", "dumpsys", "input"},
                           [this, request, size](bool rotationOk, const QString &rotationText) {
                    if (request != m_request) return;
                    const int rotation = rotationOk ? parseRotation(rotationText) : -1;
                    QSize oriented = size;
                    if (rotation == 1 || rotation == 3) oriented.transpose();
                    m_width->setValue(oriented.width());
                    m_height->setValue(oriented.height());
                    m_scanning = false;
                    m_adbStatus->setText(qtTrId("otg.adb.status.ready"));
                    setTranslatedProperty(m_deviceStatus, "text", rotation < 0 ?
                        "otg.device.orientation_unknown" : m_transport == "uhid" ?
                        "otg.uhid.device_ready" : "otg.device.ready");
                    updateEnabled();
                });
            });
        };
        runCommand(this, adb, {"-s", target, "get-state"},
                   [this, request, adb, target, readDisplay](bool ok, const QString &state) {
            if (request != m_request) return;
            if (ok && state.trimmed() == "device") {
                readDisplay();
                return;
            }
            if (!isWirelessAdbTarget(target)) {
                m_scanning = false;
                m_adbStatus->setText(qtTrId("otg.adb.status.failed"));
                appendLog(withProcessOutput(qtTrId("otg.log.adb.target_unavailable").arg(target), "ADB", state));
                updateEnabled();
                return;
            }
            runCommand(this, adb, {"connect", target},
                       [this, request, adb, target, readDisplay](bool connected, const QString &output) {
                if (request != m_request) return;
                appendLog(withProcessOutput(qtTrId(connected ? "otg.log.adb.connect_requested" : "otg.log.adb.connect_failed").arg(target),
                                            "ADB", output));
                if (!connected) {
                    m_scanning = false;
                    m_adbStatus->setText(qtTrId("otg.adb.status.failed"));
                    updateEnabled();
                    return;
                }
                readDisplay();
            });
        });
    });
}

void MainWindow::startBleService()
{
    if (!m_startBleService || m_closing || m_stoppingBleService || m_companion.busy() || m_connections.busy())
        return;
    storeDevice();
    m_discoverMultipleBle = true;
    continueBleDiscovery();
}

void MainWindow::continueBleDiscovery()
{
    if (!m_discoverMultipleBle || m_closing || m_stoppingBleService ||
        m_connections.reservedBleSessions() >= OTG_BLE_MAX_PHONES ||
        (!m_discoveryId.isEmpty() && m_connections.state(m_discoveryId) != Session::State::Idle))
        return;
    if (m_discoveryId.isEmpty()) m_discoveryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_connections.addDevice(m_discoveryId);
    auto options = connectionOptions({{"transport", "ble"}, {"phone", "discover-" + m_discoveryId}});
    options.bleDiscovery = true;
    m_connections.start(m_discoveryId, options);
}

void MainWindow::refreshBleServiceStatus()
{
    if (!m_adapter || !m_bleServiceStatus->isVisible() || isMinimized() || m_closing ||
        m_stoppingBleService || m_bleStatusPending)
        return;
    const auto adapter = m_adapter->text().trimmed();
    static const QRegularExpression adapterPattern("^hci[0-9]{1,5}$");
    if (!adapterPattern.match(adapter).hasMatch() || adapter.mid(3).toUInt() >= 65535) {
        setTranslatedProperty(m_bleServiceStatus, "text", "otg.ble.status_invalid_adapter");
        return;
    }
    const auto program = bridgeExecutable();
    if (program.isEmpty()) {
        setTranslatedProperty(m_bleServiceStatus, "text", "otg.ble.status_query_failed");
        return;
    }
    m_bleStatusPending = true;
    const auto generation = m_bleStatusGeneration;
    runCommand(this, program, {"--ble-status", "--adapter", adapter},
               [this, generation](bool ok, const QString &output) {
        m_bleStatusPending = false;
        if (m_closing || m_stoppingBleService) return;
        if (generation != m_bleStatusGeneration) {
            refreshBleServiceStatus();
            return;
        }
        const auto status = QJsonDocument::fromJson(output.toUtf8()).object();
        const auto available = status.value("available");
        const auto idle = status.value("idle");
        const auto ready = status.value("ready");
        m_bleServiceIdle = ok && status.value("version").toInt() == 2 && available.toBool() && idle.toBool();
        const char *id = "otg.ble.status_query_failed";
        if (status.value("version").toInt() == 2 && available.isBool()) {
            if (!available.toBool())
                id = "otg.ble.status_unavailable";
            else if (ok && idle.isBool() && ready.isBool())
                id = ready.toBool() ? "otg.ble.status_ready" :
                     idle.toBool() ? "otg.ble.status_idle" : "otg.ble.status_in_use";
        }
        setTranslatedProperty(m_bleServiceStatus, "text", id);
        updateBleServiceControls(!m_connections.busy() && m_connections.serverState() == DeskflowServer::State::Idle &&
                                 !m_closing && !m_companion.busy());
    }, 7000);
}

void MainWindow::updateBleServiceControls(bool sharedEditable)
{
    if (!m_bleServiceStatus) return;
    m_startBleService->setEnabled(sharedEditable);
    if (m_bleMulti) m_bleMulti->setEnabled(sharedEditable && !m_connections.hasBleProvider());
    if (m_adapter) {
        m_adapter->setEnabled(sharedEditable);
        m_bleLegacyHandles->setEnabled(sharedEditable);
    } else if (!m_stoppingBleService) {
        const auto statusId = m_connections.bleProviderStatusId();
        setTranslatedProperty(m_bleServiceStatus, "text", statusId ? statusId : "otg.ble.status_stopped");
    }
    m_stopBleService->setEnabled(!m_closing && !m_stoppingBleService &&
        (m_connections.activeBleSessions() || m_connections.hasBleProvider() || m_bleServiceIdle));
}

bool MainWindow::confirmSharedBleService(const QString &adapter)
{
    return QMessageBox::warning(this, qtTrId("otg.ble.confirm_title"),
        qtTrId("otg.ble.confirm").arg(adapter), QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel) == QMessageBox::Ok;
}

void MainWindow::acceptBleIdentity(const QString &sourceId, const QString &identity)
{
    if (m_closing || m_stoppingBleService) return;
    storeDevice();
    QString id;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        if (it->value("transport") == "ble" &&
            !it->value("bleIdentity").toString().compare(identity, Qt::CaseInsensitive)) {
            id = it.key();
            break;
        }
    }
    if (id.isEmpty()) {
        const auto source = m_records.value(sourceId);
        id = source.isEmpty() || source.value("bleIdentity").toString().isEmpty() ?
            sourceId : QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!m_connections.bindBleDiscovery(sourceId, id)) return;
    m_session = m_connections.session(m_selectedId);
    if (!m_records.contains(id)) m_unsavedDevices.insert(id);
    auto &profile = m_records[id];
    if (profile.value("bleIdentity").toString().isEmpty()) {
        const QString base = bleScreenName(identity);
        QString name = base;
        QStringList screens{m_computer->text().trimmed()};
        for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
            if (it.key() != id) screens << it->value("phone").toString();
        for (int suffix = 2; screens.contains(name, Qt::CaseInsensitive); ++suffix)
            name = base + '-' + QString::number(suffix);
        profile["transport"] = "ble";
        profile["bleIdentity"] = identity;
        profile["alias"] = profile["phone"] = name;
    }
    if (m_discoveryId == sourceId) m_discoveryId.clear();
    m_pendingBleIdentities[id] = identity;
    if (m_selectedId.isEmpty()) selectDevice(id);
    else if (id == m_selectedId) applyDeviceProfile(profile);
    persistDevices();
    updateDeviceList();
    if (profile.value("bleAutoAttach", true).toBool())
        m_connections.attachBle(id, connectionOptions(profile));
    updateEnabled();
    if (m_discoverMultipleBle) QTimer::singleShot(0, this, &MainWindow::continueBleDiscovery);
}

void MainWindow::connectDevice()
{
    if (!m_session || !supportsTransport(m_transport) || m_closing || m_scanning || m_stoppingBleService || m_companion.busy() ||
        m_connections.state(m_selectedId) != Session::State::Idle)
        return;
    saveSettings();
    auto options = connectionOptions(deviceProfile());
    if (options.transport == "uhid" && options.adbSerial.isEmpty()) return;
    if (options.transport == "ble" && (!m_startBleService || options.blePeer.isEmpty()))
        return;
    if (options.bridge.isEmpty()) {
        m_sessionStatus = false;
        setTranslatedProperty(m_status, "text", "otg.error.bridge_not_found");
        updateTray();
        return;
    }
    storeDevice();
    m_pendingProfiles[m_selectedId] = deviceProfile();
    if (options.transport == "usb") m_autoConnect.suppressSerial(options.serial);
    else if (options.transport == "uhid" && m_uhidAutoConnectEnabled->isChecked() && isUsbAdbSerial(options.adbSerial))
        m_adbAutoConnect.suppressSerial(options.adbSerial);
    m_connections.start(m_selectedId, options);
}

void MainWindow::stopBleService()
{
    if (m_closing || !m_stopBleService->isEnabled()) return;
    if (QMessageBox::warning(this, qtTrId("otg.ble.stop_service"), qtTrId("otg.ble.stop_confirm"),
                             QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok ||
        m_closing || !m_stopBleService->isEnabled())
        return;
    m_stoppingBleService = true;
    m_discoverMultipleBle = false;
    ++m_bleStatusGeneration;
    setTranslatedProperty(m_bleServiceStatus, "text", "otg.ble.status_stopping");
    updateEnabled();
    m_connections.stopBle();
    finishBleServiceStop();
}

void MainWindow::finishBleServiceStop()
{
    if (!m_stoppingBleService || m_connections.activeBleSessions() || m_connections.hasBleProvider() ||
        m_bleStopQueryPending)
        return;
    const auto complete = [this](bool ok, const QString &detail = {}) {
        appendLog(qtTrId(ok ? "otg.ble.stopped" : "otg.ble.stop_failed") + detail);
        m_stoppingBleService = false;
        m_bleServiceIdle = false;
        setTranslatedProperty(m_bleServiceStatus, "text", ok ? "otg.ble.status_stopped" : "otg.ble.status_query_failed");
        updateEnabled();
        if (!ok) refreshBleServiceStatus();
        if (m_closing) QTimer::singleShot(0, this, &QWidget::close);
    };
    if (!m_adapter) { complete(true); return; }
    const auto adapter = m_adapter->text().trimmed();
    m_bleStopQueryPending = true;
    runCommand(this, bridgeExecutable(), {"--ble-stop-service", "--adapter", adapter},
               [this, complete](bool stopped, const QString &detail) {
        m_bleStopQueryPending = false;
        complete(stopped, stopped ? QString() : detail);
    }, 17000);
}

QVariantMap MainWindow::deviceProfile() const
{
    const auto saved = m_records.value(m_selectedId);
    const bool uhidUsbSerialLocked = saved.value("uhidUsbSerialLocked").toBool();
    const auto uhidUsbSerial = uhidUsbSerialLocked ? saved.value("uhidUsbSerial").toString() :
                               m_uhidUsbSerial->text().trimmed();
    return {{"serial", m_serial->text().trimmed()},
            {"adbSerial", m_adbSerial->text().trimmed()},
            {"uhidUsbSerial", uhidUsbSerial},
            {"uhidUsbSerialLocked", uhidUsbSerialLocked},
            {"uhidWirelessTarget", m_uhidWirelessTarget->text().trimmed()},
            {"uhidAccess", m_uhidAccess->currentData()},
            {"adbEnabled", m_adbEnabled->isChecked()},
            {"alias", m_alias->text().trimmed()},
            {"transport", m_transport},
            {"bleIdentity", saved.value("bleIdentity").toString()},
            {"bleAutoAttach", m_bleAutoAttach->isChecked()},
            {"autoConnect", m_autoConnectEnabled->isChecked()},
            {"uhidUsbAutoConnect", m_uhidAutoConnectEnabled->isChecked()},
            {"originalWidth", m_width->value()}, {"originalHeight", m_height->value()},
            {"sensitivityMultiplier", m_sensitivity->value()},
            {"motionMultiplier", m_motionMultiplier->value()},
            {"phone", m_phone->text()},
                {"mouseMode", m_mouseMode->currentData()}, {"audio", m_audio->isChecked()},
                {"absolutePerAxis", m_absolutePerAxis->isChecked()},
                {"audioGainDb", m_audioGain->value()},
                  {"middleButtonAction", m_middleButtonAction->currentData()},
                  {"rightButtonAction", m_rightButtonAction->currentData()},
                  {"sideButton1Action", m_sideButton1Action->currentData()},
                  {"sideButton2Action", m_sideButton2Action->currentData()},
                  {"sideSwipeDistancePercent", m_sideSwipeDistance->value()},
                  {"unlockEnabled", m_unlockEnabled->isChecked()}, {"unlockPin", m_unlockPin->text()},
                  {"unlockWithSpace", m_unlockWithSpace->isChecked()},
                 {"clipboard", m_clipboard->isChecked()}, {"autoRotate", m_autoRotate->isChecked()}};
}

void MainWindow::applyDeviceProfile(const QVariantMap &profile)
{
    m_loadingDevice = true;
    ++m_request;
    m_scanning = false;
    m_serial->setText(profile.value("serial").toString());
    m_adbSerial->setText(profile.value("adbSerial").toString());
    m_adbEnabled->setChecked(profile.value("adbEnabled", false).toBool());
    m_width->setValue(profile.value("originalWidth", 1080).toInt());
    m_height->setValue(profile.value("originalHeight", 1920).toInt());
    m_sensitivity->setValue(profile.value("sensitivityMultiplier", 1.0).toDouble());
    m_motionMultiplier->setValue(profile.value("motionMultiplier", 1.0).toDouble());
    m_phone->setText(profile.value("phone", "android").toString());
    m_alias->setText(profile.value("alias", m_phone->text()).toString());
    const QString transport = profile.value("transport", "usb").toString();
    setProfileTransport(transport);
    m_uhidUsbSerial->setText(profile.value("uhidUsbSerial").toString());
    m_uhidWirelessTarget->setText(profile.value("uhidWirelessTarget").toString());
    m_uhidAccess->setCurrentIndex(profile.value("uhidAccess") == "wireless" ? 1 : 0);
    if (m_transport == "uhid") m_adbEnabled->setChecked(true);
    m_bleAutoAttach->setChecked(profile.value("bleAutoAttach", true).toBool());
    const QSignalBlocker audioBlocker(m_audio);
    m_audio->setChecked(profile.value("audio", false).toBool());
    m_audioGain->setValue(storedAudioGain(profile.value("audioGainDb")));
    const QSignalBlocker clipboardBlocker(m_clipboard);
    m_clipboard->setChecked(profile.value("clipboard", false).toBool());
    const QSignalBlocker autoRotateBlocker(m_autoRotate);
    m_autoRotate->setChecked(profile.value("autoRotate", false).toBool());
    m_autoConnectEnabled->setChecked(profile.value("autoConnect", false).toBool());
    m_uhidAutoConnectEnabled->setChecked(profile.value("uhidUsbAutoConnect", false).toBool());
    m_mouseMode->setCurrentIndex(qMax(0, m_mouseMode->findData(profile.value("mouseMode", "absolute"))));
    m_absolutePerAxis->setChecked(profile.value("absolutePerAxis", false).toBool());
    m_middleButtonAction->setCurrentIndex(qMax(0, m_middleButtonAction->findData(
        profile.value("middleButtonAction", "home"))));
    m_rightButtonAction->setCurrentIndex(qMax(0, m_rightButtonAction->findData(
        profile.value("rightButtonAction", "back"))));
    m_sideButton1Action->setCurrentIndex(qMax(0, m_sideButton1Action->findData(
        profile.value("sideButton1Action", "swipe-up"))));
    m_sideButton2Action->setCurrentIndex(qMax(0, m_sideButton2Action->findData(
        profile.value("sideButton2Action", "swipe-down"))));
    m_sideSwipeDistance->setValue(profile.value("sideSwipeDistancePercent", 17).toInt());
    m_unlockEnabled->setChecked(profile.value("unlockEnabled", false).toBool());
    m_unlockPin->setText(profile.value("unlockPin").toString());
    m_unlockWithSpace->setChecked(profile.value("unlockWithSpace", true).toBool());
    if (m_transport == "ble") m_mouseMode->setCurrentIndex(0);
    m_deviceStatus->setText(!supportsTransport(m_transport) ? qtTrId("otg.multi.transport_unavailable") :
                            m_transport == "ble" ? qtTrId(bleSetupHint) : QString());
    m_loadingDevice = false;
}

bool MainWindow::lockConnectedUhidUsbSerial(const QString &id)
{
    auto profile = m_records.find(id);
    if (profile == m_records.end() || profile->value("uhidUsbSerialLocked").toBool()) return false;
    const auto serial = m_connections.activeUhidTarget(id);
    if (!isUsbAdbSerial(serial)) return false;

    (*profile)["uhidUsbSerial"] = serial;
    (*profile)["uhidUsbSerialLocked"] = true;
    if (id == m_selectedId) {
        const QSignalBlocker blocker(m_uhidUsbSerial);
        m_uhidUsbSerial->setText(serial);
    }
    return true;
}

void MainWindow::rememberDevice(const QVariantMap &profile)
{
    const auto key = autoConnectKey(profile);
    if (key.second.isEmpty()) return;
    m_profiles.insert(key, profile);
    QSettings settings;
    settings.beginWriteArray("rememberedDevices", m_profiles.size());
    int index = 0;
    for (const auto &profile : m_profiles) {
        settings.setArrayIndex(index++);
        settings.setValue("profile", profile);
    }
    settings.endArray();
    updateAutoConnect();
}

void MainWindow::saveSharedBleSettings(QSettings &settings) const
{
    if (m_bleMulti) settings.setValue("sharedBle/multiDevice", m_bleMulti->isChecked());
    if (!m_adapter) return;
    settings.setValue("sharedBle/adapter", m_adapter->text().trimmed());
    settings.setValue("sharedBle/legacyHandles", m_bleLegacyHandles->isChecked());
}

void MainWindow::saveSettings()
{
    storeDevice();
    persistDevices();
    QSettings settings;
    settings.setValue("adb", m_adb->text());
    const auto apk = m_companionApk->text().trimmed();
    // Do not persist an installation-specific path for the default APK (notably
    // the temporary mount point of an AppImage). Explicit custom APKs stay saved.
    settings.setValue("companionApk", apk == CompanionApp::bundledApk() ? QString() : apk);
    settings.setValue("deskflow", m_deskflow->text());
    settings.setValue("deskflowConfigFile", m_deskflowConfig->text().trimmed());
    saveSharedBleSettings(settings);
    settings.setValue("computer", m_computer->text());
    settings.setValue("host", m_host->text());
    settings.setValue("port", m_port->value());
    settings.setValue("manageServer", m_manage->isChecked());
    settings.setValue("tls", m_tls->isChecked());
    settings.setValue("tlsKeyLength", m_tlsKeyLength->currentData());
    settings.setValue("tlsCertificate", m_tlsCert->text());
    settings.setValue("tlsFingerprint", m_tlsFingerprint->text());
    settings.setValue("tlsFingerprintFile", m_tlsFingerprintFile->text());
    settings.remove("direction");
    if (!m_session || m_unsavedDevices.contains(m_selectedId)) return;
    settings.setValue("audio", m_audio->isChecked());
    settings.setValue("audioGainDb", m_audioGain->value());
    settings.setValue("clipboard", m_clipboard->isChecked());
    settings.setValue("autoRotate", m_autoRotate->isChecked());
    settings.setValue("serial", m_serial->text());
    settings.setValue("adbSerial", profileAdbTarget(deviceProfile()));
    settings.setValue("transport", m_transport);
    settings.setValue("originalWidth", m_width->value());
    settings.setValue("originalHeight", m_height->value());
    settings.setValue("sensitivityMultiplier", m_sensitivity->value());
    settings.setValue("motionMultiplier", m_motionMultiplier->value());
    const auto size = virtualScreenSize();
    settings.setValue("width", size.width());
    settings.setValue("height", size.height());
    settings.setValue("phone", m_phone->text());
    settings.setValue("mouseMode", m_mouseMode->currentData());
    settings.setValue("absolutePerAxis", m_absolutePerAxis->isChecked());
    settings.setValue("middleButtonAction", m_middleButtonAction->currentData());
    settings.setValue("rightButtonAction", m_rightButtonAction->currentData());
    settings.setValue("sideButton1Action", m_sideButton1Action->currentData());
    settings.setValue("sideButton2Action", m_sideButton2Action->currentData());
    settings.setValue("sideSwipeDistancePercent", m_sideSwipeDistance->value());
    settings.setValue("autoConnect", m_autoConnectEnabled->isChecked());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closing)
        ++m_request; // Invalidate outstanding discovery/ADB callbacks before child teardown.
    saveSettings();
    if (!m_closing && m_tray->isVisible() && trayAvailable()) {
        event->ignore();
        hide();
        return;
    }
    m_closing = true;
    m_screenshot.cancel();
    m_manualClipboard.cancel();
    if (m_transferDialog) m_transferDialog->reject();
    if (m_connections.settled() && !m_stoppingBleService && !m_companion.busy() && !m_transferDialog &&
        !m_screenshot.busy() && !m_manualClipboard.busy()) {
        m_autoConnect.setEnabled(false);
        m_adbAutoConnect.setEnabled(false);
        m_tray->hide();
        event->accept();
        if (!m_exitEmitted) {
            m_exitEmitted = true;
            emit exitReady();
        }
        return;
    }
    event->ignore();
    if (!m_connections.settled()) m_connections.shutdown();
    updateEnabled();
}
