// SPDX-License-Identifier: Apache-2.0
#include "device_info.h"

#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

bool isUsbAdbSerial(const QString &serial)
{
    // Native Windows ADB can omit usb:<path> even for a wired device. TCP
    // endpoints, mDNS services, emulators and unreadable serials are not USB IDs.
    static const QRegularExpression whitespace("\\s");
    return !serial.isEmpty() && !serial.contains('?') && !serial.contains(':') &&
           !serial.startsWith("emulator-") && !serial.contains("adb-tls-", Qt::CaseInsensitive) &&
           !serial.contains("._tcp", Qt::CaseInsensitive) && !serial.contains(whitespace);
}

QList<AndroidDevice> parseDevices(const QString &text)
{
    QList<AndroidDevice> result;
    for (const QString &line : text.split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2 || line.startsWith("List ") || line.startsWith('*'))
            continue;
        const QString serial = fields[0];
        // AOA requires a physical USB transport, never an emulator or TCP ADB.
        if (serial.startsWith("emulator-") || serial.contains(':') ||
            serial.contains("._adb-tls-"))
            continue;
        if (fields[1] != "device" && fields[1] != "offline" &&
            fields[1] != "unauthorized" && fields[1] != "no")
            continue;
        AndroidDevice device{serial, {}, fields[1]};
        for (const QString &field : fields) {
            if (field.startsWith("model:"))
                device.model = field.mid(6).replace('_', ' ');
        }
        result.append(device);
    }
    return result;
}

QList<AndroidDevice> parseUsbScanDevices(const QString &text)
{
    QList<AndroidDevice> result;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return result;
    const auto devices = document.object().value("devices").toArray();
    for (const auto &value : devices) {
        const auto object = value.toObject();
        const auto serial = object.value("serial").toString().trimmed();
        // A device whose descriptor cannot be read is not a usable target.
        if (serial.isEmpty())
            continue;
        result.append(AndroidDevice{serial, {}, "device"});
    }
    return result;
}

QSize parseScreenSize(const QString &text)
{
    const QRegularExpression re("(?:Physical|Override) size:\\s*(\\d+)x(\\d+)");
    auto matches = re.globalMatch(text);
    QSize physical, overrideSize;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int w = match.captured(1).toInt(), h = match.captured(2).toInt();
        if (w < 1 || h < 1 || w > 32767 || h > 32767)
            continue;
        if (match.captured().startsWith("Override"))
            overrideSize = QSize(w, h);
        else
            physical = QSize(w, h);
    }
    return overrideSize.isValid() ? overrideSize : physical;
}

int parseRotation(const QString &text)
{
    const QRegularExpression re("SurfaceOrientation:\\s*([0-3])\\b");
    auto match = re.match(text);
    if (match.hasMatch())
        return match.captured(1).toInt();
    // Newer InputReader dumps report display viewports instead of the legacy
    // SurfaceOrientation field. Use the primary display, not an external one.
    const QRegularExpression viewport("Viewport[^\\n]*?displayId[ =]+0\\b[^\\n]*?orientation[ =]+([0-3])\\b");
    match = viewport.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

bool validScreenName(const QString &name)
{
    return QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}$").match(name).hasMatch();
}
