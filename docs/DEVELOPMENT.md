# Development guide

This document is for contributors and maintainers. If you only want to use Deskflow Bridge, start with the [user guide](USER_GUIDE.md).

## Project scope

Deskflow Bridge is a small Deskflow client and Android HID bridge:

```text
computer keyboard/mouse
        -> Deskflow server
        -> deskflow-bridge
        -> AOA HID / ADB + UHID / Bluetooth LE HID
        -> Android phone
```

The project does not mirror the phone screen. The phone remains a physical display, while the bridge sends keyboard and pointer input through an Android-compatible HID transport.

## Repository map

| Path | Purpose |
|---|---|
| `src/` | C11 bridge core, Deskflow protocol, key mapping, HID reports, USB scanning |
| `platform/posix/` | Shared POSIX ADB/scrcpy session and platform glue |
| `platform/linux/` | Linux audio and BLE backends |
| `platform/windows/` | Windows audio, BLE, and control backends |
| `gui/` | Qt desktop GUI, device settings, Deskflow process lifecycle |
| `android/` | Optional Android companion application |
| `tests/` | Protocol, bridge, GUI, platform, and packaging regression tests |
| `scripts/` | Fixed-version dependency downloads, license collection, and packaging helpers |
| `docs/` | User and feature-specific documentation |

## Build profiles

The normal full build enables all features supported by the current platform:

```bash
# Linux: GUI, UHID, audio, clipboard, and Linux BLE
cmake -S . -B build -DOTG_PROFILE=full -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The same profile works on macOS and Windows; unsupported platform features are omitted by CMake. Windows uses the normal Visual Studio/vcpkg toolchain and should pass the appropriate toolchain file. macOS GUI builds require a matching Intel or Apple Silicon Qt installation.

For a small command-line USB build:

```bash
cmake -S . -B build -DOTG_PROFILE=manual \
  -DBUILD_GUI=OFF -DBUILD_AUDIO=OFF -DBUILD_CLIPBOARD=OFF \
  -DBUILD_UHID=OFF -DBUILD_BLE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

See the platform and feature documents for additional dependencies:

- [ADB + UHID](UHID.md)
- [Audio](AUDIO.md)
- [Clipboard](CLIPBOARD.md)
- [Windows USB](WINDOWS_USB.md)
- [Windows BLE](WINDOWS_BLE.md)
- [Linux BLE handover](LINUX_BLE_HANDOVER.md)

## Architecture notes

### Deskflow protocol

The bridge implements the client side of the Deskflow/Synergy/Barrier TCP protocol. Each message has a four-byte big-endian outer frame length. Fields inside the payload use the protocol's own integer and string encodings. Keep the two layers separate when changing parsing code.

The handshake exchanges the protocol hello, screen information, and screen options before input is accepted. The client acknowledges normal messages with `CNOP`, echoes keep-alive messages, and ignores unknown non-fatal messages. Only protocol error messages should terminate the session.

### HID transports

- **AOA HID** sends HID descriptors and reports through Android Open Accessory control requests using libusb.
- **ADB + UHID** creates virtual HID devices through the Android shell and shares the ADB/scrcpy session with optional audio and clipboard features.
- **BLE HID** exposes the keyboard and pointer through the platform Bluetooth HID interface. Linux has the persistent service/session lifecycle; Windows has a separate experimental native backend.

The default pointer is an absolute HID digitizer report. It carries the current coordinate together with button and range state, avoiding the accumulated drift of Android's accelerated relative mouse path. Relative mode remains available for compatibility and is not guaranteed to track the Deskflow boundary exactly.

### Input state

Keyboard modifiers are kept separately from the six-key HID array. Pointer enter/leave, button, wheel, and movement events are ordered so that a release or range transition cannot be overtaken by a later movement. Transport backpressure must not reorder state barriers.

## Testing and validation

Run the complete test suite after source or build-system changes:

```bash
ctest --test-dir build --output-on-failure
```

The automated tests cover protocol framing, socket cancellation, HID report generation, GUI settings and process lifecycle, fake ADB behavior, BLE state machines, and package layout. They do not prove compatibility with every Android ROM or USB/Bluetooth controller.

For source-only checks, keep C11 files clean under the project's warning policy:

```bash
for f in src/*.c; do
  gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
    -Isrc -Ibuild/generated "$f"
done
```

When reporting a hardware result, record the host OS, phone model, Android version, transport, Deskflow version, and whether the test used ADB. Do not include private device identifiers or clipboard contents in logs.

## Release process

1. Update the root `VERSION` file.
2. Keep the version change in its own reviewed commit or include it clearly in the release change.
3. Create a matching tag such as `v0.1.11`.
4. Push the tag and wait for the GitHub Actions build, test, and packaging jobs.
5. Check the generated release assets on Linux, Windows, and both macOS architectures before announcing the release.

The release workflow verifies that the tag matches `VERSION`. Release assets should contain the GUI package, CLI package where supported, dependency notices, and the correct architecture in the filename. Deskflow is intentionally not bundled.

## Security and TLS

Deskflow TLS uses the server fingerprint and optional client certificate configured by the user. Never replace the trusted server fingerprint with the client fingerprint. Client PEM files contain private keys and must be written with private permissions and kept out of logs and commits.

The GUI may invoke ADB, Deskflow, and bridge subprocesses. Changes to command construction, temporary files, device selection, or privilege boundaries need tests for quoting, cancellation, exact target selection, and cleanup.

## Documentation rules

- Keep `README.md` and `README.zh-CN.md` short and user-oriented.
- Keep `docs/USER_GUIDE.md` and `docs/USER_GUIDE.zh-CN.md` aligned when installation or GUI behavior changes.
- Put protocol, architecture, build, release, and test details here or in a feature-specific document.
- Mark experimental features and real-device validation limits explicitly.
- Do not put private device identifiers, access tokens, clipboard text, or unreleased signing material in documentation.

## License and attribution

The project is Apache-2.0. Some HID and ADB integration is based on scrcpy and the Deskflow protocol; see [THIRD_PARTY.md](../THIRD_PARTY.md) for the complete notices and attribution details.
