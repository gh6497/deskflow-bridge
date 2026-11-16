// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "system_theme.h"
#include "build_version.h"
#include <QApplication>
#include <QSettings>
#include <cstdio>
#include <cstring>

namespace {

void migrateLegacySettings()
{
    QSettings current;
    if (!current.allKeys().isEmpty())
        return;

    // Keep existing device and server profiles when upgrading from the old
    // application identity. The Android package identity is preserved for the
    // same reason: it is part of the companion broadcast and provider API.
    QSettings legacy("deskflow-otg", "deskflow-otg-gui");
    const auto keys = legacy.allKeys();
    if (keys.isEmpty())
        return;
    for (const auto &key : keys)
        current.setValue(key, legacy.value(key));
    current.sync();
}

} // namespace

int main(int argc, char **argv)
{
    // Version queries work without a graphical desktop or loading Qt platform plugins.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("deskflow-bridge-gui %s (commit %s)\n", OTG_VERSION, OTG_COMMIT);
            return 0;
        }
    }
    QApplication app(argc, argv);
    setupSystemTheme(app);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("deskflow-bridge");
    QCoreApplication::setApplicationName("deskflow-bridge-gui");
    QCoreApplication::setApplicationVersion(OTG_VERSION);
    migrateLegacySettings();
    MainWindow window;
    QObject::connect(&window, &MainWindow::exitReady, &app, &QCoreApplication::quit);
    window.show();
    return app.exec();
}
