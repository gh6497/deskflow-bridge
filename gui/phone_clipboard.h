// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcess>
#include <QTcpSocket>
#include <QTimer>

// One explicit transfer using the selected ADB device and the local system clipboard.
class PhoneClipboard : public QObject {
    Q_OBJECT
public:
    enum class Direction { FromPhone, ToPhone };

    explicit PhoneClipboard(QObject *parent = nullptr);
    ~PhoneClipboard() override;
    bool busy() const { return m_stage != Stage::Idle; }
    void start(const QString &adb, const QString &target, const QString &server,
               const QString &version, Direction direction);
    void cancel();

signals:
    void completed(const QString &messageId);

private:
    enum class Stage { Idle, CheckDevice, Push, Forward, Exchange, FindForward, RemoveForward, RemoveJar, Finishing };
    enum class Reply { Dummy, Text, Ack, Verify };

    void command(Stage stage, const QStringList &args);
    void readCommandOutput();
    void commandFinished(bool ok);
    void startServer();
    void connectSocket();
    void socketLost();
    void readReply();
    void cleanup(const char *messageId);
    void removeForward();
    void removeJar();
    void finishIfStopped();

    QProcess m_command, m_server;
    QTcpSocket m_socket;
    QTimer m_deadline, m_retry;
    Stage m_stage = Stage::Idle;
    Reply m_reply = Reply::Dummy;
    Direction m_direction = Direction::FromPhone;
    bool m_cancelled = false;
    QString m_adb, m_target, m_serverFile, m_version, m_remoteJar, m_scid, m_remoteSocket;
    quint16 m_port = 0;
    QByteArray m_commandOutput, m_sendText;
    QString m_receivedText;
    const char *m_result = nullptr;
};
