// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "clipboard.h"
#include "i18n.h"
#include "build_version.h"
#include "system_theme.h"
#include "tls_certificate.h"
#include "transfer_dialog.h"
#include "process_utils.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSettings>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QSystemTrayIcon>
#include <QPalette>
#include <QStyleHints>
#include <QListWidget>
#include <QTabWidget>
#include <QWheelEvent>
#include <QTcpServer>
#include <QtEndian>
#include <QtTest>
#include <memory>

class TrayTestWindow : public MainWindow {
public:
    bool available = true;
protected:
    bool trayAvailable() const override { return available; }
};

static QString testPeerPath()
{
    return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}

class AutoConnectTestWindow : public TrayTestWindow {
public:
    AutoConnectTestWindow()
    {
        // These fixtures exercise AOA unless a test explicitly selects UHID.
        // Production fresh Windows profiles are tested separately below.
        auto *transport = findChild<QComboBox *>("transportSelector");
        transport->setCurrentIndex(transport->findData(QSettings().value("transport", "usb")));
        if (transport->currentData() == "usb")
            findChild<QCheckBox *>("adbEnabled")->setChecked(QSettings().value("adbEnabled", false).toBool());
    }
protected:
    QString bridgeExecutable() const override
    {
        return testPeerPath();
    }
};

#ifdef OTG_GUI_HAVE_SCRCPY
static QByteArray clipboardReply(const QByteArray &text)
{
    QByteArray packet(5, '\0');
    qToBigEndian(quint32(text.size()), packet.data() + 1);
    return packet + text;
}

