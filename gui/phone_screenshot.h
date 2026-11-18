// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcess>
#include <QTimer>

// A user-requested, single-frame copy to the GUI host's clipboard.
class PhoneScreenshot : public QObject {
    Q_OBJECT
public:
    explicit PhoneScreenshot(QObject *parent = nullptr);
    ~PhoneScreenshot() override;
    bool busy() const { return m_busy; }
    bool start(const QString &adb, const QString &target, int timeoutMs = 20000);
    void cancel();
signals:
    // A translated message ID, or empty when cancelled during shutdown.
    void completed(const QString &messageId);
private:
    void readOutput();
    void abort(const char *reason);
    void finish(const char *messageId);
    QProcess m_process;
    QTimer m_deadline;
    QByteArray m_png, m_stderr;
    const char *m_failure = nullptr;
    bool m_busy = false;
};
