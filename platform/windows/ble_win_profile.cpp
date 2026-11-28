// SPDX-License-Identifier: Apache-2.0
// Windows Bluetooth device containers: https://learn.microsoft.com/windows-hardware/drivers/install/container-ids-for-bluetooth-devices
#include "ble_win_profile.h"

#include <QRegularExpression>
#include <cwchar>
#include <vector>
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

QUuid windowsBleDeviceContainer(const QString &address)
{
    static const QRegularExpression mac("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$");
    if (!mac.match(address).hasMatch()) return {};
    auto digits = address;
    digits.remove(':');
    const auto prefix = QStringLiteral("BTHLE\\DEV_") + digits + '\\';
    constexpr wchar_t enumerator[] = L"BTHLE";
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, enumerator, flags) != CR_SUCCESS || !length) return {};

    std::vector<wchar_t> list(length);
    if (CM_Get_Device_ID_ListW(enumerator, list.data(), length, flags) != CR_SUCCESS) return {};

    QUuid container;
    for (auto id = list.data(); *id; id += std::wcslen(id) + 1) {
        if (!QString::fromWCharArray(id).startsWith(prefix, Qt::CaseInsensitive)) continue;
        DEVINST device;
        if (CM_Locate_DevNodeW(&device, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;

        GUID value{};
        DEVPROPTYPE type;
        ULONG size = sizeof(value);
        if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_ContainerId, &type,
                reinterpret_cast<PBYTE>(&value), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_GUID || size != sizeof(value)) continue;

        const QUuid candidate(value);
        if (candidate.isNull() || candidate == QUuid(0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff))
            continue; // Windows uses a reserved container for the local computer.
        if (!container.isNull() && container != candidate) return {};
        container = candidate;
    }
    return container;
}