static QByteArray clipboardAck()
{
    QByteArray ack(9, '\0');
    ack[0] = 1;
    qToBigEndian(quint64(1), ack.data() + 1);
    return ack;
}
#endif

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void uhidDefaults()
    {
#ifdef OTG_GUI_HAVE_UHID
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings; settings.setValue("adb", "");
        MainWindow window;
        auto *transport = window.findChild<QComboBox *>("transportSelector");
#ifdef Q_OS_WIN
        QCOMPARE(transport->currentData().toString(), "uhid");
#else
        QCOMPARE(transport->currentData().toString(), "usb");
#endif
        window.findChild<QAction *>("trayQuit")->trigger();
#endif
    }

    void uhidDiscoveryAndSettings()
    {
#ifdef OTG_GUI_HAVE_UHID
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        const auto peer = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
        settings.setValue("adb", peer);
        settings.setValue("transport", "uhid");
        settings.setValue("serial", "OLD_AOA_SERIAL");
        settings.setValue("adbEnabled", false);
        settings.setValue("manageServer", false);
        const auto logPath = dir.filePath("commands.jsonl");
        qputenv("OTG_TEST_UHID_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_UHID_LOG"); });
        {
            AutoConnectTestWindow window; window.show();
            auto *transport = window.findChild<QComboBox *>("transportSelector");
            auto *target = window.findChild<QLineEdit *>("adbSerial");
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *enabled = window.findChild<QCheckBox *>("adbEnabled");
            auto *connect = window.findChild<QPushButton *>("connectButton");
            QVERIFY(transport && target && serial && enabled && connect);
            QTRY_COMPARE(target->text(), "TEST123");
            QTRY_COMPARE(window.findChild<QSpinBox *>("screenWidth")->value(), 1600);
            QCOMPARE(serial->text(), "OLD_AOA_SERIAL");
            QVERIFY(serial->isHidden());
            QVERIFY(enabled->isChecked() && !enabled->isEnabled());
            QVERIFY(!window.findChild<QCheckBox *>("autoConnectEnabled")->isEnabled());
            auto *wiredAuto = window.findChild<QCheckBox *>("uhidUsbAutoConnect");
            QVERIFY(wiredAuto->isEnabled() && !wiredAuto->isHidden() && !wiredAuto->isChecked());
            QVERIFY(window.findChild<QCheckBox *>("autoRotate")->isEnabled());
            target->clear(); QVERIFY(!connect->isEnabled());
            target->setText("MANUAL_ADB_TARGET"); QVERIFY(connect->isEnabled());
            auto *language = window.findChild<QComboBox *>("languageSelector");
            language->setCurrentIndex(language->findData("zh_CN"));
            QVERIFY(transport->currentText().contains("UHID"));
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(transport->currentData().toString(), "uhid");
            const auto screenshot = qEnvironmentVariable("OTG_TEST_UHID_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                QCoreApplication::processEvents();
                QVERIFY(window.grab().save(screenshot));
            }
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        QCOMPARE(settings.value("transport").toString(), "uhid");
        QCOMPARE(settings.value("adbSerial").toString(), "MANUAL_ADB_TARGET");
        settings.beginReadArray("devices"); settings.setArrayIndex(0);
        QCOMPARE(settings.value("profile").toMap().value("transport").toString(), "uhid");
        settings.endArray();
        {
            AutoConnectTestWindow restored;
            QCOMPARE(restored.findChild<QComboBox *>("transportSelector")->currentData().toString(), "uhid");
            QTRY_VERIFY(restored.findChild<QComboBox *>("deviceSelector")->count() > 1);
            QCOMPARE(restored.findChild<QLineEdit *>("adbSerial")->text(), "MANUAL_ADB_TARGET");
            restored.findChild<QAction *>("trayQuit")->trigger();
        }
        QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
        const auto commands = log.readAll();
        QVERIFY(commands.contains("devices"));
        QVERIFY(!commands.contains("--usb-scan") && !commands.contains("get-devpath"));
#endif
    }

    void commandOwnerDestruction_data()
    {
        QTest::addColumn<bool>("started");
        QTest::newRow("starting") << false;
        QTest::newRow("running") << true;
    }
    void commandOwnerDestruction()
    {
        QFETCH(bool, started);
        // QWidget destroys its children before QObject clears QPointer guards.
        // A QProcess destroyed after a control can still emit finished(). Its
        // completion callback must not run against the partially destroyed UI.
        auto owner = std::make_unique<QWidget>();
        auto *control = new QObject(owner.get());
        bool controlDestroyed = false, callback = false;
        connect(control, &QObject::destroyed, this, [&] { controlDestroyed = true; });
        runCommand(owner.get(), QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                   ".exe"
#endif
                   , {"--gui", "--serial", "slow"},
                   [&](bool, const QString &) { callback = true; });
        auto *process = owner->findChild<QProcess *>();
        QVERIFY(process);
        if (started) QVERIFY(process->waitForStarted());
        QPointer<QProcess> child(process);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("QProcess: Destroyed while process .* is still running\\."));
        owner.reset();
        QVERIFY(controlDestroyed);
        QVERIFY(!child);
        QVERIFY(!callback);
        QCoreApplication::processEvents();
        QVERIFY(!callback);
    }
    void rightButtonSettings()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "zh_CN");
        settings.setValue("manageServer", false);
        {
            AutoConnectTestWindow window;
            QCoreApplication::processEvents();
            auto *right = window.findChild<QComboBox *>("rightButtonAction");
            auto *middle = window.findChild<QComboBox *>("middleButtonAction");
            auto *distance = window.findChild<QSpinBox *>("sideSwipeDistance");
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *language = window.findChild<QComboBox *>("languageSelector");
            auto *adbEnabled = window.findChild<QCheckBox *>("adbEnabled");
            auto *unlock = window.findChild<QCheckBox *>("unlockEnabled");
            auto *unlockWithSpace = window.findChild<QCheckBox *>("unlockWithSpace");
            auto *pin = window.findChild<QLineEdit *>("unlockPin");
            QVERIFY(right && middle && distance && list && language && adbEnabled && unlock && unlockWithSpace && pin);
            QVERIFY(unlockWithSpace->isHidden());
            QVERIFY(unlockWithSpace->isChecked());
            QCOMPARE(unlockWithSpace->text(), QStringLiteral("用空格键代替上滑"));
            QCOMPARE(right->currentData().toString(), "back");
            QCOMPARE(distance->value(), 17);
            distance->setValue(25);
            right->setCurrentIndex(right->findData("back"));
            QCOMPARE(middle->currentData().toString(), "home");
            QCOMPARE(right->currentText(), "安卓返回键（Win+Backspace）");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(right->currentText(), "Android Back (Win+Backspace)");
            QCOMPARE(unlockWithSpace->text(), QStringLiteral("Use Space instead of swiping"));
            QCOMPARE(right->currentData().toString(), "back");
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QCOMPARE(list->count(), 2);
            right->setCurrentIndex(right->findData("normal"));
            middle->setCurrentIndex(middle->findData("home"));
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(right->currentData().toString(), "back");
            QCOMPARE(middle->currentData().toString(), "home");
            adbEnabled->setChecked(true);
            unlock->setChecked(true);
            QVERIFY(!unlockWithSpace->isHidden());
            QVERIFY(unlockWithSpace->isEnabled());
            unlockWithSpace->setChecked(false);
            pin->setText("1234");
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        auto *right = restored.findChild<QComboBox *>("rightButtonAction");
        auto *middle = restored.findChild<QComboBox *>("middleButtonAction");
        auto *distance = restored.findChild<QSpinBox *>("sideSwipeDistance");
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        QCOMPARE(right->currentData().toString(), "back");
        QCOMPARE(middle->currentData().toString(), "home");
        QCOMPARE(distance->value(), 25);
        QCOMPARE(restored.findChild<QCheckBox *>("unlockEnabled")->isChecked(), true);
        auto *unlockWithSpace = restored.findChild<QCheckBox *>("unlockWithSpace");
        QVERIFY(unlockWithSpace && !unlockWithSpace->isChecked());
        QCOMPARE(restored.findChild<QLineEdit *>("unlockPin")->text(), QStringLiteral("1234"));
        list->setCurrentRow(1);
        QVERIFY(unlockWithSpace->isChecked());
        QCOMPARE(right->currentData().toString(), "normal");
        QCOMPARE(middle->currentData().toString(), "home");
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void screenshotClipboard()
    {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Use the isolated offscreen clipboard");
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.png");
        qputenv("OTG_TEST_SHOT_FILE", path.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_SHOT_FILE"); qunsetenv("OTG_TEST_SHOT_MODE"); });
        QImage original(1450, 1024, QImage::Format_RGB32);
        QRandomGenerator random(123);
        for (int y = 0; y < original.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(original.scanLine(y));
            for (int x = 0; x < original.width(); ++x) row[x] = 0xff000000u | random.generate();
        }
        QVERIFY(original.save(path, "PNG"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll(); file.close();
        QVERIFY(bytes.size() > 4 * 1024 * 1024); // Larger than the existing text runner's limit.
        PhoneScreenshot shot;
        QSignalSpy done(&shot, &PhoneScreenshot::completed);
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText("original");
        QVERIFY(!shot.start(testPeerPath(), ""));
        QVERIFY(shot.start(testPeerPath(), "USB-ID"));
        QVERIFY(!shot.start(testPeerPath(), "OTHER-ID"));
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.takeFirst().first().toString(), "otg.screenshot.copied");
        QVERIFY(clipboard->mimeData()->hasImage());
        QCOMPARE(clipboard->mimeData()->data("image/png"), bytes);
        QCOMPARE(clipboard->image().size(), original.size());
        QCOMPARE(clipboard->image().pixel(901, 700), original.pixel(901, 700));

        auto expectFailure = [&](const char *mode, const char *reason, int timeout = 20000) {
            qputenv("OTG_TEST_SHOT_MODE", mode);
            QVERIFY(shot.start(testPeerPath(), "[::1]:5555", timeout));
            QTRY_COMPARE(done.size(), 1);
            QCOMPARE(done.takeFirst().first().toString(), QString::fromLatin1(reason));
            QCOMPARE(clipboard->text(), "keep me");
        };
        clipboard->setText("keep me");
        expectFailure("fail", "otg.screenshot.adb_failed"); // Valid PNG, unsuccessful process.
        expectFailure("unauthorized", "otg.screenshot.unauthorized");
        expectFailure("hang", "otg.screenshot.timeout", 200);
        QVERIFY(file.resize(bytes.size() - 12)); // Complete pixel data, missing IEND.
        expectFailure("", "otg.screenshot.invalid");
        QVERIFY(file.resize(32 * 1024 * 1024 + 1));
        expectFailure("", "otg.screenshot.limit");
    }

    void screenshotDeviceAndTray()
    {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Use the isolated offscreen clipboard");
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", testPeerPath()); settings.setValue("transport", "usb");
        settings.setValue("manageServer", false);
        settings.beginWriteArray("devices", 2);
        settings.setArrayIndex(0); settings.setValue("id", "initial");
        settings.setValue("profile", QVariantMap{{"phone", "one"}, {"serial", "TEST123"}, {"adbSerial", "USB-ID"}});
        settings.setArrayIndex(1); settings.setValue("id", "second");
        settings.setValue("profile", QVariantMap{{"phone", "two"}, {"adbEnabled", true}, {"adbSerial", "[::1]:5555"},
                                                 {"neighbor", "one"}});
        settings.endArray();
        const auto path = dir.filePath("frame.png"), logPath = dir.filePath("commands.jsonl");
        QImage image(19, 9, QImage::Format_RGB32); image.fill(Qt::green);
        QVERIFY(image.save(path, "PNG"));
        qputenv("OTG_TEST_SHOT_FILE", path.toUtf8()); qputenv("OTG_TEST_UHID_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_SHOT_FILE"); qunsetenv("OTG_TEST_SHOT_MODE"); qunsetenv("OTG_TEST_UHID_LOG"); });
        AutoConnectTestWindow window; window.show();
        auto *button = window.findChild<QPushButton *>("copyScreenshot");
        auto *menu = window.findChild<QMenu *>("trayScreenshots");
        auto *selector = window.findChild<QComboBox *>("deviceSelector");
        auto *target = window.findChild<QLineEdit *>("adbSerial");
        QTRY_VERIFY(selector->count() > 1);
        QCOMPARE(menu->actions().size(), 2);
        QVERIFY(!button->isEnabled());
        auto *other = menu->actions().last();
        QCOMPARE(other->data().toString(), "second"); QVERIFY(other->isEnabled());
        window.close(); // Hidden to tray, no bridge session required.
        other->trigger();
        QTRY_VERIFY(window.statusBar()->currentMessage().contains("two") && other->isEnabled());
        QCOMPARE(QGuiApplication::clipboard()->image().size(), image.size());
        QCOMPARE(target->text(), "USB-ID"); // The tray action did not switch the selected device.

        window.show();
        auto *enabled = window.findChild<QCheckBox *>("adbEnabled");
        enabled->setChecked(true);
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        for (const auto *mode : {"uhid", "ble"}) {
            const auto index = transport->findData(mode);
            if (index < 0) continue;
            transport->setCurrentIndex(index); target->clear();
            QVERIFY(!button->isEnabled()); // Never fall back to the stale AOA serial or BLE peer.
            target->setText("[::1]:5555"); QVERIFY(button->isEnabled());
        }
        transport->setCurrentIndex(transport->findData("usb"));
        target->setText("USB-ID");
        auto *connectButton = window.findChild<QPushButton *>("connectButton");
        QTRY_VERIFY(connectButton->isEnabled()); connectButton->click();
        auto *manager = window.findChild<ConnectionManager *>();
        QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
        QVERIFY(button->isEnabled()); button->click();
        QTRY_VERIFY(button->isEnabled());
        QCOMPARE(manager->state("initial"), Session::State::Connected);
        QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
        QStringList targets;
        for (const auto &line : log.readAll().split('\n')) {
            const auto args = QJsonDocument::fromJson(line).array();
            if (args.contains("exec-out")) targets.append(args[1].toString());
        }
        QCOMPARE(targets, (QStringList{"[::1]:5555", "USB-ID"}));

        qputenv("OTG_TEST_SHOT_MODE", "hang");
        QGuiApplication::clipboard()->setText("keep on exit");
        button->click(); QVERIFY(!button->isEnabled());
        QSignalSpy quit(&window, &MainWindow::exitReady);
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(quit.size(), 1);
        QCOMPARE(QGuiApplication::clipboard()->text(), "keep on exit");
    }

#ifdef OTG_GUI_HAVE_SCRCPY
    void manualClipboardWindow()
    {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Use the isolated offscreen clipboard");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", testPeerPath());
        settings.setValue("transport", "usb");
        settings.setValue("language", "en");
        settings.setValue("manageServer", false);
        settings.beginWriteArray("devices", 2);
        settings.setArrayIndex(0); settings.setValue("id", "initial");
        settings.setValue("profile", QVariantMap{{"phone", "one"}, {"serial", "TEST123"}, {"adbSerial", "USB-ID"}});
        settings.setArrayIndex(1); settings.setValue("id", "second");
        settings.setValue("profile", QVariantMap{{"phone", "two"}, {"adbEnabled", true}, {"adbSerial", "[::1]:5555"}});
        settings.endArray();

        QTcpServer tunnel;
        QVERIFY(tunnel.listen(QHostAddress::LocalHost));
        const auto logPath = dir.filePath("commands.jsonl");
        qputenv("OTG_TEST_CLIPBOARD_PORT", QByteArray::number(tunnel.serverPort()));
        qputenv("OTG_TEST_UHID_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_CLIPBOARD_PORT");
            qunsetenv("OTG_TEST_UHID_LOG");
        });
        // The loopback listener can accept a connection before the fake
        // server reaches main(). Wait for each launch before replying, so
        // completing or cancelling an exchange cannot erase its launch record.
        const auto serverLaunchCount = [&logPath] {
            QFile log(logPath);
            if (!log.open(QIODevice::ReadOnly)) return qsizetype(0);
            qsizetype count = 0;
            for (const auto &line : log.readAll().split('\n'))
                if (QJsonDocument::fromJson(line).array().contains("com.genymobile.scrcpy.Server"))
                    ++count;
            return count;
        };

        AutoConnectTestWindow window;
        window.show();
        auto *button = window.findChild<QPushButton *>("manualClipboard");
        auto *receive = window.findChild<QAction *>("clipboardFromPhone");
        auto *send = window.findChild<QAction *>("clipboardToPhone");
        auto *sharing = window.findChild<QCheckBox *>("clipboardEnabled");
        auto *devices = window.findChild<QListWidget *>("deviceSidebar");
        auto *manager = window.findChild<ConnectionManager *>();
        QVERIFY(button && receive && send && sharing && devices && manager);
        QVERIFY(!button->isEnabled() && !sharing->isChecked());

        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText("original");
        // Receive while Deskflow is idle. Change selection while the request is
        // running to confirm the operation stays bound to the clicked phone.
        devices->setCurrentRow(1);
        QVERIFY(button->isEnabled() && !sharing->isChecked());
        receive->trigger();
        QVERIFY(!button->isEnabled());
        devices->setCurrentRow(0);
        QTRY_VERIFY(tunnel.hasPendingConnections());
        auto *reader = tunnel.nextPendingConnection();
        QTRY_COMPARE(serverLaunchCount(), 1);
        reader->write(QByteArray(1, '\0'));
        QTRY_COMPARE(reader->bytesAvailable(), 2);
        QCOMPARE(reader->readAll(), QByteArray::fromHex("0800"));
        const auto phoneText = QStringLiteral("\uFEFF手机📋\r\n下一行 \n");
        const auto packet = clipboardReply(phoneText.toUtf8());
        reader->write(packet.left(3));
        QTRY_COMPARE(reader->bytesToWrite(), 0);
        QCOMPARE(clipboard->text(), "original");
        reader->write(packet.mid(3));
        QTRY_COMPARE(clipboard->text(), phoneText);
        QVERIFY(window.statusBar()->currentMessage().startsWith("two · Copied"));
        QVERIFY(!button->isEnabled()); // The first device still has ADB enhancements off.

        // Send while a keyboard/mouse session is active. Changing the local
        // clipboard during startup must not change what was selected to send.
        window.findChild<QCheckBox *>("adbEnabled")->setChecked(true);
        auto *connectButton = window.findChild<QPushButton *>("connectButton");
        QTRY_VERIFY(connectButton->isEnabled());
        connectButton->click();
        QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
        QVERIFY(button->isEnabled() && !sharing->isChecked());
        const auto hostText = QStringLiteral("电脑 😀\n'$(text)'\r\n") + QString(128 * 1024, QChar('A'));
        const auto bytes = hostText.toUtf8();
        clipboard->setText(hostText);
        send->trigger();
        clipboard->setText("newer local text");
        QTRY_VERIFY(tunnel.hasPendingConnections());
        auto *writer = tunnel.nextPendingConnection();
        QTRY_COMPARE(serverLaunchCount(), 2);
        writer->write(QByteArray(1, '\0'));
        QTRY_COMPARE(writer->bytesAvailable(), bytes.size() + 14);
        const auto request = writer->readAll();
        QCOMPARE(request.left(10), QByteArray::fromHex("09000000000000000100"));
        QCOMPARE(qFromBigEndian<quint32>(request.constData() + 10), quint32(bytes.size()));
        QVERIFY(request.mid(14) == bytes);
        writer->write(clipboardAck());
        QTRY_COMPARE(writer->bytesAvailable(), 2);
        QCOMPARE(writer->readAll(), QByteArray::fromHex("0800"));
        writer->write(clipboardReply(bytes));
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(window.statusBar()->currentMessage().startsWith("one · Sent"));
        QCOMPARE(clipboard->text(), "newer local text");
        QCOMPARE(manager->state("initial"), Session::State::Connected);
        QVERIFY(!sharing->isChecked());

        // Quit with a read outstanding. Wait for owned ADB work to finish and
        // keep the local clipboard even if the phone tries to respond afterward.
        receive->trigger();
        QTRY_VERIFY(tunnel.hasPendingConnections());
        auto *cancelled = tunnel.nextPendingConnection();
        QTRY_COMPARE(serverLaunchCount(), 3);
        cancelled->write(QByteArray(1, '\0'));
        QTRY_COMPARE(cancelled->bytesAvailable(), 2);
        QSignalSpy quit(&window, &MainWindow::exitReady);
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(quit.size(), 1);
        QCOMPARE(cancelled->state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(clipboard->text(), "newer local text");
        const auto runtimeLog = window.findChild<QPlainTextEdit *>("runtimeLog")->toPlainText();
        QVERIFY(!runtimeLog.contains("手机📋") && !runtimeLog.contains("电脑 😀"));

        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QStringList jars, removedJars, pushTargets, forwarded, launched, removedPorts;
        for (const auto &line : log.readAll().split('\n')) {
            const auto args = QJsonDocument::fromJson(line).array();
            QVERIFY(!args.contains("--clipboard") && !args.contains("clipboard_autosync=true"));
            QVERIFY(!args.contains("pair") && !args.contains("connect") && !args.contains("kill-server"));
            if (args.at(2) == "push") {
                QCOMPARE(args.size(), 5);
                pushTargets << args.at(1).toString();
                jars << args.at(4).toString();
            } else if (args.at(2) == "forward" && args.at(3) == "tcp:0") {
                forwarded << args.at(4).toString();
            } else if (args.at(2) == "forward" && args.at(3) == "--remove") {
                removedPorts << args.at(4).toString();
            } else if (args.at(2) == "shell" && args.at(5) == "rm") {
                removedJars << args.at(7).toString();
            } else if (args.contains("com.genymobile.scrcpy.Server")) {
                QVERIFY(args.contains("clipboard_autosync=false") && args.contains("audio=false") &&
                        args.contains("video=false") && args.contains("power_on=false"));
                launched << (QStringLiteral("localabstract:scrcpy_") + args.last().toString().section('=', 1));
            }
        }
        QCOMPARE(pushTargets, (QStringList{"[::1]:5555", "USB-ID", "USB-ID"}));
        QCOMPARE(removedJars, jars);
        QCOMPARE(launched, forwarded);
        QCOMPARE(removedPorts.size(), 3);
        QVERIFY(jars[0] != jars[1] && jars[1] != jars[2] && jars[0] != jars[2]);
    }

    void manualClipboardFailure_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("error");
        QTest::newRow("offline") << QByteArray("offline") << QString("adb_unavailable");
        QTest::newRow("push-failed") << QByteArray("push-failed") << QString("failed");
        QTest::newRow("empty") << QByteArray("empty") << QString("read_unavailable");
        QTest::newRow("no-reply") << QByteArray("no-reply") << QString("read_unavailable");
        QTest::newRow("malformed") << QByteArray("malformed") << QString("invalid_text");
        QTest::newRow("oversized") << QByteArray("oversized") << QString("limit");
        QTest::newRow("mismatched-readback") << QByteArray("mismatched-readback") << QString("write_unconfirmed");
        QTest::newRow("image-only") << QByteArray("image-only") << QString("no_local_text");
    }

    void manualClipboardFailure()
    {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Use the isolated offscreen clipboard");
        QFETCH(QByteArray, mode);
        QFETCH(QString, error);
        QTcpServer tunnel;
        QVERIFY(tunnel.listen(QHostAddress::LocalHost));
        qputenv("OTG_TEST_CLIPBOARD_PORT", QByteArray::number(tunnel.serverPort()));
        qputenv("OTG_TEST_CLIPBOARD_ADB", mode);
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_CLIPBOARD_PORT");
            qunsetenv("OTG_TEST_CLIPBOARD_ADB");
        });
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText("preserve me");
        const bool write = mode == "mismatched-readback" || mode == "image-only";
        if (mode == "image-only") clipboard->setImage(QImage(10, 10, QImage::Format_RGB32));
        PhoneClipboard transfer;
        QSignalSpy done(&transfer, &PhoneClipboard::completed);
        transfer.start(testPeerPath(), "[::1]:5555", findScrcpyServer(), findScrcpyVersion(),
                       write ? PhoneClipboard::Direction::ToPhone : PhoneClipboard::Direction::FromPhone);
        if (mode != "offline" && mode != "push-failed" && mode != "image-only") {
            QTRY_VERIFY(tunnel.hasPendingConnections());
            auto *client = tunnel.nextPendingConnection();
            client->write(QByteArray(1, '\0'));
            if (write) {
                QTRY_COMPARE(client->bytesAvailable(), 25);
                client->readAll();
                client->write(clipboardAck());
                QTRY_COMPARE(client->bytesAvailable(), 2);
                QCOMPARE(client->readAll(), QByteArray::fromHex("0800"));
                client->write(clipboardReply("different text"));
            } else {
                QTRY_COMPARE(client->bytesAvailable(), 2);
                if (mode == "empty") client->write(clipboardReply({}));
                if (mode == "malformed") client->write(clipboardReply(QByteArray::fromHex("e4b8")));
                if (mode == "oversized") client->write(clipboardReply(QByteArray(OTG_CLIPBOARD_TEXT_MAX + 1, 'X')).left(5));
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 8000);
        QCOMPARE(done.takeFirst().first().toString(), "otg.manual_clipboard." + error);
        QVERIFY(!transfer.busy());
        if (mode == "image-only") QVERIFY(clipboard->mimeData()->hasImage());
        else QCOMPARE(clipboard->text(), "preserve me");
    }
