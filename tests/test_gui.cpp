// SPDX-License-Identifier: Apache-2.0
#include "device_info.h"
#include "process_utils.h"
#include "session.h"
#include "connection_manager.h"
#include "companion_app.h"
#include "tls_certificate.h"
#include "wireless_adb_dialog.h"
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "i18n.h"
#include <QFile>
#include <QMap>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QScopeGuard>
#include <QXmlStreamReader>
#include <QtTest>
#include <openssl/pem.h>
#include <memory>
#include <limits>

Q_DECLARE_METATYPE(ConnectionOptions)
Q_DECLARE_METATYPE(ServerOptions)

static QString peerPath()
{
    return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}

static const QByteArray deskflowConfig =
    "section: screens\n  computer:\n  android:\n  a:\n  b:\nend\n\n"
    "section: links\n  computer:\n    down = android\n  android:\n    up = computer\nend\n";

static bool writeDeskflowConfig(QTemporaryFile &file)
{
    if (!file.open()) return false;
    const bool written = file.write(deskflowConfig) == deskflowConfig.size() && file.flush();
    file.close();
    return written;
}

struct Catalog {
    QString language, error;
    QMap<QString, QString> sources, texts;
};

static Catalog readCatalog(const QString &path)
{
    Catalog catalog;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        catalog.error = file.errorString();
        return catalog;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("TS"))
            catalog.language = xml.attributes().value("language").toString();
        if (xml.name() != QLatin1String("message"))
            continue;
        const QString id = xml.attributes().value("id").toString();
        if (id.isEmpty() || catalog.texts.contains(id)) {
            xml.raiseError("Missing or duplicate ID: " + id);
            break;
        }
        QString source, translation;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (xml.name() == QLatin1String("translation")) {
                if (!xml.attributes().value("type").isEmpty()) {
                    xml.raiseError("Unfinished or obsolete translation: " + id);
                    break;
                }
                translation = xml.readElementText();
            } else {
                xml.skipCurrentElement();
            }
        }
        if (source.isEmpty() || translation.isEmpty()) {
            xml.raiseError("Empty source or translation: " + id);
            break;
        }
        catalog.sources.insert(id, source);
        catalog.texts.insert(id, translation);
    }
    if (xml.hasError())
        catalog.error = xml.errorString();
    return catalog;
}

static QStringList placeholders(const QString &text)
{
    static const QRegularExpression pattern("%L?(?:[1-9][0-9]*|n)");
    QStringList result;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext())
        result.append(matches.next().captured());
    result.sort();
    return result;
}

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void invalidConnectionOptions_data()
    {
        QTest::addColumn<ConnectionOptions>("options");
        QTest::addColumn<QString>("errorId");
        ConnectionOptions base;
        base.serial = "TEST123";
        base.bridge = peerPath();
        base.manageServer = false;
        base.scrcpyServer = "fake-server";
        base.scrcpyVersion = "5.0.1";
        const auto row = [base](const char *name, const char *id, auto change) {
            auto options = base;
            change(options);
            QTest::newRow(name) << options << QString::fromLatin1(id);
        };
        row("transport", "otg.error.transport", [](auto &o) { o.transport = "invalid"; });
        row("usb-serial", "otg.error.usb_serial_required", [](auto &o) { o.serial.clear(); });
        row("adb-program", "otg.error.adb_program_required", [](auto &o) { o.audio = true; o.adb.clear(); });
        row("adb-target", "otg.error.adb_target_required", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.clipboard = true; o.adb = peerPath();
        });
#ifdef Q_OS_WIN
        row("usb-no-adb-target", "otg.error.adb_target_required", [](auto &o) {
            o.audio = true; o.adb = peerPath();
        });
        row("usb-blank-adb-target", "otg.error.adb_target_required", [](auto &o) {
            o.audio = true; o.adb = peerPath(); o.adbSerial = "  ";
        });
#endif
#ifdef OTG_GUI_HAVE_UHID
        row("uhid-no-usb-fallback", "otg.error.adb_target_required", [](auto &o) { o.transport = "uhid"; o.adb = peerPath(); });
        row("uhid-blank-target", "otg.error.adb_target_required", [](auto &o) {
            o.transport = "uhid"; o.adbSerial = "  "; o.adb = peerPath();
        });
#endif
        row("ble-peer-format", "otg.error.ble_peer", [](auto &o) { o.transport = "ble"; o.blePeer = "bad-mac"; });
        row("ble-relative", "otg.error.ble_mouse_mode", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.mouseMode = "relative";
        });
        row("mouse-mode", "otg.error.mouse_mode", [](auto &o) { o.mouseMode = "invalid"; });
        row("middle-button", "otg.error.middle_button_action", [](auto &o) { o.middleButtonAction = "invalid"; });
        row("right-button", "otg.error.right_button_action", [](auto &o) { o.rightButtonAction = "invalid"; });
        row("side-button-1", "otg.error.side_button1_action", [](auto &o) { o.sideButton1Action = "invalid"; });
        row("side-button-2", "otg.error.side_button2_action", [](auto &o) { o.sideButton2Action = "invalid"; });
        row("swipe-low", "otg.error.swipe_distance", [](auto &o) { o.sideSwipeDistancePercent = 0; });
        row("swipe-high", "otg.error.swipe_distance", [](auto &o) { o.sideSwipeDistancePercent = 101; });
        row("pin-short", "otg.error.unlock_pin", [](auto &o) { o.unlockPin = "987"; });
        row("pin-long", "otg.error.unlock_pin", [](auto &o) { o.unlockPin = QString(33, '8'); });
        row("pin-nondigit", "otg.error.unlock_pin", [](auto &o) { o.unlockPin = "private-invalid-pin"; });
        row("tls-missing", "otg.error.tls_fingerprint_required", [](auto &o) { o.tls = true; });
        row("tls-conflict", "otg.error.tls_fingerprint_conflict", [](auto &o) {
            o.tls = true; o.tlsFingerprint = QString(64, 'a'); o.tlsFingerprintFile = "fingerprints.txt";
        });
        row("phone-empty", "otg.error.phone_screen_name", [](auto &o) { o.phone.clear(); });
        row("phone-characters", "otg.error.phone_screen_name", [](auto &o) { o.phone = "bad name"; });
        row("phone-prefix", "otg.error.phone_screen_name", [](auto &o) { o.phone = ".phone"; });
        row("phone-too-long", "otg.error.phone_screen_name", [](auto &o) { o.phone = QString(64, 'a'); });
        row("host-empty", "otg.error.host_required", [](auto &o) { o.host.clear(); });
        row("host-blank", "otg.error.host_required", [](auto &o) { o.host = "  "; });
        row("host-space", "otg.error.host_characters", [](auto &o) { o.host = "bad host"; });
        row("host-control", "otg.error.host_characters", [](auto &o) { o.host = "host\nname"; });
        row("host-too-long", "otg.error.host_length", [](auto &o) { o.host = QString(1025, 'a'); });
        row("port-low", "otg.error.port_range", [](auto &o) { o.port = 0; });
        row("port-high", "otg.error.port_range", [](auto &o) { o.port = 65536; });
        row("multiplier-low", "otg.error.motion_multiplier", [](auto &o) { o.motionMultiplier = 0.99; });
        row("multiplier-high", "otg.error.motion_multiplier", [](auto &o) { o.motionMultiplier = 10.01; });
        row("multiplier-nan", "otg.error.motion_multiplier", [](auto &o) { o.motionMultiplier = std::numeric_limits<double>::quiet_NaN(); });
        row("multiplier-inf", "otg.error.motion_multiplier", [](auto &o) { o.motionMultiplier = std::numeric_limits<double>::infinity(); });
        row("relative-multiplier", "otg.error.relative_multiplier", [](auto &o) {
            o.mouseMode = "relative"; o.motionMultiplier = 2.0;
        });
        row("relative-per-axis", "otg.error.relative_per_axis", [](auto &o) {
            o.mouseMode = "relative"; o.absolutePerAxis = true;
        });
        row("width-low", "otg.error.width_range", [](auto &o) { o.width = 0; });
        row("width-high", "otg.error.width_range", [](auto &o) { o.width = 32768; });
        row("height-low", "otg.error.height_range", [](auto &o) { o.height = 0; });
        row("height-high", "otg.error.height_range", [](auto &o) { o.height = 32768; });
        row("first-invalid-field", "otg.error.port_range", [](auto &o) { o.port = 0; o.width = 0; });
