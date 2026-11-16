#!/usr/bin/env python3
"""Build a per-user NSIS installer from an installed Windows GUI stage."""

import argparse
import pathlib
import subprocess
import sys
import tempfile
if __package__:
    from .package_version import package_version
else:
    from package_version import package_version


DOCS = ("LICENSE", "README.md", "THIRD_PARTY.md")


def validate_stage(stage):
    required = [stage / "bin" / name for name in (
        "deskflow-bridge-gui.exe", "deskflow-bridge.exe", "platform-tools/adb.exe")]
    required += [stage / "share/deskflow-bridge" / name for name in DOCS]
    licenses = stage / "share/deskflow-bridge/licenses"
    if any(not path.is_file() for path in required) or not licenses.is_dir() or not any(
            licenses.rglob("*.txt")):
        raise ValueError("--stage must contain the GUI, bridge, bundled ADB and dependency licenses")
    files = sorted((p for p in stage.rglob("*") if p.is_file()), key=lambda p: p.relative_to(stage).as_posix())
    if any(p.is_symlink() for p in stage.rglob("*")):
        raise ValueError("--stage must not contain symlinks")
    for path in files:
        if any(c in str(path) for c in ('$', '"', '\n', '\r')):
            raise ValueError(f"NSIS-unsafe path: {path}")
    return files


def make_script(stage, output, version):
    files = validate_stage(stage)
    directories = sorted({p.relative_to(stage).parent for p in files},
                         key=lambda p: (len(p.parts), p.as_posix()))
    lines = [
        'Unicode true',
        f'Name "Deskflow Bridge {version}"',
        f'OutFile "{output}"',
        'InstallDir "$LOCALAPPDATA\\Programs\\Deskflow Bridge"',
        'RequestExecutionLevel user',
        'ShowInstDetails show',
        'ShowUninstDetails show',
        'Page instfiles',
        'UninstPage instfiles',
        'Section "Install"',
        '  IfFileExists "$INSTDIR\\Uninstall.exe" 0 +4',
        "    ExecWait '$\"$INSTDIR\\Uninstall.exe$\" /S' $0",
        '    IntCmp $0 0 +2',
        '      Abort "Could not remove the previous version"',
    ]
    for directory in directories:
        dest = str(directory).replace('/', '\\')
        lines.append(f'  SetOutPath "$INSTDIR\\{dest}"')
        for path in files:
            if path.relative_to(stage).parent == directory:
                lines.append(f'  File "{path}"')
    lines += [
        '  WriteUninstaller "$INSTDIR\\Uninstall.exe"',
        '  CreateDirectory "$SMPROGRAMS\\Deskflow Bridge"',
        '  CreateShortCut "$SMPROGRAMS\\Deskflow Bridge\\Deskflow Bridge.lnk" "$INSTDIR\\bin\\deskflow-bridge-gui.exe"',
        '  CreateShortCut "$SMPROGRAMS\\Deskflow Bridge\\Uninstall.lnk" "$INSTDIR\\Uninstall.exe"',
        '  WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "DisplayName" "Deskflow Bridge"',
        f'  WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "DisplayVersion" "{version}"',
        '  WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "UninstallString" \'$"$INSTDIR\\Uninstall.exe$"\'',
        '  WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "DisplayIcon" "$INSTDIR\\bin\\deskflow-bridge-gui.exe"',
        '  WriteRegDWORD HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "NoModify" 1',
        '  WriteRegDWORD HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge" "NoRepair" 1',
        'SectionEnd',
        'Section "Uninstall"',
        '  Delete "$SMPROGRAMS\\Deskflow Bridge\\Deskflow Bridge.lnk"',
        '  Delete "$SMPROGRAMS\\Deskflow Bridge\\Uninstall.lnk"',
        '  RMDir "$SMPROGRAMS\\Deskflow Bridge"',
        '  DeleteRegKey HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Deskflow-Bridge"',
    ]
    for path in files:
        rel = str(path.relative_to(stage)).replace('/', '\\')
        lines.append(f'  Delete "$INSTDIR\\{rel}"')
    lines.append('  Delete "$INSTDIR\\Uninstall.exe"')
    for directory in reversed(directories):
        dest = str(directory).replace('/', '\\')
        lines.append(f'  RMDir "$INSTDIR\\{dest}"')
    lines += ['  RMDir "$INSTDIR"', 'SectionEnd', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--version', help='Optional check against the staged build version')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('NSIS packaging requires Windows')
    stage = args.stage.resolve()
    try:
        version = package_version(stage, args.version)
    except ValueError as exc:
        parser.error(str(exc))
    output = args.output.resolve()
    if stage == output or stage in output.parents:
        parser.error('--output must be outside --stage')
    try:
        script = make_script(stage, output, version)
    except ValueError as exc:
        parser.error(str(exc))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='deskflow-bridge-nsis-') as temp:
        path = pathlib.Path(temp) / 'package.nsi'
        path.write_text(script, encoding='utf-8-sig')
        subprocess.run(['makensis', '/V2', str(path)], check=True)
    print(output)


if __name__ == '__main__':
    main()
