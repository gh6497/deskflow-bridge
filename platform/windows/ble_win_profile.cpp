// SPDX-License-Identifier: Apache-2.0
// Windows Bluetooth device containers: https://learn.microsoft.com/windows-hardware/drivers/install/container-ids-for-bluetooth-devices
#include "ble_win_profile.h"

#include <QList>
#include <QRegularExpression>
#include <cwchar>
#include <vector>
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

namespace {
struct BluetoothNode {
    QString address;
    QUuid container;
};

QList<BluetoothNode> bluetoothNodes(const wchar_t *enumerator, const QString &address = {})
{
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, enumerator, flags) != CR_SUCCESS || !length) return {};

    std::vector<wchar_t> list(length);
    if (CM_Get_Device_ID_ListW(enumerator, list.data(), length, flags) != CR_SUCCESS) return {};

    static const QRegularExpression deviceId("^DEV_([0-9A-Fa-f]{12})$", QRegularExpression::CaseInsensitiveOption);
    const QUuid localContainer(0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
    QList<BluetoothNode> nodes;
    for (auto id = list.data(); *id; id += std::wcslen(id) + 1) {
        const auto match = deviceId.match(QString::fromWCharArray(id).section('\\', 1, 1));
        if (!match.hasMatch()) continue;
        const auto digits = match.captured(1).toUpper();
        if ((!address.isEmpty() && digits != address) || digits == "000000000000") continue;
        DEVINST device;
        if (CM_Locate_DevNodeW(&device, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;

        GUID value{};
        DEVPROPTYPE type;
        ULONG size = sizeof(value);
        if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_ContainerId, &type,
                reinterpret_cast<PBYTE>(&value), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_GUID || size != sizeof(value)) continue;

        const QUuid container(value);
        if (!container.isNull() && container != localContainer) nodes.append({digits, container});
    }
    return nodes;
}
}

WindowsBleDeviceIdentity windowsBleDeviceIdentity(const QString &address)
{
    static const QRegularExpression mac("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$");
    if (!mac.match(address).hasMatch()) return {};
    auto digits = address.toUpper();
    digits.remove(':');

    QUuid container;
    for (const auto &node : bluetoothNodes(L"BTHLE", digits)) {
        if (!container.isNull() && container != node.container) return {};
        container = node.container;
    }
    if (container.isNull()) return {};

    // Windows groups the LE and BR/EDR endpoints. The BR/EDR address survives a new pairing.
    QString publicAddress;
    for (const auto &node : bluetoothNodes(L"BTHENUM")) {
        if (node.container != container) continue;
        if (!publicAddress.isEmpty() && publicAddress != node.address) return {container, {}};
        publicAddress = node.address;
    }
    for (qsizetype index = publicAddress.size() - 2; index > 0; index -= 2)
        publicAddress.insert(index, ':');
    return {container, publicAddress};
}
