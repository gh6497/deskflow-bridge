// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QUuid>

struct WindowsBleDeviceIdentity {
    QUuid container;
    QString publicAddress;
};

WindowsBleDeviceIdentity windowsBleDeviceIdentity(const QString &address);