#ifdef Q_OS_LINUX
        row("adapter-format", "otg.error.ble_adapter", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.adapter = "invalid";
        });
        row("adapter-range", "otg.error.ble_adapter", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.adapter = "hci65535";
        });
#elif defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN)
        row("windows-isolation", "otg.error.ble_isolate_windows", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.bleIsolate = true;
        });
        row("windows-adapter", "otg.error.ble_adapter_windows", [](auto &o) {
            o.transport = "ble"; o.blePeer = "AA:BB:CC:DD:EE:FF"; o.adapter = "hci0";
        });
#endif
    }
    void invalidConnectionOptions()
    {
        QFETCH(ConnectionOptions, options);
        QFETCH(QString, errorId);
        Session session;
        QSignalSpy logs(&session, &Session::log);
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
        QCOMPARE(logs.size(), 1);
        QCOMPARE(logs.first().first().toString(), session.statusMessage());
        auto &languages = LanguageManager::instance();
        const auto restore = qScopeGuard([&languages] { languages.setLanguage("zh_CN"); });
        QString english;
        for (const auto &language : {"en", "zh_CN"}) {
            QVERIFY(languages.setLanguage(language));
            const auto message = session.statusMessage();
            QCOMPARE(message, qtTrId(errorId.toLatin1().constData()));
            QVERIFY(!message.contains("otg.")); // Missing translation must not leak an ID.
            if (english.isEmpty()) english = message;
            else QVERIFY(message != english);
            if (!options.unlockPin.isEmpty()) QVERIFY(!message.contains(options.unlockPin));
        }
        if (!options.unlockPin.isEmpty()) QVERIFY(!logs.first().first().toString().contains(options.unlockPin));
    }
    void invalidServerOptions_data()
    {
        QTest::addColumn<ServerOptions>("options");
        QTest::addColumn<QString>("errorId");
        ServerOptions base;
        base.program = peerPath();
        base.configFile = QDir::tempPath(); // A directory is not an acceptable configuration file.
        const auto row = [base](const char *name, const char *id, auto change) {
            auto options = base;
            change(options);
            QTest::newRow(name) << options << QString::fromLatin1(id);
        };
        row("computer-empty", "otg.error.computer_screen_name", [](auto &o) { o.computer.clear(); });
        row("computer-invalid", "otg.error.computer_screen_name", [](auto &o) { o.computer = "bad name"; });
        row("config-empty", "otg.error.deskflow_config_required", [](auto &o) { o.configFile.clear(); });
        row("config-is-directory", "otg.error.deskflow_config_unreadable", [](auto &) {});
        row("port-low", "otg.error.port_range", [](auto &o) { o.port = 0; });
        row("port-high", "otg.error.port_range", [](auto &o) { o.port = 65536; });
        row("program-empty", "otg.error.deskflow_program_required", [](auto &o) { o.program.clear(); });
    }
    void invalidServerOptions()
    {
        QFETCH(ServerOptions, options);
        QFETCH(QString, errorId);
        DeskflowServer server;
        QSignalSpy failed(&server, &DeskflowServer::failed);
        QSignalSpy ready(&server, &DeskflowServer::ready);
        server.start(options);
        QCOMPARE(server.state(), DeskflowServer::State::Idle);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toString(), errorId);
        QCOMPARE(failed.first().at(1).toString(), errorId == "otg.error.deskflow_config_unreadable" ?
                 QFileInfo(options.configFile).absoluteFilePath() : QString());
        QVERIFY(ready.isEmpty());
    }
    void managerValidationMessages()
    {
        ConnectionManager manager;
        manager.addDevice("test");
        ConnectionOptions options;
        options.phone = "bad name";
        QVERIFY(!manager.start("test", options));
        QCOMPARE(manager.status("test"), qtTrId("otg.error.phone_screen_name"));
        options.phone = "android";
        options.deskflow.clear();
        QVERIFY(manager.start("test", options));
        QVERIFY(!manager.busy());
        QCOMPARE(manager.status("test"), qtTrId("otg.error.deskflow_program_required"));
    }
    void validOptionBoundaries_data()
    {
        QTest::addColumn<int>("edge");
        QTest::newRow("minimum") << 0;
        QTest::newRow("maximum") << 1;
    }
    void validationRetry()
    {
        ConnectionOptions options;
        options.serial = "TEST123"; options.bridge = peerPath(); options.manageServer = false;
        options.port = options.width = 0;
        Session session;
        session.start(options);
        QCOMPARE(session.statusMessage(), qtTrId("otg.error.port_range"));
        options.port = 24800;
        session.start(options);
        QCOMPARE(session.statusMessage(), qtTrId("otg.error.width_range"));
        options.width = 1080;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QVERIFY(!session.hasFailure());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
    }
    void validOptionBoundaries()
    {
        QFETCH(int, edge);
        ConnectionOptions options;
        options.serial = "TEST123"; options.bridge = peerPath(); options.manageServer = false;
        options.port = edge ? 65535 : 1;
        options.width = options.height = edge ? 32767 : 1;
        options.phone = QString(edge ? 63 : 1, 'a');
        options.sideSwipeDistancePercent = edge ? 100 : 1;
        options.unlockPin = QString(edge ? 32 : 4, '8');
        options.motionMultiplier = edge ? 10.0 : 1.0;
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QVERIFY(!session.hasFailure());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
    }
    void rightButtonAction_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("normal") << QString("normal");
        QTest::newRow("back") << QString("back");
        QTest::newRow("invalid") << QString("home");
    }
    void rightButtonAction()
    {
        QFETCH(QString, action);
        qputenv("OTG_TEST_RIGHT_ACTION", action.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_RIGHT_ACTION"); });
        ConnectionOptions options;
        options.serial = "TEST123";
        options.bridge = peerPath();
        options.manageServer = false;
        options.rightButtonAction = action;
        Session session;
        session.start(options);
        if (action == "home") {
            QCOMPARE(session.state(), Session::State::Idle);
            QVERIFY(session.hasFailure());
        } else {
            QTRY_COMPARE(session.state(), Session::State::Connected);
            session.stop();
            QTRY_COMPARE(session.state(), Session::State::Idle);
            QVERIFY(!session.hasFailure());
        }
    }
    void companionActions_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("status");
        QTest::addColumn<int>("commands");
        using A = CompanionApp::Action;
        QTest::newRow("install") << int(A::Install) << QByteArray() << QString("otg.companion.installed") << 2;
        QTest::newRow("signature-failure-no-uninstall") << int(A::Install) << QByteArray("install-failure") << QString("otg.companion.install_failed") << 2;
        QTest::newRow("offline") << int(A::Install) << QByteArray("offline") << QString("otg.companion.adb_failed") << 1;
        QTest::newRow("adb-failure") << int(A::Check) << QByteArray("command-failure") << QString("otg.companion.adb_failed") << 1;
        QTest::newRow("open") << int(A::Open) << QByteArray() << QString("otg.companion.opened") << 3;
        QTest::newRow("open-error-zero-exit") << int(A::Open) << QByteArray("open-failure") << QString("otg.companion.command_failed") << 3;
        QTest::newRow("open-timeout-zero-exit") << int(A::Open) << QByteArray("open-timeout") << QString("otg.companion.command_failed") << 3;
        QTest::newRow("query-package-failure") << int(A::Check) << QByteArray("pm-failure") << QString("otg.companion.command_failed") << 2;
        QTest::newRow("query-permission-failure") << int(A::Check) << QByteArray("settings-failure") << QString("otg.companion.command_failed") << 3;
        QTest::newRow("query-permission-error-zero-exit") << int(A::Check) << QByteArray("settings-error") << QString("otg.companion.command_failed") << 3;
        QTest::newRow("missing") << int(A::Check) << QByteArray("missing") << QString("otg.companion.not_installed") << 2;
        QTest::newRow("permission-required") << int(A::Check) << QByteArray() << QString("otg.companion.access_required") << 3;
        QTest::newRow("granted") << int(A::Check) << QByteArray("granted") << QString("otg.companion.access_enabled") << 3;
        QTest::newRow("short-granted") << int(A::Check) << QByteArray("short-granted") << QString("otg.companion.access_enabled") << 3;
    }
    void companionActions()
    {
        QFETCH(int, action); QFETCH(QByteArray, mode); QFETCH(QString, status); QFETCH(int, commands);
        QTemporaryDir dir;
        const auto apk = dir.filePath(QString::fromUtf8("伴侣 app.apk"));
        QFile file(apk);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fake apk"); file.close();
        const auto logPath = dir.filePath("adb.jsonl");
        qputenv("OTG_TEST_COMPANION", mode);
        qputenv("OTG_TEST_COMPANION_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_COMPANION"); qunsetenv("OTG_TEST_COMPANION_LOG"); });
        CompanionApp app;
        QSignalSpy completed(&app, &CompanionApp::completed);
        app.start(static_cast<CompanionApp::Action>(action), peerPath(), "[::1]:5555", apk);
        QVERIFY(app.busy());
        app.start(CompanionApp::Action::Open, peerPath(), "WRONG"); // No overlapping command or target switch.
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!app.busy());
        QCOMPARE(completed.first().first().toString(), status);
        QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
        const auto lines = log.readAll().trimmed().split('\n');
        QCOMPARE(lines.size(), commands);
        for (const auto &line : lines) {
            const auto args = QJsonDocument::fromJson(line).array();
            QCOMPARE(args.at(0).toString(), "-s");
            QCOMPARE(args.at(1).toString(), "[::1]:5555");
            const auto text = QString::fromUtf8(line);
            QVERIFY(!text.contains("uninstall")); QVERIFY(!text.contains("allow_listener"));
            QVERIFY(!text.contains("kill-server")); QVERIFY(!text.contains("connect"));
        }
        const auto args = QJsonDocument::fromJson(lines.last()).array();
        if (action == int(CompanionApp::Action::Install) && commands == 2) {
            QCOMPARE(args.at(2).toString(), "install");
            QCOMPARE(args.at(3).toString(), "-r");
            QCOMPARE(args.last().toString(), apk);
        }
    }
    void companionValidation()
    {
        CompanionApp app;
        QSignalSpy completed(&app, &CompanionApp::completed);
        app.start(CompanionApp::Action::Open, peerPath(), "");
        QCOMPARE(completed.takeFirst().first().toString(), "otg.companion.target_required");
        app.start(CompanionApp::Action::Open, peerPath(), "bad\nserial");
        QCOMPARE(completed.takeFirst().first().toString(), "otg.companion.target_required");
        app.start(CompanionApp::Action::Install, peerPath(), "TEST", "/missing.apk");
        QCOMPARE(completed.takeFirst().first().toString(), "otg.companion.apk_required");
        app.start(CompanionApp::Action::Open, "/nonexistent/adb", "TEST");
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toString(), "otg.companion.adb_failed");
        QVERIFY(!app.busy());
    }
    void companionCommandTimeout()
    {
        qputenv("OTG_TEST_COMPANION_DELAY", "500");
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_COMPANION_DELAY"); });
        bool done = false;
        runCommand(this, peerPath(), {"-s", "TEST", "get-state"}, [&](bool ok, const QString &error) {
            QVERIFY(!ok);
            QVERIFY(error.contains(qtTrId("otg.error.command_timeout")));
            done = true;
        }, 40);
        QTRY_VERIFY(done);
    }
    void dynamicDisplay_data()
    {
        QTest::addColumn<bool>("audio"); QTest::addColumn<bool>("clipboard");
        QTest::newRow("audio") << true << false;
        QTest::newRow("clipboard") << false << true;
        QTest::newRow("both") << true << true;
        QTest::newRow("no-adb-ignores-event") << false << false;
    }
    void dynamicDisplay()
    {
        QFETCH(bool, audio); QFETCH(bool, clipboard);
        qputenv("OTG_TEST_DISPLAY_UPDATES", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_DISPLAY_UPDATES"); });
        ConnectionOptions options;
        options.manageServer = false; options.bridge = options.adb = peerPath();
        options.serial = "rotation-phone";
        options.adbSerial = "192.0.2.1:5555"; // Windows requires an explicit ADB identity.
        options.scrcpyServer = "fake-server"; options.scrcpyVersion = "5.0.1";
        options.audio = audio; options.clipboard = clipboard; options.autoRotate = audio || clipboard;
        Session session;
        QSignalSpy sizes(&session, &Session::displaySizeChanged);
        QSignalSpy states(&session, &Session::changed);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        if (audio || clipboard) {
            QTRY_COMPARE(sizes.size(), 1);
            QCOMPARE(sizes.first().first().toSize(), QSize(1920, 1080));
        }
        QTest::qWait(450);
        QCOMPARE(sizes.size(), audio || clipboard ? 1 : 0);
        QCOMPARE(states.size(), 2); // Starting, Connected; never disconnected/reconnected.
        QVERIFY(!session.hasFailure());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QCOMPARE(sizes.size(), audio || clipboard ? 1 : 0);
    }
    void usbAdbTargetAppearsAfterHotplug_data()
    {
        QTest::addColumn<QString>("adbSerial");
        QTest::newRow("explicit-usb-target") << QStringLiteral("USB_ADB_ID");
        QTest::newRow("explicit-wireless-target") << QStringLiteral("192.0.2.1:5555");
#ifndef Q_OS_WIN
        // Windows requires a separate ADB identity; POSIX also supports AOA serial fallback.
        QTest::newRow("aoa-serial-fallback") << QString();
#endif
    }
    void usbAdbTargetAppearsAfterHotplug()
    {
        QFETCH(QString, adbSerial);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto attempts = dir.filePath("adb-attempts");
        qputenv("OTG_TEST_ADB_TRANSIENT_FILE", attempts.toUtf8());
        qputenv("OTG_TEST_ADB_TRANSIENT_ATTEMPTS", "3");
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_ADB_TRANSIENT_FILE");
            qunsetenv("OTG_TEST_ADB_TRANSIENT_ATTEMPTS");
        });
        ConnectionOptions options;
        options.manageServer = false;
        options.bridge = options.adb = peerPath();
        options.serial = "TEST123";
        options.adbSerial = adbSerial;
        options.audio = true;
        options.scrcpyServer = "fake-server";
        options.scrcpyVersion = "5.0.1";
        Session session;
        session.start(options);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::State::Connected, 6000);
        QVERIFY(!session.hasFailure());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QFile state(attempts);
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll().trimmed(), QByteArray("3"));
    }
    void clipboardSession_data()
    {
        QTest::addColumn<bool>("audio");
        QTest::addColumn<bool>("unavailable");
        QTest::newRow("clipboard-only") << false << false;
        QTest::newRow("audio-and-clipboard") << true << false;
        QTest::newRow("clipboard-failure-keeps-hid") << false << true;
    }
    void uhidSession_data()
    {
        QTest::addColumn<bool>("extras");
        QTest::addColumn<int>("audioGainDb");
        QTest::newRow("uhid-only") << false << 6;
        QTest::newRow("uhid-audio-clipboard") << true << 0;
        QTest::newRow("uhid-audio-gain") << true << 6;
    }
    void uhidSession()
    {
#ifdef OTG_GUI_HAVE_UHID
        QFETCH(bool, extras);
        QFETCH(int, audioGainDb);
        QTemporaryDir dir;
        const auto logPath = dir.filePath("calls.jsonl");
        qputenv("OTG_TEST_UHID_LOG", logPath.toUtf8());
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_UHID_LOG"); });
        ConnectionOptions options;
        options.manageServer = false;
        options.transport = "uhid"; options.serial = "STALE_USB_ID";
        options.adbSerial = "USB_ADB_ID";
        options.adb = options.bridge = peerPath();
        options.scrcpyServer = "fake-server"; options.scrcpyVersion = "5.0.1";
        options.audio = options.clipboard = extras;
        options.audioGainDb = audioGainDb;
        Session session;
        QSignalSpy usb(&session, &Session::usbAttached);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QCOMPARE(usb.size(), 0);
        session.stop(); QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasFailure());
        QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
        bool launched = false;
        for (const auto &line : log.readAll().split('\n')) {
            const auto args = QJsonDocument::fromJson(line).array();
            if (!args.contains("--gui")) continue;
            QVERIFY(!args.contains("--serial"));
            QVERIFY(args.contains("uhid") && args.contains("USB_ADB_ID"));
            QVERIFY(args.contains("--scrcpy-server"));
            QCOMPARE(args.contains("--clipboard"), extras);
            QCOMPARE(args.contains("--audio"), extras);
            const auto gainArg = args.toVariantList().indexOf(QStringLiteral("--audio-gain-db"));
            if (extras && audioGainDb) {
                QVERIFY(gainArg >= 0 && gainArg + 1 < args.size());
                QCOMPARE(args.at(gainArg + 1).toString(), QString::number(audioGainDb));
            } else {
                QCOMPARE(gainArg, -1);
            }
            launched = true;
        }
        QVERIFY(launched);
        options.adbSerial.clear(); // No fallback to stale USB identity, on any OS.
        session.start(options); QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
        options.adbSerial = "USB_ADB_ID"; options.adbEnabled = false;
        session.start(options); QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
