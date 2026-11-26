# 同一蓝牙适配器连接多台手机的开发交接

2026-11-24，`deskflow-otg` 分支 `feat/20261122`，交接基线 `1fc7e9c`。

首期目标是在 Linux 上通过同一个 `hci0` 同时连接 K20 Pro 和 OPPO R17。两台手机各占一个 Deskflow 屏幕，鼠标切到哪台，键鼠报告就只发给哪台。一台手机断线或退出时，另一台保持连接。[当前使用指南](USER_GUIDE.zh-CN.md#multiple)

当前软件容量已扩展到 8 个手机会话。Linux GUI 默认在有空位时继续发现手机。发现会话仅认领当前已配对且两个通知通道均已建立的 LE 身份。离线回归覆盖三台独立通信与容量上限。以下双机段落保留早期真机记录。

## 本轮实现与离线验收

- Linux 常驻服务默认通过共享定向路径开启多机。核验脚本将已知含上游修复的参考二进制与 `/proc/PID/exe` 比对，把本次系统总线 ID 和 `org.bluez` 唯一所有者记在 `/run`。服务校验该记录，所有者变化即结束。旧版单机模式不再作为 GUI 连接方式。
- `ble_directed.inc` 按在线 Device1 地址、对象路径和 HCI peripheral 链路核对路由。每个目标各有键盘和指针 FD、输入值、LED、暂停状态与连接代次。重连后先对该手机发送两份中性报告，再接收新输入。BlueZ 对已配对手机保留 CCC 而不重建 FD 时，该手机会停在等待订阅状态。
- `ble_multi_session.inc` 通过带仅 BLE 门控的 v2 接入操作一次性预留地址和屏幕名。GUI 核对 `directed` 与 `bleOnly`，所有会话共用一次共享服务授权。GUI 会按需启动服务并为每台手机运行 `deskflow-bridge --transport ble --ble-session --ble-directed-session --adapter hci0 --ble-legacy-handles --ble-peer MAC --name SCREEN`。低句柄与服务保持一致。`--absolute-per-axis` 只需加在需要的桥接上。监督器在验证适配器已切换到仅 BLE 后降权启动服务，最后一个客户端退出后恢复。GUI 在首次接入前取消时，监督器处理启动管道 EOF 并恢复适配器。
- 私有总线回归使用真实传递的四个 FD，覆盖隔离、掉线、重新订阅、先释放再恢复。真实 Unix socket/fork 回归验证双客户端共享服务、地址和屏幕名冲突、EOF 后另一路继续和最后退出。已重新运行交接基线的 BlueZ 双订阅分发函数实验。K20 Pro 和 R17 的空口、系统模式恢复与手机 CCC 缓存行为仍待真机验收。

- 控制器接入第一台手机时可能停用用于接入的可连接广播，BlueZ 的 `ActiveInstances` 仍可能计入该登记。双机服务观察到 LE peripheral 链路变化而且还有空位时，异步重新登记自身的广播。日志 `BLE advertisement re-registered for another connection` 确认 BlueZ 已接受登记。私有 D-Bus 回归验证登记、失败重试、键鼠报告继续投递和会话退出清理。
- 2026-11-25 在 AX210、含定向通知回移修复的 BlueZ 5.72、低句柄及仅 BLE 模式下切换到新版服务。R17 保持已连接时，K20 Pro 扫到 `Deskflow Bridge` 的可连接广播（现场读数 −55 dBm）。K20 Pro 通过手机系统蓝牙设置接入键鼠后，两个加密 LE 链路同时在线，两组键鼠订阅均就绪，两个 Deskflow TCP 会话均已建立。更换常驻服务后发现的手机订阅缓存问题仍可能需要由手机重新接入。

## 已确认的 Linux 环境和行为

- 本机是 Linux Mint 22.3，基于 Ubuntu 24.04 `noble`，适配器是 Intel AX210。本机安装的 BlueZ 包为 `5.72-0ubuntu5.8`，当时官方 APT 源也只提供 5.72。上游 5.76 已在本机临时目录编译通过，验证了二进制版本和动态库依赖。系统 BLE 真机试验均使用发行版的 5.72。[Ubuntu 包列表](https://packages.ubuntu.com/search?keywords=bluez&searchon=names&exact=1&suite=all&section=all)
- BlueZ 5.72 为 `AcquireNotify` 的每个订阅者建独立 FD，但从任一 FD 收到的报告仍会发给所有订阅者。上游提交 `a08ec1a4e93b7805cdda33600d83d946969acb47` 把发送绑定到对应的 ATT 连接，从 5.76 起包含这项修复。普通 `StartNotify` 加特征 `Value` 更新仍是面向该特征所有订阅者的路径。多机输入需要使用已验证可定向发送的通道。[BlueZ 5.72 源码](https://github.com/bluez/bluez/blob/5.72/src/gatt-database.c#L2589) · [上游修复](https://github.com/bluez/bluez/commit/a08ec1a4e93b7805cdda33600d83d946969acb47)
- 上游修复已回移到 `5.72-0ubuntu5.8` 源码并通过本地编译。使用原版和回移版的实际通知分发函数做了双订阅模拟。原版发给 A 或 B 都会送到两者，回移版 `A→A`、`B→B`，找不到目标时丢弃报告。该验证在临时目录完成，双手机空口隔离和重连待验收。复现实验脚本如仍在本机，可运行 `python3 /tmp/opencode/bluez-noble-backport.kh6Oed/check-routing.py`。
- `AcquireNotify` 能在请求中提供对端对象和链路类型，但 FD 绑定当前连接。BlueZ 5.72 可保留已配对设备的 CCC 记录，同时在 ATT 断开时关闭 FD。重连时手机可能复用 CCC，BlueZ 也可能直接接受同值写入，导致新连接拿不到新的 `AcquireNotify`。上游定向修复解决发送对象，重连后如何重新取得定向通道需要独立验证。[BlueZ 5.72 GATT 实现](https://github.com/bluez/bluez/blob/5.72/src/gatt-database.c)

## 交接基线的代码入口

- `gui/connection_manager.cpp` 负责共享 Deskflow 和每个设备的 `Session`。`ConnectionManager::start()` 拒绝第二个 BLE 设备。`gui/session.cpp` 启动或复用按适配器管理的 Linux 常驻服务，校验空闲状态和 HID 句柄布局。
- `platform/linux/ble_session.c` 的 Unix seqpacket IPC 仍为 v2。一个适配器一个服务和一个桥接客户端，服务保存一个目标，停止或实例退出时清理输入。它也负责双向 UID 检查、有界报告队列、取消和空闲状态查询。
- `platform/linux/ble_hid.c` 用 HCI 快照核对唯一 LE 连接与本机 peripheral 角色，再核对在线 `Device1` 的 `Address` 和 `Paired`。`AcquireNotify` 当前只记录订阅来源并返回 `NotSupported`，由 BlueZ 回退到 `StartNotify`。`changed()` 给 BlueZ 发 `Value` 更新，定向的 D-Bus 收件人是 BlueZ 本身。键盘和指针只有两路全局订阅状态。
- `platform/linux/ble_supervisor.c` 的共享服务模式会切换和恢复整个适配器，并用管理锁、恢复快照和服务空闲检查协调退出。所有 BLE 会话共用适配器级生命周期。
- `platform/windows/ble_win.cpp` 已使用 `NotifyValueAsync(value, GattSubscribedClient)` 定向发送，但 provider 只保存一个目标，并要求订阅列表唯一。Windows 后续可按订阅客户端扩展。macOS 目前没有项目 BLE 后端。[Microsoft API 文档](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattlocalcharacteristic.notifyvalueasync)

## 两台真机的已知条件

- **Redmi K20 Pro** 已在 AX210 和 BlueZ 5.72 上完成单机 BLE 键鼠、切屏和重连测试。保留这一组合用于单机回归和双机对照。[既有 Linux BLE 交接](LINUX_BLE_HANDOVER.md)
- **OPPO R17（Android 10）** 在双模下曾只建经典蓝牙 ACL。切换仅 BLE 后可建立加密 LE，但原布局没有成功订阅键鼠。旧版 Android 会截断用于 CCCD 查找的 16 位句柄，R17 抓包中出现对 `0x000B` 的写入。`d997011` 的 `--ble-legacy-handles` 使用 `0x0040..0x0051`，配合仅 BLE 模式已通过双订阅和 Deskflow 键鼠真机测试。[低句柄兼容记录](LINUX_BLE_HANDOVER.md#旧版-android-hid-兼容选项)
- **共享布局**由同一 GATT 服务注册。首轮可用低句柄布局尝试两台手机，以满足 R17，需验证 K20 Pro 也能在这个布局下订阅。R17 另需 `--absolute-per-axis` 才能让指针到达全屏边缘。`42aeec1` 已通过 USB AOA 和 BLE 真机对照，坐标选项可按桥接会话设置，HID 描述符保持一致。真实设备重连时若缺订阅，关开 R17 的蓝牙曾有效。

## 实施顺序和验收标准

1. **先跑通 Linux 定向通知。** 在已确认来源和版本的 BlueZ 5.76+ 或回移 5.72 环境里，让 GATT 应用实际返回每条订阅的通知 FD。验证第一台连上后第二台仍能从手机发起连接，并核对两路通知都按各自的连接送达。原版 5.72 保留现有单机功能，双机模式启用前需要确认系统具备定向发送能力。对回移包，版本字符串本身不足以区分是否包含修复。
2. **为每台手机维护独立的会话状态。** 以 BlueZ 已核实的稳定身份地址绑定桥接客户端。区分稳定地址和 D-Bus 对象路径，隐私地址解析后路径可能保留旧名。分别跟踪在线对象、LE 角色、配对、键鼠订阅、队列、按下状态和连接代次。只有当前目标的两路订阅都有效时才接收该桥接的键鼠。断链、超时和身份不明时清空对应队列。双机发送路径应全程使用定向 FD，`ReadValue`、键盘 LED 和 Control Point 也按已核实的手机身份隔离。
3. **扩展常驻 IPC 和 GUI 接入。** 继续复用一个适配器常驻服务与一个共享 Deskflow，让两台设备分别建 Deskflow 客户端。首轮显式配置各手机地址，拒绝重复地址和屏幕名。为每个会话分别提供就绪、断开和错误状态。定义协议兼容方式并验证旧版单机 v2 接入。底层定向隔离验证后再开放第二台 BLE 的 GUI 连接。Windows 和 macOS 保持可构建的平台边界。
4. **优先验证重连。** 模拟 R17 或 K20 Pro 断开时关闭其两个 FD，另一台继续工作。原手机重新建立有身份的 LE 链路后，验证定向 FD 是否会重新建立、何时恢复就绪及是否先释放旧状态。已配对设备的 CCC 缓存需要真机覆盖，连不上时先区分经典蓝牙、LE、配对和订阅阶段。
5. **覆盖输入隔离和退出。** 两台在线反复切屏，分别测试数字键、移动、点击、拖拽和滚轮。观测两台手机，确认相邻纯移动可以合并，按下、释放、滚轮、键盘和指针进入/离开保持顺序。断开单个 GUI 会话、手机掉线重连和桥接进程退出都要验证。一台断开时另一台保持就绪，退出最后一个会话后验证适配器恢复。手持键切屏和断开时要验证松键行为。

离线可以扩展 `tests/test_ble_gatt.c` 的私有 D-Bus 模拟、`tests/test_ble_session.c` 的真实本地 socket 模拟和 `tests/test_ble_pointer.c` 的队列测试。先覆盖跨手机路由、设备地址复用、错序回调和过期 FD，再做两台手机的空口验收。涉及 GUI 操作的改动同步更新两份 `docs/USER_GUIDE`，并按 `AGENTS.md` 更新相关截图。[仓库约定](../AGENTS.md)

## 启动新会话时

开始前可用以下命令读取当前环境。`--ble-status` 在服务未启动时会返回 `available=false`。

```bash
git status --short --branch
bluetoothctl --version
apt-cache policy bluez
bluetoothctl devices Connected
hcitool -i hci0 con
./build/deskflow-bridge --ble-status --adapter hci0
```

先确认是否有正在使用的连接。更换系统 BlueZ、启动会断开整块适配器的仅 BLE 模式、清除配对或停止正在使用的服务之前，按当前会话的授权核对具体影响。可以先用私有 D-Bus 和本地 IPC 完成离线实现与隔离验证。
