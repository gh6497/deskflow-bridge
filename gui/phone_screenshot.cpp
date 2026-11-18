// SPDX-License-Identifier: Apache-2.0
#include "phone_screenshot.h"
#include <QBuffer>
#include <QClipboard>
#include <QFuture>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <utility>

namespace {
constexpr qsizetype MaxPngBytes = 32 * 1024 * 1024;
constexpr qint64 MaxPixels = 16 * 1024 * 1024;

struct Frame {
    QImage image;
    QByteArray png;
    const char *error = nullptr;
};

Frame decode(QByteArray png)
{
    // Some decoders accept an incomplete PNG. A prematurely closed ADB
    // stream must never replace the user's clipboard with a partial image.
    if (!png.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n")) ||
        !png.endsWith(QByteArrayLiteral("\0\0\0\0IEND\xae\x42\x60\x82")))
        return {{}, {}, "otg.screenshot.invalid"};
    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    const auto size = reader.size();
    if (size.isEmpty()) return {{}, {}, "otg.screenshot.invalid"};
    if (qint64(size.width()) * size.height() > MaxPixels) return {{}, {}, "otg.screenshot.limit"};
    auto image = reader.read();
    if (image.isNull()) return {{}, {}, "otg.screenshot.invalid"};
    return {std::move(image), std::move(png), nullptr};
}
} // namespace

PhoneScreenshot::PhoneScreenshot(QObject *parent) : QObject(parent)
{
    // Unlike the text command runner, never merge stderr with the PNG or
    // convert stdout through QString (including on Windows).
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &PhoneScreenshot::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += m_process.readAllStandardError().left(4096 - m_stderr.size());
    });
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { abort("otg.screenshot.timeout"); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish("otg.screenshot.start_failed");
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        m_deadline.stop();
        if (m_failure || code != 0 || status != QProcess::NormalExit) {
            const char *reason = m_stderr.contains("unauthorized") ? "otg.screenshot.unauthorized" :
                                 "otg.screenshot.adb_failed";
            finish(reason);
            return;
        }
        // Decode off the event loop; the clipboard itself belongs on the GUI
        // thread. Destroying the context cancels the final continuation.
        QtFuture::makeReadyValueFuture(std::exchange(m_png, {}))
            .then(QtFuture::Launch::Async, decode)
            .then(this, [this](Frame frame) {
                if (!m_failure && !frame.error) {
                    auto *mime = new QMimeData;
                    mime->setImageData(frame.image);
                    mime->setData("image/png", frame.png);
                    QGuiApplication::clipboard()->setMimeData(mime, QClipboard::Clipboard);
                }
                finish(frame.error ? frame.error : "otg.screenshot.copied");
            });
    });
}

PhoneScreenshot::~PhoneScreenshot()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

bool PhoneScreenshot::start(const QString &adb, const QString &target, int timeoutMs)
{
    if (m_busy || adb.trimmed().isEmpty() || target.trimmed().isEmpty()) return false;
    m_busy = true;
    m_failure = nullptr;
    m_deadline.start(timeoutMs);
    m_process.start(adb, {"-s", target, "exec-out", "screencap", "-p"}, QIODevice::ReadOnly);
    return true;
}

void PhoneScreenshot::readOutput()
{
    const auto chunk = m_process.readAllStandardOutput();
    if (m_failure) return;
    if (chunk.size() > MaxPngBytes - m_png.size()) abort("otg.screenshot.limit");
    else m_png += chunk;
}

void PhoneScreenshot::abort(const char *reason)
{
    if (!m_busy) return;
    m_failure = reason;
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}

void PhoneScreenshot::cancel()
{
    abort("");
}

void PhoneScreenshot::finish(const char *messageId)
{
    m_deadline.stop();
    m_png.clear();
    m_stderr.clear();
    m_busy = false;
    emit completed(QString::fromLatin1(m_failure ? m_failure : messageId));
}