#endif

    void fileSharingWindow()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", ""); settings.setValue("manageServer", false);
        qputenv("OTG_TEST_TRANSFER_DIR", dir.path().toUtf8());
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_TRANSFER_DIR"); qunsetenv("OTG_TEST_TRANSFER_MODE");
        });
        QFile manifest(dir.filePath("manifest.json")); QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("{\"version\":1,\"files\":[]}"); manifest.close();
        AutoConnectTestWindow window; window.show();
        QTRY_COMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), QString("TEST123"));
        auto *button = window.findChild<QPushButton *>("transferFiles"); QVERIFY(button);
        QVERIFY(!button->isEnabled());
        window.findChild<QCheckBox *>("adbEnabled")->setChecked(true);
        auto *adb = window.findChild<QLineEdit *>("adbExecutable");
        auto *target = window.findChild<QLineEdit *>("adbSerial");
        adb->setText(QCoreApplication::applicationDirPath() + "/test_transfer_peer"
#ifdef Q_OS_WIN
                     ".exe"
#endif
                     );
        target->setText("[::1]:5555");
        QVERIFY(button->isEnabled()); button->click();
        auto *dialog = window.findChild<TransferDialog *>("transferDialog"); QVERIFY(dialog);
        QPointer<CompanionTransfer> engine = dialog->findChild<CompanionTransfer *>(); QVERIFY(engine);
        QTRY_VERIFY(!engine->busy());
        // Changing selection/identity in the main form never retargets an open transfer window.
        target->setText("OTHER-PHONE");
        dialog->findChild<QPushButton *>("transferRefresh")->click();
        QTRY_VERIFY(!engine->busy());
        QFile log(dir.filePath("commands")); QVERIFY(log.open(QIODevice::ReadOnly));
        for (const auto &line : log.readAll().trimmed().split('\n'))
            QCOMPARE(QJsonDocument::fromJson(line).array()[1].toString(), "[::1]:5555");
        qputenv("OTG_TEST_TRANSFER_MODE", "hang-online");
        dialog->findChild<QPushButton *>("transferRefresh")->click();
        QVERIFY(engine->busy());
        QSignalSpy exit(&window, &MainWindow::exitReady);
        window.findChild<QAction *>("trayQuit")->trigger();
        QCOMPARE(exit.size(), 0); // Exit waits for cancellation of the ADB worker.
        QTRY_COMPARE(exit.size(), 1);
        QVERIFY(!engine || !engine->busy());
    }

    void numericFieldsIgnoreMouseWheel()
    {
        MainWindow window;
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *height = window.findChild<QSpinBox *>("screenHeight");
        auto *sensitivity = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
        auto *motion = window.findChild<QDoubleSpinBox *>("motionMultiplier");
        auto *port = window.findChild<QSpinBox *>("serverPort");
        QVERIFY(width && height && sensitivity && motion && port);

        width->setValue(1080);
        height->setValue(2340);
        sensitivity->setValue(2.0);
        motion->setValue(2.5);
        port->setValue(24800);

        const auto sendWheel = [](QWidget *widget) {
            const QPointF position(widget->rect().center());
            const QPointF globalPosition(widget->mapToGlobal(widget->rect().center()));
            QWheelEvent event(position, globalPosition, QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(widget, &event);
        };
        sendWheel(width);
        sendWheel(height);
        sendWheel(sensitivity);
        sendWheel(motion);
        sendWheel(port);

        QCOMPARE(width->value(), 1080);
        QCOMPARE(height->value(), 2340);
        QCOMPARE(sensitivity->value(), 2.0);
        QCOMPARE(motion->value(), 2.5);
        QCOMPARE(port->value(), 24800);
    }

    void wirelessAdbSetup_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("pairLog");
        QTest::newRow("english") << QStringLiteral("en")
            << QStringLiteral("ADB pairing succeeded: 192.0.2.10:37123\n[ADB output] Successfully paired to 192.0.2.10:37123");
        QTest::newRow("chinese") << QStringLiteral("zh_CN")
            << QStringLiteral("ADB 配对成功：192.0.2.10:37123\n[ADB 原始输出] Successfully paired to 192.0.2.10:37123");
    }
    void wirelessAdbSetup()
    {
        QFETCH(QString, language);
        QFETCH(QString, pairLog);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", language);

        AutoConnectTestWindow window;
        window.show();
        QTRY_COMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), QString("TEST123"));
        auto *adb = window.findChild<QLineEdit *>("adbExecutable");
        auto *setup = window.findChild<QPushButton *>("adbWirelessSetup");
        QVERIFY(adb && setup);
        adb->setText(QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                     ".exe"
#endif
        );
        QVERIFY(setup->isEnabled());
        setup->click();

        auto *dialog = window.findChild<QDialog *>("wirelessAdbDialog");
        auto *pairing = window.findChild<QComboBox *>("wirelessPairingTarget");
        auto *code = window.findChild<QLineEdit *>("wirelessPairingCode");
        auto *pair = window.findChild<QPushButton *>("wirelessPair");
        auto *connection = window.findChild<QComboBox *>("wirelessConnectionTarget");
        auto *connect = window.findChild<QPushButton *>("wirelessConnect");
        auto *target = window.findChild<QLineEdit *>("adbSerial");
        auto *enabled = window.findChild<QCheckBox *>("adbEnabled");
        auto *log = window.findChild<QPlainTextEdit *>("runtimeLog");
        QVERIFY(dialog && pairing && code && pair && connection && connect && target && enabled && log);
        QTRY_COMPARE(pairing->count(), 1);
        QTRY_COMPARE(connection->count(), 1);
        QCOMPARE(pairing->currentText(), "192.0.2.10:37123");
        QCOMPARE(connection->currentText(), "192.0.2.10:39833");
        const auto sendWheel = [](QWidget *widget) {
            const QPointF position(widget->rect().center());
            const QPointF globalPosition(widget->mapToGlobal(widget->rect().center()));
            QWheelEvent event(position, globalPosition, QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(widget, &event);
        };
        const int pairingIndex = pairing->currentIndex();
        const int connectionIndex = connection->currentIndex();
        sendWheel(pairing);
        sendWheel(connection);
        QCOMPARE(pairing->currentIndex(), pairingIndex);
        QCOMPARE(connection->currentIndex(), connectionIndex);

        code->setText("123456");
        pair->click();
        QTRY_VERIFY(log->toPlainText().contains(pairLog));
        QVERIFY(!log->toPlainText().contains("123456"));
        QTRY_VERIFY(pair->isEnabled());
        QTRY_VERIFY(connect->isEnabled());
        connect->click();
        QTRY_COMPARE(target->text(), "192.0.2.10:39833");
        QVERIFY(enabled->isChecked());
        dialog->close();
    }
    void dropdownsIgnoreMouseWheel_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::newRow("language") << QStringLiteral("languageSelector");
        QTest::newRow("transport") << QStringLiteral("transportSelector");
        QTest::newRow("device") << QStringLiteral("deviceSelector");
        QTest::newRow("mouse mode") << QStringLiteral("mouseMode");
        QTest::newRow("middle button") << QStringLiteral("middleButtonAction");
        QTest::newRow("right button") << QStringLiteral("rightButtonAction");
        QTest::newRow("side button 1") << QStringLiteral("sideButton1Action");
        QTest::newRow("side button 2") << QStringLiteral("sideButton2Action");
        QTest::newRow("screen position") << QStringLiteral("screenPosition");
        QTest::newRow("screen neighbor") << QStringLiteral("screenNeighbor");
        QTest::newRow("TLS key length") << QStringLiteral("tlsKeyLength");
        QTest::newRow("log filter") << QStringLiteral("logFilter");
    }

    void dropdownsIgnoreMouseWheel()
    {
        QFETCH(QString, objectName);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", true);
        settings.setValue("tls", true);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto *combo = window.findChild<QComboBox *>(objectName);
        QVERIFY(combo);
        auto *tabs = window.findChild<QTabWidget *>("deviceTabs");
        QVERIFY(tabs);
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i)->isAncestorOf(combo)) tabs->setCurrentIndex(i);
        QVERIFY(combo->isEnabled());

        // Exercise selection without triggering device discovery or changing transport.
        const QSignalBlocker blocker(combo);
        const int originalIndex = combo->currentIndex();
        const int originalCount = combo->count();
        if (combo->count() < 2) combo->addItem(QStringLiteral("Wheel test item"));
        for (bool focused : {false, true}) {
            if (focused) combo->setFocus();
            else combo->clearFocus();
            QCOMPARE(combo->hasFocus(), focused);
            for (int index = 0; index < combo->count(); ++index) {
                combo->setCurrentIndex(index);
                for (int delta : {-120, 120}) {
                    const QPointF position(combo->rect().center());
                    const QPointF globalPosition(combo->mapToGlobal(combo->rect().center()));
                    QWheelEvent event(position, globalPosition, QPoint(), QPoint(0, delta),
                                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
                    QApplication::sendEvent(combo, &event);
                    QCOMPARE(combo->currentIndex(), index);
                    QVERIFY(!event.isAccepted());
                }
            }
        }

        // Ignoring the wheel must not disable intentional keyboard selection.
        combo->setCurrentIndex(0);
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(combo->currentIndex(), 1);
        QTest::keyClick(combo, Qt::Key_Up);
        QCOMPARE(combo->currentIndex(), 0);
        while (combo->count() > originalCount) combo->removeItem(combo->count() - 1);
        combo->setCurrentIndex(originalIndex);
        window.findChild<QAction *>("trayQuit")->trigger();
    }

    void companionApkDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        const auto bundled = QCoreApplication::applicationDirPath() + "/deskflow-bridge-companion.apk";
        const bool existed = QFile::exists(bundled);
        if (!existed) {
            QFile file(bundled);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fake APK");
        }
        const auto cleanup = qScopeGuard([&] { if (!existed) QFile::remove(bundled); });
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        const auto custom = dir.filePath("custom.apk");
        QFile file(custom);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("custom fake APK"); file.close();
        for (const auto &saved : QStringList{QString(), dir.filePath("old-install/companion.apk"), custom}) {
            settings.setValue("companionApk", saved);
            AutoConnectTestWindow window;
            QCOMPARE(window.findChild<QLineEdit *>("companionApk")->text(), saved == custom ? custom : bundled);
            QVERIFY(!window.findChild<CompanionApp *>()->busy()); // No automatic install.
            window.findChild<QAction *>("trayQuit")->trigger();
            QCOMPARE(settings.value("companionApk").toString(), saved == custom ? custom : QString());
        }
        settings.remove("companionApk"); // Fresh install also defaults to the bundle.
        AutoConnectTestWindow window;
        QCOMPARE(window.findChild<QLineEdit *>("companionApk")->text(), bundled);
        window.findChild<QAction *>("trayQuit")->trigger();
        if (!existed) {
            QVERIFY(QFile::remove(bundled));
            QVERIFY(CompanionApp::preferredApk("").isEmpty());
            QCOMPARE(CompanionApp::preferredApk(custom), custom);
        }
    }
    void companionSetup()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "zh_CN");
        settings.setValue("manageServer", false);
        const auto logPath = dir.filePath("adb.jsonl");
        qputenv("OTG_TEST_COMPANION_LOG", logPath.toUtf8());
        qputenv("OTG_TEST_COMPANION_DELAY", "80");
        qputenv("OTG_TEST_USB_SCAN_DELAY", "250"); // Discovery need not finish within 20 ms.
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_COMPANION_LOG"); qunsetenv("OTG_TEST_COMPANION_DELAY");
            qunsetenv("OTG_TEST_USB_SCAN_DELAY");
        });
        QString apk;
        {
            AutoConnectTestWindow window;
            window.show();
            QTRY_COMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), QString("TEST123"));
            auto *install = window.findChild<QPushButton *>("companionInstall");
            auto *open = window.findChild<QPushButton *>("companionOpen");
            auto *check = window.findChild<QPushButton *>("companionCheck");
            auto *status = window.findChild<QLabel *>("companionStatus");
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *target = window.findChild<QLineEdit *>("adbSerial");
            auto *adb = window.findChild<QLineEdit *>("adbExecutable");
            auto *sidebar = window.findChild<QListWidget *>("deviceSidebar");
            auto *connectButton = window.findChild<QPushButton *>("connectButton");
            auto *app = window.findChild<CompanionApp *>();
            auto *adbEnabled = window.findChild<QCheckBox *>("adbEnabled");
            QVERIFY(install && open && check && status && serial && target && adb && sidebar && app && adbEnabled);
            QVERIFY(!install->isEnabled());
            adbEnabled->setChecked(true);
            adb->setText(QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                         ".exe"
#endif
                         );
            serial->setText("USB-ONLY");
            target->setText("192.0.2.10:5555");
            QVERIFY(install->isEnabled());
            QVERIFY(!QFile::exists(logPath)); // No installation/query on launch or editing.
            apk = dir.filePath(QString::fromUtf8("手机 app.apk"));
            QFile file(apk); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fake APK"); file.close();
            window.findChild<QLineEdit *>("companionApk")->setText(apk);
            install->click();
            QVERIFY(app->busy());
            QVERIFY(!sidebar->isEnabled());
            QVERIFY(!connectButton->isEnabled());
            QCOMPARE(status->text(), qtTrId("otg.companion.working"));
            QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.installed"));
            QVERIFY(sidebar->isEnabled()); QVERIFY(connectButton->isEnabled());
            auto *language = window.findChild<QComboBox *>("languageSelector");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(install->text(), "Install / update APK");
            QCOMPARE(status->text(), qtTrId("otg.companion.installed"));
            language->setCurrentIndex(language->findData("zh_CN"));
            check->click(); QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.access_required"));
            auto *transport = window.findChild<QComboBox *>("transportSelector");
            if (transport->findData("ble") >= 0) {
                transport->setCurrentIndex(transport->findData("ble"));
                QVERIFY(!install->isVisible());
                transport->setCurrentIndex(transport->findData("usb"));
            }
            target->clear();
