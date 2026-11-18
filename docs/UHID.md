# ADB＋UHID 键鼠连接

`--transport uhid` 通过 ADB/scrcpy 控制连接，在 Android 内核创建 HID 键盘和指针。
Linux、Windows、macOS 共用 `src/uhid.c`；不调用 libusb，不使用 Android SDK 按键注入。
Windows 使用正常 Google/厂商 ADB 驱动，无需为 AOA 替换 USB 接口驱动。

## 使用要求

- USB 连接须开启 USB 调试并授权电脑；也可使用已配对的无线 ADB 连接。
- 目标必须显式指定为 `--adb-serial`，不接受 `--serial` 或 BLE 选项，不选择任意在线手机。
- Android shell 必须能以读写方式打开 `/dev/uhid`。启动会先检查字符设备并实际打开，
  权限或内核不支持时终止；scrcpy-server 的诊断进入桥接 stderr/GUI 日志。
- 无需 root、无障碍、投屏或安装伴侣 APK。伴侣文件分享等功能仍按原规则主动启用。
- 需要匹配版本的 scrcpy-server。当前随包提供 5.0.1；核心不依赖 SDL/FFmpeg/Qt。

## GUI

选择「ADB＋UHID」，开启并授权 USB 调试，然后「刷新设备」。此模式在三平台均调用
`adb devices -l`，选择设备后把序列号填入 **ADB 目标**，通过同一目标读取尺寸与方向。
未授权/离线设备提示相应状态；多设备需显式选择，也可手动填写 USB ADB 序列号或无线地址。
读取尺寸失败时可手动填写，连接仍会再次校验所选目标的在线状态。

此模式下 ADB 必须开启，USB AOA 序列号和 BLE 设置隐藏；保留指针模式、
Home/Back、侧键手势、移动倍率、可选音频/剪贴板/跟随旋转及伴侣操作。UHID-only 也可跟随旋转。
配置按设备保存；同一 ADB 目标不能同时建立两个 GUI UHID 会话。
Windows 新配置默认选择 UHID，旧配置不会自动迁移到另一种传输。Linux/macOS 默认仍为 AOA。

**有线插入自动连接（GUI）**：选择 USB ADB 序列号，勾选「USB 插入时自动连接此设备」（默认关闭），
手动连接至「已连接」。程序运行期间（包括托盘）每 2 秒通过 `adb devices -l` 检查已授权的在线设备，
启动时已插入也能接入。只使用该 UHID 配置成功连接过的 ADB 序列号；同名 AOA 配置、无线地址、mDNS 服务、
模拟器、离线/未授权设备及重复序列号均不会触发。连接失败或手动断开后不在原连接上循环重试。
新 ADB 连接按 `transport_id` 区分；旧版 ADB 没有此字段时，需让程序观察到设备消失后再插入。
仅检测列表，不调用 libusb、不配对或主动连接无线 ADB。

## CLI 与构建

默认 `BUILD_UHID=ON`，`OTG_PROFILE=full` 也启用它。可用 `-DOTG_PROFILE=manual -DBUILD_UHID=OFF` 关闭；
单独启用 UHID 不需要音频、剪贴板、ALSA 或 BLE。安装和 GUI 构建目录均包含匹配 server/版本文件。
CLI 默认仍为 `--transport usb`，使用 UHID 时明确选择：

```bash
./build/deskflow-bridge --transport uhid --adb-serial PHONE_ADB_SERIAL \
  --scrcpy-server ./third_party/scrcpy/scrcpy-server-v5.0.1 --scrcpy-version 5.0.1 \
  --host 127.0.0.1 --port 24800 --name android --width 1080 --height 1920
```

Windows 使用 `deskflow-bridge.exe` 和对应文件路径；GUI 自动定位包内 ADB/server。
可追加 `--clipboard`、`--audio`（Linux/macOS/Windows）、`--auto-rotate`。
无线目标 `host:port` / `[IPv6]:port` 沿用现有策略：检查在线状态，必要时仅对该目标执行一次
`adb connect` 后重新校验；不自动配对、切换目标、断开其他 ADB 连接或重启 ADB server。

## 协议与生命周期

