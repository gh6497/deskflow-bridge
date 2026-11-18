# Deskflow Bridge user guide

[简体中文](USER_GUIDE.zh-CN.md) · [Project home](../README.md) · [Download the desktop app](https://github.com/gh6497/deskflow-bridge/releases)

**Put your phone beside your computer and use one keyboard and mouse for both.** Move the pointer across the edge of your computer screen to click, scroll and type on Android; move it back to continue working on the computer.

This guide is for people installing and using the desktop app. No compilation is needed. You still look at the phone's own display: this project does not mirror its picture or turn it into an extended monitor for computer windows.

> Screenshots show the current, actual Linux GUI with demo devices and files. `TEST123`, addresses, installation paths and resolutions are examples; use your own values. Windows/macOS may look different, and some options depend on the platform and package version. Screenshots are not evidence of phone compatibility.

## Contents

- [1. What you can do](#features)
- [2. Download, install and prepare](#install)
- [3. Choose a connection type](#connection)
- [4. Find your way around](#interface)
- [5. First connection: USB cable + ADB + UHID](#first-connection)
- [6. Without USB debugging: USB (AOA HID)](#aoa)
- [7. Everyday use and pointer adjustments](#daily)
- [8. Enable optional features](#extras)
- [9. Connect multiple phones](#multiple)
- [10. Bluetooth connections](#bluetooth)
- [11. Troubleshooting](#troubleshooting)
- [Further reading](#more)

<a id="features"></a>

## 1. What you can do

- **Share keyboard and mouse**: clicks, dragging, scrolling and typing go to the phone while the pointer is on its screen.
- **Navigate Android**: assign the middle button to Home, the right button to Back, and side buttons to swipe up or down.
- **Switch between devices**: save separate settings for phones and tablets, then move between them using your screen layout.
- **Copy and paste text**: enable sharing to copy on the computer and paste on the phone, or synchronize text in the other direction.
- **Listen through your computer**: Linux/macOS/Windows can forward phone audio.
- **Transfer pictures and files both ways**: use the optional phone companion app to receive phone shares on the computer or send computer files to the phone.
- **Reduce repeated setup**: keep the app in the tray and reconnect remembered AOA or wired ADB + UHID phones when plugged in.

Start with keyboard and mouse control. Clipboard, audio and file sharing require ADB as well; file sharing also requires the companion app. Each is explained below.

<a id="install"></a>

## 2. Download, install and prepare

### Install two applications on the computer

1. Install [Deskflow](https://github.com/deskflow/deskflow/releases), which captures the computer's keyboard and mouse and handles screen switching.
2. Open this project's [Releases](https://github.com/gh6497/deskflow-bridge/releases), expand **Assets**, and download a desktop package whose name begins with **`deskflow-bridge-gui-`**.

Desktop packages contain the bridge, GUI runtime and ADB, but **do not include Deskflow**. You normally do not need to install scrcpy separately. GitHub's automatically generated `Source code` archives are not installers.

### Pick a package for your computer

| Computer | File to choose | How to start |
| --- | --- | --- |
| Windows x64 | `…-windows-x86_64-setup.exe` | Run the installer, then open Deskflow Bridge from the Start menu |
| Windows portable | `…-windows-x86_64.zip` (with `gui` in the name) | Extract everything and run `bin/deskflow-bridge-gui.exe`; keep the accompanying files |
| Ubuntu / Debian / Linux Mint x86_64 | `…-linux-amd64.deb` | Install with the system package installer, then open from the application menu |
| Other Linux x86_64 desktops | `…-linux-x86_64.AppImage` | Allow execution in file properties and double-click; the system needs FUSE |
| Apple Silicon Mac | `…-macos-arm64.dmg` | Open and drag the app into Applications |
| Intel Mac | `…-macos-x86_64.dmg` | Open and drag the app into Applications |

The Windows installer and macOS image are currently unsigned/not notarized, so the system may show a first-launch prompt. Check that the download came from the project release page above. On macOS, use “System Settings → Privacy & Security → Open Anyway” if needed. See the [README package instructions](../README.md#publishing-prebuilt-packages) for Linux installation commands and running an AppImage without FUSE.

### Phone and system permissions

- An Android phone or tablet and a USB cable that supports **data transfer**; charging-only cables will not work.
- Keep the phone unlocked during initial setup so you can approve requests and see the pointer.
- For ADB + UHID, enable USB debugging and authorize the computer. Root is not required.
- On macOS, give **Deskflow** Accessibility permission. Allow Input Monitoring or USB accessory access if requested.
- Linux Wayland desktops may ask you to approve keyboard/mouse sharing. USB access on Linux also needs the appropriate device permissions.

Use just one phone for your first setup.

<a id="connection"></a>

## 3. Choose a connection type

“AOA” and “UHID” are the mode names used in the interface. Choose mainly by how you want to connect and whether you want to enable debugging.

| What you want | Choose | Prepare |
| --- | --- | --- |
| A USB cable with debugging enabled, especially on Windows | **ADB + UHID (USB / wireless)** | Enable and authorize USB debugging; keep normal ADB drivers on Windows |
| Keyboard and mouse without USB debugging | **USB (AOA HID)** | USB access; Windows also needs a driver compatible with libusb |
| Control over Wi-Fi | **ADB + UHID (USB / wireless)** | A phone with wireless debugging, paired and connected |
| Bluetooth control without depending on ADB | **Bluetooth Low Energy** | Compatible Linux Bluetooth hardware/software, or a Windows package with experimental BLE |

**For your first connection, start with USB + ADB + UHID below**, particularly on Windows, where it avoids replacing USB drivers for AOA. Fresh Linux/macOS profiles may default to AOA, so select UHID explicitly. If you do not want debugging, go to the [AOA instructions](#aoa).

Wireless ADB and Bluetooth are different connection types. macOS currently has no BLE mode. If an option is missing, check the features included in your release.

<a id="interface"></a>

## 4. Find your way around

![Device settings: device list on the left, connection and ADB settings on the right, connection actions and logs below](screenshots/device-en.png)

- **Device list on the left**: select the phone to view, configure or connect. Buttons below add devices, connect all or disconnect all.
- **Device settings**: select a connection, phone, dimensions and input features. **Scroll down inside the right-hand panel** to reach dimensions, mouse buttons and the companion app.
- **Screen layout**: choose which side of the computer the phone occupies and therefore which edge you cross to reach it.
- **Shared server**: select Deskflow and choose automatic startup or an existing server.
- **Bottom-right buttons**: Connect, Disconnect and Copy phone screenshot apply to the selected device. Files & sharing opens the transfer window.
- **Runtime logs below**: read connection errors and filter by device.
- **Language at the top right**: switch between English and Chinese at any time. Appearance follows the system theme.

<a id="first-connection"></a>

## 5. First connection: USB cable + ADB + UHID

### Step 1: Make the phone available to the computer

1. Enable “Developer options” and “USB debugging” in the phone's settings. The location varies by brand; commonly, tap the build number repeatedly under “About phone”, then find Developer options in Settings.
2. Connect a data cable, unlock the phone, and approve this computer in the “Allow USB debugging” prompt.
3. Open Deskflow Bridge and select **ADB + UHID (USB / wireless)** under “Device settings → Connection type”.
4. Click **Refresh devices** and select the phone marked **Online**. A single device is usually selected automatically; its identifier appears in **ADB target (required)**.
5. Use **Test ADB** to confirm that the target is available.

For “Unauthorized”, check the phone's approval prompt, then refresh. If Windows never finds the device, check the manufacturer's ADB driver. This mode does not require replacing it with WinUSB.

### Step 2: Check the screen dimensions

Click **Read screen size**, then scroll down to “Screen and display”.

![Screen dimensions and input settings after scrolling down: example 1080×2340 with both multipliers at 1](screenshots/input-en.png)

- **Phone screen resolution**: the phone's width × height in its current orientation. `1080 × 2340` is a portrait example; your phone may differ. Enter it manually if reading fails.
- **Swap dimensions**: use this when changing to landscape.
- **Bridge resolution multiplier and Movement multiplier**: start with both at `1.00 ×`.
- **Phone screen name**: keep `android` for a single phone. This is its name in Deskflow, not the phone model.
- **Mouse mode**: keep **Absolute (recommended)**.

Leave audio, clipboard, automatic unlock and other extras off until basic control works.

### Step 3: Place the phone in the layout

Open **Screen layout**, set **Adjacent screen** to `computer`, and select the phone's direction relative to the computer. If the phone is on your right, choose **Right**.

![Screen layout with android to the right of computer](screenshots/layout-en.png)

The preview shows neighboring screens, not their actual relative resolutions. It is read-only; adjust the selectors below rather than dragging the boxes.

### Step 4: Set up Deskflow

Open **Shared server**:

1. Check **Configure and start local Deskflow automatically**.
2. For one computer, keep the computer screen name `computer` and port `24800`. The local server address automatically uses `127.0.0.1`.
3. Under **Programs and mode**, the **Deskflow** field tries to locate the installed program. If empty, click **Browse** and select **`deskflow-core` or `deskflow-server`** (`.exe` on Windows), rather than Deskflow's graphical app. On a Mac, it is usually inside `/Applications/Deskflow.app/Contents/MacOS/`.
4. The **ADB** field normally points to the bundled program and can be left alone. The Linux paths in the screenshot are examples.

![Shared server: automatic Deskflow startup, computer name, port and program paths](screenshots/server-en.png)

Automatic mode starts Deskflow when you click Connect. You do not need to start another server manually on the same port.

**Already using Deskflow?** Turn off automatic startup and enter the existing server's address and port. Add a phone screen with the same name, such as `android`, in Deskflow's own layout. This app's preview does not change an external server's layout. If that server uses TLS, enable TLS here and enter its server fingerprint. If it also requires client authentication, generate a client certificate and trust that client fingerprint in Deskflow. See [TLS notes](../README.md#tls).

### Step 5: Connect and try it

1. Click **Connect** at the bottom right and wait for **Connected**.
2. For a right-side layout, move the mouse to the computer's right edge and keep moving right. You should see a movable pointer on the phone.
3. Click a text field on the phone and type a few characters with the computer keyboard. Try scrolling, dragging and Back.
4. Move out of the phone screen toward the computer to return. With the phone on the right, leave through the phone screen's left edge.

After the app says Connected, also check that the phone actually responds. If there is no pointer, see [Troubleshooting](#troubleshooting).

<a id="aoa"></a>

## 6. Without USB debugging: USB (AOA HID)

This mode can provide keyboard and mouse control with a data cable, without ADB or the phone companion app.

![USB AOA configuration with a manual USB serial and ADB enhancements disabled](screenshots/aoa-en.png)

1. Select **USB (AOA HID)** and turn off **Enable ADB enhancements for this device**.
2. Plug in the phone, click **Refresh devices** and select the matching USB device. If it is not found, check the cable and USB permissions. You can also enter its correct USB serial manually.
3. Enter the phone's width and height for its current orientation. The **USB serial** is not a Wi-Fi address or BLE address; do not copy the screenshot's example.
4. Configure **Screen layout** and **Shared server** as above, then connect.

Linux needs USB access permissions; see [Linux USB permissions](../README.md#rootless-usb-access-on-linux-udev-rule). Allow accessory access if macOS requests it. Windows AOA needs a libusb-compatible driver, which may conflict with existing ADB/file-transfer drivers. Read [Windows USB guidance](WINDOWS_USB.md) before changing drivers, or use ADB + UHID instead.

<a id="daily"></a>

## 7. Everyday use and pointer adjustments

### Click, go back and scroll

- Left-click to select; hold the left button while moving to drag.
- Use the wheel to scroll; the exact behavior depends on the phone app.
- Fresh GUI profiles assign the middle button to Android Home and the right button to Android Back. Select normal button behavior under **Input and features** if preferred. Existing profiles retain their saved choices.
- Side button 1 defaults to swipe up, and side button 2 to swipe down. **Side swipe distance** defaults to `17%`. Change the direction or restore normal side buttons as needed.
- Navigation mappings do not require the companion app or ADB. Home/Back depend on your phone system supporting the corresponding shortcuts.

### Pointer too slow or too fast

Disconnect the selected device before adjusting settings. Change one thing at a time:

- **Movement multiplier**: increasing it moves the phone pointer farther for the same computer mouse movement.
- **Bridge resolution multiplier**: increasing it makes the phone's virtual Deskflow screen smaller, also changing the movement needed to cross it. It **does not reduce the phone's actual display quality**.
- Prefer **Absolute (recommended)**. Relative mode is a compatibility option and may drift because of Android mouse acceleration.

After rotating the phone without rotation tracking enabled, disconnect, read the dimensions again or swap width/height, then reconnect.

### Disconnect, hide and quit

- **Disconnect** ends control of the selected device; **Disconnect all** ends all device sessions.
- **Closing the window** hides it when a system tray is available and keeps connections active. Click the tray icon or **Show window** to restore it.
- **Quit** from the tray menu to exit. Without a system tray, closing the window disconnects and exits.
- Settings are saved automatically. Selecting a different device does not disconnect the others.

### Connect automatically when you plug in a USB cable

In the phone's device settings, choose **ADB + UHID** or **USB (AOA HID)**. For UHID, authorize USB debugging on the phone and select its USB ADB serial, not a Wi-Fi address. AOA does not need USB debugging.

Enable **Automatically connect this device when USB is inserted** (off by default) and connect once by hand until the status reads **Connected**. Leave the app running, optionally hidden in the tray. Next time you plug in that phone, it reconnects automatically. If UHID is waiting for authorization, approve the prompt on the phone.

After a manual disconnect or failed connection, click **Connect** to retry immediately or unplug and reinsert the cable. Quitting the app stops detection; wireless ADB is not monitored by this option.

<a id="extras"></a>

## 8. Enable optional features

Disconnect the device you want to change, update **Device settings → ADB enhancements**, then reconnect. With AOA/BLE, enable ADB enhancements and select an authorized ADB target for **that same phone**. UHID already requires ADB. Bluetooth does not replace this ADB connection.

### Share the text clipboard

Enable **Share text clipboard**, connect, copy text on the computer, move to the phone and paste into a text field. Copying on the phone can synchronize text back to the computer. No companion app is needed.

This shares text; use **Files & sharing** below for pictures and files. Copying on a phone in the background may also update the shared clipboard.

### Copy a phone screenshot

Select a phone on the left and click **Copy phone screenshot**. With the window hidden, choose **Copy phone screenshot → device name** in the tray. When the copy completes, press **Ctrl+V** (macOS: **⌘V**) in an app that accepts images. The screenshot goes to the clipboard of the computer running the GUI, even if the Deskflow server runs elsewhere.

The phone must already be online in ADB and have authorized the computer. For AOA/BLE, enable **ADB enhancements**; BLE requires that phone’s explicit wireless ADB address. No companion app, text sharing or active Deskflow session is needed. An offline phone, failed capture or timeout leaves the clipboard unchanged. Android may blank protected content.

### Forward phone audio

On Linux/macOS/Windows, enable **Forward audio** under **ADB enhancements** to play phone audio through the computer. This option is saved per device and is off by default. Android 11 or later and authorized ADB are required. Keep Android 11 unlocked when starting capture. Calls, protected content and apps that disallow capture may be silent.

On macOS, audio plays through the system default output (speakers or headphones), without microphone permission or an extra audio driver. If there is no sound, check the output device and volume in System Settings, then disconnect and reconnect after selecting an available output. An audio-output failure is logged and keyboard/mouse control remains available. Audio must be included in the build; older macOS packages may not show the switch.

Normal disconnection of an audio session tries to request playback pause. Unplugging or losing ADB can prevent that request; the companion below offers USB-disconnect pause.

### Follow screen rotation

Enable **Follow screen rotation** to update control dimensions as the phone rotates. UHID can do this on its own. AOA/BLE also need audio or clipboard enabled to keep an ADB session running.

### Wake the phone and unlock with a numeric PIN

UHID sends a wake action when the pointer enters the phone screen. You still need to unlock the phone normally. Optionally enable **Try automatic numeric PIN unlock on entry** and enter that phone's numeric PIN. The app checks lock state through ADB before attempting a swipe and PIN entry. Phone lock screens vary in compatibility.

This is optional. Enabling it stores the PIN in local computer settings, not runtime logs. Leave it off and unlock manually if you do not want the PIN stored on the computer.

### Install the optional phone companion

Keyboard/mouse, text clipboard and audio forwarding do not themselves need the companion. **File sharing, phone-side connection status and pausing media on USB disconnect** use it.

1. Confirm that the same phone is online and authorized in ADB. Scroll down to **Companion app (optional)** in device settings (USB/UHID page).
2. Desktop releases normally include the APK. Click **Install / update APK**; installation is not automatic.
3. Click **Open phone setup** and open the companion on the phone. For media pause/autoplay, manually allow notification access as prompted. File sharing alone does not need it.
4. If the phone restricts auto-start, background activity or battery use, allow the companion to run in the background without battery restrictions. Open it once after installation or an update.

To get phone notifications, turn on **Notify when Deskflow connects or disconnects** on the companion home screen (off by default) and allow Android notifications for the app. It does not need the notification access used for media controls. When the desktop GUI reports changes over authorized ADB, the phone alerts once on connection and once on disconnection. With the updated desktop and companion apps, the phone can alert on unplug for a confirmed wired session, even after ADB goes offline. Plugging or unplugging USB without a Deskflow session does not trigger an alert.

The companion can request that playing media pause when a USB data connection disconnects. Brief disconnections can trigger it too, and it is not limited to Deskflow USB sessions. Autoplay is a separate phone-side option, off by default, requiring a selected player. Whether media actions take effect depends on the player and system.

### Transfer pictures and files

![Files & sharing: pending phone shares, receive actions and sending computer files to the phone](screenshots/files-en.png)

**Phone → computer**

1. Select content in the phone's gallery, file manager or another app. Use Android **Share → Share to computer**.
2. Confirm **Prepare share** on the phone.
3. Select that phone in the desktop app and open **Files & sharing → Refresh phone shares**.
4. Select files, click **Receive selected**, and choose a destination folder on the computer.

**Computer → phone**

1. Click **Send files to phone** in the same window and select files, or drag local files into the window and confirm.
2. Wait for completion. Open **Files & sharing** in the phone companion, refresh, then use **Save as** to choose where to save a received file.
3. To place files in the gallery or a chosen folder automatically, enable automatic saving in the phone companion and grant access to the relevant destination.

Transfers do not overwrite or open files automatically. The per-file limit is `1 GiB`, with up to `32` files / `2 GiB` in phone staging storage. Files are supported, not whole folders. Shared text becomes a text file rather than a clipboard update. For long transfers, keep the companion foregrounded and the phone awake until finished. To transfer to another phone, close the file window, select that device and reopen it.

### Use wireless ADB

If your phone supports wireless debugging (usually Android 11+), UHID can work over Wi-Fi:

1. Put the phone and computer on the same reachable network and enable **Developer options → Wireless debugging** on the phone.
2. Open **Wireless ADB setup** on the desktop. Choose pairing with a code on the phone.
3. Select the discovered pairing address or enter the IP and port shown in the phone's pairing dialog. Enter its six-digit pairing code and pair.
4. Then select the connection address and port shown on the phone's main **Wireless debugging** page. A successful connection fills the target in device settings.
5. Select ADB + UHID, read dimensions and connect as above.

**Pairing and connection ports can differ.** Addresses/ports may also change after switching networks or restarting wireless debugging. Update the target if needed. The companion's wireless ADB helper opens system settings; it cannot approve authorization for you.

<a id="multiple"></a>

## 9. Connect multiple phones

![Multiple-device example: android to the right of the computer, followed by android-2](screenshots/multiple-devices-en.png)

1. Disconnect all devices, click **Add device**, and set each phone's own connection target and dimensions.
2. Use unique **Phone screen names**, such as `android` and `android-2`. **Display name** can be any name that helps you recognize the device.
3. Select each device and set its neighbor and direction in **Screen layout**. For example, place the second phone to the right of `android` to obtain the layout shown.
4. Configure Deskflow once under **Shared server**, then connect individually or click **Connect all**.

Multiple AOA/UHID devices can connect, optionally with at most one BLE device. Input goes to the screen containing the pointer; it is not broadcast to all phones. Disconnecting one device does not deliberately stop the others.

Before adding/removing devices or changing screen names, layout or the shared server, disconnect all and wait for shutdown to finish. Connected-device parameters are locked, while connection targets and dimensions for other idle devices remain editable.

<a id="bluetooth"></a>

## 10. Bluetooth connections

BLE lets the phone treat the computer as a Bluetooth keyboard/mouse without USB debugging for basic input. Adapter and phone compatibility matter, so try wired control first to learn the basics.

- **Linux**: select Bluetooth Low Energy and enter the adapter (often `hci0`), the target phone's identity address in BlueZ and its dimensions. Click Connect, then pair/connect to **Deskflow Bridge** in the phone's Bluetooth settings. Bluetooth isolation affects other devices on that adapter; leave it off initially. The target must be paired and be the adapter's only connection.
- **Windows**: the option appears only in builds containing experimental BLE. Enter the target phone's BLE address as reported by Windows. Pairing and control still have compatibility limitations.
- **macOS**: no BLE backend currently; consider wireless ADB + UHID for wireless control.

The target address belongs to the **phone**, not the computer adapter or an arbitrary rotating advertisement. For help identifying it, follow the [Linux Bluetooth instructions](../README.md#3-control-the-phone-through-linux-ble) or [Windows Bluetooth guide](WINDOWS_BLE.md).

Normal Disconnect differs from Stop idle Bluetooth service. Linux keeps its persistent service after the GUI exits. Windows keeps an idle service while the GUI is running and stops it on Quit. Stopping the service or deleting pairings is not a routine reconnect step; pairing-cache behavior after restarts can still vary by device.

<a id="troubleshooting"></a>

## 11. Troubleshooting

| What you see | What to check first |
| --- | --- |
| Connection rejected with a configuration error | The status and runtime log identify the first invalid field and its requirement (for example, port 1–65535, width/height 1–32767, missing ADB target or conflicting TLS fingerprint settings). Correct that field and connect again; any remaining error is shown on the next attempt. PIN values are never included in these messages |
| No phone after refreshing | Try a data-capable cable/USB port. UHID: debugging authorization and ADB driver. AOA: USB permissions and driver |
| Unauthorized or Offline | Unlock and approve the computer, then refresh. For wireless, check the current IP, connection port and network |
| Deskflow not found | Install Deskflow and select `deskflow-core` / `deskflow-server` under Shared server, rather than its GUI |
| Port already in use | Check for a manually started Deskflow server. Use existing-server mode or another free port for automatic startup |
| Unknown screen or mismatched name | The external Deskflow screen name must exactly match this app and be unique |
| Connected, but cannot move onto the phone | Check the layout direction, Deskflow's input-capture/Accessibility permissions and any Deskflow screen-switch lock |
| No phone pointer or no response to clicks | Check logs for UHID unavailable or USB errors. Start with absolute mode; try relative mode after disconnecting for an incompatible device |
| Wrong orientation or strange movement range | Disconnect and read current dimensions or swap width/height; reset both multipliers to 1 first |
| Some symbols type incorrectly | Set the phone's physical keyboard to English (US). Deskflow protocol 1.8 with per-key language information can correct common symbols and digits such as `@`, `#`, and `1` from other host layouts. The bridge does not switch the phone's layout or correct accented letters and dead keys. |
| Clipboard, audio or files fail | Check that this phone's ADB target is online and authorized. Files also need an updated, opened companion; audio needs a supported platform and Android version |
| Sent files do not appear in the phone gallery | Refresh Files & sharing in the companion and Save as, or enable the relevant automatic saving; files first use private companion storage |
| Control continues after closing the window | Closing hides to the tray when available; use Disconnect or Quit from its menu |

The app's own runtime messages use the selected interface language. External output under labels such as `[ADB output]` and `[Deskflow output]` is left untranslated for troubleshooting. Switching languages affects new entries, not the ones already displayed.

**To report a problem**, check the version under **About**, note the computer OS, phone model/Android version, connection type and steps, and include relevant runtime logs or screenshots in [project Issues](https://github.com/gh6497/deskflow-bridge/issues). Remove device identifiers or paths you do not want public, and do not attach a PIN or private-key file.

<a id="more"></a>

## Further reading

The guide above is enough for everyday use. These references cover detailed setup and troubleshooting; most feature-specific technical documents are currently in Chinese:

- [ADB + UHID device requirements](UHID.md)
- [Windows USB drivers and discovery](WINDOWS_USB.md)
- [Audio forwarding](AUDIO.md) · [Text clipboard](CLIPBOARD.md)
- [Android companion app](ANDROID_COMPANION.md) · [File sharing](FILE_SHARING.md)
- [Windows BLE](WINDOWS_BLE.md) · [Linux BLE compatibility records](LINUX_BLE_HANDOVER.md)
- [Technical overview, CLI and building from source](../README.md)