#ifdef Q_OS_WIN
            // Windows requires an explicit ADB target, even for USB HID.
            QVERIFY(!open->isEnabled());
            const QString nextTarget = "192.0.2.20:5555";
            target->setText(nextTarget);
#else
            const QString nextTarget = "USB-ONLY"; // POSIX USB serial fallback.
#endif
            QVERIFY(open->isEnabled());
            open->click(); QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.opened"));
            const auto capture = qEnvironmentVariable("OTG_TEST_COMPANION_SCREENSHOT");
            if (!capture.isEmpty()) QVERIFY(window.grab().save(capture));
            check->click();
            QVERIFY(app->busy());
            QSignalSpy exit(&window, &MainWindow::exitReady);
            window.findChild<QAction *>("trayQuit")->trigger();
            QCOMPARE(exit.size(), 0); // Quit waits for the bounded setup command.
            QTRY_COMPARE(exit.size(), 1);
            QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
            const auto lines = log.readAll().trimmed().split('\n');
            QCOMPARE(lines.size(), 11); // install(2), check(3), open(3), check(3)
            for (int i = 0; i < lines.size(); ++i) {
                const auto args = QJsonDocument::fromJson(lines[i]).array();
                QCOMPARE(args[0].toString(), "-s");
                QCOMPARE(args[1].toString(), i < 5 ? QString("192.0.2.10:5555") : nextTarget);
            }
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QLineEdit *>("companionApk")->text(), apk);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void generateClientCertificate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        const auto path = dir.filePath(QString::fromUtf8("客户端/client.pem"));
        qputenv("OTG_TEST_USB_SCAN_DELAY", "250");
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_USB_SCAN_DELAY"); });
        {
            AutoConnectTestWindow window;
            QTRY_COMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), QString("TEST123"));
            auto *tls = window.findChild<QCheckBox *>("tlsEnabled");
            auto *certificate = window.findChild<QLineEdit *>("tlsCertificate");
            auto *fingerprint = window.findChild<QLineEdit *>("tlsFingerprint");
            auto *database = window.findChild<QLineEdit *>("tlsFingerprintFile");
            auto *generate = window.findChild<QPushButton *>("tlsRegenerateCertificate");
            auto *browse = window.findChild<QPushButton *>("tlsBrowseCertificate");
            QVERIFY(tls && certificate && fingerprint && database && generate && browse);
            QVERIFY(certificate->text().isEmpty()); // Client authentication remains optional.
            QVERIFY(!generate->isEnabled());
            QVERIFY(!browse->isEnabled());
            tls->setChecked(true);
            QVERIFY(generate->isEnabled()); // External-server mode is supported.
            QVERIFY(browse->isEnabled());
            QCOMPARE(generate->text(), QString::fromUtf8("生成客户端证书"));
            QVERIFY(!window.findChild<QLineEdit *>("tlsServerCertificate"));
            QVERIFY(!window.findChild<QCheckBox *>("tlsRequireClientCertificate"));
            certificate->setText(path);
            database->setText("trusted-servers");
            QString message;
            auto answerDialog = [&](QMessageBox::StandardButton answer) {
                QTimer::singleShot(0, &window, [&, answer] {
                    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    QVERIFY(box);
                    message = box->text();
                    box->done(answer);
                });
            };
            answerDialog(QMessageBox::Ok);
            generate->click();
            QVERIFY(QFile::exists(path));
            const auto clientFingerprint = TlsCertificate::fingerprint(path);
            QVERIFY(!clientFingerprint.isEmpty());
            QVERIFY(message.contains(clientFingerprint));
            QVERIFY(fingerprint->text().isEmpty());
            QCOMPARE(database->text(), "trusted-servers");
            QCOMPARE(certificate->text(), path);
            QCOMPARE(settings.value("tlsCertificate").toString(), path);

            QFile pem(path);
            QVERIFY(pem.open(QIODevice::ReadOnly));
            const auto original = pem.readAll();
            pem.close();
            answerDialog(QMessageBox::No);
            generate->click();
            QVERIFY(pem.open(QIODevice::ReadOnly));
            QCOMPARE(pem.readAll(), original); // Declining overwrite preserves the identity.
            pem.close();
            QVERIFY(message.contains(path));

            database->clear();
            fingerprint->setText(QString(64, 'a'));
            certificate->setText(dir.filePath("second.pem"));
            answerDialog(QMessageBox::Ok);
            generate->click();
            QCOMPARE(fingerprint->text(), QString(64, 'a'));
            QVERIFY(database->text().isEmpty());

            certificate->setText(path + "/impossible.pem"); // Parent is a file, save must fail.
            answerDialog(QMessageBox::Ok);
            generate->click();
            QVERIFY(message.contains(qtTrId("otg.tls.generate_failed").section("%1", 0, 0)));
            QCOMPARE(TlsCertificate::fingerprint(path), clientFingerprint);
            QCOMPARE(fingerprint->text(), QString(64, 'a'));
            certificate->setText(path);
            auto *language = window.findChild<QComboBox *>("languageSelector");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(generate->text(), "Generate client certificate");
            QCOMPARE(certificate->text(), path);
            language->setCurrentIndex(language->findData("zh_CN"));

            // Exercise connection gating with a fake bridge, never USB hardware.
            window.findChild<QLineEdit *>("usbSerial")->setText("TLS-client");
            auto *manager = window.findChild<ConnectionManager *>();
            auto *connectButton = window.findChild<QPushButton *>("connectButton");
            QVERIFY(connectButton->isEnabled());
            connectButton->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QVERIFY(!generate->isEnabled());
            QVERIFY(!browse->isEnabled());
            manager->stopAll();
            QTRY_VERIFY(!manager->busy());
            QVERIFY(generate->isEnabled());

            const auto capture = qEnvironmentVariable("OTG_TEST_TLS_SCREENSHOT");
            if (!capture.isEmpty()) {
                window.findChild<QTabWidget *>("deviceTabs")->setCurrentIndex(1);
                window.show();
                QTest::qWait(50);
                QVERIFY(window.grab().save(capture));
            }
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QLineEdit *>("tlsCertificate")->text(), path);
        QCOMPARE(restored.findChild<QLineEdit *>("tlsFingerprint")->text(), QString(64, 'a'));
        QVERIFY(restored.findChild<QPushButton *>("tlsRegenerateCertificate")->isEnabled());
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void displayUpdatesArePerDevice()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        {
            AutoConnectTestWindow window;
            auto *manager = window.findChild<ConnectionManager *>();
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *height = window.findChild<QSpinBox *>("screenHeight");
            auto *sensitivity = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
            auto *motion = window.findChild<QDoubleSpinBox *>("motionMultiplier");
            QVERIFY(manager && list && width && height && sensitivity && motion);
            width->setValue(1080); height->setValue(1920); sensitivity->setValue(2.0);
            motion->setValue(2.0);
            window.findChild<QPushButton *>("addDeviceButton")->click();
            const auto second = list->currentItem()->data(Qt::UserRole).toString();
            width->setValue(720); height->setValue(1600); sensitivity->setValue(1.5); motion->setValue(2.5);
            manager->session("initial")->displaySizeChanged(QSize(1920, 1080));
            QCOMPARE(width->value(), 720); QCOMPARE(height->value(), 1600);
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(width->value(), 1920); QCOMPARE(height->value(), 1080);
            QCOMPARE(sensitivity->value(), 2.0);
            QVERIFY(window.findChild<QLabel *>("virtualScreenSize")->text().startsWith("960 × 540"));
            manager->session(second)->displaySizeChanged(QSize(1600, 720));
            QCOMPARE(width->value(), 1920);
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1920);
        QCOMPARE(restored.findChild<QSpinBox *>("screenHeight")->value(), 1080);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 2.0);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("motionMultiplier")->value(), 2.0);
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(1);
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1600);
        QCOMPARE(restored.findChild<QSpinBox *>("screenHeight")->value(), 720);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 1.5);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("motionMultiplier")->value(), 2.5);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void deviceFeatureProfiles_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::newRow("clipboard") << QString("clipboardEnabled");
        QTest::newRow("audio") << QString("audioEnabled");
    }
    void deviceFeatureProfiles()
    {
        QFETCH(QString, objectName);
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        {
            AutoConnectTestWindow window;
            auto *feature = window.findChild<QCheckBox *>(objectName);
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *gain = window.findChild<QSpinBox *>("audioGainDb");
            const bool audioProfile = objectName == "audioEnabled";
            QVERIFY(feature && list && gain);
            QVERIFY(!feature->isChecked());
            QCOMPARE(gain->value(), 0);
            feature->setChecked(true);
            if (audioProfile) gain->setValue(6);
            window.findChild<QLineEdit *>("usbSerial")->setText("A");
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QVERIFY(!feature->isChecked());
            QCOMPARE(gain->value(), 0);
            window.findChild<QLineEdit *>("usbSerial")->setText("B");
            list->setCurrentRow(0);
            QVERIFY(feature->isChecked());
            if (audioProfile) QCOMPARE(gain->value(), 6);
            list->setCurrentRow(1);
            QVERIFY(!feature->isChecked());
        }
        AutoConnectTestWindow restored;
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(0);
        QVERIFY(restored.findChild<QCheckBox *>(objectName)->isChecked());
        if (objectName == "audioEnabled")
            QCOMPARE(restored.findChild<QSpinBox *>("audioGainDb")->value(), 6);
        list->setCurrentRow(1);
        QVERIFY(!restored.findChild<QCheckBox *>(objectName)->isChecked());
        QCOMPARE(restored.findChild<QSpinBox *>("audioGainDb")->value(), 0);
    }
    void audioPlacementAndAvailability()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        QSettings().setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        window.show();
        auto *adbEnabled = window.findChild<QCheckBox *>("adbEnabled");
        auto *audio = window.findChild<QCheckBox *>("audioEnabled");
        auto *gain = window.findChild<QSpinBox *>("audioGainDb");
        auto *clipboard = window.findChild<QCheckBox *>("clipboardEnabled");
        auto *autoRotate = window.findChild<QCheckBox *>("autoRotate");
        auto *unlock = window.findChild<QCheckBox *>("unlockEnabled");
        auto *pin = window.findChild<QLineEdit *>("unlockPin");
        auto *tabs = window.findChild<QTabWidget *>("deviceTabs");
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QVERIFY(adbEnabled && audio && gain && clipboard && autoRotate && unlock && pin && tabs && transport);
        QVERIFY(!adbEnabled->isChecked());
        QVERIFY(!audio->isEnabled());
        QVERIFY(gain->isHidden());
        QCOMPARE(gain->value(), 0);
        adbEnabled->setChecked(true);
        QVERIFY(!pin->isEnabled());
        unlock->setChecked(true);
        QVERIFY(pin->isEnabled());
        pin->setText("1234");
        QCOMPARE(pin->text(), QStringLiteral("1234"));
        QVERIFY(autoRotate->isVisible());
        QVERIFY(!autoRotate->isChecked());
        QVERIFY(!autoRotate->isEnabled());