#endif
    }
    void clipboardSession()
    {
        QFETCH(bool, audio); QFETCH(bool, unavailable);
        if (unavailable) qputenv("OTG_TEST_CLIPBOARD_FAIL", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_CLIPBOARD_FAIL"); });
        ConnectionOptions options;
        options.manageServer = false; options.bridge = options.adb = peerPath();
        options.serial = "clipboard-phone"; options.adbSerial = "192.0.2.1:5555";
        options.scrcpyServer = "fake-server"; options.scrcpyVersion = "5.0.1";
        options.audio = audio; options.clipboard = true;
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QTRY_COMPARE(session.clipboardStatusMessage(), qtTrId(unavailable ?
            "otg.clipboard.unavailable" : "otg.clipboard.ready"));
        QVERIFY(!session.hasFailure());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.clipboardStatusMessage(), qtTrId("otg.clipboard.disconnected"));
        options.adb.clear();
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
    }
    void multipleManagedSessions_data()
    {
        QTest::addColumn<QByteArray>("style");
        QTest::newRow("modern") << QByteArray("modern");
        QTest::newRow("legacy") << QByteArray("legacy-disable");
    }
    void multipleManagedSessions()
    {
        QFETCH(QByteArray, style);
        QTemporaryDir dir;
        const auto path = dir.filePath("starts.log");
        qputenv("OTG_TEST_HELP", style);
        qputenv("OTG_TEST_SERVER_START_LOG", path.toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_HELP"); qunsetenv("OTG_TEST_SERVER_START_LOG"); });
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        ConnectionOptions a;
        a.port = allocator.serverPort();
        allocator.close();
        a.bridge = a.deskflow = peerPath();
        a.serial = "SERIAL_A";
        a.phone = "a";
        QTemporaryFile config(dir.filePath("Deskflow layout.XXXXXX.conf"));
        QVERIFY(writeDeskflowConfig(config));
        a.deskflowConfig = config.fileName();
        auto b = a;
        b.serial = "SERIAL_B";
        b.phone = "b";
        ConnectionManager manager;
        manager.addDevice("a");
        manager.addDevice("b");
        QSignalSpy logs(&manager, &ConnectionManager::log);
        QVERIFY(manager.start("a", a));
        auto changedConfig = b;
        changedConfig.deskflowConfig += ".other";
        QVERIFY(!manager.start("b", changedConfig));
        QVERIFY(manager.start("b", b)); // Both wait on the same server startup.
        QTRY_COMPARE(manager.state("a"), Session::State::Connected);
        QTRY_COMPARE(manager.state("b"), Session::State::Connected);
        QFile starts(path);
        QVERIFY(starts.open(QIODevice::ReadOnly));
        QCOMPARE(starts.readAll(), QByteArray("server\n"));
        manager.stop("a");
        QTRY_COMPARE(manager.state("a"), Session::State::Idle);
        QCOMPARE(manager.state("b"), Session::State::Connected);
        QCOMPARE(manager.serverState(), DeskflowServer::State::Running);
        QVERIFY(manager.start("a", a));
        QTRY_COMPARE(manager.state("a"), Session::State::Connected);
        starts.seek(0);
        QCOMPARE(starts.readAll(), QByteArray("server\n"));
        manager.shutdown();
        QTRY_VERIFY(manager.settled());
        int released = 0;
        for (const auto &entry : logs) released += entry[1].toString().contains("released");
        QVERIFY(released >= 3);
        QVERIFY(allocator.listen(QHostAddress::LocalHost, quint16(a.port)));
    }
    void multiDeviceRejectionAndFailure()
    {
        ConnectionManager manager;
        manager.addDevice("a"); manager.addDevice("b"); manager.addDevice("duplicate");
        ConnectionOptions a;
        a.manageServer = false;
        a.bridge = peerPath();
        a.serial = "crash";
        a.phone = "a";
        auto b = a;
        b.serial = "B";
        b.phone = "b";
        QVERIFY(manager.start("a", a));
        QVERIFY(!manager.start("duplicate", a));
        auto changedServer = b;
        ++changedServer.port;
        QVERIFY(!manager.start("b", changedServer));
        QVERIFY(manager.start("b", b));
        QTRY_COMPARE(manager.state("a"), Session::State::Connected);
        QTRY_COMPARE(manager.state("b"), Session::State::Connected);
        QTRY_COMPARE(manager.state("a"), Session::State::Idle);
        QCOMPARE(manager.state("b"), Session::State::Connected);
        manager.shutdown();
        QTRY_VERIFY(manager.settled());
    }
    void cancelSharedStartup()
    {
        ConnectionManager manager;
        manager.addDevice("a");
        ConnectionOptions options;
        options.bridge = options.deskflow = peerPath();
        options.serial = "A";
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        options.port = allocator.serverPort(); allocator.close();
        QTemporaryFile config;
        QVERIFY(writeDeskflowConfig(config));
        options.deskflowConfig = config.fileName();
        QSignalSpy logs(&manager, &ConnectionManager::log);
        QVERIFY(manager.start("a", options));
        manager.stop("a");
        QTRY_VERIFY(manager.settled());
        QTest::qWait(150);
        for (const auto &entry : logs) QVERIFY(!entry[1].toString().contains("released"));
        QCOMPARE(manager.state("a"), Session::State::Idle);
    }
    void sharedServerCrash()
    {
        qputenv("OTG_TEST_SERVER_EXIT_MS", "1000");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_SERVER_EXIT_MS"); });
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        ConnectionOptions a;
        a.port = allocator.serverPort(); allocator.close();
        a.bridge = a.deskflow = peerPath(); a.serial = "A"; a.phone = "a";
        QTemporaryFile config;
        QVERIFY(writeDeskflowConfig(config));
        a.deskflowConfig = config.fileName();
        auto b = a; b.serial = "B"; b.phone = "b";
        ConnectionManager manager;
        manager.addDevice("a"); manager.addDevice("b");
        QVERIFY(manager.start("a", a));
        QVERIFY(manager.start("b", b));
        QTRY_COMPARE(manager.state("a"), Session::State::Connected);
        QTRY_COMPARE(manager.state("b"), Session::State::Connected);
        QTRY_VERIFY(manager.settled());
        QVERIFY(!manager.status("a").isEmpty());
    }
    void usbAndSingleBle()
    {
#if !defined(OTG_GUI_HAVE_BLE) && !defined(OTG_GUI_HAVE_BLE_WIN)
        QSKIP("BLE-enabled GUI only");
#endif
#ifdef Q_OS_WIN
        qputenv("OTG_TEST_BLE_WINDOWS", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_WINDOWS"); });
#endif
        ConnectionManager manager;
        manager.addDevice("usb"); manager.addDevice("ble"); manager.addDevice("other");
        ConnectionOptions usb;
        usb.bridge = peerPath(); usb.manageServer = false; usb.serial = "A"; usb.phone = "a";
        auto ble = usb; ble.transport = "ble"; ble.serial.clear(); ble.phone = "b";
        QVERIFY(manager.start("usb", usb));
        QVERIFY(manager.start("ble", ble));
        QTRY_COMPARE(manager.state("usb"), Session::State::Connected);
        QTRY_COMPARE(manager.state("ble"), Session::State::Connected);
        auto other = ble; other.phone = "c"; other.blePeer = "11:22:33:44:55:66";
        QVERIFY(!manager.start("other", other));
        manager.stop("ble");
        QTRY_COMPARE(manager.state("ble"), Session::State::Idle);
        QCOMPARE(manager.state("usb"), Session::State::Connected);
        manager.shutdown();
        QTRY_VERIFY(manager.settled());
    }
#ifdef OTG_GUI_HAVE_BLE
    void startsSharedBleOnlyOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto marker = dir.filePath("resident-service");
        qputenv("OTG_TEST_BLE_STATUS", "directed");
        qputenv("OTG_TEST_BLE_START_MARKER", marker.toUtf8());
        const auto restore = qScopeGuard([] {
            qunsetenv("OTG_TEST_BLE_STATUS");
            qunsetenv("OTG_TEST_BLE_START_MARKER");
        });
        ConnectionOptions options;
        options.bridge = peerPath(); options.manageServer = false; options.transport = "ble";
        options.blePrivilegeProgram = peerPath();
        options.blePeer = "AA:BB:CC:DD:EE:01";
        options.preferDirectedBle = true;
        QStringList confirmed;
        options.confirmSharedBleIsolation = [&confirmed](const QString &adapter) {
            confirmed << adapter;
            return true;
        };
        ConnectionManager manager;
        manager.addDevice("one");
        manager.addDevice("two");
        QVERIFY(manager.start("one", options));
        options.blePeer = "AA:BB:CC:DD:EE:02";
        options.phone = "android2";
        QVERIFY(manager.start("two", options));
        QTRY_COMPARE(manager.state("one"), Session::State::Connected);
        QTRY_COMPARE(manager.state("two"), Session::State::Connected);
        QCOMPARE(confirmed, QStringList{"hci0"});
        QVERIFY(QFile::exists(marker));
        manager.stop("one");
        QTRY_COMPARE(manager.state("one"), Session::State::Idle);
        QCOMPARE(manager.state("two"), Session::State::Connected);
        manager.shutdown();
        QTRY_VERIFY(manager.settled());
    }

    void refusesExistingDualModeDirectedService()
    {
        qputenv("OTG_TEST_BLE_STATUS", "directed-dual");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_STATUS"); });
        ConnectionOptions options;
        options.bridge = peerPath(); options.manageServer = false; options.transport = "ble";
        options.blePeer = "AA:BB:CC:DD:EE:01"; options.preferDirectedBle = true;
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.stop_unisolated_service"));
    }

    void multipleDirectedBleSessions()
    {
        qputenv("OTG_TEST_BLE_STATUS", "directed");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_STATUS"); });
        ConnectionManager manager;
        manager.addDevice("a"); manager.addDevice("b"); manager.addDevice("c");
        ConnectionOptions a;
        a.bridge = peerPath(); a.manageServer = false; a.transport = "ble";
        a.blePeer = "AA:BB:CC:DD:EE:01"; a.phone = "k20";
        auto b = a; b.phone = "r17"; b.blePeer = a.blePeer.toLower();
        QVERIFY(manager.start("a", a));
        QVERIFY(!manager.start("b", b));
        b.blePeer = "AA:BB:CC:DD:EE:02";
        QVERIFY(manager.start("b", b));
        auto c = a; c.phone = "third"; c.blePeer = "AA:BB:CC:DD:EE:03";
        QVERIFY(manager.start("c", c));
        QTRY_COMPARE(manager.state("a"), Session::State::Connected);
        QTRY_COMPARE(manager.state("b"), Session::State::Connected);
        QTRY_COMPARE(manager.state("c"), Session::State::Connected);
        manager.stop("a"); QTRY_COMPARE(manager.state("a"), Session::State::Idle);
        QCOMPARE(manager.state("b"), Session::State::Connected);
        QCOMPARE(manager.state("c"), Session::State::Connected);
        manager.shutdown(); QTRY_VERIFY(manager.settled());
    }
#endif
    void windowsBleProviderReuse()
    {
#ifdef OTG_TEST_NATIVE_BLE_PEER
        ConnectionOptions options;
        options.transport = "ble";
        options.bridge = OTG_TEST_NATIVE_BLE_PEER;
        options.manageServer = false;
        Session session;
        QString logs;
        connect(&session, &Session::log, this, [&logs](const QString &line) { logs += line + '\n'; });
        connect(&session, &Session::bleIdentityDetected, this, [&](const QString &identity) {
            QCOMPARE(identity, "AA:BB:CC:DD:EE:FF");
            options.blePeer = identity;
            QVERIFY(session.configureBle(options));
        });
        for (int i = 0; i < 3; ++i) {
            options.bleDiscovery = i != 1; // Initial discovery, manual reconnect, explicit discovery again.
            options.host = QString("127.0.0.%1").arg(i + 1);
            options.port = 24800 + i;
            options.phone = QString("phone-%1").arg(i);
            options.width = 540 + i;
            options.height = 1170 + i;
            options.rightButtonAction = i % 2 ? "back" : "normal";
            session.start(options);
            QTRY_COMPARE(session.state(), Session::State::Connected);
            QTRY_VERIFY(logs.contains(QString("session=%1,%2,%3,%4,%5")
                .arg(options.host).arg(options.port).arg(options.phone).arg(options.width).arg(options.height)));
            session.stop();
            QCOMPARE(session.state(), Session::State::Stopping);
            QTRY_COMPARE(session.state(), Session::State::Idle);
            QVERIFY(session.hasBleProvider());
            QTRY_COMPARE(logs.count("input-released"), i + 1);
            QCOMPARE(logs.count("back-shortcut"), (i + 1) / 2);
            QCOMPARE(logs.count("right-click"), i / 2 + 1);
            QCOMPARE(logs.count("provider-created"), 1);
            QVERIFY(!logs.contains("provider-closed"));
            const int sessions = logs.count("FAKE session=");
            QTest::qWait(120);
            QCOMPARE(logs.count("FAKE session="), sessions); // Idle is not an automatic TCP retry.
        }
        // USB can be used without destroying a parked Windows provider.
        auto usb = options;
        usb.transport = "usb"; usb.serial = "TEST123"; usb.bridge = peerPath();
        session.start(usb);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasBleProvider());
        auto other = options; other.blePeer = "11:22:33:44:55:66";
        other.bleDiscovery = false;
        auto changedTls = options;
        changedTls.tls = true;
        changedTls.tlsFingerprint = QString(64, 'a');
        session.start(changedTls);
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.provider_target"));
        QVERIFY(session.hasBleProvider());
        session.start(other);
        QCOMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.provider_target"));
        QVERIFY(session.hasBleProvider());
        QVERIFY(!logs.contains("provider-closed"));
        session.stopBleProvider();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasBleProvider());
        QTRY_COMPARE(logs.count("provider-closed"), 1);
        session.start(other); // Explicit service stop permits choosing another target.
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.shutdown(); // Also covers final exit with held key/button.
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasBleProvider());
        QTRY_COMPARE(logs.count("provider-closed"), 2);
