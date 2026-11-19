# 开发指南

本文面向贡献者和维护者。只想使用 Deskflow Bridge 时，请先阅读[用户指南](USER_GUIDE.zh-CN.md)。

## 项目范围

Deskflow Bridge 是一个精简的 Deskflow 客户端和 Android HID 桥接程序：

```text
电脑键盘鼠标
    -> Deskflow server
    -> deskflow-bridge
    -> AOA HID / ADB＋UHID / Bluetooth LE HID
    -> 安卓手机
```

本项目不负责手机画面投屏。手机仍然是物理显示设备，桥接程序通过 Android 兼容的 HID 传输发送键盘和指针输入。

## 仓库结构

| 路径 | 用途 |
|---|---|
| `src/` | C11 桥接核心、Deskflow 协议、键盘映射、HID 报告和 USB 扫描 |
| `platform/posix/` | 共享的 POSIX ADB/scrcpy 会话和平台代码 |
| `platform/linux/` | Linux 音频和 BLE 后端 |
| `platform/windows/` | Windows 音频、BLE 和控制后端 |
| `gui/` | Qt 桌面 GUI、设备设置和 Deskflow 进程生命周期 |
| `android/` | 可选 Android 伴侣应用 |
| `tests/` | 协议、桥接、GUI、平台和打包回归测试 |
| `scripts/` | 固定版本依赖下载、许可证收集和打包辅助脚本 |
| `docs/` | 用户文档和功能专题文档 |

## 构建配置

默认完整构建会启用当前平台支持的全部功能：

```bash
# Linux：GUI、UHID、音频、剪贴板和 Linux BLE
cmake -S . -B build -DOTG_PROFILE=full -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

macOS 和 Windows 也使用同一 profile，不支持的平台功能由 CMake 自动省略。Windows 使用 Visual Studio/vcpkg 工具链，并需要传入对应的工具链文件；macOS GUI 构建需要与目标架构匹配的 Qt（Intel 或 Apple Silicon）。

只构建精简命令行 USB 版本：

```bash
cmake -S . -B build -DOTG_PROFILE=manual \
  -DBUILD_GUI=OFF -DBUILD_AUDIO=OFF -DBUILD_CLIPBOARD=OFF \
  -DBUILD_UHID=OFF -DBUILD_BLE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

更多依赖和平台说明见以下专题文档：

- [ADB＋UHID](UHID.md)
- [音频](AUDIO.md)
- [剪贴板](CLIPBOARD.md)
- [Windows USB](WINDOWS_USB.md)
- [Windows BLE](WINDOWS_BLE.md)
- [Linux BLE 交接说明](LINUX_BLE_HANDOVER.md)

## 架构说明

### Deskflow 协议

桥接程序实现 Deskflow/Synergy/Barrier TCP 协议的客户端部分。每条消息都有四字节大端序外层包长度；payload 内部的整数和字符串又使用协议自己的编码。修改解析代码时必须区分这两层长度。

握手阶段交换协议 hello、屏幕信息和屏幕选项，完成后才接受输入。普通消息回复 `CNOP`，保活消息原样回显；未知的非致命消息应忽略，只有协议错误消息才应断开连接。

### HID 传输

- **AOA HID：** 通过 libusb 使用 Android Open Accessory 控制请求发送 HID 描述符和报告。
- **ADB＋UHID：** 通过 Android shell 创建虚拟 HID 设备，并与可选音频、剪贴板共用 ADB/scrcpy 会话。
- **BLE HID：** 通过平台蓝牙 HID 接口提供键盘和指针；Linux 有常驻服务/会话生命周期，Windows 有独立的实验性原生后端。

默认指针是绝对坐标 HID 数位板报告，每条报告同时携带当前位置、按钮和范围状态，避免 Android 相对鼠标加速造成累计漂移。相对模式仅用于兼容旧行为，不能保证与 Deskflow 屏幕边界始终精确同步。

### 输入状态

键盘修饰键与六键 HID 按键数组分开维护。指针进入/离开、按钮、滚轮和移动事件需要保持顺序，不能让释放或范围状态屏障被后续移动越过。传输层背压也不能改变状态事件的顺序。

## 测试和验证

源码或构建系统发生变化后运行完整测试：

```bash
ctest --test-dir build --output-on-failure
```

自动化测试覆盖协议分包、socket 取消、HID 报告、GUI 设置和进程生命周期、模拟 ADB、BLE 状态机以及发布包布局，但不能证明所有 Android ROM 或 USB/蓝牙控制器都兼容。

源码语法检查应在项目的警告策略下保持干净：

```bash
for f in src/*.c; do
  gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
    -Isrc -Ibuild/generated "$f"
done
```

记录真机结果时，请写明电脑系统、手机型号、Android 版本、连接方式、Deskflow 版本和是否使用 ADB。日志中不要包含私有设备标识或剪贴板正文。

## 发布流程

1. 修改根目录的 `VERSION` 文件。
2. 将版本修改放在独立的审查提交中，或在发布变更中明确标注。
3. 创建匹配的 tag，例如 `v0.1.10`。
4. 推送 tag，等待 GitHub Actions 完成构建、测试和打包。
5. 发布前检查 Linux、Windows 和两种 macOS 架构的 Release 产物。

发布工作流会校验 tag 与 `VERSION` 一致。产物应包含 GUI 包、适用平台的 CLI 包、依赖许可证说明，并在文件名中标明正确架构。Deskflow 有意不随包发布。

## 安全和 TLS

Deskflow TLS 使用用户配置的服务端指纹和可选客户端证书。不能把客户端指纹误填为受信任的服务端指纹。客户端 PEM 包含私钥，必须以私有权限写入，并且不能进入日志或提交记录。

GUI 可能启动 ADB、Deskflow 和桥接子进程。修改命令构造、临时文件、设备选择或权限边界时，需要补充引号、取消、精确目标选择和清理方面的测试。

## 文档规则

- `README.md` 和 `README.zh-CN.md` 保持简短，面向使用者。
- 安装方式或 GUI 行为变化时，同步维护 `docs/USER_GUIDE.md` 和 `docs/USER_GUIDE.zh-CN.md`。
- 协议、架构、构建、发布和测试细节放在本文或对应功能专题文档中。
- 明确标记实验功能和真机验证边界。
- 文档中不得写入私有设备标识、访问令牌、剪贴板正文或未发布的签名材料。

## 许可证和出处

项目采用 Apache-2.0。部分 HID、ADB 集成参考 scrcpy，协议参考 Deskflow；完整许可和出处见 [THIRD_PARTY.md](../THIRD_PARTY.md)。