#ifdef OTG_GUI_HAVE_AUDIO
        audio->setChecked(true);
        QVERIFY(!gain->isHidden() && gain->isEnabled());
        QCOMPARE(gain->maximum(), 12);
        QVERIFY(gain->toolTip().contains("手机媒体音量"));
        gain->setValue(6);
        QVERIFY(autoRotate->isEnabled());
        audio->setChecked(false);
        QVERIFY(gain->isHidden());
        QCOMPARE(gain->value(), 6);
        QVERIFY(!autoRotate->isEnabled());
#endif
#ifdef OTG_GUI_HAVE_CLIPBOARD
        clipboard->setChecked(true);
        QVERIFY(autoRotate->isEnabled());
        clipboard->setChecked(false);
        QVERIFY(!autoRotate->isEnabled());
#endif
        QCOMPARE(audio->text(), "转发音频");
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(language);
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(audio->text(), "Forward audio");
        QVERIFY(gain->toolTip().contains("Boost phone audio"));
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(audio->text(), "转发音频");
        QVERIFY(tabs->widget(0)->isAncestorOf(audio));
        QCOMPARE(audio->parentWidget(), clipboard->parentWidget());
        QVERIFY(!tabs->widget(1)->isAncestorOf(audio));
#ifdef OTG_GUI_HAVE_AUDIO
        QVERIFY(!audio->isHidden());
#else
        QVERIFY(audio->isHidden());
        audio->setChecked(true);
        QVERIFY(gain->isHidden());
        audio->setChecked(false);