- `src/bridge.c` 继续生成原有键盘和绝对数位板描述符/报告；UHID 仅封装 scrcpy
  `UHID_CREATE=12`、`UHID_INPUT=13`、`UHID_DESTROY=14`。键盘 ID=1，指针 ID=2，唤醒设备 ID=3。
- scrcpy 使用 `BUS_VIRTUAL`，Android 通常把 UHID 键盘视为内部设备；普通移动或 Enter
  不一定能触发外接设备的唤醒策略。每次 Deskflow `CINN` 先给独立 System Control 设备发送
  System Wake Up（Generic Desktop usage `0x83`）按下/释放，映射为 Linux `KEY_WAKEUP=143`
  和 Android `KEYCODE_WAKEUP`。只唤醒、不切换电源、不向应用输入字符，不依赖 PIN 开关、
  SDK `INJECT_EVENTS` 或投屏；注册/重连本身不发唤醒按下。GUI PIN 解锁随后按已有锁屏
  检测和延时上滑/PIN 流程执行，未知/未锁定状态不发送 PIN；切出取消待执行解锁。
- 默认保持绝对定位、悬停、拖拽、滚轮和导航快捷键。相对模式仍可选择，也仍受 Android 加速影响。
- 每个会话只有一个 scrcpy-server/ADB forward。音频使用独立 socket，UHID 与剪贴板共用
  同一控制 socket 和读写器；不开剪贴板时关闭手机剪贴板自动同步。
- 非阻塞发送，64 条有界 HID 队列；完整消息保序，部分发送不交错，不丢弃按键或按钮屏障。
  socket 开启 TCP_NODELAY。队列溢出或写入超时结束连接，不无限积压输入。
- 接收端处理剪贴板、ACK 及 UHID_OUTPUT（键盘 LED）分片；LED 不改变 Deskflow 的按键状态。
  剪贴板 ACK 超时只关闭共享策略并释放被屏障等待的按键，UHID 仍可用；控制 socket 断开则
  必须停止 Deskflow，让鼠标返回电脑。可手动重连；勾选上述开关的有线设备也可在重新插线后自动接入。
- 断开时先结束指针范围、释放键盘及唤醒键，再销毁三台 UHID 设备；待发消息最多排空 500ms，然后关闭
  socket 并清理自己启动的进程/forward。server 收到 EOF 也会销毁该连接的设备。
  ADB 掉线时无法承诺释放报告送达，手机端设备销毁负责最终清理。
- ADB 启动与部分 Deskflow 握手均响应取消；Windows 仍沿用 GUI stdin/EOF 生命周期，不常驻 GATT。
- 官方 scrcpy 协议没有 UHID_CREATE 成功 ACK。`uhid-ready` 表示描述符已提交到控制通道，
  GUI「已连接」还需完整 Deskflow DSOP 握手；不等于手机 ROM 已验收输入效果。

## 回归验证

`uhid_protocol` 使用真实本地 socket 与 bridge，检查描述符、仅唤醒按键、重复唤醒与真实按键隔离、绝对坐标、键盘/鼠标释放、
分片发送与 LED/剪贴板混合解析、ACK 超时、EOF、队列背压和写入超时。
`uhid_cli` 在 POSIX 使用真实 CLI＋fake ADB/scrcpy/Deskflow，覆盖纯 UHID、共享附加功能、
断线、拒绝连接、部分握手取消、设备权限失败与启动取消；还模拟息屏设备验证进入先唤醒、
重入再次唤醒、锁定后发送测试 PIN、未锁定/未知状态不输入 PIN、切出取消解锁，不访问硬件。
Qt 测试覆盖参数传递、目标隔离、设备发现、配置保存、国际化和下拉框双向滚轮/键盘操作。

以上离线测试不代表 Windows/macOS 真机或所有 Android ROM 已验收；实际使用时需验证悬停、
四角、拖拽、滚轮、导航、切屏和拔插恢复。

2026-11-16 用户在当前 Linux＋手机环境确认：更新后 UHID 息屏切入可以点亮屏幕，
并完成自动解锁。此次确认仅覆盖该环境的唤醒/解锁流程，不外推其他 ROM 或 Windows/macOS。
