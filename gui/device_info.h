// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QList>
#include <QSize>
#include <QString>

struct AndroidDevice {
    QString serial;
    QString model;
    QString state;
};

QList<AndroidDevice> parseAdbDevices(const QString &text);
bool isUsbAdbSerial(const QString &serial);
QList<AndroidDevice> parseUsbScanDevices(const QString &text);
QSize parseScreenSize(const QString &text);
int parseRotation(const QString &text);
bool validScreenName(const QString &name);
QString screenNameForAdbModel(const QString &model);