#else
        QSKIP("Windows BLE native control harness only");
#endif
    }
    void windowsBleControlEof_data()
    {
        QTest::addColumn<int>("phase");
        QTest::newRow("active") << 0;
        QTest::newRow("parked") << 1;
        QTest::newRow("waiting-phone") << 2;
    }
    void windowsBleControlEof()
    {
#ifdef OTG_TEST_NATIVE_BLE_PEER
        QFETCH(int, phase);
        QProcess child;
        if (phase == 2) {
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("OTG_TEST_NATIVE_BLE_WAIT", "1");
            child.setProcessEnvironment(env);
        }
        child.start(OTG_TEST_NATIVE_BLE_PEER, {"--gui", "--gui-ble-control", "--transport", "ble",
                                           "--ble-peer", "AA:BB:CC:DD:EE:FF"});
        QVERIFY(child.waitForStarted());
        QByteArray events;
        QByteArray errors;
        auto connected = [&] { events += child.readAllStandardOutput(); return events.contains("connected"); };
        if (phase != 2) QTRY_VERIFY(connected());
        else {
            auto started = [&] { errors += child.readAllStandardError(); return errors.contains("provider-created"); };
            QTRY_VERIFY(started());
        }
        if (phase == 1) {
            child.write("pause\n");
            auto idle = [&] { events += child.readAllStandardOutput(); return events.contains("ble-idle"); };
            QTRY_VERIFY(idle());
        }
        child.closeWriteChannel(); // Parent crash / EOF while parked must not orphan a provider.
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
        errors += child.readAllStandardError();
        QCOMPARE(errors.count("provider-closed"), 1);
#else
        QSKIP("Windows BLE native control harness only");
#endif
    }
    void windowsBleReleaseTimeout()
    {
#ifdef OTG_TEST_NATIVE_BLE_PEER
        qputenv("OTG_TEST_NATIVE_BLE_RELEASE_STALL", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_NATIVE_BLE_RELEASE_STALL"); });
        ConnectionOptions options;
        options.transport = "ble"; options.blePeer = "AA:BB:CC:DD:EE:FF";
        options.bridge = OTG_TEST_NATIVE_BLE_PEER; options.manageServer = false;
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::State::Idle, 12000);
        QVERIFY(!session.hasBleProvider());
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.pause_failed"));
#else
        QSKIP("Windows BLE native control harness only");
#endif
    }
    void bleUnavailable()
    {
#if defined(Q_OS_LINUX) || defined(OTG_GUI_HAVE_BLE_WIN)
        QSKIP("BLE session platform available");
#endif
        ConnectionOptions options;
        options.bridge = peerPath();
        options.manageServer = false;
        options.transport = "ble";
        options.blePeer = "AA:BB:CC:DD:EE:FF";
        Session session;
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.unsupported"));
    }
    void bleLifecycle_data()
    {
        QTest::addColumn<bool>("manageServer");
        QTest::newRow("external-server") << false;
        QTest::newRow("managed-server") << true;
    }
    void bleLifecycle()
    {
#if !defined(Q_OS_LINUX) && !defined(OTG_GUI_HAVE_BLE_WIN)
        QSKIP("BLE-enabled platform only");
#endif
        QFETCH(bool, manageServer);
        ConnectionOptions options;
        options.manageServer = manageServer;
        options.deskflow = peerPath();
        QTemporaryFile config;
        QVERIFY(writeDeskflowConfig(config));
        options.deskflowConfig = config.fileName();
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        options.port = port.serverPort();
        port.close();
        options.bridge = peerPath();
        options.transport = "ble";
        options.blePeer = "AA:BB:CC:DD:EE:FF";
        Session session;
        QSignalSpy usb(&session, &Session::usbAttached);
        qputenv("OTG_TEST_BLE_RECONNECT", "1");
#ifdef Q_OS_WIN
        qputenv("OTG_TEST_BLE_WINDOWS", "1");
#endif
        const auto restore = qScopeGuard([] {
            qunsetenv("OTG_TEST_BLE_RECONNECT");
            qunsetenv("OTG_TEST_BLE_STATUS");
            qunsetenv("OTG_TEST_BLE_WINDOWS");
            qunsetenv("OTG_TEST_BLE_WAIT");
        });
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QTRY_COMPARE(session.state(), Session::State::Starting);
        QVERIFY(session.statusMessage().contains("配对保留"));
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QVERIFY(usb.isEmpty());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
#ifdef Q_OS_LINUX
        qputenv("OTG_TEST_BLE_STATUS", "busy");
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.statusMessage().contains("已有会话"));
        qunsetenv("OTG_TEST_BLE_STATUS");
        options.bleLegacyHandles = true;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.handle_layout_mismatch"));
        qputenv("OTG_TEST_BLE_STATUS", "legacy");
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        options.bleLegacyHandles = false;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), qtTrId("otg.ble.handle_layout_mismatch"));
        qunsetenv("OTG_TEST_BLE_STATUS");
