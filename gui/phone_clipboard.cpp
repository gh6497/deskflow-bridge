// scrcpy 5.0.1 control and device messages. SPDX-License-Identifier: Apache-2.0
#include "phone_clipboard.h"
#include "clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUuid>
#include <QtEndian>
#include <utility>

namespace {
constexpr int CommandTimeoutMs = 5000;
constexpr int ReplyTimeoutMs = 3000;
constexpr qsizetype MaxCommandOutput = 64 * 1024;
constexpr quint64 WriteSequence = 1;
// GET_CLIPBOARD with COPY_KEY_NONE reads the current clipboard without injecting input.
constexpr char GetClipboard[] = {8, 0};
}

PhoneClipboard::PhoneClipboard(QObject *parent) : QObject(parent)
{
    for (auto *process : {&m_command, &m_server}) {
        process->setStandardInputFile(QProcess::nullDevice());
        process->setStandardErrorFile(QProcess::nullDevice());
    }
    m_server.setStandardOutputFile(QProcess::nullDevice());
    connect(&m_command, &QProcess::readyReadStandardOutput, this, &PhoneClipboard::readCommandOutput);
    connect(&m_command, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        commandFinished(code == 0 && status == QProcess::NormalExit);
    });
    connect(&m_command, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) commandFinished(false);
    }, Qt::QueuedConnection);
    const auto serverStopped = [this] {
        m_serverActive = false;
        if (m_stage == Stage::Exchange) {
            cleanup(m_reply == Reply::Dummy || m_direction == Direction::FromPhone
                ? "otg.manual_clipboard.failed" : "otg.manual_clipboard.write_unconfirmed");
        } else {
            finishIfStopped();
        }
    };
    connect(&m_server, &QProcess::started, this, &PhoneClipboard::connectSocket);
    connect(&m_server, &QProcess::finished, this, serverStopped);
    connect(&m_server, &QProcess::errorOccurred, this, [serverStopped](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) serverStopped();
    }, Qt::QueuedConnection);

    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_socket.setReadBufferSize(5 + OTG_CLIPBOARD_TEXT_MAX);
    connect(&m_socket, &QTcpSocket::readyRead, this, &PhoneClipboard::readReply);
    connect(&m_socket, &QTcpSocket::disconnected, this, &PhoneClipboard::socketLost);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &PhoneClipboard::socketLost);
    m_retry.setSingleShot(true);
    m_retry.setInterval(75);
    connect(&m_retry, &QTimer::timeout, this, &PhoneClipboard::connectSocket);

    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_stage != Stage::Exchange) {
            m_command.kill();
        } else if (m_reply == Reply::Dummy) {
            cleanup("otg.manual_clipboard.failed");
        } else {
            // scrcpy sends no reply if Android has no readable text clip.
            cleanup(m_direction == Direction::FromPhone ? "otg.manual_clipboard.read_unavailable"
                                                        : "otg.manual_clipboard.write_unconfirmed");
        }
    });
}

PhoneClipboard::~PhoneClipboard()
{
    m_command.disconnect(this);
    m_server.disconnect(this);
    m_socket.disconnect(this);
}

void PhoneClipboard::start(const QString &adb, const QString &target, const QString &server,
                           const QString &version, Direction direction)
{
    if (busy() || adb.isEmpty() || target.isEmpty()) return;
    static const QRegularExpression validVersion("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z");
    if (server.isEmpty() || !validVersion.match(version).hasMatch()) {
        emit completed(QStringLiteral("otg.manual_clipboard.missing_server"));
        return;
    }
    if (direction == Direction::ToPhone) {
        const auto *mime = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
        const auto text = mime && mime->hasText() ? mime->text() : QString();
        if (text.isEmpty()) {
            emit completed(QStringLiteral("otg.manual_clipboard.no_local_text"));
            return;
        }
        if (text.contains(QChar('\0')) || !text.isValidUtf16()) {
            emit completed(QStringLiteral("otg.manual_clipboard.invalid_text"));
            return;
        }
        auto bytes = text.toUtf8();
        if (bytes.size() > OTG_CLIPBOARD_TEXT_MAX) {
            emit completed(QStringLiteral("otg.manual_clipboard.limit"));
            return;
        }
        m_sendText = std::move(bytes);
    }
    m_adb = adb;
    m_target = target;
    m_serverFile = server;
    m_version = version;
    m_direction = direction;
    m_cancelled = false;
    // Use a separate server file and abstract socket, including when UHID,
    // audio, or another scrcpy session is already running on the same phone.
    const auto uuid = QUuid::createUuid().toString(QUuid::Id128);
    m_remoteJar = QStringLiteral("/data/local/tmp/deskflow-bridge-clipboard-%1.jar").arg(uuid);
    m_scid = QStringLiteral("%1").arg(uuid.left(8).toUInt(nullptr, 16) & 0x7fffffffU, 8, 16, QChar('0'));
    m_remoteSocket = QStringLiteral("localabstract:scrcpy_") + m_scid;
    m_result = nullptr;
    // The selected device must already be online. Keep the local server path
    // in QProcess arguments instead of interpolating it into Android's shell.
    command(Stage::CheckDevice, {QStringLiteral("get-state")});
}

