#!/usr/bin/env python3
"""Package an installed Linux distribution tree as a Debian package."""

import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile
if __package__:
    from .package_version import package_version
else:
    from package_version import package_version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True,
                        help="absolute-prefix directory produced by cmake --install")
    parser.add_argument("--version", help="Optional check against the staged build version")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="output .deb path")
    args = parser.parse_args()

    stage = args.stage.resolve()
    features_path = stage / "share/deskflow-bridge/BUILD_FEATURES.json"
    ble = False
    audio = False
    if features_path.exists():
        try:
            features = json.loads(features_path.read_text(encoding="utf-8"))
            if not isinstance(features.get("ble"), bool):
                raise ValueError("ble must be boolean")
            ble = features["ble"]
            if "audio" in features and not isinstance(features["audio"], bool):
                raise ValueError("audio must be boolean")
            audio = features.get("audio", False)
        except (ValueError, AttributeError) as exc:
            parser.error(f"Invalid BUILD_FEATURES.json: {exc}")
    try:
        version = package_version(stage, args.version)
    except ValueError as exc:
        parser.error(str(exc))
    if not (stage / "bin/deskflow-bridge-gui").is_file() or not (stage / "bin/deskflow-bridge").is_file():
        parser.error("--stage must contain bin/deskflow-bridge-gui and bin/deskflow-bridge "
                     "(build with BUILD_GUI=ON and run cmake --install first)")
    arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    bundled_adb = (stage / "bin/platform-tools/adb").is_file()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="deskflow-bridge-deb-") as temp:
        root = pathlib.Path(temp) / "root"
        app = root / "opt/deskflow-bridge"
        shutil.copytree(stage, app, symlinks=True)
        docs = root / "usr/share/doc/deskflow-bridge"
        docs.mkdir(parents=True)
        shutil.copyfile(pathlib.Path(__file__).resolve().parent.parent / "LICENSE",
                        docs / "copyright")
        bindir = root / "usr/bin"
        bindir.mkdir(parents=True)
        (bindir / "deskflow-bridge-gui").symlink_to("/opt/deskflow-bridge/bin/deskflow-bridge-gui")
        (bindir / "deskflow-bridge").symlink_to("/opt/deskflow-bridge/bin/deskflow-bridge")
        if ble:
            units = root / "usr/lib/systemd/user"
            units.mkdir(parents=True)
            shutil.copyfile(pathlib.Path(__file__).resolve().parent.parent /
                            "packaging/deskflow-bridge-ble@.service", units / "deskflow-bridge-ble@.service")
        applications = root / "usr/share/applications"
        applications.mkdir(parents=True)
        icons = root / "usr/share/icons/hicolor/scalable/apps"
        icons.mkdir(parents=True)
        shutil.copyfile(pathlib.Path(__file__).resolve().parent.parent / "packaging/deskflow-bridge.svg",
                        icons / "deskflow-bridge.svg")
        (applications / "deskflow-bridge.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Deskflow Bridge\n"
            "Comment=Control an Android device with Deskflow via USB or Bluetooth LE\n"
            "Exec=/opt/deskflow-bridge/bin/deskflow-bridge-gui\n"
            "Icon=deskflow-bridge\n"
            "Terminal=false\nCategories=Utility;\n", encoding="utf-8")

        control = root / "DEBIAN"
        control.mkdir()
        # Qt and ADB are bundled, but their platform plugins still need system libraries.
        dependencies = ("libusb-1.0-0, libssl3 | libssl3t64, libc6, libstdc++6, libgl1, libegl1, "
                        "libx11-6, libxcb1, libxkbcommon0, libxcb-cursor0")
        if ble:
            dependencies += ", bluez, libglib2.0-0t64 | libglib2.0-0, pkexec"
        if audio:
            dependencies += ", libasound2"
        if not bundled_adb:
            dependencies += ", adb"
        # GTK integration is optional: normal apt installs can follow GTK
        # desktops, while minimal/KDE installations may use Qt's default theme.
        # Do not require GTK when the Qt build did not ship this plugin.
        recommendations = ""
        if (stage / "plugins/platformthemes/libqgtk3.so").is_file():
            recommendations = "Recommends: libgtk-3-0t64 | libgtk-3-0\n"
        (control / "control").write_text(
            f"Package: deskflow-bridge-gui\nVersion: {version}\nArchitecture: {arch}\n"
            "Maintainer: deskflow-bridge contributors <noreply@github.com>\n"
            f"Depends: {dependencies}\n"
            f"{recommendations}"
            "Section: utils\nPriority: optional\n"
            "Homepage: https://github.com/gh6497/deskflow-bridge\n"
            "Description: Deskflow to Android HID bridge\n"
            " Control an Android device via USB AOA HID or Linux Bluetooth LE.\n",
            encoding="utf-8")
        subprocess.run(["dpkg-deb", "--build", "--root-owner-group", str(root), str(output)],
                       check=True)
    print(output)


if __name__ == "__main__":
    main()