#endif
        session.start(options);
        session.stop(); // Cancel an in-flight service probe, not another user's service.
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTest::qWait(150);
        QCOMPARE(session.state(), Session::State::Idle);
        options.blePeer = "not-a-mac";
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        options.blePeer = "AA:BB:CC:DD:EE:FF";
        options.mouseMode = "relative";
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
#ifdef Q_OS_WIN
        options.mouseMode = "absolute";
        options.bleIsolate = true;
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        options.bleIsolate = false;
        options.adapter = "hci0";
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        options.adapter.clear();
        session.stopBleProvider();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasBleProvider());
        // Stop while waiting for the phone (before any connected event).
        qputenv("OTG_TEST_BLE_WAIT", "1");
        session.start(options);
        QTRY_COMPARE(session.statusMessage(), qtTrId("otg.ble.waiting"));
        QTest::qWait(200);
        QCOMPARE(session.state(), Session::State::Starting);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        session.shutdown();
        QTRY_VERIFY(!session.hasBleProvider());
        QTRY_COMPARE(session.state(), Session::State::Idle);
#endif
    }
    void initTestCase()
    {
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
    }
    void translations()
    {
        auto &languages = LanguageManager::instance();
        QVERIFY(languages.setLanguage("en"));
        QCOMPARE(qtTrId("otg.action.connect"), "Connect");
        QVERIFY(!languages.setLanguage("unsupported"));
        QCOMPARE(languages.language(), "en");
        Session session;
        ConnectionOptions options;
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), "USB serial number is empty. Select the phone or enter its USB serial number.");
        QVERIFY(languages.setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "USB 序列号为空。请选择手机或填写手机的 USB 序列号。");
        QCOMPARE(qtTrId("otg.action.connect"), "连接");
    }
    void missingScrcpyRuntime_data()
    {
        QTest::addColumn<bool>("audio");
        QTest::addColumn<bool>("clipboard");
        QTest::addColumn<bool>("missingServer");
        QTest::newRow("audio-server") << true << false << true;
        QTest::newRow("audio-version") << true << false << false;
        QTest::newRow("clipboard-server") << false << true << true;
        QTest::newRow("clipboard-version") << false << true << false;
        QTest::newRow("both-server") << true << true << true;
        QTest::newRow("both-version") << true << true << false;
    }
    void missingScrcpyRuntime()
    {
        QFETCH(bool, audio);
        QFETCH(bool, clipboard);
        QFETCH(bool, missingServer);
        QTest::failOnWarning(QRegularExpression("QFile::remove:.*"));
        ConnectionOptions options;
        options.serial = "TEST";
        options.adb = options.bridge = peerPath();
        options.manageServer = false;
        options.audio = audio;
        options.clipboard = clipboard;
        options.scrcpyServer = missingServer ? QString() : QString("fake-server");
        options.scrcpyVersion = missingServer ? QString("5.0.1") : QString();
        Session session;
        QSignalSpy logs(&session, &Session::log);
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(session.hasFailure());
        QCOMPARE(session.statusMessage(), qtTrId("otg.error.scrcpy_runtime"));
        QCOMPARE(logs.size(), 1);
    }
    void noUnlockTemporaryFile()
    {
        QTest::failOnWarning(QRegularExpression("QFile::remove:.*"));
        ConnectionOptions options;
        Session session;
        session.start(options); // Validation failure before any PIN file exists.
        QCOMPARE(session.state(), Session::State::Idle);
        options.manageServer = false;
        options.serial = "TEST";
        options.bridge = peerPath();
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasFailure());
    }
    void tlsCertificateGeneration()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("tls/client.pem");
        QString error;
        QVERIFY2(TlsCertificate::generate(path, 2048, &error), qPrintable(error));
        QVERIFY(QFile::exists(path));
        QCOMPARE(TlsCertificate::keyLength(path), 2048);
        const auto fingerprint = TlsCertificate::fingerprint(path);
        QCOMPARE(fingerprint.size(), 95);
        QVERIFY(fingerprint.contains(':'));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto pem = file.readAll();
        file.close();
        std::unique_ptr<BIO, decltype(&BIO_free)> input(
            BIO_new_mem_buf(pem.constData(), int(pem.size())), BIO_free);
        QVERIFY(input);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        QVERIFY(key);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
        QVERIFY(certificate);
        QCOMPARE(X509_check_private_key(certificate.get(), key.get()), 1);
        QCOMPARE(X509_verify(certificate.get(), key.get()), 1);
