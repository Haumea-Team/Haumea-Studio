# SPDX-License-Identifier: GPL-2.0-or-later
"""Map check's client limits from open.mp's docs (Scripting > Limits).

On the synthetic game, one editor opens a map with 52 actors around one spot
(a client shows 51 at once) and 52 more spread far apart, then adds textdraws:
101 sprites on one board (a screen shows 100) and a text with a colour code
past its 255th character (which can crash the client).
"""
from pathlib import Path
import glob
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools' / 'agent'))
from haumea_test_env import editor_env  # noqa: E402

BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
XVFB = shutil.which('xvfb-run')

# 52 within a few metres of 2000,-3000; 52 more 500 m apart.
ACTORS = '\n'.join('\tCreateActor(280, %.1f, %.1f, 10.0, 0.0);' % (2000.0 + (i % 8), -3000.0 + (i // 8), )
                   for i in range(52))
SPREAD = '\n'.join('\tCreateActor(280, %.1f, -1000.0, 10.0, 0.0);' % (-2500.0 + i * 500.0 % 5000.0 + (i // 10) * 50.0)
                   for i in range(52))
MAP = 'public OnGameModeInit()\n{\n' + ACTORS + '\n' + SPREAD + '\n\treturn 1;\n}\n'


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class LimitCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hlc-', dir='/tmp'))
        game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'limits.pwn').write_text(MAP)
        cls.socket = cls.tmp / 'a.sock'
        env = editor_env(str(cls.tmp), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        cls.log = open(cls.tmp / 'out.txt', 'w')
        cls.editor = subprocess.Popen(
            [XVFB, '-a', '-s', '-screen 0 1280x800x24', BINARY, '--game', str(game),
             '--agent-socket', str(cls.socket)],
            cwd=cls.tmp, env=env, stdout=cls.log, stderr=subprocess.STDOUT, start_new_session=True)
        from haumea_ipc import HaumeaClient
        cls.client = HaumeaClient(str(cls.socket))
        end = time.time() + 240
        while time.time() < end and cls.editor.poll() is None:
            try:
                if cls.client.command('ping').get('result') == 'pong':
                    break
            except Exception:
                time.sleep(0.5)
        else:
            raise AssertionError('the editor did not answer:\n' + cls.output())
        cls.client.command('map_open', str(cls.tmp / 'limits.pwn'))
        cls.client.command('session_begin', 'textdraws')
        for i in range(101):
            cls.client.command('textdraw_add', 'x=%d' % (10 + i * 6), 'y=100', 'font=4', 'text=LD_SPAC:white')
        late = 'A' * 300 + '~r~red'
        cls.client.command('textdraw_add', 'x=10', 'y=300', 'text=' + late)
        cls.client.command('textdraw_add', 'x=10', 'y=320', 'text=~r~early ' + 'B' * 300)
        cls.items = cls.client.command('map_check')['items']

    @classmethod
    def tearDownClass(cls):
        try:
            os.killpg(cls.editor.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        cls.editor.wait()
        cls.log.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def output(cls):
        return (cls.tmp / 'out.txt').read_text(errors='replace')[-3000:]

    def texts(self, needle):
        return [i for i in self.items if needle in i['text']]

    def test_crowded_actors(self):
        found = self.texts('actors within')
        self.assertEqual(len(found), 1, self.items)
        self.assertTrue(found[0]['text'].startswith('52 actors within 200 m'), found)
        self.assertEqual(found[0]['severity'], 'warning')

    def test_sprites_on_one_board(self):
        found = self.texts('sprites: a player')
        self.assertEqual(len(found), 1, self.items)
        self.assertIn('has 101 sprites', found[0]['text'])

    def test_late_colour_code(self):
        found = self.texts('past the 255th character')
        self.assertEqual(len(found), 1, self.items)


if __name__ == '__main__':
    unittest.main()
