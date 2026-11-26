# User guide screenshots / 用户指南截图

These PNGs show the project's real Qt widgets, not design mockups.
They accompany [the English guide](../USER_GUIDE.md) and [中文指南](../USER_GUIDE.zh-CN.md).

- Original capture: 2026-11-17, source `363541b` (base version `0.1.9`).
- Refreshed both languages of `device` and `input` on 2026-11-18 from the wired UHID auto-connect worktree
  based on `83a620b` (base version `0.1.10`), using the same harness and demo profile. The device page shows the new opt-in unchecked.
- Refreshed all main-window views in both languages on 2026-11-18 for **Copy phone screenshot**, from the worktree based on `94a8fd1` (base version `0.1.10`). The new action is enabled for the authorized demo and disabled in the AOA view without ADB. The separate Files window is unchanged.
- Refreshed both `multiple-devices` views on 2026-11-18 from the worktree based on `03df239` (base version `0.1.10`) to show the new default screen name `android2`, using the same capture harness and demo profile.
- Refreshed both languages of all main-window views for **Manual clipboard**, from the worktree based on `f473676` (base version `0.1.10`). The UHID demo shows the button available with automatic sharing off, and the AOA demo shows it disabled with ADB enhancements off.
- Refreshed both languages of all main-window views for version `0.1.12` from the worktree based on `d802a99`. The GUI has two tabs, and the input and multiple-device views show the screen position controls under **Screen and display**.
- Refreshed both `input` views from the worktree based on `d997011` to show the older Android pointer compatibility option. Capture used `OTG_TEST_POINTER_SCREENSHOTS` with `gui/test_window guidePointerScreenshot` on the offscreen platform.
- Refreshed both `server` views from the worktree based on `91f5c7c` to show the Bluetooth service settings. Capture used `OTG_TEST_SHARED_SERVICE_SCREENSHOTS` with `gui/test_window guideSharedServiceScreenshot` on the offscreen platform.
- Linux, Qt 6.8.3, offscreen platform, light appearance, 1× scale.
- Both `server` screenshots were refreshed with `guideSharedServiceScreenshot` to show **Start Bluetooth service** in the shared settings.
- Main window at 1100 × 880, with `input` at 1100 × 955 and `server` at 1100 × 1080. File window at 760 × 460.
- `-en` / `-zh-CN` suffixes select the interface language.
- `device` shows UHID setup. `input` shows screen dimensions, placement and button settings.
  `server` shows shared Deskflow and Bluetooth service setup. `aoa` shows USB without ADB.
  `multiple-devices` shows screen settings for the second device. `files` shows pending phone shares.

Capture used an isolated temporary `QSettings` profile and the production
`MainWindow` / `TransferDialog` widgets. Device discovery and file lists came
from the existing `test_peer` / `test_transfer_peer` fixtures. No real ADB,
Deskflow server, USB device or Bluetooth adapter was used. Paths in the server
page are illustrative. File lists contain demo metadata only; no transfer was
performed. These images do not demonstrate real-device compatibility.

For updates, build the GUI from the matching source, use a separate demo profile
and capture both languages with the same settings. Preserve real widget labels
and states rather than editing text into images. The screenshot hooks in
`tests/test_window.cpp` and `tests/test_companion_transfer.cpp` can provide
hardware-free starting points; their default regression values differ from the
guide's examples. Review the PNGs for clipping and personal information, and
update both guides when controls move or change.

截图使用真实界面、隔离的临时设置及模拟设备/文件，不是界面设计稿或真机验收记录。
2026-11-18 已用相同演示配置更新中英文设备页及输入页；设备页显示默认关闭的有线 UHID 插线自动连接开关。
同日按上述基线更新了两种语言的所有主窗口截图，以反映「复制手机截图」按钮的位置和状态；采集未读取真实手机屏幕或桌面剪贴板。
同日使用相同采集工具和演示配置更新中英文多设备布局截图，展示新的默认屏幕名称 `android2`。
本次基于 `f473676` 更新中英文主窗口截图。UHID 示例关闭了自动共享，仍可点击「手动剪贴板」。未启用 ADB 增强的 AOA 示例中，该按钮不可用。
版本 `0.1.12` 基于 `d802a99` 的工作区重新采集中英文主窗口截图。主窗口保留两个页签，输入与多设备截图展示「屏幕与显示」中的位置设置。
基于 `d997011` 的工作区更新两张输入页截图，显示「兼容旧版 Android 指针」。使用 Qt 离屏测试 `gui/test_window guidePointerScreenshot` 和 `OTG_TEST_POINTER_SCREENSHOTS` 采集。
基于 `91f5c7c` 的工作区更新两张共享服务页截图，显示新增的「蓝牙服务」一栏。使用 Qt 离屏测试 `gui/test_window guideSharedServiceScreenshot` 和 `OTG_TEST_SHARED_SERVICE_SCREENSHOTS` 采集。
更新时请同步中英文截图与指南，保留真实控件文字和状态，勿包含真实 PIN、设备标识或私钥。
本次用 `guideSharedServiceScreenshot` 重新采集了两张共享服务页截图，展示「启动蓝牙服务」入口。