#ifdef Q_OS_UNIX
        QVERIFY(!(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                            QFileDevice::ReadOther | QFileDevice::WriteOther)));
#endif
        QVERIFY(!TlsCertificate::generate(path, 1024, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(TlsCertificate::fingerprint(path), fingerprint);
        QVERIFY(TlsCertificate::generate(path, 4096, &error));
        QCOMPARE(TlsCertificate::keyLength(path), 4096);
        QVERIFY(TlsCertificate::fingerprint(path) != fingerprint);
    }
    void externalClientCertificate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("client identity.pem");
        QString error;
        QVERIFY2(TlsCertificate::generate(path, 2048, &error), qPrintable(error));
        qputenv("OTG_TEST_CLIENT_PEM", path.toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_CLIENT_PEM"); });
        ConnectionOptions options;
        options.manageServer = false;
        options.bridge = peerPath();
        options.serial = "TLS-client";
        options.tls = true;
        options.tlsCertificate = path;
        options.tlsFingerprint = QString(64, 'a'); // Server identity differs from the client.
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(!session.hasFailure());
    }
    void catalogs()
    {
        const auto english = readCatalog(":/i18n-test/gui/translations/deskflow_bridge_en.xml");
        const auto chinese = readCatalog(":/i18n-test/gui/translations/deskflow_bridge_zh_CN.xml");
        QVERIFY2(english.error.isEmpty(), qPrintable(english.error));
        QVERIFY2(chinese.error.isEmpty(), qPrintable(chinese.error));
        QCOMPARE(english.language, "en");
        QCOMPARE(chinese.language, "zh_CN");
        QVERIFY(!english.texts.isEmpty());
        QCOMPARE(english.texts.keys(), chinese.texts.keys());
        QCOMPARE(english.sources, chinese.sources);
        QCOMPARE(english.sources, english.texts);
        for (const auto &id : english.texts.keys())
            QCOMPARE(placeholders(english.texts[id]), placeholders(chinese.texts[id]));

        // Include indirect ID references in property bindings, states and arrays.
        QSet<QString> references;
        const QRegularExpression pattern("\"(otg\\.[a-z0-9_.]+)\"");
        for (const char *name : {"main_window.cpp", "session.cpp", "process_utils.cpp", "deskflow_server.cpp", "connection_manager.cpp", "companion_app.cpp", "companion_transfer.cpp", "wireless_adb_dialog.cpp", "transfer_dialog.cpp", "phone_clipboard.cpp", "phone_screenshot.cpp"}) {
            QFile source(QString(":/i18n-test/gui/") + name + ".txt");
            QVERIFY(source.open(QIODevice::ReadOnly));
            auto matches = pattern.globalMatch(QString::fromUtf8(source.readAll()));
            while (matches.hasNext())
                references.insert(matches.next().captured(1));
        }
        const auto ids = english.texts.keys();
        QCOMPARE(references, QSet<QString>(ids.begin(), ids.end()));

        // Also verify actual -idbased QM compilation and resource loading.
        auto &languages = LanguageManager::instance();
        const auto restore = qScopeGuard([&languages] { languages.setLanguage("zh_CN"); });
        QVERIFY(languages.setLanguage("en"));
        for (auto it = english.texts.cbegin(); it != english.texts.cend(); ++it)
            QCOMPARE(qtTrId(it.key().toUtf8().constData()), it.value());
        QVERIFY(languages.setLanguage("zh_CN"));
        for (auto it = chinese.texts.cbegin(); it != chinese.texts.cend(); ++it)
            QCOMPARE(qtTrId(it.key().toUtf8().constData()), it.value());
    }
    void englishFallback()
    {
        class IncompleteTranslator : public QTranslator {
        public:
            bool isEmpty() const override { return false; }
            QString translate(const char *, const char *id, const char *, int) const override
            {
                return QByteArray(id) == "otg.action.connect" ? QStringLiteral("连接") : QString();
            }
        } partial;
        auto &languages = LanguageManager::instance();
        const auto restore = qScopeGuard([&languages] { languages.setLanguage("zh_CN"); });
        QVERIFY(languages.setLanguage("en"));
        QVERIFY(QCoreApplication::installTranslator(&partial));
        QCOMPARE(qtTrId("otg.action.connect"), "连接");
        QCOMPARE(qtTrId("otg.action.refresh"), "Refresh devices");
        QCoreApplication::removeTranslator(&partial);
    }
    void devices()
    {
        auto devices = parseDevices("* daemon started successfully *\nList of devices attached\n"
            "ABC device usb:1-2 product:p model:Pixel_8 transport_id:1\n"
            "DEF unauthorized usb:1-3\nGHI offline\n"
            "emulator-5554 device\n192.168.1.2:5555 device\n"
            "adb-foo._adb-tls-connect._tcp device\n");
        QCOMPARE(devices.size(), 3);
        QCOMPARE(devices[0].serial, "ABC");
        QCOMPARE(devices[0].model, "Pixel 8");
        QCOMPARE(devices[1].state, "unauthorized");
    }

    void wirelessMdnsServices()
    {
        const auto services = parseWirelessAdbServices(
            "List of discovered mdns services\n"
            "adb-demo._adb-tls-pairing._tcp\n"
            "192.0.2.10:37123\n"
            "adb-demo._adb-tls-connect._tcp\n"
            "192.0.2.10:39833\n"
            "adb-demo._adb-tls-connect._tcp 192.0.2.10:39833\n");
        QCOMPARE(services.size(), 2);
        QCOMPARE(services[0].target, "192.0.2.10:37123");
        QVERIFY(services[0].pairing);
        QCOMPARE(services[1].target, "192.0.2.10:39833");
        QVERIFY(!services[1].pairing);

        const auto ipv6 = parseWirelessAdbServices(
            "adb-demo._adb-tls-connect._tcp\n[2001:db8::10]:39833\n");
        QCOMPARE(ipv6.size(), 1);
        QCOMPARE(ipv6.first().target, "[2001:db8::10]:39833");
        QVERIFY(!ipv6.first().pairing);
    }
    void usbScanDevices()
    {
        const auto devices = parseUsbScanDevices(
            R"({"version":1,"devices":[{"id":"1:2","vid":1234,"pid":5678,"serial":"USB123"},{"id":"1:3","serial":""}]})");
        QCOMPARE(devices.size(), 1);
        QCOMPARE(devices[0].serial, "USB123");
        QCOMPARE(devices[0].state, "device");
        QVERIFY(parseUsbScanDevices("not json").isEmpty());
    }
    void dimensions()
    {
        QCOMPARE(parseScreenSize("Physical size: 1080x2400\r\nOverride size: 720x1600\r\n"), QSize(720, 1600));
        QCOMPARE(parseScreenSize("Override size: 720x1600\nPhysical size: 1080x2400"), QSize(720, 1600));
        QVERIFY(!parseScreenSize("error: device unauthorized").isValid());
        QVERIFY(!parseScreenSize("Physical size: 40000x0").isValid());
        QCOMPARE(parseRotation("SurfaceOrientation: 3\n"), 3);
        QCOMPARE(parseRotation("Viewport EXTERNAL: displayId=1, orientation=3\n"
                               "Viewport INTERNAL: displayId=0, uniqueId='local:1', orientation=1\n"), 1);
        QCOMPARE(parseRotation("unknown"), -1);
    }
    void screenNameValidation()
    {
        QVERIFY(validScreenName("phone-1.local"));
        QVERIFY(!validScreenName("pc\nend"));
        QVERIFY(!validScreenName("-c"));
    }
    void failedCommand()
    {
        bool finished = false;
        runCommand(this, "/nonexistent/deskflow-bridge-command", {}, [&](bool ok, const QString &error) {
            QVERIFY(!ok);
            QVERIFY(!error.isEmpty());
            finished = true;
        });
        QTRY_VERIFY(finished);
    }
    void occupiedPort()
    {
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost));
        Session session;
        QSignalSpy changes(&session, &Session::changed);
        ConnectionOptions options;
        options.port = occupied.serverPort();
        options.serial = "ABC";
        options.deskflow = peerPath();
        QTemporaryFile config;
        QVERIFY(writeDeskflowConfig(config));
        options.deskflowConfig = config.fileName();
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(changes.last()[1].toString().contains("端口"));
        QVERIFY(occupied.isListening());
    }
    void missingBridge()
    {
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "ABC";
        options.bridge = "/nonexistent/deskflow-bridge";
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        const QString detail = session.statusMessage().mid(QString("桥接启动失败：").size());
        QVERIFY(!detail.isEmpty());
        QVERIFY(LanguageManager::instance().setLanguage("en"));
        QCOMPARE(session.statusMessage(), "Failed to start bridge: " + detail);
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "桥接启动失败：" + detail);
    }
    void managedLifecycle_data()
    {
        QTest::addColumn<QByteArray>("style");
        QTest::newRow("modern") << QByteArray("modern");
        QTest::newRow("legacy-optin") << QByteArray("legacy-optin");
        QTest::newRow("legacy-disable") << QByteArray("legacy-disable");
        QTest::newRow("legacy-settings") << QByteArray("legacy-settings");
    }
    void managedLifecycle()
    {
        QFETCH(QByteArray, style);
        QTemporaryFile config;
        QVERIFY(writeDeskflowConfig(config));
        qputenv("OTG_TEST_HELP", style);
        qputenv("OTG_TEST_DESKFLOW_CONFIG", config.fileName().toUtf8());
        const auto restore = qScopeGuard([] {
            qunsetenv("OTG_TEST_HELP");
            qunsetenv("OTG_TEST_DESKFLOW_CONFIG");
        });
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        ConnectionOptions options;
        options.port = allocator.serverPort();
        allocator.close();
        options.serial = "ABC";
        options.bridge = peerPath();
        options.deskflow = peerPath();
        options.deskflowConfig = config.fileName();
        Session session;
        QSignalSpy logs(&session, &Session::log);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QVERIFY(LanguageManager::instance().setLanguage("en"));
        QCOMPARE(session.statusMessage(), "Connected — move the mouse toward the phone to control it");
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "已连接 — 将鼠标移到手机一侧即可控制");
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::State::Idle, 6000);
        bool released = false;
        for (const auto &entry : logs)
            released |= entry[0].toString().contains("released");
        QVERIFY(released);
        QVERIFY(config.open());
        QCOMPARE(config.readAll(), deskflowConfig);
        QVERIFY(allocator.listen(QHostAddress::LocalHost, quint16(options.port)));
    }
    void externalLifecycle()
    {
        QTcpServer external;
        QVERIFY(external.listen(QHostAddress::LocalHost));
        ConnectionOptions options;
        options.manageServer = false;
        options.port = external.serverPort();
        options.serial = "ABC";
        options.bridge = peerPath();
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(external.isListening());
        // Cancellation while startup has not emitted connected.
        options.serial = "slow";
        session.start(options);
        QTRY_COMPARE(session.statusMessage(), qtTrId("otg.status.waiting_for_screen").arg(options.phone));
        QCOMPARE(session.state(), Session::State::Starting);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        // A process which starts successfully but rejects the device isn't connected.
        options.serial = "reject";
        QSignalSpy states(&session, &Session::changed);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        for (const auto &entry : states)
            QVERIFY(qvariant_cast<Session::State>(entry[0]) != Session::State::Connected);
    }
};

QTEST_GUILESS_MAIN(GuiTest)
#include "test_gui.moc"