void PhoneClipboard::command(Stage stage, const QStringList &args)
{
    m_stage = stage;
    m_commandActive = true;
    m_commandOutput.clear();
    m_deadline.start(CommandTimeoutMs);
    m_command.start(m_adb, QStringList{QStringLiteral("-s"), m_target} + args, QIODevice::ReadOnly);
}

void PhoneClipboard::readCommandOutput()
{
    const auto bytes = m_command.readAllStandardOutput();
    if (m_stage == Stage::CheckDevice || m_stage == Stage::Forward || m_stage == Stage::FindForward) {
        if (bytes.size() > MaxCommandOutput - m_commandOutput.size()) {
            m_commandOutput.clear();
            m_command.kill();
        } else {
            m_commandOutput += bytes;
        }
    }
}

void PhoneClipboard::commandFinished(bool ok)
{
    m_commandActive = false;
    m_deadline.stop();
    readCommandOutput();
    switch (m_stage) {
    case Stage::CheckDevice:
        if (m_cancelled || !ok || m_commandOutput.trimmed() != "device") {
            m_result = "otg.manual_clipboard.adb_unavailable";
            m_stage = Stage::Finishing;
            finishIfStopped();
        } else {
            command(Stage::Push, {QStringLiteral("push"), m_serverFile, m_remoteJar});
        }
        break;
    case Stage::Push:
        if (m_cancelled || !ok) cleanup("otg.manual_clipboard.failed");
        else command(Stage::Forward, {QStringLiteral("forward"), QStringLiteral("tcp:0"), m_remoteSocket});
        break;
    case Stage::Forward:
        m_port = ok ? m_commandOutput.trimmed().toUShort() : 0;
        if (m_cancelled || !ok || !m_port) cleanup("otg.manual_clipboard.failed");
        else startServer();
        break;
    case Stage::FindForward: {
        // Recover an allocation if ADB accepted the forward before its client
        // timed out. Match only this operation's generated abstract socket.
        if (ok) {
            const auto target = m_target.toUtf8();
            const auto remote = m_remoteSocket.toLatin1();
            for (const auto &line : m_commandOutput.split('\n')) {
                const auto fields = line.simplified().split(' ');
                if (fields.size() != 3 || fields[0] != target || fields[2] != remote || !fields[1].startsWith("tcp:"))
                    continue;
                m_port = fields[1].mid(4).toUShort();
                break;
            }
        }
        if (m_port) removeForward();
        else removeJar();
        break;
    }
    case Stage::RemoveForward:
        removeJar();
        break;
    case Stage::RemoveJar:
        m_stage = Stage::Finishing;
        finishIfStopped();
        break;
    default:
        break;
    }
}

void PhoneClipboard::startServer()
{
    m_stage = Stage::Exchange;
    m_reply = Reply::Dummy;
    m_deadline.start(CommandTimeoutMs);
    m_serverActive = true;
    // cleanup=false avoids scrcpy's device settings cleanup. We remove our
    // uniquely named file even when the server could not start.
    m_server.start(m_adb, {"-s", m_target, "shell", "-T", "-n", "CLASSPATH=" + m_remoteJar,
        "app_process", "/", "com.genymobile.scrcpy.Server", m_version,
        "video=false", "audio=false", "control=true", "clipboard_autosync=false",
        "power_on=false", "keep_active=false", "cleanup=false", "tunnel_forward=true",
        "send_device_meta=false", "send_dummy_byte=true", "scid=" + m_scid}, QIODevice::ReadOnly);
}

void PhoneClipboard::connectSocket()
{
    if (m_stage == Stage::Exchange && m_reply == Reply::Dummy && m_socket.state() == QAbstractSocket::UnconnectedState)
        m_socket.connectToHost(QHostAddress::LocalHost, m_port);
}

void PhoneClipboard::socketLost()
{
    if (m_stage != Stage::Exchange) return;
    if (m_reply == Reply::Dummy) {
        // The forward may be ready before the Android abstract socket binds.
        m_retry.start();
    } else {
        cleanup(m_direction == Direction::FromPhone ? "otg.manual_clipboard.failed"
                                                    : "otg.manual_clipboard.write_unconfirmed");
    }
}