#endif
        const int ble = transport->findData("ble");
        if (ble >= 0) {
            transport->setCurrentIndex(ble);
#ifdef OTG_GUI_HAVE_AUDIO
            QVERIFY(!audio->isHidden());
            QVERIFY(audio->isEnabled());
#else
            QVERIFY(audio->isHidden());
            QVERIFY(!audio->isEnabled());
#endif
            transport->setCurrentIndex(0);
#ifdef OTG_GUI_HAVE_AUDIO
            QVERIFY(!audio->isHidden());
            QVERIFY(audio->isEnabled());
#else
            QVERIFY(audio->isHidden());
#endif
        }
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void multipleDevices()
    {
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("deskflow", QCoreApplication::applicationDirPath() + "/test_peer");
        settings.setValue("manageServer", true);
        settings.setValue("language", "zh_CN");
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        const auto port = allocator.serverPort(); allocator.close();
        settings.setValue("port", port);
        {
            AutoConnectTestWindow window;
            window.show();
            QCoreApplication::processEvents();
            auto *manager = window.findChild<ConnectionManager *>();
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *tabs = window.findChild<QTabWidget *>("deviceTabs");
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *screenName = window.findChild<QLineEdit *>("screenName");
            auto *alias = window.findChild<QLineEdit *>("deviceAlias");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *audio = window.findChild<QCheckBox *>("audioEnabled");
            auto *manageServer = window.findChild<QCheckBox *>("manageServer");
            auto *neighbor = window.findChild<QComboBox *>("screenNeighbor");
            auto *direction = window.findChild<QComboBox *>("screenPosition");
            auto *layoutWarning = window.findChild<QLabel *>("layoutWarning");
            auto *connectButton = window.findChild<QPushButton *>("connectButton");
            QVERIFY(manager && list && tabs && serial && screenName && alias && width && audio);
            QVERIFY(manageServer && neighbor && direction && layoutWarning);
            QCOMPARE(tabs->count(), 2);
            QCOMPARE(tabs->tabText(0), QStringLiteral("设备配置"));
            QCOMPARE(tabs->tabText(1), QStringLiteral("共享服务"));
            QVERIFY(tabs->widget(1)->isAncestorOf(manageServer));
            QCOMPARE(neighbor->parentWidget(), screenName->parentWidget());
            QCOMPARE(direction->parentWidget(), screenName->parentWidget());
            QVERIFY(!window.findChild<QWidget *>("screenLayoutCanvas"));
            QVERIFY(layoutWarning->isHidden());
            window.findChild<QCheckBox *>("adbEnabled")->setChecked(true);
            QCOMPARE(list->count(), 1);
            serial->setText("A"); alias->setText("Redmi K20 Pro"); width->setValue(1234);
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QCOMPARE(list->count(), 2);
            QCOMPARE(screenName->text(), "android2");
            QCOMPARE(alias->text(), "android2");
            QCOMPARE(neighbor->currentData().toString(), "android");
            neighbor->setCurrentIndex(neighbor->findData("computer"));
            QVERIFY(!layoutWarning->isHidden()); // Both devices would use the computer's right edge.
            manageServer->setChecked(false);
            QVERIFY(!neighbor->isEnabled() && !direction->isEnabled());
            QVERIFY(layoutWarning->isHidden());
            manageServer->setChecked(true);
            QVERIFY(neighbor->isEnabled() && direction->isEnabled());
            QVERIFY(!layoutWarning->isHidden());
            neighbor->setCurrentIndex(neighbor->findData("android"));
            direction->setCurrentIndex(direction->findData("down"));
            QVERIFY(layoutWarning->isHidden());
            const auto secondId = list->currentItem()->data(Qt::UserRole).toString();
            QVERIFY(secondId != "initial");
            auto *transport = window.findChild<QComboBox *>("transportSelector");
            transport->setCurrentIndex(transport->findData("usb"));
            window.findChild<QCheckBox *>("adbEnabled")->setChecked(false);
            serial->setText("B"); alias->setText("Pixel 8"); width->setValue(1080);
            connectButton->click();
            QTRY_COMPARE(manager->state(secondId), Session::State::Connected);
            QVERIFY(list->isEnabled());
            QVERIFY(!tabs->widget(1)->isEnabled());
            QVERIFY(!audio->isEnabled());
            QVERIFY(!window.findChild<QPushButton *>("addDeviceButton")->isEnabled());
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(serial->text(), "A");
            QCOMPARE(width->value(), 1234);
            QVERIFY(serial->isEnabled());
            QVERIFY(audio->isEnabled()); // Another device's connection must not lock this option.
            QVERIFY(!neighbor->isEnabled() && !direction->isEnabled());
            connectButton->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QVERIFY(!audio->isEnabled());
            QCOMPARE(manager->state(secondId), Session::State::Connected);
            QVERIFY(window.findChild<QLabel *>("connectionSummary")->text().contains("2 / 2"));
            auto *logFilter = window.findChild<QComboBox *>("logFilter");
            auto *log = window.findChild<QPlainTextEdit *>("runtimeLog");
            QVERIFY(log->toPlainText().contains("    down = android2\n"));
            manager->session("initial")->log("ONLY_A");
            manager->session(secondId)->log("ONLY_B");
            logFilter->setCurrentIndex(logFilter->findData(secondId));
            QVERIFY(log->toPlainText().contains("ONLY_B"));
            QVERIFY(!log->toPlainText().contains("ONLY_A"));
            logFilter->setCurrentIndex(logFilter->findData("*"));
            QVERIFY(log->toPlainText().contains("ONLY_A"));
            const auto screenshot = qEnvironmentVariable("OTG_TEST_MULTI_SCREENSHOT");
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot));
            window.findChild<QPushButton *>("disconnectAllButton")->click();
            QTRY_VERIFY(!manager->busy());
            QTRY_COMPARE(manager->serverState(), DeskflowServer::State::Idle);
            // Save with the second record selected: restoring must not merge or lose IDs.
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == secondId) list->setCurrentRow(i);
            window.findChild<QPushButton *>("connectAllButton")->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QTRY_COMPARE(manager->state(secondId), Session::State::Connected);
            window.findChild<QAction *>("trayQuit")->trigger();
            QTRY_VERIFY(manager->settled());
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QListWidget *>("deviceSidebar")->count(), 2);
        QCOMPARE(restored.findChild<QLineEdit *>("usbSerial")->text(), "A");
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1234);
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(1);
        QCOMPARE(restored.findChild<QComboBox *>("screenNeighbor")->currentData().toString(), "android");
        QCOMPARE(restored.findChild<QComboBox *>("screenPosition")->currentData().toString(), "down");
        restored.findChild<QPushButton *>("removeDeviceButton")->click();
        QCOMPARE(list->count(), 1);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void systemThemePalette()
    {
        auto *hints = QGuiApplication::styleHints();
        const auto originalScheme = hints->colorScheme();
        const auto originalPalette = QGuiApplication::palette();
        const auto restore = qScopeGuard([&] {
            QGuiApplication::setPalette(originalPalette);
            hints->setColorScheme(originalScheme);
        });

        hints->setColorScheme(Qt::ColorScheme::Dark);
        if (hints->colorScheme() != Qt::ColorScheme::Dark)
            QSKIP("the offscreen platform does not support overriding the color scheme");
        syncSystemThemePalette(*qobject_cast<QApplication *>(QCoreApplication::instance()));
        QVERIFY(QGuiApplication::palette().color(QPalette::Window).lightness() < 128);

        hints->setColorScheme(Qt::ColorScheme::Light);
        syncSystemThemePalette(*qobject_cast<QApplication *>(QCoreApplication::instance()));
        QVERIFY(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
    }
    void wiredUhidAutoConnect()
    {
#ifdef OTG_GUI_HAVE_UHID
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        const auto peer = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
        settings.setValue("adb", peer);
        settings.setValue("transport", "uhid");
        settings.setValue("manageServer", false);
        settings.setValue("autoConnect", true); // An old AOA opt-in must not opt in UHID.
        settings.beginWriteArray("rememberedDevices", 1);
        settings.setArrayIndex(0);
        settings.setValue("profile", QVariantMap{{"transport", "usb"}, {"serial", "TEST123"}});
        settings.endArray();
        const auto logPath = dir.filePath("adb.jsonl");
        qputenv("OTG_TEST_UHID_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_ADB_DEVICES"); qunsetenv("OTG_TEST_UHID_LOG"); });
        const auto log = [&] {
            QFile file(logPath);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto scans = [&] { return log().count("[\"devices\",\"-l\"]"); };
        const QByteArray header = "List of devices attached\n";
        {
            AutoConnectTestWindow window; window.show();
            auto *target = window.findChild<QLineEdit *>("adbSerial");
            auto *enable = window.findChild<QCheckBox *>("uhidUsbAutoConnect");
            auto *button = window.findChild<QPushButton *>("connectButton");
            auto *session = window.findChild<Session *>();
            QTRY_COMPARE(target->text(), "TEST123");
            QTRY_VERIFY(button->isEnabled());
            QVERIFY(!enable->isChecked());
            target->setText("192.0.2.1:5555");
            QVERIFY(!enable->isEnabled());
            target->setText("TEST123");
            QVERIFY(enable->isEnabled());
            const auto before = scans();
            enable->click();
            QTRY_VERIFY(scans() >= before + 2);
            QCOMPARE(session->state(), Session::State::Idle); // Same serial remembered only for AOA.
            button->click();
            QTRY_COMPARE(session->state(), Session::State::Connected);
            window.findChild<QAction *>("trayDisconnect")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
            const auto stopped = scans();
            QTRY_VERIFY_WITH_TIMEOUT(scans() >= stopped + 2, 8000);
            QCOMPARE(session->state(), Session::State::Idle);
            qputenv("OTG_TEST_ADB_DEVICES", header + "TEST123 device transport_id:2\n");
            QTRY_COMPARE(session->state(), Session::State::Connected);
            window.findChild<QAction *>("trayQuit")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
        }
        // Restart with an unrelated phone attached: keep the saved target. A
        // later arrival connects even with the window hidden in the tray.
        qputenv("OTG_TEST_ADB_DEVICES", header + "OTHER device transport_id:4\n");
        AutoConnectTestWindow restored; restored.show();
        auto *session = restored.findChild<Session *>();
        const auto before = scans();
        QTRY_VERIFY(scans() > before);
        QCOMPARE(restored.findChild<QLineEdit *>("adbSerial")->text(), "TEST123");
        QVERIFY(restored.findChild<QCheckBox *>("uhidUsbAutoConnect")->isChecked());
        QCOMPARE(session->state(), Session::State::Idle);
        restored.close();
        qputenv("OTG_TEST_ADB_DEVICES", header + "TEST123 device transport_id:5\nOTHER device transport_id:4\n");
        QTRY_COMPARE(session->state(), Session::State::Connected);
        restored.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(session->state(), Session::State::Idle);
        const auto commands = log();
        QCOMPARE(commands.count("\"--gui\""), 3);
        QCOMPARE(commands.count("\"--adb-serial\",\"TEST123\""), 3);
        QVERIFY(!commands.contains("--usb-scan") && !commands.contains("--usb-watch"));
        QVERIFY(!commands.contains("[\"connect\"") && !commands.contains("[\"pair\""));
#endif
    }

    void multipleAutoConnectWithoutAdb()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        QVariantMap a{{"serial", "A"}, {"phone", "a"}, {"neighbor", "computer"}, {"direction", "right"},
                      {"autoConnect", true}, {"originalWidth", 1234}, {"originalHeight", 2340}};
        auto b = a; b["serial"] = "B"; b["phone"] = "b"; b["neighbor"] = "a"; b["originalWidth"] = 1080;
        settings.beginWriteArray("devices", 2);
        settings.setArrayIndex(0); settings.setValue("id", "phone-a"); settings.setValue("profile", a);
        settings.setArrayIndex(1); settings.setValue("id", "phone-b"); settings.setValue("profile", b);
        settings.endArray();
        settings.beginWriteArray("rememberedDevices", 2);
        settings.setArrayIndex(0); settings.setValue("profile", a);
        settings.setArrayIndex(1); settings.setValue("profile", b);
        settings.endArray();
        const auto path = dir.filePath("usb.json");
        QFile snapshot(path);
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        const QByteArray devices = "{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"A\"},{\"id\":\"1:3\",\"serial\":\"B\"}]}";
        snapshot.write(devices); snapshot.close();
        qputenv("OTG_TEST_USB_SNAPSHOT", path.toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_USB_SNAPSHOT"); });
        AutoConnectTestWindow window;
        auto *manager = window.findChild<ConnectionManager *>();
        auto *monitor = window.findChild<UsbAutoConnect *>();
        QTRY_COMPARE(manager->state("phone-a"), Session::State::Connected);
        QTRY_COMPARE(manager->state("phone-b"), Session::State::Connected);
        QCOMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), "A");
        manager->stop("phone-a");
        QTRY_COMPARE(manager->state("phone-a"), Session::State::Idle);
        QCOMPARE(manager->state("phone-b"), Session::State::Connected);
        QVERIFY(monitor->acceptSnapshot(devices));
        QCOMPARE(manager->state("phone-a"), Session::State::Idle);
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_VERIFY(manager->settled());
    }
    void transportAvailability()
    {
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().setValue("adb", "");
        AutoConnectTestWindow window;
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QVERIFY(transport);
        QCOMPARE(transport->itemData(0).toString(), "usb");
        int count = 1;
#ifdef OTG_GUI_HAVE_UHID
        ++count;
        QVERIFY(transport->findData("uhid") >= 0);
#else
        QCOMPARE(transport->findData("uhid"), -1);
#endif
#if (defined(Q_OS_LINUX) && defined(OTG_GUI_HAVE_BLE)) || (defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN))
        ++count;
        QVERIFY(transport->findData("ble") >= 0);
#else
        QCOMPARE(transport->findData("ble"), -1);
#endif
        QCOMPARE(transport->count(), count);
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void bleSettingsAndNoAdb()
    {
#if !defined(OTG_GUI_HAVE_BLE) && !defined(OTG_GUI_HAVE_BLE_WIN)
        QSKIP("BLE-enabled GUI only");
#endif
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", QCoreApplication::applicationDirPath() + "/test_peer");
        settings.setValue("transport", "ble");
        settings.setValue("bleAdapter", "hci1");
        settings.setValue("blePeer", "AA:BB:CC:DD:EE:FF");
#ifdef Q_OS_WIN
        settings.setValue("bleIsolate", true); // Stale Linux settings must never leak into Windows args.
        settings.setValue("bleLegacyHandles", true);
        qputenv("OTG_TEST_BLE_WINDOWS", "1");
        const auto restoreEnv = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_WINDOWS"); });
#endif
        settings.setValue("mouseMode", "relative");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QCOMPARE(transport->currentData().toString(), "ble");
        auto *serial = window.findChild<QLineEdit *>("usbSerial");
        QVERIFY(serial->text().isEmpty());
        QVERIFY(!serial->isEnabled());
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        QCOMPARE(mode->currentData().toString(), "absolute");
        QVERIFY(!mode->isEnabled());
        QCOMPARE(window.findChild<QLineEdit *>("bleAdapter")->text(), "hci1");
#ifdef Q_OS_WIN
        QVERIFY(!window.findChild<QLineEdit *>("bleAdapter")->isVisible());
        QVERIFY(!window.findChild<QCheckBox *>("bleIsolate")->isVisible());
        QVERIFY(!window.findChild<QCheckBox *>("bleLegacyHandles")->isVisible());
        QVERIFY(!window.findChild<QPushButton *>("stopBleService")->isVisible());
        QVERIFY(window.findChild<QLineEdit *>("blePeer")->toolTip().contains("Windows"));
#elif defined(Q_OS_LINUX)
        auto *legacy = window.findChild<QCheckBox *>("bleLegacyHandles");
        QVERIFY(legacy && legacy->isVisible() && !legacy->isChecked());
        legacy->setChecked(true);
        qputenv("OTG_TEST_BLE_STATUS", "legacy");
        const auto restoreEnv = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_STATUS"); });
#endif
        auto *session = window.findChild<Session *>();
        auto *peer = window.findChild<QLineEdit *>("blePeer");
        QCOMPARE(peer->text(), "AA:BB:CC:DD:EE:FF");
        peer->clear();
        auto *perAxis = window.findChild<QCheckBox *>("absolutePerAxis");
        QVERIFY(perAxis && perAxis->isEnabled() && !perAxis->isChecked());
        perAxis->setChecked(true);
        qputenv("OTG_TEST_EXPECT_PER_AXIS", "1");
        const auto restorePerAxisEnv = qScopeGuard([] { qunsetenv("OTG_TEST_EXPECT_PER_AXIS"); });
        auto *button = window.findChild<QPushButton *>("connectButton");
        QVERIFY(button->isEnabled());
        QVERIFY(window.findChild<QPushButton *>("connectAllButton")->isEnabled());
        button->click();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        QVERIFY(serial->text().isEmpty()); // No startup ADB scan in BLE mode.
        session->stop();
        QTRY_COMPARE(session->state(), Session::State::Idle);
        const auto screenshot = qEnvironmentVariable("OTG_TEST_BLE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_VERIFY(!session->hasBleProvider());
        QCOMPARE(settings.value("transport").toString(), "ble");
        QVERIFY(settings.value("blePeer").toString().isEmpty());
        AutoConnectTestWindow restored;
        QVERIFY(restored.findChild<QLineEdit *>("blePeer")->text().isEmpty());
        QVERIFY(restored.findChild<QCheckBox *>("absolutePerAxis")->isChecked());
#ifdef Q_OS_LINUX
        QVERIFY(restored.findChild<QCheckBox *>("bleLegacyHandles")->isChecked());
#endif
        restored.findChild<QComboBox *>("transportSelector")->setCurrentIndex(0);
        QVERIFY(restored.findChild<QLineEdit *>("usbSerial")->isEnabled());
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void clearLogs()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        QCoreApplication::processEvents();
        auto *clear = window.findChild<QPushButton *>("clearLogButton");
        auto *log = window.findChild<QPlainTextEdit *>("runtimeLog");
        auto *session = window.findChild<Session *>();
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(clear && log && session && language);
        QCOMPARE(clear->text(), "清除日志");
        clear->click(); // Clearing an empty log is harmless.
        QVERIFY(log->toPlainText().isEmpty());
        window.findChild<QLineEdit *>("usbSerial")->setText("TEST123");
        QTRY_VERIFY(window.findChild<QPushButton *>("connectButton")->isEnabled());
        window.findChild<QPushButton *>("connectButton")->click();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        session->log("first line\nsecond line");
        QVERIFY(log->toPlainText().contains("first line\nsecond line"));
        QVERIFY(clear->isEnabled());
        clear->click();
        QVERIFY(log->toPlainText().isEmpty());
        QCOMPARE(session->state(), Session::State::Connected);
        session->log("new log after clearing");
        QVERIFY(log->toPlainText().contains("new log after clearing"));
        QVERIFY(!log->toPlainText().contains("first line"));
        const QString text = log->toPlainText();
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(clear->text(), "Clear logs");
        QCOMPARE(log->toPlainText(), text);
        clear->click();
        QVERIFY(log->toPlainText().isEmpty());
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(session->state(), Session::State::Idle);
    }
    void failedConnectionNotRemembered()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("usb-failure-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        AutoConnectTestWindow window;
        QCoreApplication::processEvents();
        window.findChild<QLineEdit *>("usbSerial")->setText("reject");
        auto *session = window.findChild<Session *>();
        window.findChild<QPushButton *>("connectButton")->click();
        QTRY_COMPARE(session->state(), Session::State::Idle);
        QCOMPARE(settings.beginReadArray("rememberedDevices"), 0);
        settings.endArray();
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void autoConnectWithoutAdb()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("usb-auto-connect-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "en");
        const QString snapshotFile = settingsDir.filePath("usb.json");
        auto writeSnapshot = [&](const QByteArray &data) {
            QFile file(snapshotFile);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            return file.write(data) == data.size();
        };
        const QByteArray phone = "{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"TEST123\"}]}";
        QVERIFY(writeSnapshot(phone));
        qputenv("OTG_TEST_USB_SNAPSHOT", snapshotFile.toUtf8());
        qputenv("OTG_TEST_RIGHT_ACTION", "back");
        const auto commandLog = settingsDir.filePath("commands.jsonl");
        qputenv("OTG_TEST_UHID_LOG", commandLog.toUtf8());
        const auto restore = qScopeGuard([] {
            qunsetenv("OTG_TEST_USB_SNAPSHOT"); qunsetenv("OTG_TEST_RIGHT_ACTION");
            qunsetenv("OTG_TEST_UHID_LOG");
        });
        {
            AutoConnectTestWindow window;
            auto *enabled = window.findChild<QCheckBox *>("autoConnectEnabled");
            auto *session = window.findChild<Session *>();
            auto *monitor = window.findChild<UsbAutoConnect *>();
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *adbSerial = window.findChild<QLineEdit *>("adbSerial");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *multiplier = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
            auto *button = window.findChild<QPushButton *>("connectButton");
            QVERIFY(enabled && session && monitor && serial && adbSerial && width && multiplier && button);
            QVERIFY(!enabled->isChecked());
            // First connect manually with no ADB, then remember only on success.
            QCoreApplication::processEvents();
            serial->setText("TEST123");
            adbSerial->setText("192.168.1.20:5555");
            width->setValue(1234);
            multiplier->setValue(2.0);
            window.findChild<QComboBox *>("rightButtonAction")->setCurrentIndex(1);
            enabled->setChecked(true);
            QTRY_VERIFY(button->isEnabled());
            button->click();
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QCOMPARE(settings.beginReadArray("rememberedDevices"), 1);
            settings.setArrayIndex(0);
            QCOMPARE(settings.value("profile").toMap().value("originalWidth").toInt(), 1234);
            QCOMPARE(settings.value("profile").toMap().value("adbSerial").toString(),
                     QStringLiteral("192.168.1.20:5555"));
            QCOMPARE(settings.value("profile").toMap().value("rightButtonAction").toString(), "back");
            settings.endArray();
            QVERIFY(monitor->acceptSnapshot(phone));
            window.findChild<QAction *>("trayDisconnect")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
            QVERIFY(monitor->acceptSnapshot(phone));
            QCOMPARE(session->state(), Session::State::Idle);
            // Enabling ADB enhancements after the first successful AOA
            // connection must affect the next automatic connection too;
            // the remembered snapshot still represents the old no-ADB run.
            window.findChild<QLineEdit *>("adbExecutable")->setText(testPeerPath());
#ifdef Q_OS_WIN
            // Windows keeps the AOA serial and ADB identity separate.
            adbSerial->setText("USB_ADB_ID");
#else
            adbSerial->clear(); // Use the AOA serial as the local ADB target.
#endif
            auto *adbEnabled = window.findChild<QCheckBox *>("adbEnabled");
            auto *audio = window.findChild<QCheckBox *>("audioEnabled");
            QVERIFY(adbEnabled && audio);
            adbEnabled->setChecked(true);
            audio->setChecked(true);
            // Reinsert restores the phone's settings, not unsaved form edits.
            width->setValue(999);
            multiplier->setValue(1.0);
            window.findChild<QComboBox *>("rightButtonAction")->setCurrentIndex(0);
            QVERIFY(writeSnapshot("{\"version\":1,\"devices\":[]}"));
            QVERIFY(monitor->acceptSnapshot("{\"version\":1,\"devices\":[]}"));
            window.show();
            window.close(); // Tray/background auto-connect remains active.
            QVERIFY(!window.isVisible());
            QVERIFY(writeSnapshot(phone));
            monitor->poll();
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QVERIFY(adbEnabled->isChecked());
            QVERIFY(audio->isChecked());
            QFile commands(commandLog);
            QVERIFY(commands.open(QIODevice::ReadOnly));
            const auto commandText = commands.readAll();
            QVERIFY(commandText.contains("--audio"));
            QVERIFY(commandText.contains("--adb"));
#ifdef Q_OS_WIN
            QVERIFY(commandText.contains("\"--adb-serial\",\"USB_ADB_ID\""));
            QVERIFY(!commandText.contains("\"--adb-serial\",\"TEST123\""));
#else
            QVERIFY(commandText.contains("\"--adb-serial\",\"TEST123\""));
#endif
            QCOMPARE(width->value(), 1234);
            QCOMPARE(multiplier->value(), 2.0);
            QCOMPARE(window.findChild<QComboBox *>("rightButtonAction")->currentData().toString(), "back");
            window.findChild<QAction *>("trayQuit")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
        }
        // Startup restores the updated ADB enhancements and target.
        AutoConnectTestWindow restored;
        auto *session = restored.findChild<Session *>();
        QVERIFY(restored.findChild<QCheckBox *>("autoConnectEnabled")->isChecked());
        QTRY_COMPARE(session->state(), Session::State::Connected);
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1234);
#ifdef Q_OS_WIN
        QCOMPARE(restored.findChild<QLineEdit *>("adbSerial")->text(), QStringLiteral("USB_ADB_ID"));
#else
        QVERIFY(restored.findChild<QLineEdit *>("adbSerial")->text().isEmpty());
#endif
        restored.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(session->state(), Session::State::Idle);
    }
    void aboutVersion()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().setValue("adb", "");
        QSettings().setValue("language", "en");
        MainWindow window;
        auto *about = window.findChild<QPushButton *>("aboutButton");
        QVERIFY(about);
        bool inspected = false;
        QString aboutText;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QMessageBox *>();
            if (dialog) {
                aboutText = dialog->text();
                inspected = true;
                dialog->accept();
            }
        });
        about->click();
        // QMessageBox::about() is non-blocking on macOS; let the timer run there too.
        QTRY_VERIFY(inspected);
        QVERIFY(aboutText.contains(OTG_VERSION));
        QVERIFY(aboutText.contains(OTG_COMMIT));
        QVERIFY(aboutText.contains("\nVersion:"));
    }
    void trayLifecycle_data()
    {
        QTest::addColumn<bool>("available");
        QTest::addColumn<bool>("connected");
        QTest::newRow("tray-idle") << true << false;
        QTest::newRow("tray-connected") << true << true;
        QTest::newRow("no-tray-idle") << false << false;
        QTest::newRow("no-tray-connected") << false << true;
    }
    void trayLifecycle()
    {
        QFETCH(bool, available);
        QFETCH(bool, connected);
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("tray-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "en");
        TrayTestWindow window;
        window.available = available;
        window.show();
        auto *tray = window.findChild<QSystemTrayIcon *>("systemTray");
        auto *show = window.findChild<QAction *>("trayShow");
        auto *disconnect = window.findChild<QAction *>("trayDisconnect");
        auto *quit = window.findChild<QAction *>("trayQuit");
        auto *session = window.findChild<Session *>();
        QVERIFY(tray && show && disconnect && quit && session);
        QVERIFY(!tray->icon().isNull());
        QVERIFY(!tray->icon().pixmap(32, 32).isNull());
        QCOMPARE(tray->icon().cacheKey(), window.windowIcon().cacheKey());
        QVERIFY(!disconnect->isEnabled());
        QCOMPARE(show->text(), "Show window");
        auto *language = window.findChild<QComboBox *>("languageSelector");
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(show->text(), "显示窗口");
        QCOMPARE(quit->text(), "退出");
        language->setCurrentIndex(language->findData("en"));
        if (connected) {
            ConnectionOptions options;
            options.bridge = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                ".exe"
#endif
                ;
            options.manageServer = false;
            options.serial = "TEST123";
            session->start(options);
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QVERIFY(disconnect->isEnabled());
            QVERIFY(tray->toolTip().contains(session->statusMessage()));
        }
        QSignalSpy exited(&window, &MainWindow::exitReady);
        window.close();
        if (available) {
            QVERIFY(!window.isVisible());
            QCOMPARE(exited.count(), 0);
            QCOMPARE(session->state(), connected ? Session::State::Connected : Session::State::Idle);
            show->trigger();
            QVERIFY(window.isVisible());
            window.showMinimized();
            emit tray->activated(QSystemTrayIcon::Trigger);
            QVERIFY(window.isVisible());
            QVERIFY(!window.isMinimized());
            if (connected) {
                window.close();
                disconnect->trigger();
                QTRY_COMPARE(session->state(), Session::State::Idle);
                QVERIFY(!disconnect->isEnabled());
                QCOMPARE(exited.count(), 0);
                // Quit while connected must wait for graceful bridge shutdown.
                ConnectionOptions options;
                options.bridge = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                    ".exe"
#endif
                    ;
                options.manageServer = false;
                options.serial = "TEST123";
                session->start(options);
                QTRY_COMPARE(session->state(), Session::State::Connected);
                quit->trigger();
                QCOMPARE(exited.count(), 0);
            } else {
                quit->trigger();
            }
        }
        QTRY_COMPARE(exited.count(), 1);
        QCOMPARE(session->state(), Session::State::Idle);
        QVERIFY(!window.isVisible());
        QVERIFY(!tray->isVisible());
    }
    void defaultLanguage_data()
    {
        QTest::addColumn<QString>("savedLanguage");
        QTest::newRow("first-launch") << QString();
        QTest::newRow("invalid-setting") << QString("unsupported");
    }
    void defaultLanguage()
    {
        QFETCH(QString, savedLanguage);
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("default-language-test");
        QSettings settings;
        settings.setValue("adb", "");
        if (!savedLanguage.isEmpty())
            settings.setValue("language", savedLanguage);
        MainWindow window;
        QCOMPARE(window.findChild<QComboBox *>("languageSelector")->currentData().toString(),
                 LanguageManager::systemLanguage());
        window.close();
    }
    void languageSwitching()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("language-test");
        QSettings settings;
        settings.setValue("language", "en");
        settings.setValue("adb", "");
        MainWindow window;
        window.show();
        auto *language = window.findChild<QComboBox *>("languageSelector");
        auto *button = window.findChild<QPushButton *>("connectButton");
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        QVERIFY(language && button && width && mode);
        QCOMPARE(language->currentData().toString(), "en");
        QCOMPARE(button->text(), "Connect");
        QTRY_VERIFY(window.findChildren<QLabel *>().size() > 0);
        width->setValue(1234);
        mode->setCurrentIndex(mode->findData("relative"));
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(button->text(), "连接");
        QCOMPARE(mode->currentText(), "相对坐标（兼容模式）");
        QCOMPARE(width->value(), 1234);
        QCOMPARE(mode->currentData().toString(), "relative");
        QCOMPARE(settings.value("language").toString(), "zh_CN");
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(button->text(), "Connect");
        QCOMPARE(mode->currentText(), "Relative (compatibility mode)");
        QTRY_VERIFY([&window] {
            for (auto *label : window.findChildren<QLabel *>()) {
                if (label->text().startsWith("ADB not found."))
                    return true;
            }
            return false;
        }());
        QCOMPARE(width->value(), 1234);
        window.close();
        MainWindow restored;
        QCOMPARE(restored.findChild<QComboBox *>("languageSelector")->currentData().toString(), "en");
        QCOMPARE(restored.findChild<QPushButton *>("connectButton")->text(), "Connect");
        restored.close();
    }
    void guidePointerScreenshot()
    {
        const auto capture = qEnvironmentVariable("OTG_TEST_POINTER_SCREENSHOTS");
        if (capture.isEmpty()) return;
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        const QString peer = QCoreApplication::applicationDirPath() + "/test_peer";
        for (const auto &language : {QStringLiteral("en"), QStringLiteral("zh_CN")}) {
            QSettings settings;
            settings.clear();
            settings.setValue("language", language);
            settings.setValue("adb", peer);
            settings.setValue("adbSerial", "DEMO_USB_SERIAL");
            settings.setValue("adbEnabled", false);
            settings.setValue("deskflow", peer);
            settings.setValue("transport", "uhid");
            settings.setValue("originalWidth", 1080);
            settings.setValue("originalHeight", 2340);
            AutoConnectTestWindow window;
            window.available = false;
            window.resize(1100, 955);
            window.show();
            QCoreApplication::processEvents();
            auto *perAxis = window.findChild<QCheckBox *>("absolutePerAxis");
            auto *scroll = window.findChild<QScrollArea *>();
            QVERIFY(perAxis && scroll);
            QTRY_VERIFY(perAxis->isEnabled());
            scroll->ensureWidgetVisible(perAxis, 0, 12);
            QCoreApplication::processEvents();
            const auto suffix = language == "en" ? "en.png" : "zh-CN.png";
            QVERIFY(window.grab().save(QDir(capture).filePath(QString("input-") + suffix)));
            window.close();
        }
    }
    void discoverAndRender_data()
    {
        QTest::addColumn<bool>("noDevpath");
        QTest::newRow("usb-path") << false;
        QTest::newRow("windows-native-usb") << true;
    }
    void discoverAndRender()
    {
        QFETCH(bool, noDevpath);
        if (noDevpath)
            qputenv("OTG_TEST_NO_DEVPATH", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_NO_DEVPATH"); });
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("window-test");
        const QString peer = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
        QSettings settings;
        settings.setValue("adb", peer);
        settings.setValue("adbEnabled", true);
        settings.setValue("adbSerial", "TEST123"); // Explicit ADB target on Windows.
        settings.setValue("deskflow", peer);
        settings.setValue("language", "zh_CN");
        // Windows discovers USB through the bridge, not through `adb devices`.
        AutoConnectTestWindow window;
        window.available = false;
        window.show();
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *height = window.findChild<QSpinBox *>("screenHeight");
        auto *serial = window.findChild<QLineEdit *>("usbSerial");
        auto *connectButton = window.findChild<QPushButton *>("connectButton");
        QVERIFY(width && height && serial && connectButton);
        QTRY_COMPARE(width->value(), 1600);
        QCOMPARE(height->value(), 720);
        QCOMPARE(serial->text(), "TEST123");
        QVERIFY(connectButton->isEnabled());
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(language);
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(connectButton->text(), "Connect");
        QCOMPARE(serial->text(), "TEST123");
        QCOMPARE(width->value(), 1600);
        QTRY_VERIFY([&window] {
            for (auto *label : window.findChildren<QLabel *>()) {
                if (label->isVisible() && !label->wordWrap() && label->width() < label->minimumSizeHint().width())
                    return false;
            }
            return true;
        }());
        const QString englishCapture = qEnvironmentVariable("OTG_TEST_SCREENSHOT_EN");
        if (!englishCapture.isEmpty())
            QVERIFY(window.grab().save(englishCapture));
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(connectButton->text(), "连接");
        auto *multiplier = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
        auto *motion = window.findChild<QDoubleSpinBox *>("motionMultiplier");
        auto *virtualSize = window.findChild<QLabel *>("virtualScreenSize");
        auto *swap = window.findChild<QPushButton *>("swapDimensions");
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        auto *perAxis = window.findChild<QCheckBox *>("absolutePerAxis");
        QVERIFY(multiplier && motion && virtualSize && swap && mode && perAxis);
        QVERIFY(perAxis->isEnabled() && !perAxis->isChecked());
        width->setValue(1080);
        height->setValue(2340);
        multiplier->setValue(1.5);
        motion->setValue(2.0);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        multiplier->setValue(2.0);
        QVERIFY(virtualSize->text().startsWith("540 × 1170"));
        multiplier->setValue(1.5);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        motion->setValue(3.0);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        QCOMPARE(width->value(), 1080);
        QCOMPARE(height->value(), 2340);
        swap->click();
        QVERIFY(virtualSize->text().startsWith("1560 × 720"));
        swap->click();
        mode->setCurrentIndex(mode->findData("relative"));
        QVERIFY(!multiplier->isEnabled());
        QVERIFY(!motion->isEnabled());
        QVERIFY(!perAxis->isEnabled());
        QVERIFY(virtualSize->text().startsWith("1080 × 2340"));
        mode->setCurrentIndex(mode->findData("absolute"));
        QVERIFY(multiplier->isEnabled());
        QVERIFY(motion->isEnabled());
        QVERIFY(perAxis->isEnabled());
        perAxis->setChecked(true);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        width->setValue(1);
        height->setValue(3);
        multiplier->setValue(10.0);
        QVERIFY(virtualSize->text().startsWith("1 × 1"));
        width->setValue(1080);
        height->setValue(2340);
        multiplier->setValue(1.0);
        QVERIFY(virtualSize->text().startsWith("1080 × 2340"));
        multiplier->setValue(1.5);
        motion->setValue(1.5);
        const QString capture = qEnvironmentVariable("OTG_TEST_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        window.close();
        QCOMPARE(settings.value("originalWidth").toInt(), 1080);
        QCOMPARE(settings.value("originalHeight").toInt(), 2340);
        QCOMPARE(settings.value("sensitivityMultiplier").toDouble(), 1.5);
        QCOMPARE(settings.value("motionMultiplier").toDouble(), 1.5);
        QCOMPARE(settings.value("absolutePerAxis").toBool(), true);
        QCOMPARE(settings.value("width").toInt(), 720);
        QCOMPARE(settings.value("height").toInt(), 1560);
        AutoConnectTestWindow restored;
        restored.available = false;
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1080);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 1.5);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("motionMultiplier")->value(), 1.5);
        QVERIFY(restored.findChild<QLabel *>("virtualScreenSize")->text().startsWith("720 × 1560"));
        // ADB refresh replaces the baseline, retaining the chosen multiplier.
        QTRY_COMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1600);
        QTRY_VERIFY(restored.findChild<QPushButton *>("connectButton")->isEnabled());
        QVERIFY(restored.findChild<QLabel *>("virtualScreenSize")->text().startsWith("1067 × 480"));
        restored.close();
    }
};

QTEST_MAIN(WindowTest)
#include "test_window.moc"
