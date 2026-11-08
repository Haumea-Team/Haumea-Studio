#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage one complete editor distribution and make its user downloads."""
import argparse
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def add_namaka(archive, stage):
    # Copy only the companion's payload: its second editor and setup guides
    # aren't separate products users need to download or run.
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        wheels = [n for n in names if len(PurePosixPath(n).parts) == 2
                  and PurePosixPath(n).name.startswith('haumea_namaka-') and n.endswith('.whl')]
        if len(wheels) != 1:
            raise ValueError(f'{archive}: expected one Namaka wheel')
        prefix = wheels[0].split('/')[0] + '/'
        payload = [n for n in names if n == wheels[0] or n.startswith(prefix + 'wheelhouse/')]
        if not any(n.endswith('.whl') and '/wheelhouse/' in n for n in payload):
            raise ValueError(f'{archive}: Namaka wheelhouse is missing')
        for name in payload:
            rel = PurePosixPath(name[len(prefix):])
            if rel.is_absolute() or '..' in rel.parts or '\\' in str(rel):
                raise ValueError(f'Unsafe bundle path: {name}')
            if name.endswith('/'):
                continue
            dest = stage / 'namaka' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(bundle.read(name))
    shutil.copy2(ROOT / 'tools/release/namaka_install.py', stage / 'namaka/namaka_install.py')


def portable_zip(stage, output):
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(stage.rglob('*')):
            if not path.is_file():
                continue
            info = zipfile.ZipInfo('Haumea Studio/' + path.relative_to(stage).as_posix())
            info.create_system = 3
            info.external_attr = (0o100000 | (0o755 if path.name == 'haumea' else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())


def linux_setup(bundle, output):
    # This is a shell header followed by the exact, manifest-checked portable
    # payload. Extraction and installation use a private temporary directory.
    header = '''#!/bin/sh
set -eu
command -v python3 >/dev/null 2>&1 || { echo "Install Python 3.10 or newer, then run this installer again." >&2; exit 1; }
python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' || { echo "Python 3.10 or newer is required." >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
line=$(awk '/^__HAUMEA_PAYLOAD__$/ { print NR + 1; exit }' "$0")
tail -n +"$line" "$0" | tar -xz -C "$work"
python3 "$work/install-linux.py" "$@"
exit 0
__HAUMEA_PAYLOAD__
'''
    with output.open('wb') as dest, bundle.open('rb') as payload:
        dest.write(header.encode())
        shutil.copyfileobj(payload, dest)
    output.chmod(0o755)


def package(platform, engine, stage, output, namaka=None, glfw=None, engine_d3d9=None):
    if namaka is None:
        raise ValueError('Every platform download must include the Namaka bundle')
    if platform == 'windows' and glfw is None:
        raise ValueError('The Windows download needs --glfw glfw3.dll')
    stage.mkdir(parents=True, exist_ok=False)
    output.mkdir(parents=True, exist_ok=True)
    binary = 'haumea.exe' if platform == 'windows' else 'haumea'
    shutil.copy2(engine, stage / binary)
    if platform != 'windows':
        (stage / binary).chmod(0o755)
    if engine_d3d9:
        # Started by haumea.exe when OpenGL doesn't, or chosen in Preferences.
        shutil.copy2(engine_d3d9, stage / 'haumea-d3d9.exe')
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(ROOT / name, stage / name)
    for source, name in ((ROOT / 'fonts', 'fonts'),
                         (ROOT / 'tools/haumea/prefabs', 'prefabs')):
        shutil.copytree(source, stage / name, ignore=shutil.ignore_patterns('.DS_Store'))
    (stage / 'lang').mkdir()
    for path in sorted((ROOT / 'tools/haumea/lang').glob('*.po')):
        shutil.copy2(path, stage / 'lang' / path.name)
    if namaka:
        add_namaka(namaka, stage)
    if glfw:
        shutil.copy2(glfw, stage / 'glfw3.dll')
        # MSVC builds stage their app-local C/C++ runtime beside the editor.
        for dll in engine.parent.glob('*.dll'):
            if dll.name.lower() != 'glfw3.dll':
                shutil.copy2(dll, stage / dll.name)
    start = {
        'windows': 'Extract the entire ZIP, open the Haumea Studio folder and double-click haumea.exe.\n'
                   'Keep the folders and glfw3.dll beside it. No Python setup is needed to use the editor.\n'
                   'haumea-d3d9.exe is the Direct3D 9 version: haumea.exe starts it by itself when\n'
                   'OpenGL doesn\'t start, and Preferences > Renderer chooses between them.\n',
        'linux': 'Extract this archive into its own folder, then run ./haumea.\n'
                 'To add Haumea Studio to your applications menu, run: python3 install-linux.py\n'
                 'Dependencies: libglfw3, curl and Python 3.10+ (installer only); zenity or kdialog for file browsing.\n'
                 'Debian/Ubuntu: sudo apt install libglfw3 curl python3 zenity\n'
                 'Requires x86_64, glibc 2.38+ and OpenGL 3.3.\n',
        'macos': 'Extract the ZIP and run ./haumea from the Haumea Studio folder in Terminal.\n'
                 'This build is for Apple Silicon and requires GLFW: brew install glfw\n',
    }[platform]
    text = 'HAUMEA STUDIO\n\n' + start + '\nOn first launch, choose your GTA San Andreas folder and your server folder.\n'
    if namaka:
        text += '\nNamaka is included. Open Windows > Namaka in the editor to set up the optional assistant.\n'
    text += '\nUser guide: https://github.com/itsneufox/Haumea-Studio/blob/main/docs/USER_GUIDE.md\n'
    (stage / 'START_HERE.txt').write_text(text, encoding='utf-8')
    if platform == 'linux':
        shutil.copy2(ROOT / 'tools/release/install-linux.py', stage / 'install-linux.py')
        shutil.copy2(ROOT / 'assets/haumea-mark.svg', stage / 'haumea.svg')
    if platform in ('windows', 'linux'):
        # Keep these names for clients already using the built-in updater.
        name = 'haumea-windows-gl3.tar.gz' if platform == 'windows' else 'haumea-linux-amd64.tar.gz'
        bundle = output / name
        subprocess.run([sys.executable, str(ROOT / 'tools/release/make_update_bundle.py'),
                        str(stage), str(bundle)], check=True)
        if platform == 'linux':
            linux_setup(bundle, output / 'Haumea-Studio-Linux-Setup.run')
    if platform != 'linux':
        name = 'Windows' if platform == 'windows' else 'macOS-Apple-Silicon'
        portable_zip(stage, output / f'Haumea-Studio-{name}-Portable.zip')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'linux', 'macos'), required=True)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--namaka', type=Path)
    parser.add_argument('--glfw', type=Path)
    parser.add_argument('--engine-d3d9', type=Path, help='Windows: the Direct3D 9 haumea.exe')
    args = parser.parse_args()
    package(**vars(args))


if __name__ == '__main__':
    main()