void PhoneClipboard::readReply()
{
    if (m_stage != Stage::Exchange) return;
    if (m_reply == Reply::Dummy) {
        if (!m_socket.bytesAvailable()) return;
        if (m_socket.read(1) != QByteArray(1, '\0')) {
            cleanup("otg.manual_clipboard.failed");
            return;
        }
        m_retry.stop();
        m_deadline.start(ReplyTimeoutMs);
        if (m_direction == Direction::FromPhone) {
            m_reply = Reply::Text;
            m_socket.write(GetClipboard, sizeof(GetClipboard));
        } else {
            m_reply = Reply::Ack;
            // SET_CLIPBOARD: type, BE64 sequence, paste=false, BE32 length, UTF-8 text.
            QByteArray packet(14, '\0');
            packet[0] = 9;
            qToBigEndian(WriteSequence, packet.data() + 1);
            qToBigEndian(quint32(m_sendText.size()), packet.data() + 10);
            packet += m_sendText;
            m_socket.write(packet);
        }
        return;
    }
    if (m_reply == Reply::Ack) {
        if (m_socket.bytesAvailable() < 9) return;
        const auto ack = m_socket.read(9);
        if (ack[0] != 1 || qFromBigEndian<quint64>(ack.constData() + 1) != WriteSequence) {
            cleanup("otg.manual_clipboard.write_unconfirmed");
            return;
        }
        // scrcpy acknowledges processing even if Android declined the write.
        // Read back the text once to check what is actually on the phone.
        m_reply = Reply::Verify;
        m_deadline.start(ReplyTimeoutMs);
        m_socket.write(GetClipboard, sizeof(GetClipboard));
        return;
    }
    if (m_socket.bytesAvailable() < 5) return;
    const auto header = m_socket.peek(5);
    const auto invalid = m_reply == Reply::Verify ? "otg.manual_clipboard.write_unconfirmed"
                                                : "otg.manual_clipboard.invalid_text";
    if (header[0] != 0) { cleanup(invalid); return; }
    const auto length = qFromBigEndian<quint32>(header.constData() + 1);
    if (length > OTG_CLIPBOARD_TEXT_MAX) {
        cleanup(m_reply == Reply::Verify ? invalid : "otg.manual_clipboard.limit");
        return;
    }
    if (m_socket.bytesAvailable() < 5 + length) return;
    m_socket.read(5);
    const auto text = m_socket.read(length);
    if (m_reply == Reply::Verify) {
        cleanup(text == m_sendText ? "otg.manual_clipboard.sent" : invalid);
        return;
    }
    QStringDecoder decode(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    const QString decoded = decode(text);
    if (decode.hasError() || text.contains('\0')) cleanup(invalid);
    else if (decoded.isEmpty()) cleanup("otg.manual_clipboard.read_unavailable");
    else {
        m_receivedText = decoded;
        cleanup("otg.manual_clipboard.received");
    }
}

void PhoneClipboard::cancel()
{
    if (!busy()) return;
    m_cancelled = true;
    if (m_stage == Stage::Exchange) cleanup("");
    // Let bounded push/forward commands finish, so cleanup can remove what
    // they created even if the window is closed while ADB is still working.
}

void PhoneClipboard::cleanup(const char *messageId)
{
    const bool findForward = m_stage == Stage::Forward && !m_port;
    m_result = messageId;
    m_stage = Stage::Finishing;
    m_deadline.stop();
    m_retry.stop();
    m_socket.abort();
    if (m_port) removeForward();
    else if (findForward) command(Stage::FindForward, {QStringLiteral("forward"), QStringLiteral("--list")});
    else removeJar();
    if (m_server.state() != QProcess::NotRunning) m_server.kill();
}

void PhoneClipboard::removeForward()
{
    command(Stage::RemoveForward, {QStringLiteral("forward"), QStringLiteral("--remove"),
                                  QStringLiteral("tcp:%1").arg(m_port)});
    m_port = 0;
}

void PhoneClipboard::removeJar()
{
    command(Stage::RemoveJar, {QStringLiteral("shell"), QStringLiteral("-T"), QStringLiteral("-n"),
                               QStringLiteral("rm"), QStringLiteral("-f"), m_remoteJar});
}

void PhoneClipboard::finishIfStopped()
{
    if (m_stage != Stage::Finishing || m_commandActive || m_serverActive)
        return;
    const auto id = QString::fromLatin1(m_cancelled ? "" : m_result);
    if (!m_cancelled && !m_receivedText.isEmpty())
        QGuiApplication::clipboard()->setText(m_receivedText, QClipboard::Clipboard);
    m_receivedText.clear();
    m_sendText.clear();
    m_stage = Stage::Idle;
    emit completed(id);
}
