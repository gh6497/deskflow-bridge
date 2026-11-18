# User guide screenshots / 用户指南截图

These PNGs show the project's real Qt widgets, not design mockups.
They accompany [the English guide](../USER_GUIDE.md) and [中文指南](../USER_GUIDE.zh-CN.md).

- Original capture: 2026-11-17, source `363541b` (base version `0.1.9`).
- Refreshed both languages of `device` and `input` on 2026-11-18 from the wired UHID auto-connect worktree
  based on `83a620b` (base version `0.1.10`), using the same harness and demo profile. The device page shows the new opt-in unchecked.
- Refreshed all main-window views in both languages on 2026-11-18 for **Copy phone screenshot**, from the worktree based on `94a8fd1` (base version `0.1.10`). The new action is enabled for the authorized demo and disabled in the AOA view without ADB. The separate Files window is unchanged.
- Linux, Qt 6.8.3, offscreen platform, light appearance, 1× scale.
- Main window: 1100 × 880; file window: 760 × 460.
- `-en` / `-zh-CN` suffixes select the interface language.
- `device`: UHID setup; `input`: scrolled dimensions and button settings;
  `layout`: one phone; `server`: shared Deskflow setup; `aoa`: USB without ADB;
  `multiple-devices`: two-device layout; `files`: pending phone shares.

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
更新时请同步中英文截图与指南，保留真实控件文字和状态，勿包含真实 PIN、设备标识或私钥。
