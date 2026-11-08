# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual release packager and Linux install/uninstall in a private HOME."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import plistlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
PACKAGER = ROOT / 'tools/release/package_downloads.py'


class DownloadsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='haumea-downloads-')
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        # A home path with characters meaningful to both shells and Desktop Exec.
        self.home = self.work / 'home space $cash %percent `tick` "quote" \'single\' café'
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home),
                        XDG_DATA_HOME=str(self.home / 'data'),
                        XDG_CONFIG_HOME=str(self.home / 'config'))
        self.engine = self.work / 'haumea'
        self.engine.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        self.engine.chmod(0o755)
        self.d3d9 = self.work / 'haumea-d3d9-build.exe'
        self.d3d9.write_bytes(b'Direct3D 9 test editor')
        self.glfw = self.work / 'glfw3.dll'
        self.glfw.write_bytes(b'GLFW test DLL')
        (self.work / 'vcruntime140.dll').write_bytes(b'Runtime test DLL')
        self.namaka = self.work / 'companion.zip'
        with zipfile.ZipFile(self.namaka, 'w') as bundle:
            bundle.writestr('Companion/haumea_namaka-1.0.0-py3-none-any.whl', b'wheel fixture')
            bundle.writestr('Companion/wheelhouse/dependency.whl', b'dependency fixture')
            bundle.writestr('Companion/haumea.exe', b'duplicate editor must not ship')

    def package(self, platform):
        stage = self.work / platform
        output = self.work / 'downloads'
        cmd = [sys.executable, str(PACKAGER), '--platform', platform,
               '--engine', str(self.engine), '--stage', str(stage), '--output', str(output)]
        cmd += ['--namaka', str(self.namaka)]
        if platform == 'windows':
            cmd += ['--glfw', str(self.glfw), '--engine-d3d9', str(self.d3d9)]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return stage, output

    def test_portable_windows_is_complete_and_has_one_root(self):
        stage, output = self.package('windows')
        with zipfile.ZipFile(output / 'Haumea-Studio-Windows-Portable.zip') as archive:
            names = archive.namelist()
            self.assertTrue(all(n.startswith('Haumea Studio/') for n in names))
            for name in ('haumea.exe', 'haumea-d3d9.exe', 'glfw3.dll', 'vcruntime140.dll', 'START_HERE.txt',
                         'LICENSE', 'THIRD_PARTY_NOTICES.md', 'lang/pt-BR.po',
                         'namaka/namaka_install.py', 'namaka/wheelhouse/dependency.whl'):
                self.assertIn('Haumea Studio/' + name, names)
            self.assertNotIn('Haumea Studio/namaka/haumea.exe', names)
        with tarfile.open(output / 'haumea-windows-gl3.tar.gz') as archive:
            manifest = archive.extractfile('MANIFEST').read().decode()
            for line in manifest.splitlines():
                expected, name = line.split('  ', 1)
                self.assertEqual(hashlib.sha256(archive.extractfile(name).read()).hexdigest(), expected)
        self.assertFalse(list(output.glob('*namaka*.zip')))

    @unittest.skipIf(os.name == 'nt', 'Linux installer uses sh and tar')
    def test_linux_setup_launch_reinstall_and_uninstall_preserves_user_files(self):
        stage, output = self.package('linux')
        setup = output / 'Haumea-Studio-Linux-Setup.run'
        for _ in range(2):
            subprocess.run(['sh', str(setup)], env=self.env, check=True, capture_output=True)
        installed = self.home / 'data/haumea-studio'
        launcher = self.home / '.local/bin/haumea'
        result = subprocess.check_output([str(launcher), 'a path with spaces', '$literal'], env=self.env, text=True)
        self.assertEqual(result.splitlines(), ['a path with spaces', '$literal'])
        desktop = self.home / 'data/applications/haumea.desktop'
        if shutil.which('desktop-file-validate'):
            subprocess.run(['desktop-file-validate', str(desktop)], check=True)
        (installed / 'keep-map.pwn').write_text('// user map')
        config = self.home / 'config/haumea/workspace.cfg'
        config.parent.mkdir(parents=True)
        config.write_text('user settings')
        subprocess.run([sys.executable, str(installed / 'install-linux.py'), '--uninstall'],
                       env=self.env, check=True, capture_output=True)
        self.assertFalse((installed / 'haumea').exists())
        self.assertFalse(launcher.exists())
        self.assertFalse(desktop.exists())
        self.assertEqual((installed / 'keep-map.pwn').read_text(), '// user map')
        self.assertEqual(config.read_text(), 'user settings')

    @unittest.skipIf(os.name == 'nt', 'Linux installer uses sh and tar')
    def test_linux_setup_refuses_an_unmanaged_directory(self):
        _, output = self.package('linux')
        target = self.home / 'data/haumea-studio'
        target.mkdir(parents=True)
        (target / 'keep.txt').write_text('existing files')
        result = subprocess.run(['sh', str(output / 'Haumea-Studio-Linux-Setup.run')],
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not managed', result.stderr)
        self.assertEqual((target / 'keep.txt').read_text(), 'existing files')
        self.assertFalse((target / 'haumea').exists())

    def test_macos_portable_keeps_executable_mode_and_resources(self):
        _, output = self.package('macos')
        with zipfile.ZipFile(output / 'Haumea-Studio-macOS-Apple-Silicon-Portable.zip') as archive:
            self.assertTrue(archive.getinfo('Haumea Studio/haumea').external_attr >> 16 & 0o111)
            self.assertIn('Haumea Studio/lang/pt-BR.po', archive.namelist())
            self.assertIn('Haumea Studio/START_HERE.txt', archive.namelist())
            for name in ('namaka/haumea_namaka-1.0.0-py3-none-any.whl',
                         'namaka/namaka_install.py', 'namaka/wheelhouse/dependency.whl'):
                self.assertIn('Haumea Studio/' + name, archive.namelist())
            self.assertIn('Namaka is included', archive.read('Haumea Studio/START_HERE.txt').decode())
        self.assertEqual([p.name for p in output.iterdir()],
                         ['Haumea-Studio-macOS-Apple-Silicon-Portable.zip'])

    def test_macos_download_refuses_to_ship_without_namaka(self):
        result = subprocess.run([sys.executable, str(PACKAGER), '--platform', 'macos',
                                 '--engine', str(self.engine), '--stage', str(self.work / 'macos'),
                                 '--output', str(self.work / 'downloads')], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must include the Namaka bundle', result.stderr)

    @unittest.skipIf(os.name == 'nt', 'macOS app resources use POSIX links')
    def test_macos_app_keeps_resources_and_namaka_accessible_to_the_editor(self):
        stage, _ = self.package('macos')
        spec = importlib.util.spec_from_file_location('package_macos', ROOT / 'tools/release/package_macos.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        image_root = self.work / 'disk-image'
        app = builder.create_app(stage, image_root, '1.2.3.42', '13.0')
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleShortVersionString'], '1.2.3')
        self.assertEqual(info['CFBundleVersion'], '42')
        self.assertEqual(info['CFBundlePackageType'], 'APPL')
        self.assertIn('/opt/homebrew/bin', info['LSEnvironment']['PATH'])
        executable = app / 'Contents/MacOS' / info['CFBundleExecutable']
        self.assertTrue(executable.stat().st_mode & 0o111)
        for name in ('fonts/Inter-Regular.ttf', 'lang/pt-BR.po', 'LICENSE',
                     'namaka/namaka_install.py', 'namaka/wheelhouse/dependency.whl'):
            resource = executable.parent / name
            self.assertTrue(resource.is_file(), name)
            self.assertTrue(resource.resolve().is_relative_to(app))
        icon = app / 'Contents/Resources' / info['CFBundleIconFile']
        self.assertEqual(icon.read_bytes()[:4], b'icns')
        self.assertEqual(os.readlink(image_root / 'Applications'), '/Applications')
        self.assertTrue((stage / 'haumea').is_file(), 'building the app must preserve the portable stage')


if __name__ == '__main__':
    unittest.main()
