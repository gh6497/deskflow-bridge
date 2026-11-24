// Mock ADB, Deskflow core and bridge for process lifecycle regression tests.
// SPDX-License-Identifier: Apache-2.0
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <thread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

static void output(const char *text)
{
    std::fputs(text, stdout);
    std::fflush(stdout);
}

static void appendLog(const QString &path, const QByteArray &line)
{
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(10000);
    if (!lock.tryLock(5000)) return;
    QFile log(path);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) log.write(line);
}

static void appendArgumentsLog(const QString &path, const QStringList &args)
{
    appendLog(path, QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto uhidLog = qEnvironmentVariable("OTG_TEST_UHID_LOG");
    if (!uhidLog.isEmpty()) appendArgumentsLog(uhidLog, args.mid(1));
    // The window test owns the loopback TCP peer that simulates scrcpy.
    const auto clipboardPort = qgetenv("OTG_TEST_CLIPBOARD_PORT");
    if (!clipboardPort.isEmpty() && args.value(1) == "-s" && !args.value(2).isEmpty()) {
        const auto mode = qgetenv("OTG_TEST_CLIPBOARD_ADB");
        if (args.value(3) == "get-state") {
            output(mode == "offline" ? "unauthorized\n" : "device\n");
            return mode == "offline" ? 1 : 0;
        }
        if (args.value(3) == "push") return args.size() == 6 && mode != "push-failed" ? 0 : 1;
        if (args.value(3) == "forward") {
            if (args.value(4) == "tcp:0" && args.value(5).startsWith("localabstract:scrcpy_")) {
                output((clipboardPort + '\n').constData());
                return 0;
            }
            return args.mid(4) == QStringList{"--remove", QStringLiteral("tcp:") + QString::fromLatin1(clipboardPort)} ? 0 : 1;
        }
        if (args.mid(3, 3) == QStringList{"shell", "-T", "-n"}) {
            if (args.value(6).startsWith("CLASSPATH="))
                return args.contains("clipboard_autosync=false") && !args.contains("control=false") ? app.exec() : 1;
            if (args.mid(6, 2) == QStringList{"rm", "-f"}) return 0;
        }
    }
    if (args.contains("exec-out")) {
        if (args.size() != 6 || args[1] != "-s" || args[2].isEmpty() ||
            args.mid(3) != QStringList{"exec-out", "screencap", "-p"}) return 2;
        const auto mode = qgetenv("OTG_TEST_SHOT_MODE");
        if (mode == "hang") return app.exec();
        if (mode == "unauthorized") { std::fputs("adb: device unauthorized\n", stderr); return 1; }
#ifdef Q_OS_WIN
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::fputs("fake adb diagnostic on stderr\n", stderr);
        QFile png(qEnvironmentVariable("OTG_TEST_SHOT_FILE"));
        if (!png.open(QIODevice::ReadOnly)) return 2;
        while (!png.atEnd()) {
            const auto bytes = png.read(65536);
            if (bytes.isEmpty() || std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) != size_t(bytes.size())) return 2;
        }
        return mode == "fail" ? 1 : 0;
    }
    if (args.contains("mdns") || args.contains("pair") || args.contains("connect")) {
        const auto mode = qEnvironmentVariable("OTG_TEST_WIRELESS_ADB");
        if (args.contains("mdns")) {
            output("List of discovered mdns services\n"
                   "adb-test._adb-tls-pairing._tcp\n"
                   "192.0.2.10:37123\n"
                   "adb-test._adb-tls-connect._tcp\n"
                   "192.0.2.10:39833\n");
            return 0;
        }
        if (args.contains("pair")) {
            if (mode == "pair-failure") return 1;
            output("Successfully paired to 192.0.2.10:37123\n");
            return 0;
        }
        if (mode == "connect-failure") return 1;
        output("connected to 192.0.2.10:39833\n");
        return 0;
    }
    if (args.contains("start-server") || args.contains("get-state") || args.contains("get-devpath") || args.contains("install") ||
        args.contains("com.github.gh6497.deskflowotg") || args.contains("enabled_notification_listeners") ||
        args.contains("com.github.gh6497.deskflowotg/.MainActivity") ||
        args.contains("com.github.gh6497.deskflowotg/.ConnectionStatusReceiver")) {
        const auto logPath = qEnvironmentVariable("OTG_TEST_COMPANION_LOG");
        if (!logPath.isEmpty()) appendArgumentsLog(logPath, args.mid(1));
        const auto mode = qEnvironmentVariable("OTG_TEST_COMPANION");
        if (mode == "command-failure") return 1;
        if (mode == "pm-failure" && args.contains("pm")) return 1;
        if (mode == "settings-failure" && args.contains("settings")) return 1;
        const char *text = "";
        if (args.contains("start-server")) text = "";
        else if (args.contains("get-state")) {
            const auto transientFile = qEnvironmentVariable("OTG_TEST_ADB_TRANSIENT_FILE");
            const int transientAttempts = qEnvironmentVariableIntValue("OTG_TEST_ADB_TRANSIENT_ATTEMPTS");
            if (!transientFile.isEmpty() && transientAttempts > 0) {
                int attempt = 0;
                QFile state(transientFile);
                if (state.open(QIODevice::ReadOnly))
                    attempt = QString::fromUtf8(state.readAll()).trimmed().toInt();
                if (attempt < transientAttempts) {
                    QFile next(transientFile);
                    if (next.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        next.write(QByteArray::number(attempt + 1));
                    output("error: device TEST123 not found\n");
                    return 1;
                }
            }
            text = mode == "offline" ? "unauthorized\n" : "device\n";
        }
        else if (args.contains("get-devpath")) text = mode == "wired" ? "usb:5-1\n" : "unknown\n";
        else if (args.contains("install")) text = mode == "install-failure" ?
            "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]\n" : "Success\n";
        else if (args.contains("pm")) text = mode == "missing" ? "" : "package:/data/app/companion/base.apk\n";
        else if (args.contains("am")) text = mode == "open-failure" ? "Error type 3\nError: Activity does not exist\n" :
            mode == "open-timeout" ? "Status: timeout\n" : "Status: ok\n";
        else if (mode == "settings-error") text = "Error while accessing settings provider\n";
        else if (mode == "granted") text = "com.github.gh6497.deskflowotg/com.github.gh6497.deskflowotg.DeskflowNotificationListenerService\n";
        else if (mode == "short-granted") text = "other.app/.Listener:com.github.gh6497.deskflowotg/.DeskflowNotificationListenerService\n";
        else text = "com.github.gh6497.deskflowotg.fake/.DeskflowNotificationListenerService\n";
        const int delay = qEnvironmentVariableIntValue("OTG_TEST_COMPANION_DELAY");
        if (delay > 0) {
            QTimer::singleShot(delay, &app, [text, &app] { output(text); app.quit(); });
            return app.exec();
        }
        output(text);
        return 0;
    }
    if (qEnvironmentVariableIsSet("OTG_TEST_BLE_WINDOWS") &&
        (args.contains("--ble-status") || args.contains("--ble-service"))) return 10;
    if (args.contains("--ble-status")) {
        const auto mode = qEnvironmentVariable("OTG_TEST_BLE_STATUS");
        if (mode == "busy")
            output("{\"version\":2,\"available\":true,\"idle\":false,\"ready\":true}\n");
        else if (mode == "incompatible")
            output("{\"version\":1,\"available\":true,\"idle\":true}\n");
        else
            output("{\"version\":2,\"available\":true,\"idle\":true,\"ready\":false}\n");
        return 0;
    }
    if (args.contains("--ble-service")) return 2; // Never start a real daemon in tests.
    if (args.contains("--usb-watch")) {
        const auto mode = qEnvironmentVariable("OTG_TEST_USB_WATCH");
        if (mode == "registration-failure")
            return 1;
        if (mode == "timeout")
            return app.exec();
        if (mode == "malformed") {
            output("invalid JSON\n");
            return app.exec();
        }
        if (mode != "events" && mode != "exit-after-ready")
            return 3; // Default mock backend does not support hotplug.
        output("{\"event\":\"watch-");
        QTimer::singleShot(20, &app, [] { output("ready\",\"version\":1}\n"); });
        if (mode == "exit-after-ready") {
            QTimer::singleShot(100, &app, &QCoreApplication::quit);
        } else {
            // Tests inject notifications through stdin; unlike the real watcher,
            // this mock deliberately supports arbitrary/malformed event lines.
            std::thread([&app] {
                char buffer[4096];
                while (std::fgets(buffer, sizeof(buffer), stdin)) {
                    const QByteArray line(buffer);
                    QMetaObject::invokeMethod(&app, [line] { output(line.constData()); }, Qt::QueuedConnection);
                }
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            }).detach();
        }
        return app.exec();
    }
    if (args.contains("--usb-scan")) {
        const auto logPath = qEnvironmentVariable("OTG_TEST_USB_SCAN_LOG");
        if (!logPath.isEmpty()) appendLog(logPath, "scan\n");
        const auto path = qEnvironmentVariable("OTG_TEST_USB_SNAPSHOT");
        QByteArray data;
        if (!path.isEmpty()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return 7;
            data = file.readAll();
        } else {
            data = "{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"TEST123\"}]}\n";
        }
        const int delay = qEnvironmentVariableIntValue("OTG_TEST_USB_SCAN_DELAY");
        if (delay > 0) {
            QTimer::singleShot(delay, &app, [data, &app] {
                output(data.constData());
                app.quit();
            });
            return app.exec();
        }
        output(data.constData());
        return 0;
    }
    if (args.contains("devices")) {
        const auto devices = qgetenv("OTG_TEST_ADB_DEVICES");
        output(devices.isEmpty() ? "List of devices attached\nTEST123 device usb:1-2 model:Pixel_Test transport_id:1\n" : devices.constData());
        return 0;
    }
    if (args.contains("get-devpath")) {
        output(qEnvironmentVariableIsSet("OTG_TEST_NO_DEVPATH") ? "unknown\n" : "usb:1-2\n");
        return 0;
    }
    if (args.contains("wm")) {
        output("Physical size: 1080x2400\nOverride size: 720x1600\n");
        return 0;
    }
    if (args.contains("dumpsys")) {
        output("SurfaceOrientation: 1\n");
        return 0;
    }
    if (args.contains("--help")) {
        const auto style = qEnvironmentVariable("OTG_TEST_HELP");
        if (style.startsWith("legacy")) {
            output("deskflow-server test\n--no-daemon --name --config --address\n");
            output(style == "legacy-optin" ? "--enable-crypto\n" :
                   style == "legacy-settings" ? "--settings <file>\n" : "--disable-crypto\n");
        } else {
            output("deskflow-core test\nUsage: coremode [options]\n--settings <file>\n");
        }
        return 0;
    }
    QTcpServer server;
    if (args.value(1) == "server" || args.contains("--no-daemon")) {
        const auto logPath = qEnvironmentVariable("OTG_TEST_SERVER_START_LOG");
        if (!logPath.isEmpty()) appendLog(logPath, "server\n");
        const int exitDelay = qEnvironmentVariableIntValue("OTG_TEST_SERVER_EXIT_MS");
        if (exitDelay > 0) QTimer::singleShot(exitDelay, &app, [] { QCoreApplication::exit(11); });
        QString layoutPath;
        int port = 0;
        if (args.contains("--settings")) {
            QSettings settings(args.value(args.indexOf("--settings") + 1), QSettings::IniFormat);
            if (settings.value("security/tlsEnabled", true).toBool())
                return 2;
            layoutPath = settings.value("server/externalConfigFile").toString();
            port = settings.value("core/port").toInt();
        } else {
            if (args.contains("--enable-crypto"))
                return 2;
            layoutPath = args.value(args.indexOf("--config") + 1);
            port = args.value(args.indexOf("--address") + 1).section(':', -1).toInt();
        }
        QFile layout(layoutPath);
        if (!layout.open(QIODevice::ReadOnly) || !layout.readAll().contains("section: links"))
            return 3;
        if (!port || !server.listen(QHostAddress::LocalHost, quint16(port)))
            return 4;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
            while (server.hasPendingConnections())
                server.nextPendingConnection()->deleteLater();
        });
    } else if (args.contains("--gui")) {
        const auto expectedRight = qEnvironmentVariable("OTG_TEST_RIGHT_ACTION");
        if (!expectedRight.isEmpty() &&
            (!args.contains("--right-button-action") ||
             args.value(args.indexOf("--right-button-action") + 1) != expectedRight)) return 13;
        const auto clientPem = qEnvironmentVariable("OTG_TEST_CLIENT_PEM");
        if (!clientPem.isEmpty() &&
            (!args.contains("--tls") || !args.contains("--tls-cert") ||
             args.value(args.indexOf("--tls-cert") + 1) != clientPem ||
             !args.contains("--tls-fingerprint") ||
             args.value(args.indexOf("--tls-fingerprint") + 1) != QString(64, 'a') ||
             args.contains("--tls-fingerprint-file"))) return 12;
        if (qEnvironmentVariableIsSet("OTG_TEST_DISPLAY_UPDATES")) {
            QTimer::singleShot(250, &app, [] {
                output("{\"event\":\"display-size\",\"width\":1920,\"height\":1080,\"virtual-width\":960,\"virtual-height\":540}\n");
            });
            QTimer::singleShot(350, &app, [] {
                output("{\"event\":\"display-size\",\"width\":0,\"height\":32768,\"virtual-width\":1,\"virtual-height\":1}\n");
            });
        }
        if (args.contains("--clipboard")) {
            if (!args.contains("--adb") || !args.contains("--scrcpy-server") ||
                !args.contains("--scrcpy-version")) return 11;
            QTimer::singleShot(150, &app, [] {
                output(qEnvironmentVariableIsSet("OTG_TEST_CLIPBOARD_FAIL") ?
                    "{\"event\":\"clipboard-unavailable\"}\n" : "{\"event\":\"clipboard-ready\"}\n");
            });
        }
        if (args.contains("--audio")) {
            const auto mode = qEnvironmentVariable("OTG_TEST_AUDIO_READY");
            // Emit complete lines before the deliberately split connected line;
            // never interleave another JSON event inside its two fragments.
            if (mode == "early") output("{\"event\":\"audio-ready\"}\n");
            else if (mode != "never") {
                QTimer::singleShot(300, &app, [] {
                    output("{\"event\":\"audio-ready\"}\n");
                });
            }
        }
        const bool ble = args.value(args.indexOf("--transport") + 1) == "ble";
        const bool uhid = args.value(args.indexOf("--transport") + 1) == "uhid";
        if (uhid && (args.contains("--serial") || !args.contains("--adb-serial") ||
                     !args.contains("--scrcpy-server") || !args.contains("--scrcpy-version") ||
                     args.contains("--ble-peer"))) return 14;
        if (ble && (args.contains("--serial") ||
                    args.value(args.indexOf("--mouse-mode") + 1) != "absolute")) return 9;
        if (ble && args.contains("--ble-peer") && args.value(args.indexOf("--ble-peer") + 1).isEmpty()) return 9;
        if (ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_WINDOWS") &&
            (args.contains("--ble-session") || args.contains("--adapter") ||
             args.contains("--ble-isolate") || args.contains("--background"))) return 10;
        if (args.contains("--gui-ble-control")) {
            auto generation = std::make_shared<int>(0);
            auto connectSession = [&app, generation] {
                const int current = ++*generation;
                if (qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT")) return;
                QTimer::singleShot(100, &app, [generation, current] {
                    if (*generation == current) output("{\"event\":\"connected\"}\n");
                });
                if (qEnvironmentVariableIsSet("OTG_TEST_BLE_RECONNECT")) {
                    QTimer::singleShot(250, &app, [generation, current] {
                        if (*generation == current) output("{\"event\":\"disconnected\"}\n");
                    });
                    QTimer::singleShot(600, &app, [generation, current] {
                        if (*generation == current) output("{\"event\":\"connected\"}\n");
                    });
                }
            };
            connectSession();
            std::thread([&app, generation, connectSession] {
                char bytes[1600];
                while (std::fgets(bytes, sizeof(bytes), stdin)) {
                    const QByteArray line(bytes);
                    QMetaObject::invokeMethod(&app, [&app, generation, connectSession, line] {
                        if (line == "pause\n") {
                            ++*generation;
                            output("{\"event\":\"ble-idle\"}\n");
                        } else if (line.startsWith("resume\t")) connectSession();
                        else app.quit();
                    }, Qt::QueuedConnection);
                }
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            }).detach();
            return app.exec();
        }
        const auto serial = args.value(args.indexOf("--serial") + 1);
        if (serial == "reject")
            return 5;
        // Bypass libc stream cleanup: the fake's input thread is blocked in getchar().
        if (serial == "crash") QTimer::singleShot(600, &app, [] { std::_Exit(12); });
        if (uhid) output("{\"event\":\"uhid-ready\"}\n");
        else if (!ble) output("{\"event\":\"usb-ready\",\"usb-id\":\"1:2\"}\n");
        if (ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_RECONNECT") &&
            !qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT")) {
            QTimer::singleShot(250, &app, [] { output("{\"event\":\"disconnected\"}\n"); });
            QTimer::singleShot(600, &app, [] { output("{\"event\":\"connected\"}\n"); });
        }
        if (serial != "slow" && !(ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT"))) {
            // Split across reads to exercise the line protocol framing.
            output("{\"event\":");
            QTimer::singleShot(100, &app, [] { output("\"connected\"}\n"); });
        }
        std::thread([&app] {
            (void)std::getchar();
            std::fputs("released\n", stderr);
            std::fflush(stderr);
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        }).detach();
    } else {
        return 6;
    }
    return app.exec();
}
