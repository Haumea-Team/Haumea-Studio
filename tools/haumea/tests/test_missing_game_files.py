# SPDX-License-Identifier: GPL-2.0-or-later
"""Deleted core game files lead to a repairable chooser, never a silent exit."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from haumea_test_env import editor_env, run_editor
from test_ui_scripts import BINARY, XVFB, start_xvfb

HERE = Path(__file__).resolve().parent
REQUIRED = ('data/gta.dat', 'data/default.dat', 'models/gta3.img',
            'models/gta_int.img', 'models/particle.txd', 'data/timecyc.dat')


@unittest.skipUnless(BINARY and XVFB, 'needs the editor and Xvfb')
class MissingGameFilesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='haumea-missing-game-')
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.game = self.work / 'GTA'
        subprocess.run([sys.executable, str(HERE / 'fixture/make_game.py'), str(self.game)],
                       check=True, capture_output=True)
        self.xvfb, self.display = start_xvfb()
        self.addCleanup(self.stop_xvfb)

    def stop_xvfb(self):
        self.xvfb.terminate()
        self.xvfb.wait(10)

    def play(self, source, saved=False):
        env = editor_env(self.work, virtual_display=True)
        env['DISPLAY'] = self.display
        env.pop('XAUTHORITY', None)
        env.pop('HAUMEA_GAME_DIR', None)
        script = self.work / 'check.uis'
        script.write_text(source)
        args = [BINARY, '--ui-script', script]
        if saved:
            config = Path(env['XDG_CONFIG_HOME'], 'haumea')
            config.mkdir(parents=True, exist_ok=True)
            (config / 'workspace.cfg').write_text('game ' + str(self.game) + '\n')
        else:
            args.extend(['--game', self.game])
        result = run_editor(args, cwd=self.work, env=env)
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_deleted_core_files_are_named_and_keep_the_app_open(self):
        for name in REQUIRED:
            with self.subTest(file=name):
                path = self.game / name
                original = path.read_bytes()
                path.unlink()
                try:
                    self.play(f'''wait 20
assert window 'Welcome to Haumea Studio'
assert game_problem_contains '{name}'
assert game_folder_contains '{self.game}'
click_item 'Welcome to Haumea Studio' 'Continue'
wait 3
assert window 'Welcome to Haumea Studio'
''')
                finally:
                    path.write_bytes(original)

    def test_saved_incomplete_install_can_be_replaced(self):
        healthy = self.work / 'healthy'
        shutil.copytree(self.game, healthy)
        (self.game / 'data/gta.dat').unlink()
        self.play(f'''wait 20
assert window 'Welcome to Haumea Studio'
assert game_problem_contains 'data/gta.dat'
assert game_folder_contains '{self.game}'
set_item 'Welcome to Haumea Studio' '##game' '{healthy}'
click_item 'Welcome to Haumea Studio' 'Continue'
wait 20
assert no_window 'Welcome to Haumea Studio'
assert item '*' 'editor'
''', saved=True)

    def test_empty_core_file_is_also_rejected(self):
        (self.game / 'models/particle.txd').write_bytes(b'')
        self.play('''wait 20
assert window 'Welcome to Haumea Studio'
assert game_problem_contains 'models/particle.txd'
''')


if __name__ == '__main__':
    unittest.main()
